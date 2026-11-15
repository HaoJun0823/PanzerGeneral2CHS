#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_scenario_chs.py — 由英文剧本简报 + 逐行译表生成简体中文剧本文件

设计要点（与 GUI97/MISC 的教训一致）：

1) SCENARIO/ 下混有两类文件，必须严格区分：
     - 简报文本（含长英文句 / 带 TAB 缩进的叙述）：需要翻译
     - 场景数据（城市名、部队代号、坐标，如 4PLAYER/ANTISAL/CIECHAN）：**原样复制，绝不翻译**
   判定方式见 is_brief_file()：只有出现「三个以上小写英文词组成的句子」或含 TAB 才算简报。

2) 译表按「唯一英文行」组织（Resources/CHS/scenario_terms.json），
   同一句在多个剧本中复用只翻一次；未收录的行保留英文，便于分批渐进翻译而不阻塞构建。

3) 保持文件结构：行数、行序、CRLF、末尾 0x1A（若有）、行首 TAB 缩进都原样保留。
   每行译文的 GBK 字节数若短于原文，行尾补半角空格到等长；
   这样即使游戏某些路径按原始字节偏移取串也不会整体错位。
   若译文超长则报错并列出具体行，要求缩短译法。

用法：
    python Tools/translate/gen_scenario_chs.py               # 生成并校验
    python Tools/translate/gen_scenario_chs.py --report      # 只打印未翻译清单
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parents[2]
ENG_DIR = ROOT / "Resources" / "ENG" / "SCENARIO"
CHS_DIR = ROOT / "Resources" / "CHS" / "SCENARIO"
KEYS = ROOT / "Resources" / "CHS" / "scenario_keys.txt"   # 英文键（自动生成，勿手改）
ZH = ROOT / "Resources" / "CHS" / "scenario_zh.txt"       # 中文译文，与 keys 逐行一一对应

SENTENCE = re.compile(r"[a-z]{3,}.*\s[a-z]{3,}.*\s[a-z]{3,}")
DATE_RE = re.compile(r"^([A-Z][a-z]+)\s+(\d{1,2}),\s+(\d{4})$")
MONTHS = {
    "January": "1", "February": "2", "March": "3", "April": "4",
    "May": "5", "June": "6", "July": "7", "August": "8",
    "September": "9", "October": "10", "November": "11", "December": "12",
}
COUNT_RE = re.compile(r"^(\d+)\s+(Players|Player|Turns|Turn)$")
ORDINAL_RE = re.compile(r"^(Two|Three|Four|Two|2|3|4)\s+Player(s)?$", re.I)


# 场景「数据文件」的首行其实是场景/战役标题，其余行是城市名、地形代码与
# 坐标（参与地图数据匹配，绝不能改）。之前整份文件原样复制，导致战役/场景
# 选择列表里出现英文标题（如 "Windsor 1943"、"Four Player Madness"）。
# 这里只替换首行，并保持该行字节数：译文短则行尾补半角空格，长则登记告警。
# 键 = 英文标题原文，值 = 中文（GBK 编码，汉字 2 字节）。
DATA_TITLES = {
    "Four Player Madness":           "四人混战",
    "Invasion Salerno":              "萨莱诺登陆",
    "Arracourt":                     "阿拉库尔",
    "Caen - Operation Goodwood":     "卡昂 - 古德伍德行动",
    "Caen - Charnwood - Oden Valley":"卡昂 - 查恩伍德 - 奥登谷",
    "Cauldron":                      "大熔炉",
    "Ciechanow":                     "切哈努夫",
    "Dessau":                        "德绍",
    "Finnish Finished?":             "芬兰完了？",
    "Finnish Vacation":              "芬兰假期",
    "Drive to the Sea":              "向海岸推进",
    "Gibraltar or Bust (3 Player)":  "直布罗陀或失败（3人）",
    "Kanev":                         "卡涅夫",
    "Kishinev":                      "基希讷乌",
    "Klin":                          "克林",
    "Operation Konrad":              "康拉德行动",
    "Leningrad":                     "列宁格勒",
    "Lillehammer":                   "利勒哈默尔",
    "Madrid Offensive":              "马德里攻势",
    "Malta":                         "马耳他",
    "Metz":                          "梅斯",
    "Caging the Bear (Multi)":       "困住巨熊（多人）",
    "Red Tide Rising (3 Player)":    "赤潮上涨（3人）",
    "Nordwind":                      "北风行动",
    "Novgorod":                      "诺夫哥罗德",
    "Oak Ridge":                     "橡树岭",
    "Zitadelle":                     "堡垒行动",
    "Prokhorovka":                   "普罗霍罗夫卡",
    "Pursuit To Tobruk":             "追击托布鲁克",
    "Red Tide Rising":               "赤潮上涨",
    "Race to Dunkirk":               "奔向敦刻尔克",
    "Avalanche":                     "雪崩行动",
    "Salerno - Return To Europe":    "萨莱诺 - 重返欧洲",
    "Saturn on the Chir":            "奇尔河上的土星",
    "Savannah":                      "萨凡纳",
    "Sedan":                         "色当",
    "Seelow Heights":                "塞洛高地",
    "Advance On St. Lo":             "向圣洛推进",
    "St. Lo - Operation Cobra":      "圣洛 - 眼镜蛇行动",
    "Suomussalmi":                   "苏奥穆斯萨尔米",
    "Thermopylae":                   "温泉关",
    "Tobruk":                        "托布鲁克",
    "Tutorial":                      "教程",
    "Viipuri":                       "维堡",
    "Volokolamsk":                   "沃洛科拉姆斯克",
    "Windsor":                       "温莎",
    "Windsor 1943":                  "温莎 1943",
    "Winter Storm":                  "冬季风暴",
}

