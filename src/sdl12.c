// SDL 1.2 (SDL.dll) shims on SDL2: window + GL context, events in SDL 1.2 layout, timers, RWops, surfaces.
// Guest structures are the 32-bit SDL 1.2 layouts (SDL_Event 20 bytes, SDL_Surface 60, SDL_PixelFormat 40).
#include "host.h"
#include "netplay.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

SDL_Window *g_window;
int g_win_w = 1024, g_win_h = 768;      // the game's video mode (logical size)
uint32_t g_hwnd = 0x00010010u, g_wndproc;

void gl1_window_created(SDL_Window *w, int gw, int gh);
void gl1_present(void);
void gl1_map_mouse(int wx, int wy, int *gx, int *gy);
uint32_t host_frame_count(void);

static uint32_t err_str;
static void s_GetError(void) {
    if (!err_str) { err_str = guest_static_alloc(64); strcpy(GSTR(err_str), ""); }
    R.eax = err_str; R.esp += 4;
}
// cdecl: callers pop the arguments, so only the return address goes
#define CRET(v) do { R.eax = (uint32_t)(v); R.esp += 4; return; } while (0)
#define CARG(n) R32(R.esp + 4 + 4 * (n))

static void s_Init(void) { host_log("SDL_Init(%x)\n", CARG(0)); CRET(0); }
static void s_WasInit(void) { CRET(CARG(0)); }
static void s_QuitSubSystem(void) { CRET(0); }
static int key_repeat;
static void s_EnableKeyRepeat(void) { key_repeat = CARG(0) != 0; CRET(0); }
static int unicode_on;
static void s_EnableUNICODE(void) { int old = unicode_on; if ((int32_t)CARG(0) >= 0) unicode_on = CARG(0); CRET(old); }
static void s_WM_SetCaption(void) {
    if (g_window && CARG(0)) SDL_SetWindowTitle(g_window, GSTR(CARG(0)));
    CRET(0);
}
static void s_GetAppState(void) { CRET(7); }

// ---------------------------------------------------------------- video
static uint32_t video_info, video_fmt, screen_surf;
static uint32_t make_format(int bpp, uint32_t rm, uint32_t gm, uint32_t bm, uint32_t am) {
    uint32_t f = guest_static_alloc(40);
    W8(f + 4, bpp); W8(f + 5, (bpp + 7) / 8);
    uint32_t m[4] = {rm, gm, bm, am};
    for (int i = 0; i < 4; i++) {
        int sh = 0, bits = 0;
        if (m[i]) { while (!(m[i] >> sh & 1)) sh++; while (m[i] >> (sh + bits) & 1) bits++; }
        W8(f + 6 + i, 8 - bits); W8(f + 10 + i, sh); W32(f + 16 + 4 * i, m[i]);
    }
    W8(f + 36, 255);
    return f;
}
static void s_GetVideoInfo(void) {
    if (!video_info) {
        video_info = guest_static_alloc(20);
        video_fmt = make_format(32, 0xff0000, 0xff00, 0xff, 0);
        int w = 1024, h = 768;
#ifndef __EMSCRIPTEN__
        SDL_DisplayMode dm;
        if (!SDL_GetDesktopDisplayMode(0, &dm)) { w = dm.w; h = dm.h; }
#endif
        W32(video_info, 0x3); W32(video_info + 4, 65536); W32(video_info + 8, video_fmt);
        W32(video_info + 12, w); W32(video_info + 16, h);
    }
    CRET(video_info);
}
// a fixed list, largest first like SDL 1.2 (the game walks it; it does not accept -1 for "any size")
static void s_ListModes(void) {
    static uint32_t list;
    static const uint16_t modes[][2] = {{1920, 1080}, {1680, 1050}, {1600, 1200}, {1600, 900}, {1440, 900},
        {1366, 768}, {1280, 1024}, {1280, 960}, {1280, 800}, {1280, 720}, {1152, 864}, {1024, 768}, {800, 600}, {640, 480}};
    if (!list) {
        int n = (int)(sizeof modes / sizeof *modes);
        list = guest_static_alloc(4 * (n + 1));
        for (int i = 0; i < n; i++) {
            uint32_t r = guest_static_alloc(8);
            W16(r + 4, modes[i][0]); W16(r + 6, modes[i][1]);
            W32(list + 4 * i, r);
        }
        W32(list + 4 * n, 0);
    }
    CRET(list);
}
static void s_VideoModeOK(void) { CRET(CARG(2) ? CARG(2) : 32); }

