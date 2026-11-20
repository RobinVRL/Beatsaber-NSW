/* Guest EGL/GLES bridge: original Unity GLES3 renderer on Switch Mesa.
 * SPDX-License-Identifier: MIT
 *
 * Unity links libEGL only and resolves every GLES entry through
 * eglGetProcAddress or dlsym. Both AArch64 ABIs are AAPCS64 and Mesa's
 * thread state uses libnx's TPIDRRO-based thread pointer, so host Mesa entry
 * points can be handed to guest code directly. EGL entry points are wrapped
 * to translate the Android-only pieces:
 *  - EGLNativeWindowType is the libnx NWindow that the GameActivity glue
 *    published as ANativeWindow;
 *  - Android-only config attributes (RECORDABLE/FRAMEBUFFER_TARGET) are
 *    dropped instead of failing eglChooseConfig with EGL_BAD_ATTRIBUTE;
 *  - each first call is logged so a failing step is visible in loader.log.
 */
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdint.h>
#include <string.h>

#include "egl_bridge.h"
#ifndef GL_FRAMEBUFFER_ATTACHMENT_LAYERED_EXT
#define GL_FRAMEBUFFER_ATTACHMENT_LAYERED_EXT 0x8DA7
#endif
#include "so_util.h"
#include "util.h"

#define BS_EGL_RECORDABLE_ANDROID 0x3142
#define BS_EGL_FRAMEBUFFER_TARGET_ANDROID 0x3147
#define BS_EGL_MAX_ATTRIBS 64

static int g_logged;
#define BS_EGL_LOG(...) do { if (g_logged < 400) { ++g_logged; debugPrintf(__VA_ARGS__); } } while (0)

static const char *caller_module(void *caller, unsigned long *offset) {
    so_module *m = so_find_module_by_addr(caller);
    *offset = (unsigned long)(m ? (uintptr_t)caller - (uintptr_t)m->load_virtbase
                                : (uintptr_t)caller);
    return m ? m->name : "host";
}

static void copy_attribs(const EGLint *in, EGLint *out, size_t capacity,
                         const char *what) {
    size_t n = 0;
    for (; in && in[0] != EGL_NONE && n + 3 <= capacity; in += 2) {
        if (in[0] == BS_EGL_RECORDABLE_ANDROID ||
            in[0] == BS_EGL_FRAMEBUFFER_TARGET_ANDROID)
            continue;
        BS_EGL_LOG("[egl]   %s attrib 0x%04x = 0x%x\n", what, in[0], in[1]);
        out[n++] = in[0];
        out[n++] = in[1];
    }
    out[n] = EGL_NONE;
}

static EGLDisplay bs_eglGetDisplay(EGLNativeDisplayType native) {
    unsigned long off;
    const char *mod = caller_module(__builtin_return_address(0), &off);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    BS_EGL_LOG("[egl] eglGetDisplay(%p) from %s+0x%lx -> %p\n",
               (void *)native, mod, off, d);
    return d;
}

static EGLBoolean bs_eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor) {
    EGLBoolean ok = eglInitialize(d, major, minor);
    BS_EGL_LOG("[egl] eglInitialize -> %d (%d.%d) err=0x%x\n", ok,
               major ? *major : -1, minor ? *minor : -1, ok ? 0 : eglGetError());
    if (ok) {
        BS_EGL_LOG("[egl] vendor=%s version=%s apis=%s\n",
                   eglQueryString(d, EGL_VENDOR), eglQueryString(d, EGL_VERSION),
                   eglQueryString(d, EGL_CLIENT_APIS));
    }
    return ok;
}

static EGLBoolean bs_eglChooseConfig(EGLDisplay d, const EGLint *attribs,
                                     EGLConfig *configs, EGLint size,
                                     EGLint *count) {
    EGLint filtered[BS_EGL_MAX_ATTRIBS];
    copy_attribs(attribs, filtered, BS_EGL_MAX_ATTRIBS, "config");
    EGLBoolean ok = eglChooseConfig(d, filtered, configs, size, count);
    BS_EGL_LOG("[egl] eglChooseConfig(size=%d) -> %d count=%d err=0x%x\n",
               size, ok, count ? *count : -1, ok ? 0 : eglGetError());
    return ok;
}

static EGLContext bs_eglCreateContext(EGLDisplay d, EGLConfig config,
                                      EGLContext share, const EGLint *attribs) {
    EGLint filtered[BS_EGL_MAX_ATTRIBS];
    copy_attribs(attribs, filtered, BS_EGL_MAX_ATTRIBS, "context");
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLContext c = eglCreateContext(d, config, share, filtered);
    if (!c) {
        /* OunceVK's Zink (Switch 2) may stop at ES 3.1: retry a 3.2 request
         * as 3.1. Unity reads the version it got from GL_VERSION. */
        for (size_t i = 0; filtered[i] != EGL_NONE; i += 2) {
            if (filtered[i] == EGL_CONTEXT_MINOR_VERSION && filtered[i + 1] > 1) {
                BS_EGL_LOG("[egl] ES 3.%d context refused (0x%x), retrying 3.1\n",
                           filtered[i + 1], eglGetError());
                filtered[i + 1] = 1;
                c = eglCreateContext(d, config, share, filtered);
                break;
            }
        }
    }
    BS_EGL_LOG("[egl] eglCreateContext(share=%p) -> %p err=0x%x\n",
               share, c, c ? 0 : eglGetError());
    return c;
}

#define BS_EGL_MAX_WINDOWS 8
static struct { EGLSurface surface; NWindow *window; } g_windows[BS_EGL_MAX_WINDOWS];
static size_t g_window_next;

static EGLSurface bs_eglCreateWindowSurface(EGLDisplay d, EGLConfig config,
                                            EGLNativeWindowType window,
                                            const EGLint *attribs) {
    EGLint filtered[BS_EGL_MAX_ATTRIBS];
    copy_attribs(attribs, filtered, BS_EGL_MAX_ATTRIBS, "window");
    NWindow *nw = (NWindow *)window;
    if (!nw || !nwindowIsValid(nw)) {
        BS_EGL_LOG("[egl] eglCreateWindowSurface: %p is not a live NWindow\n",
                   (void *)window);
        return EGL_NO_SURFACE;
    }
    EGLSurface s = eglCreateWindowSurface(d, config, window, filtered);
    if (s) {
        size_t slot = BS_EGL_MAX_WINDOWS;
        for (size_t i = 0; i < BS_EGL_MAX_WINDOWS; ++i) {
            if (g_windows[i].surface == s || !g_windows[i].surface) { slot = i; break; }
        }
        if (slot == BS_EGL_MAX_WINDOWS) slot = g_window_next++ % BS_EGL_MAX_WINDOWS;
        g_windows[slot].surface = s;
        g_windows[slot].window = nw;
    }
    BS_EGL_LOG("[egl] eglCreateWindowSurface(%p) -> %p err=0x%x\n",
               (void *)window, s, s ? 0 : eglGetError());
    return s;
}

static EGLBoolean bs_eglDestroySurface(EGLDisplay d, EGLSurface s) {
    for (size_t i = 0; i < BS_EGL_MAX_WINDOWS; ++i)
        if (g_windows[i].surface == s) g_windows[i].surface = NULL;
    return eglDestroySurface(d, s);
}

static EGLSurface bs_eglCreatePbufferSurface(EGLDisplay d, EGLConfig config,
                                             const EGLint *attribs) {
    EGLint filtered[BS_EGL_MAX_ATTRIBS];
    copy_attribs(attribs, filtered, BS_EGL_MAX_ATTRIBS, "pbuffer");
    EGLSurface s = eglCreatePbufferSurface(d, config, filtered);
    BS_EGL_LOG("[egl] eglCreatePbufferSurface -> %p err=0x%x\n", s,
               s ? 0 : eglGetError());
    return s;
}

static EGLBoolean bs_eglMakeCurrent(EGLDisplay d, EGLSurface draw,
                                    EGLSurface read, EGLContext c) {
    EGLBoolean ok = eglMakeCurrent(d, draw, read, c);
    static int logged;
    if (logged < 16 || !ok) {
        ++logged;
        u64 tid = 0;
        svcGetThreadId(&tid, CUR_THREAD_HANDLE);
        BS_EGL_LOG("[egl] eglMakeCurrent(%p,%p,%p) tid=%llu -> %d err=0x%x\n",
                   draw, read, c, (unsigned long long)tid, ok, ok ? 0 : eglGetError());
        if (ok && c && logged == 1) {
            BS_EGL_LOG("[gles] GL_VENDOR=%s GL_RENDERER=%s GL_VERSION=%s GLSL=%s\n",
                       (const char *)glGetString(GL_VENDOR),
                       (const char *)glGetString(GL_RENDERER),
                       (const char *)glGetString(GL_VERSION),
                       (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
        }
    }
    return ok;
}

static EGLSurface bs_eglGetCurrentSurface(EGLint which) {
    EGLSurface s = eglGetCurrentSurface(which);
    static int logged;
    if (logged < 12) {
        ++logged;
        u64 tid = 0;
        svcGetThreadId(&tid, CUR_THREAD_HANDLE);
        BS_EGL_LOG("[egl] eglGetCurrentSurface(%#x) tid=%llu -> %p\n", which,
                   (unsigned long long)tid, s);
    }
    return s;
}

static EGLContext bs_eglGetCurrentContext(void) {
    EGLContext c = eglGetCurrentContext();
    static int logged;
    if (logged < 12) {
        ++logged;
        u64 tid = 0;
        svcGetThreadId(&tid, CUR_THREAD_HANDLE);
        BS_EGL_LOG("[egl] eglGetCurrentContext tid=%llu -> %p\n",
                   (unsigned long long)tid, c);
    }
    return c;
}

static EGLBoolean bs_eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr,
                                     EGLint *value) {
    EGLBoolean ok = eglQuerySurface(d, s, attr, value);
    /* Switch Mesa only learns a window surface's size once it dequeues the
     * first buffer, so EGL_WIDTH/HEIGHT read back 0 before the first swap.
     * Unity then believes a resize is pending forever and never presents.
     * Report the backing NWindow's dimensions instead. */
    if (ok && value && *value <= 0 && (attr == EGL_WIDTH || attr == EGL_HEIGHT)) {
        for (size_t i = 0; i < BS_EGL_MAX_WINDOWS; ++i) {
            if (g_windows[i].surface != s || !g_windows[i].window) continue;
            u32 w = 0, h = 0;
            if (R_FAILED(nwindowGetDimensions(g_windows[i].window, &w, &h)) || !w || !h) {
                w = 1280;
                h = 720;
            }
            *value = attr == EGL_WIDTH ? (EGLint)w : (EGLint)h;
            break;
        }
    }
    static int logged;
    if (logged < 24) {
        ++logged;
        BS_EGL_LOG("[egl] eglQuerySurface(%p, %#x) -> %d value=%d err=0x%x\n", s,
                   attr, ok, value ? *value : -1, ok ? 0 : eglGetError());
    }
    return ok;
}

static uint64_t g_swaps;
static int g_trace_frame;
static unsigned long g_mv_draws, g_mv_shaders, g_mono_in_mv_draws;
/* Default-framebuffer activity since the last swap (black-screen probe). */
static __thread GLuint t_draw_fbo, t_read_fbo;
static unsigned long g_fb0_draws, g_fb0_clears, g_fb0_blits, g_blits, g_frame_draws;
static GLfloat g_clear_rgba[4];
static void probe_backbuffer(uint64_t n) {
    GLint prev_read = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    GLint vp[4] = {0};
    glGetIntegerv(GL_VIEWPORT, vp);
    static const int pts[5][2] = {{960,540},{100,100},{1800,100},{100,980},{1800,980}};
    unsigned char px[5][4] = {{0}};
    for (int i = 0; i < 5; ++i)
        glReadPixels(pts[i][0], pts[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[i]);
    GLenum err = glGetError();
    for (GLuint fb = 1; fb <= 6; ++fb) {
        if (!glIsFramebuffer(fb)) continue;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
        GLenum st = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
        unsigned char c[4] = {0};
        GLint w = 0, h = 0, type = 0;
        glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (st == GL_FRAMEBUFFER_COMPLETE)
            glReadPixels(64, 64, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        (void)w; (void)h;
        debugPrintf("[gles]   fbo %u status=%#x color-type=%#x px(64,64)=%02x%02x%02x%02x err=0x%x\n",
                    fb, st, type, c[0], c[1], c[2], c[3], glGetError());
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
    struct mallinfo mi = mallinfo();
    extern size_t bs_mmap_committed_mib(void), bs_mmap_pool_mib(void);
    debugPrintf("[gles]   malloc arena=%zu MiB free=%zu MiB; mmap pool %zu/%zu MiB\n",
                (size_t)mi.arena >> 20, (size_t)mi.fordblks >> 20,
                bs_mmap_committed_mib(), bs_mmap_pool_mib());
    debugPrintf("[gles] multiview: shaders rewritten=%lu draws=%lu mono-program draws in mv fbo=%lu\n",
                g_mv_shaders, g_mv_draws, g_mono_in_mv_draws);
    debugPrintf("[gles] frame %llu all-draws=%lu fb0: draws=%lu clears=%lu blits-to-fb0=%lu blits=%lu "
                "clear=(%.2f %.2f %.2f %.2f) vp=%d,%d,%d,%d px=%02x%02x%02x %02x%02x%02x "
                "%02x%02x%02x %02x%02x%02x %02x%02x%02x glerr=0x%x\n",
                (unsigned long long)n, g_frame_draws, g_fb0_draws, g_fb0_clears, g_fb0_blits, g_blits,
                g_clear_rgba[0], g_clear_rgba[1], g_clear_rgba[2], g_clear_rgba[3],
                vp[0], vp[1], vp[2], vp[3],
                px[0][0], px[0][1], px[0][2], px[1][0], px[1][1], px[1][2],
                px[2][0], px[2][1], px[2][2], px[3][0], px[3][1], px[3][2],
                px[4][0], px[4][1], px[4][2], err);
}
static EGLBoolean bs_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    uint64_t n0 = g_swaps + 1;
    if (n0 == 10 || n0 == 100 || n0 % 1000 == 0) probe_backbuffer(n0);
    g_fb0_draws = g_fb0_clears = g_fb0_blits = g_blits = g_frame_draws = 0;
    {
        static int err_logs;
        GLenum e = err_logs < 16 ? glGetError() : GL_NO_ERROR;
        if (e != GL_NO_ERROR) {
            ++err_logs;
            debugPrintf("[gles] GL error 0x%x pending at frame %llu\n", e,
                        (unsigned long long)n0);
        }
    }
    EGLBoolean ok = eglSwapBuffers(d, s);
    uint64_t n = ++g_swaps;
    static long trace_at = -1;
    if (trace_at < 0) {
        trace_at = 1500;
        FILE *tf = fopen("sdmc:/switch/beat-saber-quest/trace_frame.txt", "r");
        if (tf) { if (fscanf(tf, "%ld", &trace_at) != 1) trace_at = 1500; fclose(tf); }
    }
    g_trace_frame = (long)n == trace_at;
    if (g_trace_frame) debugPrintf("[gles:trace] ---- frame %llu ----\n", (unsigned long long)n + 1);
    if (n == 1 || n == 10 || n == 100 || n % 1000 == 0 || !ok)
        debugPrintf("[gles] eglSwapBuffers frame=%llu -> %d err=0x%x\n",
                    (unsigned long long)n, ok, ok ? 0 : eglGetError());
    return ok;
}

uint64_t bs_egl_frames_presented(void) { return g_swaps; }

/* XR compositor present: draw one eye of a swapchain texture to the window
 * surface and swap. Called on Unity's render thread from xrEndFrame with its
 * GLES context current; all touched GL state is restored.
 *
 * A shader quad is used instead of glBlitFramebuffer: the eye texture is
 * SRGB8_ALPHA8 and must reach the (linear) window surface with its encoded
 * bytes unchanged, which a sampler with SKIP_DECODE gives exactly. Mesa's
 * scaled blit from a layered sRGB source produced a flat colour. */
#define BS_GL_TEXTURE_SRGB_DECODE_EXT 0x8A48
#define BS_GL_SKIP_DECODE_EXT 0x8A4A
#define BS_GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#define BS_PRESENT_UNIT 0
static const char g_present_vs[] =
    "#version 300 es\n"
    "uniform vec4 u_rect;\n"
    "out highp vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    v_uv = u_rect.xy + p * u_rect.zw;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char g_present_fs_2d[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "in highp vec2 v_uv;\n"
    "out vec4 o;\n"
    "void main() { o = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n";
static const char g_present_fs_arr[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "precision mediump sampler2DArray;\n"
    "uniform sampler2DArray u_tex;\n"
    "uniform float u_layer;\n"
    "in highp vec2 v_uv;\n"
    "out vec4 o;\n"
    "void main() { o = vec4(texture(u_tex, vec3(v_uv, u_layer)).rgb, 1.0); }\n";

typedef struct { GLuint prog; GLint rect, layer, tex; } BsPresentProg;
static EGLContext g_present_ctx;
static BsPresentProg g_present[2];
static GLuint g_present_vao, g_present_sampler;

static GLuint present_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {0};
        glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
        debugPrintf("[xr:present] shader compile failed: %s\n", log);
    }
    return s;
}
static int present_init(EGLContext c) {
    if (g_present_ctx == c && g_present[0].prog) return 1;
    g_present_ctx = c;
    GLuint vs = present_shader(GL_VERTEX_SHADER, g_present_vs);
    for (int i = 0; i < 2; ++i) {
        GLuint fs = present_shader(GL_FRAGMENT_SHADER, i ? g_present_fs_arr : g_present_fs_2d);
        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[256] = {0};
            glGetProgramInfoLog(p, sizeof log - 1, NULL, log);
            debugPrintf("[xr:present] link failed: %s\n", log);
            glDeleteProgram(p);
            p = 0;
        }
        g_present[i].prog = p;
        g_present[i].rect = p ? glGetUniformLocation(p, "u_rect") : -1;
        g_present[i].layer = p ? glGetUniformLocation(p, "u_layer") : -1;
        g_present[i].tex = p ? glGetUniformLocation(p, "u_tex") : -1;
    }
    glDeleteShader(vs);
    glGenVertexArrays(1, &g_present_vao);
    glGenSamplers(1, &g_present_sampler);
    glSamplerParameteri(g_present_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(g_present_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(g_present_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_present_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_present_sampler, BS_GL_TEXTURE_SRGB_DECODE_EXT, BS_GL_SKIP_DECODE_EXT);
    glGetError();
    return g_present[0].prog && g_present[1].prog;
}

int bs_egl_present_texture(unsigned tex, int layer, int is_array,
                           int sx, int sy, int sw, int sh, int tw, int th) {
    EGLDisplay d = eglGetCurrentDisplay();
    EGLContext c = eglGetCurrentContext();
    if (!d || !c) return -1;
    EGLSurface win = EGL_NO_SURFACE;
    for (size_t i = 0; i < BS_EGL_MAX_WINDOWS; ++i)
        if (g_windows[i].surface) win = g_windows[i].surface;
    if (!win) return -2;
    EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
    if (prev_draw != win && !eglMakeCurrent(d, win, win, c)) return -3;
    if (!present_init(c)) {
        if (prev_draw != win) eglMakeCurrent(d, prev_draw, prev_read, c);
        return -5;
    }

    /* Save state. */
    GLint dr = 0, vp[4], prog = 0, vao = 0, active = 0, bound_tex = 0, sampler = 0;
    GLboolean mask[4], depth_mask;
    static const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_STENCIL_TEST,
                                  GL_SCISSOR_TEST, GL_RASTERIZER_DISCARD,
                                  GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE,
                                  GL_SAMPLE_COVERAGE, BS_GL_FRAMEBUFFER_SRGB_EXT};
    GLboolean cap_on[sizeof caps / sizeof caps[0]];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dr);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; ++i) {
        cap_on[i] = glIsEnabled(caps[i]);
        if (cap_on[i]) glDisable(caps[i]);
    }
    glActiveTexture(GL_TEXTURE0 + BS_PRESENT_UNIT);
    GLenum target = is_array ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    glGetIntegerv(is_array ? GL_TEXTURE_BINDING_2D_ARRAY : GL_TEXTURE_BINDING_2D, &bound_tex);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glGetError();

    EGLint ww = 0, wh = 0;
    bs_eglQuerySurface(d, win, EGL_WIDTH, &ww);
    bs_eglQuerySurface(d, win, EGL_HEIGHT, &wh);
    /* Letterbox to keep the eye's aspect ratio. */
    int dw = ww, dh = sw > 0 ? (int)((long long)ww * sh / sw) : wh;
    if (dh > wh) { dh = wh; dw = sh > 0 ? (int)((long long)wh * sw / sh) : ww; }
    int dx = (ww - dw) / 2, dy = (wh - dh) / 2;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (dw != ww || dh != wh) {
        glViewport(0, 0, ww, wh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(g_clear_rgba[0], g_clear_rgba[1], g_clear_rgba[2], g_clear_rgba[3]);
    }
    glViewport(dx, dy, dw, dh);
    BsPresentProg *pp = &g_present[is_array ? 1 : 0];
    glUseProgram(pp->prog);
    glBindVertexArray(g_present_vao);
    glBindTexture(target, tex);
    glBindSampler(BS_PRESENT_UNIT, g_present_sampler);
    glUniform1i(pp->tex, BS_PRESENT_UNIT);
    if (tw <= 0) tw = sx + sw;
    if (th <= 0) th = sy + sh;
    glUniform4f(pp->rect, (float)sx / (float)tw, (float)sy / (float)th,
                (float)sw / (float)tw, (float)sh / (float)th);
    if (pp->layer >= 0) glUniform1f(pp->layer, (float)layer);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    {
        uint64_t n = g_swaps + 1;
        if (n == 10 || n == 300 || n % 1000 == 0) {
            unsigned char px[3][4] = {{0}};
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            for (int i = 0; i < 3; ++i)
                glReadPixels(dx + dw * (1 + i) / 4, dy + dh / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[i]);
            debugPrintf("[xr:present] frame %llu tex=%u layer=%d out: %02x%02x%02x %02x%02x%02x %02x%02x%02x\n",
                        (unsigned long long)n, tex, layer, px[0][0], px[0][1], px[0][2],
                        px[1][0], px[1][1], px[1][2], px[2][0], px[2][1], px[2][2]);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, t_read_fbo);
        }
    }

    /* Restore state. */
    glBindSampler(BS_PRESENT_UNIT, (GLuint)sampler);
    glBindTexture(target, (GLuint)bound_tex);
    glActiveTexture((GLenum)active);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; ++i)
        if (cap_on[i]) glEnable(caps[i]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glDepthMask(depth_mask);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dr);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    GLenum err = glGetError();
    EGLBoolean ok = bs_eglSwapBuffers(d, win);
    if (prev_draw != win)
        eglMakeCurrent(d, prev_draw, prev_read, c);
    static int logged;
    if ((logged < 4 || err) && logged < 40) {
        ++logged;
        debugPrintf("[xr:present] tex=%u layer=%d array=%d src=%dx%d -> %dx%d swap=%d glerr=0x%x\n",
                    tex, layer, is_array, sw, sh, dw, dh, ok, err);
    }
    return ok ? 0 : -4;
}

