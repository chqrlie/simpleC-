// nano-glapi.h — the OpenGL-shaped layer: a matrix stack, glBegin/glEnd with
// strips, fans and quads, a camera, and a view frustum.
//
// WHY THE NAMES LOOK LIKE THIS
//
// The obvious objection to "OpenGL, but fixed point" is that OpenGL is a float
// API and this is an invented dialect. It is not. Khronos standardised exactly
// this: OpenGL ES 1.1 defines a fixed-point profile whose type GLfixed is
// 16.16 -- the same format this renderer already used -- and gives every entry
// point that takes a real number an `x` suffix. glRotatex, glTranslatex,
// glScalex, glFrustumx, glColor4x, glNormal3x, glMaterialx, glLightx. Those are
// real function names from a real specification, and every one of them here has
// the argument order and the meaning the specification gives it.
//
// The one place this goes beyond ES is glVertex3x. ES 1.1 dropped immediate
// mode entirely -- there is no glBegin in it, only vertex arrays -- so there is
// no standard fixed-point spelling of glVertex. glVertex3x is desktop GL 1.1's
// glVertex3i with the ES suffix, and it is flagged here rather than left to be
// discovered, because a name that looks standard and is not is worse than one
// that obviously is not.
//
// So: the geometry entry points are desktop OpenGL 1.1, the numeric spelling is
// OpenGL ES 1.1, and the primitive enum VALUES below are the real ones --
// GL_TRIANGLE_STRIP really is 0x0005. Code written against this reads like GL
// because it mostly is GL.
//
// WHAT IT DOES NOT HAVE, stated plainly: textures, lighting beyond one
// directional term, display lists, blending, and per-vertex colour
// interpolation. Shading is flat, taking the colour current when a triangle
// completes -- which is what GL's own flat mode does, since the provoking
// vertex of a triangle is its last.
//
// Requires nano-gl.h.

#ifndef NANO_GLAPI_H
#define NANO_GLAPI_H

// The primitive modes, with OpenGL's own values. Worth writing out rather than
// numbering 0..9 by hand: LINE_LOOP is 2 and LINE_STRIP is 3, which is the
// opposite of the order everybody remembers them in.
#define GL_POINTS         0x0000
#define GL_LINES          0x0001
#define GL_LINE_LOOP      0x0002
#define GL_LINE_STRIP     0x0003
#define GL_TRIANGLES      0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN   0x0006
#define GL_QUADS          0x0007
#define GL_QUAD_STRIP     0x0008
#define GL_POLYGON        0x0009

#define GL_MODELVIEW      0x1700
#define GL_PROJECTION     0x1701

#define GL_CULL_FACE      0x0B44
#define GL_LIGHTING       0x0B50
#define GL_DEPTH_TEST     0x0B71
#define GL_TEXTURE_2D     0x0DE1
#define GL_LIGHT0         0x4000
#define GL_NORMALIZE      0x0BA1

#define GL_CW             0x0900
#define GL_CCW            0x0901

#define GL_FLAT           0x1D00
#define GL_SMOOTH         0x1D01

#define GL_FRONT          0x0404
#define GL_BACK           0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_DIFFUSE        0x1201
#define GL_POSITION       0x1203

#define GL_COMPILE        0x1300

// A display list is a recorded command stream replayed through a switch --
// no function pointers, which nano_cc does not have. gears.c is the reason:
// it builds each gear once and calls it every frame, and without this the
// trigonometry for some 1300 vertices would be redone on every frame.
#define GL_MAXLIST  4
#define GL_LISTCAP  1024

#define GLC_BEGIN     1
#define GLC_END       2
#define GLC_VERTEX    3
#define GLC_NORMAL    4
#define GLC_COLOUR    5
#define GLC_TEXCOORD  6

// GL guarantees at least 32 modelview and 2 projection. A scenegraph walk
// pushes once per level, so the modelview depth is the one that has to be
// generous.
#define GL_MV_DEPTH 16
#define GL_PR_DEPTH 4

struct GlState {
    struct GLCtx *c;           // the bound viewport, or 0

    long mode;                 // GL_MODELVIEW or GL_PROJECTION
    struct M4 mv[GL_MV_DEPTH];
    long mvsp;
    struct M4 pr[GL_PR_DEPTH];
    long prsp;