def is_brief_file(text: str) -> bool:
    """判定是否为「简报文本」文件；数据文件返回 False。"""
    if "\t" in text:
        return True
    return any(SENTENCE.search(line) for line in text.split("\r\n"))


def auto_translate(line: str):
    """模板行自动翻译；返回 None 表示需要查人工译表。"""
    s = line.strip()
    if not s:
        return ""
    m = DATE_RE.match(s)
    if m:
        y, mo, d = m.group(3), MONTHS.get(m.group(1), m.group(1)), m.group(2)
        cn = f"{y}年{mo}月{d}日"
        # 短月份（May/June/July）原文只有 12 字节，中文写法会超长；
        # 退化为紧凑点分日期，仍保持「年.月.日」语义且不破坏等长约束。
        if len(cn.encode("gbk")) > len(s.encode("latin-1")):
            cn = f"{y}.{mo}.{d}"
        return cn
    m = COUNT_RE.match(s)
    if m:
        n, unit = m.group(1), m.group(2)
        return f"{n} 名玩家" if unit.lower().startswith("player") else f"{n} 回合"
    if s.lower() in ("two player", "two players"):
        return "2 名玩家"
    return None


def collect_keys():
    """从英文简报中提取所有『需要人工翻译』的唯一行，按稳定顺序排列。

    排序固定为 (-长度, 字典序)，只要英文基准不变，编号就永远稳定，
    中文译文文件因此可以只写译文、不重复冗长的英文原文。
    """
    keys = set()
    for src in sorted(ENG_DIR.glob("*.TXT")):
        text = src.read_bytes().decode("latin-1")
        if not is_brief_file(text):
            continue
        for line in text.replace("\r\n", "\n").split("\n"):
            s = line.strip()
            if not s or auto_translate(s) is not None:
                continue
            if all(ord(c) < 0x20 or ord(c) > 0x7e for c in s):
                continue                       # 0x1A 之类的控制符，原样保留
            keys.add(s)
    return sorted(keys, key=lambda s: (-len(s), s))


