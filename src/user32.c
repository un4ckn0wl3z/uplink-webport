// USER32 / ADVAPI32 / VERSION / dbghelp / network shims. The game's window comes from SDL (sdl12.c); USER32 only
// provides wsprintf and MessageBox here. The network DLLs back Uplink's optional client/server mode, which the
// port does not offer: every call fails cleanly.
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

// Win32 wsprintf subset: %[-#0][width][.prec][l|h](d i u x X c s %), arguments read from guest memory
static int guest_vsprintf(char *out, int cap, const char *fmt, uint32_t ap) {
    int n = 0;
#define PUT(ch) do { if (n < cap - 1) out[n] = (ch); n++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        p++;
        char spec[32]; int sl = 0;
        spec[sl++] = '%';
        while (*p == '-' || *p == '#' || *p == '0' || *p == ' ' || *p == '+') { if (sl < 20) spec[sl++] = *p; p++; }
        while (isdigit((unsigned char)*p)) { if (sl < 24) spec[sl++] = *p; p++; }
        if (*p == '.') { spec[sl++] = *p++; while (isdigit((unsigned char)*p)) { if (sl < 28) spec[sl++] = *p; p++; } }
        while (*p == 'l' || *p == 'h') p++;
        char c = *p;
        if (!c) break;
        char tmp[512];
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'c':
            spec[sl++] = c; spec[sl] = 0;
            snprintf(tmp, sizeof tmp, spec, (int)R32(ap)); ap += 4;
            break;
        case 's': {
            spec[sl++] = 's'; spec[sl] = 0;
            uint32_t s = R32(ap); ap += 4;
            snprintf(tmp, sizeof tmp, spec, s ? GSTR(s) : "(null)");
            break;
        }
        case '%': strcpy(tmp, "%"); break;
        default: tmp[0] = c; tmp[1] = 0;
        }
        for (char *t = tmp; *t; t++) PUT(*t);
    }
    if (cap > 0) out[n < cap ? n : cap - 1] = 0;
    return n < cap ? n : cap - 1;
#undef PUT
}
// cdecl: int wsprintfA(LPSTR buf, LPCSTR fmt, ...)
static void u_wsprintfA(void) {
    uint32_t buf = ARG(0), fmt = ARG(1);
    char tmp[1025];
    int n = guest_vsprintf(tmp, sizeof tmp, GSTR(fmt), R.esp + 12);
    COW(buf, n + 1); memcpy(G + buf, tmp, n + 1);
    R.eax = n; R.esp += 4;
}
static void u_wvsprintfA(void) {
    uint32_t buf = ARG(0), fmt = ARG(1), ap = ARG(2);
    char tmp[1025];
    int n = guest_vsprintf(tmp, sizeof tmp, GSTR(fmt), ap);
    COW(buf, n + 1); memcpy(G + buf, tmp, n + 1);
    SRET(n, 3);
}
static void u_MessageBoxA(void) {
    host_log("MessageBox: %s: %s\n", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : "");
    SRET(1, 4);   // IDOK
}
static void a_GetUserNameA(void) {
    uint32_t buf = ARG(0), psz = ARG(1);
    const char *nm = "Agent";
    if (psz && R32(psz) > strlen(nm)) { COW(buf, 6); strcpy(GSTR(buf), nm); W32(psz, (uint32_t)strlen(nm) + 1); SRET(1, 2); }
    SRET(0, 2);
}
static void v_GetFileVersionInfoSizeA(void) { SRET(0, 2); }
static void v_GetFileVersionInfoA(void) { SRET(0, 4); }
static void v_VerQueryValueA(void) { SRET(0, 4); }
static void d_MiniDumpWriteDump(void) { SRET(0, 7); }

// WSOCK32: everything fails with WSAENETDOWN
static void w_ret_err1(void) { SRET(0xffffffffu, 1); }
static void w_ret_err2(void) { SRET(0xffffffffu, 2); }
static void w_ret_err3(void) { SRET(0xffffffffu, 3); }
static void w_ret_err4(void) { SRET(0xffffffffu, 4); }
static void w_WSAStartup(void) { SRET(10050, 2); }
static void w_WSACleanup(void) { SRET(0, 0); }
static void w_htons(void) { uint32_t v = ARG(0) & 0xffff; SRET(((v >> 8) | (v << 8)) & 0xffff, 1); }
static void w_htonl(void) { uint32_t v = ARG(0); SRET((v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24), 1); }
static void w_gethostbyname(void) { SRET(0, 1); }
static void w_inet_addr(void) { SRET(0xffffffffu, 1); }
// TCP4W32 (by ordinal, stdcall): Tcp4uInit/Cleanup succeed, sockets fail
static void t_ok0(void) { SRET(1, 0); }
static void t_err1(void) { SRET(0xffffffffu, 1); }
static void t_err3(void) { SRET(0xffffffffu, 3); }
static void t_err4(void) { SRET(0xffffffffu, 4); }
static void t_err5(void) { SRET(0xffffffffu, 5); }
static void t_err8(void) { SRET(0xffffffffu, 8); }

const ShimDef shims_user32[] = {
    {"USER32", "wsprintfA", u_wsprintfA}, {"USER32", "wvsprintfA", u_wvsprintfA}, {"USER32", "MessageBoxA", u_MessageBoxA},
    {"ADVAPI32", "GetUserNameA", a_GetUserNameA},
    {"VERSION", "GetFileVersionInfoSizeA", v_GetFileVersionInfoSizeA}, {"VERSION", "GetFileVersionInfoA", v_GetFileVersionInfoA},
    {"VERSION", "VerQueryValueA", v_VerQueryValueA},
    {"dbghelp", "MiniDumpWriteDump", d_MiniDumpWriteDump},
    {"WSOCK32", "WSAStartup", w_WSAStartup}, {"WSOCK32", "WSACleanup", w_WSACleanup},
    {"WSOCK32", "socket", w_ret_err3}, {"WSOCK32", "bind", w_ret_err3}, {"WSOCK32", "listen", w_ret_err2},
    {"WSOCK32", "accept", w_ret_err3}, {"WSOCK32", "connect", w_ret_err3}, {"WSOCK32", "send", w_ret_err4},
    {"WSOCK32", "recv", w_ret_err4}, {"WSOCK32", "shutdown", w_ret_err2}, {"WSOCK32", "closesocket", w_ret_err1},
    {"WSOCK32", "htons", w_htons}, {"WSOCK32", "htonl", w_htonl}, {"WSOCK32", "gethostbyname", w_gethostbyname},
    {"WSOCK32", "inet_addr", w_inet_addr},
    {"TCP4W32", "#1", t_ok0}, {"TCP4W32", "#2", t_ok0}, {"TCP4W32", "#100", t_err4}, {"TCP4W32", "#101", t_err4},
    {"TCP4W32", "#102", t_err4}, {"TCP4W32", "#120", t_err5}, {"TCP4W32", "#130", t_err1}, {"TCP4W32", "#140", t_err3},
    {"TCP4W32", "#141", t_err4}, {"TCP4W32", "#250", t_err8},
    {0, 0, 0}
};