    long prim;                 // current glBegin mode, or -1
    long total;                // vertices seen since glBegin
    long nv;                   // vertices currently held in vbuf
    // The running window of buffered vertices -- four is enough for a quad
    // strip, which is the widest primitive. struct Vtx and not struct V3,
    // because a vertex is a position AND a texture coordinate and the two must
    // not be able to drift apart: keeping them in parallel arrays is how a
    // strip ends up drawing the right shape with the wrong texture on it.
    struct Vtx vbuf[4];
    struct Vtx first;          // fan / loop / polygon anchor
    long cs;                   // the current glTexCoord2x
    long ct;
    long colour;
    long nvalid;               // a normal was given since the last glEnd
    struct V3 nrm;             // ...in model space

    long verts;                // counters, so tests can assert on work done
    long tris;                 // triangles handed to the rasteriser
    long lines;
    long points;
    long overflow;             // glBegin nested, or a matrix stack overrun

    long list;                 // display list being compiled, or 0
    long shade;                // GL_FLAT or GL_SMOOTH; recorded, see below
};

struct GlState g_gls;

// ---------- display lists ----------

long g_list_used[GL_MAXLIST];
long g_list_n[GL_MAXLIST];
long g_list_op[GL_MAXLIST * GL_LISTCAP];
long g_list_a[GL_MAXLIST * GL_LISTCAP];
long g_list_b[GL_MAXLIST * GL_LISTCAP];
long g_list_c[GL_MAXLIST * GL_LISTCAP];
long g_list_over;              // commands dropped because a list filled up

// glGenLists(1). Names are 1-based so that 0 can mean "no list", which is
// what GL does and what lets st->list double as the compiling flag.
long glGenList() {
    long i;
    i = 0;
    while (i < GL_MAXLIST) {
        if (!g_list_used[i]) {
            g_list_used[i] = 1;
            g_list_n[i] = 0;
            return i + 1;
        }
        i = i + 1;
    }
    return 0;
}

void glNewList(struct GlState *st, long list, long mode) {
    if (list < 1 || list > GL_MAXLIST) { st->overflow = st->overflow + 1; return; }
    if (st->list) { st->overflow = st->overflow + 1; return; }   // no nesting
    g_list_used[list - 1] = 1;
    g_list_n[list - 1] = 0;
    st->list = list;
}

void glEndList(struct GlState *st) {
    st->list = 0;
}

long glListSize(long list) {
    if (list < 1 || list > GL_MAXLIST) return 0;
    return g_list_n[list - 1];
}

// Append one command. A full list is COUNTED, not silently truncated: a
// display list that quietly loses its last hundred vertices renders a gear
// with a bite out of it, and that reads as a geometry bug.
void gl_list_add(struct GlState *st, long op, long a, long b, long c) {
    long i;
    long base;
    i = st->list - 1;
    if (g_list_n[i] >= GL_LISTCAP) { g_list_over = g_list_over + 1; return; }
    base = i * GL_LISTCAP + g_list_n[i];
    g_list_op[base] = op;
    g_list_a[base] = a;
    g_list_b[base] = b;
    g_list_c[base] = c;
    g_list_n[i] = g_list_n[i] + 1;
}

// ---------- matrix stack ----------

struct M4 *gl_top(struct GlState *st) {
    if (st->mode == GL_PROJECTION) return &st->pr[st->prsp];
    return &st->mv[st->mvsp];
}

void glMatrixMode(struct GlState *st, long m) { st->mode = m; }

void glLoadIdentity(struct GlState *st) { m4_identity(gl_top(st)); }

void glLoadMatrixx(struct GlState *st, struct M4 *m) {
    long i;
    struct M4 *t;
    t = gl_top(st);
    i = 0;
    while (i < 16) { t->m[i] = m->m[i]; i = i + 1; }
}

// top = top * m. Post-multiplication, which is what makes the GL idiom work:
// the transform written LAST is the one applied to the vertex FIRST.
void glMultMatrixx(struct GlState *st, struct M4 *m) {
    struct M4 *t;
    t = gl_top(st);
    m4_mul(t, t, m);
}

void glPushMatrix(struct GlState *st) {
    long i;
    if (st->mode == GL_PROJECTION) {
        if (st->prsp + 1 >= GL_PR_DEPTH) { st->overflow = st->overflow + 1; return; }
        i = 0;
        while (i < 16) { st->pr[st->prsp + 1].m[i] = st->pr[st->prsp].m[i]; i = i + 1; }
        st->prsp = st->prsp + 1;
        return;
    }
    if (st->mvsp + 1 >= GL_MV_DEPTH) { st->overflow = st->overflow + 1; return; }
    i = 0;
    while (i < 16) { st->mv[st->mvsp + 1].m[i] = st->mv[st->mvsp].m[i]; i = i + 1; }
    st->mvsp = st->mvsp + 1;
}

