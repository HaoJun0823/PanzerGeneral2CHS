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
- **已搭工程**：`本仓库根目录`（LICENSE / CI / 资源打包 同作者三项目模式）。

---

## 1. 已确认的架构事实（均有 IDA 证据）

### 1.1 程序本体 / PE 结构
- `PG2UK220.EXE`，PE32，6 个节：`.text` / `.rdata` / `.data` / `.idata` / `.rsrc` / `.reloc`。
- 二进制泄露源路径：`main/Src/Text.c`、`main/Src/Loadmap.c`（原工程相对路径） → 内部代号 `pz97`。

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
`draw CJK #0 (5,5) ... ink=E3 text=D5 BD B6 B7`（=「战斗」）也在正常触发，但屏幕上没有中文。
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

### 3.6 汉字能显示后「错字 / 按钮串词 / 只出一个字」：文本表按原始字节偏移访问（2026-11-15 实机确认）

**现象**：中文正常显示、大小位置也对，但内容错乱——多个按钮只出一个字（如「望」），
按钮显示成别的按钮的文案（如本该是别的却出现「取消」），标题画面本该是「装甲元帅 II」却显示「战斗」。

**证据链（逐步排除渲染层）**：

1. 日志 `text=D5 BD B6 B7`（=「战斗」）。但译文里「战斗」只作为
   「选择战斗机」(97 行) / 「战斗机」(144 行) / 「战斗支援」(281 行) 的**一部分**出现，从不独立成词。
   → 说明取到的不是整行，而是**行中间的片段**。
2. `VirtualQuery(text)`：`p=0x00538960`、`type=IMAGE`、落在 EXE `.data` 段，
   而静态文件里该区域**全为 0** → 它是运行时的全局缓冲区；`0x538960` 紧邻字体对象指针 `0x53895C`。
3. `xrefs_to 0x538960` 全部落在 `0x43B1xx~0x43C9xx` 文本模块。反汇编定位到
   **`sub_43B19F`**：`vsprintf(0x538960, format, …)` 之后调 `sub_43B2C2` 绘制。
4. 在日志里用 `_ReturnAddress()` 确认调用来源恒为 `0x43B20B`（`sub_43B19F` 内部），
   再扫描栈帧定位真正的 format：`fmt=0x03A982A0`（堆），内容正是 `D5 BD B6 B7`。
   该 format 来自全局 `dword_4F0598`，由 `0x469F09` 处
   `sub_43B19F(5, 5, surface, dword_4F0598)` 传入。

**根因**：游戏把文本表整体读进内存后，**按英文原文的字节偏移**逐行取字符串。
译文改 GBK 后每行变短（GUI97 7062B → 4697B），偏移从第一处长度差异起累积，
其后所有行都指向错误位置——于是取到相邻行的片段（「战斗」）、
或停在半个汉字上（「总」+ 0xC9）。这是**数据源问题，不是渲染问题**，
调字号/字体/坐标/颜色都无效。

**修复：译文必须与原文逐行等长。**

- 译文较短 → 行尾补半角空格，补齐到原文该行的字节数；
- 译文较长 → **必须改写缩短**（否则从其下一行起继续错位）；
- 全文件总字节必须与原文完全相同（GUI97 = 7062B、MISC = 257B），行数、行序、末尾 `\x1a` 均不变。

**验证**：补齐后同一处日志变为
`text=D7 B0 BC D7 D4 AA CB A7 20 49 49 20 20 20 20 20 20`
（=「装甲元帅 II」，17B，与原文 `Panzer General II` 等长），写入像素 79 → 222，
标题画面实机确认显示「装甲元帅 II」。

**固化（防止以后再退化）**：

- `Tools/translate/gen_chs_text.py` 现在以 `Resources/ENG/*.TXT` 为基准，逐行补空格到等长，
  并断言产物总字节 == 原文；**译文超长直接抛错并列出具体行**，不再静默产出错位文件。
- `Tools/translate/pad_lines_to_orig.py` 可独立核对/补齐行长度。
- DLL 侧在**测宽与绘制时忽略行尾填充空格**，避免居中排版偏左（文件里的字节不受影响）。

### 3.7 全量汉化（2026-11-15）：剧本简报、译表工具链、专名表决策

**SCENARIO/ 下有两类文件，必须严格分开**（误译数据文件会直接弄坏场景）：

| 类别 | 数量 | 判据 | 处理 |
|---|---|---|---|
| 场景简报 | 252 | 含长英文句，或正文带 TAB 缩进 | 翻译 |
| 场景数据 | 54 | 城市名 / 部队代号 / 坐标（`4PLAYER` `ANTISAL` `CIECHAN`…） | **整文件原样复制，一个字节都不改** |

简报文件的固定结构：`标题 / 空行 / 日期 / 空行 / 描述（可多行） / 空行 / 玩家数 / 空行 / 回合数`。
全部 252 个文件共 737 行，去重后只有 **322 个唯一英文行**（大量战役胜负评语在不同剧本复用）。

