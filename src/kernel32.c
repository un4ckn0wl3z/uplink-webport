// KERNEL32 / file system shims.
#include "host.h"
#include "netplay.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_p(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0777)
#endif

char g_game_dir[512] = ".";
char g_save_dir[512] = "save";
extern int g_fs_dirty;
static uint32_t last_error;

// ---------------------------------------------------------------- paths
static int find_ci(const char *dir, const char *name, char *out, int outsz) {
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) { snprintf(out, outsz, "%s", e->d_name); found = 1; break; }
    }
    closedir(d);
    return found;
}

// normalise guest path into list of components relative to game root
static int split_path(const char *gp, char comps[][128], int max) {
    char tmp[1024]; int n = 0;
    snprintf(tmp, sizeof tmp, "%s", gp);
    for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
    char *s = tmp;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') s += 2;
    while (*s == '/') s++;
    if (!strncasecmp(s, "GAME/", 5)) s += 5;
    else if (!strcasecmp(s, "GAME")) s += 4;
    char *tok = strtok(s, "/");
    while (tok) {
        if (!strcmp(tok, ".") || !tok[0]) {}
        else if (!strcmp(tok, "..")) { if (n) n--; }
        else if (n < max) { snprintf(comps[n++], 128, "%s", tok); }
        tok = strtok(NULL, "/");
    }
    return n;
}

static int resolve_in(const char *root, char comps[][128], int n, char *out, int outsz, int create_dirs) {
    char cur[1024]; snprintf(cur, sizeof cur, "%s", root);
    for (int i = 0; i < n; i++) {
        char real[256];
        if (find_ci(cur, comps[i], real, sizeof real)) {
            size_t l = strlen(cur);
            snprintf(cur + l, sizeof cur - l, "/%s", real);
        } else {
            if (!create_dirs) return 0;
            size_t l = strlen(cur);
            snprintf(cur + l, sizeof cur - l, "/%s", comps[i]);
            if (i < n - 1) mkdir_p(cur);
        }
    }
    snprintf(out, outsz, "%s", cur);
    return 1;
}

// The game unpacks archive members to temp/ before opening them: on the web that goes to memory, not to the
// saves synced to IndexedDB.
#ifdef __EMSCRIPTEN__
static const char *tmp_root = "/tmp";
#else
static const char *tmp_root = NULL;
#endif
int fs_resolve(const char *gp, char *out, int outsz, int for_write) {
    char comps[32][128];
    int n = split_path(gp, comps, 32);
    if (tmp_root && n > 1 && !strcasecmp(comps[0], "temp")) {
        if (for_write) return resolve_in(tmp_root, comps, n, out, outsz, 1);
        if (resolve_in(tmp_root, comps, n, out, outsz, 0)) return 1;
    }
    if (for_write) {
        mkdir_p(g_save_dir);
        return resolve_in(g_save_dir, comps, n, out, outsz, 1);
    }
    if (resolve_in(g_save_dir, comps, n, out, outsz, 0)) return 1;
    if (resolve_in(g_game_dir, comps, n, out, outsz, 0)) return 1;
    // not found: return a path in game dir anyway
    resolve_in(g_game_dir, comps, n, out, outsz, 0);
    return 0;
}

// ---------------------------------------------------------------- handles
#define MAXF 256
static struct { FILE *f; char path[512]; int used; } files[MAXF];
#define FH_BASE 0x1000u
#define STD_IN 0xF00u
#define STD_OUT 0xF04u
#define STD_ERR 0xF08u

static FILE *fh(uint32_t h) {
    if (h < FH_BASE) return NULL;
    uint32_t i = (h - FH_BASE) / 4;
    if (i >= MAXF || !files[i].used) return NULL;
    return files[i].f;
}

static void k_CreateFileA(void) {
    const char *name = GSTR(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    int write = (access & 0x40000000u) != 0;
    char path[1024];
    FILE *f = NULL;
    if (!write) {
        if (fs_resolve(name, path, sizeof path, 0)) f = fopen(path, "rb");
    } else {
        char rpath[1024];
        int exists = fs_resolve(name, rpath, sizeof rpath, 0);
        fs_resolve(name, path, sizeof path, 1);
        switch (disp) {
        case 1: if (exists) { last_error = 80; SRET(0xffffffffu, 7); } f = fopen(path, "w+b"); break;
        case 2: f = fopen(path, "w+b"); break;
        case 3: case 4: case 5:
            if (exists && strcmp(rpath, path)) {
                // copy-on-write from game dir into save dir
                FILE *src = fopen(rpath, "rb"), *dst = fopen(path, "wb");
                if (src && dst) { char b[8192]; size_t k; while ((k = fread(b, 1, sizeof b, src)) > 0) fwrite(b, 1, k, dst); }
                if (src) fclose(src);
                if (dst) fclose(dst);
            }
            if (disp == 5) f = fopen(path, "w+b");
            else { f = fopen(path, "r+b"); if (!f && disp == 4) f = fopen(path, "w+b"); }
            break;
        }
    }
    static int flog = -1;
    if (flog < 0) flog = getenv("PORT_FILELOG") != NULL;
    if (flog) host_log("CreateFileA(%s, %x, disp %d) -> %s%s\n", name, access, disp, f ? "" : "FAIL ", path);
    if (!f) { last_error = 2; SRET(0xffffffffu, 7); }
    for (int i = 0; i < MAXF; i++) if (!files[i].used) {
        files[i].used = 1; files[i].f = f; snprintf(files[i].path, sizeof files[i].path, "%s", path);
        SRET(FH_BASE + i * 4, 7);
    }
    fclose(f);
    last_error = 4; SRET(0xffffffffu, 7);
}

static void k_ReadFile(void) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pread = ARG(3);
    FILE *f = fh(h);
    if (!f) { if (pread) W32(pread, 0); last_error = 6; SRET(0, 5); }
    COW(buf, n);
    size_t got = fread(G + buf, 1, n, f);
    if (pread) W32(pread, (uint32_t)got);
    if (getenv("PORT_FILELOG")) host_log("ReadFile(%x, %u) -> %u\n", h, n, (unsigned)got);
    SRET(1, 5);
}

