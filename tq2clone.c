/*
 * tq2clone.c — Titan Quest II Character Cloner (Win32 GUI)
 *
 * Compile (zig cc):
 *   zig rc tq2clone.rc
 *   zig cc tq2clone.c tq2clone.res -o tq2clone.exe -O2 ^
 *       -lkernel32 -luser32 -lgdi32 -lcomctl32 -lshell32 ^
 *       "-Wl,--subsystem,windows"
 *
 * Compile (MSVC):
 *   rc tq2clone.rc
 *   cl tq2clone.c tq2clone.res /Fe:tq2clone.exe /nologo /O2 ^
 *       /link /SUBSYSTEM:WINDOWS ^
 *       user32.lib gdi32.lib comctl32.lib shell32.lib
 *
 * GVAS StrProperty layout for m_CharacterName
 * (offsets from end of "m_CharacterName\0"):
 *   +20 int32  DataSize  (= 4 + string_length_incl_null)
 *   +24 byte   padding
 *   +25 int32  string length (includes null)
 *   +29 char[] name + '\0'
 *
 * Saving.sav (GrimSave manifest) is deleted after every clone so that
 * TQ2 regenerates it with correct checksums on next startup.
 */

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' "\
    "name='Microsoft.Windows.Common-Controls' "\
    "version='6.0.0.0' "\
    "processorArchitecture='*' "\
    "publicKeyToken='6595b64144ccf1df' "\
    "language='*'\"")

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define MAX_NAME_LEN    30
#define SAVEGAME_SUBDIR L"\\TQ2\\Saved\\SaveGames\\"

/* Offsets relative to end of "m_CharacterName\0" */
#define OFF_DATA_SIZE   20
#define OFF_PAD         24
#define OFF_STR_DATA    29

/* Control IDs */
#define ID_COMBO_SRC    101
#define ID_EDIT_NEW     102
#define ID_BTN_CLONE    103
#define ID_LOG          104
#define ID_LBL_SRC      105
#define ID_LBL_NEW      106
#define ID_LBL_TITLE    107
#define ID_BTN_REFRESH  108

/* DPI-aware scaling */
static int g_dpi = 96;
#define SCALE(x) MulDiv((x), g_dpi, 96)

/* Colours */
#define CLR_BG      RGB(24,  26,  32)
#define CLR_ACCENT  RGB(255, 176, 0)
#define CLR_ACCENT2 RGB(220, 140, 0)
#define CLR_TEXT    RGB(220, 220, 230)
#define CLR_DIM     RGB(140, 145, 160)
#define CLR_EDIT_BG RGB(45,  48,  60)
#define CLR_BORDER  RGB(60,  65,  80)

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

static HINSTANCE g_hInst;
static HWND   g_combo_src;
static HWND   g_edit_new;
static HWND   g_btn_clone;
static HWND   g_log;
static HFONT  g_font_normal;
static HFONT  g_font_bold;
static HFONT  g_font_mono;
static HBRUSH g_br_bg;
static HBRUSH g_br_edit;
static wchar_t g_save_dir[MAX_PATH];

/* ------------------------------------------------------------------ */
/* Log helpers                                                          */
/* ------------------------------------------------------------------ */

static void log_raw(const wchar_t *line)
{
    int n = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, n, n);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void log_line(const wchar_t *msg)
{
    wchar_t b[1024]; _snwprintf(b, 1024, L"%s\r\n", msg); log_raw(b);
}
static void log_ok(const wchar_t *msg)
{
    wchar_t b[1024]; _snwprintf(b, 1024, L"  \u2713 %s\r\n", msg); log_raw(b);
}
static void log_err(const wchar_t *msg)
{
    wchar_t b[1024]; _snwprintf(b, 1024, L"  \u2717 %s\r\n", msg); log_raw(b);
}

/* ------------------------------------------------------------------ */
/* File I/O                                                             */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const wchar_t *path, size_t *out_size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL);
    uint8_t *buf = malloc(sz);
    DWORD rd;
    ReadFile(h, buf, sz, &rd, NULL);
    CloseHandle(h);
    *out_size = rd;
    return buf;
}

