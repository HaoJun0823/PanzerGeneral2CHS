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
//    · 0x53895C  全局「当前字体对象」指针：+8 = 字形高度（本作 = 8px，行距 = 8+2）
//                +0x10 = 字形偏移表；字形记录 = { int width; BYTE data[w*h]; }
//    · 0x538F40  256 字节颜色表：初始化 memset(0xFF)，仅 [0] = 当前文字色
//                （由 0x43B9AF SetFontColor 写入，初值 0xE3）
//                → 0xFF 表示透明；可见像素的调色板索引就是 colorTable[0]
//    · 0x5155B0  表面头（由 Lock 填充）：{ lpSurface, lPitch-1, dwHeight-1 }
//                Lock 同时把 0x4AC6F0 置 1；只在这段窗口内像素指针有效
//    · 0x45197D / 0x451A1F  表面 Lock / Unlock（DirectDraw，vtable +0x64/+0x80）
//                锁定标志在 0x4AC6F0。cnc-ddraw 下 Unlock 才真正提交像素，
//                所以合成 CJK 时必须自己成对调用。
//
// 2) 本 DLL 用「纯 Win32 手动 detour」（无 MinHook / NuGet），挂 4 个点：
//    · 0x4287F3  字符查表：高位字节兜底返回半宽，防止其它调用点负索引崩溃
//    · 0x43B7C4  测宽：含 GBK 时按字符累加 GDI 字形步进
//    · 0x43B2C2  绘制：纯 ASCII 走跳板调原函数（零回归）；
//                      含 GBK 时用 GDI GetGlyphOutline 取字形，
//                      经 Lock/Unlock 后直接写调色板索引 colorTable[0]
//    · 0x428813  blit 兜底：字形索引为负（GBK 高位被 movsx 的必然结果）直接跳过
//
// 3) 字体尺寸：游戏自带位图字体只有 8px 高（fontObj+8），行距 8+2=10。
//    8px 画汉字是一团糊，故 CJK 默认 12px（可用 PG2CHS.ini 的 FontHeight 覆盖），
//    字形走 GGO_BITMAP（1bpp，宋体在 12px 有内置点阵 → 最清晰，贴近原作风）。
//    注意 GGO_GRAY8_BITMAP 的值域是 0..64（非 0..255），阈值必须先归一化。
//
// 4) 部署：PG2CHS_DLL.dll 改名 PG2CHS_DLL.asi，由 dist/common/winmm.dll
//    (Ultimate ASI Loader) 载入；同目录可放 PG2CHS.ini 覆盖配置。
// ============================================================================

#include "pch.h"
#include <intrin.h>
#include <vector>
#include <string>

// ===================== 配置（可用同目录 PG2CHS.ini 覆盖） =====================
static struct Cfg {
    std::wstring fontName  = L"SimSun";   // CJK 字体（依次回退 微软雅黑 等）
    int   fontHeight       = 0;    // 字号(px)；0 = 自动（游戏字体仅 8px，CJK 取 12px）
    int   vOffset          = 0;    // 垂直微调（正=下，负=上）
    int   threshold        = 0x60; // 仅 AntiAlias=1 时有效：笔画阈值 0..255（越大越细）
    bool  antiAlias        = false;// false=GGO_BITMAP 点阵（12px 更清晰，贴近原作风）
                                   // true =GGO_GRAY8 灰度 + 阈值（大字号更平滑）
    bool  debug            = false;// 是否写详细日志
    bool  dumpFrame        = false;// 首次画 CJK 时把整个表面 dump 成 PG2CHS_frame.pgm（排查用）
    bool  selfTest         = false;// 首次绘制时在画面上画一段中文测试换行渲染（排查用）
} g_cfg;

// ===================== 关键地址（VA，ImageBase = 0x400000） =================
static const char* TARGET_EXE      = "PG2UK220.EXE";
static const DWORD IMAGE_BASE      = 0x400000;
static const DWORD ADDR_DRAWTEXT   = 0x43B2C2;  // 绘制文字
static const DWORD ADDR_WRAPDRAW   = 0x43B402;  // 自动换行绘制（剧本简报/帮助）
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
// 自动换行绘制：maxLines 限制行数，flag 非 0 时末尾补 '|' 表示还有更多内容
typedef short (__cdecl* OrigWrapFn)(short maxLines, const unsigned char* colorTable,
                                    const char* text, void* surface, char flag);
