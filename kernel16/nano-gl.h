// nano-gl.h — a 3D pipeline in fixed point, rendering into a window handle.
//
// This is the layer TinyGL would plug into, built and proved before TinyGL can
// be compiled at all.
//
// TinyGL is 9,146 lines under a permissive zlib-style licence and it cannot be
// built by nano_cc today: 257 uses of GLfloat, 99 float literals, and
// `typedef float GLfloat` in its public header, against a compiler with no
// floating point. What the measurement also showed is that the part doing the
// actual pixel work is already integer -- zbuffer.c has zero floats in 389
// lines, and so do texture.c and the display lists. The float dependency is the
// API, the matrix stack, the clipper and the transform.
//
// So that is what this file is: exactly those stages, in 16.16 fixed point.
// When floats arrive in the compiler, TinyGL's ZBuffer is pointed at the same
// window backing buffer and everything below the binding contract is unchanged.
//
// THE CONTRACT, which is the point of the file:
//
//     gl_bind(&ctx, win, x, y, w, h)   -- take a window handle and a viewport
//     ...draw...
//     gl_flush(&ctx)                   -- invalidate ONLY what was written
//
// The renderer never touches the screen. It writes into the window's backing
// buffer, which is ordinary RAM, and reports the bounding box of the pixels it
// actually changed. A spinning cube therefore costs its own rectangle rather
// than the screen, exactly like a widget or a terminal cell.
//
// On fixed point: the format is the same 16.16 I audited on the C64 last week,
// but one hazard from that job cannot happen here. There, `int` was 16 bits, so
// `(a * b) >> 16` silently threw away the top half of every product and the
// multiply had to be split into four partials. Here `long` is 64 bits and holds
// the full 32x32 product, so the obvious expression is also the correct one.
// Same format, different machine, different answer about what is safe.
//
// Requires nano-wm.h (a window and its backing buffer) and nano-mm.h (kmalloc).

#ifndef NANO_GL_H
#define NANO_GL_H

// ---------- 16.16 fixed point ----------

#define GL_FRAC  16
#define GL_ONE   65536
#define GL_HALF  32768

long fx_from_int(long v) { return v << GL_FRAC; }
long fx_to_int(long v)   { return v >> GL_FRAC; }   // floors, deliberately

// Round to nearest rather than truncate. The only bits discarded are the low
// 16 of the product, so it costs one addition -- and over a transform chain of
// four matrix multiplies, truncation drifts visibly.
long fx_mul(long a, long b) {
    long p;
    p = a * b;
    if (p < 0) return 0 - ((0 - p + GL_HALF) >> GL_FRAC);
    return (p + GL_HALF) >> GL_FRAC;
}

long fx_div(long a, long b) {
    if (b == 0) return a < 0 ? (0 - 0x7FFFFFFFFFFF) : 0x7FFFFFFFFFFF;
    return (a << GL_FRAC) / b;
}

// Restoring bit-by-bit integer square root. Used to normalise face normals
// for the lighting term.
//
// sqrt of a 16.16 value x (representing x/2^16) wants to be 16.16 as well:
// sqrt(x/2^16) * 2^16 = sqrt(x * 2^16). So shift left by 16 and take the plain
// integer root of that. The shift is why this needs 64-bit longs -- a 16.16
// value up to 32768.0 becomes 2^47 before the root is taken.
long fx_sqrt(long a) {
    long n;
    long res;
    long bit;
    if (a <= 0) return 0;
    n = a << GL_FRAC;
    res = 0;
    // The highest power of four <= n, built by shifting a `long` rather than
    // written as `1 << 46`. Where an integer literal is 32 bits that shift is
    // undefined and silently starts the search in the wrong place -- the same
    // class of bug as cc65's 16-bit `int`, and gcc warns about it here.
    bit = 1;
    while (bit <= n >> 2) bit = bit << 2;
    while (bit != 0) {
        if (n >= res + bit) {
            n = n - (res + bit);
            res = (res >> 1) + bit;
        } else {
            res = res >> 1;
        }
        bit = bit >> 2;
    }
    return res;
}

// sin for whole degrees, 0..90, 16.16. Stored as `long` rather than a 16-bit
// type on purpose: on the C64 library I audited, the last entry was 65536 in a
// uint16_t, so it stored as 0 and sin(90) -- and therefore cos(0) -- came out
// zero. A 64-bit container cannot lose it.
long g_gl_sin[91] = {
         0,   1144,   2287,   3430,   4572,   5712,   6850,   7987,
      9121,  10252,  11380,  12505,  13626,  14742,  15855,  16962,
     18064,  19161,  20252,  21336,  22415,  23486,  24550,  25607,
     26656,  27697,  28729,  29753,  30767,  31772,  32768,  33754,
     34729,  35693,  36647,  37590,  38521,  39441,  40348,  41243,
     42126,  42995,  43852,  44695,  45525,  46341,  47143,  47930,
     48703,  49461,  50203,  50931,  51643,  52339,  53020,  53684,
     54332,  54963,  55578,  56175,  56756,  57319,  57865,  58393,
     58903,  59396,  59870,  60326,  60764,  61183,  61584,  61966,
     62328,  62672,  62997,  63303,  63589,  63856,  64104,  64332,
     64540,  64729,  64898,  65048,  65177,  65287,  65376,  65446,
     65496,  65526,  65536
};

// Angle in whole degrees, any sign, any magnitude.
long gl_sin(long deg) {
    long q;
    long i;
    deg = deg % 360;
    if (deg < 0) deg = deg + 360;
    q = deg / 90;
    i = deg % 90;
    if (q == 0) return g_gl_sin[i];
    if (q == 1) return g_gl_sin[90 - i];
    if (q == 2) return 0 - g_gl_sin[i];
    return 0 - g_gl_sin[90 - i];
}

