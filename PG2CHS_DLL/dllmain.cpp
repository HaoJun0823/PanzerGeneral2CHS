// dllmain.cpp — 装甲元帅2（PG2UK220.EXE）简体中文汉化 ASI
// ----------------------------------------------------------------------------
// 渲染路径（IDA + 崩溃转储确认）：
//   · 游戏内文字 = 8 位调色板帧缓冲的逐像素写入，不走 GDI。
//   · 0x43B2C2 绘制文字：int(__cdecl)(x, y, colorTable, text, surface)
//       绘图面 = 第 5 个参数 surface（blit arg1）：{ H, clipX, clipY, maxX, maxY }，
//       H -> { pixels, pitchMinus1, heightMinus1 }
//   · 0x53895C 全局存「字体对象」指针：+8 = 行高，+0x10 = 字符偏移表
//   · 0x43B7C4 测量文字宽度：short(__cdecl)(const char* text)
//       逐字节遍历，movsx 把字节当【有符号】字符，再去 0x4287F3 查字宽表。
//       GBK 汉字字节 0x81~0xFE 符号扩展成负数 → 查表越界 → 崩（本次崩溃根因）。
//   · 0x4287F3 字符宽度查表：value = *(int*)(fontRes + table[ch])
//
// 本 DLL 用「纯 Win32 手动 detour」（无 MinHook / NuGet），挂 3 个点：
//   1) 0x43B2C2 绘制：纯 ASCII → 跳板调原函数（零回归）；含 GBK → 本 DLL 用
//      GDI GetGlyphOutline 取字形、经 colorTable 写 8bpp 帧缓冲。
//   2) 0x43B7C4 测宽：纯 ASCII → 原函数；含 GBK → 按字符（GBK 双字节）用
//      GDI 字形步进累加宽度，避免逐字节符号扩展越界。
//   3) 0x4287F3 字符查表：高位字节兜底返回半宽（GBK 2 字节 = 全宽），
//      防止其它未预见的调用点仍以负索引查表而崩。
//
// 部署：PG2CHS_DLL.dll 改名 PG2CHS_DLL.asi，由 dist/common/winmm.dll
//       (Ultimate ASI Loader) 从游戏目录/子目录加载。
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
static const char* TARGET_EXE     = "PG2UK220.EXE";
static const DWORD  IMAGE_BASE    = 0x400000;
static const DWORD  ADDR_DRAWTEXT  = 0x43B2C2;   // 绘制文字（blit 入口）
static const DWORD  ADDR_MEASURE   = 0x43B7C4;   // 测量文字宽度
static const DWORD  ADDR_CHARWIDTH = 0x4287F3;   // 字符宽度查表（崩点所在）
static const DWORD  ADDR_SURFACE   = 0x53895C;   // dword_53895C（DrawSurface 本身）

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
typedef int   (__cdecl* OrigDrawTextFn)(int x, int y, const unsigned char* colorTable,
                                        const char* text, void* surface);
typedef short (__cdecl* OrigMeasureFn)(const char* text);
typedef int   (__cdecl* CharWidthFn)(DWORD fontRes, int ch);

static OrigDrawTextFn g_origDrawText = nullptr;
static OrigMeasureFn  g_origMeasure  = nullptr;
static CharWidthFn    g_charWidth    = nullptr;

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

// 单个字符的字形步进（像素）。测宽与绘制共用，保证两者一致。
static int GlyphAdvance(WCHAR wc)
{
    if (!g_fontOk) return g_cfg.fontHeight;
    MAT2 m = { {0,1},{0,0},{0,0},{0,1} };
    GLYPHMETRICS gm;
    if (GetGlyphOutlineW(g_hdc, wc, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m) != GDI_ERROR
        && gm.gmCellIncX > 0)
        return gm.gmCellIncX;
    return g_cfg.fontHeight;
}

// 解一个字符：GBK 双字节 / ASCII。返回消费的字节数（1 或 2）；孤立高位返回 1 且 wc=0。
static inline int DecodeChar(const char* text, int i, int len, WCHAR& wc)
{
    BYTE b = (BYTE)text[i];
    if (b >= 0x81 && i + 1 < len) {
        char db[3] = { text[i], text[i + 1], 0 };
        WCHAR wb[2] = { 0 };
        if (MultiByteToWideChar(936, 0, db, 2, wb, 1) > 0) { wc = wb[0]; return 2; }
        wc = L'?'; return 2;
    }
    if (b < 0x80) { wc = (WCHAR)b; return 1; }
    wc = 0; return 1;
}

