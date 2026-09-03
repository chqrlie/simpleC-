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
    // The surface being drawn into, rather than a window handle.
    //
    // The renderer used to write straight into g_win[c->win].pix, which meant
    // it could only ever run in the kernel: a process cannot call a kernel
    // function and cannot see that array. Naming the surface instead is what
    // lets the SAME renderer be linked into a user program, which draws into
    // memory it got from sbrk and blits the result through a syscall -- the
    // wingl.c arrangement, with a real GL behind it.
    long *pix;             // the pixels
    long stride;           // pixels per row
    long surfh;            // rows
    long owns_win;         // 1 = pix belongs to a window, so damage is the
                           // compositor's business; 0 = the caller's buffer

    long cull;             // GL_CULL_FACE
    // Which winding faces the camera. This renderer looks along +z where
    // standard GL looks along -z, so a model written for GL -- gears.c, an
    // OBJ file, anything exported by a modeller -- arrives with every
    // triangle wound the other way. Without a way to say so, the only fix is
    // to reverse the vertex order at every call site, which is how a model
    // ends up rendered inside-out by someone who then "fixes" it by turning
    // culling off.
    long frontcw;          // 1 = clockwise is front, which is GL's default here
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

    // The bounding box of everything the renderer has DIRTIED since the last
    // clear -- pixels written, and depth entries written, which is not the
    // same set. Outside it the colour buffer still holds bg and the depth
    // buffer still holds zero, because that is what the last clear left there
    // and nothing has touched it since.
    //
    // That invariant is what lets gl_clear clear a rectangle instead of a
    // viewport. It is maintained conservatively: a triangle dirties its whole
    // clipped bounding box whether or not every pixel in it survived the depth
    // test, so a fragment that wrote only a depth value cannot escape it.
    long cx0;
    long cy0;
    long cx1;
    long cy1;

    long tris_in;          // counters, so the tests can assert on work done
    long tris_culled;      // backfacing
    long tris_clipped;     // met the near plane
    long tris_drawn;
    long pixels;           // buffer pixels written
    long clearpix;         // of those, the ones the last gl_clear wrote

    // What the rasteriser walked over, as opposed to what it wrote. A
    // triangle is at most half of its bounding box and usually far less, so
    // these two numbers are never equal -- but the ratio between them is the
    // whole cost of a scanline loop that finds the span by looking for it.
    // Kept as counters and not as an assumption, because "the sliver in the
    // corner is cheap" is exactly the sort of thing that is true right up
    // until the camera turns.
    long boxpix;           // sum of the clipped bounding boxes, in pixels
    long steps;            // inner-loop iterations actually executed
    long covered;          // of those, the ones actually inside a triangle

    // Vertices pushed through m4_apply. Three per triangle today, whether or
    // not the triangle survives and whether or not the vertex was already
    // transformed a moment ago for its neighbour -- which is the whole
    // question transform caching asks, so it needs a number.
    long xforms;

    // Stop each triangle immediately after the backface decision. Draws
    // nothing; exists so a frame's transform-and-cull stage can be timed on
    // its own instead of inferred from the difference between two frames that
    // both draw. Never set outside a measurement.
    long xformonly;

    // Measurement instrument, off by default. Performs ONE EXTRA divide per
    // covered fragment, on the same operands, and sinks the result so nothing
    // can discard it. The frame-time delta between off and on is the cost of
    // one 64-bit divide per fragment -- measured, rather than reasoned about
    // from "a divide is slow", which under a TCG qemu means a helper call and
    // not an instruction. The picture is IDENTICAL either way, so the delta is
    // the divide and nothing else.
    long twodiv;
    long divsink;

    // Draw through the old walking rasteriser instead of the span one. Only
    // has an effect where GL_RASTER_REF was defined; it exists so a test can
    // render the same scene both ways and compare the buffers.
    long refraster;

    // Clear the whole viewport regardless of what was dirtied -- the old
    // behaviour, kept so a test can render a scene both ways and hash the
    // buffers. Without it "the box is enough" is an argument rather than a
    // measurement.
    long clearall;
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