static int write_file(const wchar_t *path, const uint8_t *data, size_t size)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD wr;
    WriteFile(h, data, (DWORD)size, &wr, NULL);
    CloseHandle(h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Name patch                                                           */
/* ------------------------------------------------------------------ */

/*
 * Rewrites every m_CharacterName StrProperty in the GVAS binary that
 * holds old_name, replacing it with new_name.
 *
 * Also adjusts three GrimSave size fields that surround the property:
 *   offset -9   from marker: PlayerState data size
 *   offset -81  from marker: DataArray size
 *   offset -336 from marker: outer GrimSave size
 * Guard 10000–3000000 skips the header file, which has arbitrary bytes
 * at those positions.
 *
 * Returns allocated buffer (caller frees), or NULL if name not found.
 */
static uint8_t *patch_name(const uint8_t *src, size_t src_size,
                            const char *old_name, const char *new_name,
                            size_t *new_size)
{
    static const char   MARKER[]    = "m_CharacterName";
    static const size_t MARKER_LEN  = sizeof(MARKER);

    const size_t old_len = strlen(old_name);
    const size_t new_len = strlen(new_name);

    uint8_t *dst = malloc(src_size + new_len + 64);
    size_t dp = 0, sp = 0;
    int patched = 0;

    while (sp < src_size) {
        if (sp + MARKER_LEN <= src_size &&
            memcmp(src + sp, MARKER, MARKER_LEN) == 0)
        {
            size_t end = sp + MARKER_LEN;
            if (end + OFF_STR_DATA + old_len + 1 <= src_size &&
                memcmp(src + end + OFF_STR_DATA, old_name, old_len) == 0 &&
                src[end + OFF_STR_DATA + old_len] == '\0')
            {
                size_t copy_to = end + OFF_DATA_SIZE;
                memcpy(dst + dp, src + sp, copy_to - sp); dp += copy_to - sp;

                uint32_t new_ds = (uint32_t)(4 + new_len + 1);
                memcpy(dst + dp, &new_ds, 4); dp += 4;

                dst[dp++] = src[end + OFF_PAD];

                uint32_t new_sl = (uint32_t)(new_len + 1);
                memcpy(dst + dp, &new_sl, 4); dp += 4;

                memcpy(dst + dp, new_name, new_len); dp += new_len;
                dst[dp++] = '\0';

                sp = end + OFF_STR_DATA + old_len + 1;
                patched++;
                continue;
            }
        }
        dst[dp++] = src[sp++];
    }
    *new_size = dp;

    if (patched == 0) { free(dst); return NULL; }

    ptrdiff_t delta = (ptrdiff_t)new_len - (ptrdiff_t)old_len;
    if (delta != 0) {
        size_t cn = 0;
        for (size_t i = 0; i + MARKER_LEN <= dp; i++)
            if (memcmp(dst + i, MARKER, MARKER_LEN) == 0) { cn = i; break; }

        static const ptrdiff_t SIZE_OFFSETS[] = { -9, -81, -336 };
        for (int k = 0; k < 3; k++) {
            ptrdiff_t off = (ptrdiff_t)cn + SIZE_OFFSETS[k];
            if (off < 0 || (size_t)(off + 4) > dp) continue;
            uint32_t v;
            memcpy(&v, dst + off, 4);
            if (v > 10000u && v < 3000000u) {
                v = (uint32_t)((int32_t)v + (int32_t)delta);
                memcpy(dst + off, &v, 4);
            }
        }
    }

    return dst;
}

/* ------------------------------------------------------------------ */
/* Delete Saving.sav                                                    */
/* ------------------------------------------------------------------ */

static void delete_saving_sav(void)
{
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%sSaving.sav", g_save_dir);
    if (DeleteFileW(path)) {
        log_ok(L"Saving.sav deleted (game will recreate it on next start)");
        return;
    }
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND)
        log_ok(L"Saving.sav not present");
    else
        log_err(L"Could not delete Saving.sav — start TQ2 and it will fix itself");
}

