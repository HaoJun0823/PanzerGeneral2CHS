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
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

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
    bool  repairFragments  = true; // 还原被游戏按字节切碎的关卡标题（见下方「碎片还原」）
    bool  translateExtra   = true; // 显示层替换：城市/河流/装备/姓氏等英文词条 → 中文
    // 游戏脏矩形按「自带位图字形高」(fontObj+8，本作 8px) 刷新文字背景，
    // 12px 汉字下面 4px 永远在刷新区外 → 上一次文字的下半截擦不掉。
    bool  fitGlyphHeight   = true; // 1 = 简报多行路径压进游戏行距（默认 10px）
    int   glyphHeight      = 0;    // 简报压高目标(px)；0 = 自动取行距(10)
    // 条件背景擦除：全屏记录我们写过的笔画点，每次画字前把该串矩形内
    // 仍呈文字色的标记点填成背景众数，只动自己写过的像素，底纹零破坏。
    bool  inkUndo          = true; // 1 = 启用条件背景擦除（沿用旧 ini 键名）
    bool  fullHeightText   = true; // 1 = 普通文本恢复完整 12x12 方字（依赖 inkUndo）
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
static const DWORD ADDR_VSPRINTF   = 0x48D910;  // CRT _vsprintf（PrintfText 内部）
static const DWORD ADDR_SOURCEBUF  = 0x538960;  // 全局格式化缓冲 Source
static const DWORD RET_PRINTFTEXT  = 0x43B20B;  // PrintfText 内 call DrawText 之后（实机 ret=0043B20B）

// IDA: 0x43B19F PrintfText(x, y, surface, fmt, ...)
//      → vsprintf(Source, fmt, args) → DrawText(x, y, palette, Source, surface)
// 按钮文字被游戏截成 1 字节，但格式化参数里往往还留着完整标题。
// 在这里把它抓下来，DrawText 侧就能精确还原，不必按坐标/候选瞎猜。
typedef int (__cdecl *VsprintfFn)(char*, const char*, va_list);
static VsprintfFn g_origVsprintf  = nullptr;
static char*      g_sourceBuf     = nullptr;
static char       g_lastFmtText[256] = { 0 };
static int        g_lastFmtLen    = 0;
static BYTE       g_lastFmtB0     = 0;
static char       g_lastFullTitle[256] = { 0 };  // 从 fmt 缓冲跨 0 拼回的完整标题
static int        g_lastFullLen   = 0;
static int        g_fmtLog        = 0;

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