def load_terms():
    """返回 (译表, 键列表, 未翻译键列表)。"""
    keys = collect_keys()
    if KEYS.is_file():
        old = KEYS.read_text(encoding="utf-8").splitlines()
        if old != keys:
            print("!! 英文基准发生变化，scenario_keys.txt 需要重新生成（--emit-keys）。")
    zh_lines = []
    if ZH.is_file():
        zh_lines = ZH.read_text(encoding="utf-8").splitlines()
        if len(zh_lines) != len(keys):
            print(f"!! 译表行数 {len(zh_lines)} != 键数 {len(keys)}；按位置配对，多余行忽略。")
            zh_lines += [""] * max(0, len(keys) - len(zh_lines))
    terms = {}
    missing = []
    for k, v in zip(keys, zh_lines):
        if v.strip():
            terms[k] = v.strip()
        else:
            missing.append(k)
    return terms, keys, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true", help="列出未翻译的编号与原文")
    ap.add_argument("--emit-keys", action="store_true", help="生成/刷新英文键文件并打印编号清单")
    args = ap.parse_args()

    terms, keys, missing = load_terms()
    if args.emit_keys:
        KEYS.write_text("\n".join(keys) + "\n", encoding="utf-8")
        print(f"已写入 {KEYS}：{len(keys)} 行")
        for i, k in enumerate(keys, 1):
            print(f"{i:3d}| {k}")
        return
    CHS_DIR.mkdir(parents=True, exist_ok=True)

    files = sorted(ENG_DIR.glob("*.TXT"))
    untranslated = {}          # 原文行 -> 出现次数
    n_brief = n_data = n_data_titled = 0
    n_lines = n_hit = 0
    too_long = []

    for src in files:
        raw = src.read_bytes()
        # 统一按 CRLF 切分；保留「最后一个分隔符之后」的尾部（可能是 0x1A 或无）
        text = raw.decode("latin-1")
        if "\r\n" in text:
            parts = text.split("\r\n")
            sep = "\r\n"
        else:
            parts = text.split("\n")
            sep = "\n"

        if not is_brief_file(text):
            # 数据文件：只译首行标题，其余行（城市名/地形/坐标）保持原字节
            title = parts[0].rstrip("\r") if parts else ""
            cn = DATA_TITLES.get(title)
            if cn:
                tail_cr = "\r" if parts[0].endswith("\r") else ""
                cap = len(parts[0].encode("latin-1")) - len(tail_cr)
                b = cn.encode("gbk")
                if len(b) > cap:
                    too_long.append((src.name, title, len(b), cap, cn))
                else:
                    b += b" " * (cap - len(b))
                parts[0] = b.decode("latin-1") + tail_cr
                (CHS_DIR / src.name).write_bytes(sep.join(parts).encode("latin-1"))
                n_data_titled += 1
            else:
                (CHS_DIR / src.name).write_bytes(raw)
            n_data += 1
            continue

        n_brief += 1
        out_lines = []
        for line in parts:
            n_lines += 1
            # 行首缩进：英文基准用 TAB，但简体中文渲染路径会把 U+0009
            # 交给 GDI 取字形（SimSun 的 .notdef 是方框），屏幕上就成了「□□」。
            # 输出时统一换成等长空格（1 字节换 1 字节，行字节数不变）。
            m = re.match(r"^(\t*)", line)
            indent = m.group(1) if m else ""
            body = line[len(indent):]
            key = body.strip()
            if not key:
                out_lines.append(line)
                continue

            cn = auto_translate(key)
            if cn is None:
                cn = terms.get(key)
            if cn is None:
                if not all(ord(c) < 0x20 or ord(c) > 0x7e for c in key):
                    untranslated[key] = untranslated.get(key, 0) + 1
                out_lines.append(line)
                continue
            n_hit += 1
            # indent 不参与译文长度计算：它只在最后拼回一次。
            # 旧版这里写成 (indent + cn)，末尾又 append(indent + ...)，
            # 于是英文的 1 个 TAB 变成中文的 2 个 TAB，行还多出 indent 个字节。
            b = cn.encode("gbk")
            cap = len(body.encode("latin-1"))
            if len(b) > cap:
                # 短地名（Malta/Tobruk/Novgorod…）音译成中文必然超过原文字节数。
                # 剧本是按「行」索引的（已实机验证），不是按字节偏移，所以超长无害；
                # 这里保留完整译文、不补齐、不截断，只登记告警。
                too_long.append((src.name, key, len(b), cap, cn))
            else:
                b += b" " * (cap - len(b))     # 行尾补齐到原文字节数
            out_lines.append(indent + b.decode("latin-1"))

        # 兜底：无论译文命中与否，输出里都不留 TAB（含未翻译行的原样缩进）。
        out_lines = [ln.replace("\t", " ") for ln in out_lines]
        out = sep.join(out_lines).encode("latin-1")
        (CHS_DIR / src.name).write_bytes(out)

    print(f"简报文件 {n_brief} 个（已生成），数据文件 {n_data} 个（其中 {n_data_titled} 个已译首行标题，其余行原样）")
    print(f"处理行 {n_lines}，命中译表/模板 {n_hit}，未翻译唯一行 {len(untranslated)}")
    print(f"输出目录：{CHS_DIR}")

    if too_long:
        print(f"\n提示：{len(too_long)} 处译文长于原文（多为短地名音译），保持完整译文、不补齐：")
        for f, k, a, b, cn in too_long[:10]:
            print(f"   {f}: {a}B > {b}B  {k[:40]!r} -> {cn!r}")

    if untranslated:
        if args.report:
            print("\n=== 待翻译唯一行（按长度降序） ===")
            for k, v in sorted(untranslated.items(), key=lambda x: -len(x[0])):
                print(f"[{v}x] {k}")
        else:
            print(f"\n提示：还有 {len(untranslated)} 个唯一行未译，运行 --report 查看清单。")


if __name__ == "__main__":
    main()