long gl_cos(long deg) { return gl_sin(deg + 90); }

// The same, for an angle that is itself 16.16 degrees, linearly interpolated
// between the two nearest table entries.
//
// Needed because glRotatex takes a fixed-point angle. Rounding it to whole
// degrees instead is visible: a cube turning at a third of a degree per frame
// sits still for two frames and then jumps, which reads as a dropped frame
// rather than as coarse arithmetic.
long gl_sin_fx(long deg) {
    long i;
    long f;
    long a;
    long b;
    i = deg >> GL_FRAC;
    f = deg & 0xFFFF;
    if (f < 0) f = 0;
    a = gl_sin(i);
    b = gl_sin(i + 1);
    return a + (((b - a) * f) >> GL_FRAC);
}

long gl_cos_fx(long deg) { return gl_sin_fx(deg + (90 << GL_FRAC)); }

// ---------- vectors and matrices ----------
//
// Passed by pointer throughout. nano_cc has no struct-by-value parameters,
// which is also what rules out microui and TinyGL's own mu_Rect-shaped API --
// so the constraint that blocks the libraries is visible right here in the
// signatures.

struct V3 { long x; long y; long z; };

// Row-major 4x4. m[row * 4 + col].
struct M4 { long m[16]; };

void m4_identity(struct M4 *o) {
    long i;
    i = 0;
    while (i < 16) { o->m[i] = 0; i = i + 1; }
    o->m[0] = GL_ONE; o->m[5] = GL_ONE; o->m[10] = GL_ONE; o->m[15] = GL_ONE;
}

// o = a * b. Written into a scratch first, so that m4_mul(x, x, y) is safe --
// aliasing the destination onto an operand is the obvious way to use this and
// silently reading half-updated values is the obvious bug.
void m4_mul(struct M4 *o, struct M4 *a, struct M4 *b) {
    struct M4 t;
    long r;
    r = 0;
    while (r < 4) {
        long c;
        c = 0;
        while (c < 4) {
            long s;
            long k;
            s = 0;
            k = 0;
            while (k < 4) {
                s = s + fx_mul(a->m[r * 4 + k], b->m[k * 4 + c]);
                k = k + 1;
            }
            t.m[r * 4 + c] = s;
            c = c + 1;
        }
        r = r + 1;
    }
    r = 0;
    while (r < 16) { o->m[r] = t.m[r]; r = r + 1; }
}

void m4_translate(struct M4 *o, long x, long y, long z) {
    m4_identity(o);
    o->m[3] = x; o->m[7] = y; o->m[11] = z;
}

void m4_scale(struct M4 *o, long s) {
    m4_identity(o);
    o->m[0] = s; o->m[5] = s; o->m[10] = s;
}

void m4_rot_x(struct M4 *o, long deg) {
    long c;
    long s;
    c = gl_cos(deg); s = gl_sin(deg);
    m4_identity(o);
    o->m[5] = c; o->m[6] = 0 - s;
    o->m[9] = s; o->m[10] = c;
}

void m4_rot_y(struct M4 *o, long deg) {
    long c;
    long s;
    c = gl_cos(deg); s = gl_sin(deg);
    m4_identity(o);
    o->m[0] = c;  o->m[2] = s;
    o->m[8] = 0 - s; o->m[10] = c;
}

void m4_rot_z(struct M4 *o, long deg) {
    long c;
    long s;
    c = gl_cos(deg); s = gl_sin(deg);
    m4_identity(o);
    o->m[0] = c; o->m[1] = 0 - s;
    o->m[4] = s; o->m[5] = c;
}

// Transform a point (w = 1). The translation column is added directly rather
// than multiplied by a fixed-point 1, which would cost three multiplies to
// achieve nothing.
void m4_apply(struct V3 *o, struct M4 *m, struct V3 *v) {
    long x;
    long y;
    long z;
    x = fx_mul(m->m[0], v->x) + fx_mul(m->m[1], v->y) + fx_mul(m->m[2],  v->z) + m->m[3];
    y = fx_mul(m->m[4], v->x) + fx_mul(m->m[5], v->y) + fx_mul(m->m[6],  v->z) + m->m[7];
    z = fx_mul(m->m[8], v->x) + fx_mul(m->m[9], v->y) + fx_mul(m->m[10], v->z) + m->m[11];
    o->x = x; o->y = y; o->z = z;
}

// A direction: the same, without the translation.
void m4_apply_dir(struct V3 *o, struct M4 *m, struct V3 *v) {
    long x;
    long y;
    long z;
    x = fx_mul(m->m[0], v->x) + fx_mul(m->m[1], v->y) + fx_mul(m->m[2],  v->z);
    y = fx_mul(m->m[4], v->x) + fx_mul(m->m[5], v->y) + fx_mul(m->m[6],  v->z);
    z = fx_mul(m->m[8], v->x) + fx_mul(m->m[9], v->y) + fx_mul(m->m[10], v->z);
    o->x = x; o->y = y; o->z = z;
}

void v3_sub(struct V3 *o, struct V3 *a, struct V3 *b) {
    o->x = a->x - b->x; o->y = a->y - b->y; o->z = a->z - b->z;
}

void v3_cross(struct V3 *o, struct V3 *a, struct V3 *b) {
    long x;
    long y;
    long z;
    x = fx_mul(a->y, b->z) - fx_mul(a->z, b->y);
    y = fx_mul(a->z, b->x) - fx_mul(a->x, b->z);
    z = fx_mul(a->x, b->y) - fx_mul(a->y, b->x);
    o->x = x; o->y = y; o->z = z;
}

long v3_dot(struct V3 *a, struct V3 *b) {
    return fx_mul(a->x, b->x) + fx_mul(a->y, b->y) + fx_mul(a->z, b->z);
}