**译表工具链**（`Tools/translate/gen_scenario_chs.py`）：

- `Resources/CHS/scenario_keys.txt` 由脚本从英文基准自动提取唯一行，按 `(-长度, 字典序)` 稳定排序；
  `scenario_zh.txt` 只放中文译文，**逐行与 keys 一一对应**。这样翻译时不必重复抄写 600 字符的长英文原句。
  `--emit-keys` 刷新键表并打印编号，`--report` 列出未译编号。
- 日期（`September 8, 1944`）、`2 Players`、`16 Turns` 等模板由 `auto_translate()` 自动转换，
  短月份（May/June/July 只有 12B）自动退化为 `1940.5.21` 点分格式以免超长。
- 输出 GBK，保持 **行数 / 行序 / CRLF / 行首 TAB / 末尾 0x1A** 全都不变；译文短于原文时行尾补半角空格。
  短地名音译（`马耳他` `苏奥穆斯萨尔米` `沃洛科拉姆斯克` 等 7 处）必然超长，保留完整译文、不补齐也不截断——
  剧本按「行」索引（§3.6 实机确认），不是按字节偏移，故无风险。
- 校验：306 个文件行数全部一致，仅上述 7 个文件因为完整音译多 1~3 字节。

**渲染路径**：长文本走 `sub_43B402` 自动换行渲染器，新增第 5 个 detour，
CJK 分支由 `WrapDrawCJK()` 复刻原换行语义（CJK 任意处可断、ASCII 整词度量、超出 maxLines 压缩行距）。
自检（用一段真实简报文本强制跑一遍）确认断行与行距正确。
三条渲染路径（绘制 / 测宽 / 换行）**统一忽略行尾填充空格**，否则几百个尾随空格会把简报撑到疯狂折行。

**`EQUIP97.TXT` / `NAMES.TXT` 决定保留英文**：

- `EQUIP97`（586 行）是装备型号与代号：`3.7 FlaK` / `Pz IVG` / `105mm` / `Me-109f` / `Strongpoint`，
  `NAMES`（400 行）是德军将领姓氏：`Ackenheil` / `Adler` / `Blucher`… 两者都是专名。
- 这类表极可能参与单位识别与存档数据，且大量条目只有 3~6 字节（中文必然超长）；
  音译 400 个人名收益低、风险高。按汉化惯例保留原文，以后若要单独做需先验证其渲染路径与存档兼容。

**部署**：游戏工作目录是 `update\`，而原版剧本在根 `SCENARIO\`；
为兼容两种相对路径解析，中文剧本**同时部署到 `SCENARIO\` 和 `update\SCENARIO\`**，
原版 306 个 TXT 已备份到 `PG2CHS_backup/SCENARIO_ENG/`。

**实机验证**：hook 五项全部 OK，主菜单 58 次绘制逐条解码为正确中文
（进行战役／进行场景／开始多人场景／邮件对战／查看最佳战绩／启动场景编辑器／载入存档／查看介绍／制作人员／退出游戏），
无新增 minidump，日志末尾为正常 `PG2CHS: 卸载`。

---

## 4. 汉化路线（按依赖排序，已据 IDA 修正）

### 阶段 0 · 文本翻译（不动程序）— **已完成，见 §3.7**
- `Resources/ENG` 的 `.txt` 按行替换成中文（保持行数/顺序）。编码用 **GBK(936)**（与游戏 ANSI/DBCS 路径一致）。
- 已完成：`GUI97.TXT`(440 行)、`MISC.TXT`(27 行)、`SCENARIO/` 252 个简报（322 个唯一译条）。
- 刻意保留英文：`EQUIP97.TXT`（装备型号）、`NAMES.TXT`（将领姓氏）、54 个场景数据文件。

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
- `ArmyMenCHS` —— 多代玩具兵大战，ASI/内存补丁，三段式 CI。
- `MajestyIIExtend` —— Majesty2，ASI + 字体烘焙(DDS) + texts.zip，msvc-wine CI。
- `SR_GAME_CHINESE` —— 黑道圣徒，ASI + `stb_truetype` 现场生成字形 + `le_string` 替换。
  （SR 项目的「GDI/ttf 取字形 → 写缓冲」思路与本作 §3.1 最接近，可重点参考其 DLL 实现。）

> 本仓库的 LICENSE / 工作流 / 资源打包 CI 思路取自上述三者；CJK 字形合成可借鉴 SR 的 stb_truetype 路线。

---

## 8. 按钮标签被按字节切碎：根因与精确还原（2026-11-15，IDA Pro MCP + 实机闭环）

### 8.1 调用链（IDA 反汇编 0x43B1A0–0x43B213）

```
sub_43B19F  PrintfText(int x, int y, void* surface, const char* fmt, ...)   ← 37 个调用者
  ├─ 0x43B1E1  _vsprintf(Source@0x538960, fmt, ArgList=&[ebp+18h])
  └─ 0x43B206  call sub_43B2C2 (DrawText)   → 返回地址是 0x43B20B，不是 0x43B206
