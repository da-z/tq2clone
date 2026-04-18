/*
 * tq2clone.c - Clone a Titan Quest II character save under a new name.
 *
 * Usage: tq2clone <source_name> <new_name>
 *
 * Clones all save files for <source_name> into a new set named <new_name>.
 * The new character appears as a separate entry in the character selection screen.
 *
 * Patched files: Header.sav, Data_Player.sav
 * Copied unchanged: Data_PlayerLocal.sav, Data_WorldCampaign.sav, Data_WorldFluff.sav
 * Updated:  Saving.sav (checksum manifest)
 *
 * Compile (MinGW / MSVC):
 *   gcc -o tq2clone.exe tq2clone.c
 *   cl tq2clone.c /Fe:tq2clone.exe
 *
 * Save file location:
 *   %LOCALAPPDATA%\TQ2\Saved\SaveGames\
 *
 * GVAS StrProperty layout for m_CharacterName (offsets from end of "m_CharacterName\0"):
 *   +0  ..+3   int32  type-name length (= 12, "StrProperty\0")
 *   +4  ..+15  char[] "StrProperty\0"
 *   +16 ..+19  int32  flags (= 0)
 *   +20 ..+23  int32  DataSize (= 4 + string_length_incl_null)
 *   +24        byte   padding
 *   +25 ..+28  int32  string_length (includes null terminator)
 *   +29 ..     char[] character name + '\0'
 *
 * Saving.sav format (GrimSave manifest):
 *   "GrimSavingFileMarker\n" + JSON with:
 *   SavedFiles: { "SLOT": { BackupFileChecksum, HasBackup, NewFileChecksum, HasNewFile }, ... }
 *   Checksum: CRC32 of the manifest body (algorithm unknown; we preserve existing or use 0)
 *   Per-file checksums use standard CRC-32 (zlib/ISO-3309 polynomial 0xEDB88320).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define MAX_NAME_LEN     30
#define SAVEGAME_SUBDIR  "\\TQ2\\Saved\\SaveGames\\"

/* Relative to end of "m_CharacterName\0" (16 bytes) */
#define OFF_DATA_SIZE    20   /* int32: 4 + string_length_incl_null      */
#define OFF_PAD          24   /* byte: padding (preserve as-is)           */
#define OFF_STR_LEN      25   /* int32: string length including '\0'      */
#define OFF_STR_DATA     29   /* char[]: name bytes + '\0'                */

/* ------------------------------------------------------------------ */
/* CRC-32 (ISO-3309 / zlib polynomial 0xEDB88320)                     */
/* ------------------------------------------------------------------ */

static uint32_t g_crc_table[256];
static int      g_crc_init  = 0;

static void crc32_init(void)
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
    if (!g_crc_init) crc32_init();
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) crc = (crc >> 8) ^ g_crc_table[(crc ^ *buf++) & 0xFF];
    return ~crc;
}

static uint32_t crc32_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    if (!g_crc_init) crc32_init();
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++)
            crc = (crc >> 8) ^ g_crc_table[(crc ^ buf[i]) & 0xFF];
    }
    fclose(f);
    return ~crc;
}

/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    *out_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(*out_size);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, *out_size, f);
    fclose(f);
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot write: %s\n", path); return -1; }
    fwrite(data, 1, size, f);
    fclose(f);
    return 0;
}

/*
 * Patch every occurrence of the m_CharacterName StrProperty that holds
 * old_name (ASCII, no null) and replace it with new_name.
 *
 * Returns a newly allocated buffer (caller must free) and sets *new_size.
 * Returns NULL on error.
 */