/* GL activity counters (watchdog): show whether the render thread issues
 * any work at all when nothing is presented. */
_Atomic unsigned long bs_gl_clears, bs_gl_draws, bs_gl_fbo_binds, bs_gl_flushes;

/* ---- GL_OVR_multiview2 emulation ----------------------------------------
 * The Quest build ships only multiview stereo shader variants. Mesa nouveau
 * has no OVR_multiview, and without it Unity picks single-pass instancing,
 * finds no instancing variants, and falls back to mono shaders (the final
 * array blit then samples the default grey texture). Keep Unity's layered
 * framebuffer layout and route emulated multiview shaders to layer zero.
 * The flat-screen OpenXR runtime presents only that eye. Duplicating each
 * instance across both layers produces stretched triangles in Eden. */
#define BS_GL_MAX_VIEWS_OVR 0x9631
#define BS_GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR 0x9630
#define BS_GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR 0x9632
#define BS_MV_MAX_FBO 8192
static unsigned char g_mv_fbo[BS_MV_MAX_FBO];
static __thread GLuint t_program;

typedef void (*BsFramebufferTextureFn)(GLenum, GLenum, GLuint, GLint);
static BsFramebufferTextureFn g_fb_texture;

static GLuint bound_fbo(GLenum target) {
    return target == GL_READ_FRAMEBUFFER ? t_read_fbo : t_draw_fbo;
}
static void mv_mark(GLenum target, GLenum attachment, int on) {
    if (attachment != GL_COLOR_ATTACHMENT0) return;
    GLuint f = bound_fbo(target);
    if (f && f < BS_MV_MAX_FBO) g_mv_fbo[f] = (unsigned char)on;
}
/* Per-object multiview state for framebuffer and shader diagnostics. */
#define BS_MV_MAX_IDS 65536
enum { BS_PROG_UNKNOWN = 0, BS_PROG_MV = 1, BS_PROG_MONO = 2 };
static unsigned char g_shader_mv[BS_MV_MAX_IDS];   /* 1 mv vertex, 2 mono vertex */
static unsigned char g_prog_mv[BS_MV_MAX_IDS];
static unsigned short g_prog_vs[BS_MV_MAX_IDS], g_prog_fs[BS_MV_MAX_IDS];
static unsigned short g_prog_fs_sh[BS_MV_MAX_IDS]; /* fragment shader object */
static unsigned short g_shader_seq[BS_MV_MAX_IDS];   /* dump file number */
static unsigned char g_shader_cut[BS_MV_MAX_IDS], g_prog_cut[BS_MV_MAX_IDS]; /* note-family VS */
/* Compute shaders that build the bloom pre-pass light quads. */
static unsigned char g_shader_light[BS_MV_MAX_IDS], g_prog_light[BS_MV_MAX_IDS];
static unsigned char g_shader_ui[BS_MV_MAX_IDS], g_prog_ui[BS_MV_MAX_IDS]; /* _ClipRect FS */
static unsigned char g_shader_note_body[BS_MV_MAX_IDS], g_prog_note_body[BS_MV_MAX_IDS];
static int prog_is_mono(GLuint p) {
    return p < BS_MV_MAX_IDS && g_prog_mv[p] == BS_PROG_MONO;
}
/* Whether the driver itself has GL_OVR_multiview2 (OunceVK's Zink may; Switch
 * 1 Mesa nouveau doesn't). Then every multiview call passes straight through
 * and none of the layered emulation below runs. Needs a current context. */
static int g_native_mv = -1;
static int native_mv(void) {
    if (g_native_mv >= 0) return g_native_mv;
    if (!eglGetCurrentContext()) return 0;
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    int found = 0;
    for (GLint i = 0; i < n && !found; ++i) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        found = e && !strcmp(e, "GL_OVR_multiview2");
    }
    g_native_mv = found;
    debugPrintf("[gles:mv] native GL_OVR_multiview2: %s\n",
                found ? "yes, passing through" : "no, emulating with layers");
    return found;
}
static int mv_active(void) {
    if (native_mv()) return 0;
    if (!(t_draw_fbo && t_draw_fbo < BS_MV_MAX_FBO && g_mv_fbo[t_draw_fbo])) return 0;
    if (prog_is_mono(t_program)) { ++g_mono_in_mv_draws; return 0; }
    return 1;
}
static void bs_glFramebufferTextureMultiviewOVR(GLenum target, GLenum attachment,
        GLuint texture, GLint level, GLint base_view, GLsizei num_views) {
    if (native_mv()) {
        typedef void (*Fn)(GLenum, GLenum, GLuint, GLint, GLint, GLsizei);
        static Fn native;
        if (!native) native = (Fn)eglGetProcAddress("glFramebufferTextureMultiviewOVR");
        if (native) native(target, attachment, texture, level, base_view, num_views);
        return;
    }
    if (!g_fb_texture)
        g_fb_texture = (BsFramebufferTextureFn)eglGetProcAddress("glFramebufferTexture");
    static int logged;
    if (logged < 8) {
        ++logged;
        debugPrintf("[gles:mv] fbo %u attach %#x tex %u level %d views %d+%d\n",
                    bound_fbo(target), attachment, texture, level, base_view, num_views);
    }
    if (texture && base_view == 0 && g_fb_texture) {
        g_fb_texture(target, attachment, texture, level);
    } else if (texture) {
        /* Non-zero base view: closest equivalent is that single layer. */
        glFramebufferTextureLayer(target, attachment, texture, level, base_view);
    } else {
        glFramebufferTextureLayer(target, attachment, 0, 0, 0);
    }
    mv_mark(target, attachment, texture != 0);
}
static void bs_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                                      GLuint texture, GLint level) {
    mv_mark(target, attachment, 0);
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
}
static void bs_glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture,
                                         GLint level, GLint layer) {
    mv_mark(target, attachment, 0);
    glFramebufferTextureLayer(target, attachment, texture, level, layer);
}
static void bs_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbt,
                                         GLuint rb) {
    mv_mark(target, attachment, 0);
    glFramebufferRenderbuffer(target, attachment, rbt, rb);
}
static void bs_glDeleteFramebuffers(GLsizei n, const GLuint *ids) {
    for (GLsizei i = 0; i < n; ++i)
        if (ids[i] < BS_MV_MAX_FBO) g_mv_fbo[ids[i]] = 0;
    glDeleteFramebuffers(n, ids);
}
static void bs_glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment,
                                                     GLenum pname, GLint *params) {
    if (native_mv()) {
        glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
        return;
    }
    GLuint f = bound_fbo(target);
    int mv = f && f < BS_MV_MAX_FBO && g_mv_fbo[f];
    if (pname == BS_GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR) { *params = mv ? 2 : 0; return; }
    if (pname == BS_GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR) { *params = 0; return; }
    glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
}

static const char *const g_extra_ext[] = {"GL_OVR_multiview", "GL_OVR_multiview2"};
#define BS_EXTRA_EXT 2
static GLint native_ext_count(void) {
    static GLint n = -1;
    if (n < 0) glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    return n;
}
static void bs_glGetIntegerv(GLenum pname, GLint *v) {
    if (native_mv()) { glGetIntegerv(pname, v); return; }
    if (pname == BS_GL_MAX_VIEWS_OVR) { *v = 2; return; }
    glGetIntegerv(pname, v);
    if (pname == GL_NUM_EXTENSIONS) *v += BS_EXTRA_EXT;
    if (pname == 0x8A30 /* GL_MAX_UNIFORM_BLOCK_SIZE */ || pname == 0x8B4A /* MAX_VERTEX_UNIFORM_COMPONENTS */ ||
        pname == 0x8DFB /* MAX_VERTEX_UNIFORM_VECTORS */ || pname == 0x8A2B /* MAX_VERTEX_UNIFORM_BLOCKS */) {
        static int n;
        if (n++ < 8) debugPrintf("[gles] glGetIntegerv(0x%x) = %d\n", pname, *v);
    }
}
static const GLubyte *bs_glGetStringi(GLenum name, GLuint index) {
    if (name == GL_EXTENSIONS && !native_mv()) {
        GLint n = native_ext_count();
        if ((GLint)index >= n && (GLint)index < n + BS_EXTRA_EXT)
            return (const GLubyte *)g_extra_ext[index - (GLuint)n];
    }
    return glGetStringi(name, index);
}
static const GLubyte *bs_glGetString(GLenum name) {
    if (name != GL_EXTENSIONS || native_mv()) return glGetString(name);
    static char *all;
    if (!all) {
        const char *base = (const char *)glGetString(GL_EXTENSIONS);
        size_t len = base ? strlen(base) : 0;
        all = malloc(len + 64);
        if (!all) return (const GLubyte *)base;
        snprintf(all, len + 64, "%s%s%s %s", base ? base : "", len ? " " : "",
                 g_extra_ext[0], g_extra_ext[1]);
    }
    return (const GLubyte *)all;
}

/* Replace every occurrence of `from` in `s` (malloc'd) by `to`. */
static char *str_replace_all(char *s, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to), count = 0;
    for (const char *p = s; (p = strstr(p, from)); p += fl) ++count;
    if (!count) return s;
    size_t out_len = strlen(s) + count * (tl > fl ? tl - fl : 0) + 1;
    char *out = malloc(out_len), *o = out;
    if (!out) return s;
    const char *p = s, *q;
    while ((q = strstr(p, from))) {
        memcpy(o, p, (size_t)(q - p)); o += q - p;
        memcpy(o, to, tl); o += tl;
        p = q + fl;
    }
    strcpy(o, p);
    free(s);
    return out;
}
/* Remove whole lines containing `needle`; returns s. */
static char *remove_lines(char *s, const char *needle) {
    char *p;
    while ((p = strstr(s, needle))) {
        char *start = p;
        while (start > s && start[-1] != '\n') --start;
        char *end = strchr(p, '\n');
        end = end ? end + 1 : p + strlen(p);
        memmove(start, end, strlen(end) + 1);
    }
    return s;
}
/* Insert `text` at the start of the line following the last #extension
 * (or #version) directive. */