typedef int   (__cdecl* CharWidthFn)(DWORD fontObj, int ch);
typedef int   (__cdecl* BlitFn)(DrawSurface* surface, int x, int y, DWORD fontObj,
                                int glyphIdx, const unsigned char* colorTable);
typedef char  (__cdecl* LockFn)(void);
typedef void  (__cdecl* UnlockFn)(void);

static OrigDrawTextFn g_origDrawText = nullptr;
static OrigMeasureFn  g_origMeasure  = nullptr;
static OrigWrapFn     g_origWrap     = nullptr;
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
static int    g_ascent    = 0;         // 字体基线以上的高度（TEXTMETRIC.tmAscent）
static DWORD  g_fontObj   = 0;         // 游戏当前字体对象（0x53895C）
static int    g_lineH     = 0;         // 游戏字体字形高度（fontObj+8，本作 = 8）
static int    g_dbgDrawLog = 0;        // 已打印的绘制诊断条数

// ===================== 日志 ================================================
static FILE* g_log = nullptr;
static wchar_t g_baseDir[MAX_PATH] = { 0 };   // DLL 所在目录（日志/快照都放这里）
static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// ===================== 文本索引表修复 =======================================
// 内存取证结论（英文/中文两次运行对比）：
//   游戏把 GUI97.TXT 读进内存时逐字节过滤，遇到「字体里没有的字节」就写 0，
//   于是一行中文被切成好几段，表项数量变多；
//   而 UI 取串用的索引来自数据记录（36 字节记录 +0x0C 的 16 位序号），
//   是按英文原版「第 k 项 = 第 k+1 行」编好的 —— 索引整体后移 → 按钮串词。
// 对策：把这张固定 432 项的表直接换成本文件「第 k+1 行」的原始 GBK 串。
static const DWORD ADDR_TEXTTABLE  = 0x4EFEC0;  // char* g_text[432]
static const int   TEXTTABLE_COUNT = 432;       // 表长（实测 0x4EFEC0..0x4F0580）

static char* g_tblPtr[TEXTTABLE_COUNT] = { nullptr };
static char* g_tblPool = nullptr;
static bool  g_tblTried = false;

// 读 GUI97.TXT，按 CRLF 切行，全部放进一块池子（游戏不会释放这些指针）
static bool BuildTextPool()
{
    if (g_tblTried) return g_tblPool != nullptr;
    g_tblTried = true;

    wchar_t path[MAX_PATH * 2];
    wcscpy_s(path, g_baseDir); wcscat_s(path, L"GUI97.TXT");
    FILE* fp = _wfopen(path, L"rb");
    if (!fp) {
        wcscpy_s(path, g_baseDir); wcscat_s(path, L"..\\GUI97.TXT");
        fp = _wfopen(path, L"rb");
    }
    if (!fp) { Log("texttable: 找不到 GUI97.TXT\n"); return false; }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) { fclose(fp); return false; }

    std::vector<char> raw((size_t)sz);
    size_t got = fread(raw.data(), 1, (size_t)sz, fp);
    fclose(fp);
    if (got < 8) return false;

    g_tblPool = (char*)HeapAlloc(GetProcessHeap(), 0, got + TEXTTABLE_COUNT + 8);
    if (!g_tblPool) return false;

    const char* p = raw.data();
    const char* end = raw.data() + got;
    char* w = g_tblPool;
    int n = 0;
    while (p < end && n < TEXTTABLE_COUNT) {
        const char* q = p;
        while (q < end && *q != '\r' && *q != '\n') ++q;
        int len = (int)(q - p);
        memcpy(w, p, (size_t)len);
        w[len] = 0;
        g_tblPtr[n++] = w;
        w += len + 1;
        while (q < end && (*q == '\r' || *q == '\n')) ++q;
        p = q;
    }
    Log("texttable: 读入 %d 行\n", n);
    return n > 0;
}