// Everything gl_bind sets that is not about WHERE the pixels are. Split out
// so the window path and the buffer path cannot drift apart -- two copies of
// "the default near plane is a quarter of a unit" is how they would.
void gl_defaults(struct GLCtx *c, long w) {
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
    c->frontcw = 0;
    c->depth = 1;
    c->lighting = 1;
    c->texturing = 0;
    c->tex = -1;
    c->vs[0] = 0; c->vs[1] = 0; c->vs[2] = 0;
    c->vt[0] = 0; c->vt[1] = 0; c->vt[2] = 0;
    c->wire = 0;
    c->bg = rgb(12, 14, 22);
    c->light.x = 0; c->light.y = 0; c->light.z = 0 - GL_ONE;
    c->dx0 = 0; c->dy0 = 0; c->dx1 = -1; c->dy1 = -1;
    // The whole viewport is dirty until it has been cleared once: the buffer
    // came from the window manager and holds whatever was there before.
    c->cx0 = 0; c->cy0 = 0; c->cx1 = c->vw - 1; c->cy1 = c->vh - 1;
    c->boxpix = 0; c->steps = 0; c->covered = 0;
    c->xforms = 0; c->xformonly = 0;
    c->twodiv = 0; c->divsink = 0;
    c->clearpix = 0;
    c->refraster = 0;
    c->clearall = 0;
    gl_perspective_pixels(c);
}

// Render into memory the caller owns, with no window and no kernel behind it.
//
// This is the entry point a user program uses: it allocates the pixels and the
// depth buffer from sbrk, renders, and blits the result through SYS_WINBLIT.
// Six arguments, which is nano_cc's ceiling exactly -- so the viewport is the
// whole buffer rather than a rectangle inside it, which is what a program
// that owns its own pixels wants anyway.
//
// The depth buffer is the caller's too: w*h longs, and there is no kmalloc on
// that side of the boundary.
long gl_bind_buf(struct GLCtx *c, long *pix, long w, long h, long *zbuf) {
    if (w <= 0 || h <= 0 || w > GL_MAXW || h > GL_MAXH) return 0;
    if (!pix || !zbuf) return 0;
    c->win = -1;
    c->pix = pix;
    c->stride = w;
    c->surfh = h;
    c->owns_win = 0;
    c->vx = 0; c->vy = 0; c->vw = w; c->vh = h;
    c->zbuf = zbuf;
    gl_defaults(c, w);
    return 1;
}

// The window path. Guarded because it is the ONLY thing in this header that
// needs the window manager and the kernel heap -- three references, all in
// here and in gl_flush. A user program defines GL_NO_WM, supplies its own
// pixels and depth buffer through gl_bind_buf, and gets the same renderer.
#ifndef GL_NO_WM
long gl_bind(struct GLCtx *c, long win, long x, long y, long w, long h) {
    if (w <= 0 || h <= 0 || w > GL_MAXW || h > GL_MAXH) return 0;
    if (!g_win[win].used) return 0;
    c->win = win;
    c->pix = g_win[win].pix;
    c->stride = g_win[win].w;
    c->surfh = g_win[win].h;
    c->owns_win = 1;
    c->vx = x; c->vy = y; c->vw = w; c->vh = h;
    c->zbuf = (long *)kmalloc(w * h * 8);
    if (!c->zbuf) return 0;
    gl_defaults(c, w);
    return 1;
}
#endif

// Record that a pixel has been TOUCHED -- written, or had its depth entry
// written, which the next clear has to undo either way. Separate from the
// damage box because the two are consumed by different things at different
// times: damage is emptied by every flush, dirt is emptied by every clear.
void gl_dirty(struct GLCtx *c, long x, long y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= c->vw) x = c->vw - 1;
    if (y >= c->vh) y = c->vh - 1;
    if (c->cx1 < c->cx0) { c->cx0 = x; c->cx1 = x; c->cy0 = y; c->cy1 = y; return; }
    if (x < c->cx0) c->cx0 = x;
    if (x > c->cx1) c->cx1 = x;
    if (y < c->cy0) c->cy0 = y;
    if (y > c->cy1) c->cy1 = y;
}

// Record that a pixel has CHANGED ON SCREEN, which is the compositor's
// business and not the clear's.
void gl_damage(struct GLCtx *c, long x, long y) {
    if (c->dx1 < c->dx0) { c->dx0 = x; c->dx1 = x; c->dy0 = y; c->dy1 = y; return; }
    if (x < c->dx0) c->dx0 = x;
    if (x > c->dx1) c->dx1 = x;
    if (y < c->dy0) c->dy0 = y;
    if (y > c->dy1) c->dy1 = y;
}

// Both, which is what drawing does: the pixel changed, and it will have to be
// put back to bg before the next frame.
void gl_mark(struct GLCtx *c, long x, long y) {
    gl_dirty(c, x, y);
    gl_damage(c, x, y);
}

// Write one pixel, in VIEWPORT coordinates, into the window's backing buffer.
// Nothing here reaches the screen.
void gl_put(struct GLCtx *c, long x, long y, long colour) {
    long wx;
    long wy;
    if (x < 0 || y < 0 || x >= c->vw || y >= c->vh) return;
    wx = c->vx + x;
    wy = c->vy + y;
    if (wx < 0 || wy < 0 || wx >= c->stride || wy >= c->surfh) return;
    c->pix[wy * c->stride + wx] = colour;
    c->pixels = c->pixels + 1;
    gl_mark(c, x, y);
}

