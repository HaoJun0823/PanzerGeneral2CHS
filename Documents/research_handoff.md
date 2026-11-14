# 装甲元帅 2（Panzer General 2）汉化 · 技术交接文档

> 本文件是调研记录，供后续用 IDA Pro + `ida-pro-mcp` 深入逆向 / 编写 ASI 补丁时对接。
> 调研对象：**`PG2UK220.EXE`**（GOG 增强兼容主程序，834,528 字节，PE32，
> 时间戳 2010-06-02 即 GOG 重打包；**不碰 `PANZER2.EXE`**）。
> 游戏本体内部代号 `pz97`（Panzer General 97），VC++ 构建。
>
> **最近更新**：2026-11-14 用 IDA Pro + ida-pro-mcp 实逆向，§3 的全部开放问题已解答，
> 渲染路径与 hook 点已精确锁定（见 §3 / §6）。

---

## 0. 范围与边界

- **目标程序**：仅 `PG2UK220.EXE`（GOG 主程序）。
- **可动用手段**：IDA Pro + `ida-pro-mcp`（已验证可连，会话 `210d14e8`，Hex-Rays 就绪）。
- **汉化总策略**：ASI/DLL 注入（`winmm.dll` 代理，已确认游戏真 import winmm，见 §1.2）。
- **已搭工程**：`G:/Projects/PanzerGeneral2CHS`（LICENSE / CI / 资源打包 同作者三项目模式）。

---

## 1. 已确认的架构事实（均有 IDA 证据）

### 1.1 程序本体 / PE 结构
- `PG2UK220.EXE`，PE32，6 个节：`.text` / `.rdata` / `.data` / `.idata` / `.rsrc` / `.reloc`。
- 二进制泄露源路径：`C:\pz97\main\Src\Text.c`、`C:\pz97\main\Src\Loadmap.c` → 内部代号 `pz97`。

### 1.2 导入表（IDA 实读，已确认代理 DLL 目标）
游戏确实 import 了 **`WINMM`**（11 个函数），这是 ASI 代理方案成立的关键：
`timeGetTime`、`timeSetEvent`、`timeKillEvent`、`timeBeginPeriod`、`timeGetDevCaps`、
`mciSendCommandA`、`mciGetErrorStringA`、`auxGetDevCapsA`、`auxGetVolume`、`auxSetVolume`、`auxGetNumDevs`。
→ **`dist/common/winmm.dll`（Ultimate ASI Loader）会被游戏加载**，注入链路成立。

其它相关模块（节选）：
- `GDI32`：`CreateFontA`、**`TextOutA`**、`GetTextExtentPoint32A`、`SetTextColor`、`SetBkColor`、`SetBkMode`、`GetDeviceCaps`、`CreateSolidBrush`、`DeleteObject`、`GetStockObject`
- `USER32`：`LoadStringA`、`DialogBoxParamA`、`CreateDialogParamA`、`SetDlgItemTextA`、`GetDlgItemTextA`、`MessageBoxA`、`SetWindowTextA`、`GetWindowTextA`、`SendMessageA`、`wsprintfA`
- `KERNEL32`：`GetPrivateProfileStringA`、`MultiByteToWideChar`/`WideCharToMultiByte`、`LCMapStringA/W`、`CompareStringA/W`、`FindResourceA`/`LoadResource`/`LockResource`
- `DDRAW`（`DirectDrawCreate`）、`DPLAYX`、`SMACKW32`（Smacker 视频）、`WSOCK32`、`CLUBDLL`、`MSS32`（Miles 音频）、`COMDLG32`、`OLE32`

### 1.3 资源段 `.rsrc` 几乎是空的
- 仅一个 Icon。**无字符串表 / 对话框模板 / 菜单资源** → 界面文字全在内联 C 串或外部 `.txt`。

### 1.4 文字的两大来源（翻译载体）
1. **外部行式 `.txt`（纯 ASCII、按行索引、无 `key=value`）**：
   `GUI97.TXT`(440) / `NAMES.TXT`(400) / `EQUIP97.TXT`(586) / `MISC.TXT`(27) / `BLANK.TXT`(空)
   + `SCENARIO/*.TXT`（**306** 个）。仓库基线已存 `Resources/ENG/`，`Resources/CHS/` 为待译工作副本。