// An unbalanced pop is counted rather than ignored. Silently clamping at zero
// is how a scenegraph with one missing pop renders correctly for a while and
// then puts a subtree in the wrong place.
void glPopMatrix(struct GlState *st) {
    if (st->mode == GL_PROJECTION) {
        if (st->prsp == 0) { st->overflow = st->overflow + 1; return; }
        st->prsp = st->prsp - 1;
        return;
    }
    if (st->mvsp == 0) { st->overflow = st->overflow + 1; return; }
    st->mvsp = st->mvsp - 1;
}

void glTranslatex(struct GlState *st, long x, long y, long z) {
    struct M4 t;
    m4_translate(&t, x, y, z);
    glMultMatrixx(st, &t);
}

void glScalex(struct GlState *st, long x, long y, long z) {
    struct M4 t;
    m4_identity(&t);
    t.m[0] = x; t.m[5] = y; t.m[10] = z;
    glMultMatrixx(st, &t);
}

// Rotate by `deg` (16.16 degrees) about an arbitrary axis, by Rodrigues'
// formula -- R = cos.I + sin.[k] + (1-cos).k(k^T) -- which is the matrix
// glRotate is specified to build. The axis is normalised first; glRotate
// requires that and silently misbehaves if you skip it.
void glRotatex(struct GlState *st, long deg, long x, long y, long z) {
    struct M4 t;
    struct V3 k;
    struct V3 u;
    long s;
    long c;
    long ic;

    k.x = x; k.y = y; k.z = z;
    v3_norm(&u, &k);
    if (u.x == 0 && u.y == 0 && u.z == 0) return;

    s = gl_sin_fx(deg);
    c = gl_cos_fx(deg);
    ic = GL_ONE - c;

    m4_identity(&t);
    t.m[0]  = c + fx_mul(ic, fx_mul(u.x, u.x));
    t.m[1]  = fx_mul(ic, fx_mul(u.x, u.y)) - fx_mul(s, u.z);
    t.m[2]  = fx_mul(ic, fx_mul(u.x, u.z)) + fx_mul(s, u.y);
    t.m[4]  = fx_mul(ic, fx_mul(u.y, u.x)) + fx_mul(s, u.z);
    t.m[5]  = c + fx_mul(ic, fx_mul(u.y, u.y));
    t.m[6]  = fx_mul(ic, fx_mul(u.y, u.z)) - fx_mul(s, u.x);
    t.m[8]  = fx_mul(ic, fx_mul(u.z, u.x)) - fx_mul(s, u.y);
    t.m[9]  = fx_mul(ic, fx_mul(u.z, u.y)) + fx_mul(s, u.x);
    t.m[10] = c + fx_mul(ic, fx_mul(u.z, u.z));
    glMultMatrixx(st, &t);
}

// glFrustumx. The original takes six numbers -- l, r, b, t, near, far -- and
// with the state pointer that is seven, one past nano_cc's ceiling. So `far`
// lives on the context and is set separately. That is the argument limit
// showing up in the shape of the API for the third time on this project, after
// gluLookAt's nine scalars and K14's widget rectangle.
//
// The near and far given here become the renderer's actual clip planes, not
// just numbers in a matrix: gl_frustum reads them off the context.
void glFrustumx(struct GlState *st, long l, long r, long b, long t, long n) {
    if (!st->c) return;
    st->c->near = n;
    gl_frustum(st->c, l, r, b, t);
    glMultMatrixx(st, &st->c->proj);
    // ...and then take back the product, so that a glLoadIdentity + glFrustumx
    // pair leaves the context agreeing with the stack. gl_sync does the same
    // job at glBegin for every other route in.
    glLoadMatrixx(st, gl_top(st));
}

// gluPerspective, with the far plane taken from the context. fovy is the full
// vertical field of view in 16.16 degrees.
void gluPerspectivex(struct GlState *st, long fovy, long aspect, long n) {
    long t;
    long r;
    long half;
    long si;
    long co;
    if (!st->c) return;
    half = fovy / 2;
    si = gl_sin_fx(half);
    co = gl_cos_fx(half);
    if (co == 0) return;
    t = fx_mul(n, fx_div(si, co));
    r = fx_mul(t, aspect);
    st->c->near = n;
    gl_frustum(st->c, 0 - r, r, 0 - t, t);
    glLoadMatrixx(st, &st->c->proj);
}

