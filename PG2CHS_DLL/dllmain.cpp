// dllmain.cpp — 装甲元帅2（PG2UK220.EXE）简体中文汉化 ASI
// ============================================================================
// 渲染管线（IDA 反汇编 + 两次真机崩溃转储确认）
// ----------------------------------------------------------------------------
// 1) 游戏文字不走 GDI，而是把字形位图逐像素写进一个 8 位调色板表面。
//    · 0x428813  blit：int __cdecl(surface*, x, y, fontObj, glyphByte, colorTable*)
//        surface = 参数1，结构 { H, clipX, clipY, maxX, maxY }
//        H       = { BYTE* pixels; int pitchMinus1; int heightMinus1; }
//        dst     = pixels + (y + clipY) * pitch + (x + clipX)      ← 已实测
//        colorTable 非空时：pal = colorTable[srcByte]; pal==0xFF 则透明
//    · 0x43B2C2  绘制：short __cdecl(x, y, colorTable*, text, surface*)
//        先 strlen，再 LockSurface()，逐字节 blit，最后 UnlockSurface()
//        换行：x 复位为初始 x，y += *(int*)(fontObj+8) + 2
//        逐字节推进：x += CharWidth(fontObj, (signed char)byte)
//    · 0x43B7C4  测宽：short __cdecl(const char* text)
//        逐字节 movsx 后调 CharWidth → GBK 高位字节负索引越界 → 崩溃根因
//    · 0x4287F3  字符宽度查表：short __cdecl(fontObj, ch)
//        value = *(int*)(fontObj + 0x10 + ch*4) + fontObj，再取该处 int
//    · 0x53895C  全局「当前字体对象」指针：+8 = 行高，+0x10 = 字形偏移表
//    · 0x538F40  256 字节颜色表：初始化 memset(0xFF)，仅 [0] = 当前文字色
//                （由 0x43B9AF SetFontColor 写入，初值 0xE3）
//                → 0xFF 表示透明；可见像素的调色板索引就是 colorTable[0]
//    · 0x45197D / 0x451A1F  表面 Lock / Unlock（DirectDraw，vtable +0x64/+0x80）
//                锁定标志在 0x4AC6F0。cnc-ddraw 下 Unlock 才真正提交像素，
//                所以合成 CJK 时必须自己成对调用。
//
// 2) 本 DLL 用「纯 Win32 手动 detour」（无 MinHook / NuGet），挂 3 个点：
//    · 0x4287F3  字符查表：高位字节兜底返回半宽，防止其它调用点负索引崩溃
//    · 0x43B7C4  测宽：含 GBK 时按字符累加 GDI 字形步进
//    · 0x43B2C2  绘制：纯 ASCII 走跳板调原函数（零回归）；
//                      含 GBK 时用 GDI GetGlyphOutline 取字形，
//                      经 Lock/Unlock 后直接写调色板索引 colorTable[0]
//
// 3) 部署：PG2CHS_DLL.dll 改名 PG2CHS_DLL.asi，由 dist/common/winmm.dll
//    (Ultimate ASI Loader) 载入；同目录可放 PG2CHS.ini 覆盖配置。
// ============================================================================

#include "pch.h"
#include <vector>
#include <string>

// ===================== 配置（可用同目录 PG2CHS.ini 覆盖） =====================
static struct Cfg {
    std::wstring fontName  = L"Microsoft YaHei"; // CJK 字体（依次回退 SimSun 等）
    int   fontHeight       = 0;    // 字号(px)；0 = 自动取游戏字体行高
    int   vOffset          = 0;    // 垂直微调（正=下，负=上）
    int   threshold        = 0x60; // 字形覆盖率阈值 0..255（越大字越细）
    bool  debug            = false;// 是否写详细日志
} g_cfg;