// 每次绘制前调用：只有发现表被别人重写时才动手（正常情况下一次比较就返回）
static void PatchTextTable()
{
    HMODULE h = GetModuleHandleA(TARGET_EXE);
    if (!h) return;
    char** tbl = (char**)((BYTE*)h + (ADDR_TEXTTABLE - IMAGE_BASE));

    if (g_tblPool && tbl[0] == g_tblPtr[0]) return;   // 已是我们的表
    if (!BuildTextPool()) return;

    int changed = 0;
    for (int i = 0; i < TEXTTABLE_COUNT; ++i) {
        // 只替换「游戏自己已经填过」的项：表的空位有语义（迭代到 NULL 结束），
        // 全填满会让上层遍历跑出预期范围，表现为菜单整个不出来。
        if (tbl[i] && g_tblPtr[i] && tbl[i] != g_tblPtr[i]) { tbl[i] = g_tblPtr[i]; ++changed; }
    }
    static int logged = 0;
    if (changed && logged < 3) {
        Log("texttable: 换掉 %d 项；#156=%s\n", changed, g_tblPtr[156] ? g_tblPtr[156] : "(null)");
        ++logged;
    }
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
    g_cfg.antiAlias  = GetPrivateProfileIntW(L"PG2CHS", L"AntiAlias",  g_cfg.antiAlias ? 1 : 0, iniPath) != 0;
    g_cfg.debug      = GetPrivateProfileIntW(L"PG2CHS", L"Debug",      g_cfg.debug ? 1 : 0, iniPath) != 0;
    g_cfg.dumpFrame  = GetPrivateProfileIntW(L"PG2CHS", L"DumpFrame",  g_cfg.dumpFrame ? 1 : 0, iniPath) != 0;
    g_cfg.selfTest   = GetPrivateProfileIntW(L"PG2CHS", L"SelfTest",   g_cfg.selfTest  ? 1 : 0, iniPath) != 0;
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

// 实际字号：优先配置；否则自动。
// 游戏自带位图字体只有 8px 高（fontObj+8），直接按 8px 画 CJK 是一团糊，
// 所以自动档下限取 12px；行距仍按游戏节奏（见 RenderStringCJK 的 lineAdv）。
static int EffectiveHeight()
{
    if (g_cfg.fontHeight > 0) return g_cfg.fontHeight;
    if (g_lineH >= 12) return g_lineH;
    return 12;
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
        UINT fmt = g_cfg.antiAlias ? GGO_GRAY8_BITMAP : GGO_BITMAP;
        DWORD rv = GetGlyphOutlineW(g_hdc, L'\u56FD', fmt, &gm, 0, nullptr, &m);
        bool ok = (rv != GDI_ERROR && rv > 0);
        TEXTMETRICW tm = { 0 };
        if (ok) GetTextMetricsW(g_hdc, &tm);
        if (ok) {
            // 关键：字体必须常驻 g_hdc。此前这里把 old 选回去，
            // 导致后续 GetGlyphOutlineW 用的还是默认系统字体（无 CJK 字形）→ 永远画不出。
            if (g_hfont && g_hfont != f) DeleteObject(g_hfont);
            g_hfont = f; g_fontOk = true;
            g_ascent = tm.tmAscent > 0 ? tm.tmAscent : (g_pxHeight * 4 / 5);
            Log("font: %ls  %dpx  ascent=%d  游戏字形高=%d  模式=%s\n",
                name, g_pxHeight, g_ascent, g_lineH,
                g_cfg.antiAlias ? "gray8" : "1bpp");
            return true;
        }
        if (old) SelectObject(g_hdc, old);
        DeleteObject(f);
    }
    Log("font: 未找到可用 TrueType CJK 字体，CJK 将跳过绘制\n");
    return false;
}