2. **EXE 内联 C 串**：菜单 / 对话框项 / 错误信息 / 格式化串。

### 1.5 自带位图字体 + 两套并存的语言系统（IDA 实读）
- 字体加载器 **`sub_43B0BC` @ 0x43B0BC**（字符串 `fontpg.dat`@0x4A7394、`fontfra.dat`@0x4A73A0、`fontgrm.dat`@0x4A73AC）：
  ```c
  if ( Locale == 9 )        wsprintfA(&Name,"fontpg.dat"); // 英语
  else if ( Locale == 12 )  wsprintfA(&Name,"fontfra.dat");// 法语
  else                     wsprintfA(&Name,"fontgrm.dat");// 德语（默认）
  dword_538958 = sub_481710(&Name,&v1,0);  // 加载位图字体文件
  sub_43B17F(dword_538958);                // 设为当前字体
  ```
  - `Locale` 全局 @ 0x4AD758（9=英 / 12=法 / 默认=德）。
  - `sub_43B17F` @ 0x43B17F：置 `dword_53895C = a1`（当前字体结构），调 `sub_43B9AF(227)`。
- **第二套 i18n（Win32 对话框/菜单路径）**：`sub_4164DE` @ 0x4164DE 读
  `GetPrivateProfileStringA("language","type",...)` → `dword_4A0FD8`（0=英/1=法/2=德/3=西），
  并 `LoadLibraryA("%s\\%s")` 加载**语言 DLL**（`english.dll` 等），同时
  `CreateFontA(-16,0,0,0,0,0,0,0,0x10,0,0,2,2,"Arial")` 建 GDI Arial 字体。
  → 说明存在「语言 DLL」扩展点，可作为对话框/菜单中文的另一杠杆。

### 1.6 原版多语言框架
- `fontpg/fontfra/fontgrm` + `.deu/.fra` + `%s%s` 拼文件名 → 原版已支持德/法。
- `pz2.int`（40 字节，16-bit 整数 127/100/1,1…）**不像语言码**（疑似视频/难度），非翻译载体。

---

## 2. 渲染管线（IDA 实读，已完全厘清）

```
调用者(37处) ──► sub_43B19F(x,y,surface,Format,va)   @0x43B19F  ← ★主文字API(printf式)
                    │ vsprintf(Format,va) → Source(0x538960)
                    ▼
                 sub_43B2C2(x,y,color_table=&byte_538F40,Str,surface)  @0x43B2C2  ← ★逐字符 blit
                    │ 遍历 Str：换行(y+=*(WORD*)(*(DWORD*)0x53895C+8)+2) / 否则
                    ▼   调 sub_428813() 画单字，x += sub_4287F3()(字宽)
                 sub_428813(surface,x,y,font,ch,color_table)  @0x428813  ← 直接像素写帧缓冲(NOT GDI)
```

**关键事实（2026-11-14 汇编逐条核对 + 崩溃转储验证）：**
- **游戏内文字不走 GDI，而是直接写进一个软件帧缓冲。** `sub_428813` 的核心：
  ```c
  surf = a1;                              // ★绘图面 = sub_43B2C2 第 5 参（blit arg1）
  H    = *(void**)surf;                   // surf[0] = H 头
  pix  = *(BYTE**)H;                      // H[0] = 像素基址
  pit  = *(int*)(H+4) + 1;                // H[1] = pitch-1 → pitch
  clX  = surf[1]; clY = surf[2];          // 裁剪原点（绘制时加到 x/y）
  maxX = surf[3]; maxY = surf[4];         // 右下边界
  dst  = pix + pit*(clY + gy) + (clX + gx);// 目标像素地址
  // 启用 xlat(a7=colorTable)：_AL=src; xlat; if(_AL!=-1) *dst=_AL;   ← 单字节写入！
  ```
  → 帧缓冲确认为 **8 位调色板**表面（每像素 1 字节 = 调色板索引）。
- **绘图面 = `sub_43B2C2` 的第 5 个参数 `surface`**（即 `sub_428813` 的 arg1）。早期「第 5 参未使用、
  绘图面取自全局」的结论**有误**，已由崩溃转储 + blit 反汇编纠正。`0x53895C` 不是绘图面。