// 日志本身是 UTF-8 文件（源码以 /utf-8 编译，字面量直接落盘），
// 而游戏文本是 GBK(936)。直接 %s 打 GBK 字节只会得到一堆乱码，
// 所以这里手工按 GBK 对齐解码成 UTF-16，再转成 UTF-8 供人阅读。
// 特殊字节做成可见标记：孤立高位=·(半个汉字)，非法对=?(替换符)，
// CRLF=\\r\\n，其它控制字符=.  —— 这样「半个字符」在日志里一眼可辨。
static std::string GbkToUtf8(const char* s, int len)
{
    std::wstring w;
    if (s && len > 0) w.reserve((size_t)len);
    int i = 0;
    while (s && i < len) {
        BYTE b = (BYTE)s[i];
        if (b >= 0x81 && i + 1 < len &&
            (BYTE)s[i+1] >= 0x40 && (BYTE)s[i+1] != 0x7F) {
            char db[3] = { s[i], s[i+1], 0 };
            WCHAR wc = 0;
            if (MultiByteToWideChar(936, 0, db, 2, &wc, 1) > 0 && wc) {
                w.push_back(wc);
            } else {
                // 非法 GBK 对：标出原始字节，日志里一眼可查
                static const char kHex[] = "0123456789ABCDEF";
                BYTE b2 = (BYTE)s[i+1];
                w.push_back(L'[');
                w.push_back(kHex[b >> 4]);  w.push_back(kHex[b & 15]);
                w.push_back(kHex[b2 >> 4]); w.push_back(kHex[b2 & 15]);
                w.push_back(L']');
            }
            i += 2;
            continue;
        }
        if (b >= 0x81) {                              // 孤立高位字节：[CF] 比 · 更好对照 hex
            static const char kHex[] = "0123456789ABCDEF";
            w.push_back(L'[');
            w.push_back(kHex[b >> 4]); w.push_back(kHex[b & 15]);
            w.push_back(L']');
            ++i; continue;
        }
        if (b == '\r') { w.push_back(L'\\'); w.push_back(L'r'); ++i; continue; }
        if (b == '\n') { w.push_back(L'\\'); w.push_back(L'n'); ++i; continue; }
        if (b < 0x20)  { w.push_back(L'.'); ++i; continue; }
        w.push_back((WCHAR)b);
        ++i;
    }
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// 紧跟在 hex dump 之后追加  | "可读中文"，让日志不需要查码表就能读。
static void LogText(const char* text, int len, int maxShow = 48)
{
    if (!g_log || !text || len < 0) return;
    int n = len < maxShow ? len : maxShow;
    std::string u = GbkToUtf8(text, n);
    Log(" | \"%s\"", u.c_str());
    if (len > n) Log("...(共%d字节)", len);
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
        Log("texttable: 换掉 %d 项；#156=", changed);
        if (g_tblPtr[156]) LogText(g_tblPtr[156], (int)strlen(g_tblPtr[156]), 40);
        else Log("(null)");
        Log("\n");
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
    g_cfg.repairFragments = GetPrivateProfileIntW(L"PG2CHS", L"RepairFragments",
                                                  g_cfg.repairFragments ? 1 : 0, iniPath) != 0;
    g_cfg.translateExtra  = GetPrivateProfileIntW(L"PG2CHS", L"TranslateExtra",
                                                  g_cfg.translateExtra ? 1 : 0, iniPath) != 0;
    g_cfg.fitGlyphHeight  = GetPrivateProfileIntW(L"PG2CHS", L"FitGlyphHeight",
                                                  g_cfg.fitGlyphHeight ? 1 : 0, iniPath) != 0;
    g_cfg.glyphHeight     = GetPrivateProfileIntW(L"PG2CHS", L"GlyphHeight",
                                                  g_cfg.glyphHeight, iniPath);
    g_cfg.inkUndo         = GetPrivateProfileIntW(L"PG2CHS", L"InkUndo",
                                                  g_cfg.inkUndo ? 1 : 0, iniPath) != 0;
    g_cfg.fullHeightText  = GetPrivateProfileIntW(L"PG2CHS", L"FullHeightText",
                                                  g_cfg.fullHeightText ? 1 : 0, iniPath) != 0;
    if (g_cfg.glyphHeight < 0)  g_cfg.glyphHeight = 0;
    if (g_cfg.glyphHeight > 32) g_cfg.glyphHeight = 32;
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

// 字形实际占用的垂直高度：不超过游戏刷新矩形的高度。
// 与字号解耦：横向仍用 fontHeight(12px) 的完整点阵，只压纵向。
static int EffectiveDrawHeight()
{
    int src = g_pxHeight > 0 ? g_pxHeight : 12;
    if (!g_cfg.fitGlyphHeight) return src;
    // 简报行距固定 10px（g_lineH+2），字形取 10 正好不越界也不压下一行；
    // 从上一版的 8px 提到 10px，汉字明显更方整，同时不增加行数。
    int dst = g_cfg.glyphHeight > 0 ? g_cfg.glyphHeight
                                    : (g_lineH > 0 ? g_lineH + 2 : 10);
    if (dst < 6)  dst = 6;
    if (dst > src) dst = src;
    return dst;
}

// 普通文本（按钮 / 状态栏 / 菜单，含描边层）：条件背景擦除已接管旧字清理，
// 不再受游戏 8px 脏矩形约束，恢复到完整字号，汉字重新方正好读。
// 简报用 WrapDrawCJK，行距只有 10px，仍走 EffectiveDrawHeight()（10px），
// 否则 12px 字会压到下一行 / 撑爆 maxH 分页。
static int EffectiveTextDrawHeight()
{
    if (g_cfg.fullHeightText && g_cfg.inkUndo)
        return g_pxHeight > 0 ? g_pxHeight : 12;
    return EffectiveDrawHeight();
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
            Log("font: %ls  %dpx  ascent=%d  游戏字形高=%d  简报绘制高=%d  "
                "普通文本绘制高=%d  条件擦除=%d  模式=%s\n",
                name, g_pxHeight, g_ascent, g_lineH, EffectiveDrawHeight(),
                EffectiveTextDrawHeight(), g_cfg.inkUndo ? 1 : 0,
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

// ===================== 关卡标题碎片还原 / GBK 自同步 ========================
// 真机日志（2026-11-15）证实：GUI97 由 PatchTextTable 逐行重建，显示正常；
// 但 SCENARIO/*.TXT 的首行是游戏自己读进内存的，会经过逐字节的 ctype 过滤 /
// 截断，遇到部分 GBK 高位字节就写 0。于是 DrawText 收到的不是完整标题，而是：
//   a) 停在半个字符上的前缀   B0 A2 C0 AD BF        = "阿拉" + "库" 的首字节
//   b) 从双字节中间起的切片   A5 C2 E5 20 2D 20 D1 DB = "圣洛 - 眼镜蛇行动" 丢了 CA
// 把 (b) 按 CP936 硬解，A5C2 落在 GBK 假名区 → 日文假名；E5 20 是非法对 → '?'；
// 再往后整体错一字节 → 缺字形方块；孤立高字节被丢掉 → 缺字/空白。
// 这正是用户看到的「日语 / 问号 / 方块 / 缺字 / 空白」的全部来源，译文本身没问题
// （已逐行校验：所有 CHS 文件 GBK 半字符行数 = 0）。
//
// 对策两层：
//   1) 登记表：启动时读 SCENARIO/*.TXT 首行完整标题；绘制时若传入串是某个完整
//      标题的真前缀 / 子串，就换回完整标题（只有唯一匹配才换，避免张冠李戴）。
//      GUI97 的 432 行作为白名单先排除，保证已正常的 GUI 零改动。
//   2) 自同步：登记表没命中时，比较从 offset 0 / 1 解码的「可疑度」，明显更优
//      才偏移；非法字节静默跳过 1 字节，绝不画 '?'、也不吞 2 字节造成级联错位。
static std::vector<std::string> g_titles;          // SCENARIO 首行全集（去尾空格，去重）
static std::vector<std::string> g_shortTitles;     // 真正的短标题：排除简报正文长句
static std::vector<std::string> g_campaignTitles;  // 战役文件标题（会被游戏切掉首字节）
static std::vector<std::string> g_guiCandidates;  // GUI97 中文行，二级还原候选
static std::vector<std::string> g_guiLines;        // GUI97 行（去尾空格）白名单
static std::vector<std::string> g_blankSeen;       // blank# 去重键
static bool g_titlesTried = false;
static int  g_repairLog   = 0;
static int  g_leadLog     = 0;
static int  g_blankLog    = 0;
static int  g_leadBtnLog  = 0;

// 只有「短、无句读」的行才可能是关卡标题。
// 实机发现某些 SCENARIO 文件首行其实是简报正文（"将军，……"），
// 若把它们放进候选表，按首字节匹配时 CE 有 13 个、BD 有 54 个候选，
// 全部歧义 → 首字节按钮永远还原不了（用户看到的空白按钮）。
static bool IsShortTitle(const std::string& s)
{
    if (s.size() < 2 || s.size() > 40) return false;
    static const char* punct[] = {
        "\xA1\xA2", "\xA1\xA3", "\xA3\xAC", "\xA3\xA1", "\xA3\xBF",
        "\xA1\xAD", "\xA1\xA6", "\xA3\xBA", "\xA3\xBB"
    };
    for (const char* p : punct)
        if (s.find(p) != std::string::npos) return false;
    return true;
}

// 实机确认的歧义消解表（GBK 字节，勿写成中文字符串字面量：源文件按 UTF-8 编译）。
// 关卡列表按钮 "卡昂 -" 同时是三个标题的前缀，光凭碎片无法区分；
// 用户实测该按钮对应 CH1《卡昂 - 查恩伍德行动》，在此固定下来。
struct AmbFix { const char* key; const char* val; };
static const AmbFix kAmbFix[] = {
    { "\xBF\xA8\xB0\xBA\x20\x2D",
      "\xBF\xA8\xB0\xBA\x20\x2D\x20\xB2\xE9\xB6\xF7\xCE\xE9\xB5\xC2\xD0\xD0\xB6\xAF" },
};

// 关卡列表按钮标题校订表（Resources/CHS/FragTitleFix.txt，GBK，TAB 分隔）
//   首字节HEX <TAB> nearest 原选中的标题 <TAB> 实际应显示的标题
// 背景：b0=C8 的 1 字节碎片同时是「萨凡纳 / 萨莱诺 - 重返欧洲 / 塞洛高地」
// 的前缀，仅凭内存距离无法 100% 消歧。实机 fmtamb# 显示 nearest 对相邻两个
// 按钮给出的结果稳定但正好互换，故用外部可校订表纠正；只对「多候选」路径生效，
// 唯一命中（如「萨莱诺登陆」）完全不经过这里，零回归。
struct FragFix { BYTE b0; std::string from, to; };
static std::vector<FragFix> g_fragFixes;
static bool g_fragFixTried = false;

static void LoadFragTitleFix()
{
    if (g_fragFixTried) return;
    g_fragFixTried = true;
    wchar_t path[MAX_PATH];
    wcscpy_s(path, g_baseDir); wcscat_s(path, L"FragTitleFix.txt");
    FILE* fp = _wfopen(path, L"rb");
    if (!fp) { Log("fragfix: 未找到 FragTitleFix.txt（跳过校订）\n"); return; }
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\r' || line[n-1] == '\n' ||
                     line[n-1] == ' '  || line[n-1] == '\t')) --n;
        line[n] = 0;
        if (n < 5 || line[0] == ';' || line[0] == '#') continue;
        char* a = strchr(line, '\t');
        if (!a) continue;
        *a++ = 0;
        char* b = strchr(a, '\t');
        if (!b) continue;
        *b++ = 0;
        BYTE key = (BYTE)strtoul(line, nullptr, 16);
        std::string from(a), to(b);
        while (!from.empty() && (from.back() == ' ' || from.back() == '\t')) from.pop_back();
        while (!to.empty()   && (to.back()   == ' ' || to.back()   == '\t')) to.pop_back();
        if (key && !from.empty() && !to.empty())
            g_fragFixes.push_back({ key, from, to });
    }
    fclose(fp);
    Log("fragfix: 登记 %d 条\n", (int)g_fragFixes.size());
}

static const std::string* ApplyFragTitleFix(BYTE b0, const std::string& picked)
{
    LoadFragTitleFix();
    for (const auto& f : g_fragFixes)
        if (f.b0 == b0 && f.from == picked) return &f.to;
    return nullptr;
}

static void LoadScenarioTitles()
{
    if (g_titlesTried) return;
    g_titlesTried = true;
    const wchar_t* pats[2] = { L"SCENARIO\\*.TXT", L"..\\SCENARIO\\*.TXT" };
    const wchar_t* dirs[2] = { L"SCENARIO\\",      L"..\\SCENARIO\\" };
    for (int d = 0; d < 2; ++d) {
        wchar_t pat[MAX_PATH];
        wcscpy_s(pat, g_baseDir); wcscat_s(pat, pats[d]);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t full[MAX_PATH];
            wcscpy_s(full, g_baseDir); wcscat_s(full, dirs[d]); wcscat_s(full, fd.cFileName);
            FILE* fp = _wfopen(full, L"rb");
            if (!fp) continue;
            char line[512] = { 0 };
            std::string first, second;
            int nonEmpty = 0;
            while (nonEmpty < 2 && fgets(line, sizeof(line), fp)) {
                size_t n = strlen(line);
                while (n && (line[n-1] == '\r' || line[n-1] == '\n' ||
                             line[n-1] == ' '  || line[n-1] == '\t')) --n;
                if (n == 0) continue;
                if (nonEmpty == 0) first.assign(line, n);
                else second.assign(line, n);
                ++nonEmpty;
            }
            if (first.size() >= 2) {
                if (std::find(g_titles.begin(), g_titles.end(), first) == g_titles.end())
                    g_titles.push_back(first);
                if (IsShortTitle(first) &&
                    std::find(g_shortTitles.begin(), g_shortTitles.end(), first) == g_shortTitles.end())
                    g_shortTitles.push_back(first);
                // 战役文件特征（原版 ENG）：全大写战役名 + 空行 + 同名标题，
                // 例如 "CRUSADE IN THE WEST" / "Crusade in the West"。
                // 这类文件的首字节会被游戏单独切出来当按钮，单独登记以便精确还原。
                if (nonEmpty >= 2 && first == second && IsShortTitle(first) &&
                    std::find(g_campaignTitles.begin(), g_campaignTitles.end(), first) == g_campaignTitles.end())
                    g_campaignTitles.push_back(first);
            }
            fclose(fp);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (!g_titles.empty()) break;
    }
    // GUI97 的按钮/说明同样会被游戏按字节切碎（实机存在「画不出来的空白按钮」）。
    // 单独存一张二级候选表：只有 SCENARIO 标题表没命中时才查它，保证零回归。
    {
        wchar_t gui[MAX_PATH];
        wcscpy_s(gui, g_baseDir); wcscat_s(gui, L"GUI97.TXT");
        FILE* fp = _wfopen(gui, L"rb");
        if (fp) {
            char line[1024];
            while (fgets(line, sizeof(line), fp)) {
                size_t n = strlen(line);
                while (n && (line[n-1] == '\r' || line[n-1] == '\n' ||
                             line[n-1] == ' '  || line[n-1] == '\t')) --n;
                if (n < 4) continue;                 // 太短的行当候选只会制造歧义
                bool cjk = false;
                for (size_t k = 0; k < n; ++k) if ((BYTE)line[k] >= 0x81) { cjk = true; break; }
                if (!cjk) continue;
                std::string t(line, n);
                if (std::find(g_guiCandidates.begin(), g_guiCandidates.end(), t) == g_guiCandidates.end())
                    g_guiCandidates.push_back(t);
            }
            fclose(fp);
        }
    }
    Log("titles: 全 %d / 短标题 %d / 战役 %d / GUI 行 %d\n",
        (int)g_titles.size(), (int)g_shortTitles.size(),
        (int)g_campaignTitles.size(), (int)g_guiCandidates.size());
}

static void EnsureGuiLines()
{
    if (!g_guiLines.empty() || !g_tblPool) return;
    for (int i = 0; i < TEXTTABLE_COUNT; ++i) {
        const char* g = g_tblPtr[i];
        if (!g) continue;
        int n = (int)strlen(g);
        while (n && (g[n-1] == ' ' || g[n-1] == '\t')) --n;
        if (n > 0) g_guiLines.push_back(std::string(g, n));
    }
}

// ===================== 显示层英文词条替换 ==================================
// 为什么要放在「绘制入口」而不是改磁盘文件：
//   SCENARIO/*.TXT 的非首行（城市名、河流名、部队代号、坐标）参与地图数据匹配，
//   NAMES.TXT / EQUIP97.TXT 的名字按索引进存档。改文件长度会同时破坏匹配与兼容性。
//   这里只在 DrawText 收到纯 ASCII 显示串时，把登记过的词条换成中文再自绘：
//   游戏内部逻辑、存档、索引全部保持英文，零风险；译名表是外部 GBK 文本，可随时改。
// ExtraTerms.txt 格式：每行 `英文<TAB>中文`，GBK 编码，';' 开头为注释。
struct ExtraTerm { std::string en, zh; };
static std::vector<ExtraTerm> g_extraTerms;
static std::vector<int>       g_termBucket[256];  // 按英文首字节分桶，避免每帧全表扫描
static bool g_extraTried = false;
static int  g_termLog    = 0;

static void LoadExtraTerms()
{
    if (g_extraTried) return;
    g_extraTried = true;
    const wchar_t* names[2] = { L"ExtraTerms.txt", L"..\\ExtraTerms.txt" };
    for (int d = 0; d < 2; ++d) {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, g_baseDir); wcscat_s(path, names[d]);
        FILE* fp = _wfopen(path, L"rb");
        if (!fp) continue;
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            int n = (int)strlen(line);
            while (n && (line[n-1] == '\r' || line[n-1] == '\n' ||
                         line[n-1] == ' '  || line[n-1] == '\t')) --n;
            if (n < 3 || line[0] == ';' || line[0] == '#') continue;
            char* tab = (char*)memchr(line, '\t', (size_t)n);
            if (!tab) continue;
            int enLen = (int)(tab - line);
            int zhLen = n - enLen - 1;
            if (enLen < 2 || zhLen < 2) continue;
            bool cjk = false;
            for (int k = 0; k < zhLen; ++k)
                if ((BYTE)tab[1 + k] >= 0x81) { cjk = true; break; }
            if (!cjk) continue;
            g_extraTerms.push_back({ std::string(line, enLen),
                                     std::string(tab + 1, zhLen) });
        }
        fclose(fp);
        if (!g_extraTerms.empty()) break;
    }
    // 最长匹配优先：避免 "St Lo" 抢先吃掉 "St Lo d'Orville"
    std::stable_sort(g_extraTerms.begin(), g_extraTerms.end(),
        [](const ExtraTerm& a, const ExtraTerm& b) { return a.en.size() > b.en.size(); });
    for (int i = 0; i < (int)g_extraTerms.size(); ++i)
        if (!g_extraTerms[i].en.empty())
            g_termBucket[(BYTE)g_extraTerms[i].en[0]].push_back(i);
    Log("terms: 登记 %d 条显示层译名\n", (int)g_extraTerms.size());
}