// gluLookAt. Nine scalars in the original, which nano_cc cannot express, so it
// takes three vectors -- and reads better for it.
//
// The basis is built for THIS renderer's convention: +z forward, +x right,
// +y up. Standard GL builds -z forward, so its `s` is cross(f, up) and this is
// cross(up, f). Getting that backwards mirrors the world, and a mirrored world
// looks perfectly plausible until text appears in it.
void gluLookAtx(struct GlState *st, struct V3 *eye, struct V3 *at, struct V3 *up) {
    struct V3 f;
    struct V3 s;
    struct V3 u;
    struct V3 d;
    struct M4 m;

    v3_sub(&d, at, eye);
    v3_norm(&f, &d);
    v3_cross(&d, up, &f);
    v3_norm(&s, &d);
    v3_cross(&u, &f, &s);

    m4_identity(&m);
    m.m[0] = s.x; m.m[1] = s.y; m.m[2]  = s.z; m.m[3]  = 0 - v3_dot(&s, eye);
    m.m[4] = u.x; m.m[5] = u.y; m.m[6]  = u.z; m.m[7]  = 0 - v3_dot(&u, eye);
    m.m[8] = f.x; m.m[9] = f.y; m.m[10] = f.z; m.m[11] = 0 - v3_dot(&f, eye);
    glMultMatrixx(st, &m);
}

// ---------- enable / disable ----------

void glEnable(struct GlState *st, long cap) {
    if (!st->c) return;
    if (cap == GL_CULL_FACE)  st->c->cull = 1;
    if (cap == GL_DEPTH_TEST) st->c->depth = 1;
    if (cap == GL_LIGHTING)   st->c->lighting = 1;
    if (cap == GL_TEXTURE_2D) st->c->texturing = 1;
}

void glDisable(struct GlState *st, long cap) {
    if (!st->c) return;
    if (cap == GL_CULL_FACE)  st->c->cull = 0;
    if (cap == GL_DEPTH_TEST) st->c->depth = 0;
    if (cap == GL_LIGHTING)   st->c->lighting = 0;
    if (cap == GL_TEXTURE_2D) st->c->texturing = 0;
}

// ---------- texture objects ----------

// glGenTextures, one at a time. The real one fills an array; nano_cc has no
// trouble with that, but returning the name is what every call site actually
// wants and it makes the failure case (no free slot) a value rather than a
// silent zero.
long glGenTexture() {
    long i;
    i = 0;
    while (i < GL_MAXTEX) {
        if (!g_tex[i].used) { g_tex[i].used = 1; g_tex[i].pix = 0; return i; }
        i = i + 1;
    }
    return -1;
}

void glBindTexture(struct GlState *st, long target, long name) {
    if (!st->c || target != GL_TEXTURE_2D) return;
    st->c->tex = name;
}

// glTexImage2D, with four of its nine arguments dropped.
//
// The real signature is (target, level, internalformat, width, height, border,
// format, type, pixels). Six of those describe a conversion this renderer does
// not do -- there is exactly one texel format here, the same packed RGB the
// window backing buffer holds -- and nine arguments is three past nano_cc's
// ceiling anyway. So the name is GL's and the argument list is not, which is
// worth stating rather than hoping nobody compares.
//
// `pixels` is NOT copied. The caller owns it, exactly as GL does not own the
// pointer you hand it, and the texture is only valid while that memory is.
long glTexImage2D(struct GlState *st, long w, long h, long *pixels) {
    long name;
    if (!st->c) return 0;
    name = st->c->tex;
    if (name < 0 || name >= GL_MAXTEX || !g_tex[name].used) return 0;
    // Powers of two only, and REJECTED rather than rounded. Rounding would
    // give a texture that samples wrongly everywhere, which looks like a bug
    // in the mapping rather than a bug in the upload.
    if (!gl_pow2(w) || !gl_pow2(h) || w < 1 || h < 1) return 0;
    g_tex[name].w = w;
    g_tex[name].h = h;
    g_tex[name].wmask = w - 1;
    g_tex[name].hmask = h - 1;
    g_tex[name].pix = pixels;
    return 1;
}

// ---------- lines and points, in view space ----------