static void k_WriteFile(void) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3);
    if (h == STD_OUT || h == STD_ERR) {
        host_log("%.*s", (int)n, GSTR(buf));
        if (pw) W32(pw, n);
        SRET(1, 5);
    }
    FILE *f = fh(h);
    if (!f) { last_error = 6; SRET(0, 5); }
    size_t put = fwrite(G + buf, 1, n, f);
    if (pw) W32(pw, (uint32_t)put);
    g_fs_dirty = 1;
    SRET(1, 5);
}

static void k_SetFilePointer(void) {
    uint32_t h = ARG(0), mv = ARG(3);
    int32_t dist = (int32_t)ARG(1);
    FILE *f = fh(h);
    if (!f) { last_error = 6; SRET(0xffffffffu, 4); }
    if (ARG(2)) W32(ARG(2), 0);
    fseek(f, dist, mv == 0 ? SEEK_SET : mv == 1 ? SEEK_CUR : SEEK_END);
    SRET((uint32_t)ftell(f), 4);
}

static void k_SetEndOfFile(void) {
    FILE *f = fh(ARG(0));
    if (f) {
        fflush(f);
        long pos = ftell(f);
#ifdef _WIN32
        _chsize(_fileno(f), pos);
#else
        if (ftruncate(fileno(f), pos)) {}
#endif
    }
    SRET(1, 1);
}

static void k_CloseHandle(void) {
    uint32_t h = ARG(0);
    if (h >= FH_BASE) {
        uint32_t i = (h - FH_BASE) / 4;
        if (i < MAXF && files[i].used) { fclose(files[i].f); files[i].used = 0; }
    }
    SRET(1, 1);
}
static void k_FlushFileBuffers(void) { FILE *f = fh(ARG(0)); if (f) fflush(f); SRET(1, 1); }
static void k_GetFileType(void) { uint32_t h = ARG(0); SRET(h >= FH_BASE ? 1 : 2, 1); }

// The wall clock. Online play uses a fixed date advanced by the game clock, in UTC, so every client sees the same
// time (games put it in saves and strings, whose lengths then shape later allocations).
static time_t wall_time(void) { return net_mode != NET_OFF ? (time_t)(1104537600 + net_clock() / 1000) : time(NULL); }
static struct tm *wall_tm(const time_t *t) { return net_mode != NET_OFF ? gmtime(t) : localtime(t); }
static void put_filetime(uint32_t a, time_t t) {
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    W32(a, (uint32_t)ft); W32(a + 4, (uint32_t)(ft >> 32));
}
static void k_GetFileTime(void) {
    uint32_t h = ARG(0);
    time_t t = wall_time();
    uint32_t hi = (h - FH_BASE) / 4;
    if (h >= FH_BASE && hi < MAXF && files[hi].used) {
        struct stat st; if (net_mode == NET_OFF && !stat(files[hi].path, &st)) t = st.st_mtime;   // online: file ages differ per client
    }
    for (int i = 1; i <= 3; i++) if (ARG(i)) put_filetime(ARG(i), t);
    SRET(1, 4);
}
static void k_FileTimeToLocalFileTime(void) { W64(ARG(1), R64(ARG(0))); SRET(1, 2); }
static void put_systemtime(uint32_t a, struct tm *tm, int ms) {
    W16(a + 0, tm->tm_year + 1900); W16(a + 2, tm->tm_mon + 1); W16(a + 4, tm->tm_wday);
    W16(a + 6, tm->tm_mday); W16(a + 8, tm->tm_hour); W16(a + 10, tm->tm_min);
    W16(a + 12, tm->tm_sec); W16(a + 14, ms);
}
static void k_FileTimeToSystemTime(void) {
    uint64_t ft = R64(ARG(0));
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm *tm = gmtime(&t);
    if (!tm) SRET(0, 2);
    put_systemtime(ARG(1), tm, 0);
    SRET(1, 2);
}
static void k_GetLocalTime(void) { time_t t = wall_time(); put_systemtime(ARG(0), wall_tm(&t), 0); SRET(0, 1); }
static void k_GetSystemTime(void) { time_t t = wall_time(); put_systemtime(ARG(0), gmtime(&t), 0); SRET(0, 1); }
static void k_GetTimeZoneInformation(void) { (COW(ARG(0), (uint32_t)(172)), memset(G + ARG(0), 0, 172)); SRET(0, 1); }

static void k_GetFileAttributesA(void) {
    char path[1024];
    if (!fs_resolve(GSTR(ARG(0)), path, sizeof path, 0)) { last_error = 2; SRET(0xffffffffu, 1); }
    struct stat st;
    if (stat(path, &st)) { last_error = 2; SRET(0xffffffffu, 1); }
    SRET(S_ISDIR(st.st_mode) ? 0x10 : 0x80, 1);
}
static void k_DeleteFileA(void) {
    char path[1024];
    // only files in the save overlay can be deleted
    fs_resolve(GSTR(ARG(0)), path, sizeof path, 1);
    SRET(remove(path) == 0, 1);
}
static void k_MoveFileA(void) {
    char a[1024], b[1024], c[1024];
    int ex = fs_resolve(GSTR(ARG(0)), c, sizeof c, 0);
    fs_resolve(GSTR(ARG(0)), a, sizeof a, 1);
    fs_resolve(GSTR(ARG(1)), b, sizeof b, 1);
    if (ex && strcmp(a, c)) {
        FILE *src = fopen(c, "rb"), *dst = fopen(b, "wb");
        if (src && dst) { char bb[8192]; size_t k; while ((k = fread(bb, 1, sizeof bb, src)) > 0) fwrite(bb, 1, k, dst); }
        if (src) fclose(src);
        if (dst) fclose(dst);
        SRET(1, 2);
    }
    SRET(rename(a, b) == 0, 2);
}

static void k_GetCurrentDirectoryA(void) {
    const char *d = "C:\\GAME";
    uint32_t n = ARG(0), buf = ARG(1);
    if (n > strlen(d)) { COW(buf, (uint32_t)strlen(d) + 1); strcpy(GSTR(buf), d); }
    SRET(strlen(d), 2);
}
static void k_GetModuleFileNameA(void) {
    const char *d = "C:\\GAME\\" GAME_EXE;
    uint32_t buf = ARG(1), n = ARG(2);
    COW(buf, n); snprintf(GSTR(buf), n, "%s", d);
    SRET(strlen(GSTR(buf)), 3);
}

