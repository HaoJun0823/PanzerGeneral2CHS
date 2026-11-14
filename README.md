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
到 **Releases** 下载维护者手动上传的汉化补丁包 zip，**解压到游戏根目录**
（exe 所在处），覆盖同名文件。安装前请备份存档与原始文件。

> ⚠️ 安装步骤与细节以包内 `必读说明.txt` 为准。当前为**全量汉化**：
> GUI / 菜单 / 剧本简报 / 关卡按钮已中文化；地形、装备、姓氏等英文词条由显示层
> 译名表 `ExtraTerms.txt` 在绘制时替换，磁盘数据文件保持英文，以兼容地图匹配、
> 索引与存档。

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

CI 工作流保留在 `.github/workflows/build-release.yml`，但**当前不启用自动构建**：
仅支持在 Actions 页手动 `workflow_dispatch` 触发。发布包由维护者本地执行
`Tools/ci/build_release.py` 装配后，**手动上传到 Release**。

---

## 两个必须知道的铁律

1. **`Resources/CHS/` 下的文本表是「行式位置索引」**：翻译时**保持行数与顺序不变**，
   逐行替换。任一行的增删都会导致游戏按行号取文本时整体错位。
2. **CJK 字形走「GDI TTF → 8 位帧缓冲」合成（已确认并实现 MVP）**：游戏原生位图字体
   不含中文字形，纯替换 `.txt` 文本会出现「方块/空白」。渲染路径已用 IDA 锁定——
   游戏内文字是 8 位调色板帧缓冲的逐像素写入，本补丁在 hook 点用 GDI `GetGlyphOutline`
   取中文字形、经游戏颜色表写进帧缓冲（见 `research_handoff.md` §2 / §3.1）。
   直接使用系统 CJK 字体（宋体 / 微软雅黑），无需额外字库文件。汉字字形按游戏
   8px 脏矩形做纵向压高（见 `research_handoff.md` §12.4），避免字底残留叠字。

---

## 翻译进度（Resources/CHS）

文本表是「行式位置索引」，翻译由 `Tools/translate/gen_chs_text.py` 生成（源码 UTF-8、
写出 GBK，严格对齐行数，保留末尾 `0x1A` 控制符）。已翻译文本经 `sub_43B2C2` 渲染为
中文；纯 ASCII 专有名词走原渲染器，不影响显示。

| 文件 | 行数 / 条数 | 内容 | 状态 |
|------|------|------|------|
| `GUI97.TXT` | 440 行 | 菜单 / 界面 / 热键 / 天气 / 单位类别 | ✅ 全译 |
| `MISC.TXT` | 27 行 | 地形类型 / 战斗术语 | ✅ 全译 |
| `SCENARIO/*.TXT` | 306 个 | 剧本标题 + 简报长文本 | ✅ 全译（换行 / 分页已适配） |
| `scenario_zh.txt` / `scenario_keys.txt` | 322 行 | 简报译文与标题键 | ✅ 校订 |
| `ExtraTerms.txt` | 1455 条 | 地形 / 城市 / 河流 / 装备 / 姓氏 / 日期 | ✅ 显示层替换 |
| `FragTitleFix.txt` | 2 条 | 关卡列表按钮首字节碎片校订 | ✅ |
| `NAMES.TXT` / `EQUIP97.TXT` | 400 / ~170 行 | 姓氏与装备型号 | ⏸ 磁盘保持英文，走显示层替换 |

> `NAMES.TXT` / `EQUIP97.TXT` 及剧本**数据行**刻意保留英文，避免破坏地图匹配、
> 索引与存档；显示层替换只影响绘制，不写回文件。`Tools/translate/` 是各译文表的生成脚本。

---

## 目录导航

| 目录 / 文件 | 说明 |
|------------|------|
| `Documents/research_handoff.md` | ★ 完整技术文档（先读这个） |
| `PG2CHS_DLL/` | ASI/DLL 汉化插件源码（VS2017 v141，Win32+x64，Win10 SDK 26100） |
| `Resources/ENG/` | 游戏原始英文文本基线（翻译源，行式索引） |
| `Resources/CHS/` | 中文译文（全量；严格保持行数/顺序与原始字节长度） |
| `Tools/translate/` | 译文生成脚本：`gen_chs_text.py` / `gen_scenario_chs.py` / `gen_extra_terms.py` |
| `Fonts/` | 可选自包含 CJK 字体（当前直接使用系统字体：宋体 / 微软雅黑） |
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
