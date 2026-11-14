# 装甲元帅 2（Panzer General 2）汉化 · 技术交接文档

> 本文件是「初步调研」的固化记录，供后续用 IDA Pro + ida-pro-mcp 深入逆向时对接。
> 调研对象：**`PG2UK220.EXE`**（GOG 增强兼容主程序，834,528 字节，PE32，
> 时间戳 2010-06-02 即 GOG 重打包；**不碰 `PANZER2.EXE`**）。
> 游戏本体内部代号 `pz97`（Panzer General 97），VC++ 构建。

---

## 0. 范围与边界

- **目标程序**：仅 `PG2UK220.EXE`（GOG 主程序）。所有汉化改动围绕它展开。
- **可动用手段**：允许用 IDA Pro + `ida-pro-mcp` 逆向字符串交叉引用、渲染逻辑。
- **不预设立场**：汉化可改 EXE，也可改伴随文件（外部 `.txt` 文本表），视渲染路径而定。

---

## 1. 已确认的架构事实（均有证据）

### 1.1 程序本体 / PE 结构
- `PG2UK220.EXE`，PE32，6 个节：`.text` / `.rdata` / `.data` / `.idata` / `.rsrc` / `.reloc`。
- 二进制里泄露源路径：`C:\pz97\main\Src\Text.c`、`C:\pz97\main\Src\Loadmap.c` → 内部代号 `pz97`，VC++ 构建。

### 1.2 导入表（判断可行性的关键）—— 同时导入两套文字能力
- **`GDI32.dll`**：`CreateFontA`、`TextOutA`、`GetTextExtentPoint32A`、`SetTextColor`、`SetBkColor`、`SetBkMode`
- **`USER32.dll`**：`LoadStringA`、`DialogBoxParamA`、`CreateDialogParamA`、`SetDlgItemTextA`、`GetDlgItemTextA`、`MessageBoxA`
- **`KERNEL32.dll`**：`GetPrivateProfileStringA`、`MultiByteToWideChar` / `WideCharToMultiByte`、`LCMapStringA/W`、`CompareStringA/W`

> 含义：游戏同时具备「GDI 文本渲染」与「对话框/菜单/字符串表 API」能力。
> **但静态无法确定游戏内文字到底走哪条路**（见 §3 开放问题）。

### 1.3 资源段 `.rsrc` 几乎是空的
- 只有一个 Icon（0x2E8）。**无字符串表、无对话框模板、无菜单资源**。
- 结论：PG2 的界面文字**不在 Win32 资源里**，全部是内联 C 字符串或外部文件。

### 1.4 文字的两大来源
1. **外部行式 `.txt`（纯 ASCII、按行索引、无 `key=value`）**：
   - `GUI97.TXT`（440 行：国家名 / 胜利类型 / UI 文本如 "Next Unit"、"Cancel Move"）
   - `NAMES.TXT`（400 行：指挥官人名，Ackenheil、Adler…）
   - `EQUIP97.TXT`（586 行：装备/单位名，StuG IIIG、Pz IVD、Me-109f…）
   - `MISC.TXT`（27 行：地形类型 + 少量 UI 碎片）
   - `BLANK.TXT`（空文件）
   - `SCENARIO/*.TXT`（**306 个**场景文本）
   - 仓库基线已存入 `Resources/ENG/`（顶层 5 个 + `SCENARIO/` 306 个），`Resources/CHS/` 为待翻译工作副本（初始=英文）。
2. **EXE 内联 C 串**：菜单、对话框项、错误信息、格式化串（如 `"Player %d"`、`"SaveScenario : Unable to save scenario strings."`）。

### 1.5 自带位图字体（游戏内文字字形来源）
- 二进制引用：`fontpg.dat` / `fontfra.dat` / `fontgrm.dat`。
- 磁盘上「缺失」——它们**打包在 13 MB 的 `PANZER2.DAT` 档案里**（SSI 打包格式，与 `BIGFLAGS.SHP`/`BR_01.SHP` 同类）。
- `fontpg`=默认(英)，`fontfra`=法语，`fontgrm`=德语。

### 1.6 原版就带多语言框架（好消息）
- 紧挨 `Text.c` 出现：`fontpg.dat fontfra.dat fontgrm.dat … .deu .fra .txt %s%s`
- 说明游戏用**语言后缀拼文件名**加载文本/字体 —— 原版已支持德(`.deu`)/法(`.fra`)。
- `pz2.int`（40 字节）= 一组 16-bit 小端整数（127/100/1,1…），**不像语言码**（疑似视频/难度配置），非翻译载体。