static int gl_attr[32];
static void s_GL_SetAttribute(void) {
    uint32_t a = CARG(0);
    if (a < 32) gl_attr[a] = (int)CARG(1);
    CRET(0);
}
static void s_GL_GetAttribute(void) {
    uint32_t a = CARG(0), p = CARG(1);
    int v = a < 32 ? gl_attr[a] : 0;
    switch (a) {
    case 0: case 1: case 2: v = 8; break;          // red/green/blue size
    case 3: v = 8; break;                          // alpha
    case 4: v = 32; break;                         // buffer size
    case 5: v = 1; break;                          // doublebuffer
    case 6: v = 24; break;                         // depth
    }
    if (p) W32(p, v);
    CRET(0);
}

static void s_SetVideoMode(void) {
    int w = (int32_t)CARG(0), h = (int32_t)CARG(1), bpp = (int32_t)CARG(2);
    uint32_t flags = CARG(3);
    host_log("SDL_SetVideoMode(%d, %d, %d, %08x)\n", w, h, bpp, flags);
    if (w <= 0 || w > 4096) w = 1024;
    if (h <= 0 || h > 4096) h = 768;
    g_win_w = w; g_win_h = h;
    if (!g_window) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        int sw = w, sh = h;
#ifndef __EMSCRIPTEN__
        SDL_DisplayMode dm;
        if (!SDL_GetDesktopDisplayMode(0, &dm) && (sw > dm.w - 80 || sh > dm.h - 80)) {
            double s = SDL_min((double)(dm.w - 80) / sw, (double)(dm.h - 120) / sh);
            sw = (int)(sw * s); sh = (int)(sh * s);
        }
        g_window = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, sw, sh,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!g_window) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
            g_window = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, sw, sh,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        }
#else
        g_window = SDL_CreateWindow(GAME_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, sw, sh, SDL_WINDOW_OPENGL);
#endif
        if (!g_window) host_fatal("SDL_CreateWindow: %s", SDL_GetError());
        SDL_StartTextInput();
    } else {
#ifdef __EMSCRIPTEN__
        SDL_SetWindowSize(g_window, w, h);
#endif
    }
    gl1_window_created(g_window, w, h);
    if (!screen_surf) screen_surf = guest_static_alloc(60);
    W32(screen_surf, (flags & 0x80000000u) | 0x2);      // SDL_OPENGL (+ SDL_FULLSCREEN as asked)
    W32(screen_surf + 4, make_format(32, 0xff0000, 0xff00, 0xff, 0));
    W32(screen_surf + 8, w); W32(screen_surf + 12, h); W16(screen_surf + 16, w * 4);
    W32(screen_surf + 56, 1);
    CRET(screen_surf);
}