// Fill n words. Behind a function so there is one place to make it fast, and
// so the user-space build -- which cannot call into the kernel's assembly --
// still has something to call.
#ifdef GL_NO_WM
void gl_fill64(long *dst, long val, long n) {
    long i;
    i = 0;
    while (i < n) { dst[i] = val; i = i + 1; }
}
#else
extern void fill64();          // isr.s; declared the way nano_cc likes it
void gl_fill64(long *dst, long val, long n) {
    if (n > 0) fill64(dst, val, n);
}
#endif

// CLEAR WHAT THE LAST FRAME DIRTIED, NOT THE VIEWPORT.
//
// The old version wrote the whole colour buffer and the whole depth buffer
// before it knew what the frame was going to contain, so the cost of a frame
// had a floor set by the size of the viewport and nothing else -- an empty
// screen cost the same as a full one, which is what "the cpu does not go down
// when nothing is drawn" is, from the inside.
//
// Outside the dirty box the colour buffer already holds bg and the depth
// buffer already holds zero: that is what the previous clear put there, and
// gl_dirty says nothing has touched it since. So clearing the box is not an
// approximation of clearing the viewport, it produces the identical buffer --
// which is a claim that gets checked rather than asserted, in glapi.c section
// 12 and gltex.c section 6, by clearing both ways and hashing.
//
// It rests entirely on every write path reporting where it wrote. That was
// already load-bearing for the compositor, but "relied on" and "checked" are
// different things, so the dirty box is deliberately conservative: a triangle
// dirties its whole clipped bounding box, not the pixels that survived the
// depth test.
void gl_clear(struct GLCtx *c) {
    long x0;
    long y0;
    long x1;
    long y1;
    long w;
    long j;

    c->tris_in = 0; c->tris_culled = 0; c->tris_clipped = 0; c->tris_drawn = 0;
    c->boxpix = 0; c->steps = 0; c->covered = 0; c->xforms = 0;
    c->clearpix = 0;

    if (c->clearall) {
        c->cx0 = 0; c->cy0 = 0; c->cx1 = c->vw - 1; c->cy1 = c->vh - 1;
    }
    x0 = c->cx0; y0 = c->cy0; x1 = c->cx1; y1 = c->cy1;
    if (x1 < x0 || y1 < y0) return;        // nothing was dirtied, nothing to undo

    w = x1 - x0 + 1;
    j = y0;
    while (j <= y1) {
        gl_fill64(c->zbuf + j * c->vw + x0, 0, w);   // 1/z of 0 = infinitely far
        gl_fill64(c->pix + (c->vy + j) * c->stride + c->vx + x0, c->bg, w);
        j = j + 1;
    }
    c->clearpix = w * (y1 - y0 + 1);
    c->pixels = c->pixels + c->clearpix;

    // The cleared rectangle held something a moment ago, so the compositor has
    // to repaint it. Damage only: the point of the clear is that the box is
    // now clean, so re-dirtying it here would defeat the whole thing.
    gl_damage(c, x0, y0);
    gl_damage(c, x1, y1);
    c->cx0 = 0; c->cy0 = 0; c->cx1 = -1; c->cy1 = -1;
}

