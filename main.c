/*
 * Azkar Desktop Notifier v2.0
 * Portable, ultra-light Win32 tray app (C + Win32 API, GDI only)
 *
 * GOLDEN RULES:
 *  - 0.0% CPU at idle: blocking GetMessage loop, NO polling, NO PeekMessage.
 *  - SetTimer/KillTimer tied to HWND; destroy timer the instant it's done.
 *  - Every GDI handle (Font, Brush, Pen, DC, Bitmap, Region) freed on every path.
 *  - Layered popup via WS_EX_LAYERED + SetLayeredWindowAttributes + SetWindowRgn.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "resource.h"

/* ═══════════════════════════════ Constants ═══════════════════════════════ */

#define APP_NAME        L"أذكار سطح المكتب"
#define APP_CLASS       L"AzkarNotifierMain"
#define POPUP_CLASS     L"AzkarNotifierPopup"
#define MUTEX_NAME      L"AzkarNotifier_SingleInstance_v2"
#define REG_RUN_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_RUN_VALUE   L"AzkarNotifier"

#define MAX_AZKAR       64
#define MAX_TEXT        512
#define WM_TRAYICON     (WM_APP + 1)

#define TIMER_MAIN      1   /* fires once per interval                    */
#define TIMER_ANIM      2   /* ~20ms, only during show/hide animation     */
#define TIMER_HIDE      3   /* fires once after display duration           */

#define ANIM_NONE       0
#define ANIM_FADE       1
#define ANIM_SLIDE_DOWN 2
#define ANIM_SLIDE_SIDE 3
#define ANIM_POP_SCALE  4
#define ANIM_COUNT      5
#define ANIM_STEPS      13
#define ANIM_MS         20  /* 13 steps × 20ms ≈ 260ms total              */

#define POS_TOP_RIGHT   0
#define POS_TOP_LEFT    1
#define POS_TOP_CENTER  2

#define STYLE_COUNT     8
#define FONT_COUNT      6

/* ══════════════════════════════ Data types ═══════════════════════════════ */

typedef struct {
    WCHAR text[MAX_TEXT];
    int   limit;       /* 0 = unlimited, else max times per day           */
    int   shown;       /* times shown today                               */
} Zikr;

typedef struct {
    Zikr  items[MAX_AZKAR];
    int   count;
    int   style;       /* 0..7                                            */
    int   anim;        /* 0..4                                            */
    int   intervalSec; /* seconds between notifications                   */
    int   durationSec; /* display duration in seconds                     */
    int   startup;     /* auto-start with Windows                         */
    int   lastDate;    /* YYYYMMDD for daily reset                        */
    int   position;    /* 0=Top-Right, 1=Top-Left, 2=Top-Center           */
    int   yOffset;     /* 0-777 px down from work area top                */
    int   xMargin;     /* 0-400 px from screen edge                       */
    int   autoFit;     /* 1=auto-size to text, 0=fixed                    */
    int   fontIndex;   /* 0-5 (Arabic fonts) or 6 (Segoe UI fallback)    */
    int   fontSize;    /* font size in points (8-177)                     */
} Config;

typedef struct {
    COLORREF bg, bg2, fg, border, accent;
    int radius; int borderW; BYTE alpha;
    const WCHAR *nameAr;
} StyleDef;

/* ═══════════════════════════ Global variables ════════════════════════════ */

static Config    g_cfg;
static HINSTANCE g_hInst;
static HWND      g_hMain, g_hPopup, g_hDlg;
static NOTIFYICONDATAW g_nid;
static WCHAR     g_iniPath[MAX_PATH];
static WCHAR     g_exeDir[MAX_PATH];
static WCHAR     g_exePath[MAX_PATH];
static int       g_paused    = 0;
static int       g_nextIndex = 0;
static int       g_selectedInList = -1;
static HFONT     g_cachedFont = NULL;

/* popup state */
static WCHAR     g_popupText[MAX_TEXT];
static int       g_animStep  = 0;
static int       g_animDir   = 0;     /* +1=showing, -1=hiding, 0=idle   */
static int       g_popupX, g_popupY, g_popupW, g_popupH;
static int       g_animStartX, g_animStartY, g_animStartW, g_animStartH;

/* font file info */
static const WCHAR *g_fontFiles[FONT_COUNT] = {
    L"ArefRuqaa-Regular.ttf",
    L"Amiri-Regular.ttf",
    L"ReemKufi-Regular.ttf",
    L"Cairo-Regular.ttf",
    L"Tajawal-Regular.ttf",
    L"Changa-Regular.ttf"
};
static const WCHAR *g_fontFaces[FONT_COUNT + 1] = {
    L"Aref Ruqaa", L"Amiri", L"Reem Kufi",
    L"Cairo", L"Tajawal", L"Changa",
    L"Segoe UI"     /* index 6 = fallback */
};
static BOOL g_fontLoaded[FONT_COUNT] = {0};

/* ── 8 visual themes ─────────────────────────────────────────────────── */
/*     bg             bg2            fg              border          accent          rad bW  α  name */
static const StyleDef g_styles[STYLE_COUNT] = {
    /* 0 Classic */
    { RGB(0,128,0),     RGB(0,128,0),     RGB(255,255,255), RGB(0,90,0),      RGB(0,128,0),      0,  0, 255, L"1 - الكلاسيكي" },
    /* 1 Night */
    { RGB(12,12,12),    RGB(12,12,12),    RGB(120,255,150), RGB(0,200,90),    RGB(0,120,60),     6,  2, 240, L"2 - الليلي" },
    /* 2 Gradient */
    { RGB(0,70,35),     RGB(5,5,5),       RGB(255,215,90),  RGB(180,140,40),  RGB(255,215,90),  10,  1, 255, L"3 - التدرّج" },
    /* 3 Glass */
    { RGB(28,32,30),    RGB(28,32,30),    RGB(235,235,235), RGB(60,70,65),    RGB(0,200,110),   14,  1, 215, L"4 - الزجاجي" },
    /* 4 Mushaf */
    { RGB(250,244,225), RGB(250,244,225), RGB(0,80,40),     RGB(200,160,60),  RGB(150,110,30),   4,  3, 255, L"5 - المصحف" },
    /* 5 Neon Cyberpunk */
    { RGB(10,10,15),    RGB(15,15,25),    RGB(0,255,230),   RGB(0,180,255),   RGB(0,220,255),    8,  2, 235, L"6 - النيون السايبربانك" },
    /* 6 Titanium Minimal */
    { RGB(50,50,55),    RGB(50,50,55),    RGB(210,210,215), RGB(80,80,85),    RGB(100,100,110),  12,  1, 245, L"7 - التيتانيوم" },
    /* 7 Crimson Velvet */
    { RGB(60,15,20),    RGB(45,10,15),    RGB(255,225,150), RGB(180,130,50),  RGB(200,150,60),   6,  2, 250, L"8 - القرمزي الملكي" },
};

