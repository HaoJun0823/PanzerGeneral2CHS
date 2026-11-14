# -*- coding: utf-8 -*-
"""
用 GDI GetGlyphOutlineW(GGO_GRAY8_BITMAP) 复现 DLL 里的字形合成算法，
把结果放大成 PNG，用于在不上真机的情况下评估字号/阈值/字体效果。

用法： python Tools/dev/glyph_preview.py [字号] [阈值] [字体]
输出： build/glyph_preview_<字号>px.png（放大 4 倍，附 1:1 原样）
"""
import ctypes as C
import sys, os
from ctypes import wintypes as W
from PIL import Image

gdi32 = C.WinDLL("gdi32", use_last_error=True)
user32 = C.WinDLL("user32", use_last_error=True)

GGO_BITMAP = 1
GGO_GRAY8_BITMAP = 6
GDI_ERROR = 0xFFFFFFFF

HDC = C.c_void_p
HFONT = C.c_void_p
HGDIOBJ = C.c_void_p

gdi32.CreateCompatibleDC.argtypes = [HDC]
gdi32.CreateCompatibleDC.restype = HDC
gdi32.CreateFontW.argtypes = [C.c_int, C.c_int, C.c_int, C.c_int, C.c_int,
                              C.c_int, C.c_int, C.c_int, C.c_int, C.c_int,
                              C.c_int, C.c_int, C.c_int, C.c_wchar_p]
gdi32.CreateFontW.restype = HFONT
gdi32.SelectObject.argtypes = [HDC, HGDIOBJ]
gdi32.SelectObject.restype = HGDIOBJ
gdi32.DeleteObject.argtypes = [HGDIOBJ]
gdi32.DeleteObject.restype = C.c_int
gdi32.DeleteDC.argtypes = [HDC]
gdi32.DeleteDC.restype = C.c_int


class GLYPHMETRICS(C.Structure):
    _fields_ = [("gmBlackBoxX", W.UINT), ("gmBlackBoxY", W.UINT),
                ("gmptGlyphOrigin", W.POINT), ("gmCellIncX", C.c_short),
                ("gmCellIncY", C.c_short)]


class MAT2(C.Structure):
    _fields_ = [("eM11", C.c_byte * 4), ("eM12", C.c_byte * 4),
                ("eM21", C.c_byte * 4), ("eM22", C.c_byte * 4)]


class TEXTMETRIC(C.Structure):
    _fields_ = [("tmHeight", C.c_long), ("tmAscent", C.c_long),
                ("tmDescent", C.c_long), ("tmInternalLeading", C.c_long),
                ("tmExternalLeading", C.c_long), ("tmAveCharWidth", C.c_long),
                ("tmMaxCharWidth", C.c_long), ("tmWeight", C.c_long),
                ("tmOverhang", C.c_long), ("tmDigitizedAspectX", C.c_long),
                ("tmDigitizedAspectY", C.c_long), ("tmFirstChar", C.c_wchar),
                ("tmLastChar", C.c_wchar), ("tmDefaultChar", C.c_wchar),
                ("tmBreakChar", C.c_wchar), ("tmItalic", C.c_byte),
                ("tmUnderlined", C.c_byte), ("tmStruckOut", C.c_byte),
                ("tmPitchAndFamily", C.c_byte), ("tmCharSet", C.c_byte)]


def make_mat2():
    # 与 DLL 中一致：{0,1},{0,0},{0,0},{0,1}
    m = MAT2()
    for f, vals in (("eM11", (0, 0, 1, 0)), ("eM12", (0, 0, 0, 0)),
                    ("eM21", (0, 0, 0, 0)), ("eM22", (0, 0, 1, 0))):
        getattr(m, f)[:] = bytes(vals)
    return m


gdi32.GetTextMetricsW.argtypes = [HDC, C.POINTER(TEXTMETRIC)]
gdi32.GetTextMetricsW.restype = C.c_int
gdi32.GetGlyphOutlineW.argtypes = [HDC, W.UINT, W.UINT, C.POINTER(GLYPHMETRICS),
                                   W.DWORD, C.c_void_p, C.POINTER(MAT2)]
gdi32.GetGlyphOutlineW.restype = W.DWORD


def glyph(px, thresh, fontname, chars, scale=4, mode="g8"):
    hdc = gdi32.CreateCompatibleDC(None)
    hfont = gdi32.CreateFontW(-px, 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 0, 0, fontname)
    old = gdi32.SelectObject(hdc, hfont)
    tm = TEXTMETRIC()
    gdi32.GetTextMetricsW(hdc, C.byref(tm))
    ascent = tm.tmAscent
    m = make_mat2()
    fmt = GGO_BITMAP if mode == "bmp" else GGO_GRAY8_BITMAP

    cells = []
    for ch in chars:
        gm = GLYPHMETRICS()
        size = gdi32.GetGlyphOutlineW(hdc, ord(ch), fmt,
                                      C.byref(gm), 0, None, C.byref(m))
        if size in (GDI_ERROR, 0):
            cells.append((ch, 0, 0, b"", 0, 0, px, 0))
            continue
        buf = (C.c_ubyte * size)()
        gdi32.GetGlyphOutlineW(hdc, ord(ch), fmt,
                               C.byref(gm), size, buf, C.byref(m))
        cells.append((ch, gm.gmBlackBoxX, gm.gmBlackBoxY, bytes(buf),
                      gm.gmptGlyphOrigin.x, gm.gmptGlyphOrigin.y, gm.gmCellIncX,
                      max(buf) if size else 0))

    gdi32.SelectObject(hdc, old)
    gdi32.DeleteObject(hfont)
    gdi32.DeleteDC(hdc)

    print(f"字体={fontname} 字号={px}px ascent={ascent} 阈值={thresh}")
    W_total = sum(max(c[6], 1) for c in cells) + 2
    H_total = px + 2
    img = Image.new("L", (W_total, H_total), 0)

    pen = 1
    for (ch, bbw, bbh, buf, ox, oy, adv, mx) in cells:
        bufPitch = ((bbw + 31) // 32) * 4 if mode == "bmp" else ((bbw + 3) & ~3)
        left = pen + ox
        top = ascent - oy
        written = 0
        for r in range(bbh):
            for c in range(bbw):
                if mode == "bmp":
                    on = (buf[r * bufPitch + (c >> 3)] >> (7 - (c & 7))) & 1
                else:
                    on = 1 if (buf[r * bufPitch + c] * 255 // 64) >= thresh else 0
                if not on:
                    continue
                x, y = left + c, top + r
                if 0 <= x < W_total and 0 <= y < H_total:
                    img.putpixel((x, y), 255)
                    written += 1
        print(f"  {ch!r}: cell={bbw}x{bbh} origin=({ox},{oy}) adv={adv} "
              f"maxCov={mx}/64 写入像素={written}")
        pen += max(adv, 1)

    out_dir = os.path.join(os.path.dirname(__file__), "..", "..", "PG2CHS_DLL", "build")
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    tag = f"{px}px_T{thresh}_{fontname}_{mode}".replace(" ", "")
    big = img.resize((W_total * scale, H_total * scale), Image.NEAREST)
    p1 = os.path.join(out_dir, f"glyph_{tag}.png")
    big.save(p1)
    print("已输出:", p1)
    return p1


if __name__ == "__main__":
    px = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    th = int(sys.argv[2]) if len(sys.argv) > 2 else 96
    fn = sys.argv[3] if len(sys.argv) > 3 else "SimSun"
    mode = sys.argv[4] if len(sys.argv) > 4 else "g8"
    text = "战略继续游戏退出"
    glyph(px, th, fn, text, mode=mode)