static char *insert_after_directives(char *s, const char *text) {
    char *anchor = NULL, *p = s;
    while ((p = strstr(p, "#extension"))) { anchor = p; p += 10; }
    if (!anchor) anchor = strstr(s, "#version");
    char *at = anchor ? strchr(anchor, '\n') : NULL;
    at = at ? at + 1 : s;
    size_t head = (size_t)(at - s), tl = strlen(text);
    char *out = malloc(strlen(s) + tl + 1);
    if (!out) return s;
    memcpy(out, s, head);
    memcpy(out + head, text, tl);
    strcpy(out + head + tl, at);
    free(s);
    return out;
}
static char *mv_rewrite(const char *src) {
    if (native_mv() || !strstr(src, "GL_OVR_multiview")) return NULL;
    char *s = strdup(src);
    if (!s) return NULL;
    int vertex = strstr(s, "num_views") != NULL;
    s = remove_lines(s, "GL_OVR_multiview");
    s = remove_lines(s, "num_views");
    s = str_replace_all(s, "gl_ViewID_OVR", "bs_ViewID");
    if (vertex) {
        s = str_replace_all(s, "gl_InstanceID", "bs_InstanceID");
        s = insert_after_directives(s,
            "#extension GL_NV_viewport_array2 : require\n"
            "#define bs_ViewID 0u\n"
            "#define bs_InstanceID gl_InstanceID\n"
            "#define main bs_MultiviewMain\n");
        /* Set the eye layer before calling the original entry point, even
         * when its declaration spans lines or uses a void parameter list. */
        const char entry[] =
            "\n#undef main\n"
            "void main() {\n"
            "    gl_Layer = 0;\n"
            "    bs_MultiviewMain();\n"
            "}\n";
        size_t length = strlen(s);
        char *out = realloc(s, length + sizeof entry);
        if (!out) {
            free(s);
            return NULL;
        }
        memcpy(out + length, entry, sizeof entry);
        s = out;
    } else {
        int es32 = strstr(s, "#version 320 es") != NULL;
        s = insert_after_directives(s, es32
            ? "#define bs_ViewID uint(gl_Layer)\n"
            : "#extension GL_EXT_geometry_shader : enable\n#define bs_ViewID uint(gl_Layer)\n");
    }
    return s;
}

/* ---- Bloom pre-pass light-quad compute shader -> transform feedback ------
 * Beat Saber builds its bloom/fog light quads in a compute shader
 * (_LightData SSBO -> _VertexBuffer SSBO, 52 words per light). Under Eden the
 * SSBO stores of Mesa/nouveau compute shaders never reach memory, leaving the
 * vertex buffer stale -> NaN bloom texture -> black level, invisible glow.
 * We re-run the same code as a vertex shader (one point per light) and
 * capture the 52 words with transform feedback, with the light data copied
 * into a uniform buffer. Disable with sdmc:/switch/beat-saber-quest/no_light_tfb. */
#define BS_LIGHT_WORDS 52
#define BS_LIGHT_UBO_BYTES 16384
typedef struct {
    GLuint cs;           /* game's compute shader object */
    char *vs;            /* converted vertex shader source */
    int light_binding, vb_binding, local_size;
    GLuint prog;         /* replacement program, 0 = not built, ~0u = failed */
} BsLightCs;
static BsLightCs g_light_cs[16];
static int g_light_ubo_binding = -1;

/* Pointer to the ']' matching the '[' at p, or NULL. */
static const char *match_bracket(const char *p) {
    int depth = 0;
    for (; *p; ++p) {
        if (*p == '[') ++depth;
        else if (*p == ']' && --depth == 0) return p;
    }
    return NULL;
}
/* Remove the `layout(...) ... <decl> { ... };` block containing `decl`,
 * returning its binding number (or -1). */
static int remove_block(char *s, const char *decl) {
    char *p = strstr(s, decl);
    if (!p) return -1;
    char *start = p;
    while (start > s && start[-1] != '\n') --start;
    int binding = -1;
    const char *b = strstr(start, "binding");
    if (b && b < p) {
        b = strchr(b, '=');
        if (b) binding = atoi(b + 1);
    }
    char *end = strstr(p, "};");
    if (!end) return -1;
    char *nl = strchr(end, '\n');
    end = nl ? nl + 1 : end + strlen(end);
    memmove(start, end, strlen(end) + 1);
    return binding;
}
/* name[A] -> pre A post (and with `second`, name[A]second[B] -> pre A, B post). */
static char *rewrite_indexed(char *s, const char *name, const char *second,
                             const char *pre, const char *post) {
    size_t nl = strlen(name), sl = second ? strlen(second) : 0;
    size_t cap = strlen(s) * 2 + 4096, o = 0;
    char *out = malloc(cap);
    if (!out) return s;
    const char *p = s;
    for (;;) {
        const char *q = strstr(p, name);
        const char *a0 = q ? q + nl : NULL;
        const char *a1 = q && *a0 == '[' ? match_bracket(a0) : NULL;
        const char *b0 = NULL, *b1 = NULL;
        if (a1 && second) {
            if (strncmp(a1 + 1, second, sl) == 0 && a1[1 + sl] == '[') {
                b0 = a1 + 1 + sl;
                b1 = match_bracket(b0);
            }
            if (!b1) a1 = NULL;
        }
        if (!a1) {
            if (!q) break;
            /* Not an indexing use; copy through. */
            size_t l = (size_t)(q + nl - p);
            if (o + l + 1 > cap) break;
            memcpy(out + o, p, l); o += l; p = q + nl;
            continue;
        }
        const char *end = second ? b1 + 1 : a1 + 1;
        size_t need = (size_t)(q - p) + strlen(pre) + (size_t)(end - q) + strlen(post) + 8;
        if (o + need + 1 > cap) { free(out); return s; }
        memcpy(out + o, p, (size_t)(q - p)); o += (size_t)(q - p);
        o += (size_t)sprintf(out + o, "%s", pre);
        memcpy(out + o, a0 + 1, (size_t)(a1 - a0 - 1)); o += (size_t)(a1 - a0 - 1);
        if (second) {
            o += (size_t)sprintf(out + o, ", ");
            memcpy(out + o, b0 + 1, (size_t)(b1 - b0 - 1)); o += (size_t)(b1 - b0 - 1);
        }
        o += (size_t)sprintf(out + o, "%s", post);
        p = end;
    }
    size_t rest = strlen(p);
    if (o + rest + 1 > cap) { free(out); return s; }
    memcpy(out + o, p, rest + 1);
    free(s);
    return out;
}
static char *light_cs_to_vs(const char *src, int *light_binding, int *vb_binding, int *local_size) {
    char *s = strdup(src);
    if (!s) return NULL;
    const char *ls = strstr(s, "local_size_x");
    ls = ls ? strchr(ls, '=') : NULL;
    *local_size = ls ? atoi(ls + 1) : 64;
    remove_lines(s, "local_size_x");
    *light_binding = remove_block(s, "buffer _LightData");
    *vb_binding = remove_block(s, "buffer _VertexBuffer");
    if (*light_binding < 0 || *vb_binding < 0 || *local_size <= 0) { free(s); return NULL; }
    s = rewrite_indexed(s, "_LightData_buf", ".value", "bs_ld(", ")");
    s = rewrite_indexed(s, "_VertexBuffer_buf", NULL, "bs_vb[bs_rel(", ")]");
    if (strstr(s, "_LightData_buf") || strstr(s, "_VertexBuffer_buf[")) { free(s); return NULL; }
    s = str_replace_all(s, "gl_GlobalInvocationID", "bs_gid");
    s = str_replace_all(s, "void main()", "void bs_cs_main()");
    char head[512];
    snprintf(head, sizeof head,
             "uvec3 bs_gid;\n"
             "uint bs_vb[%d];\n"
             "#define bs_rel(i) clamp(int(i) - int(bs_gid.x) * %d, 0, %d)\n"
             "layout(std140, binding = %d) uniform bs_LightBlock { uvec4 bs_ldv[%d]; };\n"
             "#define bs_ld(l, i) bs_ldv[(int(l) * 36 + int(i)) >> 2][(int(l) * 36 + int(i)) & 3]\n",
             BS_LIGHT_WORDS, BS_LIGHT_WORDS, BS_LIGHT_WORDS - 1,
             g_light_ubo_binding, BS_LIGHT_UBO_BYTES / 16);
    s = insert_after_directives(s, head);
    size_t l = strlen(s);
    char *out = malloc(l + 2048);
    if (!out) { free(s); return NULL; }
    memcpy(out, s, l);
    size_t o = l;
    o += (size_t)sprintf(out + o, "\n");
    for (int i = 0; i < BS_LIGHT_WORDS / 4; ++i) o += (size_t)sprintf(out + o, "flat out uvec4 bs_o%d;\n", i);
    o += (size_t)sprintf(out + o,
                         "void main() {\n"
                         "    bs_gid = uvec3(uint(gl_VertexID), 0u, 0u);\n"
                         "    for (int i = 0; i < %d; ++i) bs_vb[i] = 0u;\n"
                         "    bs_cs_main();\n", BS_LIGHT_WORDS);
    for (int i = 0; i < BS_LIGHT_WORDS / 4; ++i)
        o += (size_t)sprintf(out + o, "    bs_o%d = uvec4(bs_vb[%d], bs_vb[%d], bs_vb[%d], bs_vb[%d]);\n",
                             i, i * 4, i * 4 + 1, i * 4 + 2, i * 4 + 3);
    sprintf(out + o, "    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n    gl_PointSize = 1.0;\n}\n");
    free(s);
    return out;
}
/* Returns entry index + 1 when `src` is the light compute shader, else 0. */
static int light_cs_register(GLuint sh, const char *src) {
    if (!strstr(src, "local_size_x") || !strstr(src, "_VertexBuffer_buf") ||
        !strstr(src, "_NumberOfLights") || !strstr(src, "_LightData_buf"))
        return 0;
    static int disabled = -1;
    if (disabled < 0) {
        FILE *f = fopen("sdmc:/switch/beat-saber-quest/no_light_tfb", "r");
        disabled = f != NULL;
        if (f) fclose(f);
    }
    if (disabled) return 0;
    if (g_light_ubo_binding < 0) {
        GLint max = 0;
        glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &max);
        g_light_ubo_binding = max > 1 ? max - 1 : 0;
    }
    int idx = -1;
    for (int i = 0; i < 16; ++i) if (g_light_cs[i].cs == sh) { idx = i; break; }
    if (idx < 0) for (int i = 0; i < 16; ++i) if (!g_light_cs[i].cs) { idx = i; break; }
    if (idx < 0) { static int rr; idx = rr++ & 15; }
    BsLightCs *e = &g_light_cs[idx];
    free(e->vs);
    if (e->prog && e->prog != ~0u) glDeleteProgram(e->prog);
    memset(e, 0, sizeof *e);
    e->cs = sh;
    e->vs = light_cs_to_vs(src, &e->light_binding, &e->vb_binding, &e->local_size);
    debugPrintf("[gles] light compute shader %u -> transform feedback: %s (light=%d vb=%d local=%d ubo=%d)\n",
                sh, e->vs ? "converted" : "FAILED", e->light_binding, e->vb_binding,
                e->local_size, g_light_ubo_binding);
    if (!e->vs) { e->cs = 0; return 0; }
    mkdir("sdmc:/switch/beat-saber-quest/shaderdump", 0777);
    char path[96];
    snprintf(path, sizeof path, "sdmc:/switch/beat-saber-quest/shaderdump/light_vs_%d.txt", idx);
    FILE *f = fopen(path, "w");
    if (f) { fputs(e->vs, f); fclose(f); }
    return idx + 1;
}
static GLuint light_build(BsLightCs *e) {
    if (e->prog) return e->prog == ~0u ? 0 : e->prog;
    static const char *fs_src = "#version 310 es\nprecision mediump float;\nout vec4 bs_c;\n"
                                "void main() { bs_c = vec4(0.0); }\n";
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    const GLchar *v = e->vs;
    glShaderSource(vs, 1, &v, NULL);
    glCompileShader(vs);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    static char names[BS_LIGHT_WORDS / 4][8];
    const GLchar *np[BS_LIGHT_WORDS / 4];
    for (int i = 0; i < BS_LIGHT_WORDS / 4; ++i) { snprintf(names[i], 8, "bs_o%d", i); np[i] = names[i]; }
    glTransformFeedbackVaryings(p, BS_LIGHT_WORDS / 4, np, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    GLint ok = 0, vok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &vok);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        if (!vok) glGetShaderInfoLog(vs, sizeof log, NULL, log);
        else glGetProgramInfoLog(p, sizeof log, NULL, log);
        debugPrintf("[gles] light transform-feedback program FAILED (vs=%d): %s\n", vok, log);
        glDeleteProgram(p);
        p = 0;
    } else {
        debugPrintf("[gles] light transform-feedback program %u ready\n", p);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    e->prog = p ? p : ~0u;
    return p;
}
static void query_ssbo(int binding, GLuint *buf, GLint64 *start, GLint64 *avail) {
    GLint b = 0;
    GLint64 st = 0, sz = 0, total = 0;
    glGetIntegeri_v(0x90D3 /* SHADER_STORAGE_BUFFER_BINDING */, (GLuint)binding, &b);
    glGetInteger64i_v(0x90D4 /* START */, (GLuint)binding, &st);
    glGetInteger64i_v(0x90D5 /* SIZE */, (GLuint)binding, &sz);
    if (b) {
        GLint prev = 0;
        glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &prev);
        glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)b);
        glGetBufferParameteri64v(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &total);
        glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)prev);
    }
    *buf = (GLuint)b;
    *start = st;
    *avail = sz > 0 ? sz : total - st;
}
/* Replace the light compute dispatch. Returns 1 when handled. */
static int light_dispatch(int entry, GLuint x, GLuint y, GLuint z) {
    BsLightCs *e = &g_light_cs[entry - 1];
    GLuint prog = light_build(e);
    if (!prog) return 0;
    GLuint vbuf = 0, lbuf = 0;
    GLint64 vstart = 0, vavail = 0, lstart = 0, lavail = 0;
    query_ssbo(e->vb_binding, &vbuf, &vstart, &vavail);
    query_ssbo(e->light_binding, &lbuf, &lstart, &lavail);
    if (!vbuf || !lbuf || vavail <= 0 || lavail <= 0) return 0;
    GLint64 points = (GLint64)x * y * z * e->local_size;
    if (points > vavail / (BS_LIGHT_WORDS * 4)) points = vavail / (BS_LIGHT_WORDS * 4);
    if (points <= 0) return 1;

    static GLuint ubo, vao;
    GLint prev_read = 0, prev_write = 0, prev_ubo = 0, prev_vao = 0, prev_tfb = 0;
    glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &prev_write);
    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &prev_ubo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, &prev_tfb);
    GLboolean discard = glIsEnabled(GL_RASTERIZER_DISCARD);
    if (!ubo) {
        glGenBuffers(1, &ubo);
        glBindBuffer(GL_COPY_WRITE_BUFFER, ubo);
        glBufferData(GL_COPY_WRITE_BUFFER, BS_LIGHT_UBO_BYTES, NULL, GL_DYNAMIC_COPY);
        glGenVertexArrays(1, &vao);
    }
    glBindBuffer(GL_COPY_READ_BUFFER, lbuf);
    glBindBuffer(GL_COPY_WRITE_BUFFER, ubo);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)lstart, 0,
                        (GLsizeiptr)(lavail < BS_LIGHT_UBO_BYTES ? lavail : BS_LIGHT_UBO_BYTES));
    glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)g_light_ubo_binding, ubo);

    glUseProgram(prog);
    glBindVertexArray(vao);
    glBindBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, vbuf, (GLintptr)vstart,
                      (GLsizeiptr)(points * BS_LIGHT_WORDS * 4));
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, (GLsizei)points);
    glEndTransformFeedback();
    if (!discard) glDisable(GL_RASTERIZER_DISCARD);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, (GLuint)prev_tfb);
    glBindVertexArray((GLuint)prev_vao);
    glUseProgram(t_program);
    glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)prev_ubo);
    glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)prev_read);
    glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)prev_write);
    static int logged;
    if (logged < 3) {
        ++logged;
        debugPrintf("[gles] light dispatch %u,%u,%u -> %lld points via transform feedback err=0x%x\n",
                    x, y, z, (long long)points, glGetError());
    }
    return 1;
}

/* hlslcc indexes instanced arrays as `Arr[u_xlatiN.c / 8]`. nouveau lowers
 * signed division by a constant to IMAD.HI, which Eden's shader recompiler
 * does not implement ("IMAD (reg) is not implemented"): the whole pipeline
 * is dropped, so notes and sabers never draw. Instance indices are never
 * negative, so `[x / 2^k]` becomes `[(x) >> k]`. */
static char *idx_div_to_shift(char *s, int *changed) {
    size_t len = strlen(s);
    char *out = malloc(len * 3 + 64); /* a rewrite at most triples its >=13-byte input */
    if (!out) return s;
    const char *p = s;
    char *o = out;
    while (*p) {
        if (*p == '[') {
            const char *q = p + 1, *id = q;
            while (*q && (isalnum((unsigned char)*q) || *q == '_' || *q == '.')) ++q;
            const char *id_end = q;
            if (id_end > id && strncmp(id, "u_xlati", 7) == 0 && strncmp(q, " / ", 3) == 0) {
                q += 3;
                char *e;
                long d = strtol(q, &e, 10);
                if (e > q && *e == ']' && d > 1 && (d & (d - 1)) == 0) {
                    int k = 0;
                    while ((1L << k) < d) ++k;
                    o += sprintf(o, "[(%.*s >> %d)]", (int)(id_end - id), id, k);
                    p = e + 1;
                    ++*changed;
                    continue;
                }
                if (e > q && *e == ']' && d > 2 && d < 64) {
                    /* Non-power-of-two struct stride: x is a multiple of d,
                     * so a float divide is exact. */
                    o += sprintf(o, "[int((float(%.*s) + 0.5) / %ld.0)]", (int)(id_end - id), id, d);
                    p = e + 1;
                    ++*changed;
                    continue;
                }
            }
        }
        *o++ = *p++;
    }
    *o = '\0';
    free(s);
    return out;
}