// 单个字符的字形步进（像素）。测宽与绘制共用，保证两者一致。
static int GlyphAdvance(WCHAR wc)
{
    int fb = g_pxHeight > 0 ? g_pxHeight : 12;
    if (!g_fontOk) return fb;
    MAT2 m = { {0,1},{0,0},{0,0},{0,1} };
    UINT fmt = g_cfg.antiAlias ? GGO_GRAY8_BITMAP : GGO_BITMAP;
    GLYPHMETRICS gm = { 0 };
    if (GetGlyphOutlineW(g_hdc, wc, fmt, &gm, 0, nullptr, &m) != GDI_ERROR
        && gm.gmCellIncX > 0)
        return gm.gmCellIncX;
    return fb;
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

// 取一个字的字形位图。两种格式：
//   GGO_BITMAP(1)      每像素 1 bit，行按 32bit 对齐 —— 12px 宋体走字体自带点阵，最清晰
//   GGO_GRAY8_BITMAP(6) 每像素 8 bit，但值域是 0..64（不是 0..255！），大字号更平滑
// 返回值 = 缓冲区字节数（0 表示无位图，但 gm 仍有效，可拿 gmCellIncX）
static DWORD FetchGlyph(WCHAR wc, std::vector<BYTE>& buf, GLYPHMETRICS& gm)
{
    MAT2 m = { {0,1},{0,0},{0,0},{0,1} };
    UINT fmt = g_cfg.antiAlias ? GGO_GRAY8_BITMAP : GGO_BITMAP;
    DWORD size = GetGlyphOutlineW(g_hdc, wc, fmt, &gm, 0, nullptr, &m);
    if (size == GDI_ERROR || size == 0) return 0;
    if (size > 64 * 1024) return 0;                 // 防御非法值
    buf.resize(size);
    if (GetGlyphOutlineW(g_hdc, wc, fmt, &gm, size, buf.data(), &m) == GDI_ERROR)
        return 0;
    return size;
}

// 画一个字形。ink = 当前文字色（colorTable[0]）。
// 坐标：penX/curY 已含 clipX/clipY；baselineY = topY + 字体 ascent。
// 返回写入的像素数（诊断用）。
static int DrawGlyph(WCHAR wc, int penX, int baselineY, BYTE ink,
                     BYTE* pixels, int pitch, int minX, int maxX, int minY, int maxY,
                     int& advOut)
{
    GLYPHMETRICS gm = { 0 };
    std::vector<BYTE> buf;
    DWORD size = FetchGlyph(wc, buf, gm);

    advOut = (gm.gmCellIncX > 0) ? gm.gmCellIncX : (g_pxHeight > 0 ? g_pxHeight : 12);
    if (size == 0 || gm.gmBlackBoxX == 0 || gm.gmBlackBoxY == 0) return 0;

    // 行跨距：1bpp 按 32bit(4 字节) 对齐；8bpp 按 4 字节对齐
    int bufPitch = g_cfg.antiAlias ? (int)((gm.gmBlackBoxX + 3) & ~3u)
                                   : (int)(((gm.gmBlackBoxX + 31) / 32) * 4);

    // GDI 字形原点：x 向右为正、y 向上为正（相对笔位与基线）
    int left = penX + (int)gm.gmptGlyphOrigin.x;
    int top  = baselineY - (int)gm.gmptGlyphOrigin.y;

    int written = 0;
    for (int r = 0; r < (int)gm.gmBlackBoxY; ++r)
    {
        int sy = top + r;
        if (sy < minY || sy > maxY) continue;
        const BYTE* row = buf.data() + (size_t)r * bufPitch;
        BYTE* dstRow = pixels + (size_t)sy * pitch;
        for (int c = 0; c < (int)gm.gmBlackBoxX; ++c)
        {
            bool on;
            if (g_cfg.antiAlias) {
                // 归一化 0..64 → 0..255；早期直接用 0x60=96 比 0..64 导致一个像素都画不出
                int cov = (int)row[c] * 255 / 64;
                if (cov > 255) cov = 255;
                on = cov >= g_cfg.threshold;
            } else {
                on = (row[c >> 3] >> (7 - (c & 7))) & 1;
            }
            if (!on) continue;
            int sx = left + c;
            if (sx < minX || sx > maxX) continue;
            dstRow[sx] = ink;
            ++written;
        }
    }
    return written;
}

// 首次画 CJK 时把整个表面 dump 成 PGM（8 位调色板索引直接当灰度看）。
// 用于「屏幕上看不到文字」时确认像素到底写没写、写在哪个坐标。
static bool g_wantDump = false;   // HookDrawText 指定「这次画完存一张」
static void DumpSurfaceOnce(const BYTE* px, int pitch, int h)
{
    if (!g_wantDump || !g_cfg.dumpFrame || !px || pitch <= 0 || h <= 0) return;
    g_wantDump = false;
    wchar_t p[MAX_PATH];
    wcscpy_s(p, g_baseDir); wcscat_s(p, L"PG2CHS_frame.pgm");
    FILE* f = _wfopen(p, L"wb");
    if (!f) { Log("   -> dump 失败（打不开 PG2CHS_frame.pgm）\n"); return; }
    fprintf(f, "P5\n%d %d\n255\n", pitch, h);
    fwrite(px, 1, (size_t)pitch * (size_t)h, f);
    fclose(f);
    Log("   -> 已 dump 表面 %dx%d -> PG2CHS_frame.pgm\n", pitch, h);
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
    if (pitch <= 0 || surfH <= 0) { if (weLocked && g_unlockFn) g_unlockFn(); return 0; }

    // 裁剪范围与原 blit 完全一致（0x428836..0x428878）：
    //   坐标先按原始 clipX/clipY 偏移；边界下限按 max(clip,0)、上限与 pitch-1/height-1 取小
    int xoff = ds->clipX, yoff = ds->clipY;
    int minX = xoff > 0 ? xoff : 0;
    int minY = yoff > 0 ? yoff : 0;
    int maxX = (ds->maxX < pitch  - 1) ? ds->maxX : pitch  - 1;
    int maxY = (ds->maxY < surfH  - 1) ? ds->maxY : surfH  - 1;

    // 行距：游戏节奏 = 字形高 + 2（本作 10）；若 CJK 字号更大则至少占满自身高度
    int lineAdv = (g_lineH > 0 ? g_lineH + 2 : 10);
    if (lineAdv < g_pxHeight) lineAdv = g_pxHeight;

    int curX = x + xoff, curY = y + yoff;
    int lines = 0, totalPx = 0;
    int len  = (int)strlen(text);
    // 译文文件为对齐原始字节偏移，行尾补了半角空格；排版时不计入，
    // 否则居中/右对齐会整体偏左（文件里的字节保持不变）。
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) --len;

    for (int i = 0; i < len; )
    {
        WCHAR wc; int n = DecodeChar(text, i, len, wc);
        if (!wc) { i += n; continue; }

        if (wc == L'\n') { curX = x + xoff; curY += lineAdv; ++lines; i += n; continue; }

        int adv = 0;
        if (wc < 0x20) {                 // 控制字符（原函数也不画）
            adv = GlyphAdvance(wc);
        } else {
            int topY = curY + g_cfg.vOffset;
            totalPx += DrawGlyph(wc, curX, topY + g_ascent, ink,
                                 pixels, pitch, minX, maxX, minY, maxY, adv);
        }
        curX += adv;
        i += n;
    }

    DumpSurfaceOnce(pixels, pitch, surfH);

    if (weLocked && g_unlockFn) g_unlockFn();

    if (g_cfg.debug && g_dbgDrawLog < 5) {
        Log("   -> H=%p px0=%p px=%d pitch=%d surfH=%d clip=(%d,%d) max=(%d,%d) lineAdv=%d\n",
            H, pixels, totalPx, pitch, surfH, minX, minY, maxX, maxY, lineAdv);
        ++g_dbgDrawLog;
    }
    return lines;   // 原函数返回换行计数（单行串为 0）
}