// A segment, clipped to the near plane rather than dropped. Dropping is what a
// wireframe cube does when one corner passes behind the camera: whole edges
// blink out while the face they belong to is still visible.
void gl_seg_view(struct GLCtx *c, struct V3 *a, struct V3 *b, long colour) {
    struct V3 p;
    struct V3 q;
    long ax; long ay; long az;
    long bx; long by; long bz;

    p = *a;
    q = *b;
    if (p.z < c->near && q.z < c->near) return;
    // gl_clip_edge works on a Vtx because a clipped TRIANGLE has to carry its
    // texture coordinate across the cut. A line has none, so the wrapping is
    // just noise -- but sharing the clipper is right anyway: two copies of a
    // near-plane interpolation is two chances to get the sign wrong.
    if (p.z < c->near || q.z < c->near) {
        struct Vtx vp;
        struct Vtx vq;
        struct Vtx o;
        vp.p = p; vp.s = 0; vp.t = 0;
        vq.p = q; vq.s = 0; vq.t = 0;
        if (p.z < c->near) { gl_clip_edge(c, &o, &vq, &vp); p = o.p; }
        else               { gl_clip_edge(c, &o, &vp, &vq); q = o.p; }
    }

    if (!gl_project(c, &p, &ax, &ay, &az)) return;
    if (!gl_project(c, &q, &bx, &by, &bz)) return;
    gl_line(c, ax, ay, bx, by, colour);
}

// A point, drawn two pixels square so that it survives being one pixel from
// the edge of a viewport.
void gl_point_view(struct GLCtx *c, struct V3 *a, long colour) {
    long x; long y; long z;
    if (!gl_project(c, a, &x, &y, &z)) return;
    gl_put(c, x, y, colour);
    gl_put(c, x + 1, y, colour);
    gl_put(c, x, y + 1, colour);
    gl_put(c, x + 1, y + 1, colour);
}

// ---------- glBegin / glVertex / glEnd ----------

void glColor3ub(struct GlState *st, long r, long g, long b) {
    if (st->list) { gl_list_add(st, GLC_COLOUR, r, g, b); return; }
    st->colour = rgb(r, g, b);
}

// glColor4x's components are 0..GL_ONE, as the specification says. Alpha is
// accepted and ignored -- there is no blending -- rather than left out, so
// that code written against it does not have to be edited when there is.
void glColor4x(struct GlState *st, long r, long g, long b, long a) {
    st->colour = rgb(fx_to_int(r * 255), fx_to_int(g * 255), fx_to_int(b * 255));
}

void glNormal3x(struct GlState *st, long x, long y, long z) {
    if (st->list) { gl_list_add(st, GLC_NORMAL, x, y, z); return; }
    st->nrm.x = x; st->nrm.y = y; st->nrm.z = z;
    st->nvalid = 1;
}

// glTexCoord2x. Like glColor and glNormal, this is CURRENT STATE: it applies
// to every vertex issued after it until it is changed, and each vertex carries
// away its own copy. That is what makes `texcoord; vertex; texcoord; vertex`
// work, and it is why the coordinate is stored into the vertex buffer rather
// than read again when the triangle completes -- by then the caller has moved
// on and set the next one.
void glTexCoord2x(struct GlState *st, long s, long t) {
    if (st->list) { gl_list_add(st, GLC_TEXCOORD, s, t, 0); return; }
    st->cs = s;
    st->ct = t;
}

void glBegin(struct GlState *st, long mode) {
    if (st->list) { gl_list_add(st, GLC_BEGIN, mode, 0, 0); return; }
    if (st->prim >= 0) { st->overflow = st->overflow + 1; return; }
    st->prim = mode;
    st->total = 0;
    st->nv = 0;
    // The projection lives on the stack, the renderer reads it off the
    // context. One copy here is what keeps them the same matrix, and doing it
    // at glBegin means it is impossible to draw with a stale one.
    if (st->c) {
        long i;
        i = 0;
        while (i < 16) { st->c->proj.m[i] = st->pr[st->prsp].m[i]; i = i + 1; }
    }
}

// Hand three vertices to the rasteriser, with the current colour and, if one
// was given, the current normal transformed into view space.
//
// The texture coordinates go onto the context here, in the same order as the
// vertices. Every caller below passes the SAME struct Vtx it stored the
// position in, so a triangle cannot end up with one vertex's position and
// another's texture coordinate -- which is the failure mode of keeping them in
// two parallel arrays and indexing them separately.
void gl_emit_tri(struct GlState *st, struct Vtx *a, struct Vtx *b, struct Vtx *c) {
    if (!st->c) return;
    if (st->nvalid) {
        struct V3 n;
        m4_apply_dir(&n, &st->mv[st->mvsp], &st->nrm);
        st->c->nrm = n;
        st->c->nvalid = 1;
    } else {
        st->c->nvalid = 0;
    }
    st->c->vs[0] = a->s; st->c->vt[0] = a->t;
    st->c->vs[1] = b->s; st->c->vt[1] = b->t;
    st->c->vs[2] = c->s; st->c->vt[2] = c->t;
    st->tris = st->tris + 1;
    gl_tri_view(st->c, &a->p, &b->p, &c->p, st->colour);
}