/* std140 size of `struct <type> { ... };` in s, or 0 if not found. */
static size_t std140_struct_size(const char *s, const char *type, size_t tl) {
    const char *p = s;
    while ((p = strstr(p, "struct "))) {
        p += 7;
        if (strncmp(p, type, tl) || (p[tl] != ' ' && p[tl] != '{' && p[tl] != '\n')) continue;
        const char *b = strchr(p, '{'), *e = b ? strstr(b, "};") : NULL;
        if (!b || !e) return 0;
        size_t off = 0;
        for (const char *ln = b + 1; ln < e;) {
            const char *nl = memchr(ln, '\n', (size_t)(e - ln));
            const char *end = nl ? nl : e;
            char buf[160];
            size_t n = (size_t)(end - ln) < sizeof buf - 1 ? (size_t)(end - ln) : sizeof buf - 1;
            memcpy(buf, ln, n); buf[n] = '\0';
            ln = nl ? nl + 1 : e;
            if (!strchr(buf, ';')) continue;
            /* type is the last type-like word before the member name. */
            size_t al = 4, sz = 4;
            if (strstr(buf, "mat4")) { al = 16; sz = 64; }
            else if (strstr(buf, "mat3")) { al = 16; sz = 48; }
            else if (strstr(buf, "vec4")) { al = 16; sz = 16; }
            else if (strstr(buf, "vec3")) { al = 16; sz = 12; }
            else if (strstr(buf, "vec2")) { al = 8; sz = 8; }
            const char *br = strchr(buf, '[');
            if (br) { /* arrays: every element aligned/strided to 16 */
                long cnt = strtol(br + 1, NULL, 10);
                if (cnt <= 0) return 0;
                size_t stride = sz > 16 ? (sz + 15) & ~(size_t)15 : 16;
                al = 16; sz = stride * (size_t)cnt;
            }
            off = (off + al - 1) & ~(al - 1);
            off += sz;
        }
        return (off + 15) & ~(size_t)15;
    }
    return 0;
}
/* Unity patches UNITY_RUNTIME_INSTANCING_ARRAY_SIZE into a padded slot
 * ("[2<34 spaces>]") but leaves 2 here, while issuing draws of up to ~70
 * instances; instances past the second then read out of bounds (far notes
 * collapse, lasers get garbage widths). Size each array to fill the 64 KiB
 * uniform block instead. */
static char *fix_instancing_arrays(char *s, int *fixed) {
    static const char pad[] = "[2          ";
    char *p = s;
    while ((p = strstr(p, pad))) {
        char *close = p + 2;
        while (*close == ' ') ++close;
        if (*close != ']') { p += 2; continue; }
        /* "UNITY_UNIFORM <Type>   <name>[" -> Type */
        char *ls = p;
        while (ls > s && ls[-1] != '\n') --ls;
        char *t = strstr(ls, "UNITY_UNIFORM");
        size_t size = 0;
        if (t && t < p) {
            t += 13;
            while (*t == ' ' || *t == '\t') ++t;
            char *te = t;
            while (te < p && *te != ' ' && *te != '\t') ++te;
            size = std140_struct_size(s, t, (size_t)(te - t));
        }
        if (size) {
            size_t count = 65536 / size;
            if (count > 1024) count = 1024;
            char num[24];
            int nl = snprintf(num, sizeof num, "%zu", count);
            size_t slot = (size_t)(close - (p + 1));
            if ((size_t)nl <= slot) {
                memset(p + 1, ' ', slot);
                memcpy(p + 1, num, (size_t)nl);
                ++*fixed;
            }
        }
        p = close;
    }
    return s;
}

static void bs_glShaderSource(GLuint sh, GLsizei n, const GLchar *const *src, const GLint *len) {
    size_t total = 0;
    for (GLsizei i = 0; i < n; ++i)
        total += len && len[i] >= 0 ? (size_t)len[i] : strlen(src[i]);
    char *joined = malloc(total + 1), *o = joined;
    if (!joined) { glShaderSource(sh, n, src, len); return; }
    for (GLsizei i = 0; i < n; ++i) {
        size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(src[i]);
        memcpy(o, src[i], l); o += l;
    }
    *o = '\0';
    char *rewritten = mv_rewrite(joined);
    if (sh < BS_MV_MAX_IDS) {
        GLint type = 0;
        glGetShaderiv(sh, GL_SHADER_TYPE, &type);
        g_shader_mv[sh] = type != GL_VERTEX_SHADER ? 0
                        : rewritten && strstr(joined, "num_views") ? BS_PROG_MV : BS_PROG_MONO;
        g_shader_cut[sh] = type == GL_VERTEX_SHADER && strstr(joined, "_Cutout") != NULL;
    }
    static int dump_all = -1;
    if (dump_all < 0) {
        FILE *df = fopen("sdmc:/switch/beat-saber-quest/dump_shaders", "r");
        dump_all = df != NULL;
        if (df) fclose(df);
        if (dump_all) mkdir("sdmc:/switch/beat-saber-quest/shaderdump", 0777);
    }
    if (sh < BS_MV_MAX_IDS) g_shader_light[sh] = (unsigned char)light_cs_register(sh, joined);
    if (sh < BS_MV_MAX_IDS) g_shader_ui[sh] = strstr(joined, "_ClipRect") && !strstr(joined, "gl_Position");
    static int cube_white = -1;
    if (cube_white < 0) {
        FILE *cf = fopen("sdmc:/switch/beat-saber-quest/cube_white", "r");
        cube_white = cf != NULL;
        if (cf) fclose(cf);
    }
    char **fin = rewritten ? &rewritten : &joined;
    {
        static int total_shaders, total_divs;
        int divs = 0;
        *fin = idx_div_to_shift(*fin, &divs);
        total_divs += divs;
        if (divs && total_shaders++ < 4)
            debugPrintf("[gles] shader %u: %d instanced index division(s) -> shifts (total %d)\n", sh, divs, total_divs);
        static int arr_logs;
        int arrs = 0;
        *fin = fix_instancing_arrays(*fin, &arrs);
        if (arrs && arr_logs++ < 4)
            debugPrintf("[gles] shader %u: %d instancing array(s) resized to fill 64 KiB\n", sh, arrs);
    }
    if (cube_white && strstr(*fin, "samplerCube _EnvironmentReflectionCube")) {
        /* Diagnostic: environment reflection sampled as white. */
        *fin = str_replace_all(*fin, "textureLod(_EnvironmentReflectionCube,", "bs_cubeWhite(_EnvironmentReflectionCube,");
        *fin = str_replace_all(*fin, "texture(_EnvironmentReflectionCube,", "bs_cubeWhite2(_EnvironmentReflectionCube,");
        *fin = str_replace_all(*fin, "void main()",
            "highp vec4 bs_cubeWhite(mediump samplerCube s, highp vec3 c, highp float l) { return vec4(1.0); }\n"
            "highp vec4 bs_cubeWhite2(mediump samplerCube s, highp vec3 c) { return vec4(1.0); }\n"
            "void main()");
    }
    if (sh < BS_MV_MAX_IDS)
        /* Any Unity-instanced shader (notes, bombs, obstacles, ...): these
         * index per-instance UBO arrays by instance ID + unity_BaseInstanceID
         * and collapse on the emulated renderer when drawn as one batch. */
        g_shader_note_body[sh] = strstr(joined, "unity_BaseInstanceID") != NULL;
    static int note_split = -1;
    if (note_split < 0) {
        FILE *nf = fopen("sdmc:/switch/beat-saber-quest/note_split", "r");
        note_split = nf != NULL;
        if (nf) fclose(nf);
    }
    if (note_split && strstr(*fin, "_Cutout") && strstr(*fin, "SV_Target0") &&
        strstr(*fin, "textureLod(_EnvironmentReflectionCube,") &&
        strstr(*fin, "texture(_BloomPrePassTexture,") && strstr(*fin, "    return;\n}")) {
        /* Diagnostic, split screen: left third bloom input = black, middle
         * unchanged, right third raw reflection-cube sample (x0.5). */
        *fin = str_replace_all(*fin, "textureLod(_EnvironmentReflectionCube,", "bs_cubeDiag(_EnvironmentReflectionCube,");
        *fin = str_replace_all(*fin, "texture(_BloomPrePassTexture,", "bs_bloomDiag(_BloomPrePassTexture,");
        *fin = str_replace_all(*fin, "void main()",
            "highp vec4 bs_cubeVal;\n"
            "highp vec4 bs_cubeDiag(mediump samplerCube s, highp vec3 c, highp float l) { bs_cubeVal = textureLod(s, c, l); return bs_cubeVal; }\n"
            "highp vec4 bs_bloomDiag(mediump sampler2D s, highp vec2 c) { return gl_FragCoord.x < 427.0 ? vec4(0.0) : texture(s, c); }\n"
            "void main()");
        *fin = str_replace_all(*fin, "    return;\n}",
            "    if (gl_FragCoord.x > 853.0) SV_Target0 = vec4(bs_cubeVal.rgb * 0.5, 1.0);\n    return;\n}");
    }
    static int note_bloom = -1;
    if (note_bloom < 0) {
        FILE *bf = fopen("sdmc:/switch/beat-saber-quest/note_bloom", "r");
        note_bloom = bf != NULL;
        if (bf) fclose(bf);
    }
    if (note_bloom && strstr(*fin, "SV_Target0") && strstr(*fin, "texture(_BloomPrePassTexture,") &&
        strstr(*fin, "    return;\n}") && !strstr(*fin, "gl_Position")) {
        /* Diagnostic: left half shows the bloom pre-pass sample x4. */
        *fin = str_replace_all(*fin, "texture(_BloomPrePassTexture,", "bs_bloomTap(_BloomPrePassTexture,");
        *fin = str_replace_all(*fin, "void main()",
            "highp vec4 bs_bloomVal;\n"
            "highp vec4 bs_bloomTap(mediump sampler2D s, highp vec2 c) { bs_bloomVal = texture(s, c); return bs_bloomVal; }\n"
            "void main()");
        *fin = str_replace_all(*fin, "    return;\n}",
            "    if (gl_FragCoord.x < 640.0) SV_Target0.xyz = bs_bloomVal.xyz * 4.0;\n    return;\n}");
    }
    static int note_fogviz = -1;
    if (note_fogviz < 0) {
        FILE *ff = fopen("sdmc:/switch/beat-saber-quest/note_fogviz", "r");
        note_fogviz = ff != NULL;
        if (ff) fclose(ff);
    }
    if (note_fogviz && strstr(*fin, "_Cutout") && strstr(*fin, "SV_Target0") &&
        strstr(*fin, "texture(_BloomPrePassTexture,") && !strstr(*fin, "gl_Position")) {
        /* Diagnostic: note fog colour = magenta, so fog amount is visible. */
        *fin = str_replace_all(*fin, "texture(_BloomPrePassTexture,", "bs_fogTap(_BloomPrePassTexture,");
        *fin = str_replace_all(*fin, "void main()",
            "highp vec4 bs_fogTap(mediump sampler2D s, highp vec2 c) { return vec4(1.0, 0.0, 1.0, 1.0); }\n"
            "void main()");
    }
    static int id_viz = -1;
    static char id_filter[64]; /* optional first line of the flag file: required substring */
    if (id_viz < 0) {
        FILE *idf = fopen("sdmc:/switch/beat-saber-quest/id_viz", "r");
        id_viz = idf != NULL;
        if (idf) {
            if (fgets(id_filter, sizeof id_filter, idf))
                id_filter[strcspn(id_filter, "\r\n")] = '\0';
            fclose(idf);
        }
    }
    if (id_viz && (!id_filter[0] || strstr(*fin, id_filter)) &&
        strstr(*fin, "SV_Target0") && strstr(*fin, "    return;\n}") && !strstr(*fin, "gl_Position")) {
        /* Diagnostic: right half = shader id, 3 bits per channel (R low). */
        char ins[200];
        snprintf(ins, sizeof ins,
                 "    if (gl_FragCoord.x >= 640.0) SV_Target0 = vec4(%d.0 / 7.0, %d.0 / 7.0, %d.0 / 7.0, 1.0);\n    return;\n}",
                 (int)(sh & 7), (int)((sh >> 3) & 7), (int)((sh >> 6) & 7));
        *fin = str_replace_all(*fin, "    return;\n}", ins);
        static const char *marks[] = {"_Cutout", "_TintColor", "_SimpleColor", "_NoteSize", "_CutPlane",
            "_BloomPrePassTexture", "_EnvironmentReflectionCube", "_MainTex", "_Color", "_FakeMirrorTransparency",
            "_SizeParams", "_CutoutTexScale", "_RimDarkening", "_TimeHelperOffset", "_Intensity", "_AlphaMultiplier"};
        char ml[400];
        int mo = snprintf(ml, sizeof ml, "[idviz] sh=%u rgb=%d,%d,%d:", sh, (int)(sh & 7), (int)((sh >> 3) & 7),
                          (int)((sh >> 6) & 7));
        for (size_t mi = 0; mi < sizeof marks / sizeof marks[0] && mo < (int)sizeof ml - 32; ++mi)
            if (strstr(*fin, marks[mi])) mo += snprintf(ml + mo, sizeof ml - (size_t)mo, " %s", marks[mi]);
        debugPrintf("%s\n", ml);
    }
    static int obst_viz = -1;
    if (obst_viz < 0) {
        FILE *of = fopen("sdmc:/switch/beat-saber-quest/obst_viz", "r");
        obst_viz = of != NULL;
        if (of) fclose(of);
    }
    if (obst_viz && strstr(*fin, "_TintColorAlpha") && strstr(*fin, "_AddColor") &&
        strstr(*fin, "vs_SV_InstanceID0") && strstr(*fin, "    return;\n}") && !strstr(*fin, "gl_Position")) {
        /* Diagnostic: obstacle core -> left half _TintColor, right half _AddColor x4. */
        *fin = str_replace_all(*fin, "    return;\n}",
            "    { int bsI = int((float((int(vs_SV_InstanceID0) + unity_BaseInstanceID) * 3) + 0.5) / 3.0);\n"
            "      SV_Target0 = gl_FragCoord.x < 640.0 ? vec4(PropsArray[bsI]._TintColor.rgb, 1.0)\n"
            "                                          : vec4(PropsArray[bsI]._AddColor.rgb * 4.0, 1.0); }\n"
            "    return;\n}");
    }
    static int note_magenta = -1;
    if (note_magenta < 0) {
        FILE *mf = fopen("sdmc:/switch/beat-saber-quest/note_magenta", "r");
        note_magenta = mf != NULL;
        if (mf) fclose(mf);
    }
    if (note_magenta && strstr(*fin, "_Cutout") && strstr(*fin, "SV_Target0") &&
        !strstr(*fin, "gl_Position")) {
        /* Diagnostic: every note-family fragment shader writes solid magenta. */
        *fin = str_replace_all(*fin, "    return;\n}", "    SV_Target0 = vec4(1.0, 0.0, 1.0, 1.0);\n    return;\n}");
    }
    static int mask_viz = -1;
    if (mask_viz < 0) {
        FILE *mvf = fopen("sdmc:/switch/beat-saber-quest/mask_viz", "r");
        mask_viz = mvf != NULL;
        if (mvf) fclose(mvf);
    }
    if (mask_viz && strstr(*fin, "texture(_MaskTex, vs_TEXCOORD1.xy).w") && strstr(*fin, "    return;\n}")) {
        /* Diagnostic: UI mask shader -> R = mask alpha, G/B = mask UV. */
        *fin = str_replace_all(*fin, "texture(_MaskTex, vs_TEXCOORD1.xy).w", "bs_maskTap(vs_TEXCOORD1.xy)");
        *fin = str_replace_all(*fin, "void main()",
            "highp float bs_maskVal;\n"
            "highp float bs_maskTap(highp vec2 c) { bs_maskVal = texture(_MaskTex, c).w; return bs_maskVal; }\n"
            "void main()");
        *fin = str_replace_all(*fin, "    return;\n}",
            "    SV_Target0 = vec4(bs_maskVal, fract(vs_TEXCOORD1.x), fract(vs_TEXCOORD1.y), 1.0);\n    return;\n}");
        debugPrintf("[gles] mask_viz applied to shader %u\n", sh);
    }
    static int note_nodiscard = -1;
    if (note_nodiscard < 0) {
        FILE *ndf = fopen("sdmc:/switch/beat-saber-quest/note_nodiscard", "r");
        note_nodiscard = ndf != NULL;
        if (ndf) fclose(ndf);
    }
    if (note_nodiscard && strstr(*fin, "_Cutout") && strstr(*fin, "SV_Target0") &&
        !strstr(*fin, "gl_Position")) {
        /* Diagnostic: note-family cutout discard disabled; discarded pixels
         * would have been drawn green instead. */
        *fin = str_replace_all(*fin, "{discard;}", "{SV_Target0 = vec4(0.0, 1.0, 0.0, 1.0); return;}");
    }
    static int note_pos = -1;
    if (note_pos < 0) {
        FILE *pf = fopen("sdmc:/switch/beat-saber-quest/note_pos", "r");
        note_pos = pf != NULL;
        if (pf) fclose(pf);
    }
    if (note_pos && strstr(*fin, "_Cutout") && strstr(*fin, "gl_Position") &&
        strstr(*fin, "    bs_MultiviewMain();\n}")) {
        /* Diagnostic: note-family vertices forced to a fixed on-screen spot. */
        *fin = str_replace_all(*fin, "    bs_MultiviewMain();\n}",
            "    bs_MultiviewMain();\n    gl_Position = vec4(in_POSITION0.xy * 0.4, 0.5, 1.0);\n}");
    }
    if (dump_all) {
        static unsigned short seq;
        ++seq;
        if (sh < BS_MV_MAX_IDS) g_shader_seq[sh] = seq;
        char path[96];
        snprintf(path, sizeof path, "sdmc:/switch/beat-saber-quest/shaderdump/q%u.txt", seq);
        FILE *f = fopen(path, "w");
        if (f) { fputs(rewritten ? rewritten : joined, f); fclose(f); }
    }
    if (rewritten) {
        ++g_mv_shaders;
        if (g_mv_shaders <= 2 && !dump_all) {
            mkdir("sdmc:/switch/beat-saber-quest/shaderdump", 0777);
            char path[96];
            snprintf(path, sizeof path, "sdmc:/switch/beat-saber-quest/shaderdump/mv_%lu.txt", g_mv_shaders);
            FILE *f = fopen(path, "w");
            if (f) { fprintf(f, "%s\n===== rewritten =====\n%s", joined, rewritten); fclose(f); }
        }
    }
    const GLchar *one = rewritten ? rewritten : joined;
    glShaderSource(sh, 1, &one, NULL);
    free(rewritten);
    free(joined);
}