// =====================================================================
// 自动换行绘制的中文版（0x43B402）。
// 原函数逐「有符号字节」处理：GBK 高位字节被 __isctype 判成不可打印 →
// 既不绘制、也不参与断行，整段中文变成空白。这里保留原来的
// Lock / 裁剪 / 行距 / maxLines 语义，只把字符单位换成 GBK 字符：
//   · CJK 可在任意两字之间断行（宽度 = 全宽）
//   · ASCII 仍按空格整词断行，避免把单词劈开
// =====================================================================
static short WrapDrawCJK(short maxLines, const unsigned char* colorTable,
                         const char* text, void* surface, char flag)
{
    static int dbgWrap = 0;
    if (!EnsureFont()) return 0;

    DrawSurface* ds = (DrawSurface*)surface;
    if (!ds || !ds->H) return 0;
    SurfaceHeader* H = (SurfaceHeader*)ds->H;
    if (!H || !H->pixels) return 0;

    BYTE ink = 0xE3;
    if (colorTable) { BYTE c = colorTable[0]; if (c != 0xFF) ink = c; }

    bool weLocked = false;
    if (g_lockFn && g_pLockFlag && !*g_pLockFlag) {
        if (!g_lockFn()) return 0;
        weLocked = true;
    }

    BYTE* pixels = (BYTE*)H->pixels;
    int   pitch  = H->pitchMinus1 + 1;
    int   surfH  = H->heightMinus1 + 1;
    if (pitch <= 0 || surfH <= 0) { if (weLocked && g_unlockFn) g_unlockFn(); return 0; }

    int minX = ds->clipX > 0 ? ds->clipX : 0;
    int minY = ds->clipY > 0 ? ds->clipY : 0;
    int maxX = (ds->maxX < pitch - 1) ? ds->maxX : pitch - 1;
    int maxY = (ds->maxY < surfH - 1) ? ds->maxY : surfH - 1;

    // 可用区域：与原函数同算法（右/下边界是闭区间，故 +1）
    int maxW = ds->maxX - ds->clipX + 1;
    int maxH = ds->maxY - ds->clipY + 1;
    if (maxW <= 0 || maxH <= 0) { if (weLocked && g_unlockFn) g_unlockFn(); return 0; }

    // 行距：游戏基准是「字形高+2」，CJK 字号更大时必须撑到自身高度
    int glyphH = g_pxHeight > 0 ? g_pxHeight : 8;
    int lineH  = (g_lineH > 0 ? g_lineH : 8) + 2;
    if (lineH < glyphH) lineH = glyphH;
    // 行数多到放不下时压一压行距，但不再低于游戏基准，避免行叠行
    if (maxLines > 1) {
        int fit = maxH / maxLines;
        int base = (g_lineH > 0 ? g_lineH : 8) + 2;
        if (fit > base && fit < lineH) lineH = fit;
    }

    int len   = (int)strlen(text);
    // 剧本译文为对齐原文长度，行尾补了半角空格；换行排版时不计入，
    // 否则几百个尾随空格会把整段文字撑到疯狂折行 / 居中偏左。
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) --len;
    int lines = 0, curW = 0, yOff = 0, px = 0;
    bool stop = false;

    for (int i = 0; i < len; )
    {
        WCHAR wc; int n = DecodeChar(text, i, len, wc);
        if (!wc) { i += n; continue; }

        if (wc == L'\n') {
            if (lines >= maxLines) {
                yOff += lineH;
                if (glyphH + yOff > maxH) { stop = true; break; }
            }
            curW = 0; ++lines; i += n; continue;
        }

        int adv = GlyphAdvance(wc);

        // ---- 断行判定 ----
        bool wrap = false;
        if (wc >= 0x2E80) {                       // CJK 及全角标点：任意处可断
            wrap = (curW > 0 && curW + adv > maxW);
        } else if (wc != L' ') {                  // ASCII 词：整词度量
            int wordW = 0;
            for (int k = i; k < len; ) {
                WCHAR w2; int n2 = DecodeChar(text, k, len, w2);
                if (w2 == L'\n' || w2 == L' ' || w2 >= 0x2E80) break;
                wordW += GlyphAdvance(w2);
                k += n2;
            }
            if (wordW <= 0) wordW = adv;
            wrap = (curW > 0 && curW + wordW > maxW);
        } else {                                  // 空格本身溢出时换行并吞掉
            wrap = (curW > 0 && curW + adv > maxW);
        }

        if (wrap) {
            if (lines >= maxLines) {
                yOff += lineH;
                if (glyphH + yOff > maxH) { stop = true; break; }
            }
            curW = 0; ++lines;
            if (wc == L' ') { i += n; continue; }   // 行首不留空格
        }

        if (lines < maxLines) {
            int topY = yOff + ds->clipY + g_cfg.vOffset;
            px += DrawGlyph(wc, curW + ds->clipX, topY + g_ascent, ink,
                            pixels, pitch, minX, maxX, minY, maxY, adv);
        }
        curW += adv;
        i += n;
    }

    // flag 非 0：在末尾补一个 '|'，与原函数一致（提示「还有更多」）
    if (!stop && flag) {
        int adv = GlyphAdvance(L'|');
        if (curW > 0 && curW + adv > maxW) {
            if (lines >= maxLines) {
                yOff += lineH;
                if (glyphH + yOff > maxH) stop = true;
            }
            if (!stop) { curW = 0; ++lines; }
        }
        if (!stop && lines < maxLines) {
            int topY = yOff + ds->clipY + g_cfg.vOffset;
            px += DrawGlyph(L'|', curW + ds->clipX, topY + g_ascent, ink,
                            pixels, pitch, minX, maxX, minY, maxY, adv);
        }
    }

    if (weLocked && g_unlockFn) g_unlockFn();

    if (g_cfg.debug && dbgWrap < 8) {
        Log("wrap #%d maxLines=%d maxW=%d maxH=%d lineH=%d -> lines=%d px=%d\n",
            dbgWrap, maxLines, maxW, maxH, lineH, lines, px);
        ++dbgWrap;
    }
    return (short)lines;
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
static int g_dbgDraw = 0, g_dbgMeasure = 0, g_dbgCharW = 0, g_dbgAscii = 0, g_dbgWrap = 0;