sub_43B213  居中版：先 vsprintf + sub_43B850 测宽，再按矩形居中调 DrawText
```

- **游戏把「文字本身」当 `fmt` 传**，没有 `%s` 占位符；`ArgList` 里通常没有有效字符串参数。
  所以想拿完整标题必须看 `fmt` 本身，不能去看第一个可变参数。
- DrawText 原函数 `0x43B2C2` 里 `Str[i] < 32` 用的是**有符号 char**；GBK 高位字节
  （`movsx` 后为负）再进 `__isctype` 查 CRT 分类表 → 一律判成不可打印/控制字符。
  这是所有「中文被切碎」的总根源。

### 8.2 实机内存：16 字节标签缓冲 + 紧跟的 SCENARIO 原文

`fmt` 指向一个 16 字节的按钮标签缓冲，里面只有被截断的碎片，后面补 0；
**`fmt + 16` 起就是该关 SCENARIO 文件的完整原文**（标题行 + CRLF + 数据行）：

```
fmt#13  n=1  mem= BB 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
                  | BB F9 CF A3 DA AB CE DA 0D 0A 0D 0A 31 39 34 31 ...
                    └── "基希讷乌" ──┘  └── 数据行 "1941年..." ──
fmt#12  n=3  mem= B6 AC BC 00 ... (16 字节)
                  | B6 AC BC BE B7 E7 B1 A9 = "冬季风暴"
```

碎片长度常见 1/3/4 字节（`BB`、`B6 AC BC`、`BF A8 C4`），正好是完整标题被
在第 1/3/4 字节处写 0 截断的结果。277 个样本里 189 个的 `fmt+16` 首行直接等于登记标题。

### 8.3 修复：第 6 个 detour 挂在 CRT `_vsprintf`

| 项 | 值 |
|---|---|
| 地址 | `0x48D910` `_vsprintf`（全程序仅 11 个调用点） |
| prologue | 11 字节：`8B442404 83EC20 8B4C242C`，落在指令边界 |
| 做法 | 用 `va_list` **副本**读参数（不能推进原 `args`，否则格式化结果全乱） |
| 精确匹配 | 在 `fmt+16 .. fmt+271` 里找「与登记表逐字节相等 + 以传入碎片为前缀 + 后随 `0/CR/LF/空格/TAB`」的串 |
| 兜底 | 匹配失败一律不还原，宁可空白，绝不按坐标猜 |

`RecoverLeadByteOnly()` 只在命中精确来源、或候选唯一时还原；多候选时最多输出
所有候选共有的**一个完整首字**（绝不输出别的关卡全名）。

### 8.4 踩过的坑（务必记住）

1. **返回地址写错**：`0x43B206` 是 `call` 指令地址，实际 `_ReturnAddress()` 是 `0x43B20B`。
   差 5 字节会让「是否来自 PrintfText」的判断永远为假。
2. **按 y 坐标猜候选**：旧版用 `(y-252)/23 % 候选数` 把「候选表顺序」当成「游戏关卡顺序」，
   实测把 y=413 的「橡树岭」显示成「向圣洛推进」。同类启发式一律禁止。
3. **拿缓冲区残留消歧**：`text[2..]` 是 `Source` 里**上一次绘制**的残留
   （`哈努夫` 来自 CIECH.TXT「切哈努夫」），与当前按钮无关，不能用来消歧。
4. **最长公共前缀硬凑**：`C8` 开头候选（萨莱诺/萨凡纳/塞洛高地）只有 1 字节公共前缀，
   强行补到 2 字节 → 显示成「塞」。半个字符绝不能硬凑成字。

### 8.5 日志约定

`PG2CHS.log` 是 UTF-8；`D# / A# / W# / blank# / leadbtn# / repair# / measure` 的 hex 后面
都附 GBK→UTF-8 中文；孤立高位字节显示为 `[CF]`，非法 GBK 对显示为 `[CFBF]`。
`fmt#` 记录碎片长度、原文、精确还原结果；未命中时附 `mem=` 40 字节转储，
用于定位仍显示空白的按钮。**排查顺序**：先找 `blank#`（0 像素）→ 对应 `D#` 的 `ret` 和 hex
→ 找同批 `fmt#` 的 `mem=`，看 `fmt+16` 处是否有完整标题。

### 8.6 遗留

有一批 `C8` 槽位（y=252..413 共 8 行）精确匹配失败后保持空白，`mem=` 已记录，
待下一轮实机日志确认它们本应显示什么（也可能是列表的空槽位）。

---

## 9. 文字「不显示 / 变成很黑的黑色」：`__int16` 坐标（2026-11-15）

### 9.1 现象

用户报 `D#1841 (350,4391031) ... ink=E3 | "退出游戏     "` 完全不显示；
同时部分文字「变成很黑的黑色，几乎看不清」。两者其实是**同一个根因**。

