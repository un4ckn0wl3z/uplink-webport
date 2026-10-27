// OpenGL 1.x (OPENGL32.dll / GLU32.dll) emulation on OpenGL ES 2 / WebGL.
//
// The game draws 2D in immediate mode (glBegin/glEnd), text through FTGL bitmap/pixmap fonts (glRasterPos +
// glBitmap / glDrawPixels) and images with glDrawPixels or small textures. It never calls glClear, so it relies
// on the back buffer keeping its contents: everything renders into a persistent FBO the size of the video mode,
// and SwapBuffers copies it to the window (letterboxed). GL conventions are kept (bottom-left origin), so
// scissor boxes, raster positions and glReadPixels need no flips.
//
// Vertices are transformed on the CPU (modelview * projection) and batched: a batch holds triangles that share
// one bound texture and the blend/scissor/stipple state; untextured vertices join any batch. Glyph bitmaps go
// into an atlas texture so a page of text is a handful of draw calls.
#include "host.h"
#include "gl.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef __EMSCRIPTEN__
#define GL_DEF(n) n##_fn *p_##n;
GL_FUNCS(GL_DEF)
void (GL_APIENTRY *p_glClearDepthf)(GLfloat);
int gl_load_all(void) {
    int ok = 1;
#define GL_LOAD(n) p_##n = (n##_fn *)SDL_GL_GetProcAddress(#n); if (!p_##n) { host_log("missing GL function %s\n", #n); ok = 0; }
    GL_FUNCS(GL_LOAD)
    return ok;
}
#endif

// GL 1.x enums the shims see from the game
#define X_POINTS 0
#define X_LINES 1
#define X_LINE_LOOP 2
#define X_LINE_STRIP 3
#define X_TRIANGLES 4
#define X_TRIANGLE_STRIP 5
#define X_TRIANGLE_FAN 6
#define X_QUADS 7
#define X_QUAD_STRIP 8
#define X_POLYGON 9
#define X_MODELVIEW 0x1700
#define X_PROJECTION 0x1701
#define X_TEXTURE 0x1702

extern SDL_Window *g_window;
static SDL_GLContext g_ctx;
static int gl_is_es = 1;
static int fb_w, fb_h;                 // the game's video mode
static GLuint fbo, fbo_tex;

// ---------------------------------------------------------------- shaders
static GLuint compile_shader(GLenum type, const char *body) {
    char src[8192];
    if (gl_is_es) snprintf(src, sizeof src, "#version 100\nprecision mediump float;\n%s", body);
    else snprintf(src, sizeof src, "#version 120\n#define lowp\n#define mediump\n#define highp\n%s", body);
    GLuint s = glCreateShader(type);
    const char *p = src;
    glShaderSource(s, 1, &p, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log); host_log("shader compile failed: %s\n---\n%s\n", log, src); }
    return s;
}
static GLuint link_program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs), f = compile_shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos"); glBindAttribLocation(p, 1, "a_col");
    glBindAttribLocation(p, 2, "a_uv"); glBindAttribLocation(p, 3, "a_ext");
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(p, sizeof log, NULL, log); host_log("link failed: %s\n", log); }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

// a_ext.x = kind: 0 colour, 1 modulate, 2 replace, 3 decal, 4 glyph (colour * tex alpha), 5 pixels (tex as is),
//           6 add (GL_BLEND env approximated), a_ext.y = distance along a stippled line in pixels (< 0: none)
static const char *VS =
    "attribute vec4 a_pos; attribute vec4 a_col; attribute vec2 a_uv; attribute vec2 a_ext;\n"
    "varying vec4 v_col; varying vec2 v_uv; varying vec2 v_ext;\n"
    "void main() { gl_Position = a_pos; v_col = a_col; v_uv = a_uv; v_ext = a_ext; }\n";
static const char *FS =
    "varying vec4 v_col; varying vec2 v_uv; varying vec2 v_ext;\n"
    "uniform sampler2D u_tex; uniform sampler2D u_pat; uniform float u_factor;\n"
    "void main() {\n"
    "  vec4 c = v_col; float k = v_ext.x;\n"
    "  if (k > 0.5) {\n"
    "    vec4 t = texture2D(u_tex, v_uv);\n"
    "    if (k < 1.5) c = c * t;\n"
    "    else if (k < 2.5) c = t;\n"
    "    else if (k < 3.5) c = vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"
    "    else if (k < 4.5) { if (t.a < 0.5) discard; }\n"     // glBitmap writes only the set bits
    "    else if (k < 5.5) c = t;\n"
    "    else if (k < 6.5) c = vec4(c.rgb * (1.0 - t.rgb) + t.rgb, c.a * t.a);\n"
    "    else c = vec4(c.rgb, c.a * t.a);\n"
    "  }\n"
    "  if (v_ext.y >= 0.0) {\n"
    "    float i = mod(floor(v_ext.y / u_factor), 16.0);\n"
    "    if (texture2D(u_pat, vec2((i + 0.5) / 16.0, 0.5)).a < 0.5) discard;\n"
    "  }\n"
    "  gl_FragColor = c;\n"
    "}\n";
static const char *PVS =
    "attribute vec4 a_pos; varying vec2 v_uv;\n"
    "void main() { gl_Position = vec4(a_pos.xy, 0.0, 1.0); v_uv = a_pos.zw; }\n";
static const char *PFS =
    "varying vec2 v_uv; uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";
static GLuint prog, pprog, vbo, pvbo, pat_tex, white_tex;
static GLint u_tex, u_pat, u_factor, pu_tex;

// ---------------------------------------------------------------- state
typedef struct { float m[16]; } Mat;
static Mat mv_stack[32], pj_stack[8], tx_stack[8];
static int mv_sp, pj_sp, tx_sp, mat_mode = X_MODELVIEW;
static Mat *cur_mat(void) {
    return mat_mode == X_PROJECTION ? &pj_stack[pj_sp] : mat_mode == X_TEXTURE ? &tx_stack[tx_sp] : &mv_stack[mv_sp];
}
static void mat_ident(Mat *m) { memset(m, 0, sizeof *m); m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1; }
static void mat_mul(Mat *m, const float *b) {    // m = m * b (column major)
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            r[c * 4 + rr] = m->m[0 * 4 + rr] * b[c * 4 + 0] + m->m[1 * 4 + rr] * b[c * 4 + 1] +
                            m->m[2 * 4 + rr] * b[c * 4 + 2] + m->m[3 * 4 + rr] * b[c * 4 + 3];
    memcpy(m->m, r, sizeof r);
}

typedef struct {
    int tex2d, blend, scissor, stipple, cull, depth, line_smooth;
    GLenum bsrc, bdst;
    int sc[4];
    float line_width;
    int stip_factor; uint16_t stip_pat;
    int env_mode;                       // GL_MODULATE etc
    uint32_t bound;                     // guest texture name
    float color[4];
    float clear[4];
    float rp_scale[4], rp_bias[4];      // pixel transfer
} State;
static State st;
static State attr_stack[16];
static int attr_sp;
static float cur_uv[2];
static int unpack_align = 4, unpack_row = 0, unpack_skip_rows, unpack_skip_px, unpack_lsb, pack_align = 4;
static double rast[4]; static int rast_valid; static float rast_color[4];

