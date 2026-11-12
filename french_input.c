/*
 * french_input.c — floating palette of French accented characters (plain C / Win32, no dependencies)
 *
 * - Click a button: the character is inserted at the current caret (the window never takes focus)
 * - Global hotkeys Win+Alt+<letter>: repeated presses cycle through the group, e.g. Win+Alt+E → é è ê ë
 *   Add Shift for uppercase; Win+Alt+H shows/hides the window
 *
 * Build: build.bat (same source for x64 and arm64)
 */
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")

#define APP_CLASS L"FrenchInputFloat"
#define APP_MUTEX L"Local\\FrenchInputFloat.Instance"
#define REG_PATH  L"Software\\FrenchInputFloat"

/* Layout in pixels at 96 DPI, scaled to the monitor DPI at runtime */
#define BASE_PAD    4
#define BASE_GAP    3
#define BASE_CELL   32
#define BASE_HEADER 26
#define BASE_BTN    26
#define COLS 9
#define ROWS 2

#define CYCLE_MS   1500   /* same hotkey pressed again within this time → replace with the next character */
#define FLASH_MS   1500   /* how long the header feedback stays visible */
#define VK_MASKKEY 0xE8   /* unassigned virtual key; stops a Win/Alt release from opening the Start menu / menu bar */

#define HIT_NONE  (-1)
#define HIT_DRAG  (-2)
#define HIT_CASE  (-3)
#define HIT_CLOSE (-4)

#define HK_TOGGLE     1
#define HK_LOWER_BASE 100
#define HK_UPPER_BASE 200
#define TIMER_FLASH   1

#define DWMWA_DARK_MODE    20
#define DWMWA_CORNER_PREF  33
#define DWMWCP_ROUND_VALUE 2

typedef struct { UINT vk; const wchar_t *chars; } Group;

/* Hotkey groups: Win+Alt+<vk>, repeated presses cycle through the characters */
static const Group g_groups[] = {
    { 'E', L"éèêë" },  /* é è ê ë */
    { 'A', L"àâæ" },        /* à â æ */
    { 'C', L"ç" },                    /* ç */
    { 'I', L"îïÿ" },        /* î ï ÿ (ÿ lives here because Win+Alt+Y is often taken by other apps) */
    { 'O', L"ôœ" },              /* ô œ */
    { 'U', L"ùûü" },        /* ù û ü */
    { 'Q', L"«»" },              /* « » */
};
#define NGROUPS ((int)(sizeof g_groups / sizeof g_groups[0]))

/* Characters on the buttons (2 rows x 9 columns) */
static const wchar_t g_cells[COLS * ROWS + 1] =
    L"éèêëàâæç«"   /* é è ê ë à â æ ç « */
    L"îïôœùûüÿ»";  /* î ï ô œ ù û ü ÿ » */
#define NCELLS (COLS * ROWS)

typedef struct {
    COLORREF bg, cell, hover, press, text, sub, accent, accentText, border, closeHover;
} Theme;

static const Theme kLight = {
    RGB(243,243,243), RGB(255,255,255), RGB(229,241,251), RGB(204,228,247),
    RGB(28,28,28), RGB(100,100,100), RGB(0,103,192), RGB(255,255,255),
    RGB(208,208,208), RGB(196,43,28)
};
static const Theme kDark = {
    RGB(32,32,32), RGB(48,48,48), RGB(64,64,64), RGB(84,84,84),
    RGB(240,240,240), RGB(170,170,170), RGB(76,194,255), RGB(0,0,0),
    RGB(72,72,72), RGB(196,43,28)
};

static const Theme *T = &kLight;
static UINT   g_dpi = 96;
static HFONT  g_fontCell, g_fontHdr;
static BOOL   g_upper;          /* "Aa" caps lock (affects clicks only) */
static BOOL   g_shiftHeld;      /* Shift seen held during the last mouse move */
static int    g_hover = HIT_NONE, g_pressed = HIT_NONE;
static BOOL   g_tracking, g_dragging;
static POINT  g_dragStart;
static RECT   g_wndStart;
static wchar_t g_flash[64];
static wchar_t g_hkFail[32];

/* Hotkey cycling state */
static int   g_lastGroup = -1, g_lastIdx;
static BOOL  g_lastUpper;
static DWORD g_lastTick;
static HWND  g_lastFg;