// modules
static char *modnames[64]; static int nmods;
static uint32_t mod_handle(const char *name) {
    for (int i = 0; i < nmods; i++) if (!strcasecmp(modnames[i], name)) return 0x7E000000u + i * 0x10000;
    if (nmods < 64) modnames[nmods] = strdup(name);
    return 0x7E000000u + (nmods++) * 0x10000;
}
static void k_GetModuleHandleA(void) {
    uint32_t p = ARG(0);
    if (!p) SRET(0x400000, 1);
    SRET(mod_handle(GSTR(p)), 1);
}
static void k_LoadLibraryA(void) { SRET(mod_handle(GSTR(ARG(0))), 1); }

extern GFunc shim_lookup(const char *dll, const char *name);
static void k_GetProcAddress(void) {
    uint32_t h = ARG(0), np = ARG(1);
    if (np < 0x10000) SRET(0, 2);
    const char *dll = "?";
    if (h >= 0x7E000000u && (h - 0x7E000000u) / 0x10000 < (uint32_t)nmods) dll = modnames[(h - 0x7E000000u) / 0x10000];
    const char *name = GSTR(np);
    GFunc fn = shim_lookup(dll, name);
    host_log("GetProcAddress(%s, %s) -> %s\n", dll, name, fn ? "ok" : "null");
    if (!fn) { last_error = 127; SRET(0, 2); }
    SRET(host_thunk(fn, name), 2);
}

static void k_GetVersion(void) { SRET(0x0A280105u, 0); }
static void k_GetVersionExA(void) {
    uint32_t p = ARG(0);
    uint32_t sz = R32(p);
    (COW(p + 4, (uint32_t)(sz > 4 ? sz - 4 : 0)), memset(G + p + 4, 0, sz > 4 ? sz - 4 : 0));
    W32(p + 4, 5); W32(p + 8, 1); W32(p + 12, 2600); W32(p + 16, 2);
    strcpy(GSTR(p + 20), "Service Pack 3");
    SRET(1, 1);
}
static uint32_t cmdline_ptr, env_ptr;
static void k_GetCommandLineA(void) {
    if (!cmdline_ptr) { cmdline_ptr = guest_static_alloc(64); strcpy(GSTR(cmdline_ptr), GAME_EXE); }
    SRET(cmdline_ptr, 0);
}
static void k_GetStartupInfoA(void) { uint32_t p = ARG(0); (COW(p, (uint32_t)(68)), memset(G + p, 0, 68)); W32(p, 68); SRET(0, 1); }
static void k_GetEnvironmentStrings(void) {
    if (!env_ptr) { env_ptr = guest_static_alloc(64); memcpy(GSTR(env_ptr), "PATH=C:\\\0\0", 11); }
    SRET(env_ptr, 0);
}
static void k_GetEnvironmentStringsW(void) { SRET(0, 0); }
static void k_FreeEnvironmentStrings(void) { SRET(1, 1); }
static void k_GetEnvironmentVariableA(void) { last_error = 203; SRET(0, 3); }
static void k_SetEnvironmentVariableA(void) { SRET(1, 2); }
static void k_GetStdHandle(void) {
    int32_t n = (int32_t)ARG(0);
    SRET(n == -10 ? STD_IN : n == -11 ? STD_OUT : STD_ERR, 1);
}
static void k_SetStdHandle(void) { SRET(1, 2); }
static void k_SetHandleCount(void) { SRET(ARG(0), 1); }

// heap
static void k_HeapCreate(void) { SRET(0x7F000001u, 3); }
static void k_HeapDestroy(void) { SRET(1, 1); }
static void k_HeapAlloc(void) {
    uint32_t flags = ARG(1), n = ARG(2);
    uint32_t p = heap_alloc(n ? n : 1);
    if (p && (flags & 8)) (COW(p, (uint32_t)(n)), memset(G + p, 0, n));
    SRET(p, 3);
}
static void k_HeapFree(void) { heap_free(ARG(2)); SRET(1, 3); }
static void k_HeapReAlloc(void) {
    uint32_t flags = ARG(1), p = ARG(2), n = ARG(3);
    uint32_t old = heap_size(p);
    if (n <= old) SRET(p, 4);
    if (flags & 0x10) SRET(0, 4);    // HEAP_REALLOC_IN_PLACE_ONLY
    uint32_t q = heap_realloc(p, n);
    if (q && (flags & 8)) (COW(q + old, (uint32_t)(n - old)), memset(G + q + old, 0, n - old));
    SRET(q, 4);
}
static void k_HeapSize(void) { SRET(heap_size(ARG(2)), 3); }

// VirtualAlloc: reservations are carved out of the guest heap, 64K aligned
#define MAXV 512
static struct { uint32_t base, size, block; } vregions[MAXV];
static void k_VirtualAlloc(void) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (addr) {
        for (int i = 0; i < MAXV; i++)
            if (vregions[i].size && addr >= vregions[i].base && addr < vregions[i].base + vregions[i].size)
                SRET(addr & ~0xfffu, 4);
        host_log("VirtualAlloc: commit outside reservation %08x\n", addr);
        SRET(0, 4);
    }
    size = (size + 0xfff) & ~0xfffu;
    uint32_t blk = heap_alloc(size + 0x10000);
    if (!blk) SRET(0, 4);
    uint32_t base = (blk + 0xffff) & ~0xffffu;
    (COW(base, (uint32_t)(size)), memset(G + base, 0, size));
    for (int i = 0; i < MAXV; i++) if (!vregions[i].size) {
        vregions[i].base = base; vregions[i].size = size; vregions[i].block = blk;
        (void)type;
        SRET(base, 4);
    }
    host_fatal("too many VirtualAlloc regions");
}
static void k_VirtualFree(void) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    for (int i = 0; i < MAXV; i++) {
        if (vregions[i].size && addr >= vregions[i].base && addr < vregions[i].base + vregions[i].size) {
            if (type & 0x8000) {   // MEM_RELEASE
                heap_free(vregions[i].block);
                vregions[i].size = 0;
            } else if (type & 0x4000) {  // MEM_DECOMMIT
                uint32_t end = addr + size;
                if (end > vregions[i].base + vregions[i].size) end = vregions[i].base + vregions[i].size;
                (COW(addr, (uint32_t)(end - addr)), memset(G + addr, 0, end - addr));
            }
            SRET(1, 3);
        }
    }
    SRET(0, 3);
}

