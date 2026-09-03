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

    long *zbuf;            // vw*vh reciprocal depths; larger is nearer
    long wire;             // wireframe instead of solid
    long bg;

    struct V3 light;       // unit direction, view space

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

long gl_bind(struct GLCtx *c, long win, long x, long y, long w, long h) {
    if (w <= 0 || h <= 0 || w > GL_MAXW || h > GL_MAXH) return 0;
    if (!g_win[win].used) return 0;
    c->win = win;
    c->vx = x; c->vy = y; c->vw = w; c->vh = h;
    c->focal = w;                       // ~53 degree horizontal field of view
    c->near = GL_ONE / 4;
    c->wire = 0;
    c->bg = rgb(12, 14, 22);
    c->light.x = 0; c->light.y = 0; c->light.z = 0 - GL_ONE;
    c->zbuf = (long *)kmalloc(w * h * 8);
    if (!c->zbuf) return 0;
    c->dx0 = 0; c->dy0 = 0; c->dx1 = -1; c->dy1 = -1;
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
    long rx;
    long ry;
    if (v->z < c->near) return 0;
    rx = fx_div(v->x, v->z);
    ry = fx_div(v->y, v->z);
    *sx = c->vw / 2 + fx_to_int(rx * c->focal);
    *sy = c->vh / 2 - fx_to_int(ry * c->focal);
    // Depth is stored as 1/z and interpolated linearly, which is exact in
    // screen space. Interpolating z itself is not, and it shows as geometry
    // poking through other geometry near the edges of large triangles.
    *iz = fx_div(GL_ONE, v->z);
    return 1;
}

// ---------- lines, for wireframe ----------

void gl_line(struct GLCtx *c, long x0, long y0, long x1, long y1, long colour) {
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
                // Larger 1/z is nearer. `>` and not `>=` so that coplanar
                // surfaces drawn later do not fight for the same pixel.
                if (d > c->zbuf[idx]) {
                    c->zbuf[idx] = d;
                    gl_put(c, x, y, colour);
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

// One triangle, already in VIEW space. Handles the near plane by clipping
// rather than rejecting: rejecting is easier and makes geometry vanish in whole
// faces as it approaches the camera, which looks like a bug in the renderer.
void gl_tri_view(struct GLCtx *c, struct V3 *a, struct V3 *b, struct V3 *v, long base);

// Interpolate to the near plane between p (inside) and q (outside).
void gl_clip_edge(struct GLCtx *c, struct V3 *o, struct V3 *p, struct V3 *q) {
    long t;
    t = fx_div(c->near - p->z, q->z - p->z);
    o->x = p->x + fx_mul(q->x - p->x, t);
    o->y = p->y + fx_mul(q->y - p->y, t);
    o->z = c->near;
}

void gl_tri_project(struct GLCtx *c, struct V3 *a, struct V3 *b, struct V3 *v,
                    long colour) {
    long sx[3];
    long sy[3];
    long iz[3];
    if (!gl_project(c, a, &sx[0], &sy[0], &iz[0])) return;
    if (!gl_project(c, b, &sx[1], &sy[1], &iz[1])) return;
    if (!gl_project(c, v, &sx[2], &sy[2], &iz[2])) return;

    if (c->wire) {
        gl_line(c, sx[0], sy[0], sx[1], sy[1], colour);
        gl_line(c, sx[1], sy[1], sx[2], sy[2], colour);
        gl_line(c, sx[2], sy[2], sx[0], sy[0], colour);
        return;
    }
    gl_tri_raster(c, sx, sy, iz, colour);
}

void gl_tri_view(struct GLCtx *c, struct V3 *a, struct V3 *b, struct V3 *v, long base) {
    struct V3 e0;
    struct V3 e1;
    struct V3 n;
    long inside;
    long colour;

    c->tris_in = c->tris_in + 1;

    // Backface culling in VIEW space, from the geometric normal, before the
    // near plane is considered. Doing it after projection would divide by a z
    // that may be behind the camera.
    v3_sub(&e0, b, a);
    v3_sub(&e1, v, a);
    v3_cross(&n, &e0, &e1);
    {
        struct V3 toeye;
        toeye.x = 0 - a->x; toeye.y = 0 - a->y; toeye.z = 0 - a->z;
        if (v3_dot(&n, &toeye) <= 0) { c->tris_culled = c->tris_culled + 1; return; }
    }

    colour = c->wire ? base : gl_shade(c, &n, base);

    inside = 0;
    if (a->z >= c->near) inside = inside + 1;
    if (b->z >= c->near) inside = inside + 2;
    if (v->z >= c->near) inside = inside + 4;

    if (inside == 7) { c->tris_drawn = c->tris_drawn + 1; gl_tri_project(c, a, b, v, colour); return; }
    if (inside == 0) { c->tris_clipped = c->tris_clipped + 1; return; }

    c->tris_clipped = c->tris_clipped + 1;

    // Rotate the three vertices so that the classification becomes one of two
    // cases -- one vertex in, or two. Writing all six cases out is where the
    // sign errors live.
    {
        struct V3 p0;
        struct V3 p1;
        struct V3 p2;
        long one_in;
        if (inside == 1 || inside == 6) { p0 = *a; p1 = *b; p2 = *v; one_in = (inside == 1); }
        else if (inside == 2 || inside == 5) { p0 = *b; p1 = *v; p2 = *a; one_in = (inside == 2); }
        else { p0 = *v; p1 = *a; p2 = *b; one_in = (inside == 4); }

        if (one_in) {
            // p0 inside, p1 and p2 out: one triangle.
            struct V3 q1;
            struct V3 q2;
            gl_clip_edge(c, &q1, &p0, &p1);
            gl_clip_edge(c, &q2, &p0, &p2);
            c->tris_drawn = c->tris_drawn + 1;
            gl_tri_project(c, &p0, &q1, &q2, colour);
        } else {
            // p0 outside, p1 and p2 in: a quad, as two triangles.
            struct V3 q1;
            struct V3 q2;
            gl_clip_edge(c, &q1, &p1, &p0);
            gl_clip_edge(c, &q2, &p2, &p0);
            c->tris_drawn = c->tris_drawn + 2;
            gl_tri_project(c, &p1, &p2, &q2, colour);
            gl_tri_project(c, &p1, &q2, &q1, colour);
        }
    }
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