/* ---------- Helpers ---------- */

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static int WinW(void) { return 2 * S(BASE_PAD) + COLS * S(BASE_CELL) + (COLS - 1) * S(BASE_GAP); }
static int WinH(void) { return S(BASE_HEADER) + ROWS * S(BASE_CELL) + (ROWS - 1) * S(BASE_GAP) + S(BASE_PAD); }

static void CellRect(int i, RECT *r)
{
    int c = i % COLS, row = i / COLS;
    r->left   = S(BASE_PAD) + c * (S(BASE_CELL) + S(BASE_GAP));
    r->top    = S(BASE_HEADER) + row * (S(BASE_CELL) + S(BASE_GAP));
    r->right  = r->left + S(BASE_CELL);
    r->bottom = r->top + S(BASE_CELL);
}

static void CloseRect(RECT *r)
{
    r->right  = WinW() - S(BASE_PAD);
    r->left   = r->right - S(BASE_BTN);
    r->top    = S(3);
    r->bottom = S(BASE_HEADER) - S(3);
}

static void CaseRect(RECT *r)
{
    CloseRect(r);
    OffsetRect(r, -(S(BASE_BTN) + S(BASE_GAP)), 0);
}

static int HitTest(int x, int y)
{
    POINT p = { x, y };
    RECT r;
    int i;
    CloseRect(&r); if (PtInRect(&r, p)) return HIT_CLOSE;
    CaseRect(&r);  if (PtInRect(&r, p)) return HIT_CASE;
    for (i = 0; i < NCELLS; i++) {
        CellRect(i, &r);
        if (PtInRect(&r, p)) return i;
    }
    return HIT_DRAG;   /* any empty area drags the window */
}

/* Uppercase for French characters (fixed mapping, independent of locale) */
static wchar_t ToUpperFr(wchar_t c)
{
    if (c >= 0x00E0 && c <= 0x00FE && c != 0x00F7) return (wchar_t)(c - 0x20);
    if (c == 0x0153) return 0x0152;   /* œ → Œ */
    if (c == 0x00FF) return 0x0178;   /* ÿ → Ÿ */
    return c;
}

static int FindGroup(wchar_t c, int *idx)
{
    int g, i;
    for (g = 0; g < NGROUPS; g++)
        for (i = 0; g_groups[g].chars[i]; i++)
            if (g_groups[g].chars[i] == c) { *idx = i; return g; }
    return -1;
}

/* ---------- Keyboard injection ---------- */

static BOOL IsExtendedVk(WORD vk)
{
    return vk == VK_LWIN || vk == VK_RWIN || vk == VK_RMENU || vk == VK_RCONTROL;
}