/* ═══════════════════════════════ Utilities ═══════════════════════════════ */

static int TodayInt(void) {
    SYSTEMTIME st; GetLocalTime(&st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

static void CheckDailyReset(void) {
    int today = TodayInt();
    if (g_cfg.lastDate != today) {
        for (int i = 0; i < g_cfg.count; i++) g_cfg.items[i].shown = 0;
        g_cfg.lastDate = today;
    }
}

static void Trim(WCHAR *s) {
    WCHAR *p = s;
    while (*p == L' ' || *p == L'\t') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(WCHAR));
    size_t n = wcslen(s);
    while (n && (s[n-1] == L' ' || s[n-1] == L'\t' || s[n-1] == L'\r' || s[n-1] == L'\n'))
        s[--n] = 0;
}

static void GetWorkArea(RECT *r) {
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, r, 0)) {
        r->left = r->top = 0;
        r->right = GetSystemMetrics(SM_CXSCREEN);
        r->bottom = GetSystemMetrics(SM_CYSCREEN);
    }
}

static const WCHAR *GetFontFace(int idx) {
    if (idx >= 0 && idx < FONT_COUNT && g_fontLoaded[idx])
        return g_fontFaces[idx];
    return g_fontFaces[FONT_COUNT]; /* Segoe UI */
}

/* ═════════════════════════════ Font Loading ══════════════════════════════ */

static void LoadFonts(void) {
    WCHAR path[MAX_PATH];
    for (int i = 0; i < FONT_COUNT; i++) {
        _snwprintf(path, MAX_PATH, L"%sfonts\\%s", g_exeDir, g_fontFiles[i]);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            if (AddFontResourceExW(path, FR_PRIVATE, 0) > 0)
                g_fontLoaded[i] = TRUE;
        }
    }
}

static void UnloadFonts(void) {
    WCHAR path[MAX_PATH];
    for (int i = 0; i < FONT_COUNT; i++) {
        if (!g_fontLoaded[i]) continue;
        _snwprintf(path, MAX_PATH, L"%sfonts\\%s", g_exeDir, g_fontFiles[i]);
        RemoveFontResourceExW(path, FR_PRIVATE, 0);
        g_fontLoaded[i] = FALSE;
    }
}

static HFONT MakeFont(int fontIdx, int pointSize) {
    return CreateFontW(
        -MulDiv(pointSize, 96, 72), 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        GetFontFace(fontIdx));
}

static void UpdateCachedFont(void) {
    if (g_cachedFont) { DeleteObject(g_cachedFont); g_cachedFont = NULL; }
    int sz = g_cfg.fontSize;
    if (sz < 8)  sz = 8;
    if (sz > 177) sz = 177;
    g_cachedFont = MakeFont(g_cfg.fontIndex, sz);
}

/* ══════════════════════════ Config Save / Load ═══════════════════════════ */

static void DefaultConfig(void) {
    static const WCHAR *defs[] = {
        L"سُبْحَانَ اللهِ وَبِحَمْدِهِ",
        L"أَسْتَغْفِرُ اللهَ وَأَتُوبُ إِلَيْهِ",
        L"لَا إِلَهَ إِلَّا اللهُ",
        L"اللهُ أَكْبَرُ",
        L"الْحَمْدُ لِلهِ",
        L"لَا حَوْلَ وَلَا قُوَّةَ إِلَّا بِاللهِ",
        L"اللَّهُمَّ صَلِّ وَسَلِّمْ عَلَى نَبِيِّنَا مُحَمَّدٍ",
        L"سُبْحَانَ اللهِ وَالْحَمْدُ لِلهِ وَلَا إِلَهَ إِلَّا اللهُ وَاللهُ أَكْبَرُ",
    };
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.count = (int)(sizeof(defs) / sizeof(defs[0]));
    for (int i = 0; i < g_cfg.count; i++) {
        wcsncpy(g_cfg.items[i].text, defs[i], MAX_TEXT - 1);
        g_cfg.items[i].limit = 0;
    }
    g_cfg.style       = 0;
    g_cfg.anim        = ANIM_FADE;
    g_cfg.intervalSec = 300;
    g_cfg.durationSec = 8;
    g_cfg.startup     = 0;
    g_cfg.lastDate    = TodayInt();
    g_cfg.position    = POS_TOP_RIGHT;
    g_cfg.yOffset     = 16;
    g_cfg.xMargin     = 16;
    g_cfg.autoFit     = 1;
    g_cfg.fontIndex   = 6; /* Segoe UI fallback */
    g_cfg.fontSize    = 24;
}

static void SaveConfig(void) {
    HANDLE h = CreateFileW(g_iniPath, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WCHAR buf[MAX_TEXT + 128];
    char  utf8[(MAX_TEXT + 128) * 4];
    DWORD w;
    /* UTF-8 BOM */
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    WriteFile(h, bom, 3, &w, NULL);

    #define WLINE(...) do { \
        _snwprintf(buf, sizeof(buf)/sizeof(WCHAR), __VA_ARGS__); \
        int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), NULL, NULL); \
        if (n > 1) WriteFile(h, utf8, n - 1, &w, NULL); \
    } while(0)

    WLINE(L"; Azkar Desktop Notifier v2.0 settings\r\n");
    WLINE(L"style=%d\r\n",      g_cfg.style);
    WLINE(L"anim=%d\r\n",       g_cfg.anim);
    WLINE(L"interval=%d\r\n",   g_cfg.intervalSec);
    WLINE(L"duration=%d\r\n",   g_cfg.durationSec);
    WLINE(L"startup=%d\r\n",    g_cfg.startup);
    WLINE(L"lastdate=%d\r\n",   g_cfg.lastDate);
    WLINE(L"position=%d\r\n",   g_cfg.position);
    WLINE(L"yoffset=%d\r\n",    g_cfg.yOffset);
    WLINE(L"xmargin=%d\r\n",    g_cfg.xMargin);
    WLINE(L"autofit=%d\r\n",    g_cfg.autoFit);
    WLINE(L"fontindex=%d\r\n",  g_cfg.fontIndex);
    WLINE(L"fontsize=%d\r\n",   g_cfg.fontSize);
    WLINE(L"; zikr=<limit>|<shown>|<text>\r\n");
    for (int i = 0; i < g_cfg.count; i++)
        WLINE(L"zikr=%d|%d|%s\r\n",
              g_cfg.items[i].limit, g_cfg.items[i].shown, g_cfg.items[i].text);
    #undef WLINE
    CloseHandle(h);
}

