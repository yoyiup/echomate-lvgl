#!/usr/bin/env python3
"""
extract_ttc.py - 从 TrueType Collection (.ttc) 里拆出单个字体

为什么需要：
    Ubuntu 装的中文字体是 NotoSansCJK-Regular.ttc，一个文件里打包了
    10 个字体（JP/KR/SC/TC/HK × 常规/等宽）。lv_font_conv 只认单字体，
    遇到 ttcf 签名直接报 "Unsupported OpenType signature ttcf"。
    所以要先拆出 index=2（简体中文 SC）成为独立文件。

    fontTools 有更省事的做法（TTCollection + save），但沙箱里装不上，
    而 .ttc 的结构其实很简单，纯 struct 手工拆就够了：
       - 文件头 12 字节：'ttcf' + version + numFonts
       - 紧跟 numFonts 个 uint32，是每个子字体的表目录偏移
       - 每个子字体的表目录：sfnt header + numTables × 16 字节目录项
       - 每个目录项指向文件里某段真实数据（表）

    拆的时候只要把目标子字体引用到的表全部复制出来，重排偏移，
    拼成一个新的 sfnt 文件即可。表内容本身不用改。

用法：
    python3 tools/extract_ttc.py <input.ttc> <index> <output.ttf>

例：
    python3 tools/extract_ttc.py \\
        /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc \\
        2 fonts/NotoSansSC-Regular-extracted.ttf

怎么知道 index 是哪国语言：
    python3 -c "from PIL import ImageFont; \\
      [print(i, ImageFont.truetype(P,16,index=i).getname()) for i in range(10)]"
"""
import struct
import sys


def extract(src_path, index, out_path):
    with open(src_path, 'rb') as f:
        data = f.read()

    tag, major, minor, num_fonts = struct.unpack(">4sHHI", data[:12])
    if tag != b'ttcf':
        raise SystemExit("不是 .ttc 文件（签名 %r）" % tag)
    if index >= num_fonts:
        raise SystemExit("index %d 超出范围（共 %d 个字体）" % (index, num_fonts))

    offsets = struct.unpack(">%dI" % num_fonts, data[12:12 + 4 * num_fonts])
    off = offsets[index]

    sfnt_ver, num_tables, search_range, entry_sel, range_shift = \
        struct.unpack(">IHHHH", data[off:off + 12])

    tables = []
    for i in range(num_tables):
        p = off + 12 + i * 16
        t, checksum, t_off, t_len = struct.unpack(">4sIII", data[p:p + 16])
        tables.append((t, checksum, t_off, t_len))

    header_len = 12 + num_tables * 16
    body = bytearray()
    new_index = []
    for t, checksum, t_off, t_len in tables:
        while len(body) % 4:                 # 每张表 4 字节对齐
            body += b'\0'
        new_index.append((t, checksum, header_len + len(body), t_len))
        body += data[t_off:t_off + t_len]

    out = bytearray()
    out += struct.pack(">IHHHH", sfnt_ver, num_tables,
                       search_range, entry_sel, range_shift)
    for t, checksum, t_off, t_len in new_index:
        out += struct.pack(">4sIII", t, checksum, t_off, t_len)
    out += body

    with open(out_path, 'wb') as f:
        f.write(out)

    names = ' '.join(t.decode('latin-1') for t, _, _, _ in new_index)
    print("写出: %s" % out_path)
    print("  字节: %d" % len(out))
    print("  签名: 0x%08X (%s)" % (sfnt_ver, "TrueType"
                                   if sfnt_ver == 0x00010000 else "CFF/OTF"))
    print("  表  : %s" % names)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    extract(sys.argv[1], int(sys.argv[2]), sys.argv[3])