// ---------------------------------------------------------------- events
typedef struct { uint8_t b[20]; } Ev12;
#define EQN 512
static Ev12 evq[EQN];
static int eqh, eqt;
static int mouse_x, mouse_y, mouse_buttons;      // logical coordinates, SDL 1.2 button mask
static int quit_requested;
static void push_ev(const Ev12 *e) {
    int n = (eqt + 1) % EQN;
    if (n == eqh) return;
    evq[eqt] = *e; eqt = n;
}
static uint32_t mod12(void) {
    SDL_Keymod m = SDL_GetModState();
    uint32_t r = 0;
    if (m & KMOD_LSHIFT) r |= 0x1; if (m & KMOD_RSHIFT) r |= 0x2;
    if (m & KMOD_LCTRL) r |= 0x40; if (m & KMOD_RCTRL) r |= 0x80;
    if (m & KMOD_LALT) r |= 0x100; if (m & KMOD_RALT) r |= 0x200;
    if (m & KMOD_NUM) r |= 0x1000; if (m & KMOD_CAPS) r |= 0x2000;
    return r;
}
// SDL2 keycode -> SDL 1.2 SDLKey
static uint32_t key12(SDL_Keycode k) {
    if (k < 128) return (uint32_t)k;
    if (k >= SDLK_F1 && k <= SDLK_F12) return 282 + (k - SDLK_F1);
    switch (k) {
    case SDLK_KP_0: return 256; case SDLK_KP_1: return 257; case SDLK_KP_2: return 258; case SDLK_KP_3: return 259;
    case SDLK_KP_4: return 260; case SDLK_KP_5: return 261; case SDLK_KP_6: return 262; case SDLK_KP_7: return 263;
    case SDLK_KP_8: return 264; case SDLK_KP_9: return 265; case SDLK_KP_PERIOD: return 266; case SDLK_KP_DIVIDE: return 267;
    case SDLK_KP_MULTIPLY: return 268; case SDLK_KP_MINUS: return 269; case SDLK_KP_PLUS: return 270;
    case SDLK_KP_ENTER: return 271; case SDLK_KP_EQUALS: return 272;
    case SDLK_UP: return 273; case SDLK_DOWN: return 274; case SDLK_RIGHT: return 275; case SDLK_LEFT: return 276;
    case SDLK_INSERT: return 277; case SDLK_HOME: return 278; case SDLK_END: return 279;
    case SDLK_PAGEUP: return 280; case SDLK_PAGEDOWN: return 281;
    case SDLK_NUMLOCKCLEAR: return 300; case SDLK_CAPSLOCK: return 301; case SDLK_SCROLLLOCK: return 302;
    case SDLK_RSHIFT: return 303; case SDLK_LSHIFT: return 304; case SDLK_RCTRL: return 305; case SDLK_LCTRL: return 306;
    case SDLK_RALT: return 307; case SDLK_LALT: return 308; case SDLK_RGUI: return 309; case SDLK_LGUI: return 310;
    case SDLK_PRINTSCREEN: return 316; case SDLK_PAUSE: return 19;
    }
    return 0;
}
static void key_ev(int down, uint32_t sym, uint32_t uni, uint32_t mod) {
    Ev12 e; memset(&e, 0, sizeof e);
    e.b[0] = down ? 2 : 3; e.b[2] = down ? 1 : 0;
    uint32_t v;
    v = sym; memcpy(e.b + 8, &v, 4);
    v = mod; memcpy(e.b + 12, &v, 4);
    uint16_t u = (uint16_t)(down && unicode_on ? uni : 0); memcpy(e.b + 16, &u, 2);
    push_ev(&e);
}
static void motion_ev(int x, int y, int dx, int dy) {
    Ev12 e; memset(&e, 0, sizeof e);
    e.b[0] = 4; e.b[2] = (uint8_t)mouse_buttons;
    uint16_t v[4] = {(uint16_t)x, (uint16_t)y, (uint16_t)dx, (uint16_t)dy};
    memcpy(e.b + 4, v, 8);
    // coalesce with a pending motion event
    for (int i = eqh; i != eqt; i = (i + 1) % EQN)
        if (evq[i].b[0] == 4) { evq[i] = e; return; }
    push_ev(&e);
}
static void button_ev(int down, int button, int x, int y) {
    Ev12 e; memset(&e, 0, sizeof e);
    e.b[0] = down ? 5 : 6; e.b[2] = (uint8_t)button; e.b[3] = down ? 1 : 0;
    uint16_t v[2] = {(uint16_t)x, (uint16_t)y};
    memcpy(e.b + 4, v, 4);
    if (button <= 3) {
        int bit = 1 << (button - 1);
        if (down) mouse_buttons |= bit; else mouse_buttons &= ~bit;
    }
    push_ev(&e);
}
static void set_mouse(int wx, int wy) {
    int x, y;
    gl1_map_mouse(wx, wy, &x, &y);
    int dx = x - mouse_x, dy = y - mouse_y;
    mouse_x = x; mouse_y = y;
    motion_ev(x, y, dx, dy);
}