static void LoadConfig(void) {
    DefaultConfig();
    HANDLE h = CreateFileW(g_iniPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { SaveConfig(); return; }
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 512 * 1024) { CloseHandle(h); return; }
    char *data = (char *)malloc(size + 1);
    DWORD rd = 0;
    if (!data || !ReadFile(h, data, size, &rd, NULL)) { free(data); CloseHandle(h); return; }
    CloseHandle(h);
    data[rd] = 0;
    char *p = data;
    if (rd >= 3 && (unsigned char)p[0]==0xEF && (unsigned char)p[1]==0xBB && (unsigned char)p[2]==0xBF)
        p += 3;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
    WCHAR *wdata = (WCHAR *)malloc((wlen + 1) * sizeof(WCHAR));
    if (!wdata) { free(data); return; }
    MultiByteToWideChar(CP_UTF8, 0, p, -1, wdata, wlen + 1);
    free(data);

    g_cfg.count = 0;
    WCHAR *cur = wdata;
    while (cur && *cur) {
        WCHAR *line = cur;
        WCHAR *nl = wcschr(cur, L'\n');
        if (nl) { *nl = 0; cur = nl + 1; } else cur = NULL;
        Trim(line);
        if (!*line || *line == L';') continue;
        WCHAR *eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        WCHAR *key = line, *val = eq + 1;
        Trim(key); Trim(val);

        if      (!wcscmp(key, L"style"))     g_cfg.style       = _wtoi(val);
        else if (!wcscmp(key, L"anim"))      g_cfg.anim        = _wtoi(val);
        else if (!wcscmp(key, L"interval"))  g_cfg.intervalSec  = _wtoi(val);
        else if (!wcscmp(key, L"duration"))  g_cfg.durationSec  = _wtoi(val);
        else if (!wcscmp(key, L"startup"))   g_cfg.startup      = _wtoi(val);
        else if (!wcscmp(key, L"lastdate"))  g_cfg.lastDate     = _wtoi(val);
        else if (!wcscmp(key, L"position"))  g_cfg.position     = _wtoi(val);
        else if (!wcscmp(key, L"yoffset"))   g_cfg.yOffset      = _wtoi(val);
        else if (!wcscmp(key, L"xmargin"))   g_cfg.xMargin      = _wtoi(val);
        else if (!wcscmp(key, L"autofit"))   g_cfg.autoFit      = _wtoi(val);
        else if (!wcscmp(key, L"fontindex")) g_cfg.fontIndex    = _wtoi(val);
        else if (!wcscmp(key, L"fontsize"))  g_cfg.fontSize     = _wtoi(val);
        else if (!wcscmp(key, L"zikr") && g_cfg.count < MAX_AZKAR) {
            WCHAR *b1 = wcschr(val, L'|'); if (!b1) continue;
            WCHAR *b2 = wcschr(b1 + 1, L'|'); if (!b2) continue;
            *b1 = 0; *b2 = 0;
            Zikr *z = &g_cfg.items[g_cfg.count];
            z->limit = _wtoi(val);  if (z->limit < 0) z->limit = 0;
            z->shown = _wtoi(b1+1); if (z->shown < 0) z->shown = 0;
            wcsncpy(z->text, b2 + 1, MAX_TEXT - 1); z->text[MAX_TEXT - 1] = 0;
            Trim(z->text);
            if (*z->text) g_cfg.count++;
        }
    }
    free(wdata);

    /* clamp values */
    if (g_cfg.style < 0 || g_cfg.style >= STYLE_COUNT) g_cfg.style = 0;
    if (g_cfg.anim  < 0 || g_cfg.anim  >= ANIM_COUNT)  g_cfg.anim  = ANIM_FADE;
    if (g_cfg.intervalSec < 5)     g_cfg.intervalSec = 5;
    if (g_cfg.intervalSec > 86400) g_cfg.intervalSec = 86400;
    if (g_cfg.durationSec < 1)     g_cfg.durationSec = 1;
    if (g_cfg.durationSec > 120)   g_cfg.durationSec = 120;
    if (g_cfg.position < 0 || g_cfg.position > 2) g_cfg.position = 0;
    if (g_cfg.yOffset < 0)   g_cfg.yOffset = 0;
    if (g_cfg.yOffset > 777) g_cfg.yOffset = 777;
    if (g_cfg.xMargin < 0)   g_cfg.xMargin = 0;
    if (g_cfg.xMargin > 400) g_cfg.xMargin = 400;
    if (g_cfg.fontIndex < 0 || g_cfg.fontIndex > FONT_COUNT) g_cfg.fontIndex = FONT_COUNT;
    if (g_cfg.fontSize < 8)   g_cfg.fontSize = 8;
    if (g_cfg.fontSize > 177) g_cfg.fontSize = 177;
    g_cfg.autoFit = !!g_cfg.autoFit;
    CheckDailyReset();
}

/* ═══════════════════════════ Startup Registry ═══════════════════════════ */

static void ApplyStartup(int enable) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (enable) {
        WCHAR cmd[MAX_PATH + 4];
        _snwprintf(cmd, MAX_PATH + 4, L"\"%s\"", g_exePath);
        RegSetValueExW(k, REG_RUN_VALUE, 0, REG_SZ,
                       (const BYTE *)cmd, (DWORD)((wcslen(cmd)+1)*sizeof(WCHAR)));
    } else {
        RegDeleteValueW(k, REG_RUN_VALUE);
    }
    RegCloseKey(k);
}

/* ══════════════════════ Popup Size & Position ════════════════════════════ */

static void ComputePopupSize(void) {
    RECT wa; GetWorkArea(&wa);
    int waW = wa.right - wa.left;
    int maxWidth = (int)(waW * 0.9);

    UpdateCachedFont();
    HDC hdc = GetDC(NULL);
    HFONT old = (HFONT)SelectObject(hdc, g_cachedFont);

    int measureW = 480;
    if (g_cfg.autoFit && maxWidth - 50 < measureW)
        measureW = maxWidth - 50;

    RECT rc = {0, 0, measureW, 0};
    DrawTextW(hdc, g_popupText, -1, &rc,
              DT_CALCRECT | DT_WORDBREAK | DT_RTLREADING | DT_NOPREFIX);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);

    int padX = 25, padY = 12;
    if (g_cfg.style == 3) padX += 10; /* glass sidebar */

    if (g_cfg.autoFit) {
        g_popupW = (rc.right - rc.left) + padX * 2;
        g_popupH = (rc.bottom - rc.top) + padY * 2;
        if (g_popupW < 220) g_popupW = 220;
        if (g_popupH < 60)  g_popupH = 60;
        if (g_popupW > maxWidth) g_popupW = maxWidth;
    } else {
        g_popupW = 480;
        g_popupH = (rc.bottom - rc.top) + padY * 2;
        if (g_popupH < 90)  g_popupH = 90;
    }

    /* position */
    switch (g_cfg.position) {
    case POS_TOP_LEFT:
        g_popupX = wa.left + g_cfg.xMargin;
        break;
    case POS_TOP_CENTER:
        g_popupX = wa.left + (waW - g_popupW) / 2;
        break;
    default: /* POS_TOP_RIGHT */
        g_popupX = wa.right - g_popupW - g_cfg.xMargin;
        break;
    }
    g_popupY = wa.top + g_cfg.yOffset;
}

