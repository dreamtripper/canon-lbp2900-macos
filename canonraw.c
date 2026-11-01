// canonraw — Canon LBP2900 裸 USB CUPS 后端（v2，正确实现 DRAIN 语义）
//
// 背景：LBP2900 是 CAPT 主机型打印机，只认佳能私有协议。rastertocapt
// 过滤器通过「数据流(stdout→fd0) + side-channel(fd4) + back-channel(fd3)」
// 与后端协作，而 CAPT 协议是**严格同步**的：
//
//   过滤器 capt_send_buf():  fwrite(data) → fflush → 请求 DRAIN_OUTPUT
//                            后端收到 DRAIN 必须保证数据**已经写进 USB** 才能回 OK
//   过滤器 capt_recv_buf():  从 fd3 读 6 字节应答，超时 15s
//
// v1 把 DRAIN_OUTPUT 实现成「立刻回 OK」，数据还堵在管道/用户态缓冲里，
// 于是过滤器马上读 fd3 → 空 → soft timeout → 协议错位 → 空白页/作业卡死。
//
// v2 用单线程 poll 同时监督 stdin 与 fd4，DRAIN 时先把 stdin 残余数据
// 非阻塞读尽并全部写入 USB，再回 OK。读线程专职把 bulk IN 转发到 fd3。
//
// 编译: clang -O2 -o canonraw canonraw.c -framework IOKit -framework CoreFoundation
// 安装: sudo cp canonraw /usr/libexec/cups/backend/ && sudo chown root:wheel ... && chmod 755

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOReturn.h>

#define CANON_VID   0x04a9
#define LBP2900_PID 0x2676

#define SC_FD  4   /* CUPS side-channel (双向，与过滤器通信) */
#define BACK_FD 3  /* CUPS back-channel (向过滤器送打印机应答) */

/* CUPS side-channel 命令 */
#define SC_SOFT_RESET      1
#define SC_DRAIN_OUTPUT    2
#define SC_GET_BIDI        3
#define SC_GET_DEVICE_ID   4
#define SC_GET_STATE       5
#define SC_STATUS_OK       1
#define SC_STATUS_NOT_IMPL 7

#define EP_OUT 1
#define EP_IN  2
#define CHUNK  4096          /* bulk 写分块：这台老机 8K 以上容易 NAK */
#define WRITE_TO_MS 8000

static FILE *glog = NULL;
static IOUSBDeviceInterface    **gudev = NULL;
static IOUSBInterfaceInterface **gintf = NULL;
static volatile int g_stop = 0;
static volatile int g_interrupted = 0;
static volatile int g_usb_dead = 0;

static long g_total_written = 0;
static long g_total_in = 0;

static void L(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (glog) { vfprintf(glog, fmt, ap); fflush(glog); }
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fflush(stderr);
    va_end(ap);
}

static void on_signal(int s) { (void)s; g_stop = 1; g_interrupted = 1; }

/* ---------------- bulk 写：分块 + 超时重试 ---------------- */
static int bulk_write(const unsigned char *buf, size_t len)
{
    size_t off = 0;
    while (off < len && !g_stop) {
        size_t c = len - off;
        if (c > CHUNK) c = CHUNK;

        int attempt;
        IOReturn kr = (IOReturn)-1;
        for (attempt = 0; attempt < 12 && !g_stop; attempt++) {
            /* 首次给足超时(大块数据要时间)，重试用短超时，
             * 这样作业被取消时不会卡在 8 秒×N 次里迟迟退不出去 */
            UInt32 to = (attempt == 0) ? WRITE_TO_MS : 2000;
            kr = (*gintf)->WritePipeTO(gintf, EP_OUT, (void *)(buf + off), (UInt32)c, to, to);
            if (kr == kIOReturnSuccess) break;
            if (kr == kIOReturnNoDevice) { g_usb_dead = 1; break; }
            /* 打印机缓冲满时会 NAK/超时：稍等再试即可，不要 ClearPipeStall
             * （清 STALL 会打断正在进行的整页传输，反而把打印机搞进死状态）*/
            L("[!] WritePipeTO 0x%08x @%zu, 重试 %d/12\n", kr, off, attempt + 1);
            usleep(120000);
        }
        if (kr != kIOReturnSuccess) {
            if (g_stop) { L("[end] 写操作被中断\n"); return -1; }
            L("[!!] bulk 写最终失败 0x%08x @%zu/%zu\n", kr, off, len);
            return -1;
        }
        off += c;
        g_total_written += (long)c;
    }
    return 0;
}