- **`dword_53895C`（0x53895C）存的是「当前字体对象」指针**（`sub_43B17F` 置入）：
  `*(DWORD*)0x53895C + 8` = 行高（换行时 `y += 行高 + 2`）；`+0x10` = 字符偏移表
  （`0x4287F3` 用它按字符查字形/字宽）。
- `DrawSurface` 布局：`[0]`=H 头指针、`[1]`=clipX、`[2]`=clipY、`[3]`=maxX、`[4]`=maxY；
  `H[0]`=像素基址、`H[1]`=pitch-1、`H[2]`=height-1。
- **`0x4287F3` 字符查表以「有符号」字符为索引**：调用方 `movsx` 后 GBK 汉字字节 0x81~0xFE
  变负数 → 查表越界 → 崩溃。这是中文首次真机崩溃的根因（详见 §3.3）。
- **颜色表 `byte_538F40` 的真实语义（2026-11-14 第二轮实证，纠正早期「灰度覆盖度表」的猜测）**：
  `sub_43B0BC` 初始化时 `memset(0x538F40, 0xFF, 0x100)`；全程序对该表的 36 处引用中
  **35 处是 `push` 传参、只有 1 处写入**——即 `sub_43B9AF(byte c)`（SetFontColor）里的
  `*(BYTE*)0x538F40 = c`。也就是说整张表**只有 `[0]` 是当前文字色，其余恒为 0xFF(透明)**。
  → 可见像素的调色板索引 = `colorTable[0]`（初值 `0xE3`）；合成 CJK 时直接写这个索引，
  不能用「覆盖率查表」（覆盖率 1..255 全部命中 0xFF → 全透明，画不出来）。
- **文字绘制必须成对调用表面锁**：`sub_43B2C2` 与 `sub_43B402` 都在开头 `call 0x45197D`
  (DirectDraw `Lock`，vtable+0x64)、结尾 `call 0x451A1F` (`Unlock`，vtable+0x80)，
  锁定标志在字节全局 `0x4AC6F0`。本机游戏用 **cnc-ddraw → d3d9** 模拟 DirectDraw，
  **Unlock 才真正提交像素**；自绘 CJK 必须自己成对调用，否则画了也看不见。
- **存在第二条绘制路径 `sub_43B402`（自动换行渲染器）**：它不经过 `sub_43B2C2`，
  对每个字节 `cmp 0x20 / isctype` 后直接调 blit `0x428813`（`0x43B691`、`0x43B7A9` 两处）。
  GBK 高位字节 `movsx` 成负数 → 被判为非可打印 → **跳过不画**（不崩，但文字消失）。
  剧本简报/帮助等长文本走这条路，是下一阶段要补挂的目标。
- **`sub_43B850` 文本尺寸**（居中/换行计算用）：返回 `宽` 与 `行数*10`；宽度按字累加
  `0x4287F3`，非可打印字节计 0 → GBK 文本的尺寸会偏小，居中会有偏移（可接受，后续再校）。

- `CreateFontA` 只在 `sub_4164DE` 出现 1 次（Arial，给语言 DLL/对话框用）；
  `TextOutA` 只在 `sub_44AF0D`（真实 Win32 对话框 `hDlg`，case 89–92）出现 1 次。
  → **GDI 对游戏内主文字是边缘路径，主文字=位图字体逐像素 blit。**

---

## 3. 开放问题 → 已全部解答（2026-11-14 IDA）

| 原问题 | 结论 | 证据 |
|---|---|---|
| 代理 DLL 目标（是否真 import winmm） | ✅ 是，winmm 被 import → `winmm.dll` 代理成立 | §1.2 |
| 游戏内文字走位图 blit 还是 GDI TextOutA | ✅ **位图字体逐像素写帧缓冲**，非 GDI | §2 / `sub_428813` |
| 字形绘制函数地址（重定向点） | ✅ `sub_43B19F`(主API,37调用) / `sub_43B2C2`(blit) / `sub_428813`(像素写) | §2 / §6 |
| 字体文件选择逻辑 | ✅ `Locale`(9/12/默认)→fontpg/fontfra/fontgrm | `sub_43B0BC` |
| 内联串 / 对话框中文 | 走 Win32 对话框(`hDlg`)+`SetDlgItemTextA`/`TextOutA`，另有语言 DLL(`english.dll`)机制 | `sub_44AF0D`/`sub_4164DE` |