static void AddVk(INPUT *in, int *n, WORD vk, BOOL up)
{
    INPUT *i = &in[(*n)++];
    ZeroMemory(i, sizeof *i);
    i->type       = INPUT_KEYBOARD;
    i->ki.wVk     = vk;
    i->ki.wScan   = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    i->ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (IsExtendedVk(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
}

static void AddUnicode(INPUT *in, int *n, wchar_t c)
{
    int up;
    for (up = 0; up < 2; up++) {
        INPUT *i = &in[(*n)++];
        ZeroMemory(i, sizeof *i);
        i->type       = INPUT_KEYBOARD;
        i->ki.wScan   = c;
        i->ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
    }
}

/* Button click: send the character directly */
static void SendChar(wchar_t c)
{
    INPUT in[2];
    int n = 0;
    AddUnicode(in, &n, c);
    SendInput((UINT)n, in, sizeof(INPUT));
}

/*
 * When a hotkey fires the user is still holding Win/Alt(/Shift), so a plain injected
 * character would be read as a key combination. Instead: temporarily release those
 * modifiers → (Backspace) → send the character → press them again.
 * A "mask key" tap goes before and after, so releasing Win does not open the Start
 * menu and releasing Alt does not activate the menu bar.
 */
static void SendCharFromHotkey(wchar_t c, BOOL backspaceFirst)
{
    static const WORD mods[] = { VK_LWIN, VK_RWIN, VK_LMENU, VK_RMENU,
                                 VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL };
    WORD held[8];
    INPUT in[40];
    int nh = 0, n = 0, i;

    for (i = 0; i < (int)(sizeof mods / sizeof mods[0]); i++)
        if (GetAsyncKeyState(mods[i]) & 0x8000) held[nh++] = mods[i];

    AddVk(in, &n, VK_MASKKEY, FALSE); AddVk(in, &n, VK_MASKKEY, TRUE);
    for (i = 0; i < nh; i++) AddVk(in, &n, held[i], TRUE);
    if (backspaceFirst) { AddVk(in, &n, VK_BACK, FALSE); AddVk(in, &n, VK_BACK, TRUE); }
    AddUnicode(in, &n, c);
    for (i = 0; i < nh; i++) AddVk(in, &n, held[i], FALSE);
    if (nh) { AddVk(in, &n, VK_MASKKEY, FALSE); AddVk(in, &n, VK_MASKKEY, TRUE); }

    SendInput((UINT)n, in, sizeof(INPUT));
}

/* ---------- Appearance ---------- */

static void MakeFonts(void)
{
    if (g_fontCell) DeleteObject(g_fontCell);
    if (g_fontHdr)  DeleteObject(g_fontHdr);
    g_fontCell = CreateFontW(-S(18), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    g_fontHdr  = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
}

static BOOL SystemUsesDark(void)
{
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS)
        return FALSE;
    return v == 0;
}

static void ApplyTheme(HWND h)
{
    BOOL dark = SystemUsesDark();
    T = dark ? &kDark : &kLight;
    DwmSetWindowAttribute(h, DWMWA_DARK_MODE, &dark, sizeof dark);
}

static void FillRound(HDC dc, const RECT *r, COLORREF fill, COLORREF line, int rad)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN   p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void Text(HDC dc, const wchar_t *s, int n, RECT *r, HFONT f, COLORREF col, UINT fmt)
{
    SelectObject(dc, f);
    SetTextColor(dc, col);
    DrawTextW(dc, s, n, r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

static void HeaderText(wchar_t *buf, BOOL upper)
{
    if (g_hover >= 0) {
        wchar_t c = g_cells[g_hover];
        int idx = 0, g = FindGroup(c, &idx);
        wchar_t shown = upper ? ToUpperFr(c) : c;
        wsprintfW(buf, L"%c    Win+Alt+%s%c", shown, upper ? L"Shift+" : L"", (wchar_t)g_groups[g].vk);
        if (idx > 0) wsprintfW(buf + lstrlenW(buf), L"  (press %d times)", idx + 1);
    } else if (g_hover == HIT_CASE) {
        lstrcpyW(buf, g_upper ? L"Uppercase locked (click to unlock)" : L"Uppercase lock (or Shift+click)");
    } else if (g_hover == HIT_CLOSE) {
        lstrcpyW(buf, L"Exit");
    } else if (g_flash[0]) {
        lstrcpyW(buf, g_flash);
    } else if (g_hkFail[0]) {
        wsprintfW(buf, L"Hotkey in use: Win+Alt+%s", g_hkFail);
    } else {
        lstrcpyW(buf, L"Français  ·  Win+Alt+H show/hide");
    }
}

static void Paint(HDC dc, const RECT *rc)
{
    BOOL upper = g_upper ^ g_shiftHeld;
    wchar_t buf[128];
    HBRUSH br;
    RECT r, hr;
    int i;

    br = CreateSolidBrush(T->bg);     FillRect(dc, rc, br);  DeleteObject(br);
    br = CreateSolidBrush(T->border); FrameRect(dc, rc, br); DeleteObject(br);
    SetBkMode(dc, TRANSPARENT);

    /* Header hint */
    CaseRect(&r);
    hr.left = S(BASE_PAD) + S(4); hr.top = 0; hr.right = r.left - S(4); hr.bottom = S(BASE_HEADER);
    HeaderText(buf, upper);
    Text(dc, buf, -1, &hr, g_fontHdr, T->sub, DT_LEFT | DT_END_ELLIPSIS);

    /* "Aa" uppercase button */
    {
        COLORREF f = g_upper ? T->accent : (g_pressed == HIT_CASE ? T->press : (g_hover == HIT_CASE ? T->hover : T->bg));
        FillRound(dc, &r, f, f, S(6));
        Text(dc, L"Aa", 2, &r, g_fontHdr, g_upper ? T->accentText : T->text, DT_CENTER);
    }

    /* "×" close button */
    CloseRect(&r);
    if (g_hover == HIT_CLOSE) FillRound(dc, &r, T->closeHover, T->closeHover, S(6));
    Text(dc, L"×", 1, &r, g_fontCell, g_hover == HIT_CLOSE ? RGB(255,255,255) : T->sub, DT_CENTER);

    /* Character buttons */
    for (i = 0; i < NCELLS; i++) {
        wchar_t c = upper ? ToUpperFr(g_cells[i]) : g_cells[i];
        COLORREF f = (g_pressed == i) ? T->press : (g_hover == i ? T->hover : T->cell);
        CellRect(i, &r);
        FillRound(dc, &r, f, T->border, S(6));
        Text(dc, &c, 1, &r, g_fontCell, T->text, DT_CENTER);
    }
}

/* ---------- Window position persistence ---------- */

static void SavePos(HWND h)
{
    RECT r;
    DWORD x, y;
    GetWindowRect(h, &r);
    x = (DWORD)r.left; y = (DWORD)r.top;
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_PATH, L"X", REG_DWORD, &x, sizeof x);
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_PATH, L"Y", REG_DWORD, &y, sizeof y);
}

static BOOL LoadPos(POINT *p)
{
    DWORD x, y, sz = sizeof x;
    POINT probe;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_PATH, L"X", RRF_RT_REG_DWORD, NULL, &x, &sz) != ERROR_SUCCESS) return FALSE;
    sz = sizeof y;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_PATH, L"Y", RRF_RT_REG_DWORD, NULL, &y, &sz) != ERROR_SUCCESS) return FALSE;
    p->x = (LONG)x; p->y = (LONG)y;
    probe.x = p->x + 20; probe.y = p->y + 20;
    return MonitorFromPoint(probe, MONITOR_DEFAULTTONULL) != NULL;   /* fall back to the default if that monitor is gone */
}