void glVertex3x(struct GlState *st, long x, long y, long z) {
    struct V3 m;
    struct Vtx p;

    if (st->list) { gl_list_add(st, GLC_VERTEX, x, y, z); return; }
    if (st->prim < 0 || !st->c) return;
    m.x = x; m.y = y; m.z = z;
    // Model space to view space, once, here -- exactly where OpenGL does it.
    m4_apply(&p.p, &st->mv[st->mvsp], &m);
    // The texture coordinate is snapshotted onto the vertex NOW, not read
    // later. glTexCoord is current state and the caller will have changed it
    // by the time this vertex completes a triangle.
    p.s = st->cs;
    p.t = st->ct;
    st->verts = st->verts + 1;

    if (st->prim == GL_POINTS) {
        gl_point_view(st->c, &p.p, st->colour);
        st->points = st->points + 1;
        st->total = st->total + 1;
        return;
    }

    if (st->prim == GL_LINES) {
        if (st->nv == 0) { st->vbuf[0] = p; st->nv = 1; }
        else {
            gl_seg_view(st->c, &st->vbuf[0].p, &p.p, st->colour);
            st->lines = st->lines + 1;
            st->nv = 0;
        }
        st->total = st->total + 1;
        return;
    }

    if (st->prim == GL_LINE_STRIP || st->prim == GL_LINE_LOOP) {
        if (st->total == 0) { st->vbuf[0] = p; st->first = p; }
        else {
            gl_seg_view(st->c, &st->vbuf[0].p, &p.p, st->colour);
            st->lines = st->lines + 1;
            st->vbuf[0] = p;
        }
        st->total = st->total + 1;
        return;
    }

    if (st->prim == GL_TRIANGLES) {
        st->vbuf[st->nv] = p;
        st->nv = st->nv + 1;
        if (st->nv == 3) {
            gl_emit_tri(st, &st->vbuf[0], &st->vbuf[1], &st->vbuf[2]);
            st->nv = 0;
        }
        st->total = st->total + 1;
        return;
    }

    if (st->prim == GL_TRIANGLE_STRIP) {
        if (st->total < 2) st->vbuf[st->total] = p;
        else {
            // Every other triangle has its first two vertices swapped, which
            // is what keeps the whole strip wound the same way round. Without
            // it, half the strip is back-facing and disappears under culling
            // -- the classic "my strip renders as stripes" bug.
            if (st->total & 1) gl_emit_tri(st, &st->vbuf[1], &st->vbuf[0], &p);
            else               gl_emit_tri(st, &st->vbuf[0], &st->vbuf[1], &p);
            st->vbuf[0] = st->vbuf[1];
            st->vbuf[1] = p;
        }
        st->total = st->total + 1;
        return;
    }

    if (st->prim == GL_TRIANGLE_FAN || st->prim == GL_POLYGON) {
        if (st->total == 0) st->first = p;
        else if (st->total == 1) st->vbuf[0] = p;
        else {
            gl_emit_tri(st, &st->first, &st->vbuf[0], &p);
            st->vbuf[0] = p;
        }
        st->total = st->total + 1;
        return;
    }

    if (st->prim == GL_QUADS) {
        st->vbuf[st->nv] = p;
        st->nv = st->nv + 1;
        if (st->nv == 4) {
            gl_emit_tri(st, &st->vbuf[0], &st->vbuf[1], &st->vbuf[2]);
            gl_emit_tri(st, &st->vbuf[0], &st->vbuf[2], &st->vbuf[3]);
            st->nv = 0;
        }
        st->total = st->total + 1;
        return;
    }

    if (st->prim == GL_QUAD_STRIP) {
        // Vertices arrive in pairs and the quad is v(2k), v(2k+1), v(2k+3),
        // v(2k+2) -- note the last two are the far pair in REVERSE order,
        // which is the detail that makes a quad strip wind consistently and
        // the detail everyone gets wrong.
        if (st->total < 2) st->vbuf[st->total] = p;
        else if ((st->total & 1) == 0) st->vbuf[2] = p;
        else {
            st->vbuf[3] = p;
            gl_emit_tri(st, &st->vbuf[0], &st->vbuf[1], &st->vbuf[3]);
            gl_emit_tri(st, &st->vbuf[0], &st->vbuf[3], &st->vbuf[2]);
            st->vbuf[0] = st->vbuf[2];
            st->vbuf[1] = st->vbuf[3];
        }
        st->total = st->total + 1;
        return;
    }
}