**「1 天 vs 1 个月」的判定**：是**较难**那条——中文必须渲染进 8/16 位软件帧缓冲，
不能简单换字体名。但好消息是**完美收敛在单一函数 `sub_43B19F`/`sub_43B2C2`**，改动面极小。

### 3.1 推荐 hook 方案（已采用，见 `PG2CHS_DLL`）
- **首选 hook `sub_43B2C2`（@0x43B2C2）**：它已拿到最终串 `Str`、坐标 `x,y`、颜色表、surface，
  无需处理 `va_list`，是最「手术刀」级切入点。签名（cdecl，汇编压栈核对）：
  ```c
  int __cdecl sub_43B2C2(int x, int y, const unsigned char* colorTable, const char* text, void* surface);
  ```
  - 调用处压栈（右→左）：`surface`→`x`→`y`→`dword_53895C`→`c`→`colorTable`，`add esp,14h`(5×4)✓。
- **Hook 实现方式（MVP，已在 `PG2CHS_DLL/dllmain.cpp`）**：**不用 MinHook/NuGet**，纯 Win32
  **手动 detour + 跳板**（与参考项目 MajestyIIExtend 同思路，规避 msvc-wine 下 NuGet 还原的不确定性）。
  `sub_43B2C2` 经 IDA 确认 prologue（GOG `PG2UK220.EXE`）：
  ```
  0x55            push ebp        (1)
  8B EC           mov ebp, esp    (2)
  83 EC 10        sub esp, 10h    (3)
  53              push ebx        (1)   ← 下一条指令边界在偏移 6
  ```
  relocate 长度取 **6 字节**（恰好落在 `push ebx` 之前，避免把 3 字节的 `sub esp,10h` 拦腰切断）；
  跳板 = 原 6 字节 + `E9 rel32` 回跳到 `addr+6`（共 11 字节，`VirtualAlloc` RWX）。
  `g_origDrawText` = 跳板指针，供纯 ASCII 串零回归调用。DllMain 中先检查入口首字节 `0x55(push ebp)`
  作版本闸门——若 EXE prologue 不符（疑似版本不同）则放弃 hook，游戏仍以英文运行（安全降级）。
- **CJK 字形合成（已实现 MVP）**：
  对含 GBK 双字节（引导字节 `>=0x81`）的字符串，**整串自绘**：用 GDI
  `GetGlyphOutline(GGO_GRAY8_BITMAP)` 取字形覆盖掩码（自动归一化 0..255），
  经同一张 `colorTable`(xlat) 把覆盖值→调色板索引，写进 8 位帧缓冲（用 §2 面结构）。
  ASCII 与 CJK 均用 GDI 同字体渲染（保持一致）；步进用 `gm.gmCellIncX`。
- **纯 ASCII 字符串**：直接调原函数（蹦床），零回归。
- **部署形态**：产物 `PG2CHS_DLL.dll` 改名 **`PG2CHS_DLL.asi`** 放游戏根目录，由 `winmm.dll`
  （Ultimate ASI Loader）自动加载（ASI Loader 默认加载同目录 `.asi`）。

### 3.2 运行时地址换算（ASI 内）
```c
HMODULE hMod = GetModuleHandleA("PG2UK220.EXE");
// ImageBase = 0x400000；偏移 = VA - 0x400000
void* pDrawText = (BYTE*)hMod + (0x43B2C2 - 0x400000);   // hook 入口
// 绘图面：直接用 sub_43B2C2 的第 5 个参数 surface（不要从 0x53895C 取！）
DWORD fontObj   = *(DWORD*)((BYTE*)hMod + (0x53895C - 0x400000)); // 当前字体对象（+8 行高）
```
```c
struct DrawSurface { void* H; int clipX, clipY, maxX, maxY; };   // = sub_43B2C2 第 5 参 surface
struct SurfaceHeader { void* pixels; int pitchMinus1, heightMinus1; }; // *H
// 像素地址 = H->pixels + (H->pitchMinus1+1)*(clipY+gy) + (clipX+gx)，写 1 字节调色板索引
```