// Scripted input for tests: PORT_INPUT="frame:click:x,y;frame:rclick:x,y;frame:move:x,y;frame:key:SYM;
// frame:char:c;frame:text:abc;frame:down:SYM;frame:up:SYM" (game coordinates, SDL 1.2 key symbols in decimal)
static int deferred_click = -1, deferred_x, deferred_y, deferred_btn;
static void scripted_input(void) {
    static char *spec; static int init; static uint32_t last = 0xffffffffu;
    if (!init) { init = 1; const char *e = getenv("PORT_INPUT"); if (e) spec = strdup(e); }
    uint32_t f = host_frame_count();
    if (deferred_click >= 0 && f >= (uint32_t)deferred_click) {
        if (f == (uint32_t)deferred_click) button_ev(1, deferred_btn, deferred_x, deferred_y);
        else { button_ev(0, deferred_btn, deferred_x, deferred_y); deferred_click = -1; }
    }
    if (!spec || f == last) return;
    last = f;
    for (char *p = spec; p && *p;) {
        char *semi = strchr(p, ';');
        uint32_t fr = (uint32_t)strtoul(p, NULL, 10);
        if (fr == f) {
            char *c1 = strchr(p, ':');
            char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (c1 && c2) {
                char cmd[16]; int n = (int)(c2 - c1 - 1); if (n > 15) n = 15;
                memcpy(cmd, c1 + 1, n); cmd[n] = 0;
                const char *arg = c2 + 1;
                int x = 0, y = 0; sscanf(arg, "%d,%d", &x, &y);
                if (!strcmp(cmd, "move")) { mouse_x = x; mouse_y = y; motion_ev(x, y, 0, 0); }
                else if (!strcmp(cmd, "click") || !strcmp(cmd, "rclick")) {
                    mouse_x = x; mouse_y = y; motion_ev(x, y, 0, 0);
                    deferred_click = (int)f + 1; deferred_x = x; deferred_y = y; deferred_btn = cmd[0] == 'r' ? 3 : 1;
                } else if (!strcmp(cmd, "key")) { uint32_t s = (uint32_t)strtoul(arg, NULL, 0); key_ev(1, s, s < 128 ? s : 0, 0); key_ev(0, s, 0, 0); }
                else if (!strcmp(cmd, "down")) { uint32_t s = (uint32_t)strtoul(arg, NULL, 0); key_ev(1, s, s < 128 ? s : 0, 0); }
                else if (!strcmp(cmd, "up")) key_ev(0, (uint32_t)strtoul(arg, NULL, 0), 0, 0);
                else if (!strcmp(cmd, "char")) { key_ev(1, (uint8_t)arg[0], (uint8_t)arg[0], 0); key_ev(0, (uint8_t)arg[0], 0, 0); }
                else if (!strcmp(cmd, "text")) {
                    for (const char *t = arg; *t && *t != ';'; t++) {
                        uint32_t ch = (uint8_t)*t == '_' ? ' ' : (uint8_t)*t;
                        key_ev(1, ch >= 'A' && ch <= 'Z' ? ch + 32 : ch, ch, 0); key_ev(0, ch, 0, 0);
                    }
                }
            }
        }
        p = semi ? semi + 1 : NULL;
    }
}

static struct { int down, b, x, y; uint32_t t; } bq[16];      // mouse buttons waiting to be handed over
static int nbq;
static uint32_t moved_ms, bq_frame = 0xffffffffu;
static SDL_Event pending_key;      // a KEYDOWN waiting for its TEXTINPUT (SDL2 sends the text separately)
static int have_pending_key;
static void flush_pending_key(uint32_t uni) {
    if (!have_pending_key) return;
    have_pending_key = 0;
    SDL_Keycode k = pending_key.key.keysym.sym;
    if (!uni && k < 128 && k >= 32 && !(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT))) uni = 0;
    if (!uni && (k == 8 || k == 9 || k == 13 || k == 27 || k == 127)) uni = (uint32_t)k;
    if (!uni && k == SDLK_KP_ENTER) uni = 13;
    if (!uni && k >= 'a' && k <= 'z' && (SDL_GetModState() & KMOD_CTRL)) uni = (uint32_t)(k - 'a' + 1);
    key_ev(1, key12(k), uni > 255 ? '?' : uni, mod12());
}

