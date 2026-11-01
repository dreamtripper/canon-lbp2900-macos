# Canon LBP2900 on macOS

让 Canon LBP2900 激光打印机在 Apple Silicon Mac 上正常工作。

**状态**：macOS 27 / Apple Silicon 实测通过——正常出纸、内容正确。

## 为什么需要它

LBP2900 是 CAPT 主机型打印机，只认佳能私有协议，而佳能从未发布它的
macOS 驱动。官方 CAPT 驱动栈会独占 USB 接口，系统自带的打印类驱动又会
去轮询这台机器不支持的端口状态查询，两者都用不了。

本项目的做法是自己写一个 USB 后端，直接和打印机收发数据：

```
CUPS → rastertocapt → canonraw → USB
       (CAPT 编码)    (自写后端)
```

## 安装

```sh
sudo sh install.sh
```

装好后打印队列叫 **`Canon_LBP2900`**，并已设为默认打印机。

脚本可以重复执行。系统大版本升级后如果打印失效，重跑一次即可恢复。

## 验证

```sh
./captprobe                      # 检查打印机 USB 通道是否正常
lp -d Canon_LBP2900 diag.pdf     # 打一张测试页
```

## 注意事项

- **不要在打印途中取消作业。** 这台打印机的固件一旦在一页数据中途被打断
  就会死锁，只能关电源等 10 秒再开。
- **插机身直连的 USB 口**，不要经显示器或集线器扩展。
- 打印机缺纸时也会拒绝接收数据，表现和卡死很像——先确认有纸。

详细的原理和排障方法见 [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)。

## 文件

| 文件 | 说明 |
|---|---|
| `canonraw.c` / `canonraw` | USB 后端（源码 + arm64 二进制） |
| `captprobe.c` / `captprobe` | 诊断工具 |
| `rastertocapt` | CAPT 过滤器 |
| `CanonLBP-2900-3000.ppd` | 打印机描述文件 |
| `install.sh` | 安装脚本 |
| `diag.pdf` / `mkdiag.py` | 测试页及生成脚本 |
| `vendor/captdriver/` | 上游 captdriver 源码 |

## 许可

GPLv3（`rastertocapt` 源自 [captdriver](https://github.com/mounaiban/captdriver)，
源码见 `vendor/`）。使用、修改与分发均遵循 GPLv3 条款。

## 致谢

- [captdriver](https://github.com/mounaiban/captdriver) — CAPT 协议逆向与过滤器
- [agalakhov/captdriver](https://github.com/agalakhov/captdriver) — 原始项目