// ===================== 关键地址（VA，ImageBase = 0x400000） =================
static const char* TARGET_EXE      = "PG2UK220.EXE";
static const DWORD IMAGE_BASE      = 0x400000;
static const DWORD ADDR_DRAWTEXT   = 0x43B2C2;  // 绘制文字
static const DWORD ADDR_MEASURE    = 0x43B7C4;  // 测量文字宽度
static const DWORD ADDR_CHARWIDTH  = 0x4287F3;  // 字符宽度查表
static const DWORD ADDR_BLIT       = 0x428813;  // 单字形 blit（兜底闸门）
static const DWORD ADDR_FONTOBJ    = 0x53895C;  // 当前字体对象指针
static const DWORD ADDR_LOCK       = 0x45197D;  // 表面 Lock
static const DWORD ADDR_UNLOCK     = 0x451A1F;  // 表面 Unlock
static const DWORD ADDR_LOCKFLAG   = 0x4AC6F0;  // 已锁定标志（byte）

// ===================== 帧缓冲面结构（实测布局） =============================
#pragma pack(push, 4)
struct DrawSurface {            // blit 的参数 1（指针）
    void*  H;                   // [0] -> SurfaceHeader*
    int    clipX;               // [1] 加到位图 x 上
    int    clipY;               // [2] 加到位图 y 上
    int    maxX;                // [3] 右边界（含）
    int    maxY;                // [4] 下边界（含）
};
struct SurfaceHeader {          // *H
    void*  pixels;              // [0] 像素基址（BYTE*，8bpp）
    int    pitchMinus1;         // [1] pitch = +1
    int    heightMinus1;        // [2] height = +1
};
#pragma pack(pop)

// ===================== 原函数类型 / 跳板 ====================================
typedef int   (__cdecl* OrigDrawTextFn)(int x, int y, const unsigned char* colorTable,
                                        const char* text, void* surface);
typedef short (__cdecl* OrigMeasureFn)(const char* text);
typedef int   (__cdecl* CharWidthFn)(DWORD fontObj, int ch);
typedef int   (__cdecl* BlitFn)(DrawSurface* surface, int x, int y, DWORD fontObj,
                                int glyphIdx, const unsigned char* colorTable);
typedef char  (__cdecl* LockFn)(void);
typedef void  (__cdecl* UnlockFn)(void);

static OrigDrawTextFn g_origDrawText = nullptr;
static OrigMeasureFn  g_origMeasure  = nullptr;
static CharWidthFn    g_charWidth    = nullptr;
static BlitFn         g_origBlit     = nullptr;
static LockFn         g_lockFn       = nullptr;
static UnlockFn       g_unlockFn     = nullptr;
static BYTE*          g_pLockFlag    = nullptr;

// 三个钩子（定义在下方，安装 detour 时要取地址）
static int   __cdecl HookCharWidth(DWORD fontObj, int ch);
static short __cdecl HookMeasureText(const char* text);
static int   __cdecl HookDrawText(int x, int y, const unsigned char* colorTable,
                                  const char* text, void* surface);
static int   __cdecl HookBlit(DrawSurface* surface, int x, int y, DWORD fontObj,
                              int glyphIdx, const unsigned char* colorTable);

// ===================== GDI 字形渲染状态 =====================================
static HDC    g_hdc       = nullptr;   // 离屏 DC（GetGlyphOutline 用）
static HFONT  g_hfont     = nullptr;   // CJK 字体
static bool   g_fontOk    = false;     // 字体是否就绪
static int    g_pxHeight  = 0;         // 实际使用的字号(px)
static DWORD  g_fontObj   = 0;         // 游戏当前字体对象（0x53895C）
static int    g_lineH     = 0;         // 游戏字体行高（fontObj+8）

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
    g_cfg.threshold  = GetPrivateProfileIntW(L"PG2CHS", L"Threshold",  g_cfg.threshold,  iniPath);
    g_cfg.debug      = GetPrivateProfileIntW(L"PG2CHS", L"Debug",      g_cfg.debug ? 1 : 0, iniPath) != 0;
    if (g_cfg.threshold < 1)   g_cfg.threshold = 1;
    if (g_cfg.threshold > 254) g_cfg.threshold = 254;
}

