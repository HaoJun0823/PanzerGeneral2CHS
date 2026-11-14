// dllmain.cpp — 装甲元帅2（PG2UK220.EXE）简体中文汉化 ASI
// ----------------------------------------------------------------------------
// 思路（详见仓库 Documents/research_handoff.md）：
//   · 游戏内文字 = 8 位调色板帧缓冲的逐像素写入，不走 GDI。
//   · 主渲染入口 sub_43B2C2(0x43B2C2)：int(__cdecl)(x, y, colorTable, text, surface)
//   · 绘图面是全局 dword_53895C(0x53895C)，colorTable(byte_538F40) 作 xlat 把字形
//     覆盖值 → 调色板索引（0xFF=透明）。
//   · 本 DLL 用「纯 Win32 手动 detour」挂 sub_43B2C2（不依赖 MinHook / NuGet）：
//       - 纯 ASCII 串 → 跳板调原函数（零回归）
//       - 含 GBK 双字节(CJK) 的串 → GDI 取字形 → 经 colorTable 写进帧缓冲
//
// 部署：编译产物 PG2CHS_DLL.dll 改名 PG2CHS_DLL.asi 放游戏根目录，
//       由 dist/common/winmm.dll (Ultimate ASI Loader) 自动加载。
// ----------------------------------------------------------------------------

#include "pch.h"
#include <vector>
#include <string>

// ===================== 配置（可用同目录 PG2CHS.ini 覆盖） =====================
static struct Cfg {
    std::wstring fontName = L"Microsoft YaHei"; // CJK 字体（回退 SimSun）
    int   fontHeight      = 16;                 // 字号（像素）
    int   vOffset         = 0;                  // 垂直微调（正=下，负=上）
    bool  debug           = false;              // 是否写详细日志
} g_cfg;

// ===================== 关键地址（相对 ImageBase 0x400000） ==================
static const char* TARGET_EXE    = "PG2UK220.EXE";
static const DWORD  IMAGE_BASE   = 0x400000;
static const DWORD  ADDR_DRAWTEXT = 0x43B2C2;   // sub_43B2C2（HOOK 点）
static const DWORD  ADDR_SURFACE  = 0x53895C;   // dword_53895C（DrawSurface 本身）

// ===================== 帧缓冲面结构（运行时读出） ==========================
#pragma pack(push, 4)
struct DrawSurface {            // 位于 dword_53895C（本身即结构体，非指针）
    void*  H;                   // [0] -> SurfaceHeader*
    int    clipX;               // [1] 裁剪原点 X（绘制时加到坐标上）
    int    clipY;               // [2] 裁剪原点 Y
    int    maxX;                // [3]
    int    maxY;                // [4]
};
struct SurfaceHeader {          // *H
    void*  pixels;              // [0] 像素基址（BYTE*，8bpp）
    int    pitchMinus1;         // [1] pitch = +1
    int    heightMinus1;        // [2] height = +1
};
#pragma pack(pop)

// ===================== 原函数类型 / 蹦床 ====================================
typedef int (__cdecl* OrigDrawTextFn)(int x, int y, const unsigned char* colorTable,
                                      const char* text, void* surface);
static OrigDrawTextFn g_origDrawText = nullptr;

// ===================== GDI 字形渲染状态 =====================================
static HDC    g_hdc    = nullptr;   // 离屏 DC（用于 GetGlyphOutline）
static HFONT  g_hfont  = nullptr;   // CJK 字体
static bool   g_fontOk = false;     // 字体是否就绪

// ===================== 日志 ================================================
static FILE* g_log = nullptr;
static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// 读同目录 PG2CHS.ini
static void LoadConfig(const wchar_t* dllPath)
{
    wchar_t iniPath[MAX_PATH];
    wcscpy_s(iniPath, dllPath);
    wchar_t* slash = wcsrchr(iniPath, L'\\');
    if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash - iniPath + 1), L"PG2CHS.ini");
    else wcscpy_s(iniPath, L"PG2CHS.ini");

    wchar_t buf[64];
    if (GetPrivateProfileStringW(L"PG2CHS", L"FontName", g_cfg.fontName.c_str(),
                                 buf, 64, iniPath))
        g_cfg.fontName = buf;
    g_cfg.fontHeight = GetPrivateProfileIntW(L"PG2CHS", L"FontHeight", g_cfg.fontHeight, iniPath);
    g_cfg.vOffset    = GetPrivateProfileIntW(L"PG2CHS", L"VOffset",    g_cfg.vOffset,    iniPath);
    g_cfg.debug      = GetPrivateProfileIntW(L"PG2CHS", L"Debug",      g_cfg.debug ? 1 : 0, iniPath) != 0;
}

