#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_release.py — 校验《装甲元帅2》汉化发布包结构

不依赖构建脚本内部状态，独立断言「这个 zip 能否真的装进游戏跑起来」。
对标参考项目（SR_GAME_CHINESE）verify_release.py 的独立复核思路。

包内布局（见 build_release.py）：
  根：winmm.dll / 必读说明.txt / License.txt / ExtraTerms.txt / FragTitleFix.txt
      SCENARIO/*.TXT
  update/：winmm.dll / PG2CHS.ini / GUI97.TXT 等文本表 / SCENARIO/*.TXT
           / ExtraTerms.txt / FragTitleFix.txt / PG2CHS_DLL.asi
  GOG 版快捷方式把工作目录设为 update\，因此**文本表在 update/ 下**，
  旧版校验脚本检查根目录会误报「缺少顶层文本表」。

用法：
  python Tools/ci/verify_release.py build_out                  # 校验目录下最新的 zip
  python Tools/ci/verify_release.py build_out --require-asi   # CI：缺 ASI 直接失败
  python Tools/ci/verify_release.py build_out --strict        # 警告也当失败
  python Tools/ci/verify_release.py path/to/xxx.zip
"""

import argparse
import glob
import os
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


def verify(zip_path: Path, strict: bool, require_asi: bool):
    errors, warns = [], []

    if not zip_path.is_file():
        print(f"!! 未找到发布包：{zip_path}"); sys.exit(1)

    with zipfile.ZipFile(zip_path) as z:
        names = set(z.namelist())

        def find_one(*candidates):
            """按优先级返回包内实际存在的路径。"""
            return next((c for c in candidates if c in names), None)

        # 1) ASI Loader 必须存在且非空（根目录 + update 各一份）
        for path in ("winmm.dll", "update/winmm.dll"):
            if path not in names:
                errors.append(f"缺少 {path}（ASI Loader）")
            elif z.getinfo(path).file_size < 10_000:
                errors.append(f"{path} 体积异常（{z.getinfo(path).file_size} B），疑似非法 Loader")

        # 2) 玩家必读说明
        if "必读说明.txt" not in names:
            warns.append("缺少 必读说明.txt")

        # 3) 许可
        if "License.txt" not in names:
            warns.append("缺少 License.txt（合并许可）")

        # 4) 顶层文本表：GOG 版从 update/ 读取；兼容旧包放根目录
        for t in ROOT_TXT:
            hit = find_one(f"update/{t}", t)
            if hit is None:
                errors.append(f"缺少文本表 {t}（应位于 update/ 下）")
            else:
                data = z.read(hit)
                if b"\r" in data and strict:
                    warns.append(f"{hit} 含 CRLF，游戏按行解析可能带入尾随 \\r")

        # 5) 场景文本：根目录与 update/ 至少各一套，按文件名去重计数
        scen = [n for n in names
                if (n.startswith("SCENARIO/") or n.startswith("update/SCENARIO/"))
                and n.lower().endswith(".txt")]
        scen_names = {Path(n).name.upper() for n in scen}
        if not scen:
            errors.append("SCENARIO/ 下无任何 .txt（场景文本缺失）")
        elif len(scen_names) < 100:
            warns.append(f"场景文本仅 {len(scen_names)} 个（预期 ~306）")
        if not any(n.startswith("update/SCENARIO/") for n in scen):
            errors.append("缺少 update/SCENARIO/（GOG 版工作目录读不到场景文本）")

        # 6) 运行参数与显示层译名表
        if "update/PG2CHS.ini" not in names:
            warns.append("缺少 update/PG2CHS.ini（字体/字号参数）")
        for f in ("ExtraTerms.txt", "FragTitleFix.txt"):
            if f"update/{f}" not in names and f not in names:
                warns.append(f"缺少 {f}（显示层译名/标题校订）")

        # 7) ASI 插件：CI 默认硬要求（编译失败不能静默降级成纯文本包）
        asi = sorted(n for n in names if n.endswith("PG2CHS_DLL.asi"))
        if not asi:
            msg = "未包含 PG2CHS_DLL.asi —— 本包无法提供中文字形（纯文本）"
            (errors if require_asi else warns).append(msg)
        else:
            data = z.read(asi[0])
            if not pe_is_32bit_dll(data):
                errors.append(f"{asi[0]} 不是 32 位 PE DLL（游戏是 32 位，必须 x86）")
            elif len(data) < 10_000:
                warns.append(f"{asi[0]} 体积过小（{len(data)} B），疑似占位/未实现")

        # 8) 字体（可选）
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
    ap.add_argument("path", help="发布包 zip，或含 zip 的目录")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--require-asi", action="store_true",
                    help="缺少 32 位 PG2CHS_DLL.asi 时直接失败（CI 用）")
    args = ap.parse_args()

    p = Path(args.path)
    if p.is_dir():
        zips = glob.glob(str(p / "PanzerGeneral2-*.zip"))
        if not zips:
            print(f"!! 目录内没有 PanzerGeneral2-*.zip：{p}")
            sys.exit(1)
        # 本地 build_out 常有多个历史包；取最新校验，CI 上只会有一个
        zips.sort(key=lambda x: os.path.getmtime(x), reverse=True)
        if len(zips) > 1:
            print(f"  ⚠ 目录内 {len(zips)} 个发布包，仅校验最新的：{Path(zips[0]).name}")
        p = Path(zips[0])
    verify(p, args.strict, args.require_asi)


if __name__ == "__main__":
    main()