/* Read `n` floats of uniform `field` (element `elem` of its struct array,
 * using `field1` for the stride) from the UBO currently bound for it. */
static int read_ubo(GLuint prog, const char *block, const char *field0, const char *field1,
                    int elem, float *out, int n) {
    GLuint bi = glGetUniformBlockIndex(prog, block);
    if (bi == GL_INVALID_INDEX) return 0;
    const GLchar *names[2] = {field0, field1 ? field1 : field0};
    GLuint idx[2];
    glGetUniformIndices(prog, 2, names, idx);
    if (idx[0] == GL_INVALID_INDEX) return 0;
    GLint off[2] = {0, 0}, binding = 0, buf = 0;
    glGetActiveUniformsiv(prog, 1, &idx[0], GL_UNIFORM_OFFSET, &off[0]);
    if (field1 && idx[1] != GL_INVALID_INDEX) glGetActiveUniformsiv(prog, 1, &idx[1], GL_UNIFORM_OFFSET, &off[1]);
    GLint stride = field1 && idx[1] != GL_INVALID_INDEX ? off[1] - off[0] : 0;
    glGetActiveUniformBlockiv(prog, bi, GL_UNIFORM_BLOCK_BINDING, &binding);
    GLint64 start = 0, size = 0;
    glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, (GLuint)binding, &buf);
    glGetInteger64i_v(GL_UNIFORM_BUFFER_START, (GLuint)binding, &start);
    if (!buf) return -1;
    GLint prev_r = 0, prev_w = 0;
    glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &prev_r);
    glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &prev_w);
    glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)buf);
    glGetBufferParameteri64v(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &size);
    GLint64 at = start + off[0] + (GLint64)stride * elem;
    int ok = 0;
    if (at >= 0 && at + n * 4 <= size) {
        static GLuint scratch;
        if (!scratch) {
            glGenBuffers(1, &scratch);
            glBindBuffer(GL_COPY_WRITE_BUFFER, scratch);
            glBufferData(GL_COPY_WRITE_BUFFER, 256, NULL, GL_STREAM_READ);
        }
        glBindBuffer(GL_COPY_WRITE_BUFFER, scratch);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)at, 0, n * 4);
        const float *p = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, n * 4, GL_MAP_READ_BIT);
        if (p) { memcpy(out, p, (size_t)n * 4); glUnmapBuffer(GL_COPY_WRITE_BUFFER); ok = 1; }
    }
    glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)prev_r);
    glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)prev_w);
    return ok ? 1 : -2;
}
/* Note/saber family draws (Props with _Cutout): where do they land? */
static void trace_cutout_draw(GLuint prog, GLsizei count, GLsizei inst) {
    static unsigned char seen[BS_MV_MAX_IDS], skip[BS_MV_MAX_IDS];
    static unsigned short calls[BS_MV_MAX_IDS];
    static int total, enabled = -1;
    if (enabled < 0) {
        FILE *f = fopen("sdmc:/switch/beat-saber-quest/trace_cut", "r");
        enabled = f != NULL;
        if (f) fclose(f);
    }
    if (!enabled) return;
    if (prog >= BS_MV_MAX_IDS || skip[prog] || seen[prog] >= 12 || total >= 400 || !t_draw_fbo) return;
    if (calls[prog]++ % 40) return; /* spread samples over the run */
    GLenum draw_err = glGetError();
    if (glGetUniformBlockIndex(prog, "UnityInstancing_Props") == GL_INVALID_INDEX) {
        static int other;
        skip[prog] = 1;
        if (other++ < 200) {
            GLint fb = 0, vpt[4] = {0};
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
            glGetIntegerv(GL_VIEWPORT, vpt);
            debugPrintf("[gles:oth] prog=%u vs%u fs%u count=%d inst=%d fbo=%d vp=%d,%d,%d,%d mv=%d mvprog=%d depth=%d stencil=%d\n",
                        prog, g_prog_vs[prog], g_prog_fs[prog], count, inst, fb, vpt[0], vpt[1], vpt[2], vpt[3],
                        mv_active(), g_prog_mv[prog], glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_STENCIL_TEST));
        }
        return;
    }
    float cut[4] = {-9, -9, -9, -9}, col[4] = {0}, o2w[16] = {0}, vp[16] = {0};
    int rc = read_ubo(prog, "UnityInstancing_Props", "PropsArray[0]._Cutout", "PropsArray[1]._Cutout", 0, cut, 1);
    if (rc == 0) { skip[prog] = 1; return; }
    ++seen[prog]; ++total;
    {
        GLint nb = 0, lk = 0, vd = 0;
        glGetProgramiv(prog, GL_ACTIVE_UNIFORM_BLOCKS, &nb);
        glGetProgramiv(prog, GL_LINK_STATUS, &lk);
        glValidateProgram(prog);
        glGetProgramiv(prog, GL_VALIDATE_STATUS, &vd);
        char line[512];
        GLint fb = 0, vpt[4] = {0}, ct = -1, cn = 0, cl = -1, cly = -1;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        glGetIntegerv(GL_VIEWPORT, vpt);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &ct);
        if (ct != GL_NONE) {
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &cn);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0x8DA7 /* ATTACHMENT_LAYERED */, &cly);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &cl);
        }
        int o = snprintf(line, sizeof line, "[gles:cut]   fbo=%d col=0x%x/%d layered=%d layer=%d vp=%d,%d,%d,%d mv=%d mvprog=%d drawerr=0x%x link=%d valid=%d blocks:",
                         fb, ct, cn, cly, cl, vpt[0], vpt[1], vpt[2], vpt[3], mv_active(), g_prog_mv[prog], draw_err, lk, vd);
        for (GLint b = 0; b < nb && o < (int)sizeof line - 80; ++b) {
            char name[64] = "";
            GLint ds = 0, bp = 0, buf = 0;
            GLint64 st = 0, sz = 0;
            glGetActiveUniformBlockName(prog, (GLuint)b, sizeof name, NULL, name);
            glGetActiveUniformBlockiv(prog, (GLuint)b, GL_UNIFORM_BLOCK_DATA_SIZE, &ds);
            glGetActiveUniformBlockiv(prog, (GLuint)b, GL_UNIFORM_BLOCK_BINDING, &bp);
            glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, (GLuint)bp, &buf);
            glGetInteger64i_v(GL_UNIFORM_BUFFER_START, (GLuint)bp, &st);
            glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, (GLuint)bp, &sz);
            o += snprintf(line + o, sizeof line - (size_t)o, " %s@%d data=%d buf=%d +%lld/%lld;",
                          name, bp, ds, buf, (long long)st, (long long)sz);
        }
        if (!vd) {
            char il[200] = "";
            glGetProgramInfoLog(prog, sizeof il, NULL, il);
            debugPrintf("[gles:cut]   validate log: %s\n", il);
        }
        debugPrintf("%s\n", line);
    }
    GLint base = 0, loc = glGetUniformLocation(prog, "unity_BaseInstanceID");
    if (loc >= 0) glGetUniformiv(prog, loc, &base);
    read_ubo(prog, "UnityInstancing_Props", "PropsArray[0]._Cutout", "PropsArray[1]._Cutout", base, cut, 1);
    read_ubo(prog, "UnityInstancing_Props", "PropsArray[0]._Color", "PropsArray[1]._Color", base, col, 4);
    int ro = read_ubo(prog, "UnityInstancing_PerDraw0", "unity_Builtins0Array[0].hlslcc_mtx4x4unity_ObjectToWorldArray[0]",
                      "unity_Builtins0Array[1].hlslcc_mtx4x4unity_ObjectToWorldArray[0]", base, o2w, 16);
    int rv = read_ubo(prog, "UnityStereoViewBuffer", "hlslcc_mtx4x4unity_StereoMatrixVP[0]", NULL, 0, vp, 16);
    float wp[4] = {o2w[12], o2w[13], o2w[14], o2w[15]}, clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = vp[r] * wp[0] + vp[4 + r] * wp[1] + vp[8 + r] * wp[2] + vp[12 + r] * wp[3];
    GLboolean cm[4] = {0}, dm = 0;
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
    GLint df = 0, cf = 0, ff = 0, bs = 0, bd = 0;
    glGetIntegerv(GL_DEPTH_FUNC, &df);
    glGetIntegerv(GL_CULL_FACE_MODE, &cf);
    glGetIntegerv(GL_FRONT_FACE, &ff);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bs);
    glGetIntegerv(GL_BLEND_DST_RGB, &bd);
    debugPrintf("[gles:cut] prog=%u vs%u fs%u count=%d inst=%d base=%d cut=%g(rc%d) col=(%.2f %.2f %.2f %.2f) "
                "pos(rc%d)=(%.2f %.2f %.2f %.2f) scale=(%.2f %.2f %.2f) vp(rc%d) clip=(%.2f %.2f %.2f %.2f) "
                "depth=%d/0x%x/%d cull=%d/0x%x/0x%x blend=%d 0x%x/0x%x cmask=%d%d%d%d stencil=%d scissor=%d\n",
                prog, g_prog_vs[prog], g_prog_fs[prog], count, inst, base, cut[0], rc,
                col[0], col[1], col[2], col[3], ro, wp[0], wp[1], wp[2], wp[3],
                o2w[0], o2w[5], o2w[10], rv, clip[0], clip[1], clip[2], clip[3],
                glIsEnabled(GL_DEPTH_TEST), df, dm, glIsEnabled(GL_CULL_FACE), cf, ff,
                glIsEnabled(GL_BLEND), bs, bd, cm[0], cm[1], cm[2], cm[3],
                glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_SCISSOR_TEST));
    if (glIsEnabled(GL_STENCIL_TEST) && total <= 40) {
        GLint sf = 0, sr = 0, sm = 0, sw = 0, s1 = 0, s2 = 0, s3 = 0, bf = 0, sbits = -1, stype = -1, sname = 0, dtype = -1, dname = 0;
        glGetIntegerv(GL_STENCIL_FUNC, &sf);
        glGetIntegerv(GL_STENCIL_REF, &sr);
        glGetIntegerv(GL_STENCIL_VALUE_MASK, &sm);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &sw);
        glGetIntegerv(GL_STENCIL_FAIL, &s1);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &s2);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &s3);
        glGetIntegerv(GL_STENCIL_BACK_FUNC, &bf);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &sname);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stype);
        if (stype != GL_NONE)
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                                  GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &sbits);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &dtype);
        if (dtype != GL_NONE)
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &dname);
        debugPrintf("[gles:cut]   stencil func=0x%x ref=%d mask=0x%x wmask=0x%x ops=0x%x/0x%x/0x%x back=0x%x "
                    "fbo=%d stencil-att type=0x%x bits=%d depth-att type=0x%x name=%d err=0x%x\n",
                    sf, sr, sm, sw, s1, s2, s3, bf, sname, stype, sbits, dtype, dname, glGetError());
    }
}