static void k_GetLastError(void) { SRET(last_error, 0); }
static void k_ExitProcess(void) {
    host_log("ExitProcess(%d)\n", ARG(0));
    host_shutdown();
    exit((int)ARG(0));
}
static void k_TerminateProcess(void) { host_log("TerminateProcess\n"); host_shutdown(); exit((int)ARG(1)); }
static void k_GetCurrentProcess(void) { SRET(0xffffffffu, 0); }
static void k_GetACP(void) { SRET(1252, 0); }
static void k_GetOEMCP(void) { SRET(437, 0); }
static void k_GetCPInfo(void) {
    uint32_t p = ARG(1);
    (COW(p, (uint32_t)(20)), memset(G + p, 0, 20));
    W32(p, 1); W8(p + 4, '?');
    SRET(1, 2);
}
static void k_MultiByteToWideChar(void) {
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (cap == 0) SRET(n, 6);
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) W16(dst + i * 2, R8(src + i));
    SRET(k, 6);
}
static void k_WideCharToMultiByte(void) {
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) { n = 0; while (R16(src + n * 2)) n++; n++; }
    if (cap == 0) SRET(n, 8);
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) { uint32_t c = R16(src + i * 2); W8(dst + i, c < 256 ? c : '?'); }
    SRET(k, 8);
}
static void k_LCMapStringA(void) {
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (cap == 0) SRET(n, 6);
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) {
        int c = R8(src + i);
        if (flags & 0x100) c = tolower(c); else if (flags & 0x200) c = toupper(c);
        W8(dst + i, c);
    }
    SRET(k, 6);
}
static void k_ret0_6(void) { SRET(0, 6); }
static void k_GetStringTypeA(void) {
    uint32_t src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    for (int i = 0; i < n; i++) {
        int c = R8(src + i); uint32_t t = 0;
        if (c < 128) {
            if (isupper(c)) t |= 1; if (islower(c)) t |= 2; if (isdigit(c)) t |= 4; if (isspace(c)) t |= 8;
            if (ispunct(c)) t |= 0x10; if (iscntrl(c)) t |= 0x20; if (c == ' ' || c == '\t') t |= 0x40;
            if (isxdigit(c)) t |= 0x80; if (isalpha(c)) t |= 0x100;
        } else t = 0x100 | 0x2;
        W16(out + i * 2, t);
    }
    SRET(1, 5);
}
static void k_GetStringTypeW(void) { SRET(0, 4); }
static void k_CompareStringA(void) {
    uint32_t flags = ARG(1), s1 = ARG(2), s2 = ARG(4);
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    if (n1 < 0) n1 = (int32_t)strlen(GSTR(s1));
    if (n2 < 0) n2 = (int32_t)strlen(GSTR(s2));
    int n = n1 < n2 ? n1 : n2, r = 0;
    for (int i = 0; i < n && !r; i++) {
        int a = R8(s1 + i), b = R8(s2 + i);
        if (flags & 1) { a = tolower(a); b = tolower(b); }
        r = a - b;
    }
    if (!r) r = n1 - n2;
    SRET(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}
static void k_GetDateFormatA(void) {
    uint32_t buf = ARG(4); int32_t cap = (int32_t)ARG(5);
    time_t t = wall_time(); char s[64];
    strftime(s, sizeof s, "%Y-%m-%d", wall_tm(&t));
    if (cap == 0) SRET(strlen(s) + 1, 6);
    COW(buf, (uint32_t)cap); snprintf(GSTR(buf), cap, "%s", s);
    SRET(strlen(GSTR(buf)) + 1, 6);
}
static void k_GetTimeFormatA(void) {
    uint32_t buf = ARG(4); int32_t cap = (int32_t)ARG(5);
    time_t t = wall_time(); char s[64];
    strftime(s, sizeof s, "%H:%M:%S", wall_tm(&t));
    if (cap == 0) SRET(strlen(s) + 1, 6);
    COW(buf, (uint32_t)cap); snprintf(GSTR(buf), cap, "%s", s);
    SRET(strlen(GSTR(buf)) + 1, 6);
}

// ---------------------------------------------------------------- INI files
static int ini_get(const char *file, const char *sect, const char *key, char *out, int outsz) {
    char path[1024];
    if (!fs_resolve(file, path, sizeof path, 0)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[1024]; int insect = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
        if (*s == '[') {
            char *e = strchr(s, ']'); if (e) *e = 0;
            insect = !strcasecmp(s + 1, sect);
            continue;
        }
        if (!insect || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = s; size_t kl = strlen(k);
        while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = 0;
        if (strcasecmp(k, key)) continue;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        snprintf(out, outsz, "%s", v);
        found = 1; break;
    }
    fclose(f);
    return found;
}
static void k_GetPrivateProfileStringA(void) {
    uint32_t app = ARG(0), key = ARG(1), def = ARG(2), ret = ARG(3), size = ARG(4), file = ARG(5);
    char v[1024];
    if (!size) SRET(0, 6);
    if (!app || !key) { W8(ret, 0); W8(ret + 1, 0); SRET(0, 6); }
    if (!ini_get(GSTR(file), GSTR(app), GSTR(key), v, sizeof v))
        snprintf(v, sizeof v, "%s", def ? GSTR(def) : "");
    COW(ret, size); snprintf(GSTR(ret), size, "%s", v);
    SRET(strlen(GSTR(ret)), 6);
}
static void k_GetPrivateProfileIntA(void) {
    char v[256];
    if (ini_get(GSTR(ARG(3)), GSTR(ARG(0)), GSTR(ARG(1)), v, sizeof v)) SRET((uint32_t)strtol(v, NULL, 0), 4);
    SRET(ARG(2), 4);
}

void host_sleep(uint32_t ms);
static void k_Sleep(void) { host_sleep(ARG(0)); SRET(0, 1); }
static void k_MulDiv(void) {
    int64_t r = (int64_t)(int32_t)ARG(0) * (int32_t)ARG(1);
    int32_t d = (int32_t)ARG(2);
    if (!d) SRET(0xffffffffu, 3);
    SRET((uint32_t)(int32_t)(r / d), 3);
}
static void k_IsBad2(void) { SRET(0, 2); }
static void k_IsBad1(void) { SRET(0, 1); }
static void k_RaiseException(void) { host_fatal("RaiseException %08x", ARG(0)); }
static void k_RtlUnwind(void) { host_fatal("RtlUnwind"); }
static void k_SetUnhandledExceptionFilter(void) { SRET(0, 1); }
static void k_UnhandledExceptionFilter(void) { SRET(0, 1); }

// ---------------------------------------------------------------- more KERNEL32 (MSVC 2008 CRT, Uplink)
static void k_SetLastError(void) { last_error = ARG(0); SRET(0, 1); }
static void k_GetFileSize(void) {
    FILE *f = fh(ARG(0));
    if (!f) SRET(0xffffffffu, 2);
    long cur = ftell(f); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, cur, SEEK_SET);
    if (ARG(1)) W32(ARG(1), 0);
    SRET((uint32_t)n, 2);
}
static void k_CreateDirectoryA(void) {
    char path[1024];
    fs_resolve(GSTR(ARG(0)), path, sizeof path, 1);
    int r = mkdir_p(path);
    g_fs_dirty = 1;
    if (r) { last_error = 183; SRET(0, 2); }
    SRET(1, 2);
}
static void k_RemoveDirectoryA(void) {
    char path[1024];
    fs_resolve(GSTR(ARG(0)), path, sizeof path, 1);
    g_fs_dirty = 1;
#ifdef _WIN32
    SRET(_rmdir(path) == 0, 1);
#else
    SRET(rmdir(path) == 0, 1);
#endif
}

// FindFirstFile/FindNextFile over the save overlay and the game directory (names merged, overlay first)
static int wild_match(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') { for (;; s++) { if (wild_match(pat + 1, s)) return 1; if (!*s) return 0; } }
    if (!*s) return 0;
    if (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*s)) return wild_match(pat + 1, s + 1);
    return 0;
}
#define MAXFIND 16
static struct { int used, n, pos; char (*names)[260]; uint32_t *attr, *size; } finds[MAXFIND];
static void find_fill(uint32_t fd, int i) {
    int k = finds[i].pos;
    COW(fd, 320); memset(G + fd, 0, 320);
    W32(fd, finds[i].attr[k]);
    time_t t = time(NULL);
    for (int j = 0; j < 3; j++) put_filetime(fd + 4 + 8 * j, t);
    W32(fd + 32, finds[i].size[k]);
    snprintf(GSTR(fd + 44), 260, "%s", finds[i].names[k]);
}
static void find_scan(int i, const char *root, char comps[][128], int nc, const char *pat) {
    char dir[1024];
    if (!resolve_in(root, comps, nc, dir, sizeof dir, 0)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!wild_match(pat, e->d_name)) continue;
        int dup = 0;
        for (int k = 0; k < finds[i].n; k++) if (!strcasecmp(finds[i].names[k], e->d_name)) dup = 1;
        if (dup || finds[i].n >= 1024) continue;
        char full[1300]; snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat stt;
        if (stat(full, &stt)) continue;
        snprintf(finds[i].names[finds[i].n], 260, "%s", e->d_name);
        finds[i].attr[finds[i].n] = S_ISDIR(stt.st_mode) ? 0x10 : 0x80;
        finds[i].size[finds[i].n] = (uint32_t)stt.st_size;
        finds[i].n++;
    }
    closedir(d);
}
static void k_FindFirstFileA(void) {
    const char *gp = GSTR(ARG(0));
    uint32_t fd = ARG(1);
    char comps[32][128];
    int nc = split_path(gp, comps, 32);
    if (nc == 0) { last_error = 2; SRET(0xffffffffu, 2); }
    char pat[128]; snprintf(pat, sizeof pat, "%s", comps[nc - 1]);
    int i = 0;
    while (i < MAXFIND && finds[i].used) i++;
    if (i == MAXFIND) { last_error = 4; SRET(0xffffffffu, 2); }
    finds[i].used = 1; finds[i].n = finds[i].pos = 0;
    finds[i].names = malloc(1024 * 260); finds[i].attr = malloc(1024 * 4); finds[i].size = malloc(1024 * 4);
    find_scan(i, g_save_dir, comps, nc - 1, pat);
    find_scan(i, g_game_dir, comps, nc - 1, pat);
    if (!finds[i].n) {
        free(finds[i].names); free(finds[i].attr); free(finds[i].size); finds[i].used = 0;
        last_error = 2; SRET(0xffffffffu, 2);
    }
    find_fill(fd, i);
    SRET(0x2000u + i, 2);
}
static void k_FindNextFileA(void) {
    uint32_t i = ARG(0) - 0x2000u;
    if (i >= MAXFIND || !finds[i].used) SRET(0, 2);
    if (++finds[i].pos >= finds[i].n) { last_error = 18; SRET(0, 2); }
    find_fill(ARG(1), i);
    SRET(1, 2);
}
static void k_FindClose(void) {
    uint32_t i = ARG(0) - 0x2000u;
    if (i < MAXFIND && finds[i].used) { free(finds[i].names); free(finds[i].attr); free(finds[i].size); finds[i].used = 0; }
    SRET(1, 1);
}