### 9.2 根因：detour 形参类型与 IDA 签名不符

IDA 反编译：

```c
__int16 __cdecl sub_43B2C2(__int16 a1, __int16 a2, int a3, char *Str, int a5)
```

**x / y 是有符号 16 位**。cdecl 下它们仍各占 4 字节栈槽，但高 16 位未定义；
调用点分两类，压参方式不同：

| 调用点 | 压参指令 | 高 16 位 | `_ReturnAddress()` |
|---|---|---|---|
| `0x43BCE4` / `0x43BF66` | `movsx eax, word ptr [...]` | 干净（符号扩展） | `0x43BCEF` / `0x43BF77` |
| `0x43BD0D` / `0x43B202` | `mov eax, dword ptr [...]` | **栈垃圾**（`0x0043`/`0x0045`/`0x001A`/`0xE580`…） | `0x43BD1A` / `0x43B20B` |

原函数按 `__int16` 只读低 16 位，所以一直正常。本 DLL 的 `HookDrawText`
把前两个形参声明成 `int`，于是 `y=0x00430077=4391031` 被当成真实坐标，
`RenderStringCJK` 的 `minY..maxY` 裁剪把整串全部丢弃 → 0 像素 → 空白。

实机旧日志 1843 条 `D#` 里 **510 条**坐标异常，`& 0xFFFF` 后
**510 / 510 全部落在 640×480 屏幕内**（例：`(350,4391031) → (350,119)`），证实是高位垃圾而非真实坐标。

### 9.3 为什么会「发黑」

菜单文字由上层函数**画两遍**（IDA `0x43BC60..0x43BD1A`）：

```
0x43BC76  call sub_43B9AF([ebp+0Ch])   ; 重填配色表 → 暗色 ink=0x0F
0x43BCEA  call sub_43B2C2(x+1, y+1)    ; 描边；movsx 干净坐标 → 画出来了
0x43BCF6  call sub_43B9AF([ebp+10h])   ; 重填配色表 → 亮色 ink=0xE3
0x43BD15  call sub_43B2C2(x, y)        ; 主体；dword 带垃圾 → 被裁掉
```

`sub_428813` 的 `xlat` 确认 `colorTable`（全局 `byte_538F40`，256 项）是
「字模值 → 调色板索引」映射表，`if (_AL != -1)` 跳过 0xFF 透明；
`sub_43B9AF(colorIdx)` 在每次绘制前重填该表，所以同一个指针在两遍里
`colorTable[0]` 不同（0x0F 暗 / 0xE3 亮）。

**亮色主体那遍被裁掉，只剩暗色描边** → 用户看到的「很黑、几乎看不清」。
旧日志异常坐标里 488 条是 `ink=E3`，正是丢失的主体。

### 9.4 修复

`HookDrawText` 开头按有符号 16 位还原，后续所有逻辑（日志、碎片还原、
`RenderStringCJK`）自动使用正确坐标：

```cpp
const int rawX = x, rawY = y;
x = (short)(x & 0xFFFF);
y = (short)(y & 0xFFFF);
```

日志在坐标被修正的行尾追加 `[xy16 raw=001A011C,0DB10005]`，便于核对原始栈值。
`WrapDrawCJK` 用表面自身的 `ds->clipX/clipY` 定位、不接收 x/y，不受影响。

### 9.5 验证

- 新实机日志 607 条 `D#`，y 最大 413，**0 条越界**；`[xy16 raw=…]` 6 条显示修正生效。
- `blank#` 从「大量坐标越界」降到只剩 8 个 `C8` 孤立字节（§8.6 遗留，与本次无关）。
- 6 个 detour 全部 OK（charwidth / measure / draw / wrap / blit / vsprintf）。

### 9.6 教训

**detour 的形参类型必须照抄 IDA 反编译签名，不能凭调用点表象猜 `int`。**
16 位坐标在 32 位 cdecl 栈上占 4 字节槽但高位未定义；原函数靠类型隐式截断，
hook 必须自己截断。此类问题的典型症状是「部分文字空白 / 颜色变暗」而非崩溃，
排查时应先统计绘制日志里的坐标范围，而不是先怀疑字库或配色。

---

## 10. 显示层译名替换（地名 / 装备 / 姓氏 / 日期）

### 10.1 为什么不动磁盘文件

用户要求翻译游戏内地形名（`Carentan`、`Merderet River`），以及 `NAMES.TXT` /
`EQUIP97.TXT`。但这两处在游戏里**不是纯显示文本**：

- `SCENARIO/*.TXT` 的非首行是城市名 / 河流名 / 部队代号 / 坐标，**参与地图数据匹配**
  （`.MAP` 里的目标判定靠名字对上）；改文件长度会破坏匹配。
- `NAMES.TXT`（400 个将领姓氏）、`EQUIP97.TXT`（586 行装备）按**索引进存档**，
  翻译会改变字节布局，且与英文存档不兼容。