long v3_len(struct V3 *a) {
    return fx_sqrt(fx_mul(a->x, a->x) + fx_mul(a->y, a->y) + fx_mul(a->z, a->z));
}

void v3_norm(struct V3 *o, struct V3 *a) {
    long l;
    l = v3_len(a);
    if (l == 0) { o->x = 0; o->y = 0; o->z = 0; return; }
    o->x = fx_div(a->x, l);
    o->y = fx_div(a->y, l);
    o->z = fx_div(a->z, l);
}

// ---------- textures ----------
//
// Power-of-two only, which is not a shortcut: OpenGL 1.1 required it too, and
// it is what lets GL_REPEAT be a bitwise AND instead of a modulo. A modulo per
// pixel on a machine with no divider worth the name is the difference between
// a textured floor and a slideshow.
//
// One format, 32-bit packed RGB, the same thing the window backing buffer
// holds. There is no conversion anywhere in the pipeline.

#define GL_MAXTEX 8

struct Texture {
    long used;
    long w;
    long h;
    long wmask;            // w - 1, valid because w is a power of two
    long hmask;
    long *pix;
};

struct Texture g_tex[GL_MAXTEX];

void gl_tex_init() {
    long i;
    i = 0;
    while (i < GL_MAXTEX) { g_tex[i].used = 0; g_tex[i].pix = 0; i = i + 1; }
}

long gl_pow2(long v) {
    long p;
    p = 1;
    while (p < v) p = p << 1;
    return p == v;
}

// Fetch a texel. s and t are 16.16 texture coordinates, and they may be
// anything at all -- negative, or far outside [0,1].
//
// The wrap is `& mask` on a value that has already been arithmetic-shifted
// down. That is correct for negatives too and it is worth saying why, because
// it looks like it should not be: -1 in two's complement is all ones, so
// (-1 & 63) is 63, which is exactly the texel GL_REPEAT asks for at s = -1/64.
// Writing it as a modulo would give -1 and index out of the array.
long gl_texel(struct Texture *t, long s, long tt) {
    long u;
    long v;
    u = ((s * t->w) >> GL_FRAC) & t->wmask;
    v = ((tt * t->h) >> GL_FRAC) & t->hmask;
    return t->pix[v * t->w + u];
}

// ---------- the context, bound to a window ----------

#define GL_MAXW 640
#define GL_MAXH 480

struct GLCtx {
    long win;              // the window manager handle this renders into
    long vx;               // viewport, in WINDOW coordinates
    long vy;
    long vw;
    long vh;
    long focal;            // focal length in pixels
    long near;             // near plane, fixed point, view space
    long far;              // far plane -- see the note on gl_frustum
    long izfar;            // 1/far, the rasteriser's far rejection test

    // The projection matrix. Everything that reaches the screen goes through
    // it, which is the point: the six clipping planes are extracted from this
    // same matrix, so the frustum test and the rasteriser cannot disagree
    // about where the edge of the screen is.
    struct M4 proj;

    long *zbuf;            // vw*vh reciprocal depths; larger is nearer
    long wire;             // wireframe instead of solid
    long bg;

    // glEnable/glDisable state. Kept on the context rather than in a global
    // because two viewports in two windows are two independent renderers, and
    // OpenGL's one-big-global-state is the part of the design nobody defends.
    long cull;             // GL_CULL_FACE
    long depth;            // GL_DEPTH_TEST
    long lighting;         // GL_LIGHTING
    long texturing;        // GL_TEXTURE_2D
    long tex;              // bound texture, or -1

    // The texture coordinates of the triangle about to be drawn, one pair per
    // vertex, in the same order as the vertices. On the context rather than in
    // the signature for the usual reason: gl_tri_view already takes five
    // arguments and nano_cc's ceiling is six.
    long vs[3];
    long vt[3];

    // ...and, after projection, s/z and t/z per vertex, which is what actually
    // gets interpolated. Kept as the FULL 32.32 product with no shift: the
    // precision here is what perspective-correct texturing lives or dies on,
    // and 64 bits is what the machine has.
    long rs[3];
    long rt[3];

    struct V3 light;       // unit direction, view space

    // A normal supplied by glNormal3x, in view space. When absent the
    // geometric normal of the triangle is used, which is what K15 did and is
    // right for flat-shaded solids; a supplied normal is what lets a curved
    // surface be lit as if it were curved.
    long nvalid;
    struct V3 nrm;

    // The bounding box of pixels actually written since the last flush. This
    // is what makes the renderer a good citizen of the compositor: it does not
    // invalidate its viewport, it invalidates what it touched.
    long dx0;
    long dy0;
    long dx1;
    long dy1;

    long tris_in;          // counters, so the tests can assert on work done
    long tris_culled;      // backfacing
    long tris_clipped;     // met the near plane
    long tris_drawn;
    long pixels;           // buffer pixels written
};

