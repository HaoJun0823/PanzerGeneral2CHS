#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_release.py — 校验《装甲元帅2》汉化发布包结构

不依赖构建脚本内部状态，独立断言「这个 zip 能否真的装进游戏跑起来」。
对标参考项目（SR_GAME_CHINESE）verify_release.py 的独立复核思路。

用法：
  python Tools/ci/verify_release.py build_out                 # 校验目录下唯一 zip
  python Tools/ci/verify_release.py build_out --strict        # 警告也当失败
  python Tools/ci/verify_release.py path/to/xxx.zip
"""

import argparse
import glob
import sys
import zipfile
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT_TXT = ["GUI97.TXT", "NAMES.TXT", "EQUIP97.TXT", "MISC.TXT", "BLANK.TXT"]


def pe_is_32bit_dll(data: bytes) -> bool:
    """粗略判断是否为 32 位 PE DLL（MZ + PE32 + DLL 特征）。"""
    if data[:2] != b"MZ":
        return False
    e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        return False
    # PE 签名(4) + COFF 头(20)。可选头 magic 在 COFF 偏移 20 处。
    magic = int.from_bytes(data[e_lfanew + 4 + 20:e_lfanew + 4 + 22], "little")
    if magic != 0x10B:          # 0x10B = PE32（32 位）
        return False
    # Characteristics 是 COFF 头的最后 2 字节（COFF 偏移 18），含 0x2000（DLL）。
    ch = int.from_bytes(data[e_lfanew + 4 + 18:e_lfanew + 4 + 20], "little")
    return bool(ch & 0x2000)


def verify(zip_path: Path, strict: bool):
    errors, warns = [], []

    if not zip_path.is_file():
        print(f"!! 未找到发布包：{zip_path}"); sys.exit(1)

    with zipfile.ZipFile(zip_path) as z:
        names = set(z.namelist())

        # 1) ASI Loader 必须存在且非空
        if "winmm.dll" not in names:
            errors.append("缺少 winmm.dll（ASI Loader）")
        else:
            sz = z.getinfo("winmm.dll").file_size
            if sz < 10_000:
                errors.append(f"winmm.dll 体积异常（{sz} B），疑似非法 Loader")

        # 2) 玩家必读说明
        if "必读说明.txt" not in names:
            warns.append("缺少 必读说明.txt")

        # 3) 许可
        if "License.txt" not in names:
            warns.append("缺少 License.txt（合并许可）")

        # 4) 顶层文本表（行式位置索引，缺失会破坏取文本）
        for t in ROOT_TXT:
            if t not in names:
                errors.append(f"缺少顶层文本表 {t}")
            else:
                data = z.read(t)
                if b"\r" in data and strict:
                    warns.append(f"{t} 含 CRLF，游戏按行解析可能带入尾随 \\r")

        # 5) 场景文本目录
        scen = [n for n in names if n.startswith("SCENARIO/") and n.lower().endswith(".txt")]
        if not scen:
            errors.append("SCENARIO/ 下无任何 .txt（场景文本缺失）")
        elif len(scen) < 100:
            warns.append(f"SCENARIO/ 仅 {len(scen)} 个 .txt（预期 ~306）")

        # 6) ASI 插件（可选，但强烈建议）
        asi = [n for n in names if n.endswith("PG2CHS_DLL.asi")]
        if not asi:
            warns.append("未包含 scripts/PG2CHS_DLL.asi —— 本次包无法提供中文字形（纯文本）")
        else:
            data = z.read(asi[0])
            if not pe_is_32bit_dll(data):
                errors.append(f"{asi[0]} 不是 32 位 PE DLL（游戏是 32 位，必须 x86）")
            elif len(data) < 10_000:
                warns.append(f"{asi[0]} 体积过小（{len(data)} B），疑似占位/未实现")

        # 7) 字体（可选）
        fonts = [n for n in names if n.lower().endswith((".ttf", ".otf", ".ttc"))]
        if not fonts:
            warns.append("未包含 CJK 字体（若 ASI 走 TTF 重定向则必需）")

    print(f"校验：{zip_path.name}")
    for w in warns:
        print(f"  ⚠ {w}")
    for e in errors:
        print(f"  ✗ {e}")

    if errors or (strict and warns):
        print(f"\n校验失败：{len(errors)} 错误 / {len(warns)} 警告")
        sys.exit(1)
    print(f"\n✓ 校验通过（{len(warns)} 警告，{len(errors)} 错误）")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", help="发布包 zip，或含唯一 zip 的目录")
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    p = Path(args.path)
    if p.is_dir():
        zips = glob.glob(str(p / "PanzerGeneral2-*.zip"))
        if len(zips) != 1:
            print(f"!! 目录内期望 1 个 PanzerGeneral2-*.zip，实际 {len(zips)} 个")
            sys.exit(1)
        p = Path(zips[0])
    verify(p, args.strict)


if __name__ == "__main__":
    main()