/* ═══════════════════════════ Popup Painting ══════════════════════════════ */

static void PaintPopup(HDC hdc) {
    const StyleDef *s = &g_styles[g_cfg.style];
    RECT rc = {0, 0, g_popupW, g_popupH};
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, g_popupW, g_popupH);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);

    /* ── background ───────────────────────────────────────── */
    if (s->bg != s->bg2) {
        /* vertical gradient via GradientFill */
        TRIVERTEX vert[2];
        vert[0].x = 0; vert[0].y = 0;
        vert[0].Red   = (COLOR16)(GetRValue(s->bg) << 8);
        vert[0].Green = (COLOR16)(GetGValue(s->bg) << 8);
        vert[0].Blue  = (COLOR16)(GetBValue(s->bg) << 8);
        vert[0].Alpha = 0;
        vert[1].x = g_popupW; vert[1].y = g_popupH;
        vert[1].Red   = (COLOR16)(GetRValue(s->bg2) << 8);
        vert[1].Green = (COLOR16)(GetGValue(s->bg2) << 8);
        vert[1].Blue  = (COLOR16)(GetBValue(s->bg2) << 8);
        vert[1].Alpha = 0;
        GRADIENT_RECT gr = {0, 1};
        GradientFill(mem, vert, 2, &gr, 1, GRADIENT_FILL_RECT_V);
    } else {
        HBRUSH hb = CreateSolidBrush(s->bg);
        FillRect(mem, &rc, hb);
        DeleteObject(hb);
    }

    /* ── border / frame ───────────────────────────────────── */
    if (s->borderW > 0) {
        HPEN pen = CreatePen(PS_SOLID, s->borderW, s->border);
        HPEN oldPen = (HPEN)SelectObject(mem, pen);
        HBRUSH oldBr = (HBRUSH)SelectObject(mem, GetStockObject(NULL_BRUSH));
        int o = s->borderW / 2;
        if (s->radius)
            RoundRect(mem, o, o, g_popupW - o, g_popupH - o, s->radius*2, s->radius*2);
        else
            Rectangle(mem, o, o, g_popupW - o, g_popupH - o);

        /* Night: inner glow ring */
        if (g_cfg.style == 1) {
            HPEN p2 = CreatePen(PS_SOLID, 1, s->accent);
            SelectObject(mem, p2); DeleteObject(pen); pen = p2;
            RoundRect(mem, 4, 4, g_popupW-4, g_popupH-4, s->radius*2, s->radius*2);
        }
        /* Mushaf: inner gold line */
        if (g_cfg.style == 4) {
            HPEN p2 = CreatePen(PS_SOLID, 1, s->accent);
            SelectObject(mem, p2); DeleteObject(pen); pen = p2;
            Rectangle(mem, 7, 7, g_popupW-7, g_popupH-7);
        }
        /* Neon: double outer glow */
        if (g_cfg.style == 5) {
            HPEN p2 = CreatePen(PS_SOLID, 1, RGB(0,100,180));
            SelectObject(mem, p2); DeleteObject(pen); pen = p2;
            RoundRect(mem, 4, 4, g_popupW-4, g_popupH-4, s->radius*2, s->radius*2);
        }

        SelectObject(mem, oldPen); SelectObject(mem, oldBr);
        DeleteObject(pen);
    }

    /* ── Glass: green accent bar on right ─────────────────── */
    if (g_cfg.style == 3) {
        RECT bar = {g_popupW - 8, 10, g_popupW - 3, g_popupH - 10};
        HBRUSH hb = CreateSolidBrush(s->accent);
        FillRect(mem, &bar, hb); DeleteObject(hb);
    }
    /* ── Mushaf: gold corner dots ─────────────────────────── */
    if (g_cfg.style == 4) {
        HBRUSH hb = CreateSolidBrush(s->accent);
        RECT c1={10,10,16,16}, c2={g_popupW-16,10,g_popupW-10,16};
        RECT c3={10,g_popupH-16,16,g_popupH-10}, c4={g_popupW-16,g_popupH-16,g_popupW-10,g_popupH-10};
        FillRect(mem,&c1,hb); FillRect(mem,&c2,hb);
        FillRect(mem,&c3,hb); FillRect(mem,&c4,hb);
        DeleteObject(hb);
    }

    /* ── text ─────────────────────────────────────────────── */
    HFONT f = g_cachedFont;
    HFONT oldF = (HFONT)SelectObject(mem, f);
    SetBkMode(mem, TRANSPARENT);

    RECT tr = rc;
    InflateRect(&tr, -25, -12);
    if (g_cfg.style == 3) tr.right -= 10;

    /* shadow for gradient / neon / crimson */
    if (g_cfg.style == 2 || g_cfg.style == 5 || g_cfg.style == 7) {
        RECT sh = tr; OffsetRect(&sh, 1, 2);
        SetTextColor(mem, RGB(0, 0, 0));
        DrawTextW(mem, g_popupText, -1, &sh,
                  DT_CENTER | DT_WORDBREAK | DT_RTLREADING | DT_NOPREFIX);
    }

    SetTextColor(mem, s->fg);
    /* vertical-center text */
    RECT calc = tr;
    DrawTextW(mem, g_popupText, -1, &calc,
              DT_CALCRECT | DT_WORDBREAK | DT_RTLREADING | DT_NOPREFIX);
    int th = calc.bottom - calc.top;
    tr.top = (g_popupH - th) / 2;
    tr.bottom = tr.top + th;
    DrawTextW(mem, g_popupText, -1, &tr,
              DT_CENTER | DT_WORDBREAK | DT_RTLREADING | DT_NOPREFIX);

    SelectObject(mem, oldF);
    /* note: we do NOT DeleteObject(f) here because f == g_cachedFont (owned globally) */

    BitBlt(hdc, 0, 0, g_popupW, g_popupH, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* ═══════════════════════════ Animation System ════════════════════════════ */

static void ApplyWindowShape(int w, int h) {
    const StyleDef *s = &g_styles[g_cfg.style];
    if (s->radius > 0) {
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, s->radius * 2, s->radius * 2);
        SetWindowRgn(g_hPopup, rgn, TRUE); /* system takes ownership of rgn */
    } else {
        SetWindowRgn(g_hPopup, NULL, TRUE);
    }
}

