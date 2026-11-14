#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_extra_terms.py — 生成「显示层英文→中文」译名表 Resources/CHS/ExtraTerms.txt

为什么不改磁盘上的数据文件
--------------------------
SCENARIO/*.TXT 的非首行（城市名、河流名、部队代号、坐标）参与地图数据匹配；
NAMES.TXT / EQUIP97.TXT 的名字按索引进存档。改文件长度会同时破坏匹配与兼容性。
因此只在 DLL 的 DrawText 入口把「显示出来的纯 ASCII 串」换成中文，游戏内部
逻辑、存档、索引全部保持英文。本表就是那张替换表。

格式：GBK 编码，每行 `英文<TAB>中文`，';' 开头为注释。
OVERRIDES 是人工校订的高质量译名；未收录的名称按 SYLLABLE 规则音译，
生成结果可直接逐条修改，重跑脚本前把改好的条目加进 OVERRIDES 即可固定。

用法：
    python Tools/translate/gen_extra_terms.py            # 生成并校验
    python Tools/translate/gen_extra_terms.py --preview  # 打印前 80 条
"""

import argparse
import glob
import re
import sys
from collections import Counter
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parents[2]
ENG = ROOT / "Resources" / "ENG"
OUT = ROOT / "Resources" / "CHS" / "ExtraTerms.txt"

# ---------------------------------------------------------------------------
# 人工校订译名：知名地名、实机日志里出现过的词、全部通用单位/地形词。
# 这里优先于自动音译，也是后续修正的唯一入口。
# ---------------------------------------------------------------------------
OVERRIDES = {
    # 用户点名 / 实机日志确认
    "Carentan": "卡朗坦",
    "Merderet River": "梅德雷河",
    "Tessay-sur-Vire": "泰塞叙尔维尔",
    "Tessy-sur-Vire": "泰西叙尔维尔",
    "Regular": "正规军",
    # 诺曼底 / 西线
    "St Lo": "圣洛",
    "St. Lo": "圣洛",
    "Caen": "卡昂",
    "Bayeux": "巴约",
    "Isigny": "伊西尼",
    "Coutances": "库唐斯",
    "Sainteny": "圣特尼",
    "Lessay": "莱塞",
    "Periers": "佩里耶",
    "Airel": "艾雷勒",
    "Baupte": "博普特",
    "Portail": "波尔塔伊",
    "Trevieres": "特雷维耶尔",
    "Longueville": "隆格维尔",
    "Vierville sur Mer": "滨海维耶维尔",
    "Grandcamp les Bains": "格朗康莱班",
    "Torigni-sur-Vire": "托里尼叙尔维尔",
    "St Souveur le Vicomte": "圣索沃尔勒维孔特",
    "St Souveur Landelin": "圣索沃尔朗德兰",
    "St Souveur de Pierre Pont": "圣索沃尔德皮埃尔蓬",
    "St Jean de Day": "圣让德代",
    "La Haye du Puits": "拉艾迪皮",
    "St Lo d'Orville": "圣洛多尔维尔",
    "Manigny": "马尼尼",
    "Thury Harcourt": "蒂里阿尔古",
    "Aure River": "奥尔河",
    "Douve River": "杜夫河",
    "Taute River": "托特河",
    "Vire River": "维尔河",
    "Drome River": "德罗姆河",
    "Seinne River": "塞讷河",
    "Moselle River": "摩泽尔河",
    "Rigel Ridge": "参宿七岭",
    "Hatian Ridge": "阿蒂安岭",
    # 意大利 / 萨莱诺
    "Salerno": "萨莱诺",
    "Savannah": "萨凡纳",
    "Avellino": "阿韦利诺",
    "Persano": "佩尔萨诺",
    "Battipaglia": "巴蒂帕利亚",
    "Castellammare": "卡斯特拉马雷",
    "Sorrento": "索伦托",
    "Vietri": "维耶特里",
    "Baronissi": "巴罗尼西",
    "San Severino": "圣塞韦里诺",
    "Nocera": "诺切拉",
    "Monteforte": "蒙泰福尔泰",
    "Acerno": "阿切尔诺",
    "Eboli": "埃博利",
    "Altavilla": "阿尔塔维拉",
    "Paestum": "帕埃斯图姆",
    "Contursi": "孔图尔西",
    "Maiori": "马约里",
    "Pompei": "庞贝",
    "Torre Annunziata": "托雷安农齐亚塔",
    "Tobacco Factory": "烟草工厂",
    "Cava di Terrelli": "卡瓦迪泰雷利",
    "Montecorvino": "蒙泰科尔维诺",
    "Naples": "那不勒斯",
    "Rome": "罗马",
    "Sicily": "西西里",
    # 德国 / 中欧
    "Dessau": "德绍",
    "Bitterfeld": "比特费尔德",
    "Halberstadt": "哈尔伯施塔特",
    "Quedlinburg": "奎德林堡",
    "Aschersleben": "阿舍斯莱本",
    "Stassfurt": "施塔斯富特",
    "Hettstedt": "黑特施泰特",
    "Sangerhausen": "桑格豪森",
    "Halle": "哈雷",
    "Leipzig": "莱比锡",
    "Barby": "巴尔比",
    "Merseburg": "梅泽堡",
    "Berlin": "柏林",
    "Metz": "梅斯",
    "Sedan": "色当",
    "Nancy": "南锡",
    "Windsor": "温莎",
    "London": "伦敦",
    "Arracourt": "阿拉库尔",
    "Tobruk": "托布鲁克",
    "Acroma": "阿克鲁马",
    "Viipuri": "维堡",
    "Suomussalmi": "苏奥穆斯萨尔米",
    "Ciechanow": "切哈努夫",
    "Volokolamsk": "沃洛科拉姆斯克",
    "Leningrad": "列宁格勒",
    "Prokhorovka": "普罗霍罗夫卡",
    "Guadalajara": "瓜达拉哈拉",
    "Brihuega": "布里韦加",
    "Seelow": "塞洛",
    "Stalingrad": "斯大林格勒",
    "Moscow": "莫斯科",
    "Warsaw": "华沙",
    "Paris": "巴黎",
    "Dunkirk": "敦刻尔克",
    "Antwerp": "安特卫普",
    "Bastogne": "巴斯托涅",
    "Arnhem": "阿纳姆",
    "Aachen": "亚琛",
    "El Alamein": "阿拉曼",
    "Cassino": "卡西诺",
    "Anzio": "安齐奥",
    "Kursk": "库尔斯克",
    "Kiev": "基辅",
    "Minsk": "明斯克",
    "Smolensk": "斯摩棱斯克",
    "Rostov": "罗斯托夫",
    "Kharkov": "哈尔科夫",
    "Sevastopol": "塞瓦斯托波尔",
    "Mjosa Lake": "米约萨湖",
    # 通用单位 / 地形 / 界面词（来自 SCENARIO 与 EQUIP97）
    "Truck": "卡车",
    "Wagon": "辎重车",
    "Strongpoint": "据点",
    "Fortification": "工事",
    "Engineer": "工兵",
    "Cavalry": "骑兵",
    "Sea Transport": "海运船",
    "Two Player": "双人对战",
    "Three Player": "三人对战",
    "Four Player": "四人对战",
    "Sissi": "山地兵",
    "Ski": "滑雪兵",
    "Chasseurs": "猎兵",
    "Infantry": "步兵",
    "Artillery": "火炮",
    "Armor": "装甲兵",
    "Recon": "侦察兵",
    "Fighter": "战斗机",
    "Bomber": "轰炸机",
    "Transport": "运输队",
    "Supply": "补给",
    "Headquarters": "指挥部",
    "Airfield": "机场",
    "Bridge": "桥梁",
    "Factory": "工厂",
    "Hills": "丘陵",
    "Forest": "森林",
    "Swamp": "沼泽",
    "River": "河流",
    "Road": "道路",
    "City": "城市",
    "Town": "城镇",
    "Village": "村庄",
    "Ridge": "山岭",
    "Lake": "湖泊",
    "Mountain": "山地",
    "Desert": "沙漠",
    "Beach": "滩头",
    "Port": "港口",
    "Victory": "胜利",
    "Defeat": "失败",
    "Players": "玩家数",
    "Turns": "回合数",
    # 常见将领姓氏（NAMES.TXT 里出现频率高 / 玩家熟悉）
    "Adler": "阿德勒",
    "Bauer": "鲍尔",
    "Braun": "布劳恩",
    "Blucher": "布吕歇尔",
    "Balck": "巴尔克",
    "Bachmann": "巴赫曼",
    "Brecht": "布雷希特",
    "Brunner": "布伦纳",
    "Buchmann": "布赫曼",
    "Cramer": "克拉默",
    "Wallace": "华莱士",
    "Washington": "华盛顿",
    "West": "韦斯特",
    "Wilcox": "威尔科克斯",
    "Williams": "威廉姆斯",
    "Woods": "伍兹",
    "Youngblood": "扬布拉德",
    "Ackenheil": "阿肯海尔",
    "Arndt": "阿恩特",
    "Auerbacher": "奥尔巴赫",
    "Campenhausen": "坎彭豪森",
    "Carlburg": "卡尔堡",
    "Christmann": "克里斯特曼",
    "Creutz": "克罗伊茨",
    "Dahrendorf": "达伦多夫",
    # 场景/战役标题里的常见英文词（中文版已译首行，这里兜底）
    "Madrid Offensive": "马德里攻势",
    "Four Player Madness": "四人混战",
    "Finnish Vacation": "芬兰假期",
    "Gibraltar or Bust": "直布罗陀或败退",
    "Operation Konrad": "康拉德行动",
    "Invasion Salerno": "萨莱诺登陆",   # 与 SCENARIO 数据文件首行保持一致
    "Savannah River": "萨凡纳河",       # 与关卡「萨凡纳」同指一地
    "Advance On St. Lo": "向圣洛推进",
    "Arromanches les Bains": "阿罗芒什莱班",
    "Courseulles sur Mer": "滨海库尔瑟勒",
    "Colleville sur Mer": "滨海科勒维尔",
    "St Laurent sur Mer": "滨海圣洛朗",
    "Brettville l'Orgueilleuse": "布雷特维尔洛格约斯",
    "Brettville sur Laize": "莱兹河畔布雷特维尔",
    "La Galissoniere Class": "拉加利索尼埃级",
    "Le Fantasque Class": "空想级",
    "Scharnhorst Class": "沙恩霍斯特级",
    "King George V Class": "乔治五世级",
    "Sovetskiy Soyuz Class": "苏联级",
    "Illustrious Class": "光辉级",
    "Baltimore Class": "巴尔的摩级",
    "Condottieri Class": "佣兵队长级",
    "Storozhevoi Class": "警戒级",
}

# ---------------------------------------------------------------------------
# 音节式音译：给未收录的小地名 / 姓氏一个可读的中文名。
# 不是标准译名，但比整屏英文好；可在 ExtraTerms.txt 里逐条改。
# ---------------------------------------------------------------------------
# 整音节 -> 单个汉字（关键：不是「声母字 + 元音字」拼两三个字，
# 否则 Carentan 会变成「克阿尔恩特阿恩」这种不可读的串）。
_V = dict(a="阿", e="埃", i="伊", o="奥", u="乌", y="伊")
CV = {
    "": _V,
    "b": dict(a="巴", e="贝", i="比", o="博", u="布", y="比"),
    "c": dict(a="卡", e="策", i="齐", o="科", u="库", y="齐"),
    "d": dict(a="达", e="德", i="迪", o="多", u="杜", y="迪"),
    "f": dict(a="法", e="费", i="菲", o="福", u="弗", y="菲"),
    "g": dict(a="加", e="格", i="吉", o="戈", u="古", y="吉"),
    "h": dict(a="哈", e="赫", i="希", o="霍", u="胡", y="希"),
    "j": dict(a="雅", e="耶", i="吉", o="约", u="尤", y="吉"),
    "k": dict(a="卡", e="克", i="基", o="科", u="库", y="基"),
    "l": dict(a="拉", e="勒", i="利", o="洛", u="卢", y="利"),
    "m": dict(a="马", e="梅", i="米", o="莫", u="穆", y="米"),
    "n": dict(a="纳", e="内", i="尼", o="诺", u="努", y="尼"),
    "p": dict(a="帕", e="佩", i="皮", o="波", u="普", y="皮"),
    "q": dict(a="卡", e="克", i="基", o="科", u="库", y="基"),
    "r": dict(a="拉", e="雷", i="里", o="罗", u="鲁", y="里"),
    "s": dict(a="萨", e="塞", i="西", o="索", u="苏", y="西"),
    "t": dict(a="塔", e="特", i="蒂", o="托", u="图", y="蒂"),
    "v": dict(a="瓦", e="韦", i="维", o="沃", u="武", y="维"),
    "w": dict(a="瓦", e="韦", i="维", o="沃", u="伍", y="维"),
    "z": dict(a="察", e="策", i="齐", o="佐", u="祖", y="齐"),
}
# 带鼻韵尾 -n 的常见音节直接给一个汉字（卡+伦+坦 = 卡伦坦）
CVN = {
    ("t", "a"): "坦", ("r", "e"): "伦", ("l", "a"): "兰", ("m", "a"): "曼",
    ("n", "a"): "南", ("b", "a"): "班", ("d", "a"): "丹", ("g", "a"): "甘",
    ("k", "a"): "坎", ("s", "a"): "桑", ("t", "e"): "滕", ("m", "e"): "门",
    ("n", "e"): "嫩", ("b", "e"): "本", ("d", "e"): "登", ("l", "e"): "伦",
    ("s", "e"): "森", ("g", "e"): "根", ("k", "e"): "肯", ("t", "i"): "廷",
    ("l", "i"): "林", ("m", "i"): "明", ("n", "i"): "宁", ("s", "i"): "辛",
    ("r", "i"): "林", ("l", "o"): "隆", ("t", "o"): "顿", ("m", "o"): "蒙",
    ("n", "o"): "农", ("s", "o"): "松", ("r", "o"): "龙", ("d", "o"): "东",
    ("g", "o"): "贡", ("b", "o"): "邦", ("k", "o"): "孔", ("l", "u"): "伦",
    ("m", "u"): "蒙", ("n", "u"): "农", ("r", "u"): "伦", ("t", "u"): "通",
    ("s", "u"): "松", ("d", "u"): "敦", ("g", "u"): "贡", ("b", "u"): "本",
    ("k", "u"): "昆", ("", "a"): "安", ("", "e"): "恩", ("", "i"): "因",
    ("", "o"): "翁", ("", "u"): "温",
}
# 复辅音声母：单独成字，后接元音字
COMPLEX = [
    ("sch", "施"), ("str", "施特"), ("spr", "施普"), ("st", "施"),
    ("sp", "施"), ("ch", "希"), ("sh", "什"), ("th", "特"),
    ("ph", "弗"), ("qu", "克"), ("zh", "日"), ("ts", "茨"),
    ("cz", "恰"), ("kn", "克"), ("gn", "尼"), ("pf", "普夫"),
    ("br", "布"), ("gr", "格"), ("kr", "克"), ("tr", "特"),
    ("dr", "德"), ("pr", "普"), ("fr", "弗"), ("bl", "布"),
    ("fl", "弗"), ("gl", "格"), ("kl", "克"), ("pl", "普"),
    ("sl", "斯"), ("wr", "尔"),
]
CODA = {
    "n": "恩", "m": "姆", "r": "尔", "l": "勒", "s": "斯",
    "t": "特", "d": "德", "g": "格", "k": "克", "b": "布",
    "p": "普", "f": "弗", "x": "斯", "z": "茨",
}
SUFFIX = {
    "hausen": "豪森", "stadt": "施塔特", "stein": "施泰因",
    "berg": "贝格", "burg": "堡", "dorf": "多夫", "bach": "巴赫",
    "heim": "海姆", "feld": "费尔德", "wald": "瓦尔德",
    "bruck": "布吕克", "kirch": "基希", "hof": "霍夫",
    "thal": "塔尔", "witz": "维茨", "grad": "格勒",
    "ville": "维尔", "mont": "蒙", "sur": "叙尔",
    "saint": "圣", "santa": "圣", "pol": "波尔",
}
TAIL_WORDS = {
    "River": "河", "Lake": "湖", "Ridge": "岭", "Hill": "山",
    "Hills": "丘陵", "Island": "岛", "Isle": "岛", "Bay": "湾",
    "Forest": "森林", "Wood": "林", "Woods": "林", "Factory": "工厂",
    "Airfield": "机场", "Bridge": "桥", "Class": "级", "Heights": "高地",
    "Heath": "荒原", "Park": "公园", "Ford": "渡口", "Crossroads": "岔路口",
}

VOWELS = "aeiouy"


def _syllables(word):
    """把词切成 (声母, 单元音, 韵尾) 序列；每个音节最终只出一个汉字。"""
    w = re.sub(r"([bcdfghjklmnpqrstvwxyz])\1", r"\1", word.lower())
    w = re.sub(r"[^a-z]", "", w)
    out = []
    i = 0
    while i < len(w):
        j = i
        while j < len(w) and w[j] not in VOWELS:
            j += 1
        cons = w[i:j]
        if j >= len(w):
            if cons:
                out.append((cons, "", ""))
            break
        k = j
        while k < len(w) and w[k] in VOWELS:
            k += 1
        voc = w[j]                       # 多元音簇取第一个，避免出两字
        m = k
        while m < len(w) and w[m] not in VOWELS:
            m += 1
        coda = w[k:m]
        if m < len(w) and coda:
            # 后续还有元音：把最后一个辅音让给下个音节作声母
            out.append((cons, voc, coda[:-1]))
            i = m - 1
        else:
            out.append((cons, voc, coda))
            i = m
    return out


def _syllable_zh(cons, voc, coda):
    # 已知构词成分优先
    if not voc:
        for pre, zh in COMPLEX:
            if cons.startswith(pre):
                return zh
        return CV.get(cons[:1], _V).get("e", "厄") if cons else ""
    if coda == "n" and (cons[-1:] if cons else "", voc) in CVN:
        return CVN[(cons[-1:] if cons else "", voc)]
    for pre, zh in COMPLEX:
        if cons.startswith(pre):
            return zh + _V.get(voc, "")
    base = cons[-1:] if cons else ""
    if base == "x":
        return "克" + _V.get(voc, "")
    s = CV.get(base, _V).get(voc, "")
    if coda:
        s += CODA.get(coda[0], "")
    return s


def transliterate(name):
    # 后缀词： "... River" -> "...河"
    parts = name.split()
    tail = ""
    if len(parts) > 1 and parts[-1] in TAIL_WORDS:
        tail = TAIL_WORDS[parts[-1]]
        name = " ".join(parts[:-1])
    # 法语/英语地名前缀：St / Saint / San / Fort / Mont
    m = re.match(r"^(Saint|St\.?|San|Santa|Fort|Mont|Mount)\s+(.+)$", name, re.I)
    if m:
        head = {"saint": "圣", "st": "圣", "st.": "圣", "san": "圣",
                "santa": "圣", "fort": "堡", "mont": "蒙", "mount": "山"}.get(
                    m.group(1).lower(), "")
        return head + transliterate(m.group(2)) + tail
    # 已知构词成分（berg / burg / stadt ...）
    low = name.lower()
    for suf, zh in SUFFIX.items():
        if low.endswith(suf) and len(low) > len(suf) + 1:
            return transliterate(name[:len(name) - len(suf)]) + zh + tail

    s = "".join(_syllable_zh(*syl) for syl in _syllables(name)) + tail
    s = re.sub(r"(.)\1{2,}", r"\1\1", s)
    return s[:8] if s else name


# ---------------------------------------------------------------------------
def collect_place_names():
    names = Counter()
    for f in glob.glob(str(ENG / "SCENARIO" / "*")):
        for raw in open(f, "rb").read().replace(b"\r\n", b"\n").split(b"\n"):
            t = raw.decode("latin1").strip()
            if not (3 <= len(t) <= 40):
                continue
            if not re.fullmatch(r"[A-Za-z][A-Za-z .'\-/&()]*", t):
                continue
            if not re.search(r"[a-z]", t) or re.search(r"\d", t):
                continue
            if len(t.split()) >= 5:
                continue
            # "Caen - Operation Charnwood" 这类是场景标题，中文版已译首行，
            # 不会以英文显示；音译它们只会污染译名表。
            if " - " in t or "/" in t:
                continue
            if any(w in t.lower() for w in
                   ["players", "turns", "turn", "scenario", "victory",
                    " the ", " and ", " of ", " to ", " in "]):
                continue
            names[t] += 1
    return names


def collect_lines(path):
    out = []
    for raw in open(path, "rb").read().replace(b"\r\n", b"\n").split(b"\n"):
        t = raw.decode("latin1").strip()
        if t and t != "\x1a":
            out.append(t)
    return out


# 装备型号（含数字/罗马数字/连字符的制式代号）保持英文：它们是国际通用识别码，
# 硬翻反而妨碍老玩家辨认，也和之前 EQUIP97 不翻译的决定一致。
MODEL_LIKE = re.compile(r"\d|[-/]|\b(?:I|II|III|IV|V|mm|cm|FlaK|PaK|StuG|Pz|PSW|Me|Ju|Fokker)\b", re.I)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()

    terms = {}
    places = collect_place_names()

    # 1) 地名 / 通用词（SCENARIO）
    for en in places:
        terms[en] = OVERRIDES.get(en) or transliterate(en)

    # 2) NAMES.TXT 姓氏：全部音译，常见者用手工表
    surnames = collect_lines(ENG / "NAMES.TXT")
    for en in dict.fromkeys(surnames):
        if en not in terms:
            terms[en] = OVERRIDES.get(en) or transliterate(en)

    # 3) EQUIP97.TXT：只收纯文字条目，制式型号保持英文
    equip = collect_lines(ENG / "EQUIP97.TXT")
    kept = 0
    for en in dict.fromkeys(equip):
        if MODEL_LIKE.search(en):
            continue
        if en in terms:
            continue
        terms[en] = OVERRIDES.get(en) or transliterate(en)
        kept += 1

    # 4) 手工表里没出现在源文件、但仍需替换的词（补齐）
    for en, zh in OVERRIDES.items():
        terms.setdefault(en, zh)

    # 校验：GBK 可编码、不含 TAB/CRLF、译文不为空且不过长
    bad = []
    for en, zh in terms.items():
        if not zh or "\t" in en or "\t" in zh or len(zh.encode("gbk", "replace")) > 24:
            bad.append((en, zh))
            continue
        try:
            en.encode("ascii")
            zh.encode("gbk")
        except UnicodeEncodeError:
            bad.append((en, zh))
    if bad:
        print("!! 非法条目:", bad[:10])
        raise SystemExit(1)

    # 按英文长度降序写出（DLL 里也会再排一次，双保险）
    ordered = sorted(terms.items(), key=lambda kv: (-len(kv[0]), kv[0]))
    lines = [
        "; PG2 汉化 · 显示层英文词条译名表（GBK）",
        "; 每行: 英文<TAB>中文 ; 分号开头为注释",
        "; 本表只影响画面显示，游戏内部/存档/地图匹配仍用英文。",
        "; OVERRIDES 为人工校订；其余为 Tools/translate/gen_extra_terms.py 自动音译，可直接逐条修改。",
        "; 想临时关闭：PG2CHS.ini 里 TranslateExtra=0",
    ]
    lines += [f"{en}\t{zh}" for en, zh in ordered]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(("\r\n".join(lines) + "\r\n").encode("gbk"))

    print(f"生成 {OUT}  {OUT.stat().st_size} 字节  词条 {len(ordered)}")
    print(f"  地名/通用词 {len(places)}  姓氏 {len(set(surnames))}  装备纯文字 {kept}")
    print(f"  手工校订覆盖 {sum(1 for k in terms if k in OVERRIDES)} 条")
    if args.preview:
        print("\n--- 前 80 条（按长度降序）---")
        for en, zh in ordered[:80]:
            print(f"  {en}\t{zh}")


if __name__ == "__main__":
    main()