---

## 2. 汉化路线（分阶段，按依赖排序）

### 阶段 0 · 文本翻译（最简单，基本不用动程序）
- 把 `Resources/ENG` 下的 `.txt` **按行替换**成中文，保持行数/顺序（位置索引）。
- 编码：若走 TTF 重定向 → GBK(936) 或 UTF-8；若保留位图字体 → 不可行，须换路线。

### 阶段 1 · 字体渲染重定向（核心，决定成败）
- 在 `PG2UK220.EXE` 里定位 `Text.c` 的 `fontpg.dat` 字形表 + 逐字符 blit 函数，
  把它**重定向到 GDI TTF**（如宋体/SimSun），宽度用已导入的 `GetTextExtentPoint32A` 计算以保持排版。
- 此步完成后，上面所有 `.txt` 中文即可显示。
- 备选（更轻）：烘焙一套 CJK 位图字体，替换 `fontpg.dat`（需拆/回包 `PANZER2.DAT`）。

### 阶段 2 · EXE 内联串（需 IDA + 补丁）
- 菜单/对话框/错误信息（`SetDlgItemTextA`/`SetWindowTextA`/`MessageBoxA`）替换成中文；
  因中文更长且多字节，需把串重定位到新字符串池。
- 按需把中文语言码（如 `.chs`）加进后缀表。

### 阶段 3 · 验证
- 排版（CJK 字更宽，控件/定位可能溢出，需调间距）、代码页、确认不再强依赖 `PANZER2.DAT` 里的位图字体。

---

## 3. 当前最大卡点（必须用 IDA 解开）

**游戏内文字到底走「位图字体 blit」还是已走「GDI TextOutA」？**
两者导入都在（§1.2），无法从静态字符串断定。这点决定项目是
**1 天（已 GDI：换字体名 + 代码页即可）** 还是 **1 个月（位图字体：重定向 blit / 烘焙 CJK 字库）**。

### 用 IDA + ida-pro-mcp 优先确认的清单
1. `CreateFontA` 与 `fontpg.dat` 的交叉引用 → 定渲染路径。
2. 字形绘制函数地址（blit 或 TextOut 调用点）→ 重定向点。
3. 内联字符串的引用清单 → 补丁清单。
4. **代理 DLL 目标**：若走 ASI 注入，须确认 `PG2UK220.EXE` 实际加载的、可被代理的系统 DLL
   （winmm / dsound / 其它）—— 当前 `dist/common/winmm.dll` 是按参考项目默认放置的占位，
   **尚未验证 PG2 是否真的 import winmm**。Ultimate ASI Loader 默认加载 `scripts\*.asi`。

---

## 4. 已搭好的工程骨架（本仓库）

```
PanzerGeneral2CHS/
├─ LICENSE                      HJNCSA-Core-1.0（与参考项目一致）
├─ LICENSES/                    ASILoader / Apache-2.0 / MinHook / stb / 思源
├─ README.md                    标准免责声明 + 本档案摘要 + 目录导航
├─ .github/workflows/build-release.yml   资源打包 CI（build + publish 两段）
├─ .gitignore / .gitattributes  资产 -text（防换行符损坏）
├─ Documents/research_handoff.md         ← 本文件
├─ PG2CHS_DLL/                  ASI/DLL 源码（VS2017 v141, Win32+x64, Win10 SDK 26100）
│                                  ★ 用户已建，dllmain.cpp 目前为空桩
├─ Resources/
│  ├─ ENG/                      游戏原始英文文本基线（翻译源）
│  └─ CHS/                      中文翻译工作副本（初始=英文，待译）
├─ Fonts/                       CJK 字体占位（具体字体待定，见 §3）
├─ dist/                        构建输入源：dist/common/winmm.dll(ASI Loader) + 必读说明.txt
├─ Tools/ci/                    build_release.py（装配 zip）/ verify_release.py
└─ build_out/                   构建产物输出（gitignore）
```

---

## 5. 参考项目（同作者，可复用其模式）
- `G:/Projects/ArmyMenCHS` —— 多代玩具兵大战，ASI/内存补丁，三段式 CI。
- `G:/Projects/MajestyIIExtend` —— Majesty2，ASI + 字体烘焙(DDS) + texts.zip，msvc-wine CI。
- `G:/Projects/SR_GAME_CHINESE` —— 黑道圣徒，ASI + stb_truetype 现场生成字形 + le_string 替换。

> 本仓库的 LICENSE / 工作流 / 资源打包 CI 思路即取自上述三者。
