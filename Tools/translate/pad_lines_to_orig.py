#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pad_lines_to_orig.py — 把译文每行补齐到与原文相同的字节数

背景：实机日志证明游戏取到的中文是「行中间的子串」（例如"战斗"来自"战斗机"），
疑似游戏按原始字节偏移/长度访问字符串表。若如此，译文只要与原文逐行等长，
任何基于偏移的访问都会重新对齐。

做法：译文较短的行在行尾补半角空格；超长的行原样保留（后续人工缩短）。
行数、行序、CRLF 全部不变。

用法：
  python Tools/translate/pad_lines_to_orig.py --check     # 只报告
  python Tools/translate/pad_lines_to_orig.py --write     # 生成到 build/，再手动部署
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "PG2CHS_DLL" / "build"


def pad(name: str, write: bool):
    orig = (BUILD / f"orig_{name}.TXT").read_bytes()
    chs = (BUILD / f"chs_{name}.TXT").read_bytes()

    o = orig.split(b"\r\n")
    c = chs.split(b"\r\n")
    if len(o) != len(c):
        print(f"!! {name}: 段数不一致 {len(o)} vs {len(c)}，中止")
        return False

    out, padded, longer = [], 0, 0
    for x, y in zip(o, c):
        if len(y) < len(x):
            out.append(y + b" " * (len(x) - len(y)))
            padded += 1
        else:
            if len(y) > len(x):
                longer += 1
            out.append(y)

    result = b"\r\n".join(out)
    print(f"{name}.TXT  原文 {len(orig)}B  译文 {len(chs)}B  ->  补齐后 {len(result)}B")
    print(f"  补齐 {padded} 行；仍超长 {longer} 行；段数 {len(out)}（与原文一致）")
    if len(result) != len(orig):
        print(f"  注：总字节与原文差 {len(result) - len(orig)}B（超长行所致，行结构不受影响）")
    if write:
        dst = BUILD / f"pad_{name}.TXT"
        dst.write_bytes(result)
        print(f"  已写出 {dst}")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    ok = True
    for n in ("GUI97", "MISC"):
        try:
            ok &= pad(n, args.write)
        except FileNotFoundError as e:
            print(f"跳过 {n}: {e}")
            ok = False
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
