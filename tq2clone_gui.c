/*
 * tq2clone_gui.c  —  Titan Quest II Character Cloner (Win32 GUI)
 *
 * Compile:
 *   cl tq2clone_gui.c /Fe:tq2clone_gui.exe /nologo /O2 ^
 *      /link /SUBSYSTEM:WINDOWS user32.lib comctl32.lib shell32.lib
 *
 * No extra libs needed beyond the Windows SDK.
 */

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

/* Enable visual styles (Common Controls v6) */
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
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <wchar.h>
#include <tlhelp32.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define MAX_NAME_LEN    30
#define SAVEGAME_SUBDIR L"\\TQ2\\Saved\\SaveGames\\"

/* Offsets relative to end of "m_CharacterName\0" */
#define OFF_DATA_SIZE   20
#define OFF_PAD         24
#define OFF_STR_LEN     25
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

/* DPI scaling — set once at startup from system DPI */
static int g_dpi = 96;
#define SCALE(x) MulDiv((x), g_dpi, 96)

/* Colors */
#define CLR_BG          RGB(24, 26, 32)
#define CLR_PANEL       RGB(35, 38, 47)
#define CLR_ACCENT      RGB(255, 176, 0)
#define CLR_ACCENT2     RGB(220, 140, 0)
#define CLR_TEXT        RGB(220, 220, 230)
#define CLR_TEXT_DIM    RGB(140, 145, 160)
#define CLR_GREEN       RGB(80, 200, 120)
#define CLR_RED         RGB(240, 80, 80)
#define CLR_EDIT_BG     RGB(45, 48, 60)
#define CLR_BORDER      RGB(60, 65, 80)

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

static HWND  g_hwnd;
static HWND  g_combo_src;
static HWND  g_edit_new;
static HWND  g_btn_clone;
static HWND  g_btn_refresh;
static HWND  g_log;
static HFONT g_font_normal;
static HFONT g_font_title;
static HFONT g_font_mono;
static HBRUSH g_br_bg;
static HBRUSH g_br_panel;
static HBRUSH g_br_edit;
static wchar_t g_save_dir[MAX_PATH];

/* ------------------------------------------------------------------ */
/* Logging helpers                                                      */
/* ------------------------------------------------------------------ */

static void log_append(const wchar_t *msg, COLORREF clr)
{
    (void)clr; /* simple: just append text; colour via prefix */
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)msg);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static void log_line(const wchar_t *msg)
{
    wchar_t buf[1024];
    _snwprintf(buf, 1024, L"%s\r\n", msg);
    log_append(buf, CLR_TEXT);
}

static void log_ok(const wchar_t *msg)
{
    wchar_t buf[1024];
    _snwprintf(buf, 1024, L"  \u2713 %s\r\n", msg);
    log_append(buf, CLR_GREEN);
}

static void log_err(const wchar_t *msg)
{
    wchar_t buf[1024];
    _snwprintf(buf, 1024, L"  \u2717 %s\r\n", msg);
    log_append(buf, CLR_RED);
}

static void log_clear(void)
{
    SetWindowTextW(g_log, L"");
}

/* ------------------------------------------------------------------ */
/* File I/O                                                             */
/* ------------------------------------------------------------------ */

static uint8_t *read_file_w(const wchar_t *path, size_t *out_size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL);
    uint8_t *buf = (uint8_t *)malloc(sz);
    DWORD rd;
    ReadFile(h, buf, sz, &rd, NULL);
    CloseHandle(h);
    *out_size = rd;
    return buf;
}

static int write_file_w(const wchar_t *path, const uint8_t *data, size_t size)
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
/* CRC-32 (ISO-3309 / zlib polynomial 0xEDB88320)                     */
/* ------------------------------------------------------------------ */

static uint32_t g_crc_table[256];
static int      g_crc_init  = 0;

static void crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0u);
        g_crc_table[i] = c;
    }
    g_crc_init = 1;
}

static uint32_t crc32_buf(const uint8_t *buf, size_t len)
{
    if (!g_crc_init) crc32_init_table();
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) crc = (crc >> 8) ^ g_crc_table[(crc ^ *buf++) & 0xFF];
    return ~crc;
}

static uint32_t crc32_file_w(const wchar_t *path)
{
    if (!g_crc_init) crc32_init_table();
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t  block[4096];
    DWORD    rd;
    while (ReadFile(h, block, sizeof(block), &rd, NULL) && rd > 0) {
        for (DWORD i = 0; i < rd; i++)
            crc = (crc >> 8) ^ g_crc_table[(crc ^ block[i]) & 0xFF];
    }
    CloseHandle(h);
    return ~crc;
}