- 所有文本都经 `PrintfText(0x43B19F) → vsprintf(Source) → DrawText(0x43B2C2)`，
  `A#` 日志证实显示串是完整的，例如 `" Merderet River (25,15)"`、`"Regular"`。

所以采用**显示层替换**：只在 `HookDrawText` / `HookMeasureText` 收到纯 ASCII 串时，
把登记过的词条换成中文，再走 CJK 自绘。游戏内部逻辑、存档、索引、地图匹配
全部保持英文，零风险。

### 10.2 ExtraTerms.txt

`Resources/CHS/ExtraTerms.txt`，GBK，每行 `英文<TAB>中文`，`;` 开头为注释。
由 `Tools/translate/gen_extra_terms.py` 生成：

- `OVERRIDES`：人工校订 205 条（知名地名、实机出现过的词、全部通用单位词、
  常见将领姓氏）。这是后续修正的唯一入口。
- 其余按「整音节 → 单汉字」规则音译（CV/CVN 表 + `St/Saint` 前缀 + `River/Lake`
  后缀）。**早期用「声母字 + 元音字」拼接会产出「特乌特奥尔伊阿勒姆」这种
  不可读的串，已废弃。**
- 装备里的制式型号（`Pz IIIJ`、`88mm FlaK 41`、`M4A1`…）**保持英文**：
  它们是国际通用识别码，硬翻反而妨碍辨认。
- 合计 1455 条；DLL 按英文首字节分桶，避免每帧全表扫描。

日期 `Tuesday, July 25, 1944` 由 `MaybeFormatDate` 单独转成 `1944年7月25日 周二`。

ini 开关 `TranslateExtra=1`（默认开）；译名表放在 ASI 同目录（`update/`），
根目录也放一份兼容 `..\ExtraTerms.txt`。

### 10.3 词边界与零回归

替换要求左右都不是 `[A-Za-z0-9]`，且按英文长度降序最长匹配：
`Carentanville` / `Regularization` 不会被 `Carentan` / `Regular` 切坏。
已含 GBK 的串（GUI97、简报、已还原标题）一律跳过，避免二次替换。

离线用实机 `A#` 全部 25 条串复现 C++ 逻辑：10 条命中（`Carentan`→卡朗坦、
`Merderet River`→梅德雷河、`St Lo`→圣洛、`Tessay-sur-Vire`→泰塞叙尔维尔、
`Regular`→正规军、日期→中文），型号/坐标串保持英文，GUI97 441 行零误替换。

---

## 11. 剩余空白按钮：nearest 消歧与裁剪兜底

### 11.1 8 个 `C8` 按钮

§8.6 遗留的 8 个 `len=1 / b0=C8` 按钮一直空白。扩大 `HookVsprintf` 的精确
搜索范围后发现：旧版只扫 `fmt+16..+271`，而完整标题可能在 **`fmt` 之前**
（上一个文本缓冲的尾部）或隔着 `.SCN` 文件名更远处。现改为用 `VirtualQuery`
圈出可读内存区，扫 `fmt-2048 .. fmt+4096`，收集所有「逐字节等于登记标题、
以碎片为前缀、后随 0/CRLF/空格」的命中。

实机 `fmtamb#` 给出决定性证据——候选距离会随 `fmt` 缓冲布局整体翻转：

```
b0=C8  d=512 萨凡纳          d=672 塞洛高地   d=768 萨莱诺 - 重返欧洲
b0=C8  d=2528 萨莱诺 - 重返欧洲  d=3808 萨凡纳   d=3968 塞洛高地
```

说明它们来自不同页面各自的标题数组，**最近的一项就是当前按钮**；用户实测
「萨凡纳」「萨莱诺 - 重返欧洲」正是各自布局里最近那条。因此多候选时取
`dist` 最小者（>4096 视为无关内存、不还原），并在 `fmtamb#` 里列出全部候选
与距离供核对。这不同于此前被否掉的「按 y 坐标硬分配候选序号」——后者是
无依据的猜测，nearest 是内存布局给出的证据。

结果：`blank#` 从 8 降到 **0**；`b0=BF`→卡昂 - 查恩伍德行动、`b0=CF`→橡树岭、
`b0=CE`→沃洛科拉姆斯克 均与用户实测一致。

### 11.2 简报抬头 / HUD 的 `001AF990` 表面

用户上一轮日志里还有 12 条**完整中文却 0 像素**的绘制（"圣洛 - 眼镜蛇行动"、
"美 回合 1"、"辉煌胜利"…），全部集中在另一个 surface `001AF990`。
`RenderStringCJK` 现在：

- 裁剪区无效或 `maxX/maxY <= 0` 时放宽到整张表面（`rawMinX..rawMaxY` 保留用于日志）；
  注意必须带等号判 `maxX <= minX`，只判 `<` 会漏掉 `minX==maxX==0` 的情况。
- `blank#` 追加 `surf=... clip=(...) max=(...) pitch=... h=... fixed=...`，
  下一轮实机日志可直接判定根因。