static uint8_t *patch_name(const uint8_t *src, size_t src_size,
                            const char *old_name, const char *new_name,
                            size_t *new_size)
{
    static const char MARKER[] = "m_CharacterName";
    const size_t MARKER_LEN    = sizeof(MARKER); /* includes '\0' */

    const size_t old_len = strlen(old_name);
    const size_t new_len = strlen(new_name);
    const ptrdiff_t delta = (ptrdiff_t)new_len - (ptrdiff_t)old_len;

    /* Count patches needed to pre-allocate */
    size_t patches = 0;
    for (size_t i = 0; i + MARKER_LEN + OFF_STR_DATA + old_len < src_size; i++) {
        if (memcmp(src + i, MARKER, MARKER_LEN) != 0) continue;
        size_t end = i + MARKER_LEN;
        if (end + OFF_STR_DATA + old_len + 1 > src_size) break;
        if (memcmp(src + end + OFF_STR_DATA, old_name, old_len) == 0 &&
            src[end + OFF_STR_DATA + old_len] == '\0')
            patches++;
    }

    if (patches == 0) {
        fprintf(stderr, "  [skip] character name '%s' not found in this file\n", old_name);
        /* Return a plain copy */
        uint8_t *copy = (uint8_t *)malloc(src_size);
        memcpy(copy, src, src_size);
        *new_size = src_size;
        return copy;
    }

    *new_size = src_size + patches * (size_t)(delta < 0 ? -delta : delta)
                                   * (size_t)(delta < 0 ? 0 : 1);
    /* Safe upper bound */
    *new_size = src_size + patches * (new_len + 1);

    uint8_t *dst = (uint8_t *)malloc(*new_size);
    size_t   dp  = 0;
    size_t   sp  = 0;

    while (sp < src_size) {
        if (sp + MARKER_LEN <= src_size &&
            memcmp(src + sp, MARKER, MARKER_LEN) == 0)
        {
            size_t end = sp + MARKER_LEN;

            /* Verify the string data matches old_name */
            if (end + OFF_STR_DATA + old_len + 1 <= src_size &&
                memcmp(src + end + OFF_STR_DATA, old_name, old_len) == 0 &&
                src[end + OFF_STR_DATA + old_len] == '\0')
            {
                /* Copy everything up to (not including) DataSize field */
                size_t copy_to = end + OFF_DATA_SIZE;
                memcpy(dst + dp, src + sp, copy_to - sp);
                dp += copy_to - sp;

                /* Write new DataSize = 4 + new_len + 1 */
                uint32_t new_ds = (uint32_t)(4 + new_len + 1);
                memcpy(dst + dp, &new_ds, 4);
                dp += 4;

                /* Preserve padding byte at OFF_PAD */
                dst[dp++] = src[end + OFF_PAD];

                /* Write new string length = new_len + 1 */
                uint32_t new_sl = (uint32_t)(new_len + 1);
                memcpy(dst + dp, &new_sl, 4);
                dp += 4;

                /* Write new name + null */
                memcpy(dst + dp, new_name, new_len);
                dp += new_len;
                dst[dp++] = '\0';

                /* Advance source past old string data */
                sp = end + OFF_STR_DATA + old_len + 1;
                continue;
            }
        }
        dst[dp++] = src[sp++];
    }

    *new_size = dp;

    /*
     * Fix the three GrimSave inner size fields that enclose m_CharacterName.
     * These are int32 fields at fixed offsets before the marker:
     *   -9   PlayerState object data size
     *   -81  DataArray size
     *   -336 outer GrimSave serialized data size
     * All must grow by delta bytes to match the longer/shorter name.
     * Guard: only update values > 10000 (plausible sizes, not random bytes).
     * Header.sav has no DataArray, so those offsets hold arbitrary bytes;
     * the guard prevents corrupting Header.sav.
     */
    if (patches > 0 && delta != 0) {
        /* Find m_CharacterName in dst (same position as in src) */
        size_t cn = 0;
        for (size_t i = 0; i + MARKER_LEN <= dp; i++) {
            if (memcmp(dst + i, MARKER, MARKER_LEN) == 0) { cn = i; break; }
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

    return dst;
}

/* ------------------------------------------------------------------ */
/*
 * Delete Saving.sav so the game recreates it on next startup.
 *
 * Saving.sav is TQ2's GrimSave manifest: a custom JSON file with a
 * master Checksum whose algorithm is proprietary.  Editing it with an
 * incorrect Checksum triggers "Save error Code: 63" and breaks all saves.
 *
 * Safe strategy: delete the file.  TQ2 handles a missing Saving.sav
 * gracefully — it loads all existing .sav files without checksum
 * verification and then writes a fresh, correct manifest.  The cloned
 * character files are therefore accepted on the very first game start.
 */
static int delete_saving_sav(const char *save_dir)
{
    char sav_path[MAX_PATH];
    snprintf(sav_path, sizeof(sav_path), "%sSaving.sav", save_dir);

    if (DeleteFileA(sav_path)) {
        printf("  -> Saving.sav deleted (game will recreate it on next start)\n");
        return 0;
    }
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND) {
        printf("  -> Saving.sav not present (nothing to delete)\n");
        return 0;
    }
    fprintf(stderr, "  [warn] Could not delete Saving.sav (error %lu)\n", err);
    return -1;
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr,
            "Usage: tq2clone <source_name> <new_name>\n"
            "  Clones a TQ2 character save under a new name.\n"
            "  Character names are limited to %d characters.\n",
            MAX_NAME_LEN);
        return 1;
    }

    const char *src_name = argv[1];
    const char *new_name = argv[2];

    if (strlen(src_name) > MAX_NAME_LEN || strlen(new_name) > MAX_NAME_LEN) {
        fprintf(stderr, "Error: name must be %d characters or fewer.\n", MAX_NAME_LEN);
        return 1;
    }
    if (strlen(new_name) == 0) {
        fprintf(stderr, "Error: new name cannot be empty.\n");
        return 1;
    }

    /* Build save directory path */
    char local_app[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", local_app, sizeof(local_app))) {
        fprintf(stderr, "Error: could not read %%LOCALAPPDATA%%\n");
        return 1;
    }
    char save_dir[MAX_PATH];
    snprintf(save_dir, sizeof(save_dir), "%s%s", local_app, SAVEGAME_SUBDIR);

    /* Find source files: <src_name>_*_Header.sav */
    char search_pattern[MAX_PATH];
    snprintf(search_pattern, sizeof(search_pattern), "%s%s_*_Header.sav", save_dir, src_name);

    WIN32_FIND_DATAA fd;
    HANDLE hfind = FindFirstFileA(search_pattern, &fd);
    if (hfind == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Error: no save files found for character '%s'\n"
                        "  Searched: %s\n", src_name, search_pattern);
        return 1;
    }

    /* Extract timestamp from first matching filename */
    /* Filename format: <name>_YYYY-MM-DD--HH-MM-SS_Header.sav */
    char timestamp[64] = {0};
    const char *fname = fd.cFileName;
    size_t prefix_len = strlen(src_name) + 1; /* name + '_' */
    const char *ts_start = fname + prefix_len;
    const char *ts_end   = strstr(ts_start, "_Header.sav");
    if (!ts_end || (ts_end - ts_start) >= (ptrdiff_t)sizeof(timestamp)) {
        fprintf(stderr, "Error: unexpected filename format: %s\n", fname);
        FindClose(hfind);
        return 1;
    }
    strncpy(timestamp, ts_start, (size_t)(ts_end - ts_start));
    FindClose(hfind);

    /* Generate new timestamp for the clone */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char new_timestamp[64];
    strftime(new_timestamp, sizeof(new_timestamp), "%Y-%m-%d--%H-%M-%S", t);

    /* Abort if TQ2 is running — modifying saves while the game holds
       file handles can corrupt data and the game will overwrite our work. */
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32 pe;
            pe.dwSize = sizeof(pe);
            if (Process32First(snap, &pe)) {
                do {
                    if (_stricmp(pe.szExeFile, "TQ2-Win64-Shipping.exe") == 0 ||
                        _stricmp(pe.szExeFile, "TitanQuest2.exe") == 0) {
                        CloseHandle(snap);
                        fprintf(stderr,
                            "Error: Titan Quest II is currently running.\n"
                            "  Please exit the game before cloning a character.\n");
                        return 1;
                    }
                } while (Process32Next(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

    printf("Source character : %s\n", src_name);
    printf("New character    : %s\n", new_name);
    printf("Source timestamp : %s\n", timestamp);
    printf("New timestamp    : %s\n", new_timestamp);
    printf("Save directory   : %s\n\n", save_dir);

    /* File types: first two get name-patched, rest are plain copies */
    const char *file_types[] = {
        "_Header.sav",
        "_Data_Player.sav",
        "_Data_PlayerLocal.sav",
        "_Data_WorldCampaign.sav",
        "_Data_WorldFluff.sav",
        NULL
    };
    const int PATCH_COUNT = 2; /* Header + Data_Player */

    int errors = 0;

    for (int i = 0; file_types[i]; i++) {
        char src_path[MAX_PATH], dst_path[MAX_PATH];
        snprintf(src_path, sizeof(src_path), "%s%s_%s%s",
                 save_dir, src_name, timestamp, file_types[i]);
        snprintf(dst_path, sizeof(dst_path), "%s%s_%s%s",
                 save_dir, new_name, new_timestamp, file_types[i]);

        printf("Processing: %s%s\n", src_name, file_types[i]);

        size_t   src_size = 0;
        uint8_t *src_data = read_file(src_path, &src_size);
        if (!src_data) { errors++; continue; }

        if (i < PATCH_COUNT) {
            size_t   out_size = 0;
            uint8_t *out_data = patch_name(src_data, src_size,
                                           src_name, new_name, &out_size);
            free(src_data);
            if (!out_data) { errors++; continue; }
            if (write_file(dst_path, out_data, out_size) == 0)
                printf("  -> %s%s  (%zu -> %zu bytes)\n",
                       new_name, file_types[i], src_size, out_size);
            else
                errors++;
            free(out_data);
        } else {
            if (write_file(dst_path, src_data, src_size) == 0)
                printf("  -> %s%s  (%zu bytes, copied)\n",
                       new_name, file_types[i], src_size);
            else
                errors++;
            free(src_data);
        }
    }

    /* Delete Saving.sav so TQ2 recreates it cleanly on next start */
    printf("\nRemoving Saving.sav manifest...\n");
    delete_saving_sav(save_dir);

    if (errors)
        printf("\nDone with %d error(s).\n", errors);
    else
        printf("\nDone! Start TQ2 and '%s' should appear in character selection.\n", new_name);

    return errors ? 1 : 0;
}