// 把一个 Unicode 码点的字形（GGO_GRAY8 灰度掩码）写进 8bpp 帧缓冲。
static void DrawGlyph(WCHAR wc, int penX, int topY,
                      const unsigned char* colorTable, BYTE* pixels, int pitch, int surfH,
                      int clipX, int clipY, int& advOut)
{
    MAT2 m = { {0,1},{0,0},{0,0},{0,1} };
    GLYPHMETRICS gm;
    DWORD bufSize = GetGlyphOutlineW(g_hdc, wc, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
    advOut = (gm.gmCellIncX > 0) ? gm.gmCellIncX : g_cfg.fontHeight;
    if (bufSize == GDI_ERROR || bufSize == 0) return;

    std::vector<BYTE> buf(bufSize);
    if (GetGlyphOutlineW(g_hdc, wc, GGO_GRAY8_BITMAP, &gm, bufSize, buf.data(), &m) == GDI_ERROR)
        return;

    int mx = 0;
    for (DWORD i = 0; i < bufSize; ++i) if (buf[i] > mx) mx = buf[i];
    float scale = (mx > 1) ? (255.0f / mx) : 1.0f;

    int bufPitch = (gm.gmBlackBoxX + 3) & ~3;    // 行按 4 字节对齐
    int topX  = clipX + (penX - gm.gmptGlyphOrigin.x);
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
            pixels[(size_t)sy * pitch + sx] = pal;
        }
    }
}

// 整串自绘（含 CJK 时调用）。纯 ASCII 已在 Hook 中分流到原函数。
// 注意：绘图面是 sub_43B2C2 的第 5 个参数（blit 的 arg1），不是全局 0x53895C！
//       0x53895C 存的是「字体对象」指针：+8 = 行高，+0x10 = 字符偏移表。
static int RenderStringCJK(int x, int y, const unsigned char* colorTable,
                           const char* text, void* surface)
{
    if (!g_fontOk) return 0;

    DrawSurface* ds = (DrawSurface*)surface;
    if (!ds || !ds->H) return 0;
    SurfaceHeader* H = (SurfaceHeader*)ds->H;
    if (!H || !H->pixels) return 0;
    BYTE*  pixels = (BYTE*)H->pixels;
    int    pitch  = H->pitchMinus1 + 1;
    int    surfH  = H->heightMinus1 + 1;
    int    clipX  = ds->clipX;
    int    clipY  = ds->clipY;

    // 行高与原函数一致：*(0x53895C)->+8 + 2（取不到则退回字号+2）
    int lineAdv = g_cfg.fontHeight + 2;
    HMODULE hGame = GetModuleHandleA(TARGET_EXE);
    if (hGame) {
        DWORD fontObj = *(DWORD*)((BYTE*)hGame + (ADDR_SURFACE - IMAGE_BASE));
        if (fontObj) { int lh = *(int*)(fontObj + 8); if (lh > 0 && lh < 64) lineAdv = lh + 2; }
    }

    int curX = x, lines = 0;
    int len  = (int)strlen(text);
    for (int i = 0; i < len; )
    {
        WCHAR wc; int consumed = DecodeChar(text, i, len, wc);
        if (!wc) { i += consumed; continue; }

        if (wc == L'\n') { curX = x; y += lineAdv; ++lines; i += consumed; continue; }

        int adv = g_cfg.fontHeight;
        DrawGlyph(wc, curX, y, colorTable, pixels, pitch, surfH, clipX, clipY, adv);
        curX += adv;
        i += consumed;
    }
    return lines;   // 原函数返回换行计数（单行串为 0）
}

// ===================== 通用手动 Detour（纯 Win32） ==========================
#define MAX_DETOURS 8
struct Detour {
    BYTE* target;
    int   len;
    BYTE  orig[16];
    BYTE* tramp;
    void* origFn;
    bool  installed;
};
static Detour g_detours[MAX_DETOURS];
static int    g_detourCount = 0;

static bool WriteJmpChecked(BYTE* at, const void* dest)
{
    // 32 位 rel32 必须落在有符号 32 位内
    LONGLONG rel = (LONGLONG)((const BYTE*)dest) - (LONGLONG)(at + 5);
    if (rel < -0x80000000LL || rel > 0x7FFFFFFFLL) return false;
    at[0] = 0xE9;
    int32_t r32 = (int32_t)rel;
    memcpy(at + 1, &r32, 4);
    return true;
}