void sdl12_pump(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (net_mode == NET_TEST && ev.type != SDL_QUIT) continue;
        if (ev.type != SDL_TEXTINPUT) flush_pending_key(0);
        switch (ev.type) {
        case SDL_QUIT: quit_requested = 1; { Ev12 e; memset(&e, 0, sizeof e); e.b[0] = 12; push_ev(&e); } break;
        case SDL_KEYDOWN:
            if (ev.key.repeat && !key_repeat) break;
            pending_key = ev; have_pending_key = 1;
            break;
        case SDL_TEXTINPUT: {
            const uint8_t *t = (const uint8_t *)ev.text.text;
            uint32_t cp = t[0];
            if (cp >= 0xC0 && cp < 0xE0) cp = ((cp & 0x1F) << 6) | (t[1] & 0x3F);
            else if (cp >= 0xE0) cp = ((cp & 0x0F) << 12) | ((t[1] & 0x3F) << 6) | (t[2] & 0x3F);
            if (have_pending_key) flush_pending_key(cp);
            else { key_ev(1, cp < 128 ? cp : 0, cp > 255 ? '?' : cp, mod12()); key_ev(0, cp < 128 ? cp : 0, 0, mod12()); }
            break;
        }
        case SDL_KEYUP: key_ev(0, key12(ev.key.keysym.sym), 0, mod12()); break;
        case SDL_MOUSEMOTION:
            set_mouse(ev.motion.x, ev.motion.y); moved_ms = SDL_GetTicks();
            if (getenv("PORT_INPUTLOG")) host_log("input: move %d,%d -> %d,%d frame %u\n", ev.motion.x, ev.motion.y, mouse_x, mouse_y, host_frame_count());
            break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int b = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_MIDDLE ? 2 :
                    ev.button.button == SDL_BUTTON_RIGHT ? 3 : 0;
            if (!b) break;
            int x, y; gl1_map_mouse(ev.button.x, ev.button.y, &x, &y);
            if (getenv("PORT_INPUTLOG")) host_log("input: button %d %s at %d,%d -> %d,%d (cursor %d,%d) frame %u\n", b,
                ev.type == SDL_MOUSEBUTTONDOWN ? "down" : "up", ev.button.x, ev.button.y, x, y, mouse_x, mouse_y, host_frame_count());
            if (x != mouse_x || y != mouse_y) { mouse_x = x; mouse_y = y; motion_ev(x, y, 0, 0); moved_ms = SDL_GetTicks(); }
            // Uplink highlights the button under the cursor while drawing, and acts on a press only over a
            // highlighted button: a press that arrives with the move (or a whole click within one frame, as
            // embedded browsers and fast clicks send it) waits, and presses are handed over one per frame
            if (nbq < 16) { bq[nbq].down = ev.type == SDL_MOUSEBUTTONDOWN; bq[nbq].b = b; bq[nbq].x = x; bq[nbq].y = y; bq[nbq].t = SDL_GetTicks(); nbq++; }
            break;
        }
        case SDL_MOUSEWHEEL: {
            int b = ev.wheel.y > 0 ? 4 : ev.wheel.y < 0 ? 5 : 0;
            if (b) { button_ev(1, b, mouse_x, mouse_y); button_ev(0, b, mouse_x, mouse_y); }
            break;
        }
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                // release held buttons rather than deactivating (the game would pause)
                for (int b = 1; b <= 3; b++) if (mouse_buttons & (1 << (b - 1))) button_ev(0, b, mouse_x, mouse_y);
            }
            break;
        }
    }
    flush_pending_key(0);
    // some menus update their hover state only every so often: a press waits until the cursor has rested for
    // 100 ms (at most 250 ms after it arrived)
    uint32_t now = SDL_GetTicks();
    if (nbq && (now - moved_ms >= 100 || now - bq[0].t >= 250) && host_frame_count() != bq_frame) {
        button_ev(bq[0].down, bq[0].b, bq[0].x, bq[0].y);
        memmove(bq, bq + 1, (--nbq) * sizeof *bq);
        bq_frame = host_frame_count();
    }
    scripted_input();
}

