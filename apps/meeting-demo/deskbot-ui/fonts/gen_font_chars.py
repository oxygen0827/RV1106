#!/usr/bin/env python3
# 生成 lv_font_conv --symbols 用的字符清单：
#   ASCII 0x20-0x7E + GB2312 区1-9 全角符号 + 区16-55 一级汉字(3755) + 常用扩展标点
import sys

mode = sys.argv[1] if len(sys.argv) > 1 else "full"

def gb2312_range(a_lo, a_hi):
    chars = []
    for a in range(a_lo, a_hi + 1):
        for b in range(0xA1, 0xFF):
            try:
                c = bytes([a, b]).decode("gb2312")
                chars.append(c)
            except UnicodeDecodeError:
                pass
    return chars

chars = []
if mode == "ui22":   # 22px：仅页面 UI 文案
    chars = list("会议纪要运行中启动已停止按住提问打断退出正在失败开启录音转写未点击")
    chars += "：，。（）【】"
else:                # 14px：完整覆盖（转写文本）
    chars += [chr(c) for c in range(0x20, 0x7F)]          # ASCII
    chars += gb2312_range(0xA1, 0xA9)                     # 全角符号/希腊/西里尔/拼音
    chars += gb2312_range(0xB0, 0xD7)                     # GB2312 一级汉字
    chars += list("—…‘’“”•·×÷")                          # 常用扩展标点
    # 页面文案兜底（防漏）
    chars += list("会议纪要运行中启动已停止按住提问打断退出正在失败")

seen = set()
out = []
for c in chars:
    if c not in seen and c != "\n" and c != "\r":
        seen.add(c)
        out.append(c)
text = "".join(out)
sys.stdout.write(text)