// 初始化 GDI 字体（创建 DC + 选中 TrueType CJK 字体）
static void InitFont()
{
    struct { const wchar_t* name; } candidates[] = {
        { g_cfg.fontName.c_str() },
        { L"SimSun" },
        { L"Microsoft YaHei UI" },
        { L"Microsoft YaHei" },
    };
    g_hdc = CreateCompatibleDC(nullptr);
    if (!g_hdc) { Log("font: CreateCompatibleDC 失败\n"); return; }

    MAT2 m = { {0,1},{0,0},{0,0},{0,1} };
    for (auto& c : candidates)
    {
        HFONT f = CreateFontW(-g_cfg.fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, c.name);
        if (!f) continue;
        HFONT old = (HFONT)SelectObject(g_hdc, f);
        GLYPHMETRICS gm;
        // 查询 'A' 字形缓冲区大小；返回正数即说明该字体有可描边轮廓（TrueType）
        DWORD rv = GetGlyphOutlineW(g_hdc, L'A', GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
        bool ok = (rv != GDI_ERROR && rv > 0);
        if (old) SelectObject(g_hdc, old);
        if (ok)
        {
            if (g_hfont) DeleteObject(g_hfont);
            g_hfont = f; g_fontOk = true;
            Log("font: 使用字体 %ls (h=%d)\n", c.name, g_cfg.fontHeight);
            return;
        }
        DeleteObject(f);
    }
    Log("font: 未找到可用 TrueType CJK 字体，CJK 路径将跳过绘制\n");
}

// 把一个 Unicode 码点的字形（GGO_GRAY8 灰度掩码）写进 8bpp 帧缓冲。
// penX      = 字符笔位 X；topY = 黑盒顶边 Y（已顶对齐，调用方传入 y+V_OFFSET）。
// 黑盒左缘 = penX - 左向侧边距(gmptGlyphOrigin.x)，使字间距符合字体度量。
static void DrawGlyph(WCHAR wc, int penX, int topY,
                      const unsigned char* colorTable, BYTE* pixels, int pitch, int surfH,
                      int clipX, int clipY, int& advOut)
{
    MAT2 m = { {0,1},{0,0},{0,0},{0,1} };
    GLYPHMETRICS gm;
    DWORD bufSize = GetGlyphOutlineW(g_hdc, wc, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
    advOut = gm.gmCellIncX;
    if (bufSize == GDI_ERROR || bufSize == 0) { advOut = g_cfg.fontHeight; return; }

    std::vector<BYTE> buf(bufSize);
    if (GetGlyphOutlineW(g_hdc, wc, GGO_GRAY8_BITMAP, &gm, bufSize, buf.data(), &m) == GDI_ERROR)
    { advOut = g_cfg.fontHeight; return; }

    // GGO_GRAY8 灰度自动归一化到 0..255（兼容不同灰色阶实现）
    int mx = 0;
    for (DWORD i = 0; i < bufSize; ++i) if (buf[i] > mx) mx = buf[i];
    float scale = (mx > 1) ? (255.0f / mx) : 1.0f;

    int bufPitch = (gm.gmBlackBoxX + 3) & ~3;    // 行按 4 字节对齐
    int topX = clipX + (penX - gm.gmptGlyphOrigin.x);
    int baseY = clipY + topY;

    for (int r = 0; r < gm.gmBlackBoxY; ++r)
    {
        int sy = baseY + r;
        if (sy < 0 || sy >= surfH) continue;
        for (int c = 0; c < gm.gmBlackBoxX; ++c)
        {
            int cov = buf[(size_t)r * bufPitch + c];
            if (cov <= 0) continue;
            int cov8 = (int)(cov * scale);
            if (cov8 > 255) cov8 = 255;
            BYTE pal = colorTable[cov8 & 0xFF];
            if (pal == 0xFF) continue;           // 透明
            int sx = topX + c;
            if (sx < 0 || sx >= pitch) continue;
            pixels[(size_t)sy * pitch + sx] = pal; // 写 8bpp 调色板索引
        }
    }
}

// 整串自绘（含 CJK 时调用）。纯 ASCII 已在 Hook 中分流到原函数。
static int RenderStringCJK(int x, int y, const unsigned char* colorTable,
                           const char* text, void* /*surface*/)
{
    // 无可用 CJK 字体时，不调用原 Latin 渲染器（会把 GBK 字节画成乱码），
    // 直接跳过本串（英文/ASCII 串仍走原函数，不受影响）。
    if (!g_fontOk) return 0;

    // 读出绘图面（全局 dword_53895C 本身就是 DrawSurface 结构）
    DrawSurface* ds = (DrawSurface*)((BYTE*)GetModuleHandleA(TARGET_EXE) + (ADDR_SURFACE - IMAGE_BASE));
    if (!ds || !ds->H) return g_origDrawText(x, y, colorTable, text, nullptr);
    SurfaceHeader* H = (SurfaceHeader*)ds->H;
    if (!H || !H->pixels) return g_origDrawText(x, y, colorTable, text, nullptr);
    BYTE*  pixels = (BYTE*)H->pixels;
    int    pitch  = H->pitchMinus1 + 1;
    int    surfH  = H->heightMinus1 + 1;
    int    clipX  = ds->clipX;
    int    clipY  = ds->clipY;

    int curX = x;
    int len  = (int)strlen(text);
    for (int i = 0; i < len; )
    {
        WCHAR wc; int consumed;
        BYTE b = (BYTE)text[i];
        if (b >= 0x81 && i + 1 < len)            // GBK 双字节
        {
            char db[3] = { text[i], text[i + 1], 0 };
            WCHAR wbuf[2] = { 0 };
            if (MultiByteToWideChar(936, 0, db, 2, wbuf, 2) > 0) { wc = wbuf[0]; consumed = 2; }
            else { wc = (WCHAR)'?'; consumed = 2; }
        }
        else if (b < 0x80)                        // ASCII
        { wc = (WCHAR)b; consumed = 1; }
        else { i += 1; continue; }                // 孤立高位字节，跳过

        if (wc == L'\n') { curX = x; y += g_cfg.fontHeight + 2; i += consumed; continue; }

        int adv;
        DrawGlyph(wc, curX, y, colorTable, pixels, pitch, surfH, clipX, clipY, adv);
        curX += adv;
        i += consumed;
    }
    return 0;
}

// ===================== HOOK 函数 ===========================================
static int __cdecl HookDrawText(int x, int y, const unsigned char* colorTable,
                                const char* text, void* surface)
{
    if (!text) return g_origDrawText(x, y, colorTable, text, surface);

    // 快速判断是否为纯 ASCII（无 >=0x81 字节）→ 直接原函数（零回归）
    bool hasCJK = false;
    for (const char* p = text; *p; ++p) { if ((BYTE)*p >= 0x81) { hasCJK = true; break; } }

    if (!hasCJK) return g_origDrawText(x, y, colorTable, text, surface);
    return RenderStringCJK(x, y, colorTable, text, surface);
}

// ===================== 手动 Detour（无外部依赖，纯 Win32） ==================
// 目标 sub_43B2C2 经 IDA 确认 prologue（GOG PG2UK220.EXE）：
//   0x55            push ebp          (1)
//   8B EC           mov ebp, esp      (2)
//   83 EC 10        sub esp, 10h      (3)
//   53              push ebx          (1)  ← 下一条指令边界在偏移 6
//   ...
// relocate 长度 = 6（覆盖到 push ebx 之前），跳板 = 原 6 字节 + E9 回跳到 addr+6。
static const int HOOK_LEN = 6;
static BYTE   g_origBytes[HOOK_LEN] = { 0 };
static BYTE*  g_trampoline = nullptr;
static bool   g_hooked = false;

// 在 at 处写入 5 字节相对跳转（E9 rel32）到 target
static void WriteJmp(BYTE* at, const void* target)
{
    DWORD rel = (DWORD)((const BYTE*)target - (at + 5));
    at[0] = 0xE9;
    *(DWORD*)(at + 1) = rel;
}

static bool InstallHook(void* target)
{
    // 安全闸门：入口必须以 push ebp(0x55) 开头（prologue 未变）
    if (((const BYTE*)target)[0] != 0x55) {
        Log("InstallHook: 入口字节非 push ebp(0x55)，疑似 EXE 版本不符，放弃 hook\n");
        return false;
    }
    // 跳板：原 HOOK_LEN 字节 + 5 字节回跳
    g_trampoline = (BYTE*)VirtualAlloc(nullptr, HOOK_LEN + 5,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) { Log("InstallHook: VirtualAlloc 跳板失败\n"); return false; }
    memcpy(g_trampoline, target, HOOK_LEN);
    WriteJmp(g_trampoline + HOOK_LEN, (BYTE*)target + HOOK_LEN);

    // 32 位相对跳转范围闸门：rel32 必须落在有符号 32 位内，否则跳转会错乱致崩。
    // 用 64 位算距离再判界，避免 LONG 回卷后无法察觉溢出。
    LONGLONG dHook = (LONGLONG)(BYTE*)g_trampoline - (LONGLONG)((BYTE*)target + 5);
    LONGLONG dBack = (LONGLONG)((BYTE*)target + HOOK_LEN) -
                     (LONGLONG)((BYTE*)g_trampoline + HOOK_LEN + 5);
    if (dHook < -0x80000000LL || dHook > 0x7FFFFFFFLL ||
        dBack < -0x80000000LL || dBack > 0x7FFFFFFFLL) {
        Log("InstallHook: 跳板距离超 2GB，放弃 hook\n");
        VirtualFree(g_trampoline, 0, MEM_RELEASE); g_trampoline = nullptr;
        return false;
    }

    // 保存原字节，改写入口为 jmp 跳板
    memcpy(g_origBytes, target, HOOK_LEN);
    DWORD op;
    if (!VirtualProtect(target, HOOK_LEN, PAGE_EXECUTE_READWRITE, &op)) {
        Log("InstallHook: VirtualProtect 失败\n");
        VirtualFree(g_trampoline, 0, MEM_RELEASE); g_trampoline = nullptr;
        return false;
    }
    WriteJmp((BYTE*)target, g_trampoline);
    ((BYTE*)target)[5] = 0x90; // NOP 第 6 字节，保证整个 relocate 区域被覆盖
    VirtualProtect(target, HOOK_LEN, op, &op);
    FlushInstructionCache(GetCurrentProcess(), target, HOOK_LEN);
    FlushInstructionCache(GetCurrentProcess(), g_trampoline, HOOK_LEN + 5);

    g_origDrawText = (OrigDrawTextFn)g_trampoline;
    g_hooked = true;
    return true;
}

static void UninstallHook(void* target)
{
    if (!g_hooked || !target) return;
    DWORD op;
    if (VirtualProtect(target, HOOK_LEN, PAGE_EXECUTE_READWRITE, &op)) {
        memcpy(target, g_origBytes, HOOK_LEN);
        VirtualProtect(target, HOOK_LEN, op, &op);
        FlushInstructionCache(GetCurrentProcess(), target, HOOK_LEN);
    }
    if (g_trampoline) { VirtualFree(g_trampoline, 0, MEM_RELEASE); g_trampoline = nullptr; }
    g_hooked = false;
}

// ===================== DLL 入口 ============================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        wchar_t dllPath[MAX_PATH] = { 0 };
        GetModuleFileNameW(hModule, dllPath, MAX_PATH);
        wchar_t logPath[MAX_PATH];
        wcscpy_s(logPath, dllPath);
        wchar_t* slash = wcsrchr(logPath, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash - logPath + 1), L"PG2CHS.log");
        else wcscpy_s(logPath, L"PG2CHS.log");
        g_log = _wfopen(logPath, L"w");
        LoadConfig(dllPath);
        Log("PG2CHS: 附加到进程 (hMod=%p)\n", hModule);

        InitFont();

        HMODULE hGame = GetModuleHandleA(TARGET_EXE);
        if (!hGame) { Log("找不到 %s\n", TARGET_EXE); break; }
        void* pDrawText = (BYTE*)hGame + (ADDR_DRAWTEXT - IMAGE_BASE);
        if (!InstallHook(pDrawText)) { Log("InstallHook 失败 @%p\n", pDrawText); break; }
        Log("PG2CHS: 已挂 sub_43B2C2 @%p，hook 生效\n", pDrawText);
        if (g_cfg.debug) Log("PG2CHS: 配置 font=%ls h=%d vOff=%d\n",
                             g_cfg.fontName.c_str(), g_cfg.fontHeight, g_cfg.vOffset);
        break;
    }
    case DLL_PROCESS_DETACH:
        UninstallHook((BYTE*)GetModuleHandleA(TARGET_EXE) + (ADDR_DRAWTEXT - IMAGE_BASE));
        if (g_hdc) DeleteDC(g_hdc);
        if (g_hfont) DeleteObject(g_hfont);
        if (g_log) { Log("PG2CHS: 卸载\n"); fclose(g_log); g_log = nullptr; }
        break;
    }
    return TRUE;
}