/* ------------------------------------------------------------------ */
/* Delete Saving.sav so TQ2 recreates it cleanly on next startup.     */
/* ------------------------------------------------------------------ */

static void delete_saving_sav_w(const wchar_t *save_dir)
{
    wchar_t sav_path[MAX_PATH];
    _snwprintf(sav_path, MAX_PATH, L"%sSaving.sav", save_dir);
    if (DeleteFileW(sav_path)) {
        log_ok(L"Saving.sav deleted (game will recreate it on next start)");
        return;
    }
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND) {
        log_ok(L"Saving.sav not present (nothing to delete)");
        return;
    }
    log_err(L"Could not delete Saving.sav — start TQ2 and it will fix itself");
}

/* ------------------------------------------------------------------ */
/* Name patch (ASCII-only; TQ2 names are ASCII)                        */
/* ------------------------------------------------------------------ */

static uint8_t *patch_name(const uint8_t *src, size_t src_size,
                            const char *old_name, const char *new_name,
                            size_t *new_size)
{
    static const char MARKER[] = "m_CharacterName";
    const size_t MARKER_LEN = sizeof(MARKER); /* includes '\0' */
    const size_t old_len = strlen(old_name);
    const size_t new_len = strlen(new_name);

    /* Allocate with generous headroom */
    uint8_t *dst = (uint8_t *)malloc(src_size + new_len + 64);
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
                /* Copy up to DataSize field */
                size_t copy_to = end + OFF_DATA_SIZE;
                memcpy(dst + dp, src + sp, copy_to - sp);
                dp += copy_to - sp;

                /* New DataSize = 4 + new_len + 1 */
                uint32_t new_ds = (uint32_t)(4 + new_len + 1);
                memcpy(dst + dp, &new_ds, 4); dp += 4;

                /* Padding byte */
                dst[dp++] = src[end + OFF_PAD];

                /* New string length = new_len + 1 */
                uint32_t new_sl = (uint32_t)(new_len + 1);
                memcpy(dst + dp, &new_sl, 4); dp += 4;

                /* New name + null */
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

    /* Fix the three GrimSave inner size fields enclosing m_CharacterName */
    {
        ptrdiff_t delta = (ptrdiff_t)new_len - (ptrdiff_t)old_len;
        if (delta != 0) {
            static const char MARKER[] = "m_CharacterName";
            const size_t MLEN = sizeof(MARKER);
            size_t cn = 0;
            for (size_t i = 0; i + MLEN <= dp; i++) {
                if (memcmp(dst + i, MARKER, MLEN) == 0) { cn = i; break; }
            }
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
    }

    return dst;
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

    /* Collect unique character names */
    wchar_t names[64][MAX_NAME_LEN + 1];
    int count = 0;

    do {
        /* Filename: <name>_YYYY-MM-DD--HH-MM-SS_Header.sav */
        wchar_t *us = wcschr(fd.cFileName, L'_');
        if (!us) continue;
        size_t nlen = (size_t)(us - fd.cFileName);
        if (nlen == 0 || nlen > MAX_NAME_LEN) continue;

        /* Dedup */
        wchar_t name[MAX_NAME_LEN + 1] = {0};
        wcsncpy(name, fd.cFileName, nlen);
        int dup = 0;
        for (int i = 0; i < count; i++)
            if (wcscmp(names[i], name) == 0) { dup = 1; break; }
        if (!dup && count < 64) {
            wcscpy(names[count++], name);
            SendMessageW(g_combo_src, CB_ADDSTRING, 0, (LPARAM)name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (count > 0)
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
static const int PATCH_COUNT = 2; /* Header + Data_Player */

static void do_clone(void)
{
    log_clear();

    /* Abort if TQ2 is running */
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(pe);
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
    }

    /* Read source name from combo */
    wchar_t src_namew[MAX_NAME_LEN + 1] = {0};
    int src_idx = (int)SendMessageW(g_combo_src, CB_GETCURSEL, 0, 0);
    if (src_idx < 0) {
        log_err(L"No source character selected.");
        return;
    }
    SendMessageW(g_combo_src, CB_GETLBTEXT, src_idx, (LPARAM)src_namew);

    /* Read new name from edit */
    wchar_t new_namew[MAX_NAME_LEN + 1] = {0};
    GetWindowTextW(g_edit_new, new_namew, MAX_NAME_LEN + 1);
    /* Trim trailing spaces */
    for (int i = (int)wcslen(new_namew) - 1; i >= 0 && new_namew[i] == L' '; i--)
        new_namew[i] = 0;

    if (wcslen(new_namew) == 0) {
        log_err(L"New character name cannot be empty.");
        return;
    }
    if (wcslen(new_namew) > MAX_NAME_LEN) {
        wchar_t msg[128];
        _snwprintf(msg, 128, L"Name too long (max %d characters).", MAX_NAME_LEN);
        log_err(msg);
        return;
    }
    if (wcscmp(src_namew, new_namew) == 0) {
        log_err(L"New name is the same as source name.");
        return;
    }

    /* Convert to narrow ASCII for patch_name */
    char src_name[MAX_NAME_LEN + 1], new_name[MAX_NAME_LEN + 1];
    WideCharToMultiByte(CP_ACP, 0, src_namew, -1, src_name, sizeof(src_name), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, new_namew, -1, new_name, sizeof(new_name), NULL, NULL);

    /* Find source timestamp */
    wchar_t search_pat[MAX_PATH];
    _snwprintf(search_pat, MAX_PATH, L"%s%s_*_Header.sav", g_save_dir, src_namew);
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(search_pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) {
        log_err(L"Save files not found for selected character.");
        return;
    }
    wchar_t timestamp[64] = {0};
    const wchar_t *ts_start = fd.cFileName + wcslen(src_namew) + 1;
    const wchar_t *ts_end   = wcsstr(ts_start, L"_Header.sav");
    if (ts_end)
        wcsncpy(timestamp, ts_start, (size_t)(ts_end - ts_start));
    FindClose(hf);

    /* New timestamp from current time */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    wchar_t new_ts[64];
    wcsftime(new_ts, 64, L"%Y-%m-%d--%H-%M-%S", t);

    /* Display plan */
    wchar_t info[256];
    _snwprintf(info, 256, L"Cloning  %s  \u2192  %s", src_namew, new_namew);
    log_line(info);
    _snwprintf(info, 256, L"Source save : %s", timestamp);
    log_line(info);
    _snwprintf(info, 256, L"New save    : %s", new_ts);
    log_line(info);
    log_line(L"");

    int errors = 0;

    for (int i = 0; FILE_TYPES[i]; i++) {
        wchar_t src_path[MAX_PATH], dst_path[MAX_PATH];
        _snwprintf(src_path, MAX_PATH, L"%s%s_%s%s",
                   g_save_dir, src_namew, timestamp, FILE_TYPES[i]);
        _snwprintf(dst_path, MAX_PATH, L"%s%s_%s%s",
                   g_save_dir, new_namew, new_ts, FILE_TYPES[i]);

        size_t   src_size = 0;
        uint8_t *src_data = read_file_w(src_path, &src_size);
        if (!src_data) {
            wchar_t msg[MAX_PATH + 32];
            _snwprintf(msg, MAX_PATH + 32, L"Cannot read: %s", src_path);
            log_err(msg);
            errors++;
            continue;
        }

        uint8_t *out_data = NULL;
        size_t   out_size = 0;

        if (i < PATCH_COUNT) {
            out_data = patch_name(src_data, src_size,
                                  src_name, new_name, &out_size);
            if (!out_data) {
                /* Fallback: copy unchanged (name not found in this file) */
                out_data = src_data; out_size = src_size; src_data = NULL;
            } else {
                free(src_data); src_data = NULL;
            }
        } else {
            out_data = src_data; out_size = src_size; src_data = NULL;
        }

        if (write_file_w(dst_path, out_data, out_size) == 0) {
            wchar_t msg[128];
            const wchar_t *tag = (i < PATCH_COUNT) ? L"(patched)" : L"(copied) ";
            _snwprintf(msg, 128, L"%s  %s  %zu bytes",
                       FILE_TYPES[i] + 1, tag, out_size);
            log_ok(msg);
        } else {
            wchar_t msg[MAX_PATH + 32];
            _snwprintf(msg, MAX_PATH + 32, L"Write failed: %s", dst_path);
            log_err(msg);
            errors++;
        }
        free(out_data);
    }

    if (errors == 0) {
        log_line(L"");
        /* Delete Saving.sav so TQ2 recreates it cleanly on next startup */
        delete_saving_sav_w(g_save_dir);
        wchar_t msg[256];
        _snwprintf(msg, 256,
            L"\u2605  Done!  Start TQ2 and '%s' will appear in character selection.",
            new_namew);
        log_line(msg);
        /* Refresh combo to show new character */
        scan_characters();
    } else {
        log_err(L"Finished with errors. Check paths above.");
    }
}

/* ------------------------------------------------------------------ */
/* Custom-drawn button                                                  */
/* ------------------------------------------------------------------ */

/* Clone icon: two overlapping rects with a plus cutout, scaled from 36-unit SVG */
static void draw_clone_icon(HDC dc, int x, int y, int sz, COLORREF col, COLORREF hole_col)
{
#define ICX(v) (x + MulDiv(v, sz, 36))
#define ICY(v) (y + MulDiv(v, sz, 36))
    HBRUSH br  = CreateSolidBrush(col);
    HBRUSH brh = CreateSolidBrush(hole_col);
    RECT r;
    /* Back rect */
    r.left = ICX(4);  r.top = ICY(4);  r.right = ICX(22); r.bottom = ICY(24); FillRect(dc, &r, br);
    /* Front rect */
    r.left = ICX(14); r.top = ICY(12); r.right = ICX(30); r.bottom = ICY(30); FillRect(dc, &r, br);
    /* Plus cutout — horizontal bar */
    r.left = ICX(16); r.top = ICY(21); r.right = ICX(28); r.bottom = ICY(23); FillRect(dc, &r, brh);
    /* Plus cutout — vertical bar */
    r.left = ICX(21); r.top = ICY(16); r.right = ICX(23); r.bottom = ICY(28); FillRect(dc, &r, brh);
    DeleteObject(br);
    DeleteObject(brh);
#undef ICX
#undef ICY
}

static void draw_button(DRAWITEMSTRUCT *dis, BOOL is_clone)
{
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    BOOL pressed  = (dis->itemState & ODS_SELECTED) != 0;
    BOOL disabled = (dis->itemState & ODS_DISABLED) != 0;

    COLORREF bg = is_clone
        ? (disabled ? RGB(90,70,0) : (pressed ? CLR_ACCENT2 : CLR_ACCENT))
        : (pressed  ? RGB(50,55,70) : RGB(45,50,65));
    COLORREF fg = is_clone
        ? (disabled ? RGB(120,100,0) : RGB(20,20,20))
        : CLR_TEXT;

    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dc, &r, br);
    DeleteObject(br);

    /* Subtle border */
    HPEN pen = CreatePen(PS_SOLID, 1, is_clone ? CLR_ACCENT2 : CLR_BORDER);
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, old_pen);
    DeleteObject(pen);

    /* Label (+ icon for clone button) */
    wchar_t buf[64];
    GetWindowTextW(dis->hwndItem, buf, 64);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    SelectObject(dc, is_clone ? g_font_title : g_font_normal);

    if (is_clone) {
        int icon_sz  = SCALE(18);
        int gap      = SCALE(8);
        int btn_w    = r.right - r.left;
        int btn_h    = r.bottom - r.top;
        SIZE tsz;
        GetTextExtentPoint32W(dc, buf, lstrlenW(buf), &tsz);
        int block_w  = icon_sz + gap + tsz.cx;
        int icon_x   = r.left + (btn_w - block_w) / 2;
        int icon_y   = r.top  + (btn_h - icon_sz) / 2;
        draw_clone_icon(dc, icon_x, icon_y, icon_sz, fg, bg);
        RECT tr = r;
        tr.left = icon_x + icon_sz + gap;
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
/* TQ2 app icon                                                         */
/* ------------------------------------------------------------------ */

static HICON load_tq2_icon(void)
{
    static const wchar_t *REL = L"\\steamapps\\common\\Titan Quest II"
                                L"\\TQ2\\Binaries\\Win64\\TQ2-Win64-Shipping.exe";
    wchar_t exe[MAX_PATH];

    /* Try Steam path from registry (HKCU, then HKLM) */
    const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    for (int i = 0; i < 2; i++) {
        wchar_t steam[MAX_PATH] = {0};
        DWORD sz = sizeof(steam);
        if (RegGetValueW(roots[i], L"SOFTWARE\\Valve\\Steam", L"SteamPath",
                         RRF_RT_REG_SZ, NULL, steam, &sz) == ERROR_SUCCESS) {
            for (wchar_t *p = steam; *p; p++) if (*p == L'/') *p = L'\\';
            _snwprintf(exe, MAX_PATH, L"%s%s", steam, REL);
            HICON ic = ExtractIconW(GetModuleHandleW(NULL), exe, 0);
            if (ic && ic != (HICON)(ULONG_PTR)1) return ic;
        }
    }

    /* Fallback: default Steam install location */
    _snwprintf(exe, MAX_PATH, L"C:\\Program Files (x86)\\Steam%s", REL);
    HICON ic = ExtractIconW(GetModuleHandleW(NULL), exe, 0);
    if (ic && ic != (HICON)(ULONG_PTR)1) return ic;

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                     */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg,
                                  WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        /* Fonts — use negative height so GDI treats it as em-height in
           scaled pixels; SCALE() maps our 96-DPI baseline to actual DPI. */
        g_font_normal = CreateFontW(-SCALE(13), 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_title  = CreateFontW(-SCALE(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_mono   = CreateFontW(-SCALE(12), 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");

        g_br_bg    = CreateSolidBrush(CLR_BG);
        g_br_panel = CreateSolidBrush(CLR_PANEL);
        g_br_edit  = CreateSolidBrush(CLR_EDIT_BG);

        /* TQ2 window icon */
        {
            HICON ic = load_tq2_icon();
            if (ic) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)ic);
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)ic);
            }
        }

        /* Equal padding on all four sides: derive content width from the
           actual client rect so left margin == right margin == pad.       */
        RECT cr; GetClientRect(hwnd, &cr);
        int pad    = SCALE(20);
        int mx     = pad;
        int y      = pad;
        int w      = cr.right - 2 * pad;

        int btn_h  = SCALE(38);
        int lbl_h  = SCALE(18);
        int gap_sm = SCALE(6);
        int gap_lg = SCALE(14);

        /* Title */
        HWND lbl_title = CreateWindowW(L"STATIC",
            L"TQ2 Character Clone",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, w, SCALE(28), hwnd, (HMENU)ID_LBL_TITLE, NULL, NULL);
        SendMessageW(lbl_title, WM_SETFONT, (WPARAM)g_font_title, TRUE);

        y += SCALE(34);

        /* Separator line */
        CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
            mx, y, w, SCALE(2), hwnd, NULL, NULL, NULL);

        y += SCALE(14);

        /* Source label */
        HWND lbl1 = CreateWindowW(L"STATIC", L"Source Character",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, SCALE(200), lbl_h, hwnd, (HMENU)ID_LBL_SRC, NULL, NULL);
        SendMessageW(lbl1, WM_SETFONT, (WPARAM)g_font_normal, TRUE);

        y += lbl_h + gap_sm;

        /* Source combo — create first, then measure its actual closed height
           so the Refresh button can match it exactly.                       */
        int refresh_w = SCALE(88);
        int combo_w   = w - refresh_w - SCALE(8);

        g_combo_src = CreateWindowW(L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_SORT,
            mx, y, combo_w, SCALE(200), hwnd, (HMENU)ID_COMBO_SRC, NULL, NULL);
        SendMessageW(g_combo_src, WM_SETFONT, (WPARAM)g_font_normal, TRUE);

        /* ctrl_h = the real pixel height of the closed combo box. */
        RECT combo_rect; GetWindowRect(g_combo_src, &combo_rect);
        int ctrl_h = combo_rect.bottom - combo_rect.top;

        g_btn_refresh = CreateWindowW(L"BUTTON", L"\u21BA  Refresh",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            mx + combo_w + SCALE(8), y, refresh_w, ctrl_h,
            hwnd, (HMENU)ID_BTN_REFRESH, NULL, NULL);
        SendMessageW(g_btn_refresh, WM_SETFONT, (WPARAM)g_font_normal, TRUE);

        y += ctrl_h + gap_lg;

        /* New name label */
        HWND lbl2 = CreateWindowW(L"STATIC", L"New Character Name",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, SCALE(300), lbl_h, hwnd, (HMENU)ID_LBL_NEW, NULL, NULL);
        SendMessageW(lbl2, WM_SETFONT, (WPARAM)g_font_normal, TRUE);

        y += lbl_h + gap_sm;

        /* New name edit — same ctrl_h as combo for visual consistency */
        g_edit_new = CreateWindowW(L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            mx, y, w, ctrl_h, hwnd, (HMENU)ID_EDIT_NEW, NULL, NULL);
        SendMessageW(g_edit_new, WM_SETFONT, (WPARAM)g_font_normal, TRUE);
        SendMessageW(g_edit_new, EM_SETLIMITTEXT, MAX_NAME_LEN, 0);

        y += ctrl_h + gap_lg + SCALE(4);

        /* Clone button */
        g_btn_clone = CreateWindowW(L"BUTTON", L"Clone Character",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            mx, y, w, btn_h, hwnd, (HMENU)ID_BTN_CLONE, NULL, NULL);
        SendMessageW(g_btn_clone, WM_SETFONT, (WPARAM)g_font_title, TRUE);

        y += btn_h + gap_lg;

        /* Log label */
        HWND lbl3 = CreateWindowW(L"STATIC", L"Log",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            mx, y, SCALE(80), lbl_h, hwnd, NULL, NULL, NULL);
        SendMessageW(lbl3, WM_SETFONT, (WPARAM)g_font_normal, TRUE);

        y += lbl_h + gap_sm;

        /* Log area — stretches to fill remaining height with equal bottom pad */
        int log_h = cr.bottom - y - pad;
        if (log_h < SCALE(60)) log_h = SCALE(60);

        g_log = CreateWindowW(L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
            mx, y, w, log_h, hwnd, (HMENU)ID_LOG, NULL, NULL);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

        /* Populate characters */
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
        if (dis->CtlID == ID_BTN_CLONE)
            draw_button(dis, TRUE);
        else if (dis->CtlID == ID_BTN_REFRESH)
            draw_button(dis, FALSE);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        int id = GetDlgCtrlID(ctl);
        SetBkMode(dc, TRANSPARENT);
        if (id == ID_LBL_TITLE) {
            SetTextColor(dc, CLR_ACCENT);
        } else {
            SetTextColor(dc, CLR_TEXT_DIM);
        }
        return (LRESULT)g_br_bg;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, CLR_EDIT_BG);
        SetTextColor(dc, CLR_TEXT);
        return (LRESULT)g_br_edit;
    }

    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, CLR_EDIT_BG);
        SetTextColor(dc, CLR_TEXT);
        return (LRESULT)g_br_edit;
    }

    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(dc, &r, g_br_bg);
        return 1;
    }

    case WM_KEYDOWN:
        if (wp == VK_RETURN) SendMessageW(hwnd, WM_COMMAND, ID_BTN_CLONE, 0);
        return 0;

    case WM_DESTROY:
        DeleteObject(g_font_normal);
        DeleteObject(g_font_title);
        DeleteObject(g_font_mono);
        DeleteObject(g_br_bg);
        DeleteObject(g_br_panel);
        DeleteObject(g_br_edit);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev,
                   LPSTR lpCmd, int nShow)
{
    (void)hPrev; (void)lpCmd;

    /* DPI awareness: opt into system-DPI scaling so Windows reports the
       real DPI instead of virtualising it.  We then scale everything
       ourselves via SCALE(), which gives sharp text at any DPI. */
    {
        /* Try the Win10 API first; fall back to the Vista-era one. */
        typedef BOOL (WINAPI *SetDpiCtx_t)(DPI_AWARENESS_CONTEXT);
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        SetDpiCtx_t fn = (SetDpiCtx_t)GetProcAddress(u32,
                          "SetProcessDpiAwarenessContext");
        if (fn)
            fn(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
        else
            SetProcessDPIAware();
    }

    /* Query system DPI (device pixels per logical inch on the primary monitor) */
    {
        HDC hdc = GetDC(NULL);
        g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
        if (g_dpi < 96) g_dpi = 96; /* sanity floor */
    }

    /* Common controls */
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    /* Build save directory path */
    wchar_t local_app[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH);
    _snwprintf(g_save_dir, MAX_PATH, L"%s%s", local_app, SAVEGAME_SUBDIR);

    /* Register window class */
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; /* we paint in WM_ERASEBKGND */
    wc.lpszClassName = L"TQ2Clone";
    wc.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    int win_w = SCALE(500), win_h = SCALE(480);

    /* Centre on screen — SM_CX/CYSCREEN returns physical pixels when
       the process is DPI-aware, so no further scaling needed here. */
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"TQ2Clone",
        L"Titan Quest II \x2014 Character Clone",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sx - win_w) / 2, (sy - win_h) / 2,
        win_w, win_h,
        NULL, NULL, hInstance, NULL);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