static void s_PollEvent(void) {
    uint32_t p = CARG(0);
    static uint32_t pumped = 0xffffffffu;
    if (pumped != host_frame_count()) { pumped = host_frame_count(); sdl12_pump(); }
    if (eqh == eqt) CRET(0);
    if (!p) CRET(1);
    memcpy(G + p, evq[eqh].b, 20);
    eqh = (eqh + 1) % EQN;
    CRET(1);
}
static void s_GetMouseState(void) {
    uint32_t px = CARG(0), py = CARG(1);
    if (px) W32(px, mouse_x);
    if (py) W32(py, mouse_y);
    CRET(mouse_buttons);
}
int sdl12_quit_requested(void) { return quit_requested; }

// ---------------------------------------------------------------- frame / time
static uint32_t frame_no;
uint32_t host_frame_count(void) { return frame_no; }
void shot_maybe(uint32_t frame);
static void s_GL_SwapBuffers(void) {
    gl1_present();
    shot_maybe(frame_no);
    frame_no++;
    CRET(0);
}
static void s_GetTicks(void) {
    if (net_mode != NET_OFF) CRET(net_clock());
    CRET(host_ticks());
}

// ---------------------------------------------------------------- RWops (file backed)
// guest layout: seek, read, write, close (cdecl function pointers), type, then our host slot
#define MAXRW 64
static FILE *rwf[MAXRW];
static uint32_t rw_vt[4];
static FILE *rw_file(uint32_t ctx) { uint32_t i = R32(ctx + 20); return i < MAXRW ? rwf[i] : NULL; }
static void rw_seek(void) {
    FILE *f = rw_file(CARG(0));
    int32_t off = (int32_t)CARG(1), wh = (int32_t)CARG(2);
    if (!f || fseek(f, off, wh == 0 ? SEEK_SET : wh == 1 ? SEEK_CUR : SEEK_END)) CRET(0xffffffffu);
    CRET(ftell(f));
}
static void rw_read(void) {
    FILE *f = rw_file(CARG(0));
    uint32_t p = CARG(1), sz = CARG(2), n = CARG(3);
    if (!f || !sz) CRET(0);
    COW(p, sz * n);
    CRET(fread(G + p, sz, n, f));
}
static void rw_write(void) {
    FILE *f = rw_file(CARG(0));
    uint32_t p = CARG(1), sz = CARG(2), n = CARG(3);
    extern int g_fs_dirty;
    g_fs_dirty = 1;
    if (!f || !sz) CRET(0);
    CRET(fwrite(G + p, sz, n, f));
}
static void rw_close(void) {
    uint32_t ctx = CARG(0), i = R32(ctx + 20);
    if (i < MAXRW && rwf[i]) { fclose(rwf[i]); rwf[i] = NULL; }
    heap_free(ctx);
    CRET(0);
}
FILE *sdl12_rw_host_file(uint32_t ctx) { return rw_file(ctx); }
void sdl12_rw_free(uint32_t ctx) {
    uint32_t i = R32(ctx + 20);
    if (i < MAXRW && rwf[i]) { fclose(rwf[i]); rwf[i] = NULL; }
    heap_free(ctx);
}
static void s_RWFromFile(void) {
    const char *name = GSTR(CARG(0)), *mode = GSTR(CARG(1));
    int wr = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    char path[1024];
    int found = fs_resolve(name, path, sizeof path, wr);
    FILE *f = (found || wr) ? fopen(path, mode) : NULL;
    if (!f) { host_log("SDL_RWFromFile(%s, %s) failed\n", name, mode); CRET(0); }
    int i = 0;
    while (i < MAXRW && rwf[i]) i++;
    if (i == MAXRW) { fclose(f); CRET(0); }
    rwf[i] = f;
    if (!rw_vt[0]) {
        rw_vt[0] = host_thunk(rw_seek, "RW_seek"); rw_vt[1] = host_thunk(rw_read, "RW_read");
        rw_vt[2] = host_thunk(rw_write, "RW_write"); rw_vt[3] = host_thunk(rw_close, "RW_close");
    }
    uint32_t ctx = heap_alloc(32);
    for (int k = 0; k < 4; k++) W32(ctx + 4 * k, rw_vt[k]);
    W32(ctx + 16, 2); W32(ctx + 20, i);
    CRET(ctx);
}