void glEnd(struct GlState *st) {
    if (st->list) { gl_list_add(st, GLC_END, 0, 0, 0); return; }
    if (st->prim == GL_LINE_LOOP && st->total > 2 && st->c) {
        gl_seg_view(st->c, &st->vbuf[0].p, &st->first.p, st->colour);
        st->lines = st->lines + 1;
    }
    st->prim = -1;
    st->nvalid = 0;
    if (st->c) st->c->nvalid = 0;
}

// Replay a recorded list. The modelview in force at the call is the one the
// vertices go through, which is what makes one recorded gear usable at three
// positions and three rotations -- the list holds MODEL space, not view space.
void glCallList(struct GlState *st, long list) {
    long i;
    long k;
    long n;
    long base;
    long saved;

    if (list < 1 || list > GL_MAXLIST) { st->overflow = st->overflow + 1; return; }
    if (st->list) { st->overflow = st->overflow + 1; return; }   // no nesting

    i = list - 1;
    n = g_list_n[i];
    base = i * GL_LISTCAP;

    saved = st->list;
    st->list = 0;

    k = 0;
    while (k < n) {
        long op;
        op = g_list_op[base + k];

        if (op == GLC_VERTEX) {
            glVertex3x(st, g_list_a[base + k], g_list_b[base + k], g_list_c[base + k]);
        } else if (op == GLC_NORMAL) {
            glNormal3x(st, g_list_a[base + k], g_list_b[base + k], g_list_c[base + k]);
        } else if (op == GLC_BEGIN) {
            glBegin(st, g_list_a[base + k]);
        } else if (op == GLC_END) {
            glEnd(st);
        } else if (op == GLC_COLOUR) {
            glColor3ub(st, g_list_a[base + k], g_list_b[base + k], g_list_c[base + k]);
        } else if (op == GLC_TEXCOORD) {
            glTexCoord2x(st, g_list_a[base + k], g_list_b[base + k]);
        }

        k = k + 1;
    }

    st->list = saved;
}

// ---------- material, light, shading ----------

// glMaterialfv, for the one case this renderer can honour. There is a single
// colour per surface here, so ambient and diffuse cannot be set apart: the
// call sets the current colour, and gl_shade's ambient floor is the ambient
// term. Saying so is better than accepting four channels and dropping three.
void glMaterialx(struct GlState *st, long face, long pname, long r, long g, long b) {
    if (pname != GL_AMBIENT_AND_DIFFUSE && pname != GL_DIFFUSE) return;
    glColor3ub(st, fx_to_int(r * 255), fx_to_int(g * 255), fx_to_int(b * 255));
}

// glLightfv(GL_LIGHT0, GL_POSITION, ...) with w = 0, which is a DIRECTIONAL
// light -- the only kind here. The vector is normalised and taken as being in
// view space; a positional light would need the modelview applied and a
// per-vertex direction, which flat shading has nowhere to put.
void glLightDirx(struct GlState *st, long x, long y, long z) {
    struct V3 d;
    struct V3 u;
    if (!st->c) return;
    d.x = x; d.y = y; d.z = z;
    v3_norm(&u, &d);
    if (u.x == 0 && u.y == 0 && u.z == 0) return;
    st->c->light = u;
}

// glShadeModel. This rasteriser is flat-shaded: one colour per triangle, from
// one normal. GL_SMOOTH is accepted and RECORDED rather than ignored, so a
// caller can ask what happened to it, and so the day Gouraud arrives the call
// site does not have to change.
// glFrontFace. The default here is the winding this renderer's own demos
// use; GL_CW selects the other one, which is what a model authored against
// standard GL's -z-forward convention needs.
void glFrontFace(struct GlState *st, long mode) {
    if (!st->c) return;
    st->c->frontcw = (mode == GL_CW);
}

void glShadeModel(struct GlState *st, long m) {
    st->shade = m;
}

// ---------- the frustum, from the live matrices ----------

// clip = projection * modelview, which is the matrix the six planes come out
// of. Extracting from this gives planes in MODEL space, so an object's
// bounding sphere can be tested without transforming it at all.
void gl_clip_matrix(struct GlState *st, struct M4 *o) {
    m4_mul(o, &st->pr[st->prsp], &st->mv[st->mvsp]);
}