static void ApplyAnimFrame(void) {
    const StyleDef *s = &g_styles[g_cfg.style];
    float t = (float)g_animStep / (float)ANIM_STEPS; /* 0.0 → 1.0 */

    switch (g_cfg.anim) {
    case ANIM_FADE: {
        BYTE a = (BYTE)(s->alpha * t);
        SetLayeredWindowAttributes(g_hPopup, 0, a, LWA_ALPHA);
        break;
    }
    case ANIM_SLIDE_DOWN: {
        int startY = g_popupY - 50;
        int curY = startY + (int)((g_popupY - startY) * t);
        SetWindowPos(g_hPopup, HWND_TOPMOST, g_popupX, curY, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        SetLayeredWindowAttributes(g_hPopup, 0, s->alpha, LWA_ALPHA);
        break;
    }
    case ANIM_SLIDE_SIDE: {
        int offX;
        if (g_cfg.position == POS_TOP_LEFT)
            offX = g_popupX - g_popupW - 20; /* from left */
        else if (g_cfg.position == POS_TOP_CENTER)
            offX = g_popupX;                  /* no side slide for center, fall through to fade behavior */
        else
            offX = g_popupX + g_popupW + 20;  /* from right */
        int curX = offX + (int)((g_popupX - offX) * t);
        SetWindowPos(g_hPopup, HWND_TOPMOST, curX, g_popupY, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        SetLayeredWindowAttributes(g_hPopup, 0, s->alpha, LWA_ALPHA);
        break;
    }
    case ANIM_POP_SCALE: {
        float scale = 0.8f + 0.2f * t;
        int curW = (int)(g_popupW * scale);
        int curH = (int)(g_popupH * scale);
        int curX = g_popupX + (g_popupW - curW) / 2;
        int curY = g_popupY + (g_popupH - curH) / 2;
        SetWindowPos(g_hPopup, HWND_TOPMOST, curX, curY, curW, curH, SWP_NOACTIVATE);
        ApplyWindowShape(curW, curH);
        SetLayeredWindowAttributes(g_hPopup, 0, (BYTE)(s->alpha * t), LWA_ALPHA);
        InvalidateRect(g_hPopup, NULL, TRUE);
        break;
    }
    default: /* ANIM_NONE */
        SetLayeredWindowAttributes(g_hPopup, 0, s->alpha, LWA_ALPHA);
        break;
    }
}

static void HidePopupNow(void) {
    KillTimer(g_hPopup, TIMER_ANIM);
    KillTimer(g_hPopup, TIMER_HIDE);
    ShowWindow(g_hPopup, SW_HIDE);
    g_animDir = 0;
}

static void StartHide(void) {
    KillTimer(g_hPopup, TIMER_HIDE);
    if (g_cfg.anim == ANIM_NONE) { HidePopupNow(); return; }
    g_animDir = -1;
    g_animStep = ANIM_STEPS;
    SetTimer(g_hPopup, TIMER_ANIM, ANIM_MS, NULL);
}

static void ShowPopup(const WCHAR *text) {
    wcsncpy(g_popupText, text, MAX_TEXT - 1);
    g_popupText[MAX_TEXT - 1] = 0;
    KillTimer(g_hPopup, TIMER_ANIM);
    KillTimer(g_hPopup, TIMER_HIDE);

    ComputePopupSize();
    ApplyWindowShape(g_popupW, g_popupH);

    if (g_cfg.anim == ANIM_NONE) {
        SetWindowPos(g_hPopup, HWND_TOPMOST, g_popupX, g_popupY,
                     g_popupW, g_popupH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetLayeredWindowAttributes(g_hPopup, 0, g_styles[g_cfg.style].alpha, LWA_ALPHA);
        InvalidateRect(g_hPopup, NULL, TRUE);
        g_animDir = 0;
        g_animStep = ANIM_STEPS;
        SetTimer(g_hPopup, TIMER_HIDE, g_cfg.durationSec * 1000, NULL);
    } else {
        /* set initial position for animation */
        g_animDir = +1;
        g_animStep = 0;

        if (g_cfg.anim == ANIM_SLIDE_DOWN) {
            SetWindowPos(g_hPopup, HWND_TOPMOST, g_popupX, g_popupY - 50,
                         g_popupW, g_popupH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else if (g_cfg.anim == ANIM_SLIDE_SIDE) {
            int startX;
            if (g_cfg.position == POS_TOP_LEFT) startX = g_popupX - g_popupW - 20;
            else if (g_cfg.position == POS_TOP_CENTER) startX = g_popupX;
            else startX = g_popupX + g_popupW + 20;
            SetWindowPos(g_hPopup, HWND_TOPMOST, startX, g_popupY,
                         g_popupW, g_popupH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else if (g_cfg.anim == ANIM_POP_SCALE) {
            int sw = (int)(g_popupW * 0.8f), sh = (int)(g_popupH * 0.8f);
            int sx = g_popupX + (g_popupW - sw)/2, sy = g_popupY + (g_popupH - sh)/2;
            SetWindowPos(g_hPopup, HWND_TOPMOST, sx, sy, sw, sh,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ApplyWindowShape(sw, sh);
        } else { /* FADE */
            SetWindowPos(g_hPopup, HWND_TOPMOST, g_popupX, g_popupY,
                         g_popupW, g_popupH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        SetLayeredWindowAttributes(g_hPopup, 0, 0, LWA_ALPHA);
        InvalidateRect(g_hPopup, NULL, TRUE);
        ApplyAnimFrame();
        SetTimer(g_hPopup, TIMER_ANIM, ANIM_MS, NULL);
    }
}

/* ════════════════════════════ Popup WndProc ══════════════════════════════ */

static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintPopup(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wp == TIMER_ANIM) {
            g_animStep += g_animDir;
            if (g_animDir > 0 && g_animStep >= ANIM_STEPS) {
                /* animation-in complete */
                g_animStep = ANIM_STEPS;
                ApplyAnimFrame();
                KillTimer(hwnd, TIMER_ANIM);
                g_animDir = 0;
                /* restore final size/position in case of scale anim */
                SetWindowPos(g_hPopup, HWND_TOPMOST, g_popupX, g_popupY,
                             g_popupW, g_popupH, SWP_NOACTIVATE);
                ApplyWindowShape(g_popupW, g_popupH);
                SetLayeredWindowAttributes(g_hPopup, 0, g_styles[g_cfg.style].alpha, LWA_ALPHA);
                InvalidateRect(g_hPopup, NULL, TRUE);
                SetTimer(hwnd, TIMER_HIDE, g_cfg.durationSec * 1000, NULL);
            } else if (g_animDir < 0 && g_animStep <= 0) {
                /* animation-out complete */
                HidePopupNow();
            } else {
                ApplyAnimFrame();
            }
        } else if (wp == TIMER_HIDE) {
            StartHide();
        }
        return 0;

    case WM_LBUTTONUP: case WM_RBUTTONUP:
        HidePopupNow();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ═════════════════════════════ Scheduling ════════════════════════════════ */

static int PickNextZikr(void) {
    CheckDailyReset();
    if (g_cfg.count == 0) return -1;
    for (int k = 0; k < g_cfg.count; k++) {
        int i = (g_nextIndex + k) % g_cfg.count;
        Zikr *z = &g_cfg.items[i];
        if (z->limit == 0 || z->shown < z->limit) {
            g_nextIndex = (i + 1) % g_cfg.count;
            return i;
        }
    }
    return -1;
}

static void ShowNextZikr(void) {
    int i = PickNextZikr();
    if (i < 0) return;
    Zikr *z = &g_cfg.items[i];
    if (z->limit > 0) { z->shown++; SaveConfig(); }
    ShowPopup(z->text);
}

static void RestartMainTimer(void) {
    KillTimer(g_hMain, TIMER_MAIN);
    if (!g_paused)
        SetTimer(g_hMain, TIMER_MAIN, (UINT)g_cfg.intervalSec * 1000, NULL);
}

/* ═══════════════════════════ Settings Dialog ═════════════════════════════ */

static void RefreshList(HWND dlg) {
    HWND lb = GetDlgItem(dlg, IDC_LIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    WCHAR buf[MAX_TEXT + 48];
    for (int i = 0; i < g_cfg.count; i++) {
        Zikr *z = &g_cfg.items[i];
        if (z->limit == 0)
            _snwprintf(buf, sizeof(buf)/sizeof(WCHAR), L"%s   [دائماً]", z->text);
        else
            _snwprintf(buf, sizeof(buf)/sizeof(WCHAR), L"%s   [%d/%d اليوم]",
                       z->text, z->shown, z->limit);
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)buf);
    }
    if (g_selectedInList >= 0 && g_selectedInList < g_cfg.count)
        SendMessageW(lb, LB_SETCURSEL, g_selectedInList, 0);
}

static void UpdateInfo(HWND dlg) {
    WCHAR buf[128];
    _snwprintf(buf, 128, L"%d ذِكر محفوظ — الإعدادات في azkar.ini جنب البرنامج", g_cfg.count);
    SetDlgItemTextW(dlg, IDC_STATIC_INFO, buf);
}

static void LoadEditorFromSelection(HWND dlg) {
    int sel = (int)SendDlgItemMessageW(dlg, IDC_LIST, LB_GETCURSEL, 0, 0);
    g_selectedInList = sel;
    if (sel < 0 || sel >= g_cfg.count) return;
    Zikr *z = &g_cfg.items[sel];
    SetDlgItemTextW(dlg, IDC_EDIT_TEXT, z->text);
    CheckDlgButton(dlg, IDC_CHK_ALWAYS, z->limit == 0 ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(dlg, IDC_EDIT_COUNT, z->limit == 0 ? 5 : z->limit, FALSE);
    EnableWindow(GetDlgItem(dlg, IDC_EDIT_COUNT), z->limit != 0);
}

static int ReadEditor(HWND dlg, Zikr *out) {
    WCHAR text[MAX_TEXT];
    GetDlgItemTextW(dlg, IDC_EDIT_TEXT, text, MAX_TEXT);
    Trim(text);
    if (!*text) { MessageBoxW(dlg, L"اكتب نص الذِكر أولاً", APP_NAME, MB_ICONWARNING); return 0; }
    for (WCHAR *p = text; *p; p++)
        if (*p == L'|' || *p == L'\r' || *p == L'\n') *p = L' ';
    memset(out, 0, sizeof(*out));
    wcsncpy(out->text, text, MAX_TEXT - 1);
    if (IsDlgButtonChecked(dlg, IDC_CHK_ALWAYS) == BST_CHECKED) {
        out->limit = 0;
    } else {
        BOOL ok; int n = (int)GetDlgItemInt(dlg, IDC_EDIT_COUNT, &ok, FALSE);
        if (!ok || n < 1) n = 1; if (n > 1000) n = 1000;
        out->limit = n;
    }
    return 1;
}

static void ReadGeneral(HWND dlg) {
    int st = (int)SendDlgItemMessageW(dlg, IDC_CMB_STYLE, CB_GETCURSEL, 0, 0);
    if (st >= 0 && st < STYLE_COUNT) g_cfg.style = st;

    int an = (int)SendDlgItemMessageW(dlg, IDC_CMB_ANIM, CB_GETCURSEL, 0, 0);
    if (an >= 0 && an < ANIM_COUNT) g_cfg.anim = an;

    int pos = (int)SendDlgItemMessageW(dlg, IDC_CMB_POSITION, CB_GETCURSEL, 0, 0);
    if (pos >= 0 && pos <= 2) g_cfg.position = pos;

    int fi = (int)SendDlgItemMessageW(dlg, IDC_CMB_FONT, CB_GETCURSEL, 0, 0);
    if (fi >= 0 && fi <= FONT_COUNT) g_cfg.fontIndex = fi;

    g_cfg.yOffset  = (int)SendDlgItemMessageW(dlg, IDC_SLIDER_YOFFSET, TBM_GETPOS, 0, 0);
    g_cfg.xMargin  = (int)SendDlgItemMessageW(dlg, IDC_SLIDER_XMARGIN, TBM_GETPOS, 0, 0);
    g_cfg.fontSize = (int)SendDlgItemMessageW(dlg, IDC_SLIDER_FONTSIZE, TBM_GETPOS, 0, 0);
    g_cfg.autoFit  = (IsDlgButtonChecked(dlg, IDC_CHK_AUTOFIT) == BST_CHECKED) ? 1 : 0;

    BOOL ok; int v;
    v = (int)GetDlgItemInt(dlg, IDC_EDIT_INTERVAL, &ok, FALSE);
    if (ok) { if (v < 5) v = 5; if (v > 86400) v = 86400; g_cfg.intervalSec = v; }
    v = (int)GetDlgItemInt(dlg, IDC_EDIT_DURATION, &ok, FALSE);
    if (ok) { if (v < 1) v = 1; if (v > 120) v = 120; g_cfg.durationSec = v; }

    int su = IsDlgButtonChecked(dlg, IDC_CHK_STARTUP) == BST_CHECKED;
    if (su != g_cfg.startup) { g_cfg.startup = su; ApplyStartup(su); }

    SaveConfig();
    UpdateCachedFont();
}

static INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        g_hDlg = dlg;
        SendMessageW(dlg, WM_SETICON, ICON_BIG,
                     (LPARAM)LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP)));

        /* ── Style combo ────────────────────────────────── */
        HWND cb = GetDlgItem(dlg, IDC_CMB_STYLE);
        for (int i = 0; i < STYLE_COUNT; i++)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)g_styles[i].nameAr);
        SendMessageW(cb, CB_SETCURSEL, g_cfg.style, 0);

        /* ── Animation combo ────────────────────────────── */
        cb = GetDlgItem(dlg, IDC_CMB_ANIM);
        const WCHAR *anims[ANIM_COUNT] = {
            L"بدون أنيميشن", L"ظهور تدريجي (Fade)",
            L"انزلاق من الأعلى (Slide Down)", L"انزلاق جانبي (Slide Side)",
            L"انبثاق وتمدد (Pop/Scale)"
        };
        for (int i = 0; i < ANIM_COUNT; i++)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)anims[i]);
        SendMessageW(cb, CB_SETCURSEL, g_cfg.anim, 0);

        /* ── Position combo ────────────────────────────── */
        cb = GetDlgItem(dlg, IDC_CMB_POSITION);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"أعلى اليمين (Top-Right)");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"أعلى اليسار (Top-Left)");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"منتصف الأعلى (Top-Center)");
        SendMessageW(cb, CB_SETCURSEL, g_cfg.position, 0);

        /* ── Font combo ──────────────────────────────────── */
        cb = GetDlgItem(dlg, IDC_CMB_FONT);
        const WCHAR *fontLabels[FONT_COUNT + 1] = {
            L"خط الرقعة (Aref Ruqaa)", L"خط الأميري (Amiri)",
            L"خط كوفي (Reem Kufi)", L"خط كايرو (Cairo)",
            L"خط تجوال (Tajawal)", L"خط شنجا (Changa)",
            L"Segoe UI (افتراضي)"
        };
        for (int i = 0; i <= FONT_COUNT; i++)
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)fontLabels[i]);
        SendMessageW(cb, CB_SETCURSEL, g_cfg.fontIndex, 0);

        /* ── Y Offset slider ─────────────────────────────── */
        SendDlgItemMessageW(dlg, IDC_SLIDER_YOFFSET, TBM_SETRANGE, TRUE, MAKELPARAM(0, 777));
        SendDlgItemMessageW(dlg, IDC_SLIDER_YOFFSET, TBM_SETPOS, TRUE, g_cfg.yOffset);
        SetDlgItemInt(dlg, IDC_EDIT_YOFFSET, g_cfg.yOffset, FALSE);

        /* ── X Margin slider ─────────────────────────────── */
        SendDlgItemMessageW(dlg, IDC_SLIDER_XMARGIN, TBM_SETRANGE, TRUE, MAKELPARAM(0, 400));
        SendDlgItemMessageW(dlg, IDC_SLIDER_XMARGIN, TBM_SETPOS, TRUE, g_cfg.xMargin);
        SetDlgItemInt(dlg, IDC_EDIT_XMARGIN, g_cfg.xMargin, FALSE);

        /* ── Font Size slider ────────────────────────────── */
        SendDlgItemMessageW(dlg, IDC_SLIDER_FONTSIZE, TBM_SETRANGE, TRUE, MAKELPARAM(8, 177));
        SendDlgItemMessageW(dlg, IDC_SLIDER_FONTSIZE, TBM_SETPOS, TRUE, g_cfg.fontSize);
        SetDlgItemInt(dlg, IDC_EDIT_FONTSIZE, g_cfg.fontSize, FALSE);

        /* ── Other controls ──────────────────────────────── */
        SetDlgItemInt(dlg, IDC_EDIT_INTERVAL, g_cfg.intervalSec, FALSE);
        SetDlgItemInt(dlg, IDC_EDIT_DURATION, g_cfg.durationSec, FALSE);
        CheckDlgButton(dlg, IDC_CHK_STARTUP, g_cfg.startup ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_CHK_AUTOFIT, g_cfg.autoFit ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(dlg, IDC_EDIT_COUNT, 5, FALSE);
        CheckDlgButton(dlg, IDC_CHK_ALWAYS, BST_CHECKED);
        EnableWindow(GetDlgItem(dlg, IDC_EDIT_COUNT), FALSE);
        SendDlgItemMessageW(dlg, IDC_EDIT_TEXT, EM_LIMITTEXT, MAX_TEXT - 1, 0);

        g_selectedInList = -1;
        RefreshList(dlg);
        UpdateInfo(dlg);
        return TRUE;
    }

    case WM_HSCROLL: {
        /* Sync sliders → edit fields */
        HWND hCtrl = (HWND)lp;
        if (hCtrl == GetDlgItem(dlg, IDC_SLIDER_YOFFSET)) {
            int pos = (int)SendMessageW(hCtrl, TBM_GETPOS, 0, 0);
            SetDlgItemInt(dlg, IDC_EDIT_YOFFSET, pos, FALSE);
        } else if (hCtrl == GetDlgItem(dlg, IDC_SLIDER_XMARGIN)) {
            int pos = (int)SendMessageW(hCtrl, TBM_GETPOS, 0, 0);
            SetDlgItemInt(dlg, IDC_EDIT_XMARGIN, pos, FALSE);
        } else if (hCtrl == GetDlgItem(dlg, IDC_SLIDER_FONTSIZE)) {
            int pos = (int)SendMessageW(hCtrl, TBM_GETPOS, 0, 0);
            SetDlgItemInt(dlg, IDC_EDIT_FONTSIZE, pos, FALSE);
        }
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDC_LIST:
            if (code == LBN_SELCHANGE) LoadEditorFromSelection(dlg);
            return TRUE;

        case IDC_CHK_ALWAYS:
            EnableWindow(GetDlgItem(dlg, IDC_EDIT_COUNT),
                         IsDlgButtonChecked(dlg, IDC_CHK_ALWAYS) != BST_CHECKED);
            return TRUE;

        case IDC_BTN_ADD: {
            if (g_cfg.count >= MAX_AZKAR) {
                MessageBoxW(dlg, L"وصلت للحد الأقصى (64 ذِكر)", APP_NAME, MB_ICONWARNING);
                return TRUE;
            }
            Zikr z; if (!ReadEditor(dlg, &z)) return TRUE;
            g_cfg.items[g_cfg.count++] = z;
            g_selectedInList = g_cfg.count - 1;
            SaveConfig(); RefreshList(dlg); UpdateInfo(dlg);
            SetDlgItemTextW(dlg, IDC_EDIT_TEXT, L"");
            return TRUE;
        }
        case IDC_BTN_UPDATE: {
            int sel = (int)SendDlgItemMessageW(dlg, IDC_LIST, LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= g_cfg.count) {
                MessageBoxW(dlg, L"اختار ذِكر من القائمة أولاً", APP_NAME, MB_ICONINFORMATION);
                return TRUE;
            }
            Zikr z; if (!ReadEditor(dlg, &z)) return TRUE;
            z.shown = g_cfg.items[sel].shown;
            if (z.limit && z.shown > z.limit) z.shown = z.limit;
            g_cfg.items[sel] = z;
            g_selectedInList = sel;
            SaveConfig(); RefreshList(dlg);
            return TRUE;
        }
        case IDC_BTN_DELETE: {
            int sel = (int)SendDlgItemMessageW(dlg, IDC_LIST, LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= g_cfg.count) return TRUE;
            for (int i = sel; i < g_cfg.count - 1; i++)
                g_cfg.items[i] = g_cfg.items[i + 1];
            g_cfg.count--;
            g_selectedInList = (sel < g_cfg.count) ? sel : g_cfg.count - 1;
            if (g_nextIndex >= g_cfg.count) g_nextIndex = 0;
            SaveConfig(); RefreshList(dlg); UpdateInfo(dlg);
            return TRUE;
        }
        case IDC_BTN_PREVIEW: {
            ReadGeneral(dlg);
            WCHAR text[MAX_TEXT];
            GetDlgItemTextW(dlg, IDC_EDIT_TEXT, text, MAX_TEXT);
            Trim(text);
            if (!*text) {
                int sel = (int)SendDlgItemMessageW(dlg, IDC_LIST, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < g_cfg.count) wcscpy(text, g_cfg.items[sel].text);
                else if (g_cfg.count) wcscpy(text, g_cfg.items[0].text);
                else wcscpy(text, L"سُبْحَانَ اللهِ");
            }
            ShowPopup(text);
            return TRUE;
        }

        /* ── Sync edit fields → sliders ──────────────────── */
        case IDC_EDIT_YOFFSET:
            if (code == EN_CHANGE) {
                BOOL ok; int v = GetDlgItemInt(dlg, IDC_EDIT_YOFFSET, &ok, FALSE);
                if (ok && v >= 0 && v <= 777)
                    SendDlgItemMessageW(dlg, IDC_SLIDER_YOFFSET, TBM_SETPOS, TRUE, v);
            }
            return TRUE;
        case IDC_EDIT_XMARGIN:
            if (code == EN_CHANGE) {
                BOOL ok; int v = GetDlgItemInt(dlg, IDC_EDIT_XMARGIN, &ok, FALSE);
                if (ok && v >= 0 && v <= 400)
                    SendDlgItemMessageW(dlg, IDC_SLIDER_XMARGIN, TBM_SETPOS, TRUE, v);
            }
            return TRUE;
        case IDC_EDIT_FONTSIZE:
            if (code == EN_CHANGE) {
                BOOL ok; int v = GetDlgItemInt(dlg, IDC_EDIT_FONTSIZE, &ok, FALSE);
                if (ok && v >= 8 && v <= 177)
                    SendDlgItemMessageW(dlg, IDC_SLIDER_FONTSIZE, TBM_SETPOS, TRUE, v);
            }
            return TRUE;

        case IDC_CMB_STYLE: case IDC_CMB_ANIM: case IDC_CMB_POSITION: case IDC_CMB_FONT:
            if (code == CBN_SELCHANGE) ReadGeneral(dlg);
            return TRUE;
        case IDC_CHK_STARTUP: case IDC_CHK_AUTOFIT:
            ReadGeneral(dlg);
            return TRUE;
        case IDC_EDIT_INTERVAL: case IDC_EDIT_DURATION:
            if (code == EN_KILLFOCUS) { ReadGeneral(dlg); RestartMainTimer(); }
            return TRUE;
        case IDOK: case IDCANCEL:
            ReadGeneral(dlg); RestartMainTimer();
            EndDialog(dlg, 0); g_hDlg = NULL;
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        ReadGeneral(dlg); RestartMainTimer();
        EndDialog(dlg, 0); g_hDlg = NULL;
        return TRUE;
    }
    return FALSE;
}