// Build the projection matrix for an off-centre frustum, exactly as
// glFrustum specifies it -- with one deliberate difference of convention.
//
// Standard OpenGL looks down -z, so its w row is (0,0,-1,0). This renderer
// looks down +z: `v->z >= near` means "in front of the camera" everywhere in
// K15, and the near-plane clipper is written that way. Substituting z -> -z
// through glFrustum's matrix gives the one below: the w row is (0,0,1,0) and
// the third column of the x and y rows changes sign. Everything else -- the
// 2n/(r-l) scale, the (f+n)/(f-n) depth mapping -- is glFrustum's.
//
// Stating that here rather than in a comment somewhere in the demo, because a
// handedness convention that is only implied is the thing that later makes
// somebody's imported model appear mirrored.
void gl_frustum(struct GLCtx *c, long l, long r, long b, long t) {
    long n;
    long f;
    n = c->near;
    f = c->far;
    m4_identity(&c->proj);
    c->proj.m[0]  = fx_div(2 * n, r - l);
    c->proj.m[2]  = 0 - fx_div(r + l, r - l);
    c->proj.m[5]  = fx_div(2 * n, t - b);
    c->proj.m[6]  = 0 - fx_div(t + b, t - b);
    c->proj.m[10] = fx_div(f + n, f - n);
    c->proj.m[11] = 0 - fx_div(fx_mul(2 * f, n), f - n);
    c->proj.m[14] = GL_ONE;
    c->proj.m[15] = 0;
    // The far plane as a reciprocal depth, so the rasteriser can reject
    // fragments beyond it with one comparison against a number it already has.
    //
    // This matters more than it looks. Object-level frustum culling is only
    // sound if throwing an object away is invisible -- and without a far clip
    // in the rasteriser, an object past the far plane is culled by the frustum
    // test but WOULD have drawn pixels, so switching culling on changes the
    // picture. Then "culling is free" stops being true and starts being an
    // argument. One compare per fragment buys the property back.
    c->izfar = fx_div(GL_ONE, f);
}

// The symmetric frustum implied by a focal length in pixels, which is how K15
// expressed the camera. r = near * (vw/2) / focal, and likewise for t, so this
// produces exactly the projection the old divide-by-z was doing.
void gl_perspective_pixels(struct GLCtx *c) {
    long r;
    long t;
    r = fx_div(fx_mul(c->near, fx_from_int(c->vw / 2)), fx_from_int(c->focal));
    t = fx_div(fx_mul(c->near, fx_from_int(c->vh / 2)), fx_from_int(c->focal));
    gl_frustum(c, 0 - r, r, 0 - t, t);
}

long gl_bind(struct GLCtx *c, long win, long x, long y, long w, long h) {
    if (w <= 0 || h <= 0 || w > GL_MAXW || h > GL_MAXH) return 0;
    if (!g_win[win].used) return 0;
    c->win = win;
    c->vx = x; c->vy = y; c->vw = w; c->vh = h;
    c->focal = w;                       // ~53 degree horizontal field of view
    c->near = GL_ONE / 4;
    // 64 units, not a million. The far plane's coefficient in the extracted
    // plane equation is (1 - (f+n)/(f-n)), which tends to zero as f grows --
    // push it far enough and the plane is all rounding error. 64 keeps it at
    // about 500 units of 1/65536, so far-plane distances are good to a fifth
    // of a percent. A renderer with no floats has to choose its ranges.
    c->far = fx_from_int(64);
    c->nvalid = 0;
    c->cull = 1;
    c->depth = 1;
    c->lighting = 1;
    c->texturing = 0;
    c->tex = -1;
    c->vs[0] = 0; c->vs[1] = 0; c->vs[2] = 0;
    c->vt[0] = 0; c->vt[1] = 0; c->vt[2] = 0;
    c->wire = 0;
    c->bg = rgb(12, 14, 22);
    c->light.x = 0; c->light.y = 0; c->light.z = 0 - GL_ONE;
    c->zbuf = (long *)kmalloc(w * h * 8);
    if (!c->zbuf) return 0;
    c->dx0 = 0; c->dy0 = 0; c->dx1 = -1; c->dy1 = -1;
    gl_perspective_pixels(c);
    return 1;
}

void gl_mark(struct GLCtx *c, long x, long y) {
    if (c->dx1 < c->dx0) { c->dx0 = x; c->dx1 = x; c->dy0 = y; c->dy1 = y; return; }
    if (x < c->dx0) c->dx0 = x;
    if (x > c->dx1) c->dx1 = x;
    if (y < c->dy0) c->dy0 = y;
    if (y > c->dy1) c->dy1 = y;
}

// Write one pixel, in VIEWPORT coordinates, into the window's backing buffer.
// Nothing here reaches the screen.
void gl_put(struct GLCtx *c, long x, long y, long colour) {
    long wx;
    long wy;
    if (x < 0 || y < 0 || x >= c->vw || y >= c->vh) return;
    wx = c->vx + x;
    wy = c->vy + y;
    if (wx < 0 || wy < 0 || wx >= g_win[c->win].w || wy >= g_win[c->win].h) return;
    g_win[c->win].pix[wy * g_win[c->win].w + wx] = colour;
    c->pixels = c->pixels + 1;
    gl_mark(c, x, y);
}

void gl_clear(struct GLCtx *c) {
    long i;
    long n;
    n = c->vw * c->vh;
    i = 0;
    while (i < n) { c->zbuf[i] = 0; i = i + 1; }   // 1/z of 0 = infinitely far
    wm_win_fill(c->win, c->vx, c->vy, c->vw, c->vh, c->bg);
    c->pixels = c->pixels + n;
    gl_mark(c, 0, 0);
    gl_mark(c, c->vw - 1, c->vh - 1);
    c->tris_in = 0; c->tris_culled = 0; c->tris_clipped = 0; c->tris_drawn = 0;
}

// Hand the damage to the compositor. The whole reason the renderer takes a
// window handle rather than the framebuffer: it says what changed, and the
// compositor decides what that costs.
void gl_flush(struct GLCtx *c) {
    if (c->dx1 < c->dx0) return;                  // nothing was drawn
    wm_invalidate(c->win, c->vx + c->dx0, c->vy + c->dy0,
                  c->dx1 - c->dx0 + 1, c->dy1 - c->dy0 + 1);
    c->dx0 = 0; c->dy0 = 0; c->dx1 = -1; c->dy1 = -1;
}

// ---------- projection ----------