冒烟测试只到标题画面、没进该界面，`fixed=1` 尚未触发，需实机确认。

### 11.3 待确认

`b0=CF` 单字节碎片在当前页面全部还原为「橡树岭」。1 字节碎片本身无法区分
同首字节的多个标题，若实机发现某格应为「向圣洛推进」，把对应 `fmtamb#` 行
发回来，按 `kAmbFix` 的方式固定即可（像已修的 `卡昂 - ` → 查恩伍德行动）。

## 12. 最后一轮：按钮互换 / 两套译名 / 底部叠字（2026-11-15）

> 本章修正 §11.1 的一个结论：nearest **不再是可靠判据**。它确实让
> `blank#` 归零（那一步是对的），但对「同首字节的相邻按钮」会稳定地张冠李戴。

### 12.1 「圣洛 - 向圣洛前进」按钮只剩「向」：`len==2` 碎片

`RecoverLeadByteOnly` 原先只认 `len == 1`。实机日志给出完整证据链：

```
fmt#187 n=2 fmt= | "向" full=10 | "向圣洛推进"     ← HookVsprintf 已精确找到完整标题
D#318 (68,413) ... len=2 txt=CF F2 | "向"           ← 绘制侧只画出一个「向」
```

`CF F2` 是完整汉字「向」，不是半个字符。`HookVsprintf` 的 `g_lastFullTitle`
已经算出「向圣洛推进」，但绘制侧因 `len != 1` 直接跳过；`RepairFragment`
的 `MatchInPool` 又因「向」是「向圣洛推进 / 向海岸推进 / 奔向敦刻尔克」的
公共片段判为歧义返回 `-1`，于是永远只显示一个字。

修法：放宽到 `len <= 2`，并加 `memcmp(g_lastFullTitle, text, len) == 0`
前缀校验 —— 只在「碎片确实是精确标题的前缀」时才还原，不会把普通的两字节
完整词误当碎片。

### 12.2 `b0=C8` 两个按钮互换：改用外部校订表

关卡列表里 `C8` 开头的标题有 4 个（萨凡纳 / 萨莱诺登陆 /
萨莱诺 - 重返欧洲 / 塞洛高地），前三个被切成 1 字节碎片。实机 `fmtamb#`
显示两种稳定的内存布局，nearest 的结果恰好互反：

```
布局A  nearest 萨凡纳            ← 该按钮应是「萨莱诺 - 重返欧洲」
布局B  nearest 萨莱诺 - 重返欧洲  ← 该按钮应是「萨凡纳」
```

用户真机确认「反了」。**1 字节碎片本身不携带足够信息**，内存距离、
相邻记录、`.SCN` 文件名都会随列表分页漂移，纯算法无法 100% 消歧。
因此不再改 nearest 本身，而是在「多候选」路径后加一道**外部可校订表**
`Resources/CHS/FragTitleFix.txt`（GBK，TAB 分隔）：

```
C8<TAB>萨凡纳<TAB>萨莱诺 - 重返欧洲
C8<TAB>萨莱诺 - 重返欧洲<TAB>萨凡纳
```

唯一命中路径（如「萨莱诺登陆」）不经过此表；`b0=BF / CE / CF` 等其它
碎片也没有对应条目，离线回放确认结果全部保持不变。以后再发现错配，
加一行即可，不必重编 DLL。`fmtamb#` 现在附带 `fmt` 指针与 32 字节内存，
下一轮若仍有问题可直接按地址建立精确映射。

### 12.3 「塞洛高地」按钮 / 「泽劳高地」简报：同一关卡两套译名

这不是串位，是**两张独立译表不一致**：

- `DATA_TITLES`（`gen_scenario_chs.py`）：只译 `SCENARIO/*.TXT` 数据文件首行，
  `"Seelow Heights" → "塞洛高地"`，即列表按钮文字；
- `scenario_zh.txt`：按「唯一英文行」组织的简报译文表，
  同一句被译成「泽劳高地」，即简报正文/抬头。

`W#24` 首行「泽劳高地」与 `SEELOW.TXT` 首行「塞洛高地」因此对不上。
同类还有 `圣洛 - 向圣洛推进 / 向圣洛推进`、`向海岸突进 / 向海岸推进`、
`赤潮升起 / 赤潮上涨`、`奇尔河土星行动 / 奇尔河上的土星`。
统一到 `DATA_TITLES` 那套（数据文件首行不动，只改简报译文表，脚本会自动
按原始字节数补空格），并顺带对齐 `ExtraTerms.txt` 里的
`Invasion Salerno → 萨莱诺登陆`、`Savannah River → 萨凡纳河`。
改完重新跑 `gen_scenario_chs.py` 全量重生成 306 个剧本文件。

### 12.4 底部残留：字形高于游戏脏矩形（12.4 的原判断已被推翻）