static void state_reset(void) {
    memset(&st, 0, sizeof st);
    st.bsrc = GL_ONE; st.bdst = GL_ZERO;
    st.line_width = 1; st.stip_factor = 1; st.stip_pat = 0xffff;
    st.env_mode = 0x2100;    // GL_MODULATE
    for (int i = 0; i < 4; i++) { st.color[i] = 1; st.rp_scale[i] = 1; }
    st.sc[2] = fb_w; st.sc[3] = fb_h;
    mat_ident(&mv_stack[0]); mat_ident(&pj_stack[0]); mat_ident(&tx_stack[0]);
}

// ---------------------------------------------------------------- textures
typedef struct { uint32_t name; GLuint gl; int w, h; int min_f, mag_f, wrap_s, wrap_t; int dirty_params; } Tex;
#define MAXTEX 4096
static Tex texs[MAXTEX];
static uint32_t next_name = 1;
static Tex *tex_get(uint32_t name, int create) {
    if (!name) return NULL;
    uint32_t h = name % MAXTEX;
    for (uint32_t i = 0; i < MAXTEX; i++) {
        Tex *t = &texs[(h + i) % MAXTEX];
        if (t->name == name) return t;
        if (!t->name) {
            if (!create) return NULL;
            t->name = name;
            glGenTextures(1, &t->gl);
            t->min_f = 0x2702; t->mag_f = 0x2601; t->wrap_s = t->wrap_t = 0x2901;   // GL defaults
            t->dirty_params = 1;
            return t;
        }
    }
    host_fatal("texture table full");
    return NULL;
}
static int is_pot(int v) { return v > 0 && !(v & (v - 1)); }
static void tex_apply_params(Tex *t) {
    if (!t->dirty_params) return;
    t->dirty_params = 0;
    glBindTexture(GL_TEXTURE_2D, t->gl);
    GLint mn = (t->min_f == 0x2600 || t->min_f == 0x2700 || t->min_f == 0x2702) ? GL_NEAREST : GL_LINEAR;
    if (t->min_f == 0x2601 || t->min_f == 0x2701 || t->min_f == 0x2703) mn = GL_LINEAR;
    if (t->min_f == 0x2702) mn = GL_LINEAR;     // NEAREST_MIPMAP_LINEAR default: no mipmaps here
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mn);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->mag_f == 0x2600 ? GL_NEAREST : GL_LINEAR);
    int npot = !is_pot(t->w) || !is_pot(t->h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (t->wrap_s == 0x2901 && !npot) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (t->wrap_t == 0x2901 && !npot) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

// convert client pixels (format/type, unpack state) to RGBA8 rows, bottom row first as GL stores them
static int comps(uint32_t fmt) {
    switch (fmt) {
    case 0x1906: case 0x1909: case 0x1903: case 0x1900: return 1;   // ALPHA LUMINANCE RED COLOR_INDEX
    case 0x190A: return 2;
    case 0x1907: case 0x80E0: return 3;   // RGB BGR
    case 0x1908: case 0x80E1: return 4;   // RGBA BGRA
    }
    return 4;
}
static uint8_t *unpack_rgba(uint32_t src, int w, int h, uint32_t fmt, uint32_t type, int is_tex) {
    int nc = comps(fmt);
    int bpc = (type == 0x1401 || type == 0x1400) ? 1 : (type == 0x1403 || type == 0x1402) ? 2 : (type == 0x1406 || type == 0x1405 || type == 0x1404) ? 4 : 1;
    int rowlen = unpack_row > 0 ? unpack_row : w;
    int stride = rowlen * nc * bpc;
    stride = (stride + unpack_align - 1) / unpack_align * unpack_align;
    uint8_t *out = malloc((size_t)w * h * 4 + 4);
    uint32_t base = src + unpack_skip_rows * stride + unpack_skip_px * nc * bpc;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = G + base + (uint32_t)y * stride;
        uint8_t *o = out + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            float c[4];
            for (int k = 0; k < nc; k++) {
                const uint8_t *p = row + (x * nc + k) * bpc;
                if (bpc == 1) c[k] = p[0] / 255.0f;
                else if (bpc == 2) { uint16_t v; memcpy(&v, p, 2); c[k] = v / 65535.0f; }
                else if (type == 0x1406) { float v; memcpy(&v, p, 4); c[k] = v; }
                else { uint32_t v; memcpy(&v, p, 4); c[k] = (float)(v / 4294967295.0); }
            }
            float r, g, b, a;
            switch (fmt) {
            case 0x1906: r = g = b = is_tex ? 1.0f : 0.0f; a = c[0]; break;
            case 0x1909: r = g = b = c[0]; a = 1; break;
            case 0x190A: r = g = b = c[0]; a = c[1]; break;
            case 0x1907: r = c[0]; g = c[1]; b = c[2]; a = 1; break;
            case 0x80E0: r = c[2]; g = c[1]; b = c[0]; a = 1; break;
            case 0x80E1: r = c[2]; g = c[1]; b = c[0]; a = c[3]; break;
            default: r = c[0]; g = c[1]; b = c[2]; a = c[3]; break;
            }
            if (!is_tex) {
                r = r * st.rp_scale[0] + st.rp_bias[0]; g = g * st.rp_scale[1] + st.rp_bias[1];
                b = b * st.rp_scale[2] + st.rp_bias[2]; a = a * st.rp_scale[3] + st.rp_bias[3];
            }
            float v[4] = {r, g, b, a};
            for (int k = 0; k < 4; k++) { float q = v[k] < 0 ? 0 : v[k] > 1 ? 1 : v[k]; o[x * 4 + k] = (uint8_t)(q * 255.0f + 0.5f); }
        }
    }
    return out;
}

// ---------------------------------------------------------------- batching
typedef struct { float x, y, z, w; uint8_t c[4]; float u, v; float kind, dist; } Vtx;
#define MAXV 65536
static Vtx batch[MAXV];
static int nb;
static GLuint batch_tex;         // GL texture of the batch (0 = none yet)
typedef struct { int blend; GLenum bs, bd; int sc; int box[4]; int stip; uint16_t pat; int factor; } BKey;
static BKey bkey;
static int draw_calls, frame_draws;