### 3.3 首次真机崩溃与修复（2026-11-14，转储实证）
- **崩溃**：`0xC0000005` 读访问 @ `0x42880B`（在 `sub_4287F3` 内），`ESI=0x1D224D08` 非法。
  调用栈：`0x42880B ← 0x43B82D（sub_43B7C4 体内）← 0x453052 ← 0x467583 ← … ← user32 对话框`。
- **根因**：`sub_43B7C4`（测宽）逐字节 `movsx`（有符号）后调 `0x4287F3` 查字宽表；
  `0x4287F3` 执行 `*(int*)(fontObj + table[ch])`，其中 `table = fontObj+0x10`。
  GBK 汉字字节 0x81~0xFE 被符号扩展为负数 → 索引落在表之前 → 取到垃圾偏移 →
  `fontObj + 垃圾` 为非法地址 → 读崩。ASCII 0~127 不触发，故英文从不崩。
- **修复**（`PG2CHS_DLL/dllmain.cpp`，3 点手动 detour）：
  1. `0x43B2C2` 绘制（原有）：含 GBK → GDI 取字形写帧缓冲。
  2. `0x43B7C4` 测宽：含 GBK → 按 GBK 字符累加 `gmCellIncX`，不再逐字节负索引。
  3. `0x4287F3` 字宽查表兜底：高位字节返回半宽（2 字节 = 全宽），防其它调用点负索引崩。
  - 同时修正绘图面取址为「第 5 参 surface」，并返回换行计数匹配原函数语义。
- **经验**：真机日志 `hMod=72750000` 是 ASI 自身模块句柄（正常）；hook 点在 `@0043B2C2` 正确。
  `0x53895C` 是字体对象指针，**不是**绘图面——这是早期结论的主要坑。

### 3.4 第二次真机崩溃：三个隐藏 bug（同一 dump 地址复现）

`PG2UK220.EXE.111028.dmp` 与首轮**完全相同的崩溃点**（`EIP=0x42880B`, `ESI=0x1D222865`），
而日志显示三个 hook 都已装上 → 说明钩子根本没生效。逐项查出三个独立缺陷：

1. **detour 入口跳错目标（致命）**：`InstallDetour` 把入口写成 `jmp 跳板`，而跳板内容是
   「原 prologue + 跳回原函数」，等价于**什么都没拦**，钩子函数一次都没被调用。
   转储证据：`0x4287F3` 处字节为 `e9 08 78 cd 00` → `jmp 0x01100000`，正是日志里的
   `charwidth=01100000`（跳板地址）。**正确做法：入口 `jmp 钩子函数`，跳板只作原函数指针。**
2. **字形颜色映射错误**：见 §2「颜色表真实语义」。早期把 GDI 覆盖率 0..255 当索引查表，
   1..255 全部落到 0xFF → 全透明，画不出任何字。
3. **漏了表面锁**：见 §2。cnc-ddraw 下不 `Unlock` 等于没提交像素。

另加一道**兜底闸门**：挂钩 blit `0x428813`，字形索引为负（GBK 高位字节被 `movsx` 后的必然结果）
时直接跳过，避免任何未预见路径用 `fontObj + 0x10 + idx*4` 反向越界读到垃圾指针再崩。

**教训**：detour 类 bug 不报错、不崩溃，只会「静默失效」；验证必须取证到
**目标地址处的实际字节**（转储/内存读），不能只看日志说「装上成功」。

### 3.5 第三次真机：不崩但「一个字都不显示」

`PG2CHS.log` 显示 4 个 detour 全部 `OK`（入口指向 ASI 模块内 `72751680`，跳板 `02740000`），
`draw CJK #0 (5,5) ... ink=E3 text=D5 BD B6 B7`（=「战略」）也在正常触发，但屏幕上没有中文。
逐项复现出三个独立原因，全部会导致「零像素」：

1. **覆盖率值域搞错（主因）**：`GGO_GRAY8_BITMAP` 的像素值域是 **0..64**，不是 0..255。
   阈值 `0x60=96` 直接拿去比 0..64 永不成立 → 一个像素都不写。
   → 改为 `cov*255/64` 归一化后再比阈值。