/* ---------------- 读线程：bulk IN -> fd3 ---------------- */
static void *reader_main(void *arg)
{
    (void)arg;
    unsigned char buf[8192];
    while (!g_stop) {
        UInt32 len = sizeof(buf);
        IOReturn kr = (*gintf)->ReadPipeTO(gintf, EP_IN, buf, &len, 200, 200);

        if (kr == kIOReturnSuccess && len > 0) {
            g_total_in += (long)len;
            if (glog) {
                fprintf(glog, "[IN ] %u 字节:", (unsigned)len);
                for (UInt32 i = 0; i < len && i < 32; i++) fprintf(glog, " %02X", buf[i]);
                fprintf(glog, "\n");
                fflush(glog);
            }
            /* 原样转给过滤器 */
            size_t off = 0;
            while (off < len) {
                ssize_t w = write(BACK_FD, buf + off, len - off);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN) { usleep(2000); continue; }
                    break;   /* 后端关闭时忽略 */
                }
                off += (size_t)w;
            }
        } else if (kr == kIOReturnNoDevice || kr == kIOReturnNotResponding) {
            L("[!!] 设备已从 USB 总线消失 (0x%08x)\n", kr);
            g_usb_dead = 1;
            g_stop = 1;
            break;
        } else {
            /* 超时 / aborted 属于正常空转 */
            usleep(5000);
        }
    }
    return NULL;
}

/* ---------------- 控制传输取 device-id ---------------- */
static int ctrl_get_device_id(char *out, int maxlen)
{
    if (!gudev) return -1;
    unsigned char raw[1024];
    memset(raw, 0, sizeof(raw));
    IOUSBDevRequestTO req;
    memset(&req, 0, sizeof(req));
    req.bmRequestType = 0xA1;   /* IN | Class | Interface */
    req.bRequest      = 0;      /* GET_DEVICE_ID */
    req.wValue        = 0;
    req.wIndex        = 0;
    req.wLength       = sizeof(raw);
    req.pData         = raw;
    req.noDataTimeout = 5000;
    req.completionTimeout = 5000;
    IOReturn kr = (*gudev)->DeviceRequestTO(gudev, &req);
    if (kr != kIOReturnSuccess || req.wLenDone <= 2) {
        L("[SC ] GET_DEVICE_ID 失败 0x%08x len=%u\n", kr, (unsigned)req.wLenDone);
        return -1;
    }
    int n = (int)req.wLenDone - 2;
    if (n > maxlen - 1) n = maxlen - 1;
    memcpy(out, raw + 2, (size_t)n);
    out[n] = '\0';
    return n;
}