static void flush(void) {
    if (!nb) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, fb_w, fb_h);
    glUseProgram(prog);
    if (bkey.blend) { glEnable(GL_BLEND); glBlendFunc(bkey.bs, bkey.bd); } else glDisable(GL_BLEND);
    if (bkey.sc) { glEnable(GL_SCISSOR_TEST); glScissor(bkey.box[0], bkey.box[1], bkey.box[2], bkey.box[3]); }
    else glDisable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, pat_tex);
    if (bkey.stip) {
        uint8_t p[16 * 4];
        for (int i = 0; i < 16; i++) { p[i * 4] = p[i * 4 + 1] = p[i * 4 + 2] = 255; p[i * 4 + 3] = (bkey.pat >> i & 1) ? 255 : 0; }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
        glUniform1f(u_factor, (float)(bkey.factor < 1 ? 1 : bkey.factor));
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, batch_tex ? batch_tex : white_tex);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, nb * sizeof(Vtx), batch, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2); glEnableVertexAttribArray(3);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void *)16);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)20);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)28);
    glDrawArrays(GL_TRIANGLES, 0, nb);
    nb = 0;
    batch_tex = 0;
    draw_calls++; frame_draws++;
}
// make room for n vertices drawn with texture `tex` (0 = none) under the current state
static Vtx *reserve(int n, GLuint tex, int stippled) {
    BKey k; memset(&k, 0, sizeof k);
    k.blend = st.blend; if (st.blend) { k.bs = st.bsrc; k.bd = st.bdst; }
    k.sc = st.scissor; if (st.scissor) memcpy(k.box, st.sc, sizeof k.box);
    k.stip = stippled; if (stippled) { k.pat = st.stip_pat; k.factor = st.stip_factor; }
    if (nb && (memcmp(&k, &bkey, sizeof k) || (tex && batch_tex && tex != batch_tex) || nb + n > MAXV)) flush();
    bkey = k;
    if (tex) batch_tex = tex;
    Vtx *v = &batch[nb];
    nb += n;
    return v;
}

// ---------------------------------------------------------------- immediate mode
typedef struct { double clip[4]; float c[4]; float uv[2]; } IV;
static IV im[16384];
static int nim, im_mode = -1;

static void xform(const double in[4], double out[4]) {
    const float *a = mv_stack[mv_sp].m, *p = pj_stack[pj_sp].m;
    double e[4];
    for (int r = 0; r < 4; r++) e[r] = a[r] * in[0] + a[4 + r] * in[1] + a[8 + r] * in[2] + a[12 + r] * in[3];
    for (int r = 0; r < 4; r++) out[r] = p[r] * e[0] + p[4 + r] * e[1] + p[8 + r] * e[2] + p[12 + r] * e[3];
}
static void vertex(double x, double y, double z) {
    if (im_mode < 0) return;
    if (nim >= (int)(sizeof im / sizeof *im)) return;
    double in[4] = {x, y, z, 1};
    IV *v = &im[nim++];
    xform(in, v->clip);
    memcpy(v->c, st.color, sizeof v->c);
    v->uv[0] = cur_uv[0]; v->uv[1] = cur_uv[1];
}
static int env_kind(void) {
    switch (st.env_mode) {
    case 0x1E01: return 2;     // GL_REPLACE
    case 0x2101: return 3;     // GL_DECAL
    case 0x0BE2: return 6;     // GL_BLEND
    default: return 1;         // GL_MODULATE
    }
}
static void put(Vtx *o, const IV *v, int kind, float dist) {
    o->x = (float)v->clip[0]; o->y = (float)v->clip[1]; o->z = 0; o->w = (float)v->clip[3];
    for (int k = 0; k < 4; k++) { float q = v->c[k] < 0 ? 0 : v->c[k] > 1 ? 1 : v->c[k]; o->c[k] = (uint8_t)(q * 255.0f + 0.5f); }
    o->u = v->uv[0]; o->v = v->uv[1]; o->kind = (float)kind; o->dist = dist;
}
static GLuint bound_gl(void) {
    if (!st.tex2d) return 0;
    Tex *t = tex_get(st.bound, 0);
    if (!t || !t->w) return 0;
    tex_apply_params(t);
    return t->gl;
}
static void tri(const IV *a, const IV *b, const IV *c) {
    GLuint t = bound_gl();
    Vtx *o = reserve(3, t, 0);
    int k = t ? env_kind() : 0;
    put(o, a, k, -1); put(o + 1, b, k, -1); put(o + 2, c, k, -1);
}
// window coordinates (pixels, bottom-left) of a clip-space vertex
static void to_win(const double *c, double *wx, double *wy) {
    double w = c[3] ? c[3] : 1;
    *wx = (c[0] / w * 0.5 + 0.5) * fb_w;
    *wy = (c[1] / w * 0.5 + 0.5) * fb_h;
}
static void from_win(double wx, double wy, double *c) {
    c[0] = wx / fb_w * 2 - 1; c[1] = wy / fb_h * 2 - 1; c[2] = 0; c[3] = 1;
}
static double line_dist;
static void line(const IV *a, const IV *b) {
    double ax, ay, bx, by;
    to_win(a->clip, &ax, &ay); to_win(b->clip, &bx, &by);
    double dx = bx - ax, dy = by - ay, len = sqrt(dx * dx + dy * dy);
    double hw = (st.line_width < 1 ? 1 : st.line_width) * 0.5;
    double nx, ny;
    // GL's rasterisation of a thin line covers about one pixel across; widen along the minor axis like GL does
    if (len < 1e-6) { dx = 1; dy = 0; len = 1; }
    if (fabs(dx) >= fabs(dy)) { nx = 0; ny = hw; } else { nx = hw; ny = 0; }
    // GL lines include the start pixel and exclude the last: offset by half a pixel along the line at both ends
    double ux = dx / len * 0.5, uy = dy / len * 0.5;
    double p[4][2] = {{ax - nx + ux * 0 + 0.5 - 0.5, ay - ny}, {ax + nx, ay + ny}, {bx + nx, by + ny}, {bx - nx, by - ny}};
    (void)ux; (void)uy;
    IV q[4];
    for (int i = 0; i < 4; i++) {
        q[i] = i < 2 ? *a : *b;
        from_win(p[i][0], p[i][1], q[i].clip);
    }
    int stip = st.stipple && st.stip_pat != 0xffff;
    GLuint t = bound_gl();
    Vtx *o = reserve(6, t, stip);
    int k = t ? env_kind() : 0;
    float d0 = stip ? (float)line_dist : -1, d1 = stip ? (float)(line_dist + len) : -1;
    put(o, &q[0], k, d0); put(o + 1, &q[1], k, d0); put(o + 2, &q[2], k, d1);
    put(o + 3, &q[0], k, d0); put(o + 4, &q[2], k, d1); put(o + 5, &q[3], k, d1);
    line_dist += len;
}
static void point(const IV *a) {
    double x, y; to_win(a->clip, &x, &y);
    IV q[4];
    double s = 0.5;
    double p[4][2] = {{x - s, y - s}, {x + s, y - s}, {x + s, y + s}, {x - s, y + s}};
    for (int i = 0; i < 4; i++) { q[i] = *a; from_win(p[i][0], p[i][1], q[i].clip); }
    tri(&q[0], &q[1], &q[2]); tri(&q[0], &q[2], &q[3]);
}
static void im_end(void) {
    int n = nim, m = im_mode;
    im_mode = -1;
    line_dist = 0;
    switch (m) {
    case X_POINTS: for (int i = 0; i < n; i++) point(&im[i]); break;
    case X_LINES: for (int i = 0; i + 1 < n; i += 2) { line_dist = 0; line(&im[i], &im[i + 1]); } break;
    case X_LINE_STRIP: for (int i = 0; i + 1 < n; i++) line(&im[i], &im[i + 1]); break;
    case X_LINE_LOOP: for (int i = 0; i + 1 < n; i++) line(&im[i], &im[i + 1]); if (n > 2) line(&im[n - 1], &im[0]); break;
    case X_TRIANGLES: for (int i = 0; i + 2 < n; i += 3) tri(&im[i], &im[i + 1], &im[i + 2]); break;
    case X_TRIANGLE_STRIP: case X_QUAD_STRIP:
        for (int i = 0; i + 2 < n; i++) {
            if (i & 1) tri(&im[i + 1], &im[i], &im[i + 2]); else tri(&im[i], &im[i + 1], &im[i + 2]);
        }
        break;
    case X_TRIANGLE_FAN: case X_POLYGON: for (int i = 1; i + 1 < n; i++) tri(&im[0], &im[i], &im[i + 1]); break;
    case X_QUADS: for (int i = 0; i + 3 < n; i += 4) { tri(&im[i], &im[i + 1], &im[i + 2]); tri(&im[i], &im[i + 2], &im[i + 3]); } break;
    }
    nim = 0;
}

