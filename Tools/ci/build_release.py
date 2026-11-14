#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_release.py — 装配《装甲元帅2》简体中文汉化发布包

把「中文文本表 + ASI Loader + （编译好的）ASI 插件 + 字体 + 许可」
按游戏根目录布局收进一个带「日期-时间」戳的 zip，可直接解压覆盖到游戏根目录。

设计对标参考项目（SR_GAME_CHINESE / MajestyIIExtend）的 build_release.py：
  - 构建输入在 dist/ 与 Resources/CHS/（源材料，入库）；
  - 构建产物在 --out-root（默认 build_out/，gitignore，不入库）；
  - 不修改任何游戏原始文件，纯「覆盖层」。

用法：
  python Tools/ci/build_release.py                      # 默认输出到 build_out/
  python Tools/ci/build_release.py --out-root D:/rel
  python Tools/ci/build_release.py --lang CHS
  python Tools/ci/build_release.py --skip-dll           # 不打包 ASI（纯文本阶段）
  python Tools/ci/build_release.py --dll-path X:/PG2CHS_DLL.dll   # 显式指定已编译 DLL
  python Tools/ci/build_release.py --date 20261114      # 固定日期，便于复现
"""

import argparse
import os
import sys
import time
import shutil
import zipfile
from datetime import datetime, timezone, timedelta
from pathlib import Path

# 始终 UTF-8，避免中文路径/文本在 CI 上 UnicodeEncodeError
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parents[2]          # .../PanzerGeneral2CHS
DIST = ROOT / "dist"
RESOURCES = ROOT / "Resources"
LICENSE = ROOT / "LICENSE"
LICENSES_DIR = ROOT / "LICENSES"
DLL_PROJECT = ROOT / "PG2CHS_DLL"
FONTS = ROOT / "Fonts"

# 顶层必须存在的 5 个行式文本表（与 Resources/ENG 一致）
ROOT_TXT = ["GUI97.TXT", "NAMES.TXT", "EQUIP97.TXT", "MISC.TXT", "BLANK.TXT"]

# 上海时区（+8），与参考项目发版 tag 一致
CST = timezone(timedelta(hours=8))


def find_compiled_dll(explicit: str | None, skip_dll: bool):
    """定位已编译的 PG2CHS_DLL.dll，返回路径或 None。"""
    if skip_dll:
        print("[dll] --skip-dll：跳过 ASI 打包（纯文本阶段）")
        return None
    if explicit:
        p = Path(explicit)
        if p.is_file():
            print(f"[dll] 显式指定：{p}")
            return p
        print(f"!! [dll] --dll-path 指定的文件不存在：{p}")
        return None
    # 常见 VS / 本地构建输出位置
    candidates = [
        DLL_PROJECT / "build" / "PG2CHS_DLL.dll",
        DLL_PROJECT / "Release" / "PG2CHS_DLL.dll",
        DLL_PROJECT / "Win32" / "Release" / "PG2CHS_DLL.dll",
        DLL_PROJECT / "x64" / "Release" / "PG2CHS_DLL.dll",
    ]
    for c in candidates:
        if c.is_file():
            print(f"[dll] 自动发现：{c}")
            return c
    print("[dll] 未发现已编译的 PG2CHS_DLL.dll —— 本次为「纯文本」构建（不含 ASI）。")
    return None


def merge_license() -> str:
    """合并 LICENSE（主许可）+ LICENSES/* 为单一 License.txt 文本。"""
    parts = []
    if LICENSE.is_file():
        parts.append(LICENSE.read_text(encoding="utf-8", errors="replace"))
    if LICENSES_DIR.is_dir():
        for f in sorted(LICENSES_DIR.iterdir()):
            if f.is_file() and f.suffix == ".txt":
                parts.append("\n\n" + "=" * 72 + "\n"
                             + f"第三方许可：{f.name}\n" + "=" * 72 + "\n\n"
                             + f.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(parts)


def build(out_root: Path, lang: str, date: str | None, skip_dll: bool, dll_path: str | None):
    chs = RESOURCES / lang
    if not chs.is_dir():
        print(f"!! Resources/{lang} 不存在"); sys.exit(1)

    stamp_date = date or datetime.now(CST).strftime("%Y-%m-%d")
    stamp_time = datetime.now(CST).strftime("%H-%M-%S")
    zip_name = f"PanzerGeneral2-{lang}-{stamp_date}-{stamp_time}.zip"
    out_root.mkdir(parents=True, exist_ok=True)
    zip_path = out_root / zip_name

    stage = out_root / "_stage"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    copied = []

    # 实机确认：GOG 版由快捷方式把「工作目录」设为 update\，
    # 游戏读文本表 / ini / .asi 全部相对该目录；只有 winmm.dll 两种位置都要放。
    upd = stage / "update"
    upd.mkdir(exist_ok=True)

    # 1) ASI Loader（必须，32 位）
    loader = DIST / "common" / "winmm.dll"
    if loader.is_file():
        shutil.copy2(loader, stage / "winmm.dll")
        shutil.copy2(loader, upd / "winmm.dll")
        copied.append("winmm.dll (根 + update)")
    else:
        print("!! 缺少 dist/common/winmm.dll（ASI Loader）"); sys.exit(1)

    # 2) 玩家必读说明
    readme = DIST / "common" / "必读说明.txt"
    if readme.is_file():
        shutil.copy2(readme, stage / "必读说明.txt"); copied.append("必读说明.txt")

    # 2b) 运行参数（字体/字号/微调，改后重启游戏即生效）
    cfg = DIST / "common" / "PG2CHS.ini"
    if cfg.is_file():
        shutil.copy2(cfg, upd / "PG2CHS.ini"); copied.append("update/PG2CHS.ini")

    # 2c) 显示层译名表：把城市/河流/装备/姓氏的英文显示串换成中文。
    #     磁盘上的 NAMES/EQUIP/SCENARIO 数据文件仍是英文，逻辑与存档零改动。
    terms = chs / "ExtraTerms.txt"
    if terms.is_file():
        shutil.copy2(terms, upd / "ExtraTerms.txt")
        shutil.copy2(terms, stage / "ExtraTerms.txt")
        copied.append("ExtraTerms.txt (根 + update)")
    else:
        print("!! 缺少 Resources/CHS/ExtraTerms.txt（显示层译名表）")

    # 2d) 关卡按钮标题校订表：修正 1 字节碎片在多候选时的 nearest 互换
    fix = chs / "FragTitleFix.txt"
    if fix.is_file():
        shutil.copy2(fix, upd / "FragTitleFix.txt")
        shutil.copy2(fix, stage / "FragTitleFix.txt")
        copied.append("FragTitleFix.txt (根 + update)")
    else:
        print("!! 缺少 Resources/CHS/FragTitleFix.txt（按钮标题校订表）")

    # 3) 顶层文本表（行式位置索引，保持行数/顺序）
    for name in ROOT_TXT:
        src = chs / name
        if src.is_file():
            shutil.copy2(src, upd / name); copied.append(f"update/{name}")
        else:
            print(f"!! Resources/{lang}/{name} 缺失（文本表不完整）")

    # 4) 场景文本：根目录与工作目录各一份，兼容两种相对路径解析
    scen_src = chs / "SCENARIO"
    scen_n = 0
    if scen_src.is_dir():
        for dst in (stage / "SCENARIO", upd / "SCENARIO"):
            dst.mkdir(exist_ok=True)
            for t in sorted(scen_src.glob("*.TXT")):
                shutil.copy2(t, dst / t.name)
                if dst == stage / "SCENARIO":
                    scen_n += 1
        copied.append(f"SCENARIO/ + update/SCENARIO/ (各 {scen_n} 个)")

    # 5) ASI 插件（若已编译）
    dll = find_compiled_dll(dll_path, skip_dll)
    if dll is not None:
        dest = upd / "PG2CHS_DLL.asi"
        shutil.copy2(dll, dest)
        copied.append(f"update/PG2CHS_DLL.asi ({dest.stat().st_size} B)")

    # 6) 字体（若放置了 CJK 字体）
    font_n = 0
    if FONTS.is_dir():
        for f in sorted(FONTS.iterdir()):
            if f.suffix.lower() in (".ttf", ".otf", ".ttc"):
                shutil.copy2(f, stage / f.name); font_n += 1
        if font_n:
            copied.append(f"Fonts ({font_n} 个)")

    # 7) 合并许可
    (stage / "License.txt").write_text(merge_license(), encoding="utf-8")
    copied.append("License.txt")

    # 8) 打包
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(stage).as_posix())

    shutil.rmtree(stage)

    print("\n=== 构建完成 ===")
    print(f"  产物：{zip_path}")
    print(f"  大小：{zip_path.stat().st_size} B")
    print(f"  内容：{', '.join(copied)}")
    if dll is None:
        print("  提示：本次为纯文本构建（未含 ASI）。渲染中文需后续接入 PG2CHS_DLL.asi。")
    return zip_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-root", default=str(ROOT / "build_out"))
    ap.add_argument("--lang", default="CHS")
    ap.add_argument("--date", default=None, help="固定日期 YYYY-MM-DD，便于复现")
    ap.add_argument("--skip-dll", action="store_true")
    ap.add_argument("--dll-path", default=None)
    args = ap.parse_args()
    build(Path(args.out_root), args.lang, args.date, args.skip_dll, args.dll_path)


if __name__ == "__main__":
    main()