**原判断（错误）**：以为叠字发生在简报翻页，是「游戏按英文行数刷新背景、
中文行高 12px 排不下」；于是在 `WrapDrawCJK` 排版前按像素众数把文本矩形
刷成单色。真机结果：简报底纹被刷成一整块灰（帧图 x≈24..350 全为索引 69），
且用户实测「缓冲区不发生在简报」，叠字在别的界面。

**真因**：游戏按自带位图字形的高度（fontObj+8，实机 = 8px，行距 10px）
刷新文字背景的脏矩形。12px 汉字最下面 ~3~4px（用户目测 25%）永远落在
刷新区之外，上一次文字的下半截擦不掉，反复重绘就残留、叠字。
这与简报是否重绘无关，所有走 `RenderStringCJK` / `WrapDrawCJK` 的
文本都会中招 —— 所以「刷背景」既治错了地方，又引入了灰块。

**当时的解法（现已被 §12.6 取代，仅作中间记录）**：把字形本身压进刷新高度。

- `EffectiveDrawHeight()`：默认取 `g_lineH`（8px），可用 `GlyphHeight` 覆盖。
- `DrawGlyph()` 新增 `topY` / `outH`：基线与 `gmptGlyphOrigin.y` 同步按
  `outH/g_pxHeight` 缩放，源行 `r` 映射到 `r * boxH / srcBoxH`，多个源行落到
  同一目标行时都写同一个 `ink`，等价于按位 OR —— 笔画只会合并不会丢失。
  横向步进 `gmCellIncX` 完全不变，仍是 12px 点阵。
- 两处行距改回游戏节奏 `g_lineH + 2 = 10`（原来被 12px 字号顶到 12）。
- `ClearWrapBg` 整段删除；保留 12.4 里那处自然换行 `\n` 的越界判断。

离线字模验证（SimSun 1bpp，文字「正规军 萨莱诺 向圣洛推进」）：

| 方案 | 字形 y 范围 | 高 | 宽 | 笔画像素 |
|---|---|---|---|---|
| 12px 原样 | 1..12 | 12px | 156 | 554 |
| **12→8 压缩（默认）** | **1..8** | **8px** | **156** | **474** |
| 12→9 压缩 | 1..9 | 9px | 156 | 491 |
| 10px 原样 | 1..9 | 9px | 130 | 360 |

压缩后字形完整落在刷新区内；横向信息量比直接用 10px 字体多约三成，
可读性明显更好。预览图：`build_out/_glyph_fit_preview.png`。

`PG2CHS.ini` 开关：

```ini
FitGlyphHeight=1   ; 0 = 保持完整 12px 字高（字方，但会残留）
GlyphHeight=0      ; 0 = 自动取游戏字形高(8)；觉得偏扁可试 9 / 10
VOffset=0          ; 仍可整体上下微调 1~2px
```
### 12.5 本轮产物与待确认

- `PG2CHS_DLL.asi` 209408 B、`FragTitleFix.txt`、`ExtraTerms.txt`、
  306 个 `SCENARIO/*.TXT`、`PG2CHS.ini` 已部署到 `update/`。
- 发布包：`build_out/PanzerGeneral2-CHS-2026-11-15-04-47-38.zip`。
- 部署时游戏仍在运行，旧 ASI 被占用，已改名为 `PG2CHS_DLL.asi.old`，
  新 ASI 就位 —— **必须重启游戏才会加载新版本**。
- 待真机确认：三个 `C8` 按钮文字、简报抬头与按钮是否同名、
  底部残留是否消失、压缩后字形观感（必要时调 `GlyphHeight`）。

---

#### 12.6 恢复 12px 方字：墨迹自撤销（2026-11-15，当前默认）

##### 12.6.1 为什么「压字形」不够

§12.4 把字压成 12 宽 x 8 高的扁字后残留确实消失，但汉字是方块字，
纵向砍掉 1/3 行、相邻笔画 OR 合并后结构变形，真机反馈**难以阅读**。
用户希望的是「不残留」**且**「字方正」，所以不能一直委屈字形去迁就
那个 8px 的刷新矩形。

##### 12.6.2 IDA：8 到底被谁共用，为什么不能直接改

打开游戏目录里现成的 `PG2UK220.EXE.i64`（ida-pro-mcp，会话 headless），
反编译 / 反汇编得到：

| 地址 | 结论 |
|---|---|
| `sub_43B2C2` (`0x43B2C2`) | 统一 DrawText。**本身不擦背景**：`LockMainBuffer` 后逐字调 `sub_428813` 透明叠加，结尾 `UnlockMainBuffer`。换行时 `y += *(WORD*)(fontObj+8) + 2` |
| `sub_43B402` (`0x43B402`) | 自动换行绘制（简报）。`0x43B464` 同样读 `dword_53895C → [+8] + 2` 当行高 |
| `sub_428813` (`0x428813`) | 单字形 blit。`v15 = *(fontObj+8)` 被当作**字形位图的行数**参与裁剪与逐行拷贝 |
| `sub_45197D` / `sub_451A1F` | 只是 DirectDraw 主表面的 Lock / Unlock，不是擦除 |
| `sub_43B19F` | PrintfText：`vsprintf → sub_43B2C2`，真实返回地址 `0x43B20B` |