static void OpenSettings(void) {
    if (g_hDlg) { SetForegroundWindow(g_hDlg); return; }
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, SettingsProc, 0);
}

/* ══════════════════════════ Tray Icon & Menu ═════════════════════════════ */

static void ShowTrayMenu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_SHOW_NOW, L"اعرض ذِكر الآن");
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"الإعدادات...");
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), IDM_PAUSE,
                g_paused ? L"استئناف" : L"إيقاف مؤقت");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"خروج");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_LAYOUTRTL,
                   pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void UpdateTrayTip(void) {
    _snwprintf(g_nid.szTip, 128, L"%s%s", APP_NAME, g_paused ? L" (متوقف)" : L"");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/* ══════════════════════════ Main Window Proc ═════════════════════════════ */

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT s_taskbarCreated = 0;
    if (!s_taskbarCreated) s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == s_taskbarCreated) { Shell_NotifyIconW(NIM_ADD, &g_nid); return 0; }

    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_MAIN) ShowNextZikr();
        return 0;
    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP: case WM_CONTEXTMENU: ShowTrayMenu(hwnd); break;
        case WM_LBUTTONDBLCLK: OpenSettings(); break;
        case WM_LBUTTONUP: ShowNextZikr(); break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SETTINGS: OpenSettings(); break;
        case IDM_SHOW_NOW: ShowNextZikr(); break;
        case IDM_PAUSE:    g_paused = !g_paused; RestartMainTimer(); UpdateTrayTip(); break;
        case IDM_EXIT:     DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            CheckDailyReset(); RestartMainTimer();
        }
        return TRUE;
    case WM_DESTROY:
        SaveConfig();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ════════════════════════════ Entry Point ════════════════════════════════ */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;
    g_hInst = hInst;

    /* single instance */
    HANDLE mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(APP_CLASS, NULL);
        if (other) PostMessageW(other, WM_COMMAND, IDM_SETTINGS, 0);
        return 0;
    }

    /* paths */
    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);
    wcscpy(g_exeDir, g_exePath);
    WCHAR *slash = wcsrchr(g_exeDir, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscpy(g_iniPath, g_exeDir);
    wcscat(g_iniPath, L"azkar.ini");

    LoadConfig();
    LoadFonts();
    UpdateCachedFont();
    if (g_cfg.startup) ApplyStartup(1);

    /* init common controls for Trackbar (msctls_trackbar32) */
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    /* register window classes */
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = MainProc;
    wc.hInstance    = hInst;
    wc.lpszClassName = APP_CLASS;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);

    WNDCLASSW pc = {0};
    pc.lpfnWndProc = PopupProc;
    pc.hInstance    = hInst;
    pc.lpszClassName = POPUP_CLASS;
    pc.hCursor = LoadCursorW(NULL, IDC_HAND);
    RegisterClassW(&pc);

    /* create hidden message-only main window */
    g_hMain = CreateWindowExW(0, APP_CLASS, APP_NAME, 0,
                              0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);

    /* create layered popup window */
    g_hPopup = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        POPUP_CLASS, L"", WS_POPUP,
        0, 0, 300, 80, NULL, NULL, hInst, NULL);

    /* tray icon */
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = g_hMain;
    g_nid.uID    = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP),
                                      IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wcscpy(g_nid.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    RestartMainTimer();

    /* ═══ Blocking message loop — 0.0% CPU at idle ═══ */
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (g_hDlg && IsDialogMessageW(g_hDlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    /* cleanup */
    if (g_cachedFont) { DeleteObject(g_cachedFont); g_cachedFont = NULL; }
    UnloadFonts();
    if (mutex) CloseHandle(mutex);
    return 0;
}
