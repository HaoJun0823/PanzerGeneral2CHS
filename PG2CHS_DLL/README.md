# PG2CHS_DLL — 汉化 ASI 插件源码

> 当前状态：**空桩**。仅 `dllmain.cpp` 的 `DllMain` 骨架，尚未挂任何 hook。
> 渲染路径（GDI TextOutA vs 位图字体 blit）确认后，这里才是真正的汉化逻辑所在。
> 详见 [`../Documents/research_handoff.md`](../Documents/research_handoff.md) §3。

## 工程配置（已设好）
- VS2017，**v141（MSVC 14.16）**，`Win32` + `x64`，动态库，**Win10 SDK 10.0.26100.0**。
- 与参考项目的 msvc-wine CI 配方（14.16 + SDK 26100）严格一致，保证本地/CI 产物同源。

## 部署模型（待最终确认）
发布包里：
- `winmm.dll`（来自 `dist/common/`，Ultimate ASI Loader，MIT）放在游戏根目录，
  代理系统 DLL 并加载 `scripts/*.asi`。
- `scripts/PG2CHS_DLL.asi`（本工程编译产物改名）提供中文字形与文本替换。

> ⚠ **开放问题**：`winmm.dll` 是按参考项目默认放置的占位。
> 必须确认 `PG2UK220.EXE` 实际加载、可被代理的系统 DLL（winmm / dsound / 其它）。
> 若游戏不 import winmm，需换成它真正加载的 DLL 作代理目标（见 research_handoff §3.4）。

## 本地编译
```bash
python ../Tools/ci/build_dll_local.py --clean
```
产出 `PG2CHS_DLL/build/PG2CHS_DLL.dll`，由 `../Tools/ci/build_release.py` 改名 `.asi` 收进发布包。
