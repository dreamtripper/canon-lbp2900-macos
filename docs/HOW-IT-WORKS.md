# 原理与排障

这份文档记录实现细节和踩过的坑，供想深入了解或自己改造的人参考。

## 为什么官方方案都不行

LBP2900 是 CAPT（Canon Advanced Printing Technology）主机型打印机。它自身
不含页面描述语言解释器，只认佳能私有 CAPT 位流，必须由驱动渲染后再送过去。
佳能从没发布过它的 macOS 驱动——Windows 驱动是 WDM 架构，无法移植。

两条常见的死路：

| 方案 | 卡在哪 |
|---|---|
| 佳能官方 macOS CAPT 驱动栈（`captmoncnab3` / `ccpd`） | 驱动不含 LBP2900 型号；且守护进程**独占 USB 接口**（`USBInterfaceOpen` → `0xe00002c5`），启用后其它队列都打不开设备 |
| Apple 的 `USBGenericPrintingClass`（标准 `usb://` 后端） | 会轮询 IEEE-1284 的 `GET_PORT_STATUS`，这台机器不支持，于是一直停在"正在等待打印机变为可用"（`USB printer status: 0xffffda40`） |

## CAPT 协议是严格同步的

主机发一条命令，**必须先读到打印机 6 字节应答，才能发下一条**。
`rastertocapt` 通过三个通道与后端协作：

- **数据流**：过滤器 `fwrite` 到 stdout → 后端从 stdin 读出 → 写 bulk OUT
- **side-channel (fd 4)**：过滤器向后端发 `DRAIN_OUTPUT` 等请求
- **back-channel (fd 3)**：后端把 bulk IN 收到的应答转给过滤器

`capt-command.c` 的 `capt_send_buf()` 每写一块数据就调用一次
`cupsSideChannelDoRequest(CUPS_SC_CMD_DRAIN_OUTPUT, ...)`，语义是"等这些字节
真正落到设备上再回 OK"，紧接着 `capt_recv_buf()` 去读应答。

## 最容易踩的坑：DRAIN_OUTPUT

本项目最早的后端把 `DRAIN_OUTPUT` 实现成"立刻回 OK"，而数据还堵在管道和用户态
缓冲里没进 USB。过滤器随即读 back-channel → 空 → 超时 → 命令与应答整体错位。
现象是**出纸但空白**，或**纸出来了作业却永远卡在队列里**：

```
CAPT: send  A8 A0 04 00
CAPT: waiting for 6 bytes
CAPT: status poll timed out (soft)
CAPT: resync failed for cmd A8 A0 (0 bytes scanned)
```

正确做法：收到 `DRAIN_OUTPUT` 时，先把 stdin 里已到达的数据**非阻塞读尽并全部
写入 USB**，然后才回 `OK`。修复后同一页的日志：

```
CAPT: send  A1 E1 4C 00 ...
CAPT: recv  A1 E1 06 00 00 00      ← 页数据被正确应答
CAPT: pages 1/1/1/1                ← 打印机报"已完成 1 页"
CAPT: rastertocapt finished
[end] 写出 36116 字节 / 读回 5712 字节
```

## canonraw 的实现要点

- 单线程 `poll()` 同时监督 stdin 与 fd 4，`DRAIN_OUTPUT` 走上述"读尽+写完才回 OK"
- 独立读线程专职把 bulk IN 转发到 fd 3
- bulk 写按 4096 字节分块（这台老机 8KB 以上容易 NAK）
- 写失败只重试，**不调用 `ClearPipeStall`**——清 STALL 会打断整页传输，
  反而把打印机推进死状态
- 作业被中断时（SIGTERM）主动发 `0xD0A2`(Reset)，尽最大努力让打印机回到空闲

## captprobe

独立于 CUPS 的诊断工具，用来判断问题出在打印机还是驱动。它完整读取应答
（CAPT 应答 6 字节头里 `[2][3]` 是总长度，init/status 应答都是 56 字节），
结束时发 Reset 收尾，不会污染打印机状态。

判断标准：bulk 写返回 `0x00000000` 才算通道正常。

## 排障

| 现象 | 原因 / 处理 |
|---|---|
| `captprobe` 里 `WritePipeTO -> 0xe0004051` 全部超时 | 打印机固件卡死。软件复位（`ResetDevice`、`USBDeviceReEnumerate`、`0xD0A2` 命令）全部无效，**关电源等 10 秒**才恢复。多半是上一次作业被强行中断 |
| 能识别设备、`GET_DEVICE_ID` 正常，但打不出来 | 同上。卡死时 bulk 端点死了而控制端点还活着，极易误判成驱动问题 |
| 一直"等待打印机可用" | 佳能守护进程又起来了。`ps aux \| grep captmon` 查一下，重跑 `install.sh` |
| 出纸但空白 | 看 `/var/log/canonraw.log` 的"共写出 N 字节"。完整一页约 36 KB，明显偏小就是数据没写完 |
| 作业卡在队列不结束 | 后端未正常退出，看 `/var/log/cups/error_log` 里 `[SC ] cmd=2` 之后是否超时 |

### 关于取消作业

一页数据传到一半被中断，这台打印机的固件会死锁：之后 `Reset`(0xD0A2)、
`Init`(0xA1A1)、清 STALL、`ResetDevice`、`USBDeviceReEnumerate` 全部写不进去
（`0xe0004051`），只能断电。后端会尽力收尾，但固件不认，属于那个年代 CAPT
打印机的通病。

### 关于 USB 口

必须插机身直连的口。经集线器或显示器扩展时，bulk 传输会稳定超时，而控制传输
却正常，这种不对称极具迷惑性。