void gl_cull_setup(struct GlState *st, struct Frustum *f) {
    struct M4 clip;
    gl_clip_matrix(st, &clip);
    gl_frustum_extract(f, &clip);
}

// ---------- a camera you can fly ----------
//
// Yaw and pitch in 16.16 degrees, because that is what a mouse drag produces
// -- a drag of seven pixels at a third of a degree each is not a whole number
// of degrees and rounding it to one makes the camera stutter.

struct Camera {
    struct V3 eye;
    long yaw;                  // 0 looks along +z
    long pitch;                // positive looks up
    long fov;                  // full vertical field of view, 16.16 degrees
};

void cam_init(struct Camera *cam) {
    cam->eye.x = 0; cam->eye.y = 0; cam->eye.z = 0;
    cam->yaw = 0;
    cam->pitch = 0;
    cam->fov = 60 << GL_FRAC;
}

// The camera's own axes in world space.
void cam_axes(struct Camera *cam, struct V3 *fwd, struct V3 *right) {
    long sy; long cy; long sp; long cp;
    sy = gl_sin_fx(cam->yaw);
    cy = gl_cos_fx(cam->yaw);
    sp = gl_sin_fx(cam->pitch);
    cp = gl_cos_fx(cam->pitch);
    fwd->x = fx_mul(sy, cp);
    fwd->y = sp;
    fwd->z = fx_mul(cy, cp);
    // The right vector is taken at zero pitch on purpose: strafing should not
    // drift you upwards just because you are looking at the sky.
    right->x = cy;
    right->y = 0;
    right->z = 0 - sy;
}

// Load the camera into the current matrix, which should be the modelview.
void cam_apply(struct GlState *st, struct Camera *cam) {
    struct V3 fwd;
    struct V3 right;
    struct V3 at;
    struct V3 up;
    cam_axes(cam, &fwd, &right);
    at.x = cam->eye.x + fwd.x;
    at.y = cam->eye.y + fwd.y;
    at.z = cam->eye.z + fwd.z;
    up.x = 0; up.y = GL_ONE; up.z = 0;
    gluLookAtx(st, &cam->eye, &at, &up);
}

// Move along the camera's own axes. Distances are 16.16 world units.
void cam_move(struct Camera *cam, long fwd, long strafe, long up) {
    struct V3 f;
    struct V3 r;
    cam_axes(cam, &f, &r);
    cam->eye.x = cam->eye.x + fx_mul(f.x, fwd) + fx_mul(r.x, strafe);
    cam->eye.y = cam->eye.y + fx_mul(f.y, fwd) + up;
    cam->eye.z = cam->eye.z + fx_mul(f.z, fwd) + fx_mul(r.z, strafe);
}

// Pitch is clamped just short of straight up. At exactly ninety degrees the
// forward vector is parallel to `up` and gluLookAt's cross product is zero,
// which normalises to zero and produces a matrix of nothing but zeroes -- a
// black viewport, not an error.
void cam_look(struct Camera *cam, long dyaw, long dpitch) {
    cam->yaw = cam->yaw + dyaw;
    cam->pitch = cam->pitch + dpitch;
    if (cam->pitch > 89 << GL_FRAC) cam->pitch = 89 << GL_FRAC;
    if (cam->pitch < 0 - (89 << GL_FRAC)) cam->pitch = 0 - (89 << GL_FRAC);
    while (cam->yaw >= 360 << GL_FRAC) cam->yaw = cam->yaw - (360 << GL_FRAC);
    while (cam->yaw < 0) cam->yaw = cam->yaw + (360 << GL_FRAC);
}

// ---------- setup ----------

void gl_state_init(struct GlState *st, struct GLCtx *c) {
    st->c = c;
    st->mode = GL_MODELVIEW;
    st->mvsp = 0;
    st->prsp = 0;
    m4_identity(&st->mv[0]);
    m4_identity(&st->pr[0]);
    if (c) {
        long i;
        i = 0;
        while (i < 16) { st->pr[0].m[i] = c->proj.m[i]; i = i + 1; }
    }
    st->prim = -1;
    st->total = 0;
    st->nv = 0;
    st->cs = 0;
    st->ct = 0;
    st->colour = rgb(200, 200, 200);
    st->nvalid = 0;
    st->verts = 0;
    st->tris = 0;
    st->lines = 0;
    st->points = 0;
    st->overflow = 0;
    st->list = 0;
    st->shade = GL_FLAT;
}


#endif
