# Canon LBP2900 on macOS

让 2005 年的 Canon LBP2900 激光打印机在 Apple Silicon Mac 上正常工作。

> A working driver stack for the Canon LBP2900 (CAPT) on macOS Apple Silicon,
> without Canon's official software.

**状态**：已在 macOS 27.0.1 / Apple Silicon 上实测通过——正常出纸、内容正确、作业自动结束。

---

## 背景

LBP2900 是 **CAPT（Canon Advanced Printing Technology）主机型打印机**。
它自身不含页面描述语言解释器，只认佳能私有 CAPT 协议，必须由驱动把页面
渲染成 CAPT 位流再送过去。佳能从没发布过它的 macOS 驱动——Windows 驱动是
WDM 架构，无法移植。

于是有两条常见的死路：

| 方案 | 卡在哪 |
|---|---|
| 佳能官方 macOS CAPT 驱动栈（`captmoncnab3` / `ccpd`） | 驱动不含 LBP2900 型号；且守护进程**独占 USB 接口**（`USBInterfaceOpen` → `0xe00002c5`），启用后其它任何队列都打不开设备 |
| Apple 的 `USBGenericPrintingClass`（标准 `usb://` 后端） | 它会轮询 IEEE-1284 的 `GET_PORT_STATUS`，**这台机器不支持**，于是一直流在"正在等待打印机变为可用"（`USB printer status: 0xffffda40`） |

## 方案

```
CUPS → cgpdftoraster → rastertocapt → canonraw → USB bulk
        (PDF→raster)   (CAPT 编码)    (自写 IOKit 后端)
```

