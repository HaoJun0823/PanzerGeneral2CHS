#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
analyze_line_align.py — 对比原文/译文的逐行字节长度

用途：验证「游戏是否按原始字节偏移访问字符串」这一假设。
若假设成立，译文每行必须补齐到与原文完全相同的字节数。

输出：超长行清单（译文比原文长的行，需要缩短译文）+ 总字节数对比。
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "PG2CHS_DLL" / "build"


def load(p: Path) -> bytes:
    return p.read_bytes()


def split_lines(b: bytes):
    """按 CRLF 切分，保留每行内容。返回 (lines, sep) —— 假定全文统一 CRLF。"""
    if b"\r\n" in b:
        return b.split(b"\r\n"), b"\r\n"
    return b.split(b"\n"), b"\n"


def report(name: str):
    orig = load(BUILD / f"orig_{name}.TXT")
    chs = load(BUILD / f"chs_{name}.TXT")
    o_lines, o_sep = split_lines(orig)
    c_lines, c_sep = split_lines(chs)

    print(f"\n===== {name}.TXT =====")
    print(f"原文: {len(orig)} B / {len(o_lines)} 段 (sep={o_sep!r})")
    print(f"译文: {len(chs)} B / {len(c_lines)} 段 (sep={c_sep!r})")
    if len(o_lines) != len(c_lines):
        print(f"!! 段数不一致，差 {len(c_lines) - len(o_lines)}")
        return

    longer, shorter, equal = [], [], 0
    for i, (o, c) in enumerate(zip(o_lines, c_lines)):
        if len(c) > len(o):
            longer.append((i + 1, len(o), len(c), o, c))
        elif len(c) < len(o):
            shorter.append((i + 1, len(o) - len(c)))
        else:
            equal += 1

    print(f"逐行长度: 相等 {equal} / 译文更短 {len(shorter)} / 译文更长 {len(longer)}")
    if shorter:
        deltas = [d for _, d in shorter]
        print(f"  译文更短的总缺口 {sum(deltas)} B，单行最大缺口 {max(deltas)} B")
    if longer:
        print(f"  !! 译文更长的行（必须缩短）共 {len(longer)} 行：")
        for no, lo, lc, o, c in longer[:25]:
            print(f"    行{no}: 原文{lo}B 译文{lc}B  {c[:40]!r}")


def main():
    for n in ("GUI97", "MISC"):
        try:
            report(n)
        except FileNotFoundError as e:
            print(f"跳过 {n}: {e}")


if __name__ == "__main__":
    main()