/* One-frame draw trace: what each draw writes into its target. */
static void post_draw(void);
/* Fog uniforms of each program using the custom height fog (flag trace_fog). */
static void trace_fog_draw(GLuint prog) {
    static int enabled = -1, total;
    static unsigned char seen[BS_MV_MAX_IDS];
    if (enabled < 0) {
        FILE *f = fopen("sdmc:/switch/beat-saber-quest/trace_fog", "r");
        enabled = f != NULL;
        if (f) fclose(f);
    }
    if (!enabled || prog >= BS_MV_MAX_IDS || seen[prog] >= 3 || total >= 300) return;
    static const char *names[] = {"_CustomFogAttenuation", "_CustomFogOffset", "_CustomFogHeightFogStartY",
        "_CustomFogHeightFogHeight", "_FogStartOffset", "_FogScale", "_FogHeightScale", "_FogHeightOffset"};
    float v[8];
    int any = 0;
    for (int i = 0; i < 8; ++i) {
        GLint loc = glGetUniformLocation(prog, names[i]);
        v[i] = -12345.0f;
        if (loc >= 0) { glGetUniformfv(prog, loc, &v[i]); any = 1; }
    }
    if (!any || glGetUniformLocation(prog, "_CustomFogHeightFogHeight") < 0) { seen[prog] = 3; return; }
    ++seen[prog]; ++total;
    debugPrintf("[gles:fog] prog=%u fs%u atten=%g off=%g hStartY=%g hHeight=%g fogStart=%g fogScale=%g hScale=%g hOff=%g cut=%d\n",
                prog, g_prog_fs[prog], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                glGetUniformBlockIndex(prog, "UnityInstancing_Props") != GL_INVALID_INDEX);
}
static void trace_after(const char *what, GLenum mode, GLsizei count, GLsizei inst) {
    post_draw();
    trace_cutout_draw(t_program, count, inst);
    trace_fog_draw(t_program);
    if (!g_trace_frame) return;
    GLint vp[4] = {0}, rd = 0;
    glGetIntegerv(GL_VIEWPORT, vp);
    GLenum err = glGetError();
    unsigned char px[4] = {0};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t_draw_fbo);
    glReadPixels(vp[0] + vp[2] / 2, vp[1] + vp[3] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rd);
    glGetError();
    GLint tdf = 0, tbs = 0, tbd = 0; GLboolean tdm = 0, tcm[4] = {0};
    glGetIntegerv(GL_DEPTH_FUNC, &tdf);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &tdm);
    glGetBooleanv(GL_COLOR_WRITEMASK, tcm);
    glGetIntegerv(GL_BLEND_SRC_RGB, &tbs);
    glGetIntegerv(GL_BLEND_DST_RGB, &tbd);
    debugPrintf("[gles:trace] %s mode=%u count=%d inst=%d prog=%u(%d vs%u fs%u) fbo=%u mv=%d vp=%d,%d,%d,%d "
                "depth=%d/%x/%d blend=%d %x/%x cm=%d%d%d%d st=%d center=%02x%02x%02x%02x err=0x%x\n",
                what, mode, count, inst, t_program,
                t_program < BS_MV_MAX_IDS ? g_prog_mv[t_program] : -1,
                t_program < BS_MV_MAX_IDS ? g_prog_vs[t_program] : 0,
                t_program < BS_MV_MAX_IDS ? g_prog_fs[t_program] : 0, t_draw_fbo, mv_active(),
                vp[0], vp[1], vp[2], vp[3], glIsEnabled(GL_DEPTH_TEST), tdf, tdm, glIsEnabled(GL_BLEND), tbs, tbd,
                tcm[0], tcm[1], tcm[2], tcm[3], glIsEnabled(GL_STENCIL_TEST), px[0], px[1], px[2], px[3], err);
    static int attr_logged;
    if (!mv_active() && attr_logged < 2 && count > 100) {
        ++attr_logged;
        for (GLuint a = 0; a < 8; ++a) {
            GLint en = 0, sz = 0, ty = 0, st = 0, bb = 0, nm = 0, ro = 0, bi = 0, bst = 0, bbuf = 0;
            GLint64 boff = 0;
            void *ptr = NULL;
            glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
            if (!en) continue;
            glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_SIZE, &sz);
            glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ty);
            glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &st);
            glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &bb);
            glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &nm);
            glGetVertexAttribiv(a, 0x82D5, &ro);
            glGetVertexAttribiv(a, 0x82D4, &bi);
            glGetIntegeri_v(0x82D8, (GLuint)bi, &bst);
            glGetIntegeri_v(0x8F4F, (GLuint)bi, &bbuf);
            glGetInteger64i_v(0x82D7, (GLuint)bi, &boff);
            glGetVertexAttribPointerv(a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
            debugPrintf("[gles:trace]   attr%u size=%d type=%#x norm=%d stride=%d buf=%d ptr=%p rel=%d binding=%d bstride=%d bbuf=%d boff=%lld\n",
                        a, sz, ty, nm, st, bb, ptr, ro, bi, bst, bbuf, (long long)boff);
        }
        GLint eb = 0;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
        debugPrintf("[gles:trace]   ebo=%d err=0x%x\n", eb, glGetError());
    }
}
/* Trace helper: brightest value over the whole (single-sampled) target. */
static void trace_target_max(const char *what) {
    if (!g_trace_frame) return;
    GLint vp[4] = {0}, rd = 0, type = 0, fmt = 0;
    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0 || vp[2] * vp[3] > 1024 * 1024) return;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t_draw_fbo);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &fmt);
    size_t n = (size_t)vp[2] * (size_t)vp[3];
    float mx[4] = {0}, sum = 0;
    if (type == GL_FLOAT || type == GL_HALF_FLOAT || type == GL_UNSIGNED_INT_10F_11F_11F_REV) {
        float *px = calloc(n, 16);
        if (px) {
            glReadPixels(vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_FLOAT, px);
            size_t nan_px = 0, lit = 0;
            for (size_t i = 0; i < n; ++i) {
                int is_nan = 0, is_lit = 0;
                for (int c = 0; c < 4; ++c) {
                    float v = px[i * 4 + c];
                    if (v != v) { is_nan = 1; continue; }
                    if (v > mx[c]) mx[c] = v;
                    if (c < 3) { sum += v; if (v > 0.001f) is_lit = 1; }
                }
                nan_px += is_nan;
                lit += is_lit;
            }
            debugPrintf("[gles:trace]   nan pixels=%zu lit pixels=%zu of %zu\n", nan_px, lit, n);
            free(px);
        }
    } else {
        unsigned char *px = calloc(n, 4);
        if (px) {
            glReadPixels(vp[0], vp[1], vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, px);
            for (size_t i = 0; i < n; ++i)
                for (int c = 0; c < 4; ++c) {
                    float v = px[i * 4 + c] / 255.0f;
                    if (v > mx[c]) mx[c] = v;
                    if (c < 3) sum += v;
                }
            free(px);
        }
    }
    GLenum err = glGetError();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rd);
    debugPrintf("[gles:trace]   %s fbo=%u read-type=%#x fmt=%#x max=(%.3f %.3f %.3f %.3f) mean=%.5f err=0x%x\n",
                what, t_draw_fbo, type, fmt, mx[0], mx[1], mx[2], mx[3], sum / (3.0f * (float)n), err);
}
typedef void (*BsDispatchComputeFn)(GLuint, GLuint, GLuint);
typedef void (*BsMemoryBarrierFn)(GLbitfield);
static BsDispatchComputeFn g_dispatch;
static BsMemoryBarrierFn g_barrier;
static void bs_glDispatchCompute(GLuint x, GLuint y, GLuint z) {
    if (!g_dispatch) g_dispatch = (BsDispatchComputeFn)eglGetProcAddress("glDispatchCompute");
    if (!g_barrier) g_barrier = (BsMemoryBarrierFn)eglGetProcAddress("glMemoryBarrier");
    /* Dump the first light-quad dispatches regardless of the trace frame. */
    static int light_dumps;
    int dbg = g_trace_frame;
    if (!dbg && t_program < BS_MV_MAX_IDS && g_prog_light[t_program] && light_dumps < 2) {
        ++light_dumps;
        dbg = 1;
    }
    if (dbg) {
        GLint buf = 0;
        glGetIntegeri_v(0x90D3, 0, &buf);
        if (buf) {
            GLint link = -1, ubo = 0;
            glGetProgramiv(t_program, GL_LINK_STATUS, &link);
            glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 0, &ubo);
            uint32_t nl = 0xFFFFFFFFu;
            if (ubo) {
                GLint pu = 0;
                glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &pu);
                glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)ubo);
                GLint64 start = 0;
                glGetInteger64i_v(GL_UNIFORM_BUFFER_START, 0, &start);
                const uint32_t *u = glMapBufferRange(GL_UNIFORM_BUFFER, (GLintptr)start, 4, GL_MAP_READ_BIT);
                if (u) { nl = u[0]; glUnmapBuffer(GL_UNIFORM_BUFFER); }
                glBindBuffer(GL_UNIFORM_BUFFER, (GLuint)pu);
            }
            char log[512] = {0};
            glGetProgramInfoLog(t_program, sizeof log, NULL, log);
            debugPrintf("[gles:trace] pre-dispatch link=%d ubo0=%d numLights=%u log=%s err=0x%x\n",
                        link, ubo, nl, log, glGetError());
        }
    }
    int light = t_program < BS_MV_MAX_IDS ? g_prog_light[t_program] : 0;
    if (!(light && light_dispatch(light, x, y, z))) g_dispatch(x, y, z);
    /* Beat Saber's bloom pre-pass builds its light quads in a compute shader
     * and draws them as vertex attributes. Unity does not always emit the
     * vertex-attrib barrier before that draw (Adreno tolerates it); on
     * nouveau the draw then fetches stale data. Make results visible. */
    if (g_barrier) g_barrier(0xFFFFFFFFu);           /* GL_ALL_BARRIER_BITS */
    if (dbg) {
        debugPrintf("[gles:trace] dispatch %u,%u,%u prog=%u err=0x%x\n", x, y, z, t_program, glGetError());
        /* Dump SSBO binding 0 (the generated vertex buffer). */
        GLint buf = 0, prev = 0;
        GLint64 sz = 0;
        glGetIntegeri_v(0x90D3 /* GL_SHADER_STORAGE_BUFFER_BINDING */, 0, &buf);
        glGetIntegerv(0x90D3, &prev);
        if (buf) {
            glBindBuffer(0x90D2 /* GL_SHADER_STORAGE_BUFFER */, (GLuint)buf);
            glGetBufferParameteri64v(0x90D2, GL_BUFFER_SIZE, &sz);
            size_t n = sz > 208 * 8 ? 208 * 8 : (size_t)sz;
            const uint32_t *w = n ? glMapBufferRange(0x90D2, 0, (GLsizeiptr)n, GL_MAP_READ_BIT) : NULL;
            debugPrintf("[gles:trace] ssbo0 buf=%d size=%lld map=%p\n", buf, (long long)sz, (const void *)w);
            for (size_t r = 0; w && r < n / 208 && r < 3; ++r)
                for (size_t v = 0; v < 4; ++v) {
                    const uint32_t *q = w + r * 52 + v * 13;
                    char line[512]; int o = 0;
                    for (int k = 0; k < 13; ++k) {
                        float f; memcpy(&f, &q[k], 4);
                        o += snprintf(line + o, sizeof line - (size_t)o, " %08x(%g)", q[k], f);
                    }
                    debugPrintf("[gles:trace]  L%zu v%zu%s\n", r, v, line);
                }
            if (w) glUnmapBuffer(0x90D2);
            glBindBuffer(0x90D2, (GLuint)prev);
        }
    }
}
static void bs_glMemoryBarrier(GLbitfield b) {
    if (!g_barrier) g_barrier = (BsMemoryBarrierFn)eglGetProcAddress("glMemoryBarrier");
    g_barrier(b);
    if (g_trace_frame) debugPrintf("[gles:trace] barrier %#x\n", b);
}
/* glBufferData(NULL) contents are undefined. Adreno returns zeroed memory and
 * Beat Saber relies on it (compute-filled vertex buffers draw unwritten slots
 * as degenerate triangles); nouveau recycles memory, giving NaN-coloured
 * garbage triangles. Zero-fill instead. */
static void bs_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    static void *zeros;
    static size_t zeros_size;
    void *temp = NULL;
    if (!data && size > 0) {
        if ((size_t)size > (4u << 20)) {
            data = temp = calloc(1, (size_t)size);       /* rare: don't cache */
        } else {
            if ((size_t)size > zeros_size) {
                void *z = calloc(1, (size_t)size);
                if (z) { free(zeros); zeros = z; zeros_size = (size_t)size; }
            }
            if ((size_t)size <= zeros_size) data = zeros;
        }
    }
    glBufferData(target, size, data, usage);
    free(temp);
}
static void bs_glUseProgram(GLuint p) { t_program = p; glUseProgram(p); }
static void bs_glClear(GLbitfield mask) {
    ++bs_gl_clears; if (!t_draw_fbo) ++g_fb0_clears; glClear(mask);
    if (g_trace_frame) {
        GLfloat cd = -1, dr[2] = {-1, -1};
        GLint dt = -1, dn = 0, dl = -1, dsz = -1, ifmt = 0, ssz = -1, ct = -1, cn = 0, samp = -1, cs = -1;
        glGetFloatv(GL_DEPTH_CLEAR_VALUE, &cd);
        glGetFloatv(GL_DEPTH_RANGE, dr);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &dt);
        if (dt != GL_NONE) {
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &dn);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, 0x8DA7, &dl);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &dsz);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &ssz);
        }
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &ct);
        if (ct != GL_NONE)
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &cn);
        glGetIntegerv(GL_SAMPLES, &samp);
        cs = (GLint)glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        (void)ifmt;
        debugPrintf("[gles:trace] clear mask=%#x color=(%.2f %.2f %.2f %.2f) fbo=%u cleardepth=%g range=%g..%g "
                    "depth-att=0x%x/%d layered=%d bits=%d/%d color-att=0x%x/%d samples=%d status=0x%x err=0x%x\n",
                    mask, g_clear_rgba[0], g_clear_rgba[1], g_clear_rgba[2], g_clear_rgba[3], t_draw_fbo,
                    cd, dr[0], dr[1], dt, dn, dl, dsz, ssz, ct, cn, samp, cs, glGetError());
    }
}
static void bs_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    g_clear_rgba[0] = r; g_clear_rgba[1] = g; g_clear_rgba[2] = b; g_clear_rgba[3] = a;
    glClearColor(r, g, b, a);
}
/* Diagnostic (sdmc:/switch/beat-saber-quest/no_stencil): draw with the stencil
 * test off, to test whether stencil rejects notes/sabers. */
static int g_no_stencil = -1, g_stencil_off, g_skip_opaque = -1;
/* Diagnostic (trace_fs: fragment shader id): log the unit-0 texture and
 * _ClipRect of draws using that fragment shader, once per texture. */
static GLsizei g_trace_count;
static GLenum g_trace_idx_type;
static intptr_t g_trace_idx_off;
static GLint g_trace_base;
static void trace_fs_draw(void) {
    static int want = -2, all_ui;
    if (want == -2) {
        want = -1;
        FILE *f = fopen("sdmc:/switch/beat-saber-quest/trace_fs", "r");
        if (f) {
            char b[16] = {0};
            if (fgets(b, sizeof b, f)) { if (!strncmp(b, "ui", 2)) all_ui = 1; else want = atoi(b); }
            fclose(f);
        }
    }
    if (t_program >= BS_MV_MAX_IDS) return;
    if (all_ui ? !g_prog_ui[t_program] : (want < 0 || g_prog_fs_sh[t_program] != (unsigned)want)) return;
    static unsigned seen[256];
    static int nseen;
    GLint active = 0, tex = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    unsigned key = (unsigned)tex << 12 | (t_program & 0xfff);
    for (int i = 0; i < nseen; ++i) if (seen[i] == key) { glActiveTexture((GLenum)active); return; }
    if (nseen >= 256) { glActiveTexture((GLenum)active); return; }
    seen[nseen++] = key;
    typedef void (*GetTexLevelFn)(GLenum, GLint, GLenum, GLint *);
    static GetTexLevelFn get_level;
    if (!get_level) get_level = (GetTexLevelFn)eglGetProcAddress("glGetTexLevelParameteriv");
    GLint w = 0, h = 0, fmt = 0;
    if (get_level && tex) {
        get_level(GL_TEXTURE_2D, 0, 0x1000, &w);
        get_level(GL_TEXTURE_2D, 0, 0x1001, &h);
        get_level(GL_TEXTURE_2D, 0, 0x1003, &fmt);
    }
    glActiveTexture((GLenum)active);
    float clip[4] = {0}, st[4] = {0}, col[4] = {0};
    GLint loc = glGetUniformLocation(t_program, "_ClipRect");
    if (loc >= 0) glGetUniformfv(t_program, loc, clip);
    loc = glGetUniformLocation(t_program, "_MainTex_ST");
    if (loc >= 0) glGetUniformfv(t_program, loc, st);
    loc = glGetUniformLocation(t_program, "_Color");
    if (loc >= 0) glGetUniformfv(t_program, loc, col);
    /* Extent of attribute-0 positions referenced by this draw. */
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    GLint vbuf = 0, ibuf = 0, stride = 0, comps = 0, en = 0, vtype = 0;
    void *voff = NULL;
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &vbuf);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &comps);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &vtype);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &voff);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ibuf);
    if (en && vbuf && vtype == GL_FLOAT && ibuf && g_trace_idx_type == GL_UNSIGNED_SHORT && g_trace_count > 0) {
        GLint prev = 0, vsz = 0, isz = 0;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)vbuf);
        glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &vsz);
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &isz);
        const unsigned char *vd = glMapBufferRange(GL_ARRAY_BUFFER, 0, vsz, GL_MAP_READ_BIT);
        const unsigned char *id = vd ? glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, isz, GL_MAP_READ_BIT) : NULL;
        if (vd && id) {
            size_t st2 = stride ? (size_t)stride : (size_t)comps * 4, io = (size_t)voff;
            for (GLsizei k = 0; k < g_trace_count; ++k) {
                size_t ip = (size_t)g_trace_idx_off + (size_t)k * 2;
                if (ip + 2 > (size_t)isz) break;
                unsigned short ix;
                memcpy(&ix, id + ip, 2);
                size_t vp = io + ((size_t)ix + (size_t)g_trace_base) * st2;
                if (vp + 12 > (size_t)vsz) continue;
                float q[3];
                memcpy(q, vd + vp, 12);
                for (int c = 0; c < 3; ++c) { if (q[c] < mn[c]) mn[c] = q[c]; if (q[c] > mx[c]) mx[c] = q[c]; }
            }
            /* Per-quad dump (6 indices each): extent, colour, uv of first vertex. */
            GLint cl = glGetAttribLocation(t_program, "in_COLOR0"), tl = glGetAttribLocation(t_program, "in_TEXCOORD0");
            GLint coff = -1, toff = -1, ctype = 0;
            if (cl >= 0) { void *o = NULL; glGetVertexAttribPointerv((GLuint)cl, GL_VERTEX_ATTRIB_ARRAY_POINTER, &o); coff = (GLint)(intptr_t)o;
                           glGetVertexAttribiv((GLuint)cl, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ctype); }
            if (tl >= 0) { void *o = NULL; glGetVertexAttribPointerv((GLuint)tl, GL_VERTEX_ATTRIB_ARRAY_POINTER, &o); toff = (GLint)(intptr_t)o; }
            for (GLsizei k = 0; k + 6 <= g_trace_count && k < 6 * 40; k += 6) {
                float qa[2] = {1e30f, 1e30f}, qb[2] = {-1e30f, -1e30f};
                unsigned short ix0 = 0;
                for (int j = 0; j < 6; ++j) {
                    size_t ip = (size_t)g_trace_idx_off + (size_t)(k + j) * 2;
                    if (ip + 2 > (size_t)isz) break;
                    unsigned short ix; memcpy(&ix, id + ip, 2);
                    if (!j) ix0 = ix;
                    size_t vp = io + ((size_t)ix + (size_t)g_trace_base) * st2;
                    if (vp + 8 > (size_t)vsz) continue;
                    float q[2]; memcpy(q, vd + vp, 8);
                    for (int c = 0; c < 2; ++c) { if (q[c] < qa[c]) qa[c] = q[c]; if (q[c] > qb[c]) qb[c] = q[c]; }
                }
                size_t vb = ((size_t)ix0 + (size_t)g_trace_base) * st2;
                unsigned char col4[4] = {0}; float uv[2] = {0};
                if (coff >= 0 && vb + (size_t)coff + 4 <= (size_t)vsz) memcpy(col4, vd + vb + coff, 4);
                if (toff >= 0 && vb + (size_t)toff + 8 <= (size_t)vsz) memcpy(uv, vd + vb + toff, 8);
                debugPrintf("[gles:fs]   q%d x=%.1f..%.1f y=%.1f..%.1f col=%02x%02x%02x%02x(t=0x%x) uv=%.3f,%.3f\n", (int)(k / 6),
                            qa[0], qb[0], qa[1], qb[1], col4[0], col4[1], col4[2], col4[3], ctype, uv[0], uv[1]);
            }
        }
        if (id) glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        if (vd) glUnmapBuffer(GL_ARRAY_BUFFER);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev);
    }
    debugPrintf("[gles:fs] fs=%u prog=%u tex=%d %dx%d fmt=0x%x n=%d pos=(%.1f..%.1f, %.1f..%.1f, %.1f..%.1f) "
                "clip=(%.1f %.1f %.1f %.1f) st=(%.2f %.2f %.2f %.2f) "
                "col=(%.2f %.2f %.2f %.2f) fbo=%u\n", g_prog_fs_sh[t_program], t_program, tex, w, h, fmt,
                (int)g_trace_count, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2],
                clip[0], clip[1], clip[2], clip[3], st[0], st[1], st[2], st[3],
                col[0], col[1], col[2], col[3], t_draw_fbo);
}
static int pre_draw(void) {
    trace_fs_draw();
    if (g_no_stencil < 0) {
        FILE *f = fopen("sdmc:/switch/beat-saber-quest/no_stencil", "r");
        g_no_stencil = f != NULL;
        if (f) fclose(f);
        if (g_no_stencil) debugPrintf("[gles] DIAGNOSTIC: stencil test disabled for all draws\n");
        f = fopen("sdmc:/switch/beat-saber-quest/skip_opaque", "r");
        g_skip_opaque = f != NULL;
        if (f) fclose(f);
        if (g_skip_opaque) debugPrintf("[gles] DIAGNOSTIC: depth-writing non-note draws skipped\n");
    }
    if (g_skip_opaque && t_draw_fbo && t_program < BS_MV_MAX_IDS && !g_prog_cut[t_program]) {
        GLboolean dm = 0;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);
        if (dm && glIsEnabled(GL_DEPTH_TEST) && mv_active()) return 1;
    }
    if (glIsEnabled(GL_STENCIL_TEST)) {
        /* Log each distinct stencil state once (UI Mask diagnosis). */
        static unsigned keys[32];
        static int nkeys;
        GLint sf = 0, sr = 0, sw = 0, op = 0;
        glGetIntegerv(GL_STENCIL_FUNC, &sf);
        glGetIntegerv(GL_STENCIL_REF, &sr);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &sw);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &op);
        unsigned key = ((unsigned)sf & 0xf) | ((unsigned)sr & 0xff) << 4 | ((unsigned)sw & 0xff) << 12 |
                       ((unsigned)op & 0xfff) << 20;
        int seen = 0;
        for (int k = 0; k < nkeys; ++k) seen |= keys[k] == key;
        if (!seen && nkeys < 32) {
            keys[nkeys++] = key;
            GLboolean cm[4] = {0};
            glGetBooleanv(GL_COLOR_WRITEMASK, cm);
            debugPrintf("[gles:stencil] func=0x%x ref=%d wmask=0x%x zpass=0x%x cmask=%d%d%d%d fbo=%u prog=%u mv=%d\n",
                        sf, sr, sw, op, cm[0], cm[1], cm[2], cm[3], t_draw_fbo, t_program, mv_active());
        }
    }
    if (g_no_stencil && glIsEnabled(GL_STENCIL_TEST)) { glDisable(GL_STENCIL_TEST); g_stencil_off = 1; }
    return 0;
}
static void post_draw(void) {
    if (g_stencil_off) { glEnable(GL_STENCIL_TEST); g_stencil_off = 0; }
}
#define TRACE_ARGS(c_, t_, i_, b_) (g_trace_count = (c_), g_trace_idx_type = (t_), g_trace_idx_off = (intptr_t)(i_), g_trace_base = (b_))
#define DRAW_COUNT() do { \
    if (pre_draw()) return; \
    ++bs_gl_draws; \
    ++g_frame_draws; \
    if (!t_draw_fbo) ++g_fb0_draws; \
    if (mv_active()) ++g_mv_draws; \
} while (0)
static void bs_glDrawArrays(GLenum m, GLint f, GLsizei c) {
    TRACE_ARGS(c, 0, 0, 0);
    DRAW_COUNT();
    glDrawArrays(m, f, c);
    if (t_draw_fbo) trace_target_max("after drawArrays");
    trace_after("drawArrays", m, c, 1);
}
static void bs_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) {
    TRACE_ARGS(c, t, i, 0);
    DRAW_COUNT();
    glDrawElements(m, c, t, i);
    if (t_draw_fbo) trace_target_max("after drawElements");
    trace_after("drawElements", m, c, 1);
}
static void bs_glDrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t,
                                   const void *i) {
    TRACE_ARGS(c, t, i, 0);
    DRAW_COUNT();
    glDrawRangeElements(m, s, e, c, t, i);
    trace_after("drawRangeElements", m, c, 1);
}
static void inst_log(GLsizei n) {
    static GLsizei max_n;
    static int logs;
    if (n > max_n && logs < 12) {
        max_n = n; ++logs;
        debugPrintf("[gles] instanced draw: new max instance count %d (prog %u)\n", (int)n, t_program);
    }
}
/* Instanced batches (notes, bombs, walls) on the emulated renderer only
 * produce the first instance correctly. Submit each instance separately with
 * its original index into Unity's instancing UBOs. Programs without
 * unity_BaseInstanceID get loc -1 and draw normally. */
