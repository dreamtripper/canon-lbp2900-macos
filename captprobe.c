// captprobe — 独立验证 Canon LBP2900 的 bulk IN/OUT 是否双向可用
// 不依赖 CUPS，直接 IOKit 打开设备，发 CAPT 握手命令并读应答。
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#define CANON_VID 0x04a9
#define LBP2900_PID 0x2676

static IOUSBDeviceInterface    **udev = NULL;
static IOUSBInterfaceInterface **intf = NULL;

static io_service_t find_dev(void) {
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    io_iterator_t it; io_service_t s, found = 0;
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

static int hexdump(const char *tag, unsigned char *b, int n) {
    printf("%s (%d 字节):", tag, n);
    for (int i = 0; i < n; i++) printf(" %02X", b[i]);
    printf("\n");
    return n;
}

/* 发 4 字节命令，读完整个应答（应答头 6 字节里 [2][3] 是总长度） */
static void cmd(const char *name, const unsigned char *c, int clen, int rdlen, int timeout_ms) {
    IOReturn w = (*intf)->WritePipeTO(intf, 1, (void *)c, clen, timeout_ms, timeout_ms);
    printf("[%-10s] WritePipeTO -> 0x%08x", name, w);
    if (w) { printf("  <== 写失败\n"); return; }
    printf("\n");
    usleep(20000);

    unsigned char buf[512];
    UInt32 len = sizeof(buf);
    IOReturn r = (*intf)->ReadPipeTO(intf, 2, buf, &len, timeout_ms, timeout_ms);
    printf("[%-10s] ReadPipeTO  -> 0x%08x  len=%u\n", name, r, (unsigned)len);
    if (r != 0 || len == 0) return;
    hexdump("        应答", buf, (int)len);

    /* 关键：应答总长可能远超 6 字节（如 A8 A0 状态查询是 56 字节）。
     * 只读 6 字节会把剩下的留在端点缓冲里，破坏下一次命令的对齐，
     * 打印机随之停在半路、之后所有写都超时。必须读干净。 */
    if (len >= 6) {
        int total = ((int)buf[2] << 8) | buf[3];
        int want = total - (int)len;
        int guard = 0;
        while (want > 0 && guard++ < 16) {
            unsigned char t[512];
            UInt32 tl = (UInt32)(want > (int)sizeof(t) ? (int)sizeof(t) : want);
            IOReturn rr = (*intf)->ReadPipeTO(intf, 2, t, &tl, 1500, 1500);
            if (rr != 0 || tl == 0) break;
            printf("[%-10s] (续读 %u 字节补齐应答)\n", name, (unsigned)tl);
            want -= (int)tl;
        }
    }
}

int main(int argc, char **argv) {
    int do_drain = (argc > 1 && strcmp(argv[1], "drain") == 0);

    io_service_t d = find_dev();
    if (!d) { printf("未找到设备\n"); return 1; }

    IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
    IOCreatePlugInInterfaceForService(d, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&udev);
    IODestroyPlugInInterface(plug);

    IOReturn kr = (*udev)->USBDeviceOpen(udev);
    printf("USBDeviceOpen   : 0x%08x\n", kr);
    if (kr) return 1;

    IOUSBConfigurationDescriptorPtr cfg = NULL;
    (*udev)->GetConfigurationDescriptorPtr(udev, 0, &cfg);
    kr = (*udev)->SetConfiguration(udev, cfg->bConfigurationValue);
    printf("SetConfiguration: 0x%08x\n", kr);

    io_iterator_t it = IO_OBJECT_NULL;
    IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                      kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    (*udev)->CreateInterfaceIterator(udev, &req, &it);
    io_service_t isvc = IOIteratorNext(it);
    IOObjectRelease(it);

    IOCFPlugInInterface **iplug = NULL;
    IOCreatePlugInInterfaceForService(isvc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &iplug, &score);
    (*iplug)->QueryInterface(iplug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID *)&intf);
    IODestroyPlugInInterface(iplug);

    kr = (*intf)->USBInterfaceOpen(intf);
    printf("USBInterfaceOpen: 0x%08x\n", kr);
    if (kr) return 1;

    /* 打印端点信息 */
    UInt8 ne = 0;
    (*intf)->GetNumEndpoints(intf, &ne);
    printf("端点数量: %d\n", ne);
    for (int i = 1; i <= ne; i++) {
        UInt8 dir, num, tt, interval; UInt16 mps;
        if ((*intf)->GetPipeProperties(intf, i, &dir, &num, &tt, &mps, &interval) == 0)
            printf("  pipe %d: %s ep=0x%02x 类型=%d maxPacket=%d\n",
                   i, dir ? "IN " : "OUT", num, tt, mps);
    }

    (*intf)->ClearPipeStallBothEnds(intf, 1);
    (*intf)->ClearPipeStallBothEnds(intf, 2);

    /* 先排空残留 */
    { unsigned char b[512]; int total = 0;
      for (int i = 0; i < 20; i++) { UInt32 l = sizeof(b);
        if ((*intf)->ReadPipeTO(intf, 2, b, &l, 100, 100) != 0 || l == 0) break; total += l; }
      printf("排空残留: %d 字节\n", total); }

    if (do_drain) {
        /* 只发一堆数据看能否写完，不读应答 */
        printf("== 纯写测试 ==\n");
        unsigned char *page = malloc(40000);
        memset(page, 0x55, 40000);
        long off = 0; int fails = 0;
        while (off < 40000) {
            long c = 40000 - off; if (c > 4096) c = 4096;
            IOReturn w = (*intf)->WritePipeTO(intf, 1, page + off, (UInt32)c, 10000, 10000);
            if (w) { printf("  写失败 @%ld -> 0x%08x\n", off, w); fails++; if (fails > 2) break; }
            off += c; usleep(5000);
        }
        printf("写出 %ld 字节, 失败 %d\n", off, fails);
        free(page);
    } else {
        printf("\n=== CAPT 握手探测 ===\n");
        static const unsigned char c_init[]  = {0xA1,0xA1,0x04,0x00};
        static const unsigned char c_state[] = {0xA8,0xA0,0x04,0x00};
        static const unsigned char c_devid[] = {0xA1,0xA1,0x04,0x00};

        cmd("init",       c_init,  4, 6, 3000);
        sleep(1);
        cmd("getstate",   c_state, 4, 6, 3000);
        sleep(1);
        cmd("init2",      c_devid, 4, 6, 3000);
        sleep(1);
        cmd("getstate2",  c_state, 4, 6, 3000);

        printf("\n=== 控制传输 GET_DEVICE_ID ===\n");
        unsigned char raw[1024]; memset(raw, 0, sizeof(raw));
        IOUSBDevRequestTO rq; memset(&rq, 0, sizeof(rq));
        rq.bmRequestType = 0xA1; rq.bRequest = 0; rq.wLength = sizeof(raw);
        rq.pData = raw; rq.noDataTimeout = 5000; rq.completionTimeout = 5000;
        kr = (*udev)->DeviceRequestTO(udev, &rq);
        printf("DeviceRequestTO -> 0x%08x wLenDone=%u\n", kr, (unsigned)rq.wLenDone);
        if (kr == 0 && rq.wLenDone > 2) {
            int n = rq.wLenDone - 2;
            printf("DEVICE_ID: %.*s\n", n, raw + 2);
        }
    }

    /* 收尾：发 0xD0A2 (Reset, 字节 A2 D0) 让打印机回到空闲。
     * 不做这一步，裸命令会把打印机留在"等后续数据"的半路，
     * 之后的正常打印会全部 WritePipeTO 超时。 */
    printf("\n=== 收尾 Reset ===\n");
    {
        static const unsigned char c_rst[] = {0xA2, 0xD0, 0x04, 0x00};
        cmd("reset", c_rst, 4, 6, 2000);
        usleep(300000);
        unsigned char d[256];
        UInt32 dl = sizeof(d);
        (*intf)->ReadPipeTO(intf, 2, d, &dl, 800, 800);
        printf("打印机已复位回空闲\n");
    }

    (*intf)->USBInterfaceClose(intf);
    (*udev)->USBDeviceClose(udev);
    return 0;
}