// View space to viewport pixels. Returns 0 if the point is at or behind the
// near plane, where the divide is meaningless.
long gl_project(struct GLCtx *c, struct V3 *v, long *sx, long *sy, long *iz) {
    long cx;
    long cy;
    long cw;
    if (v->z < c->near) return 0;
    // Clip space. Only the rows that can be non-zero for a frustum matrix are
    // multiplied out; the full 4x4 would be three wasted multiplies per vertex
    // for terms that glFrustum guarantees are zero.
    cx = fx_mul(c->proj.m[0], v->x) + fx_mul(c->proj.m[2], v->z);
    cy = fx_mul(c->proj.m[5], v->y) + fx_mul(c->proj.m[6], v->z);
    cw = fx_mul(c->proj.m[14], v->z) + c->proj.m[15];
    if (cw <= 0) return 0;
    // Normalised device coordinates, then the viewport transform. y is flipped
    // because NDC grows upwards and a framebuffer row index grows downwards.
    *sx = c->vw / 2 + fx_to_int(fx_div(cx, cw) * (c->vw / 2));
    *sy = c->vh / 2 - fx_to_int(fx_div(cy, cw) * (c->vh / 2));
    // Depth is stored as 1/z and interpolated linearly, which is exact in
    // screen space. Interpolating z itself is not, and it shows as geometry
    // poking through other geometry near the edges of large triangles.
    *iz = fx_div(GL_ONE, v->z);
    return 1;
}

// ---------- lines, for wireframe ----------

// The general case: every pixel through gl_put, which bounds-checks it. Kept
// because a line with an endpoint outside the viewport needs it, and because
// it is the baseline the fast path is measured and checked against.
void gl_line_slow(struct GLCtx *c, long x0, long y0, long x1, long y1, long colour) {
    long dx;
    long dy;
    long sx;
    long sy;
    long err;
    dx = x1 - x0; if (dx < 0) dx = 0 - dx;
    dy = y1 - y0; if (dy < 0) dy = 0 - dy;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx - dy;
    for (;;) {
        long e2;
        gl_put(c, x0, y0, colour);
        if (x0 == x1 && y0 == y1) return;
        e2 = err * 2;
        if (e2 > (0 - dy)) { err = err - dy; x0 = x0 + sx; }
        if (e2 < dx)       { err = err + dx; y0 = y0 + sy; }
    }
}

// ---------- lines, faster ----------
//
// Asked whether a line would be faster stepped from both ends, or written out
// as spans the way a glyph blit does. Both were built and timed against the
// baseline on the ACPI PM timer -- see linebench.c, which still contains them
// -- and the answer was neither. What the measurement found instead:
//
//     the same 1.5M stores, written directly:   52 ms
//     the same 1.5M stores, through gl_put:    352 ms      -- 6.7x
//
// The line was never the line. It was the FIVE OPERATIONS OF BOOKKEEPING
// around each store: two bounds tests, a window-size test, a damage-box update
// and a counter increment, per pixel.
//
// And all of it can be hoisted, because of one fact about straight lines: if
// both endpoints are inside a rectangle then every pixel between them is too.
// A line cannot leave a rectangle and come back. So the tests happen ONCE per
// segment and the inner loop becomes an add, a compare and a store.
//
//     baseline                       43,447 ns per line   100%
//     both ends at once              46,059 ns per line   106%   (and wrong)
//     runs batched into spans        17,383 ns per line    40%
//     bounds checked once             9,104 ns per line    20%
//
// Five times faster, and bit-identical over a fan of 224 lines at every slope.

long gl_line_inside(struct GLCtx *c, long x0, long y0, long x1, long y1) {
    if (x0 < 0 || y0 < 0 || x0 >= c->vw || y0 >= c->vh) return 0;
    if (x1 < 0 || y1 < 0 || x1 >= c->vw || y1 >= c->vh) return 0;
    return 1;
}

void gl_line(struct GLCtx *c, long x0, long y0, long x1, long y1, long colour) {
    long dx; long dy; long sx; long sy; long err;
    long *pix;
    long stride;
    long ox;
    long oy;
    long n;

    if (!gl_line_inside(c, x0, y0, x1, y1)) {
        gl_line_slow(c, x0, y0, x1, y1, colour);
        return;
    }

    // The damage box for the whole segment, once. gl_mark only ever computes a
    // bounding box, so marking the two ends is exactly what marking every
    // pixel would have produced -- this is not an approximation.
    gl_mark(c, x0, y0);
    gl_mark(c, x1, y1);

    pix = g_win[c->win].pix;
    stride = g_win[c->win].w;
    ox = c->vx;
    oy = c->vy;

    dx = x1 - x0; if (dx < 0) dx = 0 - dx;
    dy = y1 - y0; if (dy < 0) dy = 0 - dy;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx - dy;
    n = (dx > dy ? dx : dy) + 1;
    c->pixels = c->pixels + n;

    for (;;) {
        long e2;
        pix[(oy + y0) * stride + ox + x0] = colour;
        if (x0 == x1 && y0 == y1) return;
        e2 = err * 2;
        if (e2 > (0 - dy)) { err = err - dy; x0 = x0 + sx; }
        if (e2 < dx)       { err = err + dx; y0 = y0 + sy; }
    }
}

// ---------- the triangle ----------