// Hand the damage to the compositor. The whole reason the renderer takes a
// window handle rather than the framebuffer: it says what changed, and the
// compositor decides what that costs.
void gl_flush(struct GLCtx *c) {
    if (c->dx1 < c->dx0) return;                  // nothing was drawn
    // A caller-supplied buffer has no compositor behind it. The damage box is
    // still maintained, because the caller wants it for its own blit.
    if (!c->owns_win) { c->dx0 = 0; c->dy0 = 0; c->dx1 = -1; c->dy1 = -1; return; }
#ifndef GL_NO_WM
    wm_invalidate(c->win, c->vx + c->dx0, c->vy + c->dy0,
                  c->dx1 - c->dx0 + 1, c->dy1 - c->dy0 + 1);
#endif
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

    pix = c->pix;
    stride = c->stride;
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
//
// EVERY MULTIPLY IS OUT OF THE PIXEL LOOP, and that is the whole performance
// story of this function. Each edge function is affine in x and y:
//
//     w0(x, y) = A0*(y - sy1) - B0*(x - sx1),   A0 = sx2-sx1, B0 = sy2-sy1
//
// so stepping x by one subtracts B0 and stepping y by one adds A0. Computing it
// from scratch per pixel -- which is what this did -- is six multiplies and six
// subtractions to rediscover a number that three additions already had.
//
// The same holds for everything interpolated across the triangle. The depth
// numerator w0*iz0 + w1*iz1 + w2*iz2 is a sum of affine functions and is
// therefore affine itself, as are the two perspective-corrected texture
// numerators, so all three are carried along by addition too.
//
// This matters far more here than it would in a compiler with an imul. Programs
// this OS builds are compiled by `cc --minimal`, whose target assembler has no
// multiply instruction at all: every `*` becomes a shift-add loop over the bits
// of the multiplier, and every `/` a 64-step restoring division. A multiply in
// the pixel loop is not one instruction, it is around seventy.
//
// It is exactly the same arithmetic, not an approximation. Integer addition
// reproduces the products bit for bit, so every pixel this writes is the pixel
// the multiplying version wrote -- which is a property the tests check rather
// than something to take on trust.
// Floor division by a POSITIVE divisor. C's / truncates towards zero, which
// is the wrong direction on the negative side -- and the negative side is
// exactly where a span starts when the triangle's left edge is off the left
// of the bounding box. Getting this wrong puts the span one pixel out on
// some rows and not others, which looks like a jagged edge and is not.
long gl_floordiv(long a, long b) {
    long q;
    q = a / b;
    if (a % b != 0 && a < 0) q = q - 1;
    return q;
}

// THE REFERENCE RASTERISER -- the one this file had until K24, kept so the
// replacement can be checked against it rather than against an opinion. It
// finds the covered run on each row by walking into it from the left edge of
// the bounding box, one pixel at a time, testing three edge functions at each.
//
// That prefix is invisible in a screenshot and it is most of the work in a
// frame full of long thin triangles -- which is what a ground plane seen at a
// grazing angle is. Measured on the demo scene: 439,507 inner-loop iterations
// to write 96,070 pixels.
//
// Compiled only where something asks for it, by defining GL_RASTER_REF before
// including this header. wingl.c does not: the compiler INSIDE the OS has to
// lex every token of this file and it has a size ceiling.
#ifdef GL_RASTER_REF
void gl_tri_raster_ref(struct GLCtx *c, long *sx, long *sy, long *iz, long colour) {
    long minx; long miny; long maxx; long maxy;
    long area;
    long y;
    long textured;
    long cr; long cg; long cb;
    struct Texture *tx;
    // Edge coefficients. w_i steps by -B_i across x and +A_i down y.
    long a0; long a1; long a2;
    long b0; long b1; long b2;
    // The three edge functions at the top-left corner of the clipped box.
    long w0r; long w1r; long w2r;
    // Interpolation numerators at the same corner, and their steps.
    long nd; long ndx; long ndy;
    long nu; long nux; long nuy;
    long nv; long nvx; long nvy;
    // Written-pixel extent, so the damage box is updated twice per triangle
    // instead of once per pixel. gl_mark takes a box union, so recording the
    // extremes of what was written gives the identical box.
    long hit; long tx0; long ty0; long tx1; long ty1;

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

    // Clip to the SURFACE as well as the viewport, once, here. gl_put tested
    // this per pixel; doing it per triangle is what lets the inner loop write
    // through the buffer directly, and it is the same test -- a viewport
    // hanging off the edge of its surface is caught either way.
    if (c->vx + minx < 0) minx = 0 - c->vx;
    if (c->vy + miny < 0) miny = 0 - c->vy;
    if (c->vx + maxx > c->stride - 1) maxx = c->stride - 1 - c->vx;
    if (c->vy + maxy > c->surfh - 1) maxy = c->surfh - 1 - c->vy;
    if (minx > maxx || miny > maxy) return;

    // The whole clipped box is dirty from here, whatever the depth test does
    // with it. Deliberately larger than the set of pixels written: a fragment
    // that loses the depth test still wrote a depth value on the way, and
    // there is no version of this that is safe to be clever about.
    gl_dirty(c, minx, miny);
    gl_dirty(c, maxx, maxy);

    c->boxpix = c->boxpix + (maxx - minx + 1) * (maxy - miny + 1);

    area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (area == 0) return;                     // degenerate, zero pixels

    a0 = sx[2] - sx[1]; b0 = sy[2] - sy[1];
    a1 = sx[0] - sx[2]; b1 = sy[0] - sy[2];
    a2 = sx[1] - sx[0]; b2 = sy[1] - sy[0];

    // The one place the edge functions are evaluated the expensive way.
    w0r = a0 * (miny - sy[1]) - b0 * (minx - sx[1]);
    w1r = a1 * (miny - sy[2]) - b1 * (minx - sx[2]);
    w2r = a2 * (miny - sy[0]) - b2 * (minx - sx[0]);

    nd  = w0r * iz[0] + w1r * iz[1] + w2r * iz[2];
    ndx = 0 - (b0 * iz[0] + b1 * iz[1] + b2 * iz[2]);
    ndy = a0 * iz[0] + a1 * iz[1] + a2 * iz[2];

    // The texture numerators are only stepped when there is a texture, so an
    // untextured triangle does not pay for them at all.
    nu = 0; nux = 0; nuy = 0;
    nv = 0; nvx = 0; nvy = 0;
    if (textured) {
        nu  = w0r * c->rs[0] + w1r * c->rs[1] + w2r * c->rs[2];
        nux = 0 - (b0 * c->rs[0] + b1 * c->rs[1] + b2 * c->rs[2]);
        nuy = a0 * c->rs[0] + a1 * c->rs[1] + a2 * c->rs[2];
        nv  = w0r * c->rt[0] + w1r * c->rt[1] + w2r * c->rt[2];
        nvx = 0 - (b0 * c->rt[0] + b1 * c->rt[1] + b2 * c->rt[2]);
        nvy = a0 * c->rt[0] + a1 * c->rt[1] + a2 * c->rt[2];
    }

    hit = 0; tx0 = 0; ty0 = 0; tx1 = 0; ty1 = 0;

    y = miny;
    while (y <= maxy) {
        long x;
        long w0;
        long w1;
        long w2;
        long nn;
        long nuu;
        long nvv;
        long rowbase;
        long zrow;
        long inside;
        long xend;

        inside = 0;
        w0 = w0r; w1 = w1r; w2 = w2r;
        nn = nd; nuu = nu; nvv = nv;
        // The row's two base offsets, one multiply each, per row rather than
        // per pixel. gl_put recomputed the first of these 52,000 times a frame.
        rowbase = (c->vy + y) * c->stride + c->vx;
        zrow = y * c->vw;

        x = minx;
        xend = maxx;
        while (x <= xend) {
            if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                long d;
                long idx;
                inside = 1;
                c->covered = c->covered + 1;
                d = nn / area;
                idx = zrow + x;
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
                        uoz = nuu / area;
                        voz = nvv / area;
                        // rs is the full 32.32 product of s and 1/z, and d is
                        // 1/z in 16.16, so this quotient is s in 16.16 with no
                        // shift. Shifting the product down to 16.16 first
                        // would throw away exactly the bits that matter for a
                        // distant surface, where 1/z is small.
                        if (d != 0) {
                            texel = gl_texel(tx, uoz / d, voz / d);
                            c->pix[rowbase + x] =
                                   rgb(((texel >> 16) & 255) * cr / 255,
                                       ((texel >> 8) & 255) * cg / 255,
                                       (texel & 255) * cb / 255);
                            c->pixels = c->pixels + 1;
                            if (!hit) { hit = 1; tx0 = x; tx1 = x; ty0 = y; ty1 = y; }
                            if (x < tx0) tx0 = x;
                            if (x > tx1) tx1 = x;
                            if (y > ty1) ty1 = y;
                        }
                    } else {
                        c->pix[rowbase + x] = colour;
                        c->pixels = c->pixels + 1;
                        if (!hit) { hit = 1; tx0 = x; tx1 = x; ty0 = y; ty1 = y; }
                        if (x < tx0) tx0 = x;
                        if (x > tx1) tx1 = x;
                        if (y > ty1) ty1 = y;
                    }
                }
            } else if (inside) {
                // A triangle is the intersection of three half-planes, so it
                // is convex, so the covered pixels in a row are one unbroken
                // run. Having been inside it and come out again, there is
                // nothing further along this row -- and on a tall thin
                // triangle that is most of the bounding box.
                //
                // Exact, not an approximation: convexity is a property of the
                // shape, not of the arithmetic.
                //
                // Moving the END of the row rather than x itself, so that x is
                // left holding where the walk actually stopped and the row's
                // cost can be counted without a per-pixel increment.
                xend = x;
            }
            w0 = w0 - b0; w1 = w1 - b1; w2 = w2 - b2;
            nn = nn + ndx;
            nuu = nuu + nux; nvv = nvv + nvx;
            x = x + 1;
        }
        c->steps = c->steps + (x - minx);
        w0r = w0r + a0; w1r = w1r + a1; w2r = w2r + a2;
        nd = nd + ndy;
        nu = nu + nuy; nv = nv + nvy;
        y = y + 1;
    }

    // ty0 is the first row that wrote anything and rows are walked in order, so
    // it needs no minimum test inside the loop -- only ty1 grows.
    if (hit) { gl_mark(c, tx0, ty0); gl_mark(c, tx1, ty1); }
}
#endif  // GL_RASTER_REF