static GLint note_batch_base(GLsizei instances, GLint *base) {
    if (instances <= 1 || t_program >= BS_MV_MAX_IDS ||
        !g_prog_note_body[t_program] || !mv_active())
        return -1;
    GLint loc = glGetUniformLocation(t_program, "unity_BaseInstanceID");
    if (loc >= 0) {
        glGetUniformiv(t_program, loc, base);
        static int logged;
        if (logged++ < 4)
            debugPrintf("[gles:notes] splitting program %u draw with %d instances, base=%d\n",
                        t_program, instances, *base);
    }
    return loc;
}
static void bs_glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t,
                                       const void *i, GLsizei n) {
    TRACE_ARGS(c, t, i, 0);
    DRAW_COUNT();
    inst_log(n);
    GLint base = 0;
    GLint loc = note_batch_base(n, &base);
    if (loc < 0) {
        glDrawElementsInstanced(m, c, t, i, n);
    } else {
        for (GLsizei note = 0; note < n; ++note) {
            glUniform1i(loc, base + note);
            glDrawElementsInstanced(m, c, t, i, 1);
        }
        glUniform1i(loc, base);
    }
    trace_after("drawElementsInst", m, c, n);
}
static void bs_glDrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) {
    TRACE_ARGS(c, 0, 0, 0);
    DRAW_COUNT();
    inst_log(n);
    glDrawArraysInstanced(m, f, c, n);
    trace_after("drawArraysInst", m, c, n);
}
typedef void (*BsDrawElementsBaseVertexFn)(GLenum, GLsizei, GLenum, const void *, GLint);
typedef void (*BsDrawElementsInstancedBaseVertexFn)(GLenum, GLsizei, GLenum, const void *,
                                                    GLsizei, GLint);
typedef void (*BsDrawRangeElementsBaseVertexFn)(GLenum, GLuint, GLuint, GLsizei, GLenum,
                                                const void *, GLint);
static BsDrawElementsBaseVertexFn g_dbv;
static BsDrawElementsInstancedBaseVertexFn g_dibv;
static BsDrawRangeElementsBaseVertexFn g_drbv;
static void load_base_vertex(void) {
    if (!g_dibv) {
        g_dbv = (BsDrawElementsBaseVertexFn)eglGetProcAddress("glDrawElementsBaseVertex");
        g_dibv = (BsDrawElementsInstancedBaseVertexFn)eglGetProcAddress("glDrawElementsInstancedBaseVertex");
        g_drbv = (BsDrawRangeElementsBaseVertexFn)eglGetProcAddress("glDrawRangeElementsBaseVertex");
    }
}
static void bs_glDrawElementsBaseVertex(GLenum m, GLsizei c, GLenum t, const void *i, GLint b) {
    TRACE_ARGS(c, t, i, b);
    DRAW_COUNT(); load_base_vertex();
    g_dbv(m, c, t, i, b);
    trace_after("drawElementsBV", m, c, 1);
}
static void bs_glDrawElementsInstancedBaseVertex(GLenum m, GLsizei c, GLenum t,
                                                 const void *i, GLsizei n, GLint b) {
    TRACE_ARGS(c, t, i, b);
    DRAW_COUNT(); load_base_vertex();
    inst_log(n);
    GLint base = 0;
    GLint loc = note_batch_base(n, &base);
    if (loc < 0) {
        g_dibv(m, c, t, i, n, b);
    } else {
        for (GLsizei note = 0; note < n; ++note) {
            glUniform1i(loc, base + note);
            g_dibv(m, c, t, i, 1, b);
        }
        glUniform1i(loc, base);
    }
    trace_after("drawElementsInstBV", m, c, n);
}
static void bs_glDrawRangeElementsBaseVertex(GLenum m, GLuint s, GLuint e, GLsizei c,
                                             GLenum t, const void *i, GLint b) {
    TRACE_ARGS(c, t, i, b);
    DRAW_COUNT(); load_base_vertex();
    g_drbv(m, s, e, c, t, i, b);
    trace_after("drawRangeElementsBV", m, c, 1);
}
static void bs_glVertexAttribDivisor(GLuint index, GLuint divisor) {
    static int logged;
    if (divisor && logged < 4) {
        ++logged;
        debugPrintf("[gles:mv] warning: instanced attribute %u divisor %u\n", index, divisor);
    }
    glVertexAttribDivisor(index, divisor);
}
static void bs_glBindFramebuffer(GLenum t, GLuint f) {
    ++bs_gl_fbo_binds;
    if (t == GL_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER) t_draw_fbo = f;
    if (t == GL_FRAMEBUFFER || t == GL_READ_FRAMEBUFFER) t_read_fbo = f;
    glBindFramebuffer(t, f);
}
static void bs_glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f,
                                 GLint g, GLint h, GLbitfield m, GLenum fl) {
    ++g_blits; if (!t_draw_fbo) ++g_fb0_blits;
    if (g_trace_frame)
        debugPrintf("[gles:trace] blit read=%u draw=%u src=%d,%d,%d,%d dst=%d,%d,%d,%d mask=%#x\n",
                    t_read_fbo, t_draw_fbo, a, b, c, d, e, f, g, h, m);
    glBlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
}
static void bs_glCompileShader(GLuint s) {
    glCompileShader(s);
    static int logged;
    GLint ok = 1;
    if (logged >= 24) return;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char text[512] = {0};
        glGetShaderInfoLog(s, sizeof text - 1, NULL, text);
        ++logged;
        debugPrintf("[gles] shader %u compile FAILED: %s\n", s, text);
    }
}
static void bs_glLinkProgram(GLuint p) {
    glLinkProgram(p);
    static int logged, total;
    GLint ok = 1;
    ++total;
    if (logged >= 24) return;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char text[512] = {0};
        glGetProgramInfoLog(p, sizeof text - 1, NULL, text);
        ++logged;
        debugPrintf("[gles] program %u link FAILED (#%d): %s\n", p, total, text);
    }
}
static void bs_glAttachShader(GLuint p, GLuint s) {
    if (p < BS_MV_MAX_IDS && s < BS_MV_MAX_IDS) {
        if (g_shader_light[s]) g_prog_light[p] = g_shader_light[s];
        if (g_shader_ui[s]) g_prog_ui[p] = 1;
        if (g_shader_mv[s]) { g_prog_mv[p] = g_shader_mv[s]; g_prog_vs[p] = g_shader_seq[s]; g_prog_cut[p] = g_shader_cut[s]; }
        else {
            g_prog_fs[p] = g_shader_seq[s];
            g_prog_fs_sh[p] = (unsigned short)s;
        }
        if (g_shader_note_body[s]) g_prog_note_body[p] = 1;
    }
    glAttachShader(p, s);
}
static GLuint bs_glCreateProgram(void) {
    GLuint p = glCreateProgram();
    if (p < BS_MV_MAX_IDS) {
        g_prog_mv[p] = BS_PROG_UNKNOWN;
        g_prog_light[p] = 0;
        g_prog_note_body[p] = 0;
    }
    return p;
}
static void bs_glFlush(void) { ++bs_gl_flushes; glFlush(); }
static __eglMustCastToProperFunctionPointerType bs_eglGetProcAddress(const char *name);

typedef struct { const char *name; void *fn; } BsEglEntry;
/* Texture upload diagnostics: cube maps and any failing upload. */
static int tex_is_cube(GLenum t) { return t == 0x8513 || (t >= 0x8515 && t <= 0x851A) || t == 0x9009; }
static void tex_log(const char *what, GLenum target, GLint level, GLenum fmt, GLenum type,
                    GLsizei w, GLsizei h, GLsizei d, GLsizei size) {
    GLenum e = glGetError();
    static int cube_logs, err_logs;
    int cube = tex_is_cube(target) || target == GL_TEXTURE_3D;
    /* Cube face 0 (+X) at every mip level shows whether the mip chain is complete. */
    int face0 = target == 0x8515 || target == 0x8513 || target == GL_TEXTURE_3D || target == 0x9009;
    if (e != GL_NO_ERROR ? err_logs++ < 48 : (cube && face0 && cube_logs++ < 160)) {
        GLint tex = 0;
        glGetIntegerv(tex_is_cube(target) ? 0x8514 : target == GL_TEXTURE_3D ? 0x806A
                      : GL_TEXTURE_BINDING_2D, &tex);
        debugPrintf("[gles:tex] %s target=0x%x tex=%d level=%d fmt=0x%x type=0x%x %dx%dx%d size=%d err=0x%x\n",
                    what, target, tex, level, fmt, type, w, h, d, size, e);
    }
}
#define BS_TEX_FMT_MAX 8192
static GLint bound_tex2d(GLenum t) {
    GLint tex = 0;
    glGetIntegerv(t == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D :
                  tex_is_cube(t) ? 0x8514 : t == GL_TEXTURE_3D ? 0x806A :
                  t == GL_TEXTURE_2D_ARRAY ? 0x8C1D : GL_TEXTURE_BINDING_2D, &tex);
    return tex;
}
/* Alpha8 emulation. Unity stores Alpha8 textures as GL_R8 with swizzle
 * (ZERO,ZERO,ZERO,RED), set before glTexStorage2D. That swizzle is lost
 * on the Switch Mesa/Eden path, so alpha samples as 1.0 and soft sprites
 * (note arrow/circle glows) draw as solid oversized squares. Allocate such
 * textures as RGBA8, expand GL_RED uploads to (0,0,0,a), and keep the
 * hardware swizzle at identity. */
static GLint g_tex_swz[BS_TEX_FMT_MAX][4];
static unsigned char g_tex_swz_set[BS_TEX_FMT_MAX];
static unsigned char g_tex_a8[BS_TEX_FMT_MAX]; /* storage converted to RGBA8 */
static int swz_is_alpha8(GLint tex) {
    const GLint *s = g_tex_swz[tex];
    return g_tex_swz_set[tex] == 0xf &&
           s[0] == GL_ZERO && s[1] == GL_ZERO && s[2] == GL_ZERO && s[3] == GL_RED;
}
static void bs_glDeleteTextures(GLsizei n, const GLuint *textures) {
    /* Mesa can reuse a deleted name for a regular red-channel texture. */
    for (GLsizei i = 0; i < n; ++i) {
        GLuint tex = textures[i];
        if (tex && tex < BS_TEX_FMT_MAX) {
            g_tex_swz_set[tex] = 0;
            g_tex_a8[tex] = 0;
        }
    }
    glDeleteTextures(n, textures);
}
static void swz_identity(GLenum t) {
    glTexParameteri(t, 0x8E42, GL_RED);
    glTexParameteri(t, 0x8E43, GL_GREEN);
    glTexParameteri(t, 0x8E44, GL_BLUE);
    glTexParameteri(t, 0x8E45, GL_ALPHA);
}
static void swz_log(const char *what, GLenum t, GLenum f, GLsizei w, GLsizei h) {
    GLint tex = bound_tex2d(t);
    if (tex <= 0 || tex >= BS_TEX_FMT_MAX || !g_tex_swz_set[tex]) return;
    const GLint *z = g_tex_swz[tex];
    static int n;
    if (n++ < 40)
        debugPrintf("[gles:swz] %s tex=%d target=0x%x fmt=0x%x %dx%d swz=0x%x,0x%x,0x%x,0x%x set=0x%x\n",
                    what, tex, t, f, w, h, z[0], z[1], z[2], z[3], g_tex_swz_set[tex]);
}
static void bs_glTexStorage2D(GLenum t, GLsizei lv, GLenum f, GLsizei w, GLsizei h) {
    GLint tex = bound_tex2d(t);
    int ok = tex > 0 && tex < BS_TEX_FMT_MAX;
    if (ok) g_tex_a8[tex] = 0;
    if (ok && t == GL_TEXTURE_2D && f == GL_R8 && swz_is_alpha8(tex)) {
        glTexStorage2D(t, lv, GL_RGBA8, w, h);
        swz_identity(t);
        g_tex_a8[tex] = 1;
        static int n;
        if (n++ < 8)
            debugPrintf("[gles:tex] Alpha8 tex=%d %dx%d levels=%d -> RGBA8 (0,0,0,a)\n", tex, w, h, lv);
        tex_log("TexStorage2D", t, 0, GL_RGBA8, lv, w, h, 1, 0);
        return;
    }
    swz_log("TexStorage2D", t, f, w, h);
    glTexStorage2D(t, lv, f, w, h);
    tex_log("TexStorage2D", t, 0, f, lv, w, h, 1, 0);
}
static void swz_record(GLenum t, GLenum p, GLint v) {
    if (t != GL_TEXTURE_2D && t != GL_TEXTURE_3D) return;
    GLint tex = bound_tex2d(t);
    if (tex > 0 && tex < BS_TEX_FMT_MAX) {
        GLuint channel = p - 0x8E42;
        g_tex_swz[tex][channel] = v;
        g_tex_swz_set[tex] |= 1u << channel;
        if (channel == 3 && v != GL_ALPHA && !g_tex_a8[tex]) {
            GLint fmt = 0, w = 0;
            typedef void (*GetTexLevelFn)(GLenum, GLint, GLenum, GLint *);
            static GetTexLevelFn get_level;
            if (!get_level) get_level = (GetTexLevelFn)eglGetProcAddress("glGetTexLevelParameteriv");
            if (get_level) {
                get_level(t, 0, 0x1003 /* INTERNAL_FORMAT */, &fmt);
                get_level(t, 0, 0x1000 /* WIDTH */, &w);
            }
            static int n;
            if (n++ < 40)
                debugPrintf("[gles:swz] set A=0x%x tex=%d fmt=0x%x w=%d (r=0x%x)\n", v, tex, fmt, w, g_tex_swz[tex][0]);
        }
    }
}
static int swz_tex_converted(GLenum t) {
    GLint tex = t == GL_TEXTURE_2D || t == GL_TEXTURE_3D ? bound_tex2d(t) : 0;
    return tex > 0 && tex < BS_TEX_FMT_MAX && g_tex_a8[tex];
}
static void bs_glTexParameteri(GLenum t, GLenum p, GLint v) {
    if (p >= 0x8E42 && p <= 0x8E45) { /* GL_TEXTURE_SWIZZLE_R..A */
        swz_record(t, p, v);
        if (swz_tex_converted(t)) return; /* already baked into RGBA8 */
    }
    glTexParameteri(t, p, v);
}
static void bs_glTexParameteriv(GLenum t, GLenum p, const GLint *v) {
    if (p == 0x8E46 && v) { /* GL_TEXTURE_SWIZZLE_RGBA */
        for (int i = 0; i < 4; ++i) swz_record(t, 0x8E42 + i, v[i]);
        if (swz_tex_converted(t)) return;
    } else if (p >= 0x8E42 && p <= 0x8E45 && v) {
        swz_record(t, p, v[0]);
        if (swz_tex_converted(t)) return;
    }
    glTexParameteriv(t, p, v);
}
/* GL_RED/UNSIGNED_BYTE texels -> RGBA (0,0,0,a), honouring unpack state.
 * d > 0 selects a 3D upload at slice z. */