- **`rastertocapt`**：来自开源项目 [captdriver](https://github.com/mounaiban/captdriver)，
  负责把 CUPS raster 编码成 CAPT 位流。
- **`canonraw`**：本项目的核心。直接用 IOKit 收发 USB bulk 端点，绕开 Apple 的
  类驱动，并自己实现 CUPS 的 side-channel(fd4) / back-channel(fd3)。

## 关键难点：CAPT 协议是严格同步的

主机发一条命令，**必须先读到打印机 6 字节应答，才能发下一条**。
`rastertocapt` 通过三个通道与后端协作：

- **数据流**：过滤器 `fwrite` 到 stdout → 后端从 stdin 读出 → 写 bulk OUT
- **side-channel (fd 4)**：过滤器向后端发 `DRAIN_OUTPUT` 等请求
- **back-channel (fd 3)**：后端把 bulk IN 收到的应答转给过滤器

`capt-command.c` 的 `capt_send_buf()` 每写一块数据就调用一次
`cupsSideChannelDoRequest(CUPS_SC_CMD_DRAIN_OUTPUT, ...)`，**语义是"等这些字节
真正落到设备上再回 OK"**，紧接着 `capt_recv_buf()` 去读应答。

### 踩过的坑

这个项目最早的后端把 `DRAIN_OUTPUT` 实现成"**立刻回 OK**"，而数据还堵在管道和
用户态缓冲里没进 USB。过滤器随即读 back-channel → 空 → 超时 → 命令与应答整体
错位。现象是**出纸但空白**、或**纸出来了作业却永远卡在队列里**：

```
CAPT: send  A8 A0 04 00
CAPT: waiting for 6 bytes
CAPT: status poll timed out (soft)
CAPT: resync failed for cmd A8 A0 (0 bytes scanned)
```

正确的做法是：收到 `DRAIN_OUTPUT` 时，先把 stdin 里已到达的数据**非阻塞读尽并
全部写入 USB**，然后才回 `OK`。修复后同一页的日志：

```
CAPT: send  A1 E1 4C 00 ...
CAPT: recv  A1 E1 06 00 00 00      ← 页数据被正确应答
CAPT: pages 1/1/1/1                ← 打印机报"已完成 1 页"
CAPT: rastertocapt finished
[end] 写出 36116 字节 / 读回 5712 字节
```

`canonraw` 的实现要点：

- 单线程 `poll()` 同时监督 stdin 与 fd 4，`DRAIN_OUTPUT` 走上述"读尽+写完才回 OK"
- 独立读线程专职把 bulk IN 转发到 fd 3
- bulk 写按 4096 字节分块（这台老机 8KB 以上容易 NAK），失败重试但不 `ClearPipeStall`
  （清 STALL 会打断整页传输，反而把打印机搞进死状态）
- 作业被中断时（SIGTERM）主动发 `0xD0A2`(Reset)，尽最大努力让打印机回到空闲

## 安装

```sh
sudo sh install.sh
```

脚本会：编译/安装后端 → 安装过滤器 → 生成并安装 PPD → 停用会抢占 USB 的佳能
官方组件与登录自启 → 建立队列 `Canon_LBP2900` 并设为默认 → 准备日志文件。

**幂等**，可重复执行。系统大版本升级后若打印失效，重跑一次即可恢复
（`/usr/libexec/cups` 下的第三方文件可能被系统清理）。

## 验证

```sh
# 设备是否在 USB 上
ioreg -p IOUSB -w 0 | grep "CAPT USB"

# bulk 通道是否正常（能读到 A1 A1 38 00 00 0B 才算通）
./captprobe

# 打印
lp -d Canon_LBP2900 -o media=A4 diag.pdf

# 看日志
tail -f /var/log/canonraw.log
tail -f /var/log/cups/error_log
```

`captprobe` 是独立于 CUPS 的诊断工具，会完整读取应答（CAPT 应答 6 字节头里
`[2][3]` 是总长度，init/status 应答都是 56 字节）并收尾复位，不会污染打印机状态。

## 已知限制

**不要在打印途中取消作业。** 实测：一页数据传到一半被中断，这台打印机的固件
会死锁——之后 `Reset`(0xD0A2)、`Init`(0xA1A1)、清 STALL、`ResetDevice`、
`USBDeviceReEnumerate` 全部写不进去（`0xe0004051`），**只能关电源等 10 秒再开**。
后端会尽力收尾，但固件不认，属于那个年代 CAPT 打印机的通病。

卡死时有个极具迷惑性的现象：`ioreg` 仍能看到设备、`GET_DEVICE_ID` 仍正常返回，
很容易误判成驱动问题。判断依据只有一条——跑 `./captprobe`，看 bulk 写是否返回
`0x00000000`。

其他：

- **必须插机身直连的 USB 口**，不要经显示器或集线器扩展。经 Hub 时 bulk 传输会
  稳定超时，而控制传输却正常。
- 打印机缺纸时也会拒绝接收数据，表现与卡死相同——先确认有纸。
- 出纸但空白：检查 `/var/log/canonraw.log` 的"共写出 N 字节"。完整一页约 36 KB，
  明显偏小就是数据没写完。

## 目录

| 文件 | 说明 |
|---|---|
| `canonraw.c` / `canonraw` | 自写裸 USB CUPS 后端（源码 + arm64 二进制） |
| `captprobe.c` / `captprobe` | 诊断工具：独立跑 CAPT 握手，判断 USB 通道好坏 |
| `rastertocapt` | CAPT 过滤器（arm64，由 `vendor/captdriver` 源码编译） |
| `CanonLBP-2900-3000.ppd` | captdriver 的 PPD（r2c 0.1.4） |
| `install.sh` | 一键安装（幂等） |
| `diag.pdf` / `mkdiag.py` | 测试页及其生成脚本 |
| `vendor/captdriver/` | 上游 captdriver 源码，供编译 `rastertocapt` 之用 |

## 许可

- `canonraw.c`、`captprobe.c`、`install.sh`、`mkdiag.py`：MIT
- `rastertocapt` 及其上游源码 `vendor/captdriver/`：**GPLv3**
  *（本项目对其唯一改动是放宽了应答超时，源码已随仓库提供）*

因此**整个仓库按 GPLv3 分发**，全文见 `LICENSE`。

## 致谢

- [captdriver](https://github.com/mounaiban/captdriver) — CAPT 协议逆向与过滤器实现
- [agalakhov/captdriver](https://github.com/agalakhov/captdriver) — 原始项目
- 协议细节见 `vendor/captdriver/SPECS`