// THE SPAN RASTERISER.
//
// Same arithmetic, same pixels, one less thing to search for. On each row the
// covered run is SOLVED rather than found: an edge function is
//
//     w_i(minx + k) = w_ir - k * b_i
//
// which is linear in k, so "inside" -- w_i >= 0 -- is one inequality in k per
// edge. Whether an edge bounds the run on the left or on the right is the
// sign of b_i and nothing else. Three inequalities, intersected, give [lo,hi]
// directly, and the pixel loop then runs over exactly the covered pixels with
// no edge functions in it at all.
//
// Two savings, and the second is the bigger one on a normal frame:
//
//   1. the walk from the left edge of the bounding box into the triangle is
//      gone -- it was never in the picture
//   2. the three subtractions and up to six comparisons per pixel are gone
//      from the pixels that ARE in the picture
//
// It is the same set of pixels, bit for bit. The proof is not the argument
// above: gl_tri_raster_ref is still here and glapi.c renders the whole demo
// scene through both and compares the buffers.
void gl_tri_raster(struct GLCtx *c, long *sx, long *sy, long *iz, long colour) {
    long minx; long miny; long maxx; long maxy;
    long area;
    long y;
    long textured;
    long cr; long cg; long cb;
    struct Texture *tx;
    // Edge coefficients. w_i steps by -B_i across x and +A_i down y.
    long a0; long a1; long a2;
    long b0; long b1; long b2;
    // The three edge functions at the top-left corner of the clipped box.
    long w0r; long w1r; long w2r;
    // Interpolation numerators at the same corner, and their steps.
    long nd; long ndx; long ndy;
    long nu; long nux; long nuy;
    long nv; long nvx; long nvy;
    // The far test, premultiplied by the area so it can be applied to the
    // numerator directly -- see the note in the pixel loop.
    long farnum;
    // Written-pixel extent, so the damage box is updated twice per triangle
    // instead of once per pixel.
    long hit; long tx0; long ty0; long tx1; long ty1;

    // The texture, and the colour it is modulated by, hoisted out of the loop.
    // GL's default texture environment is GL_MODULATE: the fragment is the
    // texel times the primary colour, which here already carries the lighting
    // term.
    textured = 0;
    tx = &g_tex[0];
    // `pix` and not just `used`. glGenTexture marks a name used before
    // anything has been uploaded to it, which is exactly what GL does, so a
    // program that binds a name and draws before calling glTexImage2D is
    // doing something ordinary and slightly wrong. Without this test it reads
    // through a null pointer and takes the machine down.
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

    // Clip to the SURFACE as well as the viewport, once, here.
    if (c->vx + minx < 0) minx = 0 - c->vx;
    if (c->vy + miny < 0) miny = 0 - c->vy;
    if (c->vx + maxx > c->stride - 1) maxx = c->stride - 1 - c->vx;
    if (c->vy + maxy > c->surfh - 1) maxy = c->surfh - 1 - c->vy;
    if (minx > maxx || miny > maxy) return;

    // The whole clipped box is dirty from here, whatever the depth test does
    // with it. Deliberately larger than the set of pixels written: a fragment
    // that loses the depth test still wrote a depth value on the way, and
    // there is no version of this that is safe to be clever about.
    gl_dirty(c, minx, miny);
    gl_dirty(c, maxx, maxy);

    c->boxpix = c->boxpix + (maxx - minx + 1) * (maxy - miny + 1);

    area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (area == 0) return;                     // degenerate, zero pixels

    a0 = sx[2] - sx[1]; b0 = sy[2] - sy[1];
    a1 = sx[0] - sx[2]; b1 = sy[0] - sy[2];
    a2 = sx[1] - sx[0]; b2 = sy[1] - sy[0];

    // Normalise the winding away, which is what lets the span be solved with
    // one set of inequalities instead of two. Negating the edge coefficients
    // AND the area together leaves every quotient downstream unchanged --
    // nn/area and the two texture ones are each a ratio of two things that
    // both changed sign -- so a clockwise triangle still produces the byte-
    // for-byte pixels it did before. That is the whole reason it is safe.
    if (area < 0) {
        a0 = 0 - a0; a1 = 0 - a1; a2 = 0 - a2;
        b0 = 0 - b0; b1 = 0 - b1; b2 = 0 - b2;
        area = 0 - area;
    }

    // The one place the edge functions are evaluated the expensive way. They
    // come out already negated when the winding was, because a and b are.
    w0r = a0 * (miny - sy[1]) - b0 * (minx - sx[1]);
    w1r = a1 * (miny - sy[2]) - b1 * (minx - sx[2]);
    w2r = a2 * (miny - sy[0]) - b2 * (minx - sx[0]);

    nd  = w0r * iz[0] + w1r * iz[1] + w2r * iz[2];
    ndx = 0 - (b0 * iz[0] + b1 * iz[1] + b2 * iz[2]);
    ndy = a0 * iz[0] + a1 * iz[1] + a2 * iz[2];

    // The texture numerators are only stepped when there is a texture, so an
    // untextured triangle does not pay for them at all.
    nu = 0; nux = 0; nuy = 0;
    nv = 0; nvx = 0; nvy = 0;
    if (textured) {
        nu  = w0r * c->rs[0] + w1r * c->rs[1] + w2r * c->rs[2];
        nux = 0 - (b0 * c->rs[0] + b1 * c->rs[1] + b2 * c->rs[2]);
        nuy = a0 * c->rs[0] + a1 * c->rs[1] + a2 * c->rs[2];
        nv  = w0r * c->rt[0] + w1r * c->rt[1] + w2r * c->rt[2];
        nvx = 0 - (b0 * c->rt[0] + b1 * c->rt[1] + b2 * c->rt[2]);
        nvy = a0 * c->rt[0] + a1 * c->rt[1] + a2 * c->rt[2];
    }

    // `d >= izfar` is `nn / area >= izfar`, and inside the span nn is a sum of
    // non-negative products and area is positive, so it is exactly
    // `nn >= izfar * area` with no division. Hoisted, because it is the same
    // number for every pixel of the triangle.
    farnum = c->izfar * area;

    hit = 0; tx0 = 0; ty0 = 0; tx1 = 0; ty1 = 0;

    y = miny;
    while (y <= maxy) {
        long lo;
        long hi;
        long q;
        long x;
        long xe;
        long nn;
        long nuu;
        long nvv;
        long rowbase;
        long zrow;

        // The covered run, as three inequalities in k where x = minx + k.
        //   b_i > 0 : w_ir - k*b_i >= 0  =>  k <= floor(w_ir / b_i)
        //   b_i < 0 : w_ir - k*b_i >= 0  =>  k >= -floor(w_ir / -b_i)
        //   b_i = 0 : the edge is horizontal and decides the whole row at once
        lo = 0;
        hi = maxx - minx;

        if (b0 > 0)       { q = gl_floordiv(w0r, b0);         if (q < hi) hi = q; }
        else if (b0 < 0)  { q = 0 - gl_floordiv(w0r, 0 - b0); if (q > lo) lo = q; }
        else if (w0r < 0) lo = hi + 1;

        if (b1 > 0)       { q = gl_floordiv(w1r, b1);         if (q < hi) hi = q; }
        else if (b1 < 0)  { q = 0 - gl_floordiv(w1r, 0 - b1); if (q > lo) lo = q; }
        else if (w1r < 0) lo = hi + 1;

        if (b2 > 0)       { q = gl_floordiv(w2r, b2);         if (q < hi) hi = q; }
        else if (b2 < 0)  { q = 0 - gl_floordiv(w2r, 0 - b2); if (q > lo) lo = q; }
        else if (w2r < 0) lo = hi + 1;

        if (lo <= hi) {
            c->steps = c->steps + (hi - lo + 1);
            c->covered = c->covered + (hi - lo + 1);

            // Jump the numerators to the start of the run in one step each,
            // rather than stepping them across a prefix that draws nothing.
            nn = nd + ndx * lo;
            nuu = nu + nux * lo;
            nvv = nv + nvx * lo;
            // The row's two base offsets, one multiply each, per row rather
            // than per pixel.
            rowbase = (c->vy + y) * c->stride + c->vx;
            zrow = y * c->vw;

            x = minx + lo;
            xe = minx + hi;
            while (x <= xe) {
                long idx;
                idx = zrow + x;
                // Every pixel here is inside the triangle by construction, so
                // the only tests left are the two that are about depth. The
                // far one is done on the numerator; the z-buffer one needs the
                // actual reciprocal, so it pays for the divide -- but only
                // once the far test has let it through.
                // TWO THINGS TRIED HERE AND MEASURED, NEITHER OF WHICH WORKED.
                // Do not reach for either again without a paired measurement:
                //
                //   Hoisting c->zbuf, c->pix, c->depth and the pixel counter
                //   into locals for the whole triangle. nano_cc keeps nothing
                //   in a register across a statement, so each of those is a
                //   load per fragment and this looked certain to pay. Output
                //   was bit-identical -- 70635 pixels either way -- and the
                //   fill did not move out of its 5500-6900 us noise band.
                //
                //   The divide below. c->twodiv times one EXTRA divide per
                //   fragment on the same operands, so the delta is a divide
                //   and nothing else: 327-584 us over 27513 fragments, which
                //   is 12-20 ns each and under a tenth of the fill.
                //
                // The fill runs at about 200 ns per fragment and where that
                // time actually goes is still open.
                if (nn >= farnum) {
                    long d;
                    // Larger 1/z is nearer, so a fragment beyond the far plane
                    // has the SMALLER reciprocal. `>` and not `>=` on the
                    // depth test, so that coplanar surfaces drawn later do not
                    // fight for the same pixel.
                    d = nn / area;
                    if (c->twodiv) c->divsink = c->divsink + (nn / area);
                    if (!c->depth || d > c->zbuf[idx]) {
                        c->zbuf[idx] = d;
                        if (textured) {
                            long uoz;
                            long voz;
                            long texel;
                            // PERSPECTIVE CORRECTION, and the whole reason it
                            // is done this way. s and t are not linear in
                            // screen space; s/z and t/z are, and so is 1/z. So
                            // interpolate those three and divide at the end.
                            //
                            // Interpolating s directly is the cheap way and it
                            // is what gave the PlayStation 1 its swimming
                            // floors: a quad split along one diagonal renders
                            // differently from the same quad split along the
                            // other. There is a test for exactly that, because
                            // it is an invariant rather than something to
                            // judge from a screenshot.
                            uoz = nuu / area;
                            voz = nvv / area;
                            // rs is the full 32.32 product of s and 1/z, and d
                            // is 1/z in 16.16, so this quotient is s in 16.16
                            // with no shift. Shifting the product down to
                            // 16.16 first would throw away exactly the bits
                            // that matter for a distant surface, where 1/z is
                            // small.
                            if (d != 0) {
                                texel = gl_texel(tx, uoz / d, voz / d);
                                c->pix[rowbase + x] =
                                       rgb(((texel >> 16) & 255) * cr / 255,
                                           ((texel >> 8) & 255) * cg / 255,
                                           (texel & 255) * cb / 255);
                                c->pixels = c->pixels + 1;
                                if (!hit) { hit = 1; tx0 = x; tx1 = x; ty0 = y; ty1 = y; }
                                if (x < tx0) tx0 = x;
                                if (x > tx1) tx1 = x;
                                if (y > ty1) ty1 = y;
                            }
                        } else {
                            c->pix[rowbase + x] = colour;
                            c->pixels = c->pixels + 1;
                            if (!hit) { hit = 1; tx0 = x; tx1 = x; ty0 = y; ty1 = y; }
                            if (x < tx0) tx0 = x;
                            if (x > tx1) tx1 = x;
                            if (y > ty1) ty1 = y;
                        }
                    }
                }
                nn = nn + ndx;
                nuu = nuu + nux; nvv = nvv + nvx;
                x = x + 1;
            }
        }

        w0r = w0r + a0; w1r = w1r + a1; w2r = w2r + a2;
        nd = nd + ndy;
        nu = nu + nuy; nv = nv + nvy;
        y = y + 1;
    }

    // ty0 is the first row that wrote anything and rows are walked in order, so
    // it needs no minimum test inside the loop -- only ty1 grows.
    if (hit) { gl_mark(c, tx0, ty0); gl_mark(c, tx1, ty1); }
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

#ifdef GL_RASTER_REF
    if (c->refraster) { gl_tri_raster_ref(c, sx, sy, iz, colour); return; }
#endif
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
        long f;
        toeye.x = 0 - a->x; toeye.y = 0 - a->y; toeye.z = 0 - a->z;
        f = v3_dot(&n, &toeye);
        if (c->frontcw) f = 0 - f;
        if (f <= 0) { c->tris_culled = c->tris_culled + 1; return; }
    }

    if (c->xformonly) return;      // measurement only; see GLCtx.xformonly

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

// GL_NO_FRUSTUM leaves the culling helpers out. A program that never calls
// them pays for them anyway when the compiler INSIDE the OS has to lex them,
// and that compiler has a size ceiling this file is close to -- see the note
// at the top of src/gears.c. Excluded by the preprocessor, so the tokens never
// reach the lexer at all.
#ifndef GL_NO_FRUSTUM

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

#endif  // GL_NO_FRUSTUM

// A triangle in MODEL space, transformed by the current modelview.
void gl_tri(struct GLCtx *c, struct M4 *mv, struct V3 *a, struct V3 *b,
            struct V3 *v, long base) {
    struct V3 ta;
    struct V3 tb;
    struct V3 tc;
    m4_apply(&ta, mv, a);
    m4_apply(&tb, mv, b);
    m4_apply(&tc, mv, v);
    c->xforms = c->xforms + 3;
    gl_tri_view(c, &ta, &tb, &tc, base);
}

#endif