static int a8_upload3(GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                      GLsizei d, GLenum f, GLenum ty, const void *p) {
    if (!swz_tex_converted(t) || f != GL_RED || ty != GL_UNSIGNED_BYTE || w <= 0 || h <= 0)
        return 0;
    int is3d = t == GL_TEXTURE_3D;
    if (is3d && d <= 0) return 0;
    GLsizei depth = is3d ? d : 1;
    GLint pbo = 0, align = 4, row_len = 0, skip_px = 0, skip_rows = 0, img_h = 0, skip_img = 0;
    glGetIntegerv(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */, &pbo);
    if (pbo || !p) {
        static int n;
        if (n++ < 4) debugPrintf("[gles:tex] Alpha8 upload via PBO/null not converted (pbo=%d)\n", pbo);
        return 0;
    }
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_len);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_px);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    if (is3d) {
        glGetIntegerv(0x806E /* UNPACK_IMAGE_HEIGHT */, &img_h);
        glGetIntegerv(0x806D /* UNPACK_SKIP_IMAGES */, &skip_img);
    }
    size_t rl = (size_t)(row_len > 0 ? row_len : w);
    size_t a = align > 0 ? (size_t)align : 1;
    size_t stride = (rl + a - 1) / a * a;
    size_t slice = stride * (size_t)(img_h > 0 ? img_h : h);
    unsigned char *rgba = malloc((size_t)w * (size_t)h * (size_t)depth * 4);
    if (!rgba) return 0;
    const unsigned char *src = (const unsigned char *)p + (size_t)skip_img * slice +
                               (size_t)skip_rows * stride + (size_t)skip_px;
    for (GLsizei k = 0; k < depth; ++k)
        for (GLsizei r = 0; r < h; ++r) {
            const unsigned char *s = src + (size_t)k * slice + (size_t)r * stride;
            unsigned char *o = rgba + (((size_t)k * (size_t)h + (size_t)r) * (size_t)w) * 4;
            for (GLsizei c = 0; c < w; ++c) {
                o[c * 4 + 0] = 0; o[c * 4 + 1] = 0; o[c * 4 + 2] = 0; o[c * 4 + 3] = s[c];
            }
        }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    if (is3d) {
        glPixelStorei(0x806E, 0);
        glPixelStorei(0x806D, 0);
        glTexSubImage3D(t, l, x, y, z, w, h, d, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glPixelStorei(0x806E, img_h);
        glPixelStorei(0x806D, skip_img);
    } else {
        glTexSubImage2D(t, l, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_len);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_px);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
    free(rgba);
    return 1;
}
static int a8_upload(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h,
                     GLenum f, GLenum ty, const void *p) {
    return a8_upload3(t, l, x, y, 0, w, h, 0, f, ty, p);
}
static void bs_glTexStorage3D(GLenum t, GLsizei lv, GLenum f, GLsizei w, GLsizei h, GLsizei d) {
    /* Only GL_TEXTURE_3D: bound_tex2d() falls back to the 2D binding for
     * other targets (cube arrays), which must not clear a 2D texture's state. */
    GLint tex = t == GL_TEXTURE_3D ? bound_tex2d(t) : 0;
    int ok = tex > 0 && tex < BS_TEX_FMT_MAX;
    if (ok) g_tex_a8[tex] = 0;
    if (ok && f == GL_R8 && swz_is_alpha8(tex)) {
        /* Same swizzle loss as 2D Alpha8: the note dissolve noise
         * (_CutoutTex) is a 3D Alpha8 texture. */
        glTexStorage3D(t, lv, GL_RGBA8, w, h, d);
        swz_identity(t);
        g_tex_a8[tex] = 1;
        static int n;
        if (n++ < 8)
            debugPrintf("[gles:tex] Alpha8 3D tex=%d %dx%dx%d levels=%d -> RGBA8 (0,0,0,a)\n", tex, w, h, d, lv);
        tex_log("TexStorage3D", t, 0, GL_RGBA8, lv, w, h, d, 0);
        return;
    }
    glTexStorage3D(t, lv, f, w, h, d);
    tex_log("TexStorage3D", t, 0, f, lv, w, h, d, 0);
}
static void bs_glTexImage2D(GLenum t, GLint l, GLint inf, GLsizei w, GLsizei h, GLint b, GLenum f,
                            GLenum ty, const void *p) {
    GLint tex = t == GL_TEXTURE_2D ? bound_tex2d(t) : 0;
    if (tex > 0 && tex < BS_TEX_FMT_MAX) {
        if (l == 0) g_tex_a8[tex] = 0;
        if (inf == GL_R8 && f == GL_RED && ty == GL_UNSIGNED_BYTE &&
            (g_tex_a8[tex] || (l == 0 && swz_is_alpha8(tex)))) {
            glTexImage2D(t, l, GL_RGBA8, w, h, b, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            if (l == 0) { swz_identity(t); g_tex_a8[tex] = 1; }
            if (p) a8_upload(t, l, 0, 0, w, h, f, ty, p);
            tex_log("TexImage2D", t, l, GL_RGBA8, ty, w, h, 1, 0);
            return;
        }
    }
    if (l == 0) swz_log("TexImage2D", t, (GLenum)inf, w, h);
    glTexImage2D(t, l, inf, w, h, b, f, ty, p);
    tex_log("TexImage2D", t, l, (GLenum)inf, ty, w, h, 1, 0);
}
static void bs_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f,
                               GLenum ty, const void *p) {
    if (!a8_upload(t, l, x, y, w, h, f, ty, p))
        glTexSubImage2D(t, l, x, y, w, h, f, ty, p);
    tex_log("TexSubImage2D", t, l, f, ty, w, h, 1, 0);
}
static void bs_glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b,
                                      GLsizei sz, const void *p) {
    if (l == 0) swz_log("CompressedTexImage2D", t, f, w, h);
    glCompressedTexImage2D(t, l, f, w, h, b, sz, p);
    tex_log("CompressedTexImage2D", t, l, f, 0, w, h, 1, sz);
}
static void bs_glCompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h,
                                         GLenum f, GLsizei sz, const void *p) {
    glCompressedTexSubImage2D(t, l, x, y, w, h, f, sz, p);
    tex_log("CompressedTexSubImage2D", t, l, f, 0, w, h, 1, sz);
}
static void bs_glTexSubImage3D(GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                               GLsizei d, GLenum f, GLenum ty, const void *p) {
    if (!a8_upload3(t, l, x, y, z, w, h, d, f, ty, p))
        glTexSubImage3D(t, l, x, y, z, w, h, d, f, ty, p);
    tex_log("TexSubImage3D", t, l, f, ty, w, h, d, 0);
}
static void bs_glCompressedTexSubImage3D(GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w,
                                         GLsizei h, GLsizei d, GLenum f, GLsizei sz, const void *p) {
    glCompressedTexSubImage3D(t, l, x, y, z, w, h, d, f, sz, p);
    tex_log("CompressedTexSubImage3D", t, l, f, 0, w, h, d, sz);
}
static void bs_glGenerateMipmap(GLenum t) {
    glGenerateMipmap(t);
    tex_log("GenerateMipmap", t, 0, 0, 0, 0, 0, 0, 0);
}

static const BsEglEntry g_wrapped[] = {
    {"glDeleteTextures", (void *)bs_glDeleteTextures},
    {"glTexStorage2D", (void *)bs_glTexStorage2D},
    {"glTexParameteri", (void *)bs_glTexParameteri},
    {"glTexParameteriv", (void *)bs_glTexParameteriv},
    {"glTexStorage3D", (void *)bs_glTexStorage3D},
    {"glTexImage2D", (void *)bs_glTexImage2D},
    {"glTexSubImage2D", (void *)bs_glTexSubImage2D},
    {"glCompressedTexImage2D", (void *)bs_glCompressedTexImage2D},
    {"glCompressedTexSubImage2D", (void *)bs_glCompressedTexSubImage2D},
    {"glTexSubImage3D", (void *)bs_glTexSubImage3D},
    {"glCompressedTexSubImage3D", (void *)bs_glCompressedTexSubImage3D},
    {"glGenerateMipmap", (void *)bs_glGenerateMipmap},
    {"eglGetDisplay", (void *)bs_eglGetDisplay},
    {"eglInitialize", (void *)bs_eglInitialize},
    {"eglChooseConfig", (void *)bs_eglChooseConfig},
    {"eglCreateContext", (void *)bs_eglCreateContext},
    {"eglCreateWindowSurface", (void *)bs_eglCreateWindowSurface},
    {"eglCreatePbufferSurface", (void *)bs_eglCreatePbufferSurface},
    {"eglMakeCurrent", (void *)bs_eglMakeCurrent},
    {"eglSwapBuffers", (void *)bs_eglSwapBuffers},
    {"eglGetProcAddress", (void *)bs_eglGetProcAddress},
    /* Direct Mesa entry points (no Android translation required). */
    {"eglTerminate", (void *)eglTerminate},
    {"eglBindAPI", (void *)eglBindAPI},
    {"eglDestroySurface", (void *)bs_eglDestroySurface},
    {"eglDestroyContext", (void *)eglDestroyContext},
    {"eglGetCurrentContext", (void *)bs_eglGetCurrentContext},
    {"eglGetCurrentSurface", (void *)bs_eglGetCurrentSurface},
    {"eglGetCurrentDisplay", (void *)eglGetCurrentDisplay},
    {"eglSwapInterval", (void *)eglSwapInterval},
    {"eglGetError", (void *)eglGetError},
    {"eglGetConfigAttrib", (void *)eglGetConfigAttrib},
    {"eglGetConfigs", (void *)eglGetConfigs},
    {"eglQueryContext", (void *)eglQueryContext},
    {"eglQuerySurface", (void *)bs_eglQuerySurface},
    {"eglQueryString", (void *)eglQueryString},
    {"eglSurfaceAttrib", (void *)eglSurfaceAttrib},
    {"eglReleaseThread", (void *)eglReleaseThread},
    {"eglWaitGL", (void *)eglWaitGL},
    {"eglWaitClient", (void *)eglWaitClient},
    {"glClear", (void *)bs_glClear},
    {"glClearColor", (void *)bs_glClearColor},
    {"glDrawArraysInstanced", (void *)bs_glDrawArraysInstanced},
    {"glBlitFramebuffer", (void *)bs_glBlitFramebuffer},
    {"glDrawArrays", (void *)bs_glDrawArrays},
    {"glDrawElements", (void *)bs_glDrawElements},
    {"glDrawElementsInstanced", (void *)bs_glDrawElementsInstanced},
    {"glBindFramebuffer", (void *)bs_glBindFramebuffer},
    {"glFlush", (void *)bs_glFlush},
    {"glUseProgram", (void *)bs_glUseProgram},
    {"glShaderSource", (void *)bs_glShaderSource},
    {"glFramebufferTextureMultiviewOVR", (void *)bs_glFramebufferTextureMultiviewOVR},
    {"glFramebufferTexture2D", (void *)bs_glFramebufferTexture2D},
    {"glFramebufferTextureLayer", (void *)bs_glFramebufferTextureLayer},
    {"glFramebufferRenderbuffer", (void *)bs_glFramebufferRenderbuffer},
    {"glDeleteFramebuffers", (void *)bs_glDeleteFramebuffers},
    {"glGetFramebufferAttachmentParameteriv", (void *)bs_glGetFramebufferAttachmentParameteriv},
    {"glGetStringi", (void *)bs_glGetStringi},
    {"glDrawRangeElements", (void *)bs_glDrawRangeElements},
    {"glDrawElementsBaseVertex", (void *)bs_glDrawElementsBaseVertex},
    {"glDrawElementsBaseVertexEXT", (void *)bs_glDrawElementsBaseVertex},
    {"glDrawElementsBaseVertexOES", (void *)bs_glDrawElementsBaseVertex},
    {"glDrawElementsInstancedBaseVertex", (void *)bs_glDrawElementsInstancedBaseVertex},
    {"glDrawElementsInstancedBaseVertexEXT", (void *)bs_glDrawElementsInstancedBaseVertex},
    {"glDrawElementsInstancedBaseVertexOES", (void *)bs_glDrawElementsInstancedBaseVertex},
    {"glDrawRangeElementsBaseVertex", (void *)bs_glDrawRangeElementsBaseVertex},
    {"glDrawRangeElementsBaseVertexEXT", (void *)bs_glDrawRangeElementsBaseVertex},
    {"glDrawRangeElementsBaseVertexOES", (void *)bs_glDrawRangeElementsBaseVertex},
    {"glVertexAttribDivisor", (void *)bs_glVertexAttribDivisor},
    {"glCompileShader", (void *)bs_glCompileShader},
    {"glLinkProgram", (void *)bs_glLinkProgram},
    {"glAttachShader", (void *)bs_glAttachShader},
    {"glDispatchCompute", (void *)bs_glDispatchCompute},
    {"glBufferData", (void *)bs_glBufferData},
    {"glMemoryBarrier", (void *)bs_glMemoryBarrier},
    {"glCreateProgram", (void *)bs_glCreateProgram},
    {"glGetString", (void *)bs_glGetString},
    {"glGetIntegerv", (void *)bs_glGetIntegerv},
    {"glGetError", (void *)glGetError},
};

void *bs_egl_symbol(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_wrapped / sizeof g_wrapped[0]; ++i)
        if (!strcmp(g_wrapped[i].name, name)) return g_wrapped[i].fn;
    if (strncmp(name, "gl", 2) && strncmp(name, "egl", 3)) return NULL;
    void *proc = (void *)eglGetProcAddress(name);
    if (!proc) BS_EGL_LOG("[gles] no Mesa entry for %s\n", name);
    return proc;
}

static __eglMustCastToProperFunctionPointerType bs_eglGetProcAddress(const char *name) {
    return (__eglMustCastToProperFunctionPointerType)bs_egl_symbol(name);
}

/* Host-thread context creation before any guest code runs. Separates Mesa or
 * driver problems from state the guest established later. */
int bs_egl_host_preflight(void) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(d, &major, &minor)) {
        debugPrintf("[egl:preflight] eglInitialize failed 0x%x\n", eglGetError());
        return -1;
    }
    static const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(d, cfg_attr, &cfg, 1, &n) || n < 1) {
        debugPrintf("[egl:preflight] no ES3 config 0x%x\n", eglGetError());
        return -2;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    debugPrintf("[egl:preflight] creating ES3.2 context on host thread\n");
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (!c) {
        debugPrintf("[egl:preflight] ES3.2 refused 0x%x, trying ES3.1\n", eglGetError());
        ctx_attr[3] = 1;
        c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
    }
    debugPrintf("[egl:preflight] context=%p err=0x%x\n", c, c ? 0 : eglGetError());
    if (!c) return -3;
    eglDestroyContext(d, c);
    return 0;
}
