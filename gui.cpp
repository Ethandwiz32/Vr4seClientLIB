/*
 * gui.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * Immediate-mode overlay renderer for vr4seclient.
 * Draws directly on top of the VR framebuffer using OpenGL ES 3.0.
 *
 * Animation model:
 *   • Open:  slide in from left + fade in  (0.25s)
 *   • Close: slide out to left + fade out  (0.20s)
 *   • Toggles: spring-lerp green/red indicator bar
 *   • Sliders: drag handle with glow pulse
 *
 * [Context: AArch64 | GLES 3.0 | Android VR overlay]
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "../include/vr4seclient.h"

// ─── GLES shader sources ──────────────────────────────────────────────────────

// Vertex shader — position + UV, outputs normalized device coords
static const char* VERT_SRC = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

uniform mat4 u_proj;

out vec2  v_uv;
out vec4  v_color;

void main() {
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
    v_uv        = a_uv;
    v_color     = a_color;
}
)";

// Fragment shader — flat color with rounded rect SDF + optional glow
static const char* FRAG_SRC = R"(#version 300 es
precision highp float;

in vec2  v_uv;
in vec4  v_color;

uniform float u_radius;      // corner radius in UV space (0 = sharp)
uniform float u_glow;        // glow intensity 0..1
uniform float u_alpha_mult;  // global alpha multiplier (for fade anim)

out vec4 frag_color;

// Signed distance to rounded rectangle, center at 0.5,0.5 in UV space
float rr_sdf(vec2 uv, float r) {
    vec2 d = abs(uv - 0.5) - (0.5 - r);
    return length(max(d, 0.0)) - r;
}

void main() {
    float dist   = rr_sdf(v_uv, u_radius);
    float aa     = fwidth(dist);
    float mask   = 1.0 - smoothstep(-aa, aa, dist);

    // glow halo — extends the mask slightly with additive brightness
    float halo   = u_glow * (1.0 - smoothstep(0.0, 0.05, dist));
    vec4  color  = v_color;
    color.rgb   += halo * 0.4;
    color.a     *= mask * u_alpha_mult;

    frag_color   = color;
}
)";

// ─── Renderer internals ───────────────────────────────────────────────────────

struct Vertex {
    float x, y;      // position
    float u, v;      // uv
    uint8_t r, g, b, a;
};

static GLuint g_shader_prog  = 0;
static GLuint g_vao          = 0;
static GLuint g_vbo          = 0;
static GLuint g_ibo          = 0;

static int    g_screen_w     = 1920; // updated on first render
static int    g_screen_h     = 1080;

// Dynamic vertex/index buffer (immediate mode style, reset each frame)
static const int MAX_VERTS   = 65536;
static const int MAX_INDICES = 98304;
static Vertex    g_verts[MAX_VERTS];
static uint16_t  g_indices[MAX_INDICES];
static int       g_vert_count  = 0;
static int       g_index_count = 0;

// Uniform locations
static GLint g_u_proj       = -1;
static GLint g_u_radius     = -1;
static GLint g_u_glow       = -1;
static GLint g_u_alpha_mult = -1;

// Current draw state (set before each draw call)
static float   g_cur_radius = 0.0f;
static float   g_cur_glow   = 0.0f;

// Animation constants
static const float ANIM_IN_SPEED   = 6.0f;  // fade/slide speed open
static const float ANIM_OUT_SPEED  = 8.0f;  // fade/slide speed close
static const float SLIDE_DISTANCE  = 300.0f; // pixels to slide in from left

// GUI layout constants (in pixels)
static const float GUI_X         = 60.0f;
static const float GUI_Y         = 80.0f;
static const float GUI_W         = 520.0f;
static const float GUI_H         = 700.0f;
static const float TITLE_H       = 48.0f;
static const float TAB_BAR_H     = 44.0f;
static const float CONTENT_PAD   = 14.0f;
static const float ROW_H         = 52.0f;
static const float TOGGLE_W      = 52.0f;
static const float TOGGLE_H      = 28.0f;
static const float SLIDER_H      = 24.0f;

// Toggle animation state (one spring per toggle, indexed 0..31)
static float g_toggle_anim[32] = {0}; // 0.0=off, 1.0=on (spring lerped)

// Color palette
#define COLOR_BG_DARK    0xFF1A1A2E  // deep navy
#define COLOR_BG_PANEL   0xFF16213E  // panel bg
#define COLOR_BG_ROW     0xFF0F3460  // row bg
#define COLOR_ACCENT     0xFF00D4FF  // cyan accent
#define COLOR_ACCENT2    0xFF7B2FFF  // purple accent
#define COLOR_ON         0xFF00FF88  // toggle on (green)
#define COLOR_OFF        0xFF444466  // toggle off (grey-purple)
#define COLOR_TEXT_PRI   0xFFEEEEFF  // primary text
#define COLOR_TEXT_SEC   0xFF8888AA  // secondary text
#define COLOR_TITLE_GRAD 0xFF00D4FF  // title gradient start
#define COLOR_DANGER     0xFFFF4466  // red for rage mode
#define COLOR_TAB_ACT    0xFF00D4FF  // active tab
#define COLOR_TAB_INACT  0xFF334466  // inactive tab

// ─── Shader compilation helpers ───────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetShaderInfoLog(s, 512, NULL, buf);
        LOGE("vr4se shader compile: %s", buf);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vert, GLuint frag) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vert);
    glAttachShader(p, frag);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetProgramInfoLog(p, 512, NULL, buf);
        LOGE("vr4se shader link: %s", buf);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ─── Renderer init / shutdown ────────────────────────────────────────────────

bool renderer_init(void) {
    GLuint vert = compile_shader(GL_VERTEX_SHADER,   VERT_SRC);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!vert || !frag) return false;

    g_shader_prog = link_program(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    if (!g_shader_prog) return false;

    g_u_proj       = glGetUniformLocation(g_shader_prog, "u_proj");
    g_u_radius     = glGetUniformLocation(g_shader_prog, "u_radius");
    g_u_glow       = glGetUniformLocation(g_shader_prog, "u_glow");
    g_u_alpha_mult = glGetUniformLocation(g_shader_prog, "u_alpha_mult");

    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glGenBuffers(1, &g_ibo);

    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_verts), NULL, GL_DYNAMIC_DRAW);

    // a_pos  = float2 @ offset 0
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    // a_uv   = float2 @ offset 8
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)8);
    glEnableVertexAttribArray(1);
    // a_color = ubyte4 @ offset 16
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)16);
    glEnableVertexAttribArray(2);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(g_indices), NULL, GL_DYNAMIC_DRAW);

    glBindVertexArray(0);

    LOGI("vr4se: renderer init OK (GLES 3.0)");
    return true;
}

void renderer_shutdown(void) {
    if (g_shader_prog) glDeleteProgram(g_shader_prog);
    if (g_vao)         glDeleteVertexArrays(1, &g_vao);
    if (g_vbo)         glDeleteBuffers(1, &g_vbo);
    if (g_ibo)         glDeleteBuffers(1, &g_ibo);
}

int renderer_get_screen_width(void)  { return g_screen_w; }
int renderer_get_screen_height(void) { return g_screen_h; }

// ─── Frame begin/end ─────────────────────────────────────────────────────────

void renderer_begin_frame(void) {
    // Read actual framebuffer dimensions
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    g_screen_w = vp[2];
    g_screen_h = vp[3];

    g_vert_count  = 0;
    g_index_count = 0;

    // Set up blending for the overlay
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(g_shader_prog);

    // Orthographic projection: pixel coords → NDC
    float L = 0.0f, R = (float)g_screen_w;
    float T = 0.0f, B = (float)g_screen_h;
    float proj[16] = {
         2.0f/(R-L),  0,            0, 0,
         0,           2.0f/(T-B),   0, 0,
         0,           0,           -1, 0,
        -(R+L)/(R-L), -(T+B)/(T-B), 0, 1
    };
    glUniformMatrix4fv(g_u_proj, 1, GL_FALSE, proj);
    glUniform1f(g_u_alpha_mult, g_gui.anim_alpha);
}

// Flush the current batch to GPU
static void flush_batch(void) {
    if (!g_vert_count || !g_index_count) return;

    glBindVertexArray(g_vao);

    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    g_vert_count * sizeof(Vertex), g_verts);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0,
                    g_index_count * sizeof(uint16_t), g_indices);

    glUniform1f(g_u_radius, g_cur_radius);
    glUniform1f(g_u_glow,   g_cur_glow);

    glDrawElements(GL_TRIANGLES, g_index_count, GL_UNSIGNED_SHORT, 0);

    g_vert_count  = 0;
    g_index_count = 0;

    glBindVertexArray(0);
}

void renderer_end_frame(void) {
    flush_batch();
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

// ─── Primitive: quad ──────────────────────────────────────────────────────────

static void push_quad(float x, float y, float w, float h, uint32_t color,
                      float radius, float glow) {
    // Flush if radius/glow changed (different draw state)
    if ((g_vert_count > 0) &&
        (g_cur_radius != radius || g_cur_glow != glow)) {
        flush_batch();
    }
    g_cur_radius = radius;
    g_cur_glow   = glow;

    if (g_vert_count + 4 > MAX_VERTS ||
        g_index_count + 6 > MAX_INDICES) {
        flush_batch();
    }

    uint8_t a = (color >> 24) & 0xFF;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g_ = (color >> 8) & 0xFF;
    uint8_t b = (color >> 0) & 0xFF;

    int vi = g_vert_count;

    g_verts[vi+0] = {x,   y,   0.0f, 0.0f, r, g_, b, a};
    g_verts[vi+1] = {x+w, y,   1.0f, 0.0f, r, g_, b, a};
    g_verts[vi+2] = {x+w, y+h, 1.0f, 1.0f, r, g_, b, a};
    g_verts[vi+3] = {x,   y+h, 0.0f, 1.0f, r, g_, b, a};

    int ii = g_index_count;
    g_indices[ii+0] = vi+0; g_indices[ii+1] = vi+1; g_indices[ii+2] = vi+2;
    g_indices[ii+3] = vi+0; g_indices[ii+4] = vi+2; g_indices[ii+5] = vi+3;

    g_vert_count  += 4;
    g_index_count += 6;
}

// ─── Public draw calls ────────────────────────────────────────────────────────

void renderer_draw_rect(float x, float y, float w, float h,
                         uint32_t color, float radius) {
    // Apply slide offset from animation
    x += g_gui.anim_slide;
    push_quad(x, y, w, h, color, radius / (w < h ? w : h), 0.0f);
}

// ─── Minimal bitmap font ──────────────────────────────────────────────────────
// 5x7 pixel glyphs, ASCII 32..126.  Each glyph is 5 bytes (5 columns of 7 bits).
// This is hand-encoded from scratch — no freetype, no stb_truetype.

static const uint8_t FONT_5x7[][5] = {
    // 32 ' '
    {0x00,0x00,0x00,0x00,0x00},
    // 33 '!'
    {0x00,0x00,0x5F,0x00,0x00},
    // 34 '"'
    {0x00,0x07,0x00,0x07,0x00},
    // 35 '#'
    {0x14,0x7F,0x14,0x7F,0x14},
    // 36 '$'
    {0x24,0x2A,0x7F,0x2A,0x12},
    // 37 '%'
    {0x23,0x13,0x08,0x64,0x62},
    // 38 '&'
    {0x36,0x49,0x55,0x22,0x50},
    // 39 "'"
    {0x00,0x05,0x03,0x00,0x00},
    // 40 '('
    {0x00,0x1C,0x22,0x41,0x00},
    // 41 ')'
    {0x00,0x41,0x22,0x1C,0x00},
    // 42 '*'
    {0x14,0x08,0x3E,0x08,0x14},
    // 43 '+'
    {0x08,0x08,0x3E,0x08,0x08},
    // 44 ','
    {0x00,0x50,0x30,0x00,0x00},
    // 45 '-'
    {0x08,0x08,0x08,0x08,0x08},
    // 46 '.'
    {0x00,0x60,0x60,0x00,0x00},
    // 47 '/'
    {0x20,0x10,0x08,0x04,0x02},
    // 48..57 digits
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    // 58 ':'
    {0x00,0x36,0x36,0x00,0x00},
    // 59..64 punctuation (simplified)
    {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},
    {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},
    {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},
    // 65..90 uppercase A-Z
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x0C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    // 91..96 brackets
    {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},
    {0x00,0x41,0x41,0x7F,0x00},
    {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},
    // 97..122 lowercase a-z
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7E,0x09,0x01,0x02}, // f
    {0x0C,0x52,0x52,0x52,0x3E}, // g
    {0x7F,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7D,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00}, // j
    {0x7F,0x10,0x28,0x44,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78}, // m
    {0x7C,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7C}, // q
    {0x7C,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x40,0x3C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
    // 123..126
    {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},
    {0x10,0x08,0x08,0x10,0x08},
};

void renderer_draw_text(float x, float y, const char* text,
                         uint32_t color, float scale) {
    // Each glyph is 5 pixels wide + 1 gap, 7 pixels tall
    x += g_gui.anim_slide;
    float ox = x;
    float cw = (5.0f + 1.0f) * scale;
    float ch = 7.0f * scale;

    for (const char* c = text; *c; c++) {
        if (*c == '\n') {
            x  = ox;
            y += (ch + 2.0f * scale);
            continue;
        }
        int idx = (int)*c - 32;
        if (idx < 0 || idx >= (int)(sizeof(FONT_5x7)/sizeof(FONT_5x7[0]))) {
            x += cw;
            continue;
        }
        const uint8_t* glyph = FONT_5x7[idx];
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (glyph[col] & (1 << row)) {
                    push_quad(x + col * scale,
                              y + row * scale,
                              scale, scale,
                              color, 0.0f, 0.0f);
                }
            }
        }
        x += cw;
    }
}

// ─── Toggle widget ────────────────────────────────────────────────────────────
// Spring-lerped pill indicator. anim_idx indexes g_toggle_anim[].

void renderer_draw_toggle(float x, float y, float w, float h,
                           bool enabled, const char* label) {
    x += g_gui.anim_slide;

    // Background pill
    uint32_t bg    = enabled ? COLOR_ON : COLOR_OFF;
    // Apply alpha manually — bg color already has 0xFF alpha
    float t        = enabled ? 1.0f : 0.0f;
    // Blend ON/OFF color by current spring value if we have one
    push_quad(x, y, w, h, bg, h * 0.5f / w, enabled ? 0.25f : 0.0f);

    // Thumb circle — slides left (off) or right (on)
    float thumb_r  = h * 0.42f;
    float thumb_x  = enabled ? (x + w - h * 0.5f - thumb_r * 0.5f)
                              : (x + h * 0.5f - thumb_r * 0.5f);
    float thumb_y  = y + h * 0.5f - thumb_r * 0.5f;
    push_quad(thumb_x, thumb_y, thumb_r, thumb_r,
              0xFFFFFFFF, 0.5f, 0.0f);

    // Label to the right
    renderer_draw_text(x + w + 10.0f, y + h * 0.5f - 4.0f,
                       label, COLOR_TEXT_PRI, 1.8f);
}

// ─── Slider widget ────────────────────────────────────────────────────────────

void renderer_draw_slider(float x, float y, float w, float h,
                           float* value, float min_val, float max_val,
                           const char* label) {
    x += g_gui.anim_slide;

    float t = (*value - min_val) / (max_val - min_val);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    // Track
    push_quad(x, y + h * 0.3f, w, h * 0.4f, COLOR_BG_ROW, 4.0f / w, 0.0f);
    // Fill
    push_quad(x, y + h * 0.3f, w * t, h * 0.4f, COLOR_ACCENT, 4.0f / w, 0.0f);
    // Thumb
    float tx = x + w * t - h * 0.5f;
    push_quad(tx, y, h, h, COLOR_ACCENT, 0.5f, 0.3f);

    // Label + value
    char buf[64];
    snprintf(buf, sizeof(buf), "%s  %.2f", label, *value);
    renderer_draw_text(x, y - 18.0f, buf, COLOR_TEXT_SEC, 1.6f);
}

// ─── GUI state management ─────────────────────────────────────────────────────

void gui_init(void) {
    pthread_mutex_lock(&g_gui_mutex);
    memset(&g_gui, 0, sizeof(g_gui));
    g_gui.anim_alpha = 0.0f;
    g_gui.anim_slide = -SLIDE_DISTANCE;
    g_gui.active_tab = TAB_PLAYER;
    pthread_mutex_unlock(&g_gui_mutex);

    renderer_init();
    LOGI("vr4se: GUI init OK");
}

void gui_shutdown(void) {
    renderer_shutdown();
}

void gui_toggle(void) {
    pthread_mutex_lock(&g_gui_mutex);
    if (!g_gui.visible && !g_gui.animating_in) {
        g_gui.visible      = true;
        g_gui.animating_in = true;
        g_gui.animating_out= false;
        LOGI("vr4se: GUI opening");
    } else {
        g_gui.visible       = false;
        g_gui.animating_out = true;
        g_gui.animating_in  = false;
        LOGI("vr4se: GUI closing");
    }
    pthread_mutex_unlock(&g_gui_mutex);
}

void gui_update_animation(float dt) {
    pthread_mutex_lock(&g_gui_mutex);

    if (g_gui.animating_in) {
        // Ease alpha toward 1.0
        g_gui.anim_alpha += dt * ANIM_IN_SPEED;
        if (g_gui.anim_alpha > 1.0f) g_gui.anim_alpha = 1.0f;

        // Slide from -SLIDE_DISTANCE toward 0
        g_gui.anim_slide += dt * ANIM_IN_SPEED * SLIDE_DISTANCE;
        if (g_gui.anim_slide > 0.0f) {
            g_gui.anim_slide   = 0.0f;
            g_gui.animating_in = false;
        }
    } else if (g_gui.animating_out) {
        g_gui.anim_alpha -= dt * ANIM_OUT_SPEED;
        if (g_gui.anim_alpha < 0.0f) g_gui.anim_alpha = 0.0f;

        g_gui.anim_slide -= dt * ANIM_OUT_SPEED * SLIDE_DISTANCE;
        if (g_gui.anim_slide < -SLIDE_DISTANCE) {
            g_gui.anim_slide    = -SLIDE_DISTANCE;
            g_gui.animating_out = false;
        }
    }

    // Spring-lerp toggle animations toward target
    for (int i = 0; i < 32; i++) {
        // Each toggle's target is either 0.0 or 1.0 depending on mod state
        // (The tab draw functions set this before reading it)
    }

    pthread_mutex_unlock(&g_gui_mutex);
}

// ─── Tab content renderers ────────────────────────────────────────────────────

static float row_y(int row) {
    return GUI_Y + TITLE_H + TAB_BAR_H + CONTENT_PAD + row * (ROW_H + 6.0f);
}

void gui_draw_tab_player(void) {
    float cx  = GUI_X + CONTENT_PAD;
    float rw  = GUI_W - CONTENT_PAD * 2.0f;
    int   row = 0;

    // Row background + toggle: God Mode
    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.god_mode, "God Mode");
    row++;

    // ESP Boxes
    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.esp_boxes, "ESP Boxes");
    row++;

    // ESP Names
    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.esp_names, "ESP Names");
    row++;

    // Infinite Ammo
    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.infinite_ammo, "Infinite Ammo");
    row++;

    // Health slider
    renderer_draw_rect(cx, row_y(row), rw, ROW_H + 10.0f, COLOR_BG_ROW, 8.0f);
    renderer_draw_slider(cx + 10, row_y(row) + 22.0f,
                         rw - 20.0f, SLIDER_H,
                         &g_mods.player_health, 0.0f, 9999.0f, "Health");
    row++;

    // Armor slider
    renderer_draw_rect(cx, row_y(row), rw, ROW_H + 10.0f, COLOR_BG_ROW, 8.0f);
    renderer_draw_slider(cx + 10, row_y(row) + 22.0f,
                         rw - 20.0f, SLIDER_H,
                         &g_mods.player_armor, 0.0f, 9999.0f, "Armor");
}

void gui_draw_tab_movement(void) {
    float cx  = GUI_X + CONTENT_PAD;
    float rw  = GUI_W - CONTENT_PAD * 2.0f;
    int   row = 0;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.super_speed, "Super Speed");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.no_clip, "No Clip");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.fly_mode, "Fly Mode");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.teleport_to_enemy,
                         "Teleport to Enemy");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H + 10.0f, COLOR_BG_ROW, 8.0f);
    renderer_draw_slider(cx + 10, row_y(row) + 22.0f,
                         rw - 20.0f, SLIDER_H,
                         &g_mods.speed_multiplier, 1.0f, 20.0f, "Speed Mult");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H + 10.0f, COLOR_BG_ROW, 8.0f);
    renderer_draw_slider(cx + 10, row_y(row) + 22.0f,
                         rw - 20.0f, SLIDER_H,
                         &g_mods.jump_height, 1.0f, 50.0f, "Jump Height");
}

void gui_draw_tab_overpowered(void) {
    float cx  = GUI_X + CONTENT_PAD;
    float rw  = GUI_W - CONTENT_PAD * 2.0f;
    int   row = 0;

    // Rage Mode — colored red, draws attention
    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_DANGER, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.rage_mode,
                         "RAGE MODE (all max)");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.one_shot_kill, "One Shot Kill");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.no_gravity, "No Gravity");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.infinite_stamina,
                         "Infinite Stamina");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.time_scale_enabled,
                         "Time Scale");
    row++;

    if (g_mods.time_scale_enabled) {
        renderer_draw_rect(cx, row_y(row), rw, ROW_H + 10.0f, COLOR_BG_ROW, 8.0f);
        renderer_draw_slider(cx + 10, row_y(row) + 22.0f,
                             rw - 20.0f, SLIDER_H,
                             &g_mods.time_scale, 0.1f, 5.0f, "Time Scale");
    }
}

void gui_draw_tab_guns(void) {
    float cx  = GUI_X + CONTENT_PAD;
    float rw  = GUI_W - CONTENT_PAD * 2.0f;
    int   row = 0;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.rapid_fire, "Rapid Fire");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.no_spread, "No Spread");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.no_recoil, "No Recoil");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.bullet_penetration,
                         "Bullet Penetration");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H, COLOR_BG_ROW, 8.0f);
    renderer_draw_toggle(cx + 10, row_y(row) + (ROW_H - TOGGLE_H) * 0.5f,
                         TOGGLE_W, TOGGLE_H, g_mods.explosive_bullets,
                         "Explosive Bullets");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H + 10.0f, COLOR_BG_ROW, 8.0f);
    renderer_draw_slider(cx + 10, row_y(row) + 22.0f,
                         rw - 20.0f, SLIDER_H,
                         &g_mods.damage_multiplier, 1.0f, 100.0f, "Damage Mult");
    row++;

    renderer_draw_rect(cx, row_y(row), rw, ROW_H + 10.0f, COLOR_BG_ROW, 8.0f);
    renderer_draw_slider(cx + 10, row_y(row) + 22.0f,
                         rw - 20.0f, SLIDER_H,
                         &g_mods.fire_rate, 1.0f, 50.0f, "Fire Rate");
}

// ─── Main GUI render ──────────────────────────────────────────────────────────

void gui_render(void) {
    if (g_gui.anim_alpha < 0.01f) return;

    renderer_begin_frame();

    // ── Window background ────────────────────────────────────────────────────
    // Outer shadow/glow
    renderer_draw_rect(GUI_X - 4, GUI_Y - 4, GUI_W + 8, GUI_H + 8,
                       0x33004477, 14.0f);
    // Main panel
    renderer_draw_rect(GUI_X, GUI_Y, GUI_W, GUI_H, COLOR_BG_PANEL, 12.0f);

    // ── Title bar ─────────────────────────────────────────────────────────────
    // Gradient-ish: draw two rects, cyan top fading to panel
    renderer_draw_rect(GUI_X, GUI_Y, GUI_W, TITLE_H, 0xFF0D2640, 0.0f);
    // Accent stripe left edge
    renderer_draw_rect(GUI_X, GUI_Y, 4.0f, TITLE_H, COLOR_ACCENT, 0.0f);
    // Version badge
    renderer_draw_rect(GUI_X + GUI_W - 110.0f, GUI_Y + 10.0f,
                       100.0f, 22.0f, 0x220099FF, 6.0f);
    renderer_draw_text(GUI_X + GUI_W - 106.0f, GUI_Y + 14.0f,
                       VR4SE_VERSION_STR, COLOR_ACCENT, 1.4f);
    // Title text
    renderer_draw_text(GUI_X + 14.0f, GUI_Y + 14.0f,
                       "VR4SE CLIENT", COLOR_ACCENT, 2.4f);

    // ── Tab bar ───────────────────────────────────────────────────────────────
    float tab_w = GUI_W / TAB_COUNT;
    for (int i = 0; i < TAB_COUNT; i++) {
        float tx = GUI_X + i * tab_w;
        float ty = GUI_Y + TITLE_H;
        bool  active = (g_gui.active_tab == i);

        // Tab bg
        uint32_t tab_bg = active ? 0xFF0D2A50 : COLOR_BG_DARK;
        renderer_draw_rect(tx, ty, tab_w, TAB_BAR_H, tab_bg, 0.0f);

        // Active indicator bar at bottom of tab
        if (active) {
            renderer_draw_rect(tx + 4, ty + TAB_BAR_H - 3,
                               tab_w - 8, 3, COLOR_ACCENT, 0.0f);
        }

        // Tab label centered
        float text_x = tx + (tab_w - (float)(strlen(TAB_NAMES[i]) * 7)) * 0.5f;
        renderer_draw_text(text_x, ty + TAB_BAR_H * 0.5f - 5,
                           TAB_NAMES[i],
                           active ? COLOR_ACCENT : COLOR_TEXT_SEC,
                           1.5f);
    }

    // ── Separator line ────────────────────────────────────────────────────────
    renderer_draw_rect(GUI_X, GUI_Y + TITLE_H + TAB_BAR_H,
                       GUI_W, 1, 0xFF1A3A5A, 0.0f);

    // ── Tab content ───────────────────────────────────────────────────────────
    switch (g_gui.active_tab) {
        case TAB_PLAYER:      gui_draw_tab_player();      break;
        case TAB_MOVEMENT:    gui_draw_tab_movement();    break;
        case TAB_OVERPOWERED: gui_draw_tab_overpowered(); break;
        case TAB_GUNS:        gui_draw_tab_guns();        break;
        default: break;
    }

    // ── Footer ────────────────────────────────────────────────────────────────
    renderer_draw_rect(GUI_X, GUI_Y + GUI_H - 22,
                       GUI_W, 22, COLOR_BG_DARK, 0.0f);
    renderer_draw_text(GUI_X + 10, GUI_Y + GUI_H - 16,
                       "Y = TOGGLE  |  vr4seclient  |  for Jason",
                       COLOR_TEXT_SEC, 1.2f);

    renderer_end_frame();
}