// 取游戏当前字体对象（0x53895C）
static DWORD GetGameFontObj()
{
    HMODULE h = GetModuleHandleA(TARGET_EXE);
    if (!h) return 0;
    return *(DWORD*)((BYTE*)h + (ADDR_FONTOBJ - IMAGE_BASE));
}

// 实际字号：优先配置；否则用游戏字体行高
static int EffectiveHeight()
{
    if (g_cfg.fontHeight > 0) return g_cfg.fontHeight;
    if (g_lineH > 0) return g_lineH;
    return 16;
}

// 懒初始化 GDI 字体。必须延迟到第一次真正画 CJK 时做：
// DllMain 阶段游戏还没加载字体，读不到行高，字号会猜错。
static bool EnsureFont()
{
    if (g_fontOk) return true;
    if (!g_hdc) {
        g_hdc = CreateCompatibleDC(nullptr);
        if (!g_hdc) { Log("font: CreateCompatibleDC 失败\n"); return false; }
    }

    g_fontObj = GetGameFontObj();
    if (g_fontObj) {
        int lh = *(int*)(g_fontObj + 8);
        if (lh > 0 && lh < 80) g_lineH = lh;
    }
    g_pxHeight = EffectiveHeight();

    const wchar_t* cands[] = { g_cfg.fontName.c_str(), L"SimSun",
                               L"Microsoft YaHei UI", L"Microsoft YaHei" };
    MAT2 m = { {0,1},{0,0},{0,0},{0,1} };
    for (const wchar_t* name : cands)
    {
        HFONT f = CreateFontW(-g_pxHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, name);
        if (!f) continue;
        HFONT old = (HFONT)SelectObject(g_hdc, f);
        GLYPHMETRICS gm;
        DWORD rv = GetGlyphOutlineW(g_hdc, L'\u56FD', GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
        bool ok = (rv != GDI_ERROR && rv > 0);
        if (old) SelectObject(g_hdc, old);
        if (ok) {
            if (g_hfont) DeleteObject(g_hfont);
            g_hfont = f; g_fontOk = true;
            Log("font: %ls  %dpx  (游戏行高=%d, 阈值=%d)\n",
                name, g_pxHeight, g_lineH, g_cfg.threshold);
            return true;
        }
        DeleteObject(f);
    }
    Log("font: 未找到可用 TrueType CJK 字体，CJK 将跳过绘制\n");
    return false;
}

// 单个字符的字形步进（像素）。测宽与绘制共用，保证两者一致。
static int GlyphAdvance(WCHAR wc)
{
    if (!g_fontOk) return g_pxHeight > 0 ? g_pxHeight : 16;
    MAT2 m = { {0,1},{0,0},{0,0},{0,1} };
    GLYPHMETRICS gm;
    if (GetGlyphOutlineW(g_hdc, wc, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m) != GDI_ERROR
        && gm.gmCellIncX > 0)
        return gm.gmCellIncX;
    return g_pxHeight > 0 ? g_pxHeight : 16;
}

// 解一个字符：GBK 双字节 / ASCII。返回消费字节数（1 或 2）；孤立高位返回 1 且 wc=0。
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

// 画一个字形。ink = 当前文字色（colorTable[0]）；
// 覆盖率 >= 阈值 的像素写成 ink，其余保持原样（透明）。
static void DrawGlyph(WCHAR wc, int penX, int topY, BYTE ink,
                      BYTE* pixels, int pitch, int surfH, int clipX, int clipY, int& advOut)
{
    MAT2 m = { {0,1},{0,0},{0,0},{0,1} };
    GLYPHMETRICS gm;
    DWORD bufSize = GetGlyphOutlineW(g_hdc, wc, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
    advOut = (gm.gmCellIncX > 0) ? gm.gmCellIncX : (g_pxHeight > 0 ? g_pxHeight : 16);
    if (bufSize == GDI_ERROR || bufSize == 0) return;

    std::vector<BYTE> buf(bufSize);
    if (GetGlyphOutlineW(g_hdc, wc, GGO_GRAY8_BITMAP, &gm, bufSize, buf.data(), &m) == GDI_ERROR)
        return;

    int bufPitch = (int)((gm.gmBlackBoxX + 3) & ~3u);            // 行 4 字节对齐
    int topX  = clipX + penX - (int)gm.gmptGlyphOrigin.x;
    int baseY = clipY + topY;

    for (int r = 0; r < (int)gm.gmBlackBoxY; ++r)
    {
        int sy = baseY + r;
        if (sy < 0 || sy >= surfH) continue;
        const BYTE* row = buf.data() + (size_t)r * bufPitch;
        BYTE* dstRow = pixels + (size_t)sy * pitch;
        for (int c = 0; c < (int)gm.gmBlackBoxX; ++c)
        {
            if (row[c] < (BYTE)g_cfg.threshold) continue;
            int sx = topX + c;
            if (sx < 0 || sx >= pitch) continue;
            dstRow[sx] = ink;
        }
    }
}

// 整串自绘（含 CJK 时调用）。纯 ASCII 已在 HookDrawText 中分流到原函数。
static int RenderStringCJK(int x, int y, const unsigned char* colorTable,
                           const char* text, void* surface)
{
    if (!EnsureFont()) return 0;

    DrawSurface* ds = (DrawSurface*)surface;
    if (!ds || !ds->H) return 0;
    SurfaceHeader* H = (SurfaceHeader*)ds->H;
    if (!H || !H->pixels) return 0;

    // 文字色：colorTable[0]（0xFF 表示未设色 → 用默认 0xE3）
    BYTE ink = 0xE3;
    if (colorTable) {
        BYTE c = colorTable[0];
        if (c != 0xFF) ink = c;
    }

    // 与原函数一致：先锁表面，画完解锁。cnc-ddraw 下 Unlock 才提交像素。
    bool weLocked = false;
    if (g_lockFn && g_pLockFlag && !*g_pLockFlag) {
        if (!g_lockFn()) return 0;      // 显示未就绪 → 原函数同样直接返回
        weLocked = true;
    }

    BYTE* pixels = (BYTE*)H->pixels;
    int   pitch  = H->pitchMinus1 + 1;
    int   surfH  = H->heightMinus1 + 1;
    int   clipX  = ds->clipX;
    int   clipY  = ds->clipY;

    // 行高：fontObj+8 + 2（与原函数 0x43B3AA 一致）
    int lineAdv = (g_lineH > 0 ? g_lineH : EffectiveHeight()) + 2;
    int curX = x, lines = 0;
    int len  = (int)strlen(text);

    for (int i = 0; i < len; )
    {
        WCHAR wc; int n = DecodeChar(text, i, len, wc);
        if (!wc) { i += n; continue; }

        if (wc == L'\n') { curX = x; y += lineAdv; ++lines; i += n; continue; }

        int adv = 0;
        if (wc < 0x20) {                 // 控制字符（原函数也不画）
            adv = GlyphAdvance(wc);
        } else {
            DrawGlyph(wc, curX, y + g_cfg.vOffset, ink,
                      pixels, pitch, surfH, clipX, clipY, adv);
        }
        curX += adv;
        i += n;
    }

    if (weLocked && g_unlockFn) g_unlockFn();
    return lines;   // 原函数返回换行计数（单行串为 0）
}

// ===================== 通用手动 Detour（纯 Win32） ==========================
#define MAX_DETOURS 8
struct Detour {
    BYTE* target;
    int   len;
    BYTE  orig[16];
    BYTE* tramp;
    bool  installed;
};
static Detour g_detours[MAX_DETOURS];
static int    g_detourCount = 0;

static bool WriteJmpChecked(BYTE* at, const void* dest)
{
    LONGLONG rel = (LONGLONG)((const BYTE*)dest) - (LONGLONG)(at + 5);
    if (rel < -0x80000000LL || rel > 0x7FFFFFFFLL) return false;
    at[0] = 0xE9;
    int32_t r32 = (int32_t)rel;
    memcpy(at + 1, &r32, 4);
    return true;
}

// 在 va 处安装 len 字节 detour：
//   入口 = E9 <hookFn>           ← 必须跳钩子函数
//   跳板 = 原 len 字节 + E9 回跳   ← 作为「原函数」指针交给钩子回调
// 注意：入口绝不能跳跳板，否则等于没拦，钩子永不执行（曾因此白跑两轮）。
static void* InstallDetour(DWORD va, int len, const BYTE* expect, void* hookFn)
{
    HMODULE hGame = GetModuleHandleA(TARGET_EXE);
    if (!hGame) { Log("detour %08X: 找不到 %s\n", va, TARGET_EXE); return nullptr; }
    BYTE* target = (BYTE*)hGame + (va - IMAGE_BASE);

    if (g_detourCount >= MAX_DETOURS || len < 5 || len > 15) return nullptr;

    // 版本闸门：prologue 必须与预期一致（同时保证 relocate 落在指令边界）
    if (memcmp(target, expect, len) != 0) {
        Log("detour %08X: prologue 不符\n   实际 ", va);
        for (int i = 0; i < len; ++i) Log("%02X ", target[i]);
        Log("\n   预期 ");
        for (int i = 0; i < len; ++i) Log("%02X ", expect[i]);
        Log("\n   → 跳过（EXE 版本不符）\n");
        return nullptr;
    }

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
    if (!WriteJmpChecked(target, hookFn)) {           // ← 跳钩子函数（不是跳板）
        VirtualProtect(target, len, op, &op);
        VirtualFree(d.tramp, 0, MEM_RELEASE); d.tramp = nullptr; return nullptr;
    }
    for (int i = 5; i < len; ++i) target[i] = 0x90;   // 多余字节填 NOP
    VirtualProtect(target, len, op, &op);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    FlushInstructionCache(GetCurrentProcess(), d.tramp, len + 5);

    d.installed = true;
    g_detourCount++;
    Log("detour %08X: OK  入口->%p  跳板=%p\n", va, hookFn, d.tramp);
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
static int g_dbgDraw = 0, g_dbgMeasure = 0, g_dbgCharW = 0;

// 绘制：纯 ASCII 走原函数（零回归）；含 GBK 走本 DLL 合成
static int __cdecl HookDrawText(int x, int y, const unsigned char* colorTable,
                                const char* text, void* surface)
{
    if (!text) return g_origDrawText(x, y, colorTable, text, surface);
    bool hasCJK = false;
    for (const char* p = text; *p; ++p) { if ((BYTE)*p >= 0x81) { hasCJK = true; break; } }
    if (!hasCJK) return g_origDrawText(x, y, colorTable, text, surface);

    if (g_cfg.debug && g_dbgDraw < 5) {
        Log("draw CJK #%d (%d,%d) surface=%p ink=%02X text=", g_dbgDraw, x, y, surface,
            colorTable ? colorTable[0] : 0xFF);
        for (int k = 0; k < 20 && text[k]; ++k) Log("%02X ", (BYTE)text[k]);
        Log("\n");
    }
    ++g_dbgDraw;
    return RenderStringCJK(x, y, colorTable, text, surface);
}

// 测宽：纯 ASCII 走原函数；含 GBK 按字符累加字形步进
static short __cdecl HookMeasureText(const char* text)
{
    if (!text) return 0;
    bool hasCJK = false;
    for (const char* p = text; *p; ++p) { if ((BYTE)*p >= 0x81) { hasCJK = true; break; } }
    if (!hasCJK) return g_origMeasure ? g_origMeasure(text) : 0;

    if (!EnsureFont()) return 0;

    int len = (int)strlen(text), w = 0;
    for (int i = 0; i < len; ) {
        WCHAR wc; int n = DecodeChar(text, i, len, wc);
        if (wc) w += GlyphAdvance(wc);
        i += n;
    }
    if (g_cfg.debug && g_dbgMeasure < 5)
        Log("measure CJK #%d len=%d -> %d\n", g_dbgMeasure, len, w);
    ++g_dbgMeasure;
    return (short)w;
}

// 字符宽度查表：高位字节兜底返回半宽（GBK 2 字节 ≈ 全宽），防止负索引崩溃
static int __cdecl HookCharWidth(DWORD fontObj, int ch)
{
    if (ch < 0 || ch >= 0x80) {
        if (g_cfg.debug && g_dbgCharW < 5)
            Log("charwidth hi ch=%d font=%08X\n", ch, fontObj);
        ++g_dbgCharW;
        int h = (g_cfg.fontHeight > 0) ? g_cfg.fontHeight : (g_lineH > 0 ? g_lineH : 16);
        return (h + 1) / 2;
    }
    if (!g_charWidth || !fontObj) return 6;
    return g_charWidth(fontObj, ch);
}

// 单字形 blit 兜底闸门：
// 字形索引来自 movsx（有符号字节），GBK 高位字节会变成负数，
// 原函数会用 fontObj + 0x10 + idx*4 反向越界读偏移表 → 读到垃圾指针再解引用崩溃。
// 这里只挡负数（真正的崩溃源）；0x80+ 的正索引仍放行，避免误伤游戏自带的高位字形。
static int __cdecl HookBlit(DrawSurface* surface, int x, int y, DWORD fontObj,
                            int glyphIdx, const unsigned char* colorTable)
{
    if (glyphIdx < 0) return 0;
    if (!g_origBlit) return 0;
    return g_origBlit(surface, x, y, fontObj, glyphIdx, colorTable);
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
        Log("PG2CHS: 附加 (hMod=%p)\n", hModule);

        HMODULE hGame = GetModuleHandleA(TARGET_EXE);
        if (hGame) {
            g_lockFn    = (LockFn)  ((BYTE*)hGame + (ADDR_LOCK     - IMAGE_BASE));
            g_unlockFn  = (UnlockFn)((BYTE*)hGame + (ADDR_UNLOCK   - IMAGE_BASE));
            g_pLockFlag = (BYTE*)hGame + (ADDR_LOCKFLAG - IMAGE_BASE);
        } else {
            Log("PG2CHS: !! 找不到 %s，跳过\n", TARGET_EXE);
            break;
        }

        // prologue 期望值（实测 PG2UK220.EXE；同时保证 relocate 落在指令边界）
        static const BYTE kCharWidthPro[5] = { 0x55, 0x8B, 0xEC, 0x53, 0x56 };          // push ebp;mov ebp,esp;push ebx;push esi
        static const BYTE kMeasurePro[6]   = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };    // + sub esp,0Ch
        static const BYTE kDrawPro[6]      = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 };    // + sub esp,10h
        static const BYTE kBlitPro[6]      = { 0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xD0 };    // + add esp,-30h

        // 顺序：先字符查表（兜底），再测宽，再绘制，最后 blit 闸门
        g_charWidth    = (CharWidthFn)   InstallDetour(ADDR_CHARWIDTH, 5, kCharWidthPro, (void*)&HookCharWidth);
        g_origMeasure  = (OrigMeasureFn) InstallDetour(ADDR_MEASURE,   6, kMeasurePro,   (void*)&HookMeasureText);
        g_origDrawText = (OrigDrawTextFn)InstallDetour(ADDR_DRAWTEXT,  6, kDrawPro,      (void*)&HookDrawText);
        g_origBlit     = (BlitFn)        InstallDetour(ADDR_BLIT,      6, kBlitPro,      (void*)&HookBlit);

        Log("PG2CHS: hook 状态 charwidth=%p measure=%p draw=%p\n",
            g_charWidth, g_origMeasure, g_origDrawText);
        if (!g_origDrawText) Log("PG2CHS: !! 绘制 hook 未装上，中文无法显示\n");
        if (!g_origMeasure)  Log("PG2CHS: !! 测宽 hook 未装上，含中文的串可能崩\n");
        if (!g_charWidth)    Log("PG2CHS: !! 字符查表 hook 未装上\n");
        break;
    }
    case DLL_PROCESS_DETACH:
        UninstallAll();
        if (g_hfont) DeleteObject(g_hfont);
        if (g_hdc) DeleteDC(g_hdc);
        if (g_log) { Log("PG2CHS: 卸载\n"); fclose(g_log); g_log = nullptr; }
        break;
    }
    return TRUE;
}