// Half-space rasterisation over the bounding box. The three edge functions are
// the barycentric weights up to a constant, so the depth interpolation comes
// out of the same numbers as the inside test and costs nothing extra.
//
// Screen coordinates are integers here; the fixed point ended at projection.
void gl_tri_raster(struct GLCtx *c, long *sx, long *sy, long *iz, long colour) {
    long minx; long miny; long maxx; long maxy;
    long area;
    long y;
    long textured;
    long cr; long cg; long cb;
    struct Texture *tx;

    // The texture, and the colour it is modulated by, hoisted out of the loop.
    // GL's default texture environment is GL_MODULATE: the fragment is the
    // texel times the primary colour, which here already carries the lighting
    // term. So a white primary colour gives the texel lit, and a tinted one
    // tints it -- the same behaviour, from the same arithmetic.
    textured = 0;
    tx = &g_tex[0];
    // `pix` and not just `used`. glGenTexture marks a name used before
    // anything has been uploaded to it, which is exactly what GL does -- a
    // name exists from the moment it is generated -- so a program that binds a
    // name and draws before calling glTexImage2D is doing something ordinary
    // and slightly wrong. Without this test it reads through a null pointer
    // and takes the machine down with a page fault at address 0. Found by a
    // test that did precisely that on purpose.
    if (c->texturing && c->tex >= 0 && c->tex < GL_MAXTEX &&
        g_tex[c->tex].used && g_tex[c->tex].pix) {
        textured = 1;
        tx = &g_tex[c->tex];
    }
    cr = (colour >> 16) & 255;
    cg = (colour >> 8) & 255;
    cb = colour & 255;

    minx = sx[0]; if (sx[1] < minx) minx = sx[1]; if (sx[2] < minx) minx = sx[2];
    maxx = sx[0]; if (sx[1] > maxx) maxx = sx[1]; if (sx[2] > maxx) maxx = sx[2];
    miny = sy[0]; if (sy[1] < miny) miny = sy[1]; if (sy[2] < miny) miny = sy[2];
    maxy = sy[0]; if (sy[1] > maxy) maxy = sy[1]; if (sy[2] > maxy) maxy = sy[2];

    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > c->vw - 1) maxx = c->vw - 1;
    if (maxy > c->vh - 1) maxy = c->vh - 1;
    if (minx > maxx || miny > maxy) return;

    area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (area == 0) return;                     // degenerate, zero pixels

    y = miny;
    while (y <= maxy) {
        long x;
        x = minx;
        while (x <= maxx) {
            long w0;
            long w1;
            long w2;
            w0 = (sx[2] - sx[1]) * (y - sy[1]) - (sy[2] - sy[1]) * (x - sx[1]);
            w1 = (sx[0] - sx[2]) * (y - sy[2]) - (sy[0] - sy[2]) * (x - sx[2]);
            w2 = (sx[1] - sx[0]) * (y - sy[0]) - (sy[1] - sy[0]) * (x - sx[0]);
            if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                long d;
                long idx;
                d = (w0 * iz[0] + w1 * iz[1] + w2 * iz[2]) / area;
                idx = y * c->vw + x;
                // Larger 1/z is nearer, so a fragment beyond the far plane has
                // the SMALLER reciprocal. `>` and not `>=` on the depth test,
                // so that coplanar surfaces drawn later do not fight for the
                // same pixel.
                if (d >= c->izfar && (!c->depth || d > c->zbuf[idx])) {
                    c->zbuf[idx] = d;
                    if (textured) {
                        long uoz;
                        long voz;
                        long texel;
                        // PERSPECTIVE CORRECTION, and the whole reason it is
                        // done this way. s and t are not linear in screen
                        // space; s/z and t/z are, and so is 1/z. So
                        // interpolate those three and divide at the end.
                        //
                        // Interpolating s directly is the cheap way and it is
                        // what gave the PlayStation 1 its swimming floors: a
                        // quad split along one diagonal renders differently
                        // from the same quad split along the other. There is a
                        // test for exactly that, because it is an invariant
                        // rather than something to judge from a screenshot.
                        uoz = (w0 * c->rs[0] + w1 * c->rs[1] + w2 * c->rs[2]) / area;
                        voz = (w0 * c->rt[0] + w1 * c->rt[1] + w2 * c->rt[2]) / area;
                        // rs is the full 32.32 product of s and 1/z, and d is
                        // 1/z in 16.16, so this quotient is s in 16.16 with no
                        // shift. Shifting the product down to 16.16 first
                        // would throw away exactly the bits that matter for a
                        // distant surface, where 1/z is small.
                        if (d != 0) {
                            texel = gl_texel(tx, uoz / d, voz / d);
                            gl_put(c, x, y,
                                   rgb(((texel >> 16) & 255) * cr / 255,
                                       ((texel >> 8) & 255) * cg / 255,
                                       (texel & 255) * cb / 255));
                        }
                    } else {
                        gl_put(c, x, y, colour);
                    }
                }
            }
            x = x + 1;
        }
        y = y + 1;
    }
}

// Shade a face by its normal in view space. Returns a colour.
long gl_shade(struct GLCtx *c, struct V3 *n, long base) {
    long d;
    long r;
    long g;
    long b;
    struct V3 u;
    v3_norm(&u, n);
    d = v3_dot(&u, &c->light);
    if (d < 0) d = 0 - d;                      // light both faces of a surface
    if (d > GL_ONE) d = GL_ONE;
    d = GL_ONE / 4 + fx_mul(d, GL_ONE - GL_ONE / 4);   // ambient floor
    r = ((base >> 16) & 255) * d >> GL_FRAC;
    g = ((base >> 8) & 255) * d >> GL_FRAC;
    b = (base & 255) * d >> GL_FRAC;
    return rgb(r, g, b);
}

// A vertex as it moves through the back of the pipeline: a position and a
// texture coordinate, travelling together.
//
// They HAVE to travel together, and that is the whole reason this struct
// exists. The near-plane clipper cuts an edge partway and invents a new
// vertex; if it interpolates the position but not the texture coordinate, the
// new vertex gets whatever was in the slot before, and a wall smears sideways
// the moment one of its corners passes behind the camera. That is a bug that
// only appears when you walk INTO something, which is exactly when nobody is
// looking at the far corner of the screen.
struct Vtx {
    struct V3 p;
    long s;
    long t;
};