/* ---------- Hotkeys ---------- */

static void AppendFail(UINT vk)
{
    int len = lstrlenW(g_hkFail);
    if (len + 3 >= (int)(sizeof g_hkFail / sizeof g_hkFail[0])) return;
    if (len) g_hkFail[len++] = L',';
    g_hkFail[len++] = (wchar_t)vk;
    g_hkFail[len] = 0;
}

static void RegisterHotkeys(HWND h)
{
    const UINT mod = MOD_WIN | MOD_ALT | MOD_NOREPEAT;
    int i;
    for (i = 0; i < NGROUPS; i++) {
        BOOL a = RegisterHotKey(h, HK_LOWER_BASE + i, mod, g_groups[i].vk);
        BOOL b = RegisterHotKey(h, HK_UPPER_BASE + i, mod | MOD_SHIFT, g_groups[i].vk);
        if (!a || !b) AppendFail(g_groups[i].vk);
    }
    if (!RegisterHotKey(h, HK_TOGGLE, mod, 'H')) AppendFail('H');
}

static void UnregisterHotkeys(HWND h)
{
    int i;
    for (i = 0; i < NGROUPS; i++) {
        UnregisterHotKey(h, HK_LOWER_BASE + i);
        UnregisterHotKey(h, HK_UPPER_BASE + i);
    }
    UnregisterHotKey(h, HK_TOGGLE);
}

static void OnGroupHotkey(HWND h, int g, BOOL upper)
{
    DWORD now = GetTickCount();
    HWND fg = GetForegroundWindow();
    int n = lstrlenW(g_groups[g].chars);
    BOOL cycle = n > 1 && g == g_lastGroup && upper == g_lastUpper &&
                 fg == g_lastFg && now - g_lastTick < CYCLE_MS;
    int idx = cycle ? (g_lastIdx + 1) % n : 0;
    wchar_t c = g_groups[g].chars[idx];
    if (upper) c = ToUpperFr(c);

    SendCharFromHotkey(c, cycle);

    g_lastGroup = g; g_lastIdx = idx; g_lastUpper = upper; g_lastTick = now; g_lastFg = fg;

    if (n > 1) wsprintfW(g_flash, L"%c    (%d/%d)", c, idx + 1, n);
    else       wsprintfW(g_flash, L"%c", c);
    SetTimer(h, TIMER_FLASH, FLASH_MS, NULL);
    InvalidateRect(h, NULL, FALSE);
}

/* ---------- Window procedure ---------- */