// 诊断：打印 text 指针的「内存上下文」。
// 目的——判定字符串是被截断（后随 NUL），还是指针落在了行中间（前有残字）。
// 这两种病因的修法完全不同，必须靠实机数据区分，不能靠猜。
static void DumpTextContext(const char* p)
{
    MEMORY_BASIC_INFORMATION mbi = { 0 };
    if (VirtualQuery(p, &mbi, sizeof(mbi))) {
        const char* ty = (mbi.Type == MEM_MAPPED) ? "MAPPED"
                       : (mbi.Type == MEM_IMAGE)  ? "IMAGE" : "PRIVATE";
        Log("   region: base=%p size=%08X type=%s alloc=%08X\n",
            mbi.BaseAddress, (unsigned)mbi.RegionSize, ty, (unsigned)mbi.AllocationBase);
    }
    if (!IsBadReadPtr(p - 48, 48)) {
        Log("   pre[-48]: ");
        for (int i = -48; i < 0; ++i) Log("%02X ", (BYTE)p[i]);
        Log("\n");
    }
    if (!IsBadReadPtr(p, 64)) {
        Log("   post   : ");
        for (int i = 0; i < 64; ++i) Log("%02X ", (BYTE)p[i]);
        Log("\n");
    }
}

// 绘制：纯 ASCII 走原函数（零回归）；含 GBK 走本 DLL 合成
static int __cdecl HookDrawText(int x, int y, const unsigned char* colorTable,
                                const char* text, void* surface)
{
    if (!text) return g_origDrawText(x, y, colorTable, text, surface);
    PatchTextTable();          // 保证「一行一项」的表已就位
    // 菜单画到第 100 次时存一张整屏，便于核对排版（前 3 次是标题画面）
    if (g_cfg.dumpFrame && g_dbgDraw == 100) g_wantDump = true;
    bool hasCJK = false;
    for (const char* p = text; *p; ++p) { if ((BYTE)*p >= 0x81) { hasCJK = true; break; } }
    if (!hasCJK) {
        if (g_cfg.debug && g_dbgAscii < 300) {
            int n = (int)strlen(text);
            Log("A#%d (%d,%d) s=%p len=%d txt=", g_dbgAscii, x, y, surface, n);
            for (int k = 0; k < 40 && k < n; ++k) Log("%02X ", (BYTE)text[k]);
            Log("\n");
            ++g_dbgAscii;
        }
        return g_origDrawText(x, y, colorTable, text, surface);
    }

    if (g_cfg.debug && g_dbgDraw < 300) {
        int n = (int)strlen(text);
        Log("D#%d (%d,%d) s=%p p=%p len=%d ink=%02X txt=", g_dbgDraw,
            x, y, surface, text, n, colorTable ? colorTable[0] : 0xFF);
        for (int k = 0; k < 40 && k < n; ++k) Log("%02X ", (BYTE)text[k]);
        Log("\n");
    }
    ++g_dbgDraw;

    // 自检：首次绘制时用真实表面跑一遍换行渲染器，验证断行/行距，并把整屏 dump 下来。
    // 只在 ini 里 SelfTest=1 时启用，日常不跑。
    if (g_cfg.selfTest && g_dbgDraw == 1) {
        WCHAR w[] = L"剧本简报换行测试：这一段故意写得比较长，用来验证自动换行渲染在中文下的"
                    L"断行、行距与字距是否正确，以及是否会被容器边界裁掉。第二行应该自然折下来。";
        char gbk[600] = { 0 };
        WideCharToMultiByte(936, 0, w, -1, gbk, sizeof(gbk), nullptr, nullptr);
        short n = WrapDrawCJK(4, colorTable, gbk, surface, 0);
        Log("selftest: wrap -> lines=%d\n", n);
        g_wantDump = true;
    }
    return RenderStringCJK(x, y, colorTable, text, surface);
}

