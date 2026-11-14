# PG2CHS_DLL — 汉化 ASI 插件源码

> 当前状态：**MVP 已实现**。挂钩游戏主文字 blit 函数 `sub_43B2C2`，
> 对含 GBK 双字节（CJK）的字符串，用 GDI `GetGlyphOutline(GGO_GRAY8)` 取字形 →
> 经游戏颜色 xlat 表写进 8 位调色板帧缓冲；纯 ASCII 串走跳板调原函数（零回归）。
> 详见 [`../Documents/research_handoff.md`](../Documents/research_handoff.md) §2 / §3.1。

## 工程配置（已设好）
- VS2017，**v141（MSVC 14.16）**，`Win32` + `x64`，动态库，**Win10 SDK 10.0.26100.0**。
- 与参考项目的 msvc-wine CI 配方（14.16 + SDK 26100）严格一致，保证本地/CI 产物同源。
- **不依赖 MinHook / NuGet**：挂钩用纯 Win32 手动 detour（E9 跳转 + 跳板），
  与 MajestyIIExtend 同思路，规避 msvc-wine 下原生包还原的不确定性。

## Hook 实现要点
- 目标：`sub_43B2C2` @ 0x43B2C2，`int(__cdecl)(x, y, colorTable, text, surface)`。
- relocate 长度 **6 字节**（IDA 确认 prologue：`push ebp / mov ebp,esp / sub esp,10h / push ebx`，
  取 6 字节恰好落在 `push ebx` 之前，避免切断 `sub esp,10h`）。
- 跳板 = 原 6 字节 + `E9 rel32` 回跳到 `addr+6`；`g_origDrawText` = 跳板指针。
- 版本闸门：DllMain 检查入口首字节是否为 `0x55(push ebp)`，不符则放弃 hook（安全降级）。
- 绘图面从全局 `dword_53895C`(0x53895C) 读出（非第 5 参），结构见 research_handoff §2。

## 字体
- MVP 直接用系统 CJK 字体：`CreateFontW` 依次尝试配置字体 / SimSun / Microsoft YaHei UI / YaHei。
- 可用同目录 `PG2CHS.ini` 覆盖：`FontName`、`FontHeight`(默认16)、`VOffset`、`Debug`。

## 部署模型
发布包里：
- `winmm.dll`（来自 `dist/common/`，Ultimate ASI Loader，MIT）放在游戏根目录，
  代理系统 DLL 并加载 `scripts/*.asi`。
- `scripts/PG2CHS_DLL.asi`（本工程编译产物改名）提供中文字形与文本替换。
- 已用 IDA 确认 `PG2UK220.EXE` 真 import `winmm`（11 个函数），代理链路成立。

## 本地编译
```bash
python ../Tools/ci/build_dll_local.py --clean
```
产出 `PG2CHS_DLL/build/PG2CHS_DLL.dll`，由 `../Tools/ci/build_release.py` 改名 `.asi` 收进发布包。