// file mappings: the data is copied into the guest heap when the mapping is created
static struct { uint32_t mem, size; } maps[32];
static void k_CreateFileMappingA(void) {
    FILE *f = fh(ARG(0));
    if (!f) SRET(0, 6);
    long cur = ftell(f); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    for (int i = 0; i < 32; i++) if (!maps[i].mem) {
        maps[i].mem = heap_alloc((uint32_t)n + 1); maps[i].size = (uint32_t)n;
        COW(maps[i].mem, (uint32_t)n);
        if (fread(G + maps[i].mem, 1, n, f) != (size_t)n) host_log("CreateFileMapping: short read\n");
        fseek(f, cur, SEEK_SET);
        SRET(0x3000u + i, 6);
    }
    SRET(0, 6);
}
static void k_MapViewOfFile(void) {
    uint32_t i = ARG(0) - 0x3000u;
    if (i >= 32 || !maps[i].mem) SRET(0, 5);
    SRET(maps[i].mem + ARG(3), 5);
}
static void k_UnmapViewOfFile(void) { SRET(1, 1); }

static void k_ret0_0(void) { SRET(0, 0); }
static void k_ret1_1(void) { SRET(1, 1); }
static void k_ret0_1(void) { SRET(0, 1); }
static void k_ret0_2(void) { SRET(0, 2); }
static void k_ret1_2(void) { SRET(1, 2); }
static void k_ret0_4(void) { SRET(0, 4); }
static void k_ret0_5(void) { SRET(0, 5); }
static void k_InterlockedIncrement(void) { uint32_t p = ARG(0), v = R32(p) + 1; W32(p, v); SRET(v, 1); }
static void k_InterlockedDecrement(void) { uint32_t p = ARG(0), v = R32(p) - 1; W32(p, v); SRET(v, 1); }
static void k_InterlockedExchange(void) { uint32_t p = ARG(0), old = R32(p); W32(p, ARG(1)); SRET(old, 2); }
static uint32_t tls[64]; static int tls_used[64];
static void k_TlsAlloc(void) { for (int i = 0; i < 64; i++) if (!tls_used[i]) { tls_used[i] = 1; tls[i] = 0; SRET(i, 0); } SRET(0xffffffffu, 0); }
static void k_TlsFree(void) { if (ARG(0) < 64) tls_used[ARG(0)] = 0; SRET(1, 1); }
static void k_TlsGetValue(void) { last_error = 0; SRET(ARG(0) < 64 ? tls[ARG(0)] : 0, 1); }
static void k_TlsSetValue(void) { if (ARG(0) < 64) tls[ARG(0)] = ARG(1); SRET(1, 2); }
static void k_GetTickCount(void) { SRET(net_mode != NET_OFF ? net_clock() : host_ticks(), 0); }
extern uint64_t host_time_ns(void);
static void k_QueryPerformanceCounter(void) {
    uint64_t t = net_mode != NET_OFF ? (uint64_t)net_clock() * 1000000ull : host_time_ns();
    W64(ARG(0), t / 1000);          // microseconds
    SRET(1, 1);
}
static void k_QueryPerformanceFrequency(void) { W64(ARG(0), 1000000); SRET(1, 1); }
static void k_GetSystemTimeAsFileTime(void) { put_filetime(ARG(0), wall_time()); SRET(0, 1); }
static void k_FileTimeToDosDateTime(void) {
    uint64_t ft = R64(ARG(0));
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm *tm = gmtime(&t);
    if (!tm) SRET(0, 3);
    if (ARG(1)) W16(ARG(1), ((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
    if (ARG(2)) W16(ARG(2), (tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
    SRET(1, 3);
}
static void k_GetSystemInfo(void) {
    uint32_t p = ARG(0);
    COW(p, 36); memset(G + p, 0, 36);
    W32(p + 4, 4096); W32(p + 8, 0x10000); W32(p + 12, GUEST_SIZE - 1); W32(p + 16, 1); W32(p + 20, 1);
    W32(p + 24, 586); W32(p + 28, 0x10000); W16(p + 32, 6); W16(p + 34, 0x0603);
    SRET(0, 1);
}
static void k_GlobalMemoryStatus(void) {
    uint32_t p = ARG(0);
    W32(p, 32); W32(p + 4, 10);
    W32(p + 8, 512u << 20); W32(p + 12, 256u << 20); W32(p + 16, 1024u << 20); W32(p + 20, 768u << 20);
    W32(p + 24, 0x7ffe0000u); W32(p + 28, 0x60000000u);
    SRET(0, 1);
}
static void k_GetComputerNameA(void) {
    uint32_t buf = ARG(0), psz = ARG(1);
    const char *nm = "UPLINK";
    COW(buf, 7); strcpy(GSTR(buf), nm); W32(psz, (uint32_t)strlen(nm));
    SRET(1, 2);
}
static void k_GlobalAlloc(void) {
    uint32_t p = heap_alloc(ARG(1) ? ARG(1) : 1);
    if (p && (ARG(0) & 0x40)) { COW(p, ARG(1)); memset(G + p, 0, ARG(1)); }
    SRET(p, 2);
}
static void k_GlobalFree(void) { heap_free(ARG(0)); SRET(0, 1); }
static void k_GlobalSize(void) { SRET(heap_size(ARG(0)), 1); }
static void k_VirtualQuery(void) {
    uint32_t a = ARG(0), p = ARG(1);
    COW(p, 28); memset(G + p, 0, 28);
    W32(p, a & ~0xfffu); W32(p + 4, a & ~0xffffu); W32(p + 8, 4); W32(p + 12, 0x10000); W32(p + 16, 0x1000); W32(p + 20, 4); W32(p + 24, 0x20000);
    SRET(28, 3);
}
static void k_OutputDebugStringA(void) { host_log("[debug] %s", GSTR(ARG(0))); SRET(0, 1); }
static void k_WriteConsoleA(void) {
    host_log("%.*s", (int)ARG(2), GSTR(ARG(1)));
    if (ARG(3)) W32(ARG(3), ARG(2));
    SRET(1, 5);
}
static void k_WriteConsoleW(void) {
    for (uint32_t i = 0; i < ARG(2); i++) { uint32_t c = R16(ARG(1) + 2 * i); host_log("%c", c < 128 ? (int)c : '?'); }
    if (ARG(3)) W32(ARG(3), ARG(2));
    SRET(1, 5);
}
static void k_GetConsoleCP(void) { SRET(437, 0); }
static void k_GetModuleFileNameW(void) {
    const char *d = "C:\\GAME\\" GAME_EXE;
    uint32_t buf = ARG(1), n = ARG(2), i = 0;
    for (; d[i] && i + 1 < n; i++) W16(buf + 2 * i, (uint8_t)d[i]);
    if (n) W16(buf + 2 * i, 0);
    SRET(i, 3);
}
static void k_GetModuleHandleW(void) {
    uint32_t p = ARG(0);
    if (!p) SRET(0x400000, 1);
    char nm[128]; int i = 0;
    for (; i < 127 && R16(p + 2 * i); i++) nm[i] = (char)R16(p + 2 * i);
    nm[i] = 0;
    if (!strncasecmp(nm, "kernel32", 8) || !strncasecmp(nm, "user32", 6)) SRET(mod_handle(nm), 1);
    SRET(0, 1);     // mscoree.dll and the like: not loaded
}
// the C++ library opens streams with _wfsopen: convert the name and do CreateFileA's work
static void k_CreateFileW(void) {
    uint32_t w = ARG(0), n = 0;
    while (n < 1023 && R16(w + 2 * n)) n++;
    uint32_t a = heap_alloc(n + 1);
    for (uint32_t i = 0; i < n; i++) { uint32_t c = R16(w + 2 * i); W8(a + i, c < 256 ? c : '_'); }
    W8(a + n, 0);
    W32(R.esp + 4, a);
    k_CreateFileA();      // pops the 7 arguments, same signature
    heap_free(a);
}
static void k_GetCurrentProcessId(void) { SRET(1234, 0); }
static void k_GetCurrentThreadId(void) { SRET(1, 0); }
static void k_GetProcessHeap(void) { SRET(0x7F000001u, 0); }
static void k_GetUserDefaultLCID(void) { SRET(0x409, 0); }
static void k_CreateThread(void) { host_log("CreateThread(start %08x) refused\n", ARG(2)); last_error = 8; SRET(0, 6); }
static void k_CreateProcessA(void) { host_log("CreateProcessA refused\n"); SRET(0, 10); }
static void k_lstrlenA(void) { SRET(ARG(0) ? strlen(GSTR(ARG(0))) : 0, 1); }
static void k_lstrcpyA(void) { uint32_t n = (uint32_t)strlen(GSTR(ARG(1))) + 1; COW(ARG(0), n); memmove(G + ARG(0), G + ARG(1), n); SRET(ARG(0), 2); }
static void k_lstrcatA(void) {
    uint32_t d = ARG(0) + (uint32_t)strlen(GSTR(ARG(0))), n = (uint32_t)strlen(GSTR(ARG(1))) + 1;
    COW(d, n); memmove(G + d, G + ARG(1), n); SRET(ARG(0), 2);
}
static void k_lstrcpynA(void) {
    uint32_t d = ARG(0), s = ARG(1); int n = (int)ARG(2);
    if (n <= 0) SRET(d, 3);
    COW(d, n);
    int i = 0;
    for (; i < n - 1 && G[s + i]; i++) G[d + i] = G[s + i];
    G[d + i] = 0;
    SRET(d, 3);
}

const ShimDef shims_kernel32[] = {
    {"KERNEL32", "SetLastError", k_SetLastError}, {"KERNEL32", "GetFileSize", k_GetFileSize},
    {"KERNEL32", "CreateDirectoryA", k_CreateDirectoryA}, {"KERNEL32", "RemoveDirectoryA", k_RemoveDirectoryA},
    {"KERNEL32", "FindFirstFileA", k_FindFirstFileA}, {"KERNEL32", "FindNextFileA", k_FindNextFileA},
    {"KERNEL32", "FindClose", k_FindClose}, {"KERNEL32", "CreateFileMappingA", k_CreateFileMappingA},
    {"KERNEL32", "MapViewOfFile", k_MapViewOfFile}, {"KERNEL32", "UnmapViewOfFile", k_UnmapViewOfFile},
    {"KERNEL32", "InitializeCriticalSection", k_ret0_1}, {"KERNEL32", "InitializeCriticalSectionAndSpinCount", k_ret1_2},
    {"KERNEL32", "DeleteCriticalSection", k_ret0_1}, {"KERNEL32", "EnterCriticalSection", k_ret0_1},
    {"KERNEL32", "LeaveCriticalSection", k_ret0_1}, {"KERNEL32", "InterlockedIncrement", k_InterlockedIncrement},
    {"KERNEL32", "InterlockedDecrement", k_InterlockedDecrement}, {"KERNEL32", "InterlockedExchange", k_InterlockedExchange},
    {"KERNEL32", "TlsAlloc", k_TlsAlloc}, {"KERNEL32", "TlsFree", k_TlsFree},
    {"KERNEL32", "TlsGetValue", k_TlsGetValue}, {"KERNEL32", "TlsSetValue", k_TlsSetValue},
    {"KERNEL32", "GetTickCount", k_GetTickCount}, {"KERNEL32", "QueryPerformanceCounter", k_QueryPerformanceCounter},
    {"KERNEL32", "QueryPerformanceFrequency", k_QueryPerformanceFrequency},
    {"KERNEL32", "GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime}, {"KERNEL32", "FileTimeToDosDateTime", k_FileTimeToDosDateTime},
    {"KERNEL32", "GetSystemInfo", k_GetSystemInfo}, {"KERNEL32", "GlobalMemoryStatus", k_GlobalMemoryStatus},
    {"KERNEL32", "GetComputerNameA", k_GetComputerNameA}, {"KERNEL32", "GlobalAlloc", k_GlobalAlloc},
    {"KERNEL32", "GlobalFree", k_GlobalFree}, {"KERNEL32", "GlobalSize", k_GlobalSize},
    {"KERNEL32", "VirtualQuery", k_VirtualQuery}, {"KERNEL32", "OutputDebugStringA", k_OutputDebugStringA},
    {"KERNEL32", "WriteConsoleA", k_WriteConsoleA}, {"KERNEL32", "WriteConsoleW", k_WriteConsoleW},
    {"KERNEL32", "GetConsoleCP", k_GetConsoleCP}, {"KERNEL32", "GetConsoleOutputCP", k_GetConsoleCP},
    {"KERNEL32", "GetConsoleMode", k_ret0_2}, {"KERNEL32", "GetModuleFileNameW", k_GetModuleFileNameW},
    {"KERNEL32", "GetModuleHandleW", k_GetModuleHandleW}, {"KERNEL32", "LoadLibraryW", k_ret0_1},
    {"KERNEL32", "FreeLibrary", k_ret1_1}, {"KERNEL32", "CreateFileW", k_CreateFileW},
    {"KERNEL32", "GetCurrentProcessId", k_GetCurrentProcessId}, {"KERNEL32", "GetCurrentThreadId", k_GetCurrentThreadId},
    {"KERNEL32", "GetProcessHeap", k_GetProcessHeap}, {"KERNEL32", "GetUserDefaultLCID", k_GetUserDefaultLCID},
    {"KERNEL32", "GetLocaleInfoA", k_ret0_4}, {"KERNEL32", "GetLocaleInfoW", k_ret0_4},
    {"KERNEL32", "IsValidCodePage", k_ret1_1}, {"KERNEL32", "IsValidLocale", k_ret1_2},
    {"KERNEL32", "EnumSystemLocalesA", k_ret1_2}, {"KERNEL32", "IsDebuggerPresent", k_ret0_0},
    {"KERNEL32", "CreateThread", k_CreateThread}, {"KERNEL32", "CreateProcessA", k_CreateProcessA},
    {"KERNEL32", "WaitForSingleObject", k_ret0_2}, {"KERNEL32", "TerminateThread", k_ret1_2},
    {"KERNEL32", "lstrlenA", k_lstrlenA}, {"KERNEL32", "lstrcpyA", k_lstrcpyA},
    {"KERNEL32", "lstrcatA", k_lstrcatA}, {"KERNEL32", "lstrcpynA", k_lstrcpynA},
    {"KERNEL32", "GetStringTypeExA", k_ret0_5},
    {"KERNEL32", "CreateFileA", k_CreateFileA}, {"KERNEL32", "ReadFile", k_ReadFile},
    {"KERNEL32", "WriteFile", k_WriteFile}, {"KERNEL32", "SetFilePointer", k_SetFilePointer},
    {"KERNEL32", "SetEndOfFile", k_SetEndOfFile}, {"KERNEL32", "CloseHandle", k_CloseHandle},
    {"KERNEL32", "FlushFileBuffers", k_FlushFileBuffers}, {"KERNEL32", "GetFileType", k_GetFileType},
    {"KERNEL32", "GetFileTime", k_GetFileTime}, {"KERNEL32", "FileTimeToLocalFileTime", k_FileTimeToLocalFileTime},
    {"KERNEL32", "FileTimeToSystemTime", k_FileTimeToSystemTime}, {"KERNEL32", "GetLocalTime", k_GetLocalTime},
    {"KERNEL32", "GetSystemTime", k_GetSystemTime}, {"KERNEL32", "GetTimeZoneInformation", k_GetTimeZoneInformation},
    {"KERNEL32", "GetFileAttributesA", k_GetFileAttributesA}, {"KERNEL32", "DeleteFileA", k_DeleteFileA},
    {"KERNEL32", "MoveFileA", k_MoveFileA}, {"KERNEL32", "GetCurrentDirectoryA", k_GetCurrentDirectoryA},
    {"KERNEL32", "GetModuleFileNameA", k_GetModuleFileNameA}, {"KERNEL32", "GetModuleHandleA", k_GetModuleHandleA},
    {"KERNEL32", "LoadLibraryA", k_LoadLibraryA}, {"KERNEL32", "GetProcAddress", k_GetProcAddress},
    {"KERNEL32", "GetVersion", k_GetVersion}, {"KERNEL32", "GetVersionExA", k_GetVersionExA},
    {"KERNEL32", "GetCommandLineA", k_GetCommandLineA}, {"KERNEL32", "GetStartupInfoA", k_GetStartupInfoA},
    {"KERNEL32", "GetEnvironmentStrings", k_GetEnvironmentStrings}, {"KERNEL32", "GetEnvironmentStringsW", k_GetEnvironmentStringsW},
    {"KERNEL32", "FreeEnvironmentStringsA", k_FreeEnvironmentStrings}, {"KERNEL32", "FreeEnvironmentStringsW", k_FreeEnvironmentStrings},
    {"KERNEL32", "GetEnvironmentVariableA", k_GetEnvironmentVariableA}, {"KERNEL32", "SetEnvironmentVariableA", k_SetEnvironmentVariableA},
    {"KERNEL32", "GetStdHandle", k_GetStdHandle}, {"KERNEL32", "SetStdHandle", k_SetStdHandle},
    {"KERNEL32", "SetHandleCount", k_SetHandleCount}, {"KERNEL32", "HeapCreate", k_HeapCreate},
    {"KERNEL32", "HeapDestroy", k_HeapDestroy}, {"KERNEL32", "HeapAlloc", k_HeapAlloc},
    {"KERNEL32", "HeapFree", k_HeapFree}, {"KERNEL32", "HeapReAlloc", k_HeapReAlloc},
    {"KERNEL32", "HeapSize", k_HeapSize}, {"KERNEL32", "VirtualAlloc", k_VirtualAlloc},
    {"KERNEL32", "VirtualFree", k_VirtualFree}, {"KERNEL32", "GetLastError", k_GetLastError},
    {"KERNEL32", "ExitProcess", k_ExitProcess}, {"KERNEL32", "TerminateProcess", k_TerminateProcess},
    {"KERNEL32", "GetCurrentProcess", k_GetCurrentProcess}, {"KERNEL32", "GetACP", k_GetACP},
    {"KERNEL32", "GetOEMCP", k_GetOEMCP}, {"KERNEL32", "GetCPInfo", k_GetCPInfo},
    {"KERNEL32", "MultiByteToWideChar", k_MultiByteToWideChar}, {"KERNEL32", "WideCharToMultiByte", k_WideCharToMultiByte},
    {"KERNEL32", "LCMapStringA", k_LCMapStringA}, {"KERNEL32", "LCMapStringW", k_ret0_6},
    {"KERNEL32", "GetStringTypeA", k_GetStringTypeA}, {"KERNEL32", "GetStringTypeW", k_GetStringTypeW},
    {"KERNEL32", "CompareStringA", k_CompareStringA}, {"KERNEL32", "CompareStringW", k_ret0_6},
    {"KERNEL32", "GetDateFormatA", k_GetDateFormatA}, {"KERNEL32", "GetTimeFormatA", k_GetTimeFormatA},
    {"KERNEL32", "GetPrivateProfileStringA", k_GetPrivateProfileStringA}, {"KERNEL32", "GetPrivateProfileIntA", k_GetPrivateProfileIntA},
    {"KERNEL32", "Sleep", k_Sleep}, {"KERNEL32", "MulDiv", k_MulDiv},
    {"KERNEL32", "IsBadReadPtr", k_IsBad2}, {"KERNEL32", "IsBadWritePtr", k_IsBad2}, {"KERNEL32", "IsBadCodePtr", k_IsBad1},
    {"KERNEL32", "RaiseException", k_RaiseException}, {"KERNEL32", "RtlUnwind", k_RtlUnwind},
    {"KERNEL32", "SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter},
    {"KERNEL32", "UnhandledExceptionFilter", k_UnhandledExceptionFilter},
    {0, 0, 0}
};