// 测宽：纯 ASCII 走原函数；含 GBK 按字符累加字形步进
static short __cdecl HookMeasureText(const char* text)
{
    if (!text) return 0;
    PatchTextTable();
    bool hasCJK = false;
    for (const char* p = text; *p; ++p) { if ((BYTE)*p >= 0x81) { hasCJK = true; break; } }
    if (!hasCJK) return g_origMeasure ? g_origMeasure(text) : 0;

    if (!EnsureFont()) return 0;

    int len = (int)strlen(text), w = 0;
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) --len;  // 去掉行尾填充空格
    for (int i = 0; i < len; ) {
        WCHAR wc; int n = DecodeChar(text, i, len, wc);
        if (wc) w += GlyphAdvance(wc);
        i += n;
    }
    if (g_cfg.debug && g_dbgMeasure < 20) {
        Log("measure CJK #%d len=%d -> %d txt=", g_dbgMeasure, len, w);
        for (int k = 0; k < 24 && k < len; ++k) Log("%02X ", (BYTE)text[k]);
        Log("\n");
    }
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
        int h = EffectiveHeight();
        return (h + 1) / 2;
    }
    if (!g_charWidth || !fontObj) return 6;
    return g_charWidth(fontObj, ch);
}

// 自动换行绘制：纯 ASCII 走原函数（零回归）；含 GBK 走中文实现
static short __cdecl HookWrapDraw(short maxLines, const unsigned char* colorTable,
                                  const char* text, void* surface, char flag)
{
    if (!text || !g_origWrap)
        return g_origWrap ? g_origWrap(maxLines, colorTable, text, surface, flag) : 0;
    bool hasCJK = false;
    for (const char* p = text; *p; ++p) { if ((BYTE)*p >= 0x81) { hasCJK = true; break; } }
    if (!hasCJK) return g_origWrap(maxLines, colorTable, text, surface, flag);
    // 剧本简报验证用：打印传进来的文本字节，可判断中文是否被加载器切碎
    if (g_cfg.debug && g_dbgWrap < 20) {
        int n = (int)strlen(text);
        Log("W#%d maxLines=%d len=%d txt=", g_dbgWrap, maxLines, n);
        for (int k = 0; k < 32 && k < n; ++k) Log("%02X ", (BYTE)text[k]);
        Log("\n");
        ++g_dbgWrap;
    }
    return WrapDrawCJK(maxLines, colorTable, text, surface, flag);
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
        wcscpy_s(g_baseDir, dllPath);
        wchar_t* pdir = wcsrchr(g_baseDir, L'\\');
        if (pdir) pdir[1] = L'\0'; else g_baseDir[0] = L'\0';   // 只留目录部分
        wchar_t logPath[MAX_PATH];
        wcscpy_s(logPath, dllPath);
        wchar_t* slash = wcsrchr(logPath, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash - logPath + 1), L"PG2CHS.log");
        else wcscpy_s(logPath, L"PG2CHS.log");
        g_log = _wfopen(logPath, L"w");
        LoadConfig(dllPath);
        Log("PG2CHS: 附加 (hMod=%p)\n", hModule);
        {
            // 诊断：游戏工作目录决定它从哪里读文本表（GUI97 / MISC / SCENARIO）
            wchar_t cwd[MAX_PATH] = { 0 };
            GetCurrentDirectoryW(MAX_PATH, cwd);
            wchar_t full[MAX_PATH] = { 0 };
            GetFullPathNameW(L"SCENARIO\\A.TXT", MAX_PATH, full, nullptr);
            Log("cwd=%ls\nscenario 探测=%ls exists=%d\n", cwd, full,
                GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES ? 1 : 0);
        }

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
        static const BYTE kWrapPro[6]      = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30 };    // + sub esp,30h

        // 顺序：先字符查表（兜底），再测宽，再绘制，最后 blit 闸门
        g_charWidth    = (CharWidthFn)   InstallDetour(ADDR_CHARWIDTH, 5, kCharWidthPro, (void*)&HookCharWidth);
        g_origMeasure  = (OrigMeasureFn) InstallDetour(ADDR_MEASURE,   6, kMeasurePro,   (void*)&HookMeasureText);
        g_origDrawText = (OrigDrawTextFn)InstallDetour(ADDR_DRAWTEXT,  6, kDrawPro,      (void*)&HookDrawText);
        g_origWrap     = (OrigWrapFn)    InstallDetour(ADDR_WRAPDRAW,  6, kWrapPro,      (void*)&HookWrapDraw);
        g_origBlit     = (BlitFn)        InstallDetour(ADDR_BLIT,      6, kBlitPro,      (void*)&HookBlit);

        Log("PG2CHS: hook 状态 charwidth=%p measure=%p draw=%p wrap=%p\n",
            g_charWidth, g_origMeasure, g_origDrawText, g_origWrap);
        if (!g_origDrawText) Log("PG2CHS: !! 绘制 hook 未装上，中文无法显示\n");
        if (!g_origMeasure)  Log("PG2CHS: !! 测宽 hook 未装上，含中文的串可能崩\n");
        if (!g_charWidth)    Log("PG2CHS: !! 字符查表 hook 未装上\n");
        if (!g_origWrap)     Log("PG2CHS: !! 换行绘制 hook 未装上，剧本简报不会有中文\n");
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