// One triangle, already in VIEW space. Handles the near plane by clipping
// rather than rejecting: rejecting is easier and makes geometry vanish in whole
// faces as it approaches the camera, which looks like a bug in the renderer.
void gl_tri_view(struct GLCtx *c, struct V3 *a, struct V3 *b, struct V3 *v, long base);

// Interpolate to the near plane between p (inside) and q (outside) -- position
// and texture coordinate both, by the same parameter.
void gl_clip_edge(struct GLCtx *c, struct Vtx *o, struct Vtx *p, struct Vtx *q) {
    long t;
    t = fx_div(c->near - p->p.z, q->p.z - p->p.z);
    o->p.x = p->p.x + fx_mul(q->p.x - p->p.x, t);
    o->p.y = p->p.y + fx_mul(q->p.y - p->p.y, t);
    o->p.z = c->near;
    o->s = p->s + fx_mul(q->s - p->s, t);
    o->t = p->t + fx_mul(q->t - p->t, t);
}

void gl_tri_project(struct GLCtx *c, struct Vtx *a, struct Vtx *b, struct Vtx *v,
                    long colour) {
    long sx[3];
    long sy[3];
    long iz[3];
    if (!gl_project(c, &a->p, &sx[0], &sy[0], &iz[0])) return;
    if (!gl_project(c, &b->p, &sx[1], &sy[1], &iz[1])) return;
    if (!gl_project(c, &v->p, &sx[2], &sy[2], &iz[2])) return;

    if (c->wire) {
        gl_line(c, sx[0], sy[0], sx[1], sy[1], colour);
        gl_line(c, sx[1], sy[1], sx[2], sy[2], colour);
        gl_line(c, sx[2], sy[2], sx[0], sy[0], colour);
        return;
    }

    // s/z and t/z, as full 64-bit products with no shift. These are the
    // quantities that ARE linear in screen space; s and t themselves are not.
    c->rs[0] = a->s * iz[0]; c->rt[0] = a->t * iz[0];
    c->rs[1] = b->s * iz[1]; c->rt[1] = b->t * iz[1];
    c->rs[2] = v->s * iz[2]; c->rt[2] = v->t * iz[2];

    gl_tri_raster(c, sx, sy, iz, colour);
}

void gl_tri_view(struct GLCtx *c, struct V3 *a, struct V3 *b, struct V3 *v, long base) {
    struct V3 e0;
    struct V3 e1;
    struct V3 n;
    struct Vtx va;
    struct Vtx vb;
    struct Vtx vc;
    long inside;
    long colour;

    c->tris_in = c->tris_in + 1;

    // Backface culling in VIEW space, from the geometric normal, before the
    // near plane is considered. Doing it after projection would divide by a z
    // that may be behind the camera.
    v3_sub(&e0, b, a);
    v3_sub(&e1, v, a);
    v3_cross(&n, &e0, &e1);
    if (c->cull) {
        struct V3 toeye;
        toeye.x = 0 - a->x; toeye.y = 0 - a->y; toeye.z = 0 - a->z;
        if (v3_dot(&n, &toeye) <= 0) { c->tris_culled = c->tris_culled + 1; return; }
    }

    // Cull with the geometric normal always -- which way a triangle faces is a
    // property of its winding, not of whatever normal the caller supplied.
    // Shade with the supplied one when there is one.
    colour = (c->wire || !c->lighting) ? base
                                       : gl_shade(c, c->nvalid ? &c->nrm : &n, base);

    va.p = *a; va.s = c->vs[0]; va.t = c->vt[0];
    vb.p = *b; vb.s = c->vs[1]; vb.t = c->vt[1];
    vc.p = *v; vc.s = c->vs[2]; vc.t = c->vt[2];

    inside = 0;
    if (a->z >= c->near) inside = inside + 1;
    if (b->z >= c->near) inside = inside + 2;
    if (v->z >= c->near) inside = inside + 4;

    if (inside == 7) {
        c->tris_drawn = c->tris_drawn + 1;
        gl_tri_project(c, &va, &vb, &vc, colour);
        return;
    }
    if (inside == 0) { c->tris_clipped = c->tris_clipped + 1; return; }

    c->tris_clipped = c->tris_clipped + 1;

    // Rotate the three vertices so that the classification becomes one of two
    // cases -- one vertex in, or two. Writing all six cases out is where the
    // sign errors live.
    {
        struct Vtx p0;
        struct Vtx p1;
        struct Vtx p2;
        long one_in;
        if (inside == 1 || inside == 6) { p0 = va; p1 = vb; p2 = vc; one_in = (inside == 1); }
        else if (inside == 2 || inside == 5) { p0 = vb; p1 = vc; p2 = va; one_in = (inside == 2); }
        else { p0 = vc; p1 = va; p2 = vb; one_in = (inside == 4); }

        if (one_in) {
            // p0 inside, p1 and p2 out: one triangle.
            struct Vtx q1;
            struct Vtx q2;
            gl_clip_edge(c, &q1, &p0, &p1);
            gl_clip_edge(c, &q2, &p0, &p2);
            c->tris_drawn = c->tris_drawn + 1;
            gl_tri_project(c, &p0, &q1, &q2, colour);
        } else {
            // p0 outside, p1 and p2 in: a quad, as two triangles.
            struct Vtx q1;
            struct Vtx q2;
            gl_clip_edge(c, &q1, &p1, &p0);
            gl_clip_edge(c, &q2, &p2, &p0);
            c->tris_drawn = c->tris_drawn + 2;
            gl_tri_project(c, &p1, &p2, &q2, colour);
            gl_tri_project(c, &p1, &q2, &q1, colour);
        }
    }
}

