#!/bin/sh
# ============================================================
#  Canon LBP2900 — macOS 驱动安装脚本 v2
#
#  用法:  sudo sh install.sh
#
#  链路:  CUPS -> cgpdftoraster -> rastertocapt -> canonraw -> USB bulk
#
#  设计要点(为什么自己写后端):
#   LBP2900 是 CAPT 主机型打印机,只讲佳能私有协议,且协议是严格同步的:
#   主机发命令 -> 必须先读到打印机 6 字节应答 -> 才能发下一条。
#   Apple 的 USBGenericPrintingClass 会去轮询 IEEE-1284 的 GET_PORT_STATUS,
#   这台机器根本不支持,于是永远"等待打印机就绪";佳能官方守护进程则会
#   独占 USB 接口。所以改用 canonraw 直接用 IOKit 收发 bulk 端点,
#   并自己实现 CUPS 的 side-channel(fd4)/back-channel(fd3)。
#
#  v2 关键修复: 正确实现 side-channel 的 DRAIN_OUTPUT —— 必须等数据
#   真正写进 USB 才回 OK。v1 立即回 OK,过滤器读不到应答导致协议错位,
#   表现为"出纸但空白 / 作业永远卡在队列里"。
# ============================================================
set -e

HERE=$(cd "$(dirname "$0")" && pwd)

BACKEND=/usr/libexec/cups/backend/canonraw
FILTERDIR=/usr/libexec/cups/filter
PPDSTORE=/Library/Printers/PPDs/Contents/Resources
PPDNAME=CanonLBP2900-captdriver.ppd
QUEUE=Canon_LBP2900
DEVID="canonraw:/Canon/LBP2900"

echo "=== [1/7] 编译 canonraw 后端 ==="
if [ -f "$HERE/canonraw" ] && [ -x "$HERE/canonraw" ]; then
    echo "  使用已有二进制 $HERE/canonraw"
else
    echo "  重新编译 canonraw.c ..."
    clang -O2 -o "$HERE/canonraw" "$HERE/canonraw.c" \
        -framework IOKit -framework CoreFoundation
fi

echo "=== [2/7] 安装后端 ==="
cp "$HERE/canonraw" "$BACKEND"
chown root:wheel "$BACKEND"
chmod 755 "$BACKEND"
echo "  -> $BACKEND"

echo "=== [3/7] 安装 rastertocapt 过滤器 ==="
if [ -f "$HERE/rastertocapt" ]; then
    cp "$HERE/rastertocapt" "$FILTERDIR/rastertocapt"
elif [ ! -x "$FILTERDIR/rastertocapt" ]; then
    echo "  !! 找不到 rastertocapt,安装中止" >&2
    exit 1
fi
chown root:wheel "$FILTERDIR/rastertocapt"
chmod 755 "$FILTERDIR/rastertocapt"
echo "  -> $FILTERDIR/rastertocapt"

echo "=== [4/7] 安装 PPD ==="
# 统一过滤器名,避免出现 rastertocapt595 之类的历史遗留名
sed 's|rastertocapt[0-9a-z]*|rastertocapt|g' "$HERE/CanonLBP-2900-3000.ppd" \
    > "$PPDSTORE/$PPDNAME"
chmod 644 "$PPDSTORE/$PPDNAME"
cp "$PPDSTORE/$PPDNAME" "$HERE/$PPDNAME"
echo "  -> $PPDSTORE/$PPDNAME"

echo "=== [5/7] 停用会抢占 USB 的佳能官方组件 ==="
for f in "/Library/Printers/Canon/CUPSCAPT2/BackGrounder/Canon CAPT BackGrounder.app/Contents/MacOS/Canon CAPT BackGrounder" \
         "/Library/Printers/Canon/CUPSCAPT2/CCPD/ccpd" \
         "/Library/Printers/Canon/CUPSCAPT2/Bidi/captmoncnab3"; do
    if [ -f "$f" ] && [ ! -f "$f.disabled" ]; then
        mv "$f" "$f.disabled" && echo "  已停用: $(basename "$f")"
    fi
done
for p in /Library/LaunchAgents/jp.co.canon.CUPSCAPT2.BG.plist \
         /Library/LaunchDaemons/jp.co.canon.CUPSCAPT2.BG.plist; do
    if [ -f "$p" ]; then
        launchctl bootout "gui/$(id -u)/jp.co.canon.CUPSCAPT2.BG" 2>/dev/null || true
        launchctl bootout "system/jp.co.canon.CUPSCAPT2.BG" 2>/dev/null || true
        mv "$p" "$p.disabled" && echo "  已禁用自启: $(basename "$p")"
    fi
done
pkill -9 -f 'captmon[c]nab3' 2>/dev/null || true
pkill -9 -f 'CCPD/[c]cpd'    2>/dev/null || true

echo "=== [6/7] 建立打印队列 ==="
lpadmin -x "$QUEUE" 2>/dev/null || true
lpadmin -p "$QUEUE" -E -v "$DEVID" -P "$PPDSTORE/$PPDNAME" \
    -D "Canon LBP2900" -L "USB" -o printer-is-shared=false
cupsenable "$QUEUE" 2>/dev/null || true
cupsaccept "$QUEUE" 2>/dev/null || true
lpoptions -d "$QUEUE" >/dev/null 2>&1 || true
echo "  -> 队列 $QUEUE 已建立并设为默认"

echo "=== [7/7] 准备日志文件 ==="
touch /var/log/canonraw.log && chmod 666 /var/log/canonraw.log
echo "  -> /var/log/canonraw.log"

echo
echo "================ 安装完成 ================"
lpstat -p -d 2>&1
echo
echo "测试打印 :  lp -d $QUEUE 某个.pdf"
echo "查看日志 :  tail -f /var/log/canonraw.log"
echo "队列日志 :  tail -f /var/log/cups/error_log"
echo
echo "注意:"
echo "  1. 必须插在机身直连的 USB 口,不要经显示器/集线器扩展"
echo "  2. 若打印机被中断的作业卡死(传数据全部超时),断电重启打印机"