// a window-aligned image quad at window pixel (x, y) bottom-left, size w x h, from texture tex with uv rect
static void win_quad(double x, double y, double w, double h, GLuint tex, float u0, float v0, float u1, float v1,
                     const float col[4], int kind) {
    IV q[4];
    double p[4][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    for (int i = 0; i < 4; i++) {
        from_win(p[i][0], p[i][1], q[i].clip);
        memcpy(q[i].c, col, sizeof q[i].c);
        q[i].uv[0] = uv[i][0]; q[i].uv[1] = uv[i][1];
    }
    Vtx *o = reserve(6, tex, 0);
    put(o, &q[0], kind, -1); put(o + 1, &q[1], kind, -1); put(o + 2, &q[2], kind, -1);
    put(o + 3, &q[0], kind, -1); put(o + 4, &q[2], kind, -1); put(o + 5, &q[3], kind, -1);
}

// ---------------------------------------------------------------- glyph atlas (glBitmap)
#define ATLAS 1024
static GLuint atlas_tex;
static int at_x, at_y, at_row;
typedef struct { uint64_t key; int x, y, w, h; } Glyph;
#define MAXG 8192
static Glyph glyphs[MAXG];
static int nglyphs;
static uint64_t fnv(const uint8_t *p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static void atlas_reset(void) {
    flush();
    nglyphs = 0; at_x = at_y = at_row = 0;
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    uint8_t *z = calloc(ATLAS * ATLAS, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS, ATLAS, 0, GL_RGBA, GL_UNSIGNED_BYTE, z);
    free(z);
}
static Glyph *glyph_get(int w, int h, uint32_t bits) {
    int stride = ((w + 7) / 8 + unpack_align - 1) / unpack_align * unpack_align;
    if (unpack_row > 0) stride = ((unpack_row + 7) / 8 + unpack_align - 1) / unpack_align * unpack_align;
    uint64_t key = fnv(G + bits, (size_t)stride * h, 1469598103934665603ull ^ ((uint64_t)w << 40) ^ ((uint64_t)h << 20) ^ (uint64_t)unpack_lsb);
    uint32_t hs = (uint32_t)(key % MAXG);
    for (int i = 0; i < MAXG; i++) {
        Glyph *g = &glyphs[(hs + i) % MAXG];
        if (g->key == key && g->w == w) return g;
        if (!g->key) break;
    }
    if (nglyphs > MAXG * 3 / 4 || w > 256 || h > 256) {
        if (w > 256 || h > 256) return NULL;
        atlas_reset();
    }
    if (at_x + w + 1 > ATLAS) { at_x = 0; at_y += at_row + 1; at_row = 0; }
    if (at_y + h + 1 > ATLAS) { atlas_reset(); }
    uint8_t *px = calloc((size_t)w * h, 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t byte = G[bits + y * stride + x / 8];
            int on = unpack_lsb ? (byte >> (x & 7)) & 1 : (byte >> (7 - (x & 7))) & 1;
            uint8_t *o = px + (y * w + x) * 4;
            o[0] = o[1] = o[2] = 255; o[3] = on ? 255 : 0;
        }
    flush();
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, at_x, at_y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    for (int i = 0; i < MAXG; i++) {
        Glyph *g = &glyphs[(hs + i) % MAXG];
        if (!g->key) {
            g->key = key; g->x = at_x; g->y = at_y; g->w = w; g->h = h;
            nglyphs++;
            at_x += w + 1; if (h > at_row) at_row = h;
            return g;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- image cache (glDrawPixels)
typedef struct { uint64_t key; GLuint gl; int w, h; uint32_t last; } Img;
#define MAXIMG 256
static Img imgs[MAXIMG];
static uint32_t frame_counter;
static GLuint image_tex(uint32_t px, int w, int h, uint32_t fmt, uint32_t type) {
    int nc = comps(fmt);
    int rowlen = unpack_row > 0 ? unpack_row : w;
    int stride = (rowlen * nc + unpack_align - 1) / unpack_align * unpack_align;
    uint64_t key = fnv(G + px, (size_t)stride * h, 1469598103934665603ull ^ ((uint64_t)w << 32) ^ ((uint64_t)h << 16) ^ fmt ^ ((uint64_t)type << 48));
    for (int k = 0; k < 4; k++) key = key * 31 + (uint64_t)(st.rp_scale[k] * 1000) + (uint64_t)(st.rp_bias[k] * 1000) * 7;
    int lru = 0;
    for (int i = 0; i < MAXIMG; i++) {
        if (imgs[i].key == key && imgs[i].gl) { imgs[i].last = frame_counter; return imgs[i].gl; }
        if (imgs[i].last < imgs[lru].last) lru = i;
    }
    Img *m = &imgs[lru];
    flush();
    if (!m->gl) glGenTextures(1, &m->gl);
    m->key = key; m->w = w; m->h = h; m->last = frame_counter;
    uint8_t *rgba = unpack_rgba(px, w, h, fmt, type, 0);
    glBindTexture(GL_TEXTURE_2D, m->gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(rgba);
    return m->gl;
}

// ---------------------------------------------------------------- context, present, mouse
static int vp_x, vp_y, vp_w, vp_h;
void gl1_window_created(SDL_Window *w, int gw, int gh) {
    if (!g_ctx) {
        g_ctx = SDL_GL_CreateContext(w);
        if (!g_ctx) {
            host_log("GLES2 context failed (%s), trying desktop GL 2.1\n", SDL_GetError());
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
            g_ctx = SDL_GL_CreateContext(w);
            gl_is_es = 0;
        }
        if (!g_ctx) host_fatal("cannot create GL context: %s", SDL_GetError());
        SDL_GL_MakeCurrent(w, g_ctx);
        if (!GL_LOAD_ALL()) host_fatal("GL functions missing");
        const char *ver = (const char *)glGetString(GL_VERSION);
        host_log("GL: %s / %s\n", ver, (const char *)glGetString(GL_RENDERER));
        if (ver && !strstr(ver, "OpenGL ES") && !strstr(ver, "WebGL")) gl_is_es = 0;
#ifndef __EMSCRIPTEN__
        SDL_GL_SetSwapInterval(1);
#endif
        prog = link_program(VS, FS);
        u_tex = glGetUniformLocation(prog, "u_tex"); u_pat = glGetUniformLocation(prog, "u_pat");
        u_factor = glGetUniformLocation(prog, "u_factor");
        glUseProgram(prog); glUniform1i(u_tex, 0); glUniform1i(u_pat, 1); glUniform1f(u_factor, 1);
        pprog = link_program(PVS, PFS);
        pu_tex = glGetUniformLocation(pprog, "u_tex");
        glGenBuffers(1, &vbo); glGenBuffers(1, &pvbo);
        uint8_t white[4] = {255, 255, 255, 255};
        glGenTextures(1, &white_tex); glBindTexture(GL_TEXTURE_2D, white_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        GLuint ts[2]; glGenTextures(2, ts); pat_tex = ts[0]; atlas_tex = ts[1];
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, ts[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, pat_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        atlas_reset();
        glGenFramebuffers(1, &fbo); glGenTextures(1, &fbo_tex);
    }
    flush();
    if (gw != fb_w || gh != fb_h) {
        fb_w = gw; fb_h = gh;
        glBindTexture(GL_TEXTURE_2D, fbo_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fb_w, fb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) host_log("FBO incomplete\n");
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    state_reset();
}
static void compute_viewport(void) {
    int ww, wh;
    SDL_GL_GetDrawableSize(g_window, &ww, &wh);
    if (ww <= 0 || wh <= 0) { ww = fb_w; wh = fb_h; }
    double s = SDL_min((double)ww / fb_w, (double)wh / fb_h);
    vp_w = (int)(fb_w * s + 0.5); vp_h = (int)(fb_h * s + 0.5);
    vp_x = (ww - vp_w) / 2; vp_y = (wh - vp_h) / 2;
}
void gl1_map_mouse(int wx, int wy, int *gx, int *gy) {
    if (!fb_w || !g_window) { *gx = wx; *gy = wy; return; }
    int lw, lh, dw, dh;
    SDL_GetWindowSize(g_window, &lw, &lh);
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    double sx = lw ? (double)dw / lw : 1, sy = lh ? (double)dh / lh : 1;
    compute_viewport();
    double x = (wx * sx - vp_x) * fb_w / vp_w, y = (wy * sy - vp_y) * fb_h / vp_h;
    *gx = (int)SDL_clamp(x, 0, fb_w - 1); *gy = (int)SDL_clamp(y, 0, fb_h - 1);
}
void gl1_present(void) {
    flush();
    compute_viewport();
    int ww, wh;
    SDL_GL_GetDrawableSize(g_window, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(vp_x, vp_y, vp_w, vp_h);
    glUseProgram(pprog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    int integer = vp_w % fb_w == 0 && vp_h % fb_h == 0;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, integer ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, integer ? GL_NEAREST : GL_LINEAR);
    glUniform1i(pu_tex, 0);
    float q[] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
    glBindBuffer(GL_ARRAY_BUFFER, pvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1); glDisableVertexAttribArray(2); glDisableVertexAttribArray(3);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    SDL_GL_SwapWindow(g_window);
    frame_counter++;
    static int logged;
    if (getenv("PORT_GLLOG") && logged++ < 400) host_log("frame %u: %d draws\n", frame_counter, frame_draws);
    frame_draws = 0;
}
// PORT_SHOT=100,500: write the frame to shot_00100.bmp
void shot_maybe(uint32_t frame) {
    static const char *spec; static int init;
    if (!init) { init = 1; spec = getenv("PORT_SHOT"); }
    if (!spec) return;
    char tok[16]; snprintf(tok, sizeof tok, "%u", frame);
    const char *p = spec; int hit = 0;
    while (p && *p) {
        size_t l = strcspn(p, ",");
        if (l == strlen(tok) && !strncmp(p, tok, l)) hit = 1;
        p = p[l] ? p + l + 1 : NULL;
    }
    if (!hit) return;
    flush();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    uint8_t *px = malloc((size_t)fb_w * fb_h * 4);
    glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    char fn[64]; snprintf(fn, sizeof fn, "shot_%05u.bmp", frame);
    FILE *f = fopen(fn, "wb");
    if (f) {
        int rowb = fb_w * 3; rowb = (rowb + 3) & ~3;
        uint8_t hdr[54] = {'B', 'M'};
        uint32_t v = 54 + rowb * fb_h; memcpy(hdr + 2, &v, 4); v = 54; memcpy(hdr + 10, &v, 4);
        v = 40; memcpy(hdr + 14, &v, 4); memcpy(hdr + 18, &fb_w, 4); memcpy(hdr + 22, &fb_h, 4);
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        uint8_t *row = calloc(1, rowb);
        for (int y = 0; y < fb_h; y++) {
            for (int x = 0; x < fb_w; x++) {
                uint8_t *s = px + ((size_t)y * fb_w + x) * 4;
                row[x * 3] = s[2]; row[x * 3 + 1] = s[1]; row[x * 3 + 2] = s[0];
            }
            fwrite(row, 1, rowb, f);
        }
        free(row);
        fclose(f);
        host_log("screenshot %s\n", fn);
    }
    free(px);
}

// Host-rendered text (lang.c): an alpha bitmap whose baseline is at the game coordinate (gx, gy), drawn like the
// game's own glyphs (raster position through the current matrices, current colour), always alpha blended.
void gl1_draw_text_alpha(int gx, int gy, int w, int h, int top, const uint8_t *alpha, uint64_t key) {
    double in[4] = {gx, gy, 0, 1}, c[4];
    xform(in, c);
    double cw = c[3] ? c[3] : 1;
    double wx = (c[0] / cw * 0.5 + 0.5) * fb_w, wy = (c[1] / cw * 0.5 + 0.5) * fb_h;
    GLuint tex = 0;
    int lru = 0;
    for (int i = 0; i < MAXIMG; i++) {
        if (imgs[i].key == key && imgs[i].gl && imgs[i].w == w) { imgs[i].last = frame_counter; tex = imgs[i].gl; break; }
        if (imgs[i].last < imgs[lru].last) lru = i;
    }
    if (!tex) {
        Img *m = &imgs[lru];
        flush();
        if (!m->gl) glGenTextures(1, &m->gl);
        m->key = key; m->w = w; m->h = h; m->last = frame_counter;
        uint8_t *rgba = malloc((size_t)w * h * 4);
        for (int i = 0; i < w * h; i++) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = alpha[i]; }
        glBindTexture(GL_TEXTURE_2D, m->gl);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        free(rgba);
        tex = m->gl;
    }
    int ob = st.blend; GLenum os = st.bsrc, od = st.bdst;
    st.blend = 1; st.bsrc = GL_SRC_ALPHA; st.bdst = GL_ONE_MINUS_SRC_ALPHA;
    win_quad(floor(wx), floor(wy) + top - h, w, h, tex, 0, 1, 1, 0, st.color, 7);
    st.blend = ob; st.bsrc = os; st.bdst = od;
}

// ---------------------------------------------------------------- the GL 1.x entry points (stdcall)
static float AF(int i) { float f; uint32_t v = ARG(i); memcpy(&f, &v, 4); return f; }
static double AD(int i) { uint64_t v = (uint64_t)ARG(i) | ((uint64_t)ARG(i + 1) << 32); double d; memcpy(&d, &v, 8); return d; }
static void logonce(const char *what, uint32_t v) {
    static uint32_t seen[256]; static int n;
    uint32_t key = (uint32_t)(uintptr_t)what * 2654435761u ^ v;
    for (int i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 256) seen[n++] = key;
    host_log("gl: %s 0x%x\n", what, v);
}

static void g_Begin(void) { if (im_mode >= 0) im_end(); im_mode = (int)ARG(0); nim = 0; SRET(0, 1); }
static void g_End(void) { im_end(); SRET(0, 0); }
static void g_Vertex2i(void) { vertex((int32_t)ARG(0), (int32_t)ARG(1), 0); SRET(0, 2); }
static void g_Vertex2f(void) { vertex(AF(0), AF(1), 0); SRET(0, 2); }
static void g_Vertex2d(void) { vertex(AD(0), AD(2), 0); SRET(0, 4); }
static void g_Vertex3i(void) { vertex((int32_t)ARG(0), (int32_t)ARG(1), (int32_t)ARG(2)); SRET(0, 3); }
static void g_TexCoord2f(void) { cur_uv[0] = AF(0); cur_uv[1] = AF(1); SRET(0, 2); }
static void g_Color3f(void) { st.color[0] = AF(0); st.color[1] = AF(1); st.color[2] = AF(2); st.color[3] = 1; SRET(0, 3); }
static void g_Color4f(void) { st.color[0] = AF(0); st.color[1] = AF(1); st.color[2] = AF(2); st.color[3] = AF(3); SRET(0, 4); }
static void g_Color3d(void) { st.color[0] = (float)AD(0); st.color[1] = (float)AD(2); st.color[2] = (float)AD(4); st.color[3] = 1; SRET(0, 6); }
static void g_Color3ub(void) {
    st.color[0] = (ARG(0) & 0xff) / 255.0f; st.color[1] = (ARG(1) & 0xff) / 255.0f; st.color[2] = (ARG(2) & 0xff) / 255.0f;
    st.color[3] = 1; SRET(0, 3);
}

static void g_MatrixMode(void) { mat_mode = (int)ARG(0); SRET(0, 1); }
static void g_LoadIdentity(void) { mat_ident(cur_mat()); SRET(0, 0); }
static void g_PushMatrix(void) {
    if (mat_mode == X_PROJECTION) { if (pj_sp < 7) { pj_stack[pj_sp + 1] = pj_stack[pj_sp]; pj_sp++; } }
    else if (mat_mode == X_TEXTURE) { if (tx_sp < 7) { tx_stack[tx_sp + 1] = tx_stack[tx_sp]; tx_sp++; } }
    else if (mv_sp < 31) { mv_stack[mv_sp + 1] = mv_stack[mv_sp]; mv_sp++; }
    SRET(0, 0);
}
static void g_PopMatrix(void) {
    if (mat_mode == X_PROJECTION) { if (pj_sp) pj_sp--; }
    else if (mat_mode == X_TEXTURE) { if (tx_sp) tx_sp--; }
    else if (mv_sp) mv_sp--;
    SRET(0, 0);
}
static void g_Ortho(void) {
    double l = AD(0), r = AD(2), b = AD(4), t = AD(6), n = AD(8), f = AD(10);
    float m[16] = {0};
    m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n)); m[15] = 1;
    mat_mul(cur_mat(), m);
    SRET(0, 12);
}
static void g_Translatef(void) {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, AF(0), AF(1), AF(2), 1};
    mat_mul(cur_mat(), m);
    SRET(0, 3);
}
static void glu_Perspective(void) {
    double fovy = AD(0), aspect = AD(2), zn = AD(4), zf = AD(6);
    double f = 1.0 / tan(fovy * M_PI / 360.0);
    float m[16] = {0};
    m[0] = (float)(f / aspect); m[5] = (float)f; m[10] = (float)((zf + zn) / (zn - zf)); m[11] = -1;
    m[14] = (float)(2 * zf * zn / (zn - zf));
    mat_mul(cur_mat(), m);
    SRET(0, 8);
}

static int *cap_ptr(uint32_t cap) {
    switch (cap) {
    case 0x0DE1: return &st.tex2d;
    case 0x0BE2: return &st.blend;
    case 0x0C11: return &st.scissor;
    case 0x0B24: return &st.stipple;
    case 0x0B44: return &st.cull;
    case 0x0B71: return &st.depth;
    case 0x0B20: return &st.line_smooth;
    }
    return NULL;
}
static void g_Enable(void) { int *p = cap_ptr(ARG(0)); if (p) *p = 1; else logonce("glEnable", ARG(0)); SRET(0, 1); }
static void g_Disable(void) { int *p = cap_ptr(ARG(0)); if (p) *p = 0; else logonce("glDisable", ARG(0)); SRET(0, 1); }
static void g_BlendFunc(void) { st.bsrc = ARG(0); st.bdst = ARG(1); SRET(0, 2); }
static void g_Scissor(void) { st.sc[0] = ARG(0); st.sc[1] = ARG(1); st.sc[2] = ARG(2); st.sc[3] = ARG(3); SRET(0, 4); }
static void g_LineWidth(void) { st.line_width = AF(0); SRET(0, 1); }
static void g_LineStipple(void) { st.stip_factor = (int)ARG(0); st.stip_pat = (uint16_t)ARG(1); SRET(0, 2); }
static void g_Hint(void) { SRET(0, 2); }
static void g_Finish(void) { flush(); SRET(0, 0); }
static void g_ClearColor(void) { for (int i = 0; i < 4; i++) st.clear[i] = AF(i); SRET(0, 4); }
static void g_TexEnvf(void) { if (ARG(1) == 0x2200) st.env_mode = (int)AF(2); SRET(0, 3); }
static void g_PushAttrib(void) { if (attr_sp < 16) attr_stack[attr_sp++] = st; SRET(0, 1); }
static void g_PopAttrib(void) {
    if (attr_sp) {
        State s = attr_stack[--attr_sp];
        // matrices are not attributes; the colour and the rest are restored as a block
        st = s;
    }
    SRET(0, 0);
}
static void g_PixelStorei(void) {
    uint32_t p = ARG(0); int v = (int)ARG(1);
    switch (p) {
    case 0x0CF5: unpack_align = v ? v : 1; break;
    case 0x0CF2: unpack_row = v; break;
    case 0x0CF3: unpack_skip_rows = v; break;
    case 0x0CF4: unpack_skip_px = v; break;
    case 0x0CF1: unpack_lsb = v; break;
    case 0x0D05: pack_align = v ? v : 1; break;
    case 0x0CF0: break;
    default: logonce("glPixelStorei", p);
    }
    SRET(0, 2);
}
static void g_PixelTransferi(void) {
    uint32_t p = ARG(0); float v = (float)(int32_t)ARG(1);
    switch (p) {
    case 0x0D14: st.rp_scale[0] = v; break; case 0x0D15: st.rp_bias[0] = v; break;
    case 0x0D18: st.rp_scale[1] = v; break; case 0x0D19: st.rp_bias[1] = v; break;
    case 0x0D1A: st.rp_scale[2] = v; break; case 0x0D1B: st.rp_bias[2] = v; break;
    case 0x0D1C: st.rp_scale[3] = v; break; case 0x0D1D: st.rp_bias[3] = v; break;
    default: logonce("glPixelTransferi", p);
    }
    SRET(0, 2);
}
static int get_int(uint32_t p, int *out) {
    switch (p) {
    case 0x0BA2: out[0] = 0; out[1] = 0; out[2] = fb_w; out[3] = fb_h; return 4;     // VIEWPORT
    case 0x0C10: memcpy(out, st.sc, 16); return 4;                                   // SCISSOR_BOX
    case 0x0D33: out[0] = 2048; return 1;                                            // MAX_TEXTURE_SIZE
    case 0x0CF5: out[0] = unpack_align; return 1;
    case 0x0CF2: out[0] = unpack_row; return 1;
    case 0x0CF3: out[0] = unpack_skip_rows; return 1;
    case 0x0CF4: out[0] = unpack_skip_px; return 1;
    case 0x0CF1: out[0] = unpack_lsb; return 1;
    case 0x0CF0: out[0] = 0; return 1;
    case 0x0D05: out[0] = pack_align; return 1;
    case 0x0BA0: out[0] = mat_mode; return 1;
    case 0x8069: out[0] = (int)st.bound; return 1;                                   // TEXTURE_BINDING_2D
    case 0x0B08: out[0] = rast_valid; return 1;                                      // CURRENT_RASTER_POSITION_VALID
    case 0x0BE2: out[0] = st.blend; return 1;
    case 0x0DE1: out[0] = st.tex2d; return 1;
    case 0x0C11: out[0] = st.scissor; return 1;
    case 0x0BE1: out[0] = (int)st.bsrc; return 1;                                    // BLEND_SRC
    case 0x0BE0: out[0] = (int)st.bdst; return 1;                                    // BLEND_DST
    case 0x0D32: out[0] = 8; return 1;                                               // MAX_MODELVIEW_STACK_DEPTH etc
    case 0x0D3A: out[0] = fb_w; out[1] = fb_h; return 2;                             // MAX_VIEWPORT_DIMS
    }
    logonce("glGet", p);
    out[0] = 0;
    return 1;
}
static void g_GetIntegerv(void) {
    int v[16]; int n = get_int(ARG(0), v);
    for (int i = 0; i < n; i++) W32(ARG(1) + 4 * i, (uint32_t)v[i]);
    SRET(0, 2);
}
static void g_GetBooleanv(void) {
    int v[16]; int n = get_int(ARG(0), v);
    for (int i = 0; i < n; i++) W8(ARG(1) + i, v[i] != 0);
    SRET(0, 2);
}

static void g_GenTextures(void) {
    int n = (int)ARG(0); uint32_t p = ARG(1);
    for (int i = 0; i < n; i++) { W32(p + 4 * i, next_name); tex_get(next_name, 1); next_name++; }
    SRET(0, 2);
}
static void g_BindTexture(void) {
    st.bound = ARG(1);
    if (st.bound) tex_get(st.bound, 1);
    SRET(0, 2);
}
static void g_TexParameteri(void) {
    Tex *t = tex_get(st.bound, 1);
    if (t) {
        switch (ARG(1)) {
        case 0x2801: t->min_f = ARG(2); break;
        case 0x2800: t->mag_f = ARG(2); break;
        case 0x2802: t->wrap_s = ARG(2); break;
        case 0x2803: t->wrap_t = ARG(2); break;
        default: logonce("glTexParameteri", ARG(1));
        }
        t->dirty_params = 1;
    }
    SRET(0, 3);
}
static void g_TexImage2D(void) {
    uint32_t level = ARG(1), ifmt = ARG(2); int w = (int)ARG(3), h = (int)ARG(4);
    uint32_t fmt = ARG(6), type = ARG(7), px = ARG(8);
    logonce("glTexImage2D ifmt", ifmt); logonce("glTexImage2D fmt", fmt | type << 16);
    Tex *t = tex_get(st.bound, 1);
    if (!t || level) SRET(0, 9);
    flush();
    uint8_t *rgba;
    if (px) {
        rgba = unpack_rgba(px, w, h, fmt, type, 1);
        // internal formats without colour or alpha
        int n = w * h;
        if (ifmt == 0x1906 || ifmt == 0x803C) for (int i = 0; i < n; i++) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        else if (ifmt == 1 || ifmt == 0x1909 || ifmt == 0x8040) for (int i = 0; i < n; i++) rgba[i * 4 + 3] = 255;
        else if (ifmt == 3 || ifmt == 0x1907 || ifmt == 0x8051) for (int i = 0; i < n; i++) rgba[i * 4 + 3] = 255;
    } else rgba = calloc((size_t)w * h, 4);
    glBindTexture(GL_TEXTURE_2D, t->gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    t->w = w; t->h = h; t->dirty_params = 1;
    SRET(0, 9);
}

static void g_RasterPos2i(void) {
    double in[4] = {(int32_t)ARG(0), (int32_t)ARG(1), 0, 1}, c[4];
    xform(in, c);
    double w = c[3] ? c[3] : 1;
    rast_valid = fabs(c[0] / w) <= 1.0001 && fabs(c[1] / w) <= 1.0001;
    rast[0] = (c[0] / w * 0.5 + 0.5) * fb_w; rast[1] = (c[1] / w * 0.5 + 0.5) * fb_h;
    memcpy(rast_color, st.color, sizeof rast_color);
    SRET(0, 2);
}
static void g_Bitmap(void) {
    int w = (int)ARG(0), h = (int)ARG(1);
    float xo = AF(2), yo = AF(3), xm = AF(4), ym = AF(5);
    uint32_t bits = ARG(6);
    static int nlog;
    if (getenv("PORT_GLLOG") && w > 0 && nlog++ < 20) {
        host_log("glBitmap %dx%d orig %.1f,%.1f move %.1f,%.1f row %d align %d lsb %d rast %.1f,%.1f valid %d bytes",
                 w, h, xo, yo, xm, ym, unpack_row, unpack_align, unpack_lsb, rast[0], rast[1], rast_valid);
        for (int i = 0; i < 16; i++) host_log(" %02x", G[bits + i]);
        host_log("\n");
    }
    if (rast_valid && w > 0 && h > 0 && bits) {
        Glyph *g = glyph_get(w, h, bits);
        if (g) {
            double x = floor(rast[0] - xo + 0.0001), y = floor(rast[1] - yo + 0.0001);
            win_quad(x, y, w, h, atlas_tex, (float)g->x / ATLAS, (float)g->y / ATLAS,
                     (float)(g->x + w) / ATLAS, (float)(g->y + h) / ATLAS, rast_color, 4);
        }
    }
    rast[0] += xm; rast[1] += ym;
    SRET(0, 7);
}
static void g_DrawPixels(void) {
    int w = (int)ARG(0), h = (int)ARG(1); uint32_t fmt = ARG(2), type = ARG(3), px = ARG(4);
    logonce("glDrawPixels fmt", fmt | type << 16);
    if (rast_valid && w > 0 && h > 0 && px) {
        GLuint t = image_tex(px, w, h, fmt, type);
        float white[4] = {1, 1, 1, 1};
        win_quad(floor(rast[0] + 0.0001), floor(rast[1] + 0.0001), w, h, t, 0, 0, 1, 1, white, 5);
    }
    SRET(0, 5);
}
static void g_ReadPixels(void) {
    int x = (int)ARG(0), y = (int)ARG(1), w = (int)ARG(2), h = (int)ARG(3);
    uint32_t fmt = ARG(4), type = ARG(5), dst = ARG(6);
    logonce("glReadPixels fmt", fmt | type << 16);
    flush();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    uint8_t *px = malloc((size_t)w * h * 4 + 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int nc = comps(fmt);
    int stride = (w * nc + pack_align - 1) / pack_align * pack_align;
    COW(dst, stride * h);
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            uint8_t *s = px + ((size_t)yy * w + xx) * 4, *o = G + dst + yy * stride + xx * nc;
            switch (fmt) {
            case 0x1907: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; break;
            case 0x80E0: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; break;
            case 0x80E1: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3]; break;
            case 0x1909: o[0] = (uint8_t)((s[0] + s[1] + s[2]) / 3); break;
            default: memcpy(o, s, nc > 4 ? 4 : nc);
            }
        }
    free(px);
    SRET(0, 7);
}
static void glu_ErrorString(void) {
    static uint32_t s;
    if (!s) { s = guest_static_alloc(16); strcpy(GSTR(s), "no error"); }
    SRET(s, 1);
}
// gluScaleImage(format, wi, hi, typein, datain, wo, ho, typeout, dataout): box/bilinear resample, unsigned bytes
static void glu_ScaleImage(void) {
    uint32_t fmt = ARG(0); int wi = ARG(1), hi = ARG(2); uint32_t din = ARG(4);
    int wo = ARG(5), ho = ARG(6); uint32_t dout = ARG(8);
    int nc = comps(fmt);
    if (ARG(3) != 0x1401 || ARG(7) != 0x1401) logonce("gluScaleImage type", ARG(3) | ARG(7) << 16);
    int si = (wi * nc + unpack_align - 1) / unpack_align * unpack_align;
    int so = (wo * nc + pack_align - 1) / pack_align * pack_align;
    COW(dout, so * ho);
    for (int y = 0; y < ho; y++)
        for (int x = 0; x < wo; x++) {
            // area average over the source box
            double x0 = (double)x * wi / wo, x1 = (double)(x + 1) * wi / wo;
            double y0 = (double)y * hi / ho, y1 = (double)(y + 1) * hi / ho;
            for (int k = 0; k < nc; k++) {
                double acc = 0, wsum = 0;
                for (int sy = (int)y0; sy < (int)ceil(y1) && sy < hi; sy++) {
                    double wy = fmin(y1, sy + 1) - fmax(y0, sy);
                    for (int sx = (int)x0; sx < (int)ceil(x1) && sx < wi; sx++) {
                        double wx = fmin(x1, sx + 1) - fmax(x0, sx);
                        acc += G[din + sy * si + sx * nc + k] * wx * wy; wsum += wx * wy;
                    }
                }
                G[dout + y * so + x * nc + k] = (uint8_t)(wsum > 0 ? acc / wsum + 0.5 : 0);
            }
        }
    SRET(0, 9);
}

const ShimDef shims_gl[] = {
    {"OPENGL32", "glBegin", g_Begin}, {"OPENGL32", "glEnd", g_End},
    {"OPENGL32", "glVertex2i", g_Vertex2i}, {"OPENGL32", "glVertex2f", g_Vertex2f},
    {"OPENGL32", "glVertex2d", g_Vertex2d}, {"OPENGL32", "glVertex3i", g_Vertex3i},
    {"OPENGL32", "glTexCoord2f", g_TexCoord2f}, {"OPENGL32", "glColor3f", g_Color3f},
    {"OPENGL32", "glColor4f", g_Color4f}, {"OPENGL32", "glColor3d", g_Color3d}, {"OPENGL32", "glColor3ub", g_Color3ub},
    {"OPENGL32", "glMatrixMode", g_MatrixMode}, {"OPENGL32", "glLoadIdentity", g_LoadIdentity},
    {"OPENGL32", "glPushMatrix", g_PushMatrix}, {"OPENGL32", "glPopMatrix", g_PopMatrix},
    {"OPENGL32", "glOrtho", g_Ortho}, {"OPENGL32", "glTranslatef", g_Translatef},
    {"OPENGL32", "glEnable", g_Enable}, {"OPENGL32", "glDisable", g_Disable},
    {"OPENGL32", "glBlendFunc", g_BlendFunc}, {"OPENGL32", "glScissor", g_Scissor},
    {"OPENGL32", "glLineWidth", g_LineWidth}, {"OPENGL32", "glLineStipple", g_LineStipple},
    {"OPENGL32", "glHint", g_Hint}, {"OPENGL32", "glFinish", g_Finish}, {"OPENGL32", "glClearColor", g_ClearColor},
    {"OPENGL32", "glTexEnvf", g_TexEnvf}, {"OPENGL32", "glPushAttrib", g_PushAttrib}, {"OPENGL32", "glPopAttrib", g_PopAttrib},
    {"OPENGL32", "glPixelStorei", g_PixelStorei}, {"OPENGL32", "glPixelTransferi", g_PixelTransferi},
    {"OPENGL32", "glGetIntegerv", g_GetIntegerv}, {"OPENGL32", "glGetBooleanv", g_GetBooleanv},
    {"OPENGL32", "glGenTextures", g_GenTextures}, {"OPENGL32", "glBindTexture", g_BindTexture},
    {"OPENGL32", "glTexParameteri", g_TexParameteri}, {"OPENGL32", "glTexImage2D", g_TexImage2D},
    {"OPENGL32", "glRasterPos2i", g_RasterPos2i}, {"OPENGL32", "glBitmap", g_Bitmap},
    {"OPENGL32", "glDrawPixels", g_DrawPixels}, {"OPENGL32", "glReadPixels", g_ReadPixels},
    {"GLU32", "gluPerspective", glu_Perspective}, {"GLU32", "gluErrorString", glu_ErrorString},
    {"GLU32", "gluScaleImage", glu_ScaleImage},
    {0, 0, 0}
};