static inline bool IsWordChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// "Tuesday, July 25, 1944" -> "1944年7月25日 星期二"（GBK 字节字面量）
static bool MaybeFormatDate(const char* text, std::string& out)
{
    char wd[16] = { 0 }, mo[16] = { 0 };
    int d = 0, y = 0;
    if (sscanf(text, "%15[A-Za-z], %15[A-Za-z] %d, %d", wd, mo, &d, &y) != 4) return false;
    static const char* kW[] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                "Thursday", "Friday", "Saturday" };
    static const char* kWZ[] = { "\xC8\xD5", "\xD2\xBB", "\xB6\xFE", "\xC8\xFD",
                                 "\xCB\xC4", "\xCE\xE5", "\xC1\xF9" };
    static const char* kM[] = { "January", "February", "March", "April", "May", "June",
                                "July", "August", "September", "October", "November", "December" };
    int wi = -1, mi = -1;
    for (int i = 0; i < 7; ++i)  if (_stricmp(wd, kW[i]) == 0) wi = i;
    for (int i = 0; i < 12; ++i) if (_stricmp(mo, kM[i]) == 0) mi = i;
    if (wi < 0 || mi < 0 || d < 1 || d > 31 || y < 1000 || y > 9999) return false;
    char num[32];
    _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", y); out = num;
    out += "\xC4\xEA";                                  // 年
    _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", mi + 1); out += num;
    out += "\xD4\xC2";                                  // 月
    _snprintf_s(num, sizeof(num), _TRUNCATE, "%d", d);    out += num;
    out += "\xC8\xD5 \xD6\xDC";                         // 日 周
    out += kWZ[wi];
    return true;
}

// 返回 true 表示 out 里已经有中文，调用方应改走 CJK 自绘路径。
static bool ReplaceDisplayTerms(const char* text, std::string& out)
{
    if (!g_cfg.translateExtra || !text || !*text) return false;
    LoadExtraTerms();
    int len = (int)strlen(text);
    // 已经含 CJK 的串（GUI97 / 简报 / 已还原的标题）一律不动，避免二次替换
    for (int i = 0; i < len; ++i) if ((BYTE)text[i] >= 0x81) return false;
    if (MaybeFormatDate(text, out)) return true;
    if (g_extraTerms.empty()) return false;

    out.clear();
    out.reserve((size_t)len + 32);
    bool hit = false;
    for (int i = 0; i < len; ) {
        bool done = false;
        for (int idx : g_termBucket[(BYTE)text[i]]) {
            const ExtraTerm& t = g_extraTerms[idx];
            int n = (int)t.en.size();
            if (i + n > len) continue;
            if (memcmp(text + i, t.en.data(), (size_t)n) != 0) continue;
            if (i > 0 && IsWordChar(text[i - 1])) continue;      // 左词边界
            if (i + n < len && IsWordChar(text[i + n])) continue; // 右词边界
            out += t.zh; hit = true; i += n; done = true; break;
        }
        if (!done) out.push_back(text[i++]);
    }
    if (hit && g_cfg.debug && g_termLog < 120) {
        Log("term#%d: ", g_termLog);
        LogText(text, len, 48);
        Log(" -> ");
        LogText(out.c_str(), (int)out.size(), 48);
        Log("\n");
        ++g_termLog;
    }
    return hit;
}

// 解一个字符：GBK 双字节 / ASCII。返回消费字节数（1 或 2）。
// 关键：任何非法/半个字符都只丢弃当前 1 个字节、wc=0（不画），
// 绝不返回 '?' 或吞掉 2 个字节 —— 后者会让后面整段错位，拼出假名和方块。
static inline int DecodeChar(const char* text, int i, int len, WCHAR& wc)
{
    BYTE b = (BYTE)text[i];
    if (b >= 0x81 && i + 1 < len) {
        BYTE b2 = (BYTE)text[i + 1];
        if (b2 >= 0x40 && b2 != 0x7F) {
            char db[3] = { text[i], text[i + 1], 0 };
            WCHAR wb[2] = { 0 };
            if (MultiByteToWideChar(936, 0, db, 2, wb, 1) > 0 && wb[0]) {
                wc = wb[0];
                return 2;
            }
        }
        wc = 0; return 1;                 // 非法对：只跳 1 字节，重新找对齐
    }
    if (b < 0x80) { wc = (WCHAR)b; return 1; }
    wc = 0; return 1;                     // 尾部孤立高字节：静默丢弃
}

// 从 start 开始解码的「可疑度」，越低越像一段正常中文。
static int GBKDecodeCost(const char* s, int len, int start)
{
    int cost = 0;
    for (int i = start; i < len; ) {
        BYTE b = (BYTE)s[i];
        if (b >= 0x81) {
            if (i + 1 < len) {
                BYTE b2 = (BYTE)s[i + 1];
                if (b2 < 0x40 || b2 == 0x7F) { cost += 8; i += 1; continue; }
                char db[3] = { s[i], s[i + 1], 0 };
                WCHAR wb[2] = { 0 };
                if (MultiByteToWideChar(936, 0, db, 2, wb, 1) <= 0 || !wb[0]) {
                    cost += 8; i += 1; continue;
                }
                WCHAR w = wb[0];
                // 假名 / 制表方块 / 替换符：正常中文译文几乎不用，是错位的强信号
                if ((w >= 0x3040 && w <= 0x30FF) ||
                    (w >= 0x2500 && w <= 0x259F) || w == 0xFFFD) cost += 6;
                i += 2;
            } else { cost += 2; i += 1; } // 尾部孤立高字节
        } else {
            if (b < 0x20 && b != '\n' && b != '\r' && b != '\t') cost += 2;
            i += 1;
        }
    }
    return cost;
}