**所以不能直接把 `fontObj+8` 从 8 改成 12**：前两处会顺带把行距变成 14
（布局、分页、`startLine` 语义全变），第三处会让原版英文字形位图**读越界**
（位图数据只有 8 行），花屏甚至崩溃。按「像素众数」整块刷背景也不行
（§12.4 已证明会把简报底纹刷成灰块）。

##### 12.6.3 日志证据：残留发生在单行路径，且文字画两遍

```
D#2421 (302,458) ... ink=0F  "正规军"   ← 暗色描边
D#2422 (301,457) ... ink=E3  "正规军"   ← 亮色主体
D#2435 (302,458) ... ink=0F  "正规军"
D#2436 (301,457) ... ink=E3  "正规军"
```

- 残留的「正规军」是状态栏**单行文本**，走 `RenderStringCJK`；
  简报走 `WrapDrawCJK`（`W#`，整轮日志只有 51 条），用户实测简报无此问题。
- 同一串字被画两遍做描边：坐标差 1px、墨色不同（0F / E3）。
  因此撤销记录必须按 **(表面, x, y, 墨色)** 分组，两层各自独立，
  否则第二次绘制会把第一次刚画好的描边擦掉。

##### 12.6.4 方案：由 DLL 自己擦，只碰自己写过的像素

不依赖游戏擦、不改游戏代码、不动任何背景像素：

1. `DrawGlyph` 每写一个笔画像素，把 `(x, y, 原色, 墨色)` 追加到当前文本块。
2. 下次同一文本块重画前（`InkBegin`），逐点检查：**只有当前值仍等于
   当时写下的墨色**才写回原色。若游戏后来在此画了别的东西，值已变，
   跳过即可，绝不破坏新内容。
3. 撤销完清空记录，再画新字。同一像素在一串里被写多次时，顺序撤销
   会因「第一次写回真实背景、后续记录当前值 ≠ 墨色而跳过」自然收敛。
4. `InkScope` 用 RAII 保证所有早退路径都清空 `g_curInk`，
   `WrapDrawCJK` 开头再显式置空，避免两条路径串味。
5. 块上限 512，超出按最久未用淘汰；`pitch/surfH` 变了就丢弃旧记录，
   防止表面重建后写错地址。

**分路径**，因为两条路径的约束不同：

| 路径 | 字形高 | 行距 | 擦除 |
|---|---|---|---|
| `RenderStringCJK`（按钮 / 状态栏 / 菜单，单行） | **完整 12px 方字** | `drawH+1 = 13` | 墨迹自撤销 |
| `WrapDrawCJK`（简报，多行、行距仅 10px） | 8px 压高 | 10 | 沿用游戏刷新 |

简报若也用 12px 会压到下一行且要重算 `startLine` 分页，风险大且本来没病，
保持不动。

##### 12.6.5 离线验证

用真机同款 SimSun 12px 1bpp 字模（`tmAscent=10`）在带纹理底纹上模拟
「上一帧写 正规军 → 游戏只擦前 8 行 → 下一帧改写 党卫军」，含暗描边 +
亮主体两层，与「只画新字」的理想画面逐像素比对：

| 方案 | 差异像素（残留） | 底纹被改动 |
|---|---|---|
| 12px 方字，不自擦 | **38**（明显叠字） | 0 |
| **12px 方字 + 墨迹自撤销** | **3**（描边色，视觉即描边） | **0** |
| 8px 压扁字（§12.4 旧默认） | 4 | 0 |

那 3 个像素来自描边层与主体层的交叠边缘：主体块记录的 `old` 可能是
描边色 0F。因为游戏本来就用 0F 画描边，视觉上无感，且下次重画即被覆盖。
预览图：`build_out/_ink_undo_preview.png`（三行对比）。

卸载时日志会打印 `inkundo: 累计恢复 N 个笔画像素，活跃文本块 M`，
可直接确认机制确实在跑；`font:` 行现在也会显示
`简报绘制高=8  普通文本绘制高=12  墨迹撤销=1`。

##### 12.6.6 开关与回退

```ini
InkUndo=1          ; 0 = 关掉自擦除（会回到旧字残留）
FullHeightText=1   ; 0 = 普通文本也压成 8px（回到 §12.4 的扁字）
FitGlyphHeight=1   ; 仍控制简报多行路径 / 回退路径
GlyphHeight=0      ; 简报压高目标，0 = 自动取游戏字形高(8)
VOffset=0          ; 整体上下微调
```

最坏情况把 `InkUndo=0` + `FullHeightText=0` 即完全回到上一版行为，
无需重编译。

- 待真机确认：状态栏 / 按钮的 12px 方字观感、是否还有底部残影、
  多层描边处有无极少量暗点、大批量文本下 `inkundo` 统计是否正常。