static void SetHover(HWND h, int hit)
{
    if (hit == HIT_DRAG) hit = HIT_NONE;
    if (hit != g_hover) { g_hover = hit; InvalidateRect(h, NULL, FALSE); }
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;                 /* key point: clicks never steal focus */

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; int hit;
            GetCursorPos(&p); ScreenToClient(h, &p);
            hit = HitTest(p.x, p.y);
            SetCursor(LoadCursorW(NULL, hit == HIT_DRAG ? IDC_SIZEALL : IDC_HAND));
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE: {
        BOOL sh = (wp & MK_SHIFT) != 0;
        if (sh != g_shiftHeld) { g_shiftHeld = sh; InvalidateRect(h, NULL, FALSE); }
        if (!g_tracking) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            g_tracking = TRUE;
        }
        if (g_dragging) {
            POINT p;
            GetCursorPos(&p);
            SetWindowPos(h, NULL, g_wndStart.left + p.x - g_dragStart.x, g_wndStart.top + p.y - g_dragStart.y,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetHover(h, HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        g_shiftHeld = FALSE;
        SetHover(h, HIT_NONE);
        return 0;

    case WM_LBUTTONDOWN: {
        int hit = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        SetCapture(h);
        if (hit == HIT_DRAG) {
            g_dragging = TRUE;
            GetCursorPos(&g_dragStart);
            GetWindowRect(h, &g_wndStart);
        } else {
            g_pressed = hit;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int hit = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        int pressed = g_pressed;
        BOOL wasDragging = g_dragging;
        ReleaseCapture();                     /* triggers WM_CAPTURECHANGED, which resets state */
        if (wasDragging) { SavePos(h); return 0; }
        if (pressed != HIT_NONE && hit == pressed) {
            if (pressed >= 0) {
                wchar_t c = g_cells[pressed];
                if (g_upper ^ ((wp & MK_SHIFT) != 0)) c = ToUpperFr(c);
                g_lastGroup = -1;
                SendChar(c);
            } else if (pressed == HIT_CASE) {
                g_upper = !g_upper;
            } else if (pressed == HIT_CLOSE) {
                DestroyWindow(h);
                return 0;
            }
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_CAPTURECHANGED:
        g_dragging = FALSE;
        g_pressed = HIT_NONE;
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_HOTKEY: {
        int id = (int)wp;
        if (id == HK_TOGGLE) {
            ShowWindow(h, IsWindowVisible(h) ? SW_HIDE : SW_SHOWNOACTIVATE);
        } else if (id >= HK_UPPER_BASE && id < HK_UPPER_BASE + NGROUPS) {
            OnGroupHotkey(h, id - HK_UPPER_BASE, TRUE);
        } else if (id >= HK_LOWER_BASE && id < HK_LOWER_BASE + NGROUPS) {
            OnGroupHotkey(h, id - HK_LOWER_BASE, FALSE);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_FLASH) {
            KillTimer(h, TIMER_FLASH);
            g_flash[0] = 0;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        g_dpi = HIWORD(wp);
        MakeFonts();
        SetWindowPos(h, NULL, r->left, r->top, WinW(), WinH(), SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
            ApplyTheme(h);
            InvalidateRect(h, NULL, FALSE);
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ old;
        GetClientRect(h, &rc);
        mem = CreateCompatibleDC(dc);                       /* double buffering to avoid flicker */
        bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        old = SelectObject(mem, bmp);
        Paint(mem, &rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_DESTROY:
        SavePos(h);
        UnregisterHotkeys(h);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---------- Entry point ---------- */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    HANDLE mutex;
    HWND h;
    POINT pos = { 0, 0 };
    BOOL havePos;
    DWORD corner = DWMWCP_ROUND_VALUE;
    MSG m;
    (void)prev; (void)cmd; (void)show;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* Single instance: running it again just shows the existing window */
    mutex = CreateMutexW(NULL, TRUE, APP_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(APP_CLASS, NULL);
        if (other) ShowWindow(other, SW_SHOWNOACTIVATE);
        return 0;
    }

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);

    havePos = LoadPos(&pos);
    h = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                        APP_CLASS, L"French Input", WS_POPUP,
                        pos.x, pos.y, 10, 10, NULL, NULL, inst, NULL);
    if (!h) return 1;

    g_dpi = GetDpiForWindow(h);
    MakeFonts();
    ApplyTheme(h);
    DwmSetWindowAttribute(h, DWMWA_CORNER_PREF, &corner, sizeof corner);   /* rounded corners on Win11 */

    if (!havePos) {   /* default: bottom-right corner of the primary monitor */
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        pos.x = mi.rcWork.right  - WinW() - S(24);
        pos.y = mi.rcWork.bottom - WinH() - S(24);
    }
    SetWindowPos(h, HWND_TOPMOST, pos.x, pos.y, WinW(), WinH(), SWP_NOACTIVATE);

    RegisterHotkeys(h);
    ShowWindow(h, SW_SHOWNOACTIVATE);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_fontCell) DeleteObject(g_fontCell);
    if (g_fontHdr)  DeleteObject(g_fontHdr);
    if (mutex) CloseHandle(mutex);
    return (int)m.wParam;
}