// 游戏按字节分页/截断时，起点可能落在 GBK 双字节中间（日志里"圣洛"丢了 CA）。
// 只有 offset=1 明显更干净时才偏移，避免误伤本来正确的串。
static int ChooseGBKStart(const char* s, int len)
{
    if (len < 2) return 0;
    int c0 = GBKDecodeCost(s, len, 0);
    if (c0 == 0) return 0;
    int c1 = GBKDecodeCost(s, len, 1);
    return (c1 + 6 <= c0) ? 1 : 0;
}

// 在指定候选池里找碎片 t 对应的完整串。
//   1 = 唯一命中（out 有效）；0 = 无匹配；-1 = 歧义且无法消解
static int MatchInPool(const std::vector<std::string>& pool,
                       const std::string& t, int n, std::string& out)
{
    // 1) 前缀：碎片是某完整串的前 n 字节（最常见的「半字符处被截断」）
    const std::string* hit = nullptr;
    int prefCount = 0;
    size_t lcp = 0;
    for (const auto& T : pool) {
        if ((int)T.size() <= n || T.compare(0, n, t) != 0) continue;
        if (prefCount == 0) { hit = &T; lcp = T.size(); }
        else {
            size_t j = 0;
            while (j < lcp && j < T.size() && (*hit)[j] == T[j]) ++j;
            lcp = j;
        }
        ++prefCount;
    }
    if (prefCount == 1) { out = *hit; return 1; }
    if (prefCount > 1) {
        // 实机确认的歧义优先；否则退化为所有候选的公共前缀。
        for (size_t k = 0; k < sizeof(kAmbFix) / sizeof(kAmbFix[0]); ++k)
            if (t == kAmbFix[k].key) { out = kAmbFix[k].val; return 1; }
        if (lcp > (size_t)n && lcp >= 2) { out = hit->substr(0, lcp); return 1; }
        return -1;
    }

    // 2) 子串：游戏吃掉了首字节，指针落在双字节中间（"圣洛" → A5 C2 E5...）
    if (n >= 2) {
        const std::string* sub = nullptr;
        int subCount = 0;
        for (const auto& T : pool) {
            size_t p = T.find(t);
            if (p == std::string::npos || p == 0) continue;
            sub = &T; ++subCount;
        }
        if (subCount == 1) { out = *sub; return 1; }
        if (subCount > 1) return -1;
    }
    return 0;
}

// 传入串若为某个完整标题 / GUI 行的碎片，写入 out 并返回 true。
static bool RepairFragment(const char* text, int len, std::string& out)
{
    if (!g_cfg.repairFragments || !text || len < 1 || len > 64) return false;
    LoadScenarioTitles();
    if (g_shortTitles.empty()) return false;

    int n = len;
    while (n > 0 && (text[n-1] == ' ' || text[n-1] == '\t')) --n;  // 去行尾填充
    if (n < 1) return false;
    std::string t(text, n);

    EnsureGuiLines();
    if (std::find(g_guiLines.begin(), g_guiLines.end(), t) != g_guiLines.end()) return false;
    if (std::find(g_titles.begin(),  g_titles.end(),  t) != g_titles.end())  return false;
    if (std::find(g_guiCandidates.begin(), g_guiCandidates.end(), t) != g_guiCandidates.end()) return false;

    // 两级隔离：先查关卡标题，没命中才查 GUI97 行，保证原有 258 个标题的
    // 唯一匹配结果零回归。
    std::string r;
    int k = MatchInPool(g_shortTitles, t, n, r);
    if (k == 0) k = MatchInPool(g_guiCandidates, t, n, r);
    if (k != 1) return false;
    out = r;
    return true;
}

// 游戏读 SCENARIO 时首字节被 ctype 过滤吃掉（实机 W#11/W#15）：
//   CE F7 CF DF ... "西线十字军" -> F7 CF DF ... = "线十字军"
//   BD F8 BE FC ... "进军柏林"   -> F8 BE FC ... = "军柏林"
// 简报第一行只剩「完整标题去掉首字节」。这里补回那一个字节；第一行之后的
// 全部内容（填充空格、空行、第二份标题、日期、正文）原样保留。
static bool FixMissingLeadByte(const char* text, int len, std::string& out)
{
    if (!g_cfg.repairFragments || !text || len < 4 || len > 8192) return false;
    LoadScenarioTitles();
    if (g_shortTitles.empty()) return false;

    int e = 0;
    while (e < len && text[e] != '\r' && text[e] != '\n') ++e;
    int n = e;
    while (n > 0 && (text[n-1] == ' ' || text[n-1] == '\t')) --n;
    if (n < 3) return false;
    std::string head(text, n);

    const std::string* hit = nullptr;
    for (const auto& T : g_shortTitles) {
        if ((int)T.size() != n + 1) continue;      // 只可能是「丢了 1 个字节」
        if (T.compare(1, n, head) != 0) continue;
        if (hit && *hit != T) return false;        // 不唯一，宁可不改
        hit = &T;
    }
    if (!hit) return false;

    out.assign(1, (*hit)[0]);
    out.append(text, len);
    return true;
}