// ---------- the view frustum ----------
//
// Six planes pulled straight out of the combined projection * modelview
// matrix, the way Mark Morley's article describes. The whole idea is that you
// never work out where the planes are: the matrix already contains them,
// because "inside the frustum" is by definition -w <= x,y,z <= w in clip
// space, and each of those six inequalities is one row of the matrix added to
// or subtracted from the w row.
//
//     left   = row3 + row0        right = row3 - row0
//     bottom = row3 + row1        top   = row3 - row1
//     near   = row3 + row2        far   = row3 - row2
//
// The article writes them as columns because it assumes OpenGL's
// column-major storage; struct M4 here is row-major, so they are rows.
//
// The payoff is that the test cannot drift away from the renderer. If someone
// widens the field of view, the planes widen with it, because both come from
// the same sixteen numbers. There is a test that checks exactly this by
// comparing the frustum's verdict against whether the rasteriser actually
// puts any pixels on the screen.

#define GL_OUTSIDE   0
#define GL_INTERSECT 1
#define GL_INSIDE    2

struct Plane { long a; long b; long c; long d; };   // a*x+b*y+c*z+d, >0 inside
struct Frustum { struct Plane p[6]; };

// Scale a plane so that (a,b,c) is a unit vector. Without this the value of
// a*x+b*y+c*z+d still has the right SIGN, so a point test works, but it is not
// a distance, so no sphere or box test does.
void gl_plane_norm(struct Plane *p) {
    long l;
    l = fx_sqrt(fx_mul(p->a, p->a) + fx_mul(p->b, p->b) + fx_mul(p->c, p->c));
    if (l == 0) return;
    p->a = fx_div(p->a, l);
    p->b = fx_div(p->b, l);
    p->c = fx_div(p->c, l);
    p->d = fx_div(p->d, l);
}

// `clip` is projection * modelview. The planes come out in whatever space the
// modelview started from -- pass projection alone for view space, or
// projection * modelview for the model's own space, which is what makes
// culling a model by its bounding sphere free of any per-object transform.
void gl_frustum_extract(struct Frustum *f, struct M4 *clip) {
    long i;
    long r;
    i = 0;
    while (i < 3) {
        r = i * 4;
        // Even index: row3 + row_i (left, bottom, near).
        f->p[i * 2].a = clip->m[12] + clip->m[r];
        f->p[i * 2].b = clip->m[13] + clip->m[r + 1];
        f->p[i * 2].c = clip->m[14] + clip->m[r + 2];
        f->p[i * 2].d = clip->m[15] + clip->m[r + 3];
        // Odd index: row3 - row_i (right, top, far).
        f->p[i * 2 + 1].a = clip->m[12] - clip->m[r];
        f->p[i * 2 + 1].b = clip->m[13] - clip->m[r + 1];
        f->p[i * 2 + 1].c = clip->m[14] - clip->m[r + 2];
        f->p[i * 2 + 1].d = clip->m[15] - clip->m[r + 3];
        gl_plane_norm(&f->p[i * 2]);
        gl_plane_norm(&f->p[i * 2 + 1]);
        i = i + 1;
    }
}

// Signed distance from a plane. Positive is the inside half-space.
long gl_plane_dist(struct Plane *p, struct V3 *v) {
    return fx_mul(p->a, v->x) + fx_mul(p->b, v->y) + fx_mul(p->c, v->z) + p->d;
}

long gl_frustum_point(struct Frustum *f, struct V3 *v) {
    long i;
    i = 0;
    while (i < 6) {
        if (gl_plane_dist(&f->p[i], v) < 0) return GL_OUTSIDE;
        i = i + 1;
    }
    return GL_INSIDE;
}

// GL_OUTSIDE, GL_INTERSECT or GL_INSIDE. Rejecting on the first plane the
// sphere is wholly behind is what makes this cheap: most rejected objects cost
// one plane, not six.
long gl_frustum_sphere(struct Frustum *f, struct V3 *v, long radius) {
    long i;
    long partial;
    long d;
    partial = 0;
    i = 0;
    while (i < 6) {
        d = gl_plane_dist(&f->p[i], v);
        if (d < 0 - radius) return GL_OUTSIDE;
        if (d < radius) partial = 1;
        i = i + 1;
    }
    return partial ? GL_INTERSECT : GL_INSIDE;
}

// An axis-aligned box given by its centre and half-extents. The "p-vertex"
// trick: for each plane only the single corner furthest along the plane
// normal decides rejection, so this is eight corners' worth of answer for one
// corner's worth of work.
long gl_frustum_box(struct Frustum *f, struct V3 *v, struct V3 *half) {
    long i;
    long partial;
    long d;
    long r;
    partial = 0;
    i = 0;
    while (i < 6) {
        long ax; long ay; long az;
        ax = f->p[i].a; if (ax < 0) ax = 0 - ax;
        ay = f->p[i].b; if (ay < 0) ay = 0 - ay;
        az = f->p[i].c; if (az < 0) az = 0 - az;
        r = fx_mul(ax, half->x) + fx_mul(ay, half->y) + fx_mul(az, half->z);
        d = gl_plane_dist(&f->p[i], v);
        if (d < 0 - r) return GL_OUTSIDE;
        if (d < r) partial = 1;
        i = i + 1;
    }
    return partial ? GL_INTERSECT : GL_INSIDE;
}

// A triangle in MODEL space, transformed by the current modelview.
void gl_tri(struct GLCtx *c, struct M4 *mv, struct V3 *a, struct V3 *b,
            struct V3 *v, long base) {
    struct V3 ta;
    struct V3 tb;
    struct V3 tc;
    m4_apply(&ta, mv, a);
    m4_apply(&tb, mv, b);
    m4_apply(&tc, mv, v);
    gl_tri_view(c, &ta, &tb, &tc, base);
}

#endif