2. **字体被取消选中（同样致命）**：`EnsureFont` 验证完字体后把原字体 `SelectObject` 选回去了，
   于是后续所有 `GetGlyphOutlineW` 实际用的是**默认系统字体**（无 CJK 字形）→ 返回 0。
   → 改为字体**常驻 DC**。
3. **字形原点算错**：`gmptGlyphOrigin.x` 用了减号（应为加），纵向也没按基线对齐
   （`gmptGlyphOrigin.y` 是相对基线向上的偏移）。→ 改为 `left = penX + origin.x`、
   `top = baseline - origin.y`，`baseline = 文本行顶 + tmAscent`。

**字号结论（实测系统 GDI）**：游戏自带位图字体 `fontObj+8 = 8`（字形高 8px，行距 8+2=10），
8px 画汉字是一团糊。用同一套算法离线渲染对比 10/11/12/13/14/16px 后确认：
**12px 起清晰可辨**，且 12px 与 10px 行距只重叠 2px。字形格式改走
**`GGO_BITMAP`（1bpp）**——宋体 12px 用字体自带点阵，笔画完整、风格贴近原版；
`GGO_GRAY8` 在 12px 反而糊（可在 ini 用 `AntiAlias=1` 切回）。

> 离线对比工具：`Tools/dev/glyph_preview.py`（同算法 + 4 倍放大 PNG，可改字号/阈值/字体）。
> 注意 `MAT2` 的 `FIXED` 是 `{WORD fract; short value;}`：C 里写 `{{0,1},...}` 是按字段初始化
> （=1.0），但用**字节数组**手工填时要写成 `00 00 01 00`，否则缩放成 0.004（字形缩成 2×2）。

---

## 4. 汉化路线（按依赖排序，已据 IDA 修正）

### 阶段 0 · 文本翻译（不动程序）
- `Resources/ENG` 的 `.txt` 按行替换成中文（保持行数/顺序）。编码用 **GBK(936)**（与游戏 ANSI/DBCS 路径一致）。

### 阶段 1 · 帧缓冲字体重定向（核心，决定成败）
- 在 ASI(DLL) 里 hook `sub_43B2C2`/`sub_43B19F`，对 CJK 走「GDI 取字形 → 写帧缓冲」合成（§3.1）。
- 字体选型：`Fonts/` 放 **SimSun(宋体) / Microsoft YaHei(微软雅黑)**，GDI `CreateFont` 时指定。
- 完成后 `Resources/CHS` 全部中文即可显示。

### 阶段 2 · EXE 内联串 / 对话框中文
- Win32 对话框路径（`sub_44AF0D` 的 `hDlg` + `SetDlgItemTextA`/`TextOutA`）及语言 DLL 机制
  （`english.dll` → 可加 `chinese.dll` 或 hook `LoadStringA`）→ 菜单/对话框/错误信息中文。
- 内联 C 串：重定位到新字符串池 + 重定向引用（需 IDA 列 xref 清单）。

### 阶段 3 · 验证
- 排版（CJK 更宽 → 控件/定位可能溢出，需调间距）、GBK 代码页、确认不再强依赖 `PANZER2.DAT` 里的位图字体。

---

## 5. 已搭好的工程骨架（本仓库）
```
PanzerGeneral2CHS/
├─ LICENSE / LICENSES/          HJNCSA-Core-1.0 + ASILoader/Apache-2.0/思源（MinHook、stb 许可备用，当前未用）
├─ README.md                    标准免责声明 + 本档案摘要
├─ .github/workflows/build-release.yml   资源打包 CI（build + publish 两段，msvc-wine v141 x86）
├─ Documents/research_handoff.md         ← 本文件
├─ PG2CHS_DLL/                  ASI/DLL 源码（VS2017 v141, Win32+x64）—— dllmain.cpp 已实现 MVP（手动 detour + CJK 字形合成）
├─ Resources/{ENG,CHS}/         英文基线 / 中文工作副本（311 文件，已入库）
├─ Fonts/                       CJK 字体占位（SimSun/YaHei，待放入）
├─ dist/common/                 winmm.dll(ASI Loader, MIT) + 必读说明.txt
├─ Tools/ci/                    build_release.py / verify_release.py / build_dll_local.py
└─ build_out/                   构建产物（gitignore）
```

---