/* ---------------- 把 stdin 里已到达的数据全部读尽并写入 USB ---------------- */
static int flush_pending(int in_fd)
{
    static unsigned char buf[65536];
    int fl = fcntl(in_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(in_fd, F_SETFL, fl | O_NONBLOCK);

    for (;;) {
        ssize_t n = read(in_fd, buf, sizeof(buf));
        if (n > 0) {
            if (bulk_write(buf, (size_t)n) < 0) {
                if (fl >= 0) fcntl(in_fd, F_SETFL, fl);
                return -1;
            }
            continue;
        }
        if (n == 0) {            /* EOF：没有更多数据了 */
            if (fl >= 0) fcntl(in_fd, F_SETFL, fl);
            return 1;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        L("[!!] 读 stdin 出错: %s\n", strerror(errno));
        if (fl >= 0) fcntl(in_fd, F_SETFL, fl);
        return -1;
    }
    if (fl >= 0) fcntl(in_fd, F_SETFL, fl);
    return 0;
}

/* ---------------- 处理一条 side-channel 请求 ---------------- */
static int handle_sc(int in_fd)
{
    unsigned char hdr[4], data[8192];
    ssize_t got = 0;

    while (got < 4) {
        ssize_t r = read(SC_FD, hdr + got, (size_t)(4 - got));
        if (r <= 0) {
            if (r < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
            return 0;
        }
        got += r;
    }

    int cmd  = hdr[0];
    int dlen = ((int)hdr[2] << 8) | hdr[3];
    while (dlen > 0) {
        int want = dlen > (int)sizeof(data) ? (int)sizeof(data) : dlen;
        ssize_t r = read(SC_FD, data, (size_t)want);
        if (r <= 0) break;
        dlen -= (int)r;
    }

    unsigned char resp[4 + 1024];
    memset(resp, 0, sizeof(resp));
    resp[0] = (unsigned char)cmd;
    int rlen = 0;

    switch (cmd) {
    case SC_DRAIN_OUTPUT: {
        /* 核心修复：真正把数据推下去，再回 OK。
         * 返回 1 表示 stdin 已 EOF。 */
        int fr = flush_pending(in_fd);
        resp[1] = (fr < 0) ? SC_STATUS_NOT_IMPL : SC_STATUS_OK;
        rlen = 0;
        break;
    }
    case SC_GET_DEVICE_ID: {
        char id[1024];
        int n = ctrl_get_device_id(id, sizeof(id));
        if (n > 0) { memcpy(resp + 4, id, (size_t)n); rlen = n; }
        else {
            static const char fb[] =
                "MFG:Canon;MDL:LBP2900;CMD:CAPT;VER:2.1;CLS:PRINTER;DES:Canon LBP2900;";
            memcpy(resp + 4, fb, sizeof(fb) - 1);
            rlen = (int)sizeof(fb) - 1;
        }
        resp[1] = SC_STATUS_OK;
        break;
    }
    case SC_SOFT_RESET:
        /* 软复位：清掉未完成的传输即可，别 ResetDevice（会让固件失联） */
        resp[1] = SC_STATUS_OK;
        rlen = 0;
        break;
    case SC_GET_BIDI:
        resp[1] = SC_STATUS_OK;
        rlen = 1;
        resp[4] = 1;
        break;
    case SC_GET_STATE:
        memcpy(resp + 4, "idle", 4);
        resp[1] = SC_STATUS_OK;
        rlen = 4;
        break;
    default:
        resp[1] = SC_STATUS_NOT_IMPL;
        rlen = 0;
        break;
    }

    resp[2] = (unsigned char)((rlen >> 8) & 0xff);
    resp[3] = (unsigned char)(rlen & 0xff);
    size_t total = (size_t)(4 + rlen), off = 0;
    while (off < total) {
        ssize_t w = write(SC_FD, resp + off, total - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (size_t)w;
    }
    return 1;
}

/* ---------------- 找设备并打开 ---------------- */
static io_service_t find_canon(void)
{
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    io_iterator_t it;
    io_service_t s, found = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) != KERN_SUCCESS) return 0;
    while ((s = IOIteratorNext(it))) {
        CFNumberRef nv = IORegistryEntryCreateCFProperty(s, CFSTR("idVendor"), kCFAllocatorDefault, 0);
        CFNumberRef np = IORegistryEntryCreateCFProperty(s, CFSTR("idProduct"), kCFAllocatorDefault, 0);
        int v = -1, p = -1;
        if (nv) { CFNumberGetValue(nv, kCFNumberIntType, &v); CFRelease(nv); }
        if (np) { CFNumberGetValue(np, kCFNumberIntType, &p); CFRelease(np); }
        if (v == CANON_VID && p == LBP2900_PID) { found = s; break; }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return found;
}

static int usb_open(void)
{
    io_service_t dev = find_canon();
    if (!dev) { L("[!!] 未找到 Canon LBP2900\n"); return -1; }

    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    IOCreatePlugInInterfaceForService(dev, kIOUSBDeviceUserClientTypeID,
                                      kIOCFPlugInInterfaceID, &plug, &score);
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&gudev);
    IODestroyPlugInInterface(plug);
    IOObjectRelease(dev);

    IOReturn kr = (*gudev)->USBDeviceOpen(gudev);
    if (kr) { L("[!!] USBDeviceOpen 0x%08x\n", kr); return -1; }

    IOUSBConfigurationDescriptorPtr cfg = NULL;
    (*gudev)->GetConfigurationDescriptorPtr(gudev, 0, &cfg);
    if (cfg) {
        kr = (*gudev)->SetConfiguration(gudev, cfg->bConfigurationValue);
        if (kr && kr != kIOReturnExclusiveAccess)
            L("[!] SetConfiguration 0x%08x\n", kr);
    }

    io_iterator_t it = IO_OBJECT_NULL;
    IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                      kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    (*gudev)->CreateInterfaceIterator(gudev, &req, &it);
    io_service_t isvc = IOIteratorNext(it);
    IOObjectRelease(it);
    if (!isvc) { L("[!!] 找不到接口\n"); return -1; }

    IOCFPlugInInterface **iplug = NULL;
    IOCreatePlugInInterfaceForService(isvc, kIOUSBInterfaceUserClientTypeID,
                                      kIOCFPlugInInterfaceID, &iplug, &score);
    (*iplug)->QueryInterface(iplug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID *)&gintf);
    IODestroyPlugInInterface(iplug);
    IOObjectRelease(isvc);

    kr = (*gintf)->USBInterfaceOpen(gintf);
    if (kr) { L("[!!] USBInterfaceOpen 0x%08x\n", kr); return -1; }

    /* 先清掉上次中断作业留下的残留回包，避免污染本次应答对齐 */
    {
        unsigned char d[512];
        int n = 0, total = 0;
        for (int i = 0; i < 30; i++) {
            UInt32 dl = sizeof(d);
            if ((*gintf)->ReadPipeTO(gintf, EP_IN, d, &dl, 120, 120) != kIOReturnSuccess || dl == 0) break;
            n++; total += (int)dl;
        }
        L("[ok] USB 已打开，排空残留 %d 次/%d 字节\n", n, total);
    }
    return 0;
}

/* ---------------- main ---------------- */
int main(int argc, char *argv[])
{
    g_stop = 0;
    g_usb_dead = 0;

    /* 发现模式：CUPS 用它列可用设备 */
    if (argc < 2) {
        printf("direct canonraw:/Canon/LBP2900 \"Canon\" \"Canon LBP2900\"\n");
        return 0;
    }

    /* 日志写到 /var/log 供排查（安装时预先创建并放开权限），
     * 同时 stderr 进 CUPS error_log —— 两条路都能看到 */
    glog = fopen("/var/log/canonraw.log", "a");
    if (!glog) glog = fopen("/tmp/canonraw.log", "a");
    setvbuf(stdout, NULL, _IONBF, 0);

    L("\n===== canonraw v2 启动 (job=%s) =====\n",
      argc > 1 ? argv[1] : "?");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* 输入：argv[6] 给了真实文件名就用它，否则 stdin */
    int in_fd = STDIN_FILENO;
    if (argc >= 7 && argv[6] && strcmp(argv[6], "-") != 0 && argv[6][0] != '\0') {
        in_fd = open(argv[6], O_RDONLY);
        if (in_fd < 0) { L("[!!] 打不开输入 %s\n", argv[6]); }
        else L("[ok] 输入文件 %s fd=%d\n", argv[6], in_fd);
    }

    if (usb_open() < 0) return 1;

    pthread_t th;
    pthread_create(&th, NULL, reader_main, NULL);
    L("[ok] 读线程已启动，进入主循环\n");

    int eof_seen = 0;
    int sc_closed = 0;
    int idle_ticks = 0;

    while (!g_stop) {
        struct pollfd pfd[2];
        int nf = 0;
        int idx_in = -1, idx_sc = -1;

        if (!eof_seen && in_fd >= 0) {
            idx_in = nf; pfd[nf].fd = in_fd; pfd[nf].events = POLLIN; pfd[nf].revents = 0; nf++;
        }
        if (!sc_closed) {
            idx_sc = nf; pfd[nf].fd = SC_FD; pfd[nf].events = POLLIN; pfd[nf].revents = 0; nf++;
        }
        if (nf == 0) break;

        int pr = poll(pfd, (nfds_t)nf, 150);
        if (pr < 0 && errno != EINTR) { L("[!] poll: %s\n", strerror(errno)); break; }

        int activity = 0;

        if (pr > 0 && idx_in >= 0) {
            short re = pfd[idx_in].revents;
            if (re & (POLLIN | POLLHUP | POLLERR)) {
                static unsigned char buf[65536];
                ssize_t n = read(in_fd, buf, sizeof(buf));
                if (n > 0) {
                    if (bulk_write(buf, (size_t)n) < 0) break;
                    activity = 1;
                } else if (n == 0) {
                    eof_seen = 1;
                    L("[ok] stdin EOF，共写出 %ld 字节\n", g_total_written);
                } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    eof_seen = 1;
                }
            }
        }

        if (pr > 0 && idx_sc >= 0) {
            short re = pfd[idx_sc].revents;
            if (re & POLLIN) {
                handle_sc(in_fd);
                activity = 1;
            }
            /* side-channel 关闭后别再 poll 它，否则 POLLHUP 会立即返回、
             * 把 idle 计数不断清零 -> 死循环烧 CPU、作业永不结束 */
            if (re & (POLLHUP | POLLERR | POLLNVAL)) sc_closed = 1;
        }

        if (g_usb_dead) break;

        /* 收尾：stdin 结束且 side-channel 也断了，或长时间无活动 */
        if (eof_seen) {
            if (sc_closed) { break; }
            if (activity) idle_ticks = 0;
            else if (++idle_ticks > 60) break;    /* ~9 秒无活动 */
        }
    }

    /* 给读线程留点时间把最后的应答转出去 */
    usleep(300000);
    g_stop = 1;
    pthread_join(th, NULL);

    L("[end] 写出 %ld 字节 / 读回 %ld 字节%s%s\n",
      g_total_written, g_total_in,
      g_interrupted ? " (作业被中断)" : "",
      g_usb_dead ? " (USB异常)" : "");

    /* 作业被取消/中断时，打印机可能停在"等后续数据"的半路，
     * 之后所有 bulk 都不再应答，只能拔电源。主动发 0xD0A2(Reset)
     * 让它回到空闲态，是避免下次卡死的关键一步。 */
    if (!g_usb_dead && g_interrupted && gintf) {
        unsigned char rst[4] = { 0xA2, 0xD0, 0x04, 0x00 };  /* 0xD0A2 = Reset */
        IOReturn rk = (*gintf)->WritePipeTO(gintf, EP_OUT, rst, 4, 2000, 2000);
        usleep(250000);
        unsigned char d[256];
        UInt32 dl = sizeof(d);
        (*gintf)->ReadPipeTO(gintf, EP_IN, d, &dl, 800, 800);
        L("[end] 已发送 Reset 让打印机回到空闲 (0x%08x)\n", rk);
    }

    if (gintf) (*gintf)->USBInterfaceClose(gintf);
    if (gudev) (*gudev)->USBDeviceClose(gudev);
    if (glog) fclose(glog);
    return g_usb_dead ? 1 : 0;
}