// 游戏把标题的「第一个字节」单独复制到按钮缓冲区并写 0 截断，
// 于是按钮收到 len=1 的孤立高位字节（实机 blank#：CE / BD / C8 / BB），
// DecodeChar 静默丢弃 → 按钮完全空白。这里把它还原成完整标题。
//
// 2026-11-15 修正（「萨莱诺」被显示成「塞」的根因）：
//   旧版在候选不唯一时取「所有候选的最长公共前缀」，而 C8 开头的候选
//   （萨莱诺/萨凡纳/塞洛高地）只有首字节相同，公共前缀 = 1 字节，
//   代码又强行补到 2 字节 → 取了第一个候选的头一个 GBK 字符「塞」。
//   这是把一个「半个字符」硬凑成字，必然出错。现在一律只输出完整标题。
//
//   另外 text[2..] 不是原标题的剩余字节：所有 D# 的 p 都等于 0x538960，
//   那是游戏的全局格式化缓冲 Source，里面留的是「上一次绘制」的残留
//   （实测 tail='哈努夫' 来自 CIECH.TXT「切哈努夫」、'奥穆斯' 来自
//    SUOMUSS.TXT「苏奥穆斯萨尔米」）。拿它消歧是错的，已删除。
static bool RecoverLeadByteOnly(const char* text, int len,
                                int x, int y, bool fromPrintf, std::string& out)
{
    // 实机 fmt#187 证实碎片也可能是「一个完整汉字」(len=2, CF F2 =「向」)：
    // 只认 len==1 会让「向圣洛推进」按钮永远只画出「向」。
    if (!g_cfg.repairFragments || !text || len < 1 || len > 2) return false;
    BYTE b0 = (BYTE)text[0];
    if (b0 < 0x81) return false;
    LoadScenarioTitles();
    if (g_shortTitles.empty()) return false;

    // ① 精确来源：本次 PrintfText → vsprintf 的第一个可变参数（完整标题）。
    //    只认返回地址 0x43B206，且首字节必须一致、内容必须是登记过的标题，
    //    避免把上一次绘制的标题张冠李戴到本次按钮上。
    if (g_lastFullLen > len && g_lastFmtB0 == b0 &&
        (fromPrintf || g_lastFmtLen == len) &&
        memcmp(g_lastFullTitle, text, (size_t)len) == 0) {  // 碎片必须是完整标题的前缀
        out.assign(g_lastFullTitle, g_lastFullLen);   // 已由 HookVsprintf 校验为登记标题
        if (g_cfg.debug && x >= 0 && g_leadBtnLog < 2000) {
            Log("leadbtn#%d (%d,%d) b0=%02X 精确(fmt缓冲拼回)=", g_leadBtnLog, x, y, b0);
            LogText(out.c_str(), (int)out.size(), 48);
            Log("\n");
            ++g_leadBtnLog;
        }
        return true;
    }

    // 候选只取 SCENARIO 标题：GUI97 行里 C8 开头的有「瑞典/软目标/燃料/
    // 三月/取消…」一大堆，混进来只会把关卡按钮显示成 GUI 词。
    std::vector<const std::string*> cand;
    auto collect = [&](const std::vector<std::string>& pool) {
        for (const auto& T : pool)
            if (!T.empty() && (BYTE)T[0] == b0 &&
                std::find(cand.begin(), cand.end(), &T) == cand.end())
                cand.push_back(&T);
    };
    collect(g_campaignTitles);   // 战役标题优先
    collect(g_shortTitles);

    const std::string* hit = nullptr;
    int pick = -1;
    if (cand.size() == 1) {
        hit = cand[0]; pick = 0;                       // 唯一 → 精确还原
    }
    // 2026-11-15 误伤修复：旧版在候选 >1 时用 (y-252)/23 % 候选数 硬分配标题，
    // 那是把「候选表顺序」当成「游戏关卡顺序」，纯属巧合，实测把 y=413 的
    // 「橡树岭」显示成「向圣洛推进」。现在多候选一律不猜整个标题。
    // 折中：若所有候选的第二个字节相同（即首字是同一个汉字），
    // 只显示这一个完整汉字 —— 宁可短，也绝不把 A 关卡写成 B 关卡。
    if (!hit && cand.size() > 1 && cand[0]->size() >= 2) {
        BYTE b1 = (BYTE)(*cand[0])[1];
        bool same = (b1 >= 0x40 && b1 != 0x7F);
        for (const auto* T : cand)
            if (T->size() < 2 || (BYTE)(*T)[1] != b1) { same = false; break; }
        if (same) {
            out.assign(1, (char)b0);
            out.append(1, (char)b1);
            hit = cand[0];
            pick = -2;                                  // 标记「仅首字，未猜标题」
        }
    }

    if (hit && pick != -2) out = *hit;

    if (g_cfg.debug && x >= 0 && g_leadBtnLog < 2000) {
        Log("leadbtn#%d (%d,%d) b0=%02X 候选=%d", g_leadBtnLog, x, y, b0, (int)cand.size());
        for (size_t i = 0; i < cand.size(); ++i) {
            Log(" [%d]", (int)i);
            LogText(cand[i]->c_str(), (int)cand[i]->size(), 40);
        }
        if (pick == -2) Log(" -> 仅首字=");
        else Log(" -> 选用[%d]=", pick);
        if (hit) LogText(out.c_str(), (int)out.size(), 40);
        else Log("(不还原)");
        Log("\n");
        ++g_leadBtnLog;
    }
    return hit != nullptr;
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

// ===================== 条件背景擦除 (Backplate) =====================
// IDA: sub_43B2C2 逐字调 sub_428813 透明叠加，本身不擦背景；游戏按
// *(fontObj+8)=8px 的脏矩形刷新文字区。12px 汉字下面 4px 落在矩形外，
// 上一串字的下半截擦不掉，反复重绘就糊成一团。
//
// 上一版「墨迹自撤销」按 (表面,x,y,墨色) 分块、逐点写回原色，真机无效：
//   * 游戏把同一串字画两遍做描边（ink=00/0F 暗层 + ink=13/E3 亮层），
//     两层互相覆盖后，亮层旧笔画在下一帧已变成暗色，
//     `*q == d.ink` 判定失效，永远擦不掉；
//   * `d.old` 常是另一层的墨色，恢复回去等于把旧字又写一遍；
//   * 实机日志 75 个绘制块里有 23 个只画过一次，从没有机会被撤销。
//
// 现改为不依赖「块身份」：全屏位图标记哪些像素是我们写过的笔画，
// 每次绘制一串文字前，把该串紧凑矩形内「仍呈文字色」的标记点填成
// 矩形像素众数（背景色），清标记，再画新字。
//   * 只动我们写过的点 → 底纹 / 边框 / 图标零破坏，不会重演灰块；
//   * 不比较本块墨色 → 双层描边、坐标漂移、只画一次的块都能擦掉；
//   * 描边两层靠「同串配对」识别，只在第一层擦一次，避免抹掉刚画好的
//     暗色描边。
struct LastLayer { void* surf; int x, y; DWORD hash, ms; };
static LastLayer g_lastLayer = {};
static BYTE*  g_inkMark = nullptr;
static int    g_inkW = 0, g_inkH = 0;
static BYTE   g_isInk[256];
static DWORD  g_backplatePx = 0;
static bool   g_inkPaletteReady = false;

static void InitInkPalette()
{
    if (g_inkPaletteReady) return;
    g_inkPaletteReady = true;
    // 实机日志里出现过的文字色；其余墨色在绘制时动态登记。
    const BYTE seed[] = { 0x00, 0x0F, 0x13, 0x6D, 0xE3 };
    for (BYTE c : seed) g_isInk[c] = 1;
}
static void RegisterInk(BYTE c) { InitInkPalette(); g_isInk[c] = 1; }

static void EnsureInkMap(int w, int h)
{
    if (!g_cfg.inkUndo || w <= 0 || h <= 0) return;
    if (g_inkMark && g_inkW == w && g_inkH == h) return;
    free(g_inkMark);
    g_inkMark = (BYTE*)calloc((size_t)w * h, 1);
    g_inkW = w; g_inkH = h;
}

static DWORD NowMs()
{
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    if (freq.QuadPart <= 0) return GetTickCount();
    return (DWORD)(c.QuadPart * 1000 / freq.QuadPart);
}

static DWORD HashText(const char* t)
{
    DWORD h = 2166136261u;
    for (const unsigned char* q = (const unsigned char*)t; *q; ++q) {
        h ^= *q;
        h *= 16777619u;
    }
    return h;
}

// 描边两次调用间隔通常 <0.1ms，跨帧 >=16ms；4ms 阈值稳妥区分。
static bool IsSecondLayer(void* surf, int x, int y, const char* text)
{
    DWORD now = NowMs(), h = HashText(text);
    int dx = x - g_lastLayer.x; if (dx < 0) dx = -dx;
    int dy = y - g_lastLayer.y; if (dy < 0) dy = -dy;
    bool second = g_lastLayer.surf == surf && dx <= 2 && dy <= 2
               && h == g_lastLayer.hash
               && (now - g_lastLayer.ms) < 4;
    g_lastLayer.surf = surf; g_lastLayer.x = x; g_lastLayer.y = y;
    g_lastLayer.hash = h;     g_lastLayer.ms = now;
    return second;
}

// 擦掉矩形内我们写过、且当前仍呈文字色的像素。背景色取矩形内「非文字色」
// 像素的众数（背景在文字矩形里天然占多数）；若整块都是文字色则退到全局众数。
static int ClearTextBackplate(BYTE* pixels, int pitch, int surfH,
                              int x0, int y0, int x1, int y1)
{
    if (!g_cfg.inkUndo || !pixels || !g_inkMark) return 0;
    InitInkPalette();
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= pitch)  x1 = pitch  - 1;
    if (y1 >= surfH)  y1 = surfH - 1;
    if (x1 <= x0 || y1 <= y0) return 0;
    // clip 异常时会得到整屏矩形，直接跳过，避免把满屏文字误刷成单色。
    if ((long long)(x1 - x0 + 1) * (y1 - y0 + 1) >
        (long long)pitch * surfH / 2) return 0;

    int hist[256] = { 0 };
    for (int yy = y0; yy <= y1; ++yy) {
        const BYTE* row = pixels + (size_t)yy * pitch;
        for (int xx = x0; xx <= x1; ++xx) ++hist[row[xx]];
    }
    int bg = -1, best = 0, bgAny = -1, bestAny = 0;
    for (int c = 0; c < 256; ++c) {
        if (hist[c] > bestAny) { bestAny = hist[c]; bgAny = c; }
        if (!g_isInk[c] && hist[c] > best) { best = hist[c]; bg = c; }
    }
    if (bg < 0) bg = bgAny;
    if (bg < 0) return 0;

    int n = 0;
    for (int yy = y0; yy <= y1; ++yy) {
        BYTE* row = pixels + (size_t)yy * pitch;
        BYTE* mk  = g_inkMark + (size_t)yy * g_inkW;
        for (int xx = x0; xx <= x1; ++xx) {
            if (!mk[xx]) continue;
            mk[xx] = 0;
            if (g_isInk[row[xx]]) { row[xx] = (BYTE)bg; ++n; }
        }
    }
    g_backplatePx += n;
    return n;
}