## 6. IDA 符号地址速查表（PG2UK220.EXE，ImageBase=0x400000）

| 符号 | 地址 | 作用 |
|---|---|---|
| `sub_43B0BC` | 0x43B0BC | 字体加载器（选 fontpg/fontfra/fontgrm） |
| `sub_43B17F` | 0x43B17F | 置当前字体 `dword_53895C` |
| `sub_481710` | 0x481710 | 文件打开/加载（字体文件） |
| `sub_43B19F` | 0x43B19F | **主文字 API（printf 式，37 调用者）** |
| `sub_43B2C2` | 0x43B2C2 | **逐字符 blit（★推荐 hook 点）** |
| `sub_43B402` | 0x43B402 | **自动换行渲染器**（不经 `sub_43B2C2`，直接调 blit；11 个调用者） |
| `sub_43B850` | 0x43B850 | 文本尺寸（返回宽、高=行数*10），用于居中/换行 |
| `sub_45197D` | 0x45197D | **表面 Lock**（DirectDraw vtable+0x64，置锁定标志） |
| `sub_451A1F` | 0x451A1F | **表面 Unlock**（vtable+0x80，cnc-ddraw 下真正提交像素） |
| `byte_4AC6F0` | 0x4AC6F0 | 表面是否已锁定（byte） |
| `sub_428813` | 0x428813 | **单字像素写帧缓冲（NOT GDI）** |
| `sub_4287F3` | 0x4287F3 | 取单字宽度 |
| `sub_43B9AF` | 0x43B9AF | 设字体/颜色状态（参数 227/1/6） |
| `sub_44AF0D` | 0x44AF0D | 界面分项渲染（仅 case89–92 用 GDI TextOutA 于 hDlg） |
| `sub_4164DE` | 0x4164DE | 初始化+读 language.ini+LoadLibrary 语言 DLL+CreateFont(Arial) |
| `sub_413370` | 0x413370 | **析构/清理**（释放 DDraw 面 + free 字体）—— 非渲染器 |
| `Locale`     | 0x4AD758 | 语言全局（9=英/12=法/默认=德） |
| `dword_4A0FD8` | 0x4A0FD8 | 语言码（0=英/1=法/2=德/3=西，来自 language.ini） |
| `dword_538958` | 0x538958 | 已加载位图字体句柄 |
| `dword_53895C` | 0x53895C | **当前字体对象指针**（`+8`=字形高度（本作 8px，行距 +2）、`+0x10`=字形偏移表）；绘图面是 `sub_43B2C2` 第 5 参，**不是**它 |
| `dword_5155B0` | 0x5155B0 | **表面头**（`Lock` 填充）：`{ lpSurface, lPitch-1, dwHeight-1 }`；只在锁定窗口内有效 |
| `byte_538F40` | 0x538F40 | **文字颜色表**（256 字节；初始化 memset 0xFF，仅 `[0]`=当前文字色，初值 0xE3） |
| `Source`     | 0x538960 | vsprintf 输出缓冲（最终文本串） |
| `fontpg.dat` | 0x4A7394 | 英文字体文件名串 |
| `fontfra.dat`| 0x4A73A0 | 法文字体文件名串 |
| `fontgrm.dat`| 0x4A73AC | 德文字体文件名串 |

> 注：`sub_413370` 早期曾被误判为渲染器，实为析构函数（含 `free(dword_538958)`）。真正渲染入口是 `sub_43B19F`。

---

## 7. 参考项目（同作者，可复用其模式）
- `G:/Projects/ArmyMenCHS` —— 多代玩具兵大战，ASI/内存补丁，三段式 CI。
- `G:/Projects/MajestyIIExtend` —— Majesty2，ASI + 字体烘焙(DDS) + texts.zip，msvc-wine CI。
- `G:/Projects/SR_GAME_CHINESE` —— 黑道圣徒，ASI + `stb_truetype` 现场生成字形 + `le_string` 替换。
  （SR 项目的「GDI/ttf 取字形 → 写缓冲」思路与本作 §3.1 最接近，可重点参考其 DLL 实现。）

> 本仓库的 LICENSE / 工作流 / 资源打包 CI 思路取自上述三者；CJK 字形合成可借鉴 SR 的 stb_truetype 路线。