// ---------------------------------------------------------------- surfaces
static void s_CreateRGBSurface(void) {
    uint32_t flags = CARG(0); int w = CARG(1), h = CARG(2), d = CARG(3);
    uint32_t s = heap_alloc(60);
    memset(G + s, 0, 60);
    int bpp = (d + 7) / 8, pitch = (w * bpp + 3) & ~3;
    W32(s, flags); W32(s + 4, make_format(d, CARG(4), CARG(5), CARG(6), CARG(7)));
    W32(s + 8, w); W32(s + 12, h); W16(s + 16, pitch);
    uint32_t px = heap_alloc(pitch * h + 4);
    memset(G + px, 0, pitch * h);
    W32(s + 20, px);
    W16(s + 32, 0); W16(s + 34, 0); W16(s + 36, w); W16(s + 38, h);
    W32(s + 56, 1);
    CRET(s);
}
static void s_FreeSurface(void) {
    uint32_t s = CARG(0);
    if (s && s != screen_surf) { heap_free(R32(s + 20)); heap_free(s); }
    CRET(0);
}
// 24/32-bit surfaces only (screenshots)
static void s_SaveBMP_RW(void) {
    uint32_t s = CARG(0), rw = CARG(1), freedst = CARG(2);
    FILE *f = rw_file(rw);
    int w = R32(s + 8), h = R32(s + 12), pitch = R16(s + 16);
    uint32_t fmt = R32(s + 4), px = R32(s + 20);
    int bpp = R8(fmt + 5);
    int rs = R8(fmt + 10), gs = R8(fmt + 11), bs = R8(fmt + 12);
    if (f) {
        int rowb = (w * 3 + 3) & ~3;
        uint8_t hdr[54] = {'B', 'M'};
        uint32_t v = 54 + rowb * h; memcpy(hdr + 2, &v, 4);
        v = 54; memcpy(hdr + 10, &v, 4); v = 40; memcpy(hdr + 14, &v, 4);
        memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        uint8_t *row = calloc(1, rowb);
        for (int y = h - 1; y >= 0; y--) {
            for (int x = 0; x < w; x++) {
                uint32_t c = 0;
                memcpy(&c, G + px + y * pitch + x * bpp, bpp > 4 ? 4 : bpp);
                row[x * 3] = (uint8_t)(c >> bs); row[x * 3 + 1] = (uint8_t)(c >> gs); row[x * 3 + 2] = (uint8_t)(c >> rs);
            }
            fwrite(row, 1, rowb, f);
        }
        free(row);
        extern int g_fs_dirty; g_fs_dirty = 1;
    }
    if (freedst) sdl12_rw_free(rw);
    CRET(f ? 0 : 0xffffffffu);
}

const ShimDef shims_sdl[] = {
    {"SDL", "SDL_GetError", s_GetError}, {"SDL", "SDL_Init", s_Init}, {"SDL", "SDL_WasInit", s_WasInit},
    {"SDL", "SDL_QuitSubSystem", s_QuitSubSystem}, {"SDL", "SDL_EnableKeyRepeat", s_EnableKeyRepeat},
    {"SDL", "SDL_EnableUNICODE", s_EnableUNICODE}, {"SDL", "SDL_WM_SetCaption", s_WM_SetCaption},
    {"SDL", "SDL_GetAppState", s_GetAppState}, {"SDL", "SDL_GetVideoInfo", s_GetVideoInfo},
    {"SDL", "SDL_ListModes", s_ListModes}, {"SDL", "SDL_VideoModeOK", s_VideoModeOK},
    {"SDL", "SDL_SetVideoMode", s_SetVideoMode}, {"SDL", "SDL_GL_SetAttribute", s_GL_SetAttribute},
    {"SDL", "SDL_GL_GetAttribute", s_GL_GetAttribute}, {"SDL", "SDL_GL_SwapBuffers", s_GL_SwapBuffers},
    {"SDL", "SDL_GetTicks", s_GetTicks}, {"SDL", "SDL_GetMouseState", s_GetMouseState},
    {"SDL", "SDL_PollEvent", s_PollEvent}, {"SDL", "SDL_RWFromFile", s_RWFromFile},
    {"SDL", "SDL_CreateRGBSurface", s_CreateRGBSurface}, {"SDL", "SDL_FreeSurface", s_FreeSurface},
    {"SDL", "SDL_SaveBMP_RW", s_SaveBMP_RW},
    {0, 0, 0}
};