// 画一个字形。ink = 当前文字色（colorTable[0]）。
// 坐标：penX/curY 已含 clipX/clipY；baselineY = topY + 字体 ascent。
// 返回写入的像素数（诊断用）。
static int DrawGlyph(WCHAR wc, int penX, int topY, int baselineY, int outH, BYTE ink,
                     BYTE* pixels, int pitch, int minX, int maxX, int minY, int maxY,
                     int& advOut)
{
    // 兜底闸门：假名 / 制表方块 / 替换符只可能是 GBK 错位拼出来的，
    // 正常中文译文不会用到。即使某个碎片没被登记表命中，也不让它画到屏幕上。
    if ((wc >= 0x3040 && wc <= 0x30FF) ||
        (wc >= 0x2500 && wc <= 0x259F) || wc == 0xFFFD) {
        advOut = g_pxHeight > 0 ? g_pxHeight : 12;
        return 0;
    }

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

    // 纵向压缩：把 srcBoxH 行映射到 boxH 行。多个源行落到同一目标行时
    // 都写同一个 ink，等价于按位 OR —— 笔画只会合并、不会丢失。
    // 基线 / 原点同步缩放，保证压缩后的字形完整落在 [topY, topY+outH)。
    int srcH    = g_pxHeight > 0 ? g_pxHeight : 12;
    int srcBoxH = (int)gm.gmBlackBoxY;
    int boxH    = srcBoxH;
    if (outH > 0 && outH < srcH) {
        int baseOff = baselineY - topY;                 // 调用处传的是 g_ascent
        int newBase = topY + baseOff * outH / srcH;
        int newOrig = (int)gm.gmptGlyphOrigin.y * outH / srcH;
        top   = newBase - newOrig;
        boxH  = srcBoxH * outH / srcH;
        if (boxH < 1) boxH = 1;
    }

    int written = 0;
    for (int r = 0; r < srcBoxH; ++r)
    {
        int d  = (boxH == srcBoxH) ? r : (r * boxH / srcBoxH);
        int sy = top + d;
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
            BYTE* dst = &dstRow[sx];
            if (g_inkMark && g_inkW == pitch &&
                sy >= 0 && sy < g_inkH && sx >= 0 && sx < g_inkW)
                g_inkMark[(size_t)sy * g_inkW + sx] = 1;
            *dst = ink;
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
    const int rawMinX = minX, rawMinY = minY, rawMaxX = maxX, rawMaxY = maxY;
    // 简报抬头 / HUD 用的某些 surface（实机 s=001AF990）字段不构成有效裁剪区，
    // 逐像素裁剪会把所有位置判到界外 → 完整中文一个像素都写不出（blank#8..25）。
    // 原 blit sub_428813 此时返回 -2 也不画；但字形是我们自己合成的，放宽到整张
    // 表面即可显示，且这些调用的 (x,y) 本身都在 640x480 内，不会画到屏外。
    // maxX/maxY 为 0 时 minX==maxX，字面裁剪同样是「一个像素都画不出」，
    // 所以判定要带等号，不能只判 maxX < minX。
    bool clipFixed = false;
    if (maxX <= minX || maxY <= minY || maxX <= 0 || maxY <= 0) {
        minX = 0; minY = 0; maxX = pitch - 1; maxY = surfH - 1;
        clipFixed = true;
    }

    // 条件背景擦除：先把上一次这串字写在游戏 8px 刷新区之外的笔画擦掉。
    int len  = (int)strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) --len;
    RegisterInk(ink);
    EnsureInkMap(pitch, surfH);

    // 普通文本用完整 12px 方字；擦除负责清掉超出游戏 8px 脏矩形的部分。
    // 行距随之放宽到 drawH+1，避免串内 \n 换行时上下两行叠在一起。
    int drawH   = EffectiveTextDrawHeight();
    int lineAdv = (g_lineH > 0 ? g_lineH + 2 : 10);
    if (lineAdv < drawH + 1) lineAdv = drawH + 1;

    if (!IsSecondLayer(surface, x, y, text)) {
        int baseX = x + xoff, baseY = y + yoff;
        int estW = 0, nl = 0;
        for (int k = ChooseGBKStart(text, len); k < len; ) {
            WCHAR wc; int n = DecodeChar(text, k, len, wc);
            if (!n) break;
            if (wc == L'\n') { ++nl; estW = 0; }
            else if (wc != L'\r' && wc >= 0x20) estW += GlyphAdvance(wc);
            k += n;
        }
        // 向下多覆盖 8px：旧字比新字高/长、以及描边层的 1px 偏移都吃进来。
        int rectH = (nl + 1) * lineAdv + drawH + 8;
        ClearTextBackplate(pixels, pitch, surfH,
                           baseX - 2, baseY - 2,
                           baseX + estW + 32, baseY + rectH);
    }

    int curX = x + xoff, curY = y + yoff;
    int lines = 0, totalPx = 0;

    // 起点可能落在 GBK 双字节中间（游戏按字节截断时），先自同步再画
    for (int i = ChooseGBKStart(text, len); i < len; )
    {
        WCHAR wc; int n = DecodeChar(text, i, len, wc);
        if (!wc) { i += n; continue; }

        if (wc == L'\n') { curX = x + xoff; curY += lineAdv; ++lines; i += n; continue; }

        int adv = 0;
        if (wc < 0x20) {                 // 控制字符（原函数也不画）
            adv = GlyphAdvance(wc);
        } else {
            int topY = curY + g_cfg.vOffset;
            totalPx += DrawGlyph(wc, curX, topY, topY + g_ascent, drawH, ink,
                                 pixels, pitch, minX, maxX, minY, maxY, adv);
        }
        curX += adv;
        i += n;
    }

    // 空白诊断：串里有可画字符却一个像素都没写 → 记录下来定位「空白按钮」。
    // 菜单每帧重绘会产生大量重复，按 (x,y,字节) 去重，才能覆盖到后面的界面。
    if (g_cfg.debug && totalPx == 0 && g_blankLog < 300) {
        bool inkable = false;
        for (int k = 0; k < len; ++k) if ((BYTE)text[k] > 0x20) { inkable = true; break; }
        if (inkable) {
            std::string key = std::to_string(x) + "," + std::to_string(y) + ":";
            char hx[4];
            for (int k = 0; k < 24 && k < len; ++k) {
                _snprintf_s(hx, sizeof(hx), _TRUNCATE, "%02X", (BYTE)text[k]);
                key += hx;
            }
            if (std::find(g_blankSeen.begin(), g_blankSeen.end(), key) == g_blankSeen.end()) {
                g_blankSeen.push_back(key);
            Log("blank#%d (%d,%d) len=%d txt=", g_blankLog, x, y, len);
            for (int k = 0; k < 24 && k < len; ++k) Log("%02X ", (BYTE)text[k]);
            LogText(text, len, 24);
            Log(" surf=%p clip=(%d,%d) max=(%d,%d) pitch=%d h=%d fixed=%d\n",
                surface, rawMinX, rawMinY, rawMaxX, rawMaxY, pitch, surfH, clipFixed ? 1 : 0);
                ++g_blankLog;
            }
        }
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
// 自动换行渲染器（EXE sub_43B402）的中文实现。
//
// 【关键】第一个参数不是「最大行数」，而是「起始行 / 滚动偏移」：
//   IDA 0x43B65D  lines >= arg1 才调用 blit 绘制
//   IDA 0x43B6B8  lines >= arg1 时换行才累加 yOff（前 arg1 行不占位）
//   IDA 0x43B439  返回 [ebp-20h] = 总行数
// 游戏用它做简报翻页：日志里的 0/2/3/4/5 就是滚动起始行。
// 早期误当成 maxLines、判断写反，且只在「超限」时累加 yOff，
// 结果 startLine=0 时整段空白、startLine>0 时所有行叠在同一 Y
// ——表现为「文字不显示 / 换行错乱 / 字只剩上半截」。
static short WrapDrawCJK(short startLine, const unsigned char* colorTable,
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
    int maxW = ds->maxX - ds->clipX + 1;
    int maxH = ds->maxY - ds->clipY + 1;
    if (maxW <= 0 || maxH <= 0) { if (weLocked && g_unlockFn) g_unlockFn(); return 0; }

    // 简报每次都是整页全量重绘：开头一次性擦掉上一页留在刷新区外的笔画。
    // 只动我们写过的点，不会像早期「整块刷单色」那样把底纹抹成灰块。
    RegisterInk(ink);
    EnsureInkMap(pitch, surfH);
    g_lastLayer.surf = nullptr;   // 重置描边配对，强制本页执行擦除
    ClearTextBackplate(pixels, pitch, surfH, minX, minY, maxX, maxY);
    int drawH  = EffectiveDrawHeight();   // 默认 10px，行距 10px 下的最方整高度
    int glyphH = drawH;
    int lineH  = (g_lineH > 0 ? g_lineH : 8) + 2;
    if (lineH < glyphH) lineH = glyphH;
    int len = (int)strlen(text);
    // 译文为对齐原文长度在行尾补了空格，排版时忽略整段末尾的填充
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) --len;

    // 游戏按字节分页时，text 起点可能落在 GBK 双字节中间（日志 W#2 以 0xFA 开头）。
    // 用自同步选起点，比旧的「只看首两字节」更可靠：既不会把合法汉字误判成碎片，
    // 也不会让首字节是合法假名区(A4/A5)的碎片被硬解成日文。
    int i = ChooseGBKStart(text, len);

    int lines = 0, curW = 0, yOff = 0, px = 0;
    bool stop = false;

    for (; i < len && !stop; )
    {
        WCHAR wc; int n = DecodeChar(text, i, len, wc);
        if (!wc) { i += n; continue; }
        if (wc == L'\r') { i += n; continue; }   // CRLF 里的 CR 不参与排版

        if (wc == L'\n') {
            curW = 0;
            if (lines >= startLine) {
                yOff += lineH;
                if (glyphH + yOff > maxH) { stop = true; break; }
            }
            ++lines;
            i += n;
            continue;
        }

        int adv = GlyphAdvance(wc);

        // 断行：CJK 任意处可断；ASCII 按整词度量（复刻原函数 0x43B4F4 起）
        bool wrap = false;
        if (wc >= 0x2E80) {
            wrap = (curW > 0 && curW + adv > maxW);
        } else if (wc != L' ') {
            int wordW = 0;
            for (int k = i; k < len; ) {
                WCHAR w2; int n2 = DecodeChar(text, k, len, w2);
                if (w2 == L'\n' || w2 == L'\r' || w2 == L' ' || w2 >= 0x2E80) break;
                wordW += GlyphAdvance(w2);
                k += n2;
            }
            if (wordW <= 0) wordW = adv;
            wrap = (curW > 0 && curW + wordW > maxW);
        } else {
            wrap = (curW > 0 && curW + adv > maxW);
        }

        if (wrap) {
            if (lines >= startLine) {
                yOff += lineH;
                if (glyphH + yOff > maxH) { stop = true; break; }
            }
            curW = 0; ++lines;
            if (wc == L' ') { i += n; continue; }   // 行首不留空格
        }

        if (lines >= startLine && wc != L' ') {
            int topY = yOff + ds->clipY + g_cfg.vOffset;
            px += DrawGlyph(wc, curW + ds->clipX, topY, topY + g_ascent, drawH, ink,
                            pixels, pitch, minX, maxX, minY, maxY, adv);
        }
        curW += adv;
        i += n;
    }

    // flag 非 0：末尾补 '|' 提示「还有更多」（原函数 0x43B722）
    if (!stop && flag && lines >= startLine) {
        int adv = GlyphAdvance(L'|');
        if (curW > 0 && curW + adv > maxW) {
            yOff += lineH;
            if (glyphH + yOff > maxH) stop = true;
            if (!stop) { curW = 0; ++lines; }
        }
        if (!stop) {
            int topY = yOff + ds->clipY + g_cfg.vOffset;
            px += DrawGlyph(L'|', curW + ds->clipX, topY, topY + g_ascent, drawH, ink,
                            pixels, pitch, minX, maxX, minY, maxY, adv);
        }
    }

    if (g_wantDump) DumpSurfaceOnce(pixels, pitch, surfH);
    if (weLocked && g_unlockFn) g_unlockFn();

    if (g_cfg.debug && dbgWrap < 8) {
        Log("wrap #%d start=%d maxW=%d maxH=%d lineH=%d drawH=%d -> lines=%d px=%d\n",
            dbgWrap, startLine, maxW, maxH, lineH, drawH, lines, px);
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

// 抓 PrintfText 的格式化参数。IDA 0x43B19F：
//   vsprintf(Source, fmt, ArgList=&[ebp+1Ch]) → DrawText(..., Source, ...)
// 只要第一个可变参数是完整中文标题，DrawText 侧就能精确还原 1 字节按钮，
// 不再依赖「按 y 猜候选」这种必然出错的启发式。
static int __cdecl HookVsprintf(char* buf, const char* fmt, va_list args)
{
    g_lastFmtLen = 0; g_lastFmtB0 = 0; g_lastFullLen = 0;
    if (buf == g_sourceBuf && fmt && args && !IsBadReadPtr(fmt, 2)) {
        // 实机确认：游戏把「文字本身」当 fmt 传（无 %s 占位符），
        // 而且这个 fmt 已经被按字节切碎：BB / B6 AC BC / BF A8 C4 …
        int n = 0;
        while (n < 64 && !IsBadReadPtr(fmt + n, 1) && fmt[n]) ++n;
        BYTE b0 = (BYTE)fmt[0];
        if (n >= 1) {
            memcpy(g_lastFmtText, fmt, n);
            g_lastFmtText[n] = 0;
            g_lastFmtLen = n;
            g_lastFmtB0 = b0;
        }

        // 实机 mem= 证实：fmt 指向 16 字节按钮标签缓冲（碎片 + 一串 0），
        // 其后 fmt+16 起就是该关 SCENARIO 文件的完整原文（标题行 + CRLF + 数据）。
        // 少数情况下标题行不在最前，所以在后面 256 字节里精确搜索：
        //   · 必须与登记标题逐字节相等（后随 0 / CRLF / 空格，不能是更长串的中段）
        //   · 必须以传入的碎片为前缀
        // 命中即 100% 精确，绝不按坐标猜；命中不了就什么都不改。
        if (n >= 1 && n <= 8 && b0 >= 0x81) {
            LoadScenarioTitles();
            // 旧版只在 fmt+16..+271 里找，实机 fmt#222 的 mem 却显示完整标题
            // 可能在 fmt 之前（前一个文本缓冲的尾部），或隔着 .SCN 文件名更远处。
            // 这里在同「可读内存区」内前后各扫一段；VirtualQuery 保证不跨页踩空。
            const BYTE* base = (const BYTE*)fmt;
            const BYTE* lo = base, *hi = base, *re = base;
            MEMORY_BASIC_INFORMATION mbi = { 0 };
            if (VirtualQuery(base, &mbi, sizeof(mbi))) {
                const BYTE* rb = (const BYTE*)mbi.BaseAddress;
                re = rb + mbi.RegionSize;
                lo = (base - 2048 < rb) ? rb : base - 2048;
                hi = (base + 4096 > re - 64) ? re - 64 : base + 4096;
            }
            // 先按首字节筛候选，避免每个位置都遍历 250+ 标题
            std::vector<const std::string*> cand;
            const std::vector<std::string>* pools[2] = { &g_campaignTitles, &g_shortTitles };
            for (int pi = 0; pi < 2; ++pi)
                for (const auto& T : *pools[pi])
                    if (T.size() > (size_t)n && (BYTE)T[0] == b0 &&
                        std::find(cand.begin(), cand.end(), &T) == cand.end())
                        cand.push_back(&T);
            struct Hit { int dist; const std::string* t; };
            std::vector<Hit> hits;
            if (hi > lo && re > hi && !cand.empty()) {
                for (const BYTE* q = lo; q < hi; ++q) {
                    if (*q != b0) continue;
                    if (q + n > re || memcmp(q, fmt, (size_t)n) != 0) continue;
                    for (const std::string* T : cand) {
                        int L = (int)T->size();
                        if (q + L + 1 > re) continue;
                        if (memcmp(q, T->data(), (size_t)L) != 0) continue;
                        BYTE nx = q[L];
                        if (nx != 0 && nx != 0x0D && nx != 0x0A && nx != ' ' && nx != '\t') continue;
                        int dist = (int)(q > base ? q - base : base - q);
                        bool dup = false;
                        for (auto& h : hits)
                            if (*h.t == *T) { if (dist < h.dist) h.dist = dist; dup = true; break; }
                        if (!dup) hits.push_back({ dist, T });
                    }
                }
            }
            if (!hits.empty()) {
                std::sort(hits.begin(), hits.end(),
                          [](const Hit& a, const Hit& b) { return a.dist < b.dist; });
                // 多候选时取「内存里离 fmt 最近的登记标题」。
                // 实机 fmtamb# 证据：同一 b0=C8 的候选距离会随 fmt 缓冲布局整体翻转
                //   （512/672/768 与 2528/3808/3968），说明它们来自不同页面各自的
                //   标题数组，最近的一项就是当前按钮；用户实测「萨凡纳」「萨莱诺 -
                //   重返欧洲」正是各自布局里最近的那条。超过 4096 字节视为无关内存。
                bool pick = hits.size() == 1 || hits[0].dist <= 4096;
                if (pick) {
                    const std::string* T = hits[0].t;
                    // 多候选时 nearest 只是「原选中」：b0=C8 的两个相邻按钮
                    // 在实机里被稳定地互换（fmtamb# 两种内存布局），
                    // 用外部校订表按真机确认的对应关系纠正，不动唯一命中路径。
                    if (hits.size() > 1) {
                        if (const std::string* fx = ApplyFragTitleFix(b0, *T)) T = fx;
                    }
                    memcpy(g_lastFullTitle, T->data(), T->size());
                    g_lastFullTitle[T->size()] = 0;
                    g_lastFullLen = (int)T->size();
                }
                if (g_cfg.debug && g_fmtLog < 3000 && hits.size() > 1) {
                    Log("fmtamb# b0=%02X n=%d -> %s ", b0, n, pick ? "nearest" : "放弃");
                    for (const auto& h : hits) {
                        Log(" d=%d ", h.dist);
                        LogText(h.t->c_str(), (int)h.t->size(), 24);
                    }
                    if (!IsBadReadPtr(fmt, 32)) {
                        Log(" p=%p mem=", (void*)fmt);
                        for (int k = 0; k < 32; ++k) Log("%02X ", (BYTE)fmt[k]);
                    }
                    Log("\n");
                }
            }
        }

        // 只记录「碎片」类调用，避免主菜单长文本把日志上限吃满
        bool isFrag = (n <= 8 && b0 >= 0x81);
        if (g_cfg.debug && isFrag && g_fmtLog < 3000) {
            Log("fmt#%d p=%p n=%d fmt=", g_fmtLog, (void*)fmt, n);
            LogText(fmt, n, 24);
            Log(" full=%d ", g_lastFullLen);
            if (g_lastFullLen) LogText(g_lastFullTitle, g_lastFullLen, 48);
            else Log("-");
            // 未命中时附上缓冲原文，便于定位「仍显示空白」的按钮
            if (!g_lastFullLen && !IsBadReadPtr(fmt, 48)) {
                Log(" mem=");
                for (int k = 0; k < 40; ++k) Log("%02X ", (BYTE)fmt[k]);
            }
            Log("\n");
            ++g_fmtLog;
        }
    }
    // 必须用未修改的 args 调用原函数，否则格式化结果全乱
    return g_origVsprintf ? g_origVsprintf(buf, fmt, args) : 0;
}

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
    void* caller = _ReturnAddress();   // 谁调用了绘制：IDA 里直接查这个地址

    // ★ 原函数 sub_43B2C2 的前两个形参是 __int16（IDA 反编译确认：
    //   __int16 __cdecl sub_43B2C2(__int16 a1, __int16 a2, int a3, char*, int)）。
    //   调用点分两类：0x43BCE4 用 movsx word ptr 压干净的 16 位；
    //   0x43BD0D / 0x43B202 直接 push dword，高 16 位是栈上残留
    //   （实机见到 0x0043 / 0x0045 / 0x001A / 0xE580…）。
    //   原函数只取低 16 位所以没事；本 hook 若按 int 读，就会把
    //   y=4391031(0x00430077) 当坐标，整串被 clip 掉 → 文字空白。
    //   更隐蔽的是菜单文字画两遍（暗色描边 + 亮色主体）：描边那遍坐标干净、
    //   主体那遍带垃圾高位，结果只剩黑描边 →「字变成很黑的黑色，几乎看不清」。
    //   这里统一按有符号 16 位还原，日志同时保留原始 32 位值便于核对。
    const int rawX = x, rawY = y;
    x = (short)(x & 0xFFFF);
    y = (short)(y & 0xFFFF);

    if (!text) return g_origDrawText(x, y, colorTable, text, surface);
    PatchTextTable();          // 保证「一行一项」的表已就位
    std::string repaired;      // 还原后的完整标题，生命周期覆盖到 RenderStringCJK 返回
    // 菜单画到第 100 次时存一张整屏，便于核对排版（前 3 次是标题画面）
    if (g_cfg.dumpFrame && g_dbgDraw == 100) g_wantDump = true;
    bool hasCJK = false;
    for (const char* p = text; *p; ++p) { if ((BYTE)*p >= 0x81) { hasCJK = true; break; } }
    // 英文地名/装备/姓氏/日期：只在显示层换中文，磁盘数据文件保持英文
    std::string termOut;
    if (!hasCJK && ReplaceDisplayTerms(text, termOut)) {
        text = termOut.c_str();
        hasCJK = true;
    }
    if (!hasCJK) {
        if (g_cfg.debug && g_dbgAscii < 4000) {
            int n = (int)strlen(text);
            Log("A#%d (%d,%d) ret=%08X s=%p len=%d txt=", g_dbgAscii, x, y,
                (DWORD)(DWORD_PTR)caller, surface, n);
            for (int k = 0; k < 40 && k < n; ++k) Log("%02X ", (BYTE)text[k]);
            LogText(text, n, 40);
            Log("\n");
            ++g_dbgAscii;
        }
        return g_origDrawText(x, y, colorTable, text, surface);
    }

    if (g_cfg.debug && g_dbgDraw < 4000) {
        int n = (int)strlen(text);
        Log("D#%d (%d,%d) ret=%08X s=%p p=%p len=%d ink=%02X txt=", g_dbgDraw,
            x, y, (DWORD)(DWORD_PTR)caller, surface, text, n,
            colorTable ? colorTable[0] : 0xFF);
        for (int k = 0; k < 40 && k < n; ++k) Log("%02X ", (BYTE)text[k]);
        LogText(text, n, 40);
        if (rawX != x || rawY != y)
            Log(" [xy16 raw=%08X,%08X]", (unsigned)rawX, (unsigned)rawY);
        Log("\n");
    }
    ++g_dbgDraw;

    // 孤立首字节按钮（len=1 的 CE / BD 等）先单独还原，否则会画成完全空白。
    std::string leadBtn;
    bool leadDone = RecoverLeadByteOnly(text, (int)strlen(text), x, y,
                                        (DWORD)(DWORD_PTR)caller == RET_PRINTFTEXT, leadBtn);
    if (leadDone) text = leadBtn.c_str();

    // 游戏把关卡标题按字节切碎后才交给绘制，这里换回完整标题。
    // 只有命中登记表且唯一匹配的碎片才改；完整标题、GUI97 串一律不动。
    if (!leadDone && RepairFragment(text, (int)strlen(text), repaired)) {
        if (g_cfg.debug && g_repairLog < 2000) {
            int nr = (int)strlen(text);
            Log("repair#%d: ", g_repairLog);
            for (int k = 0; k < 32 && k < nr; ++k) Log("%02X ", (BYTE)text[k]);
            LogText(text, nr, 32);
            Log(" -> ");
            LogText(repaired.c_str(), (int)repaired.size(), 64);
            Log("\n");
            ++g_repairLog;
        }
        text = repaired.c_str();
    }

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
    // 显示层译名替换：与 HookDrawText 保持同一套规则，让居中宽度按中文算
    std::string termOut;
    if (!hasCJK && ReplaceDisplayTerms(text, termOut)) {
        text = termOut.c_str();
        hasCJK = true;
    }
    if (!hasCJK) return g_origMeasure ? g_origMeasure(text) : 0;

    if (!EnsureFont()) return 0;

    // 与绘制保持一致：碎片还原后按完整标题测宽，避免游戏按残缺宽度居中
    std::string repaired;
    bool fixed = RecoverLeadByteOnly(text, (int)strlen(text), -1, -1, false, repaired);
    if (!fixed) fixed = RepairFragment(text, (int)strlen(text), repaired);
    if (fixed) text = repaired.c_str();

    int len = (int)strlen(text), w = 0;
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) --len;  // 去掉行尾填充空格
    for (int i = ChooseGBKStart(text, len); i < len; ) {
        WCHAR wc; int n = DecodeChar(text, i, len, wc);
        if (wc) w += GlyphAdvance(wc);
        i += n;
    }
    if (g_cfg.debug && g_dbgMeasure < 4000) {
        Log("measure CJK #%d len=%d -> %d txt=", g_dbgMeasure, len, w);
        for (int k = 0; k < 24 && k < len; ++k) Log("%02X ", (BYTE)text[k]);
        LogText(text, len, 24);
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
static short __cdecl HookWrapDraw(short startLine, const unsigned char* colorTable,
                                  const char* text, void* surface, char flag)
{
    if (!text || !g_origWrap)
        return g_origWrap ? g_origWrap(startLine, colorTable, text, surface, flag) : 0;
    bool hasCJK = false;
    for (const char* p = text; *p; ++p) { if ((BYTE)*p >= 0x81) { hasCJK = true; break; } }
    if (!hasCJK) return g_origWrap(startLine, colorTable, text, surface, flag);
    // 剧本简报验证用：打印传进来的文本字节，可判断中文是否被加载器切碎
    if (g_cfg.debug && g_dbgWrap < 200) {
        int n = (int)strlen(text);
        Log("W#%d start=%d len=%d txt=", g_dbgWrap, startLine, n);
        for (int k = 0; k < 32 && k < n; ++k) Log("%02X ", (BYTE)text[k]);
        LogText(text, n, 32);
        Log("\n");
        ++g_dbgWrap;
    }
    if (g_cfg.dumpFrame && g_dbgWrap == 3) g_wantDump = true;
    // 长文本首字节可能被游戏吃掉（"线十字军" / "军柏林"），先补回再排版。
    // 只动第一行那一个字节，其余内容原样保留。
    std::string leadFixed;
    if (FixMissingLeadByte(text, (int)strlen(text), leadFixed)) {
        if (g_cfg.debug && g_leadLog < 40) {
            Log("lead#%d: %02X -> %02X len=%d\n", g_leadLog,
                (BYTE)text[0], (BYTE)leadFixed[0], (int)strlen(text));
            ++g_leadLog;
        }
        text = leadFixed.c_str();
    }
    // 短串才可能是标题碎片（简报正文通常上百字节，RepairFragment 会直接跳过）
    std::string repaired;
    if (RepairFragment(text, (int)strlen(text), repaired)) text = repaired.c_str();
    return WrapDrawCJK(startLine, colorTable, text, surface, flag);
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
            g_sourceBuf = (char*)((BYTE*)hGame + (ADDR_SOURCEBUF - IMAGE_BASE));
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
        // _vsprintf @0x48D910: mov eax,[esp+4] / sub esp,20h / mov ecx,[esp+2Ch]
        static const BYTE kVsprintfPro[11] = {
            0x8B, 0x44, 0x24, 0x04,
            0x83, 0xEC, 0x20,
            0x8B, 0x4C, 0x24, 0x2C
        };

        // 顺序：先字符查表（兜底），再测宽，再绘制，最后 blit 闸门
        g_charWidth    = (CharWidthFn)   InstallDetour(ADDR_CHARWIDTH, 5, kCharWidthPro, (void*)&HookCharWidth);
        g_origMeasure  = (OrigMeasureFn) InstallDetour(ADDR_MEASURE,   6, kMeasurePro,   (void*)&HookMeasureText);
        g_origDrawText = (OrigDrawTextFn)InstallDetour(ADDR_DRAWTEXT,  6, kDrawPro,      (void*)&HookDrawText);
        g_origWrap     = (OrigWrapFn)    InstallDetour(ADDR_WRAPDRAW,  6, kWrapPro,      (void*)&HookWrapDraw);
        g_origBlit     = (BlitFn)        InstallDetour(ADDR_BLIT,      6, kBlitPro,      (void*)&HookBlit);
        g_origVsprintf = (VsprintfFn)    InstallDetour(ADDR_VSPRINTF, 11, kVsprintfPro, (void*)&HookVsprintf);

        Log("PG2CHS: hook 状态 charwidth=%p measure=%p draw=%p wrap=%p vsprintf=%p\n",
            g_charWidth, g_origMeasure, g_origDrawText, g_origWrap, g_origVsprintf);
        if (!g_origVsprintf) Log("PG2CHS: !! vsprintf hook 未装上，1 字节按钮只能靠唯一候选还原\n");
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
        if (g_log) {
            Log("backplate: 累计擦除 %u 个旧笔画像素，墨迹图 %dx%d\n",
                g_backplatePx, g_inkW, g_inkH);
            free(g_inkMark); g_inkMark = nullptr;
            Log("PG2CHS: 卸载\n"); fclose(g_log); g_log = nullptr;
        }
        break;
    }
    return TRUE;
}