// 在 va（VA，基址 0x400000）处安装 len 字节 detour。成功返回跳板地址（可当原函数指针）。
static void* InstallDetour(DWORD va, int len)
{
    HMODULE hGame = GetModuleHandleA(TARGET_EXE);
    if (!hGame) { Log("detour %08X: 找不到 %s\n", va, TARGET_EXE); return nullptr; }
    BYTE* target = (BYTE*)hGame + (va - IMAGE_BASE);

    if (target[0] != 0x55) {
        Log("detour %08X: 入口首字节 %02X 非 0x55，跳过\n", va, target[0]);
        return nullptr;
    }
    if (g_detourCount >= MAX_DETOURS || len < 5 || len > 15) return nullptr;

    Detour& d = g_detours[g_detourCount];
    d.target = target; d.len = len; d.installed = false;
    d.tramp = (BYTE*)VirtualAlloc(nullptr, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!d.tramp) { Log("detour %08X: VirtualAlloc 失败\n", va); return nullptr; }

    memcpy(d.tramp, target, len);
    if (!WriteJmpChecked(d.tramp + len, target + len)) {
        Log("detour %08X: 跳板回跳超 2GB\n", va);
        VirtualFree(d.tramp, 0, MEM_RELEASE); d.tramp = nullptr; return nullptr;
    }
    memcpy(d.orig, target, len);

    DWORD op;
    if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &op)) {
        Log("detour %08X: VirtualProtect 失败\n", va);
        VirtualFree(d.tramp, 0, MEM_RELEASE); d.tramp = nullptr; return nullptr;
    }
    if (!WriteJmpChecked(target, d.tramp)) {
        VirtualProtect(target, len, op, &op);
        VirtualFree(d.tramp, 0, MEM_RELEASE); d.tramp = nullptr; return nullptr;
    }
    for (int i = 5; i < len; ++i) target[i] = 0x90;   // 多余字节填 NOP
    VirtualProtect(target, len, op, &op);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    FlushInstructionCache(GetCurrentProcess(), d.tramp, len + 5);

    d.origFn = d.tramp;
    d.installed = true;
    g_detourCount++;
    return d.tramp;
}

static void UninstallAll()
{
    for (int i = 0; i < g_detourCount; ++i) {
        Detour& d = g_detours[i];
        if (!d.installed) continue;
        DWORD op;
        if (VirtualProtect(d.target, d.len, PAGE_EXECUTE_READWRITE, &op)) {
            memcpy(d.target, d.orig, d.len);
            VirtualProtect(d.target, d.len, op, &op);
            FlushInstructionCache(GetCurrentProcess(), d.target, d.len);
        }
        if (d.tramp) { VirtualFree(d.tramp, 0, MEM_RELEASE); d.tramp = nullptr; }
        d.installed = false;
    }
    g_detourCount = 0;
}

// ===================== 各挂钩函数 ==========================================

// 绘制：纯 ASCII 走原函数；含 GBK 走本 DLL 合成
static int __cdecl HookDrawText(int x, int y, const unsigned char* colorTable,
                                const char* text, void* surface)
{
    if (!text) return g_origDrawText(x, y, colorTable, text, surface);
    bool hasCJK = false;
    for (const char* p = text; *p; ++p) { if ((BYTE)*p >= 0x81) { hasCJK = true; break; } }
    if (!hasCJK) return g_origDrawText(x, y, colorTable, text, surface);
    return RenderStringCJK(x, y, colorTable, text, surface);
}

// 测宽：纯 ASCII 走原函数；含 GBK 按字符累加字形步进（避免逐字节符号扩展越界）
static short __cdecl HookMeasureText(const char* text)
{
    if (!text) return 0;
    bool hasCJK = false;
    for (const char* p = text; *p; ++p) { if ((BYTE)*p >= 0x81) { hasCJK = true; break; } }
    if (!hasCJK || !g_origMeasure) return g_origMeasure ? g_origMeasure(text) : 0;

    int len = (int)strlen(text), w = 0;
    for (int i = 0; i < len; ) {
        WCHAR wc; int n = DecodeChar(text, i, len, wc);
        if (wc) w += GlyphAdvance(wc);
        i += n;
    }
    return (short)w;
}

// 字符宽度查表：高位字节兜底返回半宽（GBK 2 字节 → 全宽），防止负索引查表崩溃
static int __cdecl HookCharWidth(DWORD fontRes, int ch)
{
    if (ch < 0 || ch >= 0x80)
        return (g_cfg.fontHeight + 1) / 2;
    if (!g_charWidth || !fontRes) return 6;
    return g_charWidth(fontRes, ch);
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

        // 顺序：先字符查表（兜底），再测宽，再绘制
        g_charWidth    = (CharWidthFn)   InstallDetour(ADDR_CHARWIDTH, 5);
        g_origMeasure  = (OrigMeasureFn) InstallDetour(ADDR_MEASURE,   6);
        g_origDrawText = (OrigDrawTextFn)InstallDetour(ADDR_DRAWTEXT,  6);

        Log("PG2CHS: hook 状态 charwidth=%p measure=%p draw=%p\n",
            g_charWidth, g_origMeasure, g_origDrawText);

        if (!g_origDrawText) Log("PG2CHS: !! 绘制 hook 未装上，中文无法显示\n");
        else Log("PG2CHS: 已挂 sub_43B2C2，绘制生效\n");
        if (!g_origMeasure) Log("PG2CHS: !! 测宽 hook 未装上，含中文的串可能崩\n");
        if (!g_charWidth)   Log("PG2CHS: !! 字符查表 hook 未装上\n");
        break;
    }
    case DLL_PROCESS_DETACH:
        UninstallAll();
        if (g_hdc) DeleteDC(g_hdc);
        if (g_hfont) DeleteObject(g_hfont);
        if (g_log) { Log("PG2CHS: 卸载\n"); fclose(g_log); g_log = nullptr; }
        break;
    }
    return TRUE;
}
