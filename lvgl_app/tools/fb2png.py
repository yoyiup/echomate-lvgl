#!/usr/bin/env python3
"""
fb2png.py —— 把板子的 /dev/fb0 转成 PNG

用法（在宿主上跑）：
    # 1. 先从板子把显存抓下来
    ssh 板子 'dd if=/dev/fb0 of=/tmp/fb.bin bs=153600 count=1'
    scp 板子:/tmp/fb.bin /tmp/fb.bin

    # 2. 转成图片
    python3 fb2png.py /tmp/fb.bin out.png [放大倍数]

为什么不用 PIL：板上和宿主都不一定装了，而且这里只处理
RGB565 一种格式，用 zlib 手写 PNG 反而更短更可控。
"""
import struct
import sys
import zlib

# ★ 默认按屏幕尺寸 320x240。**相机帧是 640x480，必须显式传 --wh 640 480**，
#   否则会按 320x240 读 → 每行只读前 320 像素、下一行数据被当成新行 →
#   产生密集的"假横条纹"。我曾因此误判成 ISP debayer 有问题，白查半天。
W, H = 320, 240


def rgb565_to_rgb888(v):
    """RGB565 -> (r,g,b)。
    5 位扩到 8 位用 v*255//31，比 v<<3 更均匀（后者全 1 只到 248）。"""
    return ((v >> 11) & 0x1F) * 255 // 31, \
           ((v >> 5) & 0x3F) * 255 // 63, \
           (v & 0x1F) * 255 // 31


def png_chunk(tag, data):
    return (struct.pack('>I', len(data)) + tag + data +
            struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))


def build_png(pixels, w, h):
    raw = bytearray()
    for y in range(h):
        raw.append(0)                      # 每行开头的 filter 类型：0 = None
        for x in range(w):
            raw += bytes(rgb565_to_rgb888(pixels[y * w + x]))

    return (b'\x89PNG\r\n\x1a\n'
            + png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + png_chunk(b'IDAT', zlib.compress(bytes(raw), 9))
            + png_chunk(b'IEND', b''))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    global W, H

    args = sys.argv[1:]
    # --wh W H：显式指定源图的宽高（相机帧必须传，屏幕抓图不用）
    if "--wh" in args:
        i = args.index("--wh")
        W = int(args[i + 1])
        H = int(args[i + 2])
        del args[i:i + 3]

    if len(args) < 2:
        print(__doc__)
        return 1

    src, dst = args[0], args[1]
    scale = int(args[2]) if len(args) > 2 else 2

    data = open(src, 'rb').read()
    need = W * H * 2
    if len(data) < need:
        print(f"错误: {src} 只有 {len(data)} 字节，按 {W}x{H} 需要 {need}")
        print(f"      如果这是相机帧（640x480），请加 --wh 640 480")
        return 1

    pixels = struct.unpack('<%dH' % (W * H), data[:need])

    if scale == 1:
        png = build_png(pixels, W, H)
    else:
        # 最近邻放大：不引入插值失真，看像素边界更清楚
        big = []
        for y in range(H):
            row = pixels[y * W:(y + 1) * W]
            for _ in range(scale):
                for v in row:
                    big.extend([v] * scale)
        png = build_png(big, W * scale, H * scale)

    open(dst, 'wb').write(png)

    # 顺便报一下颜色统计，能快速判断"界面到底画没画"
    from collections import Counter
    c = Counter(pixels)
    print(f"已生成 {dst}")
    print(f"  唯一颜色 {len(c)} 种，最常见的 3 种:")
    for v, n in c.most_common(3):
        print(f"    0x{v:04x}  {n:6d} px  {n * 100.0 / len(pixels):5.1f}%")
    return 0


if __name__ == '__main__':
    sys.exit(main())
