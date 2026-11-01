#!/usr/bin/env python3
"""生成高信息量诊断页 PDF：一次打印即可区分
   - band 重叠（只有顶部出图）
   - 行宽错位（垂直线变斜 / 错开）
   - 缩放 / 偏移
PDF 坐标原点在左下角，y 向上；A4 = 595 x 842。
"""

W, H = 595, 842
ops = []

# 1) 页面外边框
ops.append(b"3 w 0 0 0 RG")
ops.append(b"30 30 535 782 re S")

# 2) 顶部粗黑条（最显眼的"顶部出图"标志）
ops.append(b"0 0 0 rg")
ops.append(b"30 798 535 14 re f")

# 3) 等间距水平线，共 12 条，从顶部往下；左端标 H1..H12
for k, y in enumerate(range(780, 100, -60), start=1):
    ops.append(b"0 0 0 rg")
    ops.append(b"30 %d 535 3 re f" % y)          # 3pt 粗水平线
    ops.append(b"BT /F1 12 Tf 34 %d Td (H%d) Tj ET" % (y + 6, k))

# 4) 等间距垂直线，共 10 条；顶端标 V1..V10
for k, x in enumerate(range(90, 570, 48), start=1):
    ops.append(b"0 0 0 rg")
    ops.append(b"%d 120 2 630 re f" % x)          # 2pt 粗垂直线
    ops.append(b"BT /F1 10 Tf %d 760 Td (%d) Tj ET" % (x - 4, k))

# 5) 中部实心黑块（证明渲染有内容）
ops.append(b"0 0 0 rg")
ops.append(b"230 400 130 80 re f")

# 6) 底部大号文字
ops.append(b"BT /F1 28 Tf 40 70 Td (LBP2900 DIAG) Tj ET")

content = b"\n".join(ops) + b"\n"

objects = []
objects.append(b"<< /Type /Catalog /Pages 2 0 R >>")
objects.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
objects.append(
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] "
    b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>" % (W, H)
)
objects.append(b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream")
objects.append(
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman /Encoding /WinAnsiEncoding >>"
)

out = bytearray()
out += b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"
offsets = []
for i, obj in enumerate(objects, start=1):
    offsets.append(len(out))
    out += b"%d 0 obj\n" % i + obj + b"\nendobj\n"

xref_pos = len(out)
out += b"xref\n0 %d\n" % (len(objects) + 1)
out += b"0000000000 65535 f \n"
for off in offsets:
    out += b"%010d 00000 n \n" % off
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
    len(objects) + 1,
    xref_pos,
)

path = "/tmp/lbp2900/diag.pdf"
with open(path, "wb") as f:
    f.write(bytes(out))
print("written %s size=%d" % (path, len(out)))