/* ------------------------------------------------------------------ */
/* Character discovery                                                  */
/* ------------------------------------------------------------------ */

static void scan_characters(void)
{
    SendMessageW(g_combo_src, CB_RESETCONTENT, 0, 0);

    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s*_Header.sav", g_save_dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        wchar_t *us = wcschr(fd.cFileName, L'_');
        if (!us) continue;
        size_t nlen = (size_t)(us - fd.cFileName);
        if (nlen == 0 || nlen > MAX_NAME_LEN) continue;

        wchar_t name[MAX_NAME_LEN + 1] = {0};
        wcsncpy(name, fd.cFileName, nlen);

        if (SendMessageW(g_combo_src, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)name) == CB_ERR)
            SendMessageW(g_combo_src, CB_ADDSTRING, 0, (LPARAM)name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (SendMessageW(g_combo_src, CB_GETCOUNT, 0, 0) > 0)
        SendMessageW(g_combo_src, CB_SETCURSEL, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Clone operation                                                      */
/* ------------------------------------------------------------------ */

static const wchar_t *FILE_TYPES[] = {
    L"_Header.sav",
    L"_Data_Player.sav",
    L"_Data_PlayerLocal.sav",
    L"_Data_WorldCampaign.sav",
    L"_Data_WorldFluff.sav",
    NULL
};
static const int PATCH_COUNT = 2; /* Header + Data_Player get name-patched */

static void do_clone(void)
{
    SetWindowTextW(g_log, L"");

    /* Abort if TQ2 is running */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { .dwSize = sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"TQ2-Win64-Shipping.exe") == 0 ||
                    _wcsicmp(pe.szExeFile, L"TitanQuest2.exe") == 0) {
                    CloseHandle(snap);
                    log_err(L"Titan Quest II is running — exit the game first.");
                    return;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    /* Read inputs */
    wchar_t src_namew[MAX_NAME_LEN + 1] = {0};
    int src_idx = (int)SendMessageW(g_combo_src, CB_GETCURSEL, 0, 0);
    if (src_idx < 0) { log_err(L"No source character selected."); return; }
    SendMessageW(g_combo_src, CB_GETLBTEXT, src_idx, (LPARAM)src_namew);

    wchar_t new_namew[MAX_NAME_LEN + 1] = {0};
    GetWindowTextW(g_edit_new, new_namew, MAX_NAME_LEN + 1);
    for (int i = (int)wcslen(new_namew) - 1; i >= 0 && new_namew[i] == L' '; i--)
        new_namew[i] = 0;

    if (wcslen(new_namew) == 0) { log_err(L"New character name cannot be empty."); return; }
    if (wcslen(new_namew) > MAX_NAME_LEN) { log_err(L"Name too long (max 30 characters)."); return; }
    if (wcscmp(src_namew, new_namew) == 0) { log_err(L"New name is the same as source."); return; }

    /* Find source timestamp */
    wchar_t search_pat[MAX_PATH];
    _snwprintf(search_pat, MAX_PATH, L"%s%s_*_Header.sav", g_save_dir, src_namew);
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(search_pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) { log_err(L"Save files not found for selected character."); return; }
    wchar_t timestamp[64] = {0};
    const wchar_t *ts0 = fd.cFileName + wcslen(src_namew) + 1;
    const wchar_t *ts1 = wcsstr(ts0, L"_Header.sav");
    if (ts1) wcsncpy(timestamp, ts0, (size_t)(ts1 - ts0));
    FindClose(hf);

    /* New timestamp */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    wchar_t new_ts[64];
    wcsftime(new_ts, 64, L"%Y-%m-%d--%H-%M-%S", tm);

    wchar_t info[256];
    _snwprintf(info, 256, L"Cloning  %s  \u2192  %s", src_namew, new_namew);
    log_line(info);
    log_line(L"");

    /* Narrow names for patch_name */
    char src_name[MAX_NAME_LEN + 1], new_name[MAX_NAME_LEN + 1];
    WideCharToMultiByte(CP_ACP, 0, src_namew, -1, src_name, sizeof(src_name), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, new_namew, -1, new_name, sizeof(new_name), NULL, NULL);

    int errors = 0;

    for (int i = 0; FILE_TYPES[i]; i++) {
        wchar_t src_path[MAX_PATH], dst_path[MAX_PATH];
        _snwprintf(src_path, MAX_PATH, L"%s%s_%s%s", g_save_dir, src_namew, timestamp, FILE_TYPES[i]);
        _snwprintf(dst_path, MAX_PATH, L"%s%s_%s%s", g_save_dir, new_namew, new_ts,   FILE_TYPES[i]);

        size_t   src_size = 0;
        uint8_t *src_data = read_file(src_path, &src_size);
        if (!src_data) {
            wchar_t msg[MAX_PATH + 32];
            _snwprintf(msg, MAX_PATH + 32, L"Cannot read: %s", src_path);
            log_err(msg); errors++; continue;
        }

        uint8_t *out_data; size_t out_size;
        if (i < PATCH_COUNT) {
            out_data = patch_name(src_data, src_size, src_name, new_name, &out_size);
            if (out_data) { free(src_data); }
            else           { out_data = src_data; out_size = src_size; } /* name not in file, copy as-is */
        } else {
            out_data = src_data; out_size = src_size;
        }

        if (write_file(dst_path, out_data, out_size) == 0) {
            wchar_t msg[128];
            _snwprintf(msg, 128, L"%s  %s  %zu bytes",
                       FILE_TYPES[i] + 1,
                       i < PATCH_COUNT ? L"(patched)" : L"(copied) ",
                       out_size);
            log_ok(msg);
        } else {
            wchar_t msg[MAX_PATH + 32];
            _snwprintf(msg, MAX_PATH + 32, L"Write failed: %s", dst_path);
            log_err(msg); errors++;
        }
        free(out_data);
    }

    log_line(L"");
    if (errors == 0) {
        delete_saving_sav();
        wchar_t msg[256];
        _snwprintf(msg, 256, L"\u2605  Done!  Start TQ2 and '%s' will appear in character selection.", new_namew);
        log_line(msg);
        scan_characters();
    } else {
        log_err(L"Finished with errors.");
    }
}

/* ------------------------------------------------------------------ */
/* Custom-drawn buttons                                                 */
/* ------------------------------------------------------------------ */

/* Clone icon: two overlapping rects with a plus cutout (36-unit SVG space) */
static void draw_clone_icon(HDC dc, int x, int y, int sz, COLORREF col, COLORREF hole)
{
#define IX(v) (x + MulDiv(v, sz, 36))
#define IY(v) (y + MulDiv(v, sz, 36))
    HBRUSH br  = CreateSolidBrush(col);
    HBRUSH brh = CreateSolidBrush(hole);
    RECT r;
    r = (RECT){ IX(4),  IY(4),  IX(22), IY(24) }; FillRect(dc, &r, br);
    r = (RECT){ IX(14), IY(12), IX(30), IY(30) }; FillRect(dc, &r, br);
    r = (RECT){ IX(16), IY(21), IX(28), IY(23) }; FillRect(dc, &r, brh);
    r = (RECT){ IX(21), IY(16), IX(23), IY(28) }; FillRect(dc, &r, brh);
    DeleteObject(br); DeleteObject(brh);
#undef IX
#undef IY
}

static void draw_button(DRAWITEMSTRUCT *dis, BOOL is_clone)
{
    HDC  dc       = dis->hDC;
    RECT r        = dis->rcItem;
    BOOL pressed  = (dis->itemState & ODS_SELECTED) != 0;
    BOOL disabled = (dis->itemState & ODS_DISABLED)  != 0;

    COLORREF bg = is_clone
        ? (disabled ? RGB(90,70,0) : (pressed ? CLR_ACCENT2 : CLR_ACCENT))
        : (pressed  ? RGB(50,55,70) : RGB(45,50,65));
    COLORREF fg = is_clone
        ? (disabled ? RGB(120,100,0) : RGB(20,20,20))
        : CLR_TEXT;

    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &r, br);
    DeleteObject(br);

    HPEN pen = CreatePen(PS_SOLID, 1, is_clone ? CLR_ACCENT2 : CLR_BORDER);
    HPEN old = (HPEN)SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, old);
    DeleteObject(pen);

    wchar_t buf[64];
    GetWindowTextW(dis->hwndItem, buf, 64);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    SelectObject(dc, is_clone ? g_font_bold : g_font_normal);

    if (is_clone) {
        int icon_sz = SCALE(18);
        int gap     = SCALE(8);
        SIZE tsz;
        GetTextExtentPoint32W(dc, buf, lstrlenW(buf), &tsz);
        int block_w = icon_sz + gap + tsz.cx;
        int icon_x  = r.left + (r.right  - r.left - block_w) / 2;
        int icon_y  = r.top  + (r.bottom - r.top  - icon_sz) / 2;
        draw_clone_icon(dc, icon_x, icon_y, icon_sz, fg, bg);
        RECT tr = r; tr.left = icon_x + icon_sz + gap;
        DrawTextW(dc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        DrawTextW(dc, buf, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (dis->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(dc, &r);
    }
}


/* ------------------------------------------------------------------ */
/* Window procedure                                                     */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        g_font_normal = CreateFontW(-SCALE(13), 0,0,0, FW_NORMAL,   0,0,0, DEFAULT_CHARSET, 0,0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_bold   = CreateFontW(-SCALE(13), 0,0,0, FW_SEMIBOLD, 0,0,0, DEFAULT_CHARSET, 0,0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_mono   = CreateFontW(-SCALE(12), 0,0,0, FW_NORMAL,   0,0,0, DEFAULT_CHARSET, 0,0, CLEARTYPE_QUALITY, 0, L"Consolas");
        g_br_bg   = CreateSolidBrush(CLR_BG);
        g_br_edit = CreateSolidBrush(CLR_EDIT_BG);

        HICON ic = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
        if (ic) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)ic);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)ic);
        }

        RECT cr; GetClientRect(hwnd, &cr);
        int pad    = SCALE(20);
        int mx     = pad;
        int y      = pad;
        int w      = cr.right - 2 * pad;
        int btn_h  = SCALE(38);
        int lbl_h  = SCALE(18);
        int gap_sm = SCALE(6);
        int gap_lg = SCALE(14);

        HWND h_title = CreateWindowW(L"STATIC", L"TQ2 Character Clone",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, w, SCALE(28), hwnd, (HMENU)ID_LBL_TITLE, NULL, NULL);
        SendMessageW(h_title, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        y += SCALE(34);

        CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            mx, y, w, SCALE(2), hwnd, NULL, NULL, NULL);
        y += SCALE(14);

        HWND lbl1 = CreateWindowW(L"STATIC", L"Source Character",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, SCALE(200), lbl_h, hwnd, (HMENU)ID_LBL_SRC, NULL, NULL);
        SendMessageW(lbl1, WM_SETFONT, (WPARAM)g_font_normal, TRUE);
        y += lbl_h + gap_sm;

        int refresh_w = SCALE(88);
        int combo_w   = w - refresh_w - SCALE(8);

        g_combo_src = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_SORT,
            mx, y, combo_w, SCALE(200), hwnd, (HMENU)ID_COMBO_SRC, NULL, NULL);
        SendMessageW(g_combo_src, WM_SETFONT, (WPARAM)g_font_normal, TRUE);

        RECT cr2; GetWindowRect(g_combo_src, &cr2);
        int ctrl_h = cr2.bottom - cr2.top;

        HWND btn_ref = CreateWindowW(L"BUTTON", L"\u21BA  Refresh",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            mx + combo_w + SCALE(8), y, refresh_w, ctrl_h,
            hwnd, (HMENU)ID_BTN_REFRESH, NULL, NULL);
        SendMessageW(btn_ref, WM_SETFONT, (WPARAM)g_font_normal, TRUE);
        y += ctrl_h + gap_lg;

        HWND lbl2 = CreateWindowW(L"STATIC", L"New Character Name",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, SCALE(300), lbl_h, hwnd, (HMENU)ID_LBL_NEW, NULL, NULL);
        SendMessageW(lbl2, WM_SETFONT, (WPARAM)g_font_normal, TRUE);
        y += lbl_h + gap_sm;

        g_edit_new = CreateWindowW(L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            mx, y, w, ctrl_h, hwnd, (HMENU)ID_EDIT_NEW, NULL, NULL);
        SendMessageW(g_edit_new, WM_SETFONT, (WPARAM)g_font_normal, TRUE);
        SendMessageW(g_edit_new, EM_SETLIMITTEXT, MAX_NAME_LEN, 0);
        y += ctrl_h + gap_lg + SCALE(4);

        g_btn_clone = CreateWindowW(L"BUTTON", L"Clone Character",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            mx, y, w, btn_h, hwnd, (HMENU)ID_BTN_CLONE, NULL, NULL);
        SendMessageW(g_btn_clone, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        y += btn_h + gap_lg;

        HWND lbl3 = CreateWindowW(L"STATIC", L"Log",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, SCALE(80), lbl_h, hwnd, NULL, NULL, NULL);
        SendMessageW(lbl3, WM_SETFONT, (WPARAM)g_font_normal, TRUE);
        y += lbl_h + gap_sm;

        int log_h = cr.bottom - y - pad;
        if (log_h < SCALE(60)) log_h = SCALE(60);
        g_log = CreateWindowW(L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            mx, y, w, log_h, hwnd, (HMENU)ID_LOG, NULL, NULL);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

        scan_characters();
        log_line(L"Ready. Select a character and enter a new name, then click Clone.");
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_CLONE:
            EnableWindow(g_btn_clone, FALSE);
            do_clone();
            EnableWindow(g_btn_clone, TRUE);
            break;
        case ID_BTN_REFRESH:
            scan_characters();
            log_line(L"Character list refreshed.");
            break;
        }
        return 0;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        draw_button(dis, dis->CtlID == ID_BTN_CLONE);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetDlgCtrlID((HWND)lp) == ID_LBL_TITLE ? CLR_ACCENT : CLR_DIM);
        return (LRESULT)g_br_bg;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, CLR_EDIT_BG);
        SetTextColor(dc, CLR_TEXT);
        return (LRESULT)g_br_edit;
    }

    case WM_ERASEBKGND: {
        RECT r; GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, g_br_bg);
        return 1;
    }

    case WM_KEYDOWN:
        if (wp == VK_RETURN) SendMessageW(hwnd, WM_COMMAND, ID_BTN_CLONE, 0);
        return 0;

    case WM_DESTROY:
        DeleteObject(g_font_normal);
        DeleteObject(g_font_bold);
        DeleteObject(g_font_mono);
        DeleteObject(g_br_bg);
        DeleteObject(g_br_edit);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    (void)hPrev; (void)lpCmd;
    g_hInst = hInst;

    /* DPI awareness */
    {
        typedef BOOL (WINAPI *fn_t)(DPI_AWARENESS_CONTEXT);
        fn_t fn = (fn_t)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                        "SetProcessDpiAwarenessContext");
        if (fn) fn(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
        else    SetProcessDPIAware();
    }
    {
        HDC hdc = GetDC(NULL);
        g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
        if (g_dpi < 96) g_dpi = 96;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    wchar_t local_app[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH);
    _snwprintf(g_save_dir, MAX_PATH, L"%s%s", local_app, SAVEGAME_SUBDIR);

    WNDCLASSEXW wc = {
        .cbSize        = sizeof(wc),
        .style         = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc   = wnd_proc,
        .hInstance     = hInst,
        .hCursor       = LoadCursorW(NULL, IDC_ARROW),
        .lpszClassName = L"TQ2Clone",
        .hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1)),
    };
    RegisterClassExW(&wc);

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int ww = SCALE(500), wh = SCALE(480);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, L"TQ2Clone",
        L"Titan Quest II \x2014 Character Clone",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sx - ww) / 2, (sy - wh) / 2, ww, wh,
        NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
