# 装甲元帅 2（Panzer General 2）简体中文汉化工程 · PanzerGeneral2CHS

> ⚠️ **使用前必读**
>
> 本补丁为个人非官方作品，**未获游戏官方授权**；安装会修改或注入游戏文件，
> 可能与该游戏的用户协议（EULA）冲突。请自行判断是否可用，风险自担，
> **安装前请备份存档与原始文件**。
>
> **禁止商业使用**；二次修改后分发，须公开源代码与改动记录，并保留原作者署名。
> 作者不对使用后果承担责任，也不拥有本游戏及其原始素材的版权。
> 完整条款见 [LICENSE](LICENSE)。

通过 **ASI（DLL）插件注入** 给 32 位《装甲元帅 2》（GOG 版 `PG2UK220.EXE`）
提供中文文本与 CJK 字形支持，实现**简体中文**汉化。

> 📖 **第一次接触本项目，请先读 [`Documents/research_handoff.md`](Documents/research_handoff.md)** ——
> 那是完整的技术交接文档：PE/资源/字符串分析结论、渲染路径开放问题、阶段化汉化路线、
> 以及用 IDA Pro + ida-pro-mcp 继续深挖的待办清单。

---

## 目标程序

| 项目 | 说明 |
|---|---|
| 主程序 | `PG2UK220.EXE`（GOG 增强兼容主程序，834 KB，PE32） |
| 不处理 | `PANZER2.EXE`（旧主程序，不在范围内） |
| 文本来源 | 外部行式 `.txt`（GUI97 / NAMES / EQUIP97 / MISC + `SCENARIO/*.TXT` 共 311 个） |
| 字形来源 | 游戏自带位图字体 `fontpg.dat`（打包在 `PANZER2.DAT`，原版已带 `.deu/.fra` 多语框架） |

---

## 快速开始

### 玩家：安装
下载 Release 里的 zip，**解压到游戏根目录**（exe 所在处），覆盖同名文件。

> ⚠️ 安装步骤与细节以包内 `必读说明.txt` 为准。渲染重定向（CJK 字形合成）已完成 MVP，
> 当前可用的发布包已能注入并渲染中文；缺的是 `Resources/CHS/` 的中文文本翻译
> （目前仍是英文基线，需逐行汉化）。

### 开发者：构建
```bash
cd G:\Projects\PanzerGeneral2CHS

# 1) 编译 ASI（MSVC 2017 v141 x86，与参考项目同款配方）
python Tools\ci\build_dll_local.py

# 2) 装配发布包（Resources/CHS + dist → build_out/PanzerGeneral2-CHS-*.zip）
python Tools\ci\build_release.py --out-root build_out

# 3) 校验发布包结构
python Tools\ci\verify_release.py build_out
```

CI（GitHub Actions）在 push 主分支时自动：msvc-wine 编译 32 位 ASI → 装配 zip →
上传 Artifact → 同 run 内按「日期-时间」打 tag 并发布 Release。

---

## 两个必须知道的铁律

1. **`Resources/CHS/` 下的文本表是「行式位置索引」**：翻译时**保持行数与顺序不变**，
   逐行替换。任一行的增删都会导致游戏按行号取文本时整体错位。
2. **CJK 字形走「GDI TTF → 8 位帧缓冲」合成（已确认并实现 MVP）**：游戏原生位图字体
   不含中文字形，纯替换 `.txt` 文本会出现「方块/空白」。渲染路径已用 IDA 锁定——
   游戏内文字是 8 位调色板帧缓冲的逐像素写入，本补丁在 hook 点用 GDI `GetGlyphOutline`
   取中文字形、经游戏颜色表写进帧缓冲（见 `research_handoff.md` §2 / §3.1）。
   当前 MVP 直接使用系统 CJK 字体（微软雅黑 / 宋体），无需额外字库文件。

---

## 翻译进度（Resources/CHS）

文本表是「行式位置索引」，翻译由 `Tools/translate/gen_chs_text.py` 生成（源码 UTF-8、
写出 GBK，严格对齐行数，保留末尾 `0x1A` 控制符）。已翻译文本经 `sub_43B2C2` 渲染为
中文；纯 ASCII 专有名词走原渲染器，不影响显示。

| 文件 | 行数 | 内容 | 状态 |
|------|------|------|------|
| `GUI97.TXT` | 440 | 菜单 / 界面 / 热键 / 天气 / 单位类别 | ✅ 已译（2026-11-14） |
| `MISC.TXT` | 27 | 地形类型 / 战斗术语 | ✅ 已译（2026-11-14） |
| `NAMES.TXT` | 400 | 指挥官姓氏（德语/俄语/英/美专有名词） | ⏸ 暂不改（ASCII 渲染器即可显示） |
| `EQUIP97.TXT` | ~170 | 装备/单位型号（StuG IIIG、Pz IVG 等专有代号） | ⏸ 暂不改（同上） |
| `SCENARIO/*.TXT` | 250 | 各剧本说明长文本 | 🔜 待译（批次二） |

> 已译文件已部署到游戏根目录（及 `update\`）供真机验证；游戏原始英文已备份到
> `I:\LocalGames\Panzer General 2\PG2CHS_backup\`。

---

## 目录导航

| 目录 / 文件 | 说明 |
|------------|------|
| `Documents/research_handoff.md` | ★ 完整技术文档（先读这个） |
| `PG2CHS_DLL/` | ASI/DLL 汉化插件源码（VS2017 v141，Win32+x64，Win10 SDK 26100） |
| `Resources/ENG/` | 游戏原始英文文本基线（翻译源，行式索引） |
| `Resources/CHS/` | 中文翻译工作副本（初始=英文，待译；保持行数/顺序） |
| `Fonts/` | 可选自包含 CJK 字体（当前 MVP 直接用系统字体：微软雅黑/宋体） |
| `dist/` | 构建**输入**源：ASI Loader（`common/winmm.dll`）+ `必读说明.txt` |
| `Tools/ci/` | 构建、打包、校验脚本 |
| `build_out/` | 构建产物输出（gitignore，不入版本控制） |
| `LICENSES/` | 第三方依赖许可证（ASILoader / Apache-2.0 / 思源；MinHook、stb 备用） |

---

## 环境依赖

- **Python 3**（+ 后续字体/文本工具链）
- **Visual Studio 2017（MSVC 14.16 / v141）** + **Windows SDK 10.0.26100.0**（与参考项目同款，
  保证 msvc-wine CI 与本地一致）
- 可选：**IDA Pro + ida-pro-mcp**（用于逆向渲染路径与字符串交叉引用）

## 游戏路径

```
I:\LocalGames\Panzer General 2\PG2UK220.EXE
```

---

## 参考项目（同作者，本仓库的 LICENSE / 工作流 / 资源打包 CI 思路取自它们）

- `G:/Projects/ArmyMenCHS` —— 玩具兵大战系列，ASI/内存补丁
- `G:/Projects/MajestyIIExtend` —— Majesty2，ASI + 字体烘焙 + texts.zip
- `G:/Projects/SR_GAME_CHINESE` —— 黑道圣徒，ASI + stb_truetype 现场字形 + le_string 替换
