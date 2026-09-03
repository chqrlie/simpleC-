// gears.c — Brian Paul's gears, on this machine's own renderer.
//
// The program is the one everybody knows: three meshing gears, lit, rotating,
// z-buffered. It is a good target because it is not a demo written to flatter
// a renderer. It uses quad strips and quads, per-face normals in the middle of
// a strip, display lists, backface culling and a depth buffer, and it looks
// obviously wrong if any of those is subtly broken.
//
// Two things had to be true before it could be ported at all, and neither is
// a matter of typing:
//
//   THERE ARE NO FLOATS. GLfloat becomes 16.16, sin and cos become the
//   interpolated degree table from K16, and sqrt becomes fx_sqrt. The gear
//   geometry is trigonometry all the way down, so this is most of the port.
//
//   THERE ARE NO FUNCTION POINTERS, so a display list cannot be a list of
//   closures. It is a recorded command stream -- opcode plus three longs --
//   replayed through a switch, which is the same shape as the syscall
//   dispatcher and the widget IDs. gears.c needs them: it builds each gear
//   once and calls it every frame, and without that the trigonometry for
//   ~1300 vertices would be redone sixty times a second.
//
// What is asserted here, rather than looked at:
//
//   1. A display list draws EXACTLY what immediate mode draws. Same gear, one
//      through glCallList and one through the calls themselves, hashed. If
//      recording drops or reorders anything this is where it shows.
//
//   2. Backface culling does not change the picture. A gear is a closed
//      solid, so every back face is behind a front face, and with a depth
//      buffer the result cannot depend on whether the back faces were drawn.
//      The control matters as much as the check: with the depth test OFF the
//      picture MUST change, otherwise the equality is being satisfied by an
//      empty viewport.
//
//   3. The teeth are actually there. A gear rendered without its teeth is
//      still a plausible-looking disc, so the count of lit pixels outside the
//      root radius is checked against zero.
//
// nano-kernel.h first, as in every graphics image: it only mirrors console
// output onto the framebuffer if NANO_FB_H is already defined, and here it
// must not, because this image reads the framebuffer back.
#include "nano-kernel.h"
#include "nano-fb.h"
// For the PM timer only, and only for the frame-cost report: a frame here is
// a couple of milliseconds and the PIT tick is ten, so timing one on g_ticks
// would be measuring the clock.
#include "nano-acpi.h"
#include "nano-mouse.h"
#include "nano-int.h"
#include "nano-mm.h"
#include "nano-wm.h"
#include "nano-wmin.h"
#include "nano-term.h"
#include "nano-ui.h"
#include "nano-gl.h"
#define GL_VBO
#include "nano-glapi.h"

long g_fail;

void fail(char *msg) {
    printf("FAIL: %s\n", msg);
    g_fail = g_fail + 1;
}

void expect_true(char *what, long cond) {
    if (cond) printf("  ok  %s\n", what);
    else fail(what);
}

void expect(char *what, long got, long want) {
    if (got == want) printf("  ok  %s = %d\n", what, got);
    else {
        printf("  got %d, wanted %d\n", got, want);
        fail(what);
    }
}

#define VPW  300
#define VPH  220
#define VPX  (WM_BORDER + 4)
#define VPY  (WM_TITLE_H + 4)

struct GLCtx g_gl;
long g_win3d;

long win_hash(long win, long x, long y, long w, long h) {
    long hash;
    long j;
    hash = 5381;
    j = 0;
    while (j < h) {
        long i;
        i = 0;
        while (i < w) {
            hash = ((hash * 33) + g_win[win].pix[(y + j) * g_win[win].w + x + i])
                   & 0xFFFFFFFF;
            i = i + 1;
        }
        j = j + 1;
    }
    return hash;
}

// Pixels in the viewport that are not the clear colour. The number the
// equalities below are measured against: two hashes agreeing means nothing
// if both viewports are empty.
long win_drawn(long win, long x, long y, long w, long h) {
    long n;
    long j;
    n = 0;
    j = 0;
    while (j < h) {
        long i;
        i = 0;
        while (i < w) {
            if (g_win[win].pix[(y + j) * g_win[win].w + x + i] != g_gl.bg) n = n + 1;
            i = i + 1;
        }
        j = j + 1;
    }
    return n;
}

// A copy of the viewport, and the count of pixels that differ from it. A
// hash says two pictures are not the same; this says by how much, which is
// the difference between "the winding is inverted" and "two silhouette
// pixels moved".
long g_snap[VPW * VPH];

void win_snap(long win, long x, long y, long w, long h) {
    long j;
    j = 0;
    while (j < h) {
        long i;
        i = 0;
        while (i < w) {
            g_snap[j * w + i] = g_win[win].pix[(y + j) * g_win[win].w + x + i];
            i = i + 1;
        }
        j = j + 1;
    }
}

long win_diff(long win, long x, long y, long w, long h) {
    long n;
    long j;
    n = 0;
    j = 0;
    while (j < h) {
        long i;
        i = 0;
        while (i < w) {
            if (g_snap[j * w + i] != g_win[win].pix[(y + j) * g_win[win].w + x + i])
                n = n + 1;
            i = i + 1;
        }
        j = j + 1;
    }
    return n;
}

// How many distinct colours are on screen. Flat shading gives one colour per
// triangle orientation, so this is how "the light is doing something" gets
// measured without looking at the picture.
#define SHADE_MAX 512
long g_shades[SHADE_MAX];

long win_shades(long win, long x, long y, long w, long h) {
    long n;
    long j;
    n = 0;
    j = 0;
    while (j < h) {
        long i;
        i = 0;
        while (i < w) {
            long c;
            long k;
            long seen;
            c = g_win[win].pix[(y + j) * g_win[win].w + x + i];
            seen = 0;
            k = 0;
            while (k < n) {
                if (g_shades[k] == c) { seen = 1; k = n; }
                k = k + 1;
            }
            if (!seen && n < SHADE_MAX) { g_shades[n] = c; n = n + 1; }
            i = i + 1;
        }
        j = j + 1;
    }
    return n;
}

// ---------- the gear ----------
//
// A direct transcription of Brian Paul's gear(), with three changes and no
// others: floats become 16.16, radians become degrees because that is what
// the sine table is indexed by, and the arguments are one fewer because
// nano_cc stops at six and the state pointer is global here.
//
//   r0 = inner radius, r1 = root radius, r2 = tip radius
//   da = a quarter of the angle between teeth
//
// The tooth face normals are the interesting part: they are computed from the
// edge, normalised with fx_sqrt, and issued INSIDE the quad strip, so the
// flat sides of a tooth catch the light differently from its tip. That is a
// glNormal3x mid-primitive, which is exactly the case the renderer's
// "supplied normal beats geometric normal" rule exists for.

long g_teeth;                  // the gear currently being built, for the tests
long g_r1;
long g_r2;

void gear(long inner_radius, long outer_radius, long width, long teeth,
          long tooth_depth) {
    long i;
    long r0;
    long r1;
    long r2;
    long angle;
    long da;
    long hw;
    long full;

    r0 = inner_radius;
    r1 = outer_radius - tooth_depth / 2;
    r2 = outer_radius + tooth_depth / 2;

    g_teeth = teeth;
    g_r1 = r1;
    g_r2 = r2;

    full = 360 * GL_ONE;
    da = full / teeth / 4;
    hw = width / 2;

    glShadeModel(&g_gls, GL_FLAT);

    glNormal3x(&g_gls, 0, 0, GL_ONE);

    // front face
    glBegin(&g_gls, GL_QUAD_STRIP);
    i = 0;
    while (i <= teeth) {
        angle = i * full / teeth;
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)),
                           fx_mul(r0, gl_sin_fx(angle)), hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)),
                           fx_mul(r1, gl_sin_fx(angle)), hw);
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)),
                           fx_mul(r0, gl_sin_fx(angle)), hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), hw);
        i = i + 1;
    }
    glEnd(&g_gls);

    // front sides of the teeth
    glBegin(&g_gls, GL_QUADS);
    i = 0;
    while (i < teeth) {
        angle = i * full / teeth;
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)),
                           fx_mul(r1, gl_sin_fx(angle)), hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + da)),
                           fx_mul(r2, gl_sin_fx(angle + da)), hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                           fx_mul(r2, gl_sin_fx(angle + 2 * da)), hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), hw);
        i = i + 1;
    }
    glEnd(&g_gls);

    glNormal3x(&g_gls, 0, 0, 0 - GL_ONE);

    // back face
    glBegin(&g_gls, GL_QUAD_STRIP);
    i = 0;
    while (i <= teeth) {
        angle = i * full / teeth;
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)),
                           fx_mul(r1, gl_sin_fx(angle)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)),
                           fx_mul(r0, gl_sin_fx(angle)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)),
                           fx_mul(r0, gl_sin_fx(angle)), 0 - hw);
        i = i + 1;
    }
    glEnd(&g_gls);

    // back sides of the teeth
    glBegin(&g_gls, GL_QUADS);
    i = 0;
    while (i < teeth) {
        angle = i * full / teeth;
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                           fx_mul(r2, gl_sin_fx(angle + 2 * da)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + da)),
                           fx_mul(r2, gl_sin_fx(angle + da)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)),
                           fx_mul(r1, gl_sin_fx(angle)), 0 - hw);
        i = i + 1;
    }
    glEnd(&g_gls);

    // outward faces of the teeth
    glBegin(&g_gls, GL_QUAD_STRIP);
    i = 0;
    while (i < teeth) {
        long u;
        long v;
        long len;

        angle = i * full / teeth;

        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)),
                           fx_mul(r1, gl_sin_fx(angle)), hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)),
                           fx_mul(r1, gl_sin_fx(angle)), 0 - hw);

        u = fx_mul(r2, gl_cos_fx(angle + da)) - fx_mul(r1, gl_cos_fx(angle));
        v = fx_mul(r2, gl_sin_fx(angle + da)) - fx_mul(r1, gl_sin_fx(angle));
        len = fx_sqrt(fx_mul(u, u) + fx_mul(v, v));
        if (len) { u = fx_div(u, len); v = fx_div(v, len); }
        glNormal3x(&g_gls, v, 0 - u, 0);

        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + da)),
                           fx_mul(r2, gl_sin_fx(angle + da)), hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + da)),
                           fx_mul(r2, gl_sin_fx(angle + da)), 0 - hw);

        glNormal3x(&g_gls, gl_cos_fx(angle), gl_sin_fx(angle), 0);

        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                           fx_mul(r2, gl_sin_fx(angle + 2 * da)), hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                           fx_mul(r2, gl_sin_fx(angle + 2 * da)), 0 - hw);

        u = fx_mul(r1, gl_cos_fx(angle + 3 * da)) - fx_mul(r2, gl_cos_fx(angle + 2 * da));
        v = fx_mul(r1, gl_sin_fx(angle + 3 * da)) - fx_mul(r2, gl_sin_fx(angle + 2 * da));
        len = fx_sqrt(fx_mul(u, u) + fx_mul(v, v));
        if (len) { u = fx_div(u, len); v = fx_div(v, len); }
        glNormal3x(&g_gls, v, 0 - u, 0);

        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), 0 - hw);

        glNormal3x(&g_gls, gl_cos_fx(angle), gl_sin_fx(angle), 0);
        i = i + 1;
    }

    glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(0)), fx_mul(r1, gl_sin_fx(0)), hw);
    glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(0)), fx_mul(r1, gl_sin_fx(0)), 0 - hw);
    glEnd(&g_gls);

    glShadeModel(&g_gls, GL_SMOOTH);

    // inside radius cylinder
    glBegin(&g_gls, GL_QUAD_STRIP);
    i = 0;
    while (i <= teeth) {
        angle = i * full / teeth;
        glNormal3x(&g_gls, 0 - gl_cos_fx(angle), 0 - gl_sin_fx(angle), 0);
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)),
                           fx_mul(r0, gl_sin_fx(angle)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)),
                           fx_mul(r0, gl_sin_fx(angle)), hw);
        i = i + 1;
    }
    glEnd(&g_gls);
}

// ---------- the scene ----------

long g_gear1;
long g_gear2;
long g_gear3;

long g_view_rotx;
long g_view_roty;
long g_view_rotz;

// The projection gears.c asks for: glFrustum(-1, 1, -h, h, 5, 60) with
// h = height/width, and the whole scene pushed 40 units away.
//
// One sign, and it is the sign that matters: this renderer looks along +z,
// standard GL along -z, so the original's glTranslatef(0, 0, -40) is a
// translate of +40 here. Getting it backwards puts the gears behind the
// camera, which looks exactly like a black window.
void setup_projection() {
    long h;
    h = (VPH * GL_ONE) / VPW;
    g_gl.far = 60 * GL_ONE;
    glMatrixMode(&g_gls, GL_PROJECTION);
    glLoadIdentity(&g_gls);
    glFrustumx(&g_gls, 0 - GL_ONE, GL_ONE, 0 - h, h, 5 * GL_ONE);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    glTranslatex(&g_gls, 0, 0, 40 * GL_ONE);
}

void build_gears() {
    g_gear1 = glGenList();
    glNewList(&g_gls, g_gear1, GL_COMPILE);
    glMaterialx(&g_gls, GL_FRONT, GL_AMBIENT_AND_DIFFUSE,
                (GL_ONE * 8) / 10, GL_ONE / 10, 0);
    gear(GL_ONE, 4 * GL_ONE, GL_ONE, 20, (GL_ONE * 7) / 10);
    glEndList(&g_gls);

    g_gear2 = glGenList();
    glNewList(&g_gls, g_gear2, GL_COMPILE);
    glMaterialx(&g_gls, GL_FRONT, GL_AMBIENT_AND_DIFFUSE,
                0, (GL_ONE * 8) / 10, (GL_ONE * 2) / 10);
    gear(GL_ONE / 2, 2 * GL_ONE, 2 * GL_ONE, 10, (GL_ONE * 7) / 10);
    glEndList(&g_gls);

    g_gear3 = glGenList();
    glNewList(&g_gls, g_gear3, GL_COMPILE);
    glMaterialx(&g_gls, GL_FRONT, GL_AMBIENT_AND_DIFFUSE,
                (GL_ONE * 2) / 10, (GL_ONE * 2) / 10, GL_ONE);
    gear((GL_ONE * 13) / 10, 2 * GL_ONE, GL_ONE / 2, 10, (GL_ONE * 7) / 10);
    glEndList(&g_gls);
}

// draw(), transcribed. `angle` is 16.16 degrees.
void draw_frame(long angle) {
    gl_clear(&g_gl);
    setup_projection();

    glPushMatrix(&g_gls);
    glRotatex(&g_gls, g_view_rotx, GL_ONE, 0, 0);
    glRotatex(&g_gls, g_view_roty, 0, GL_ONE, 0);
    glRotatex(&g_gls, g_view_rotz, 0, 0, GL_ONE);

    glPushMatrix(&g_gls);
    glTranslatex(&g_gls, 0 - 3 * GL_ONE, 0 - 2 * GL_ONE, 0);
    glRotatex(&g_gls, angle, 0, 0, GL_ONE);
    glCallList(&g_gls, g_gear1);
    glPopMatrix(&g_gls);

    glPushMatrix(&g_gls);
    glTranslatex(&g_gls, (31 * GL_ONE) / 10, 0 - 2 * GL_ONE, 0);
    glRotatex(&g_gls, 0 - 2 * angle - 9 * GL_ONE, 0, 0, GL_ONE);
    glCallList(&g_gls, g_gear2);
    glPopMatrix(&g_gls);

    glPushMatrix(&g_gls);
    glTranslatex(&g_gls, 0 - (31 * GL_ONE) / 10, (42 * GL_ONE) / 10, 0);
    glRotatex(&g_gls, 0 - 2 * angle - 25 * GL_ONE, 0, 0, GL_ONE);
    glCallList(&g_gls, g_gear3);
    glPopMatrix(&g_gls);

    glPopMatrix(&g_gls);
}


// ============================================================
// the same gear, through a vertex buffer
// ============================================================
//
// gear() above submits every face's corners individually, so a corner shared
// by three faces is transformed three times. Here the corners go into a
// buffer once and the faces become index lists, which is what glDrawElements
// wants and what an OBJ file already is.
//
// Two details make this a transcription rather than a rewrite, which matters
// because the test asserts the two draw the SAME PICTURE:
//
//   The normal changes in the MIDDLE of the outward-face strip -- once per
//   quad. A draw call has one current normal, so each of those quads becomes
//   its own GL_QUAD_STRIP call of four indices: the previous pair, then the
//   new pair. A four-vertex quad strip emits (v0,v1,v3,v2), which is exactly
//   the quad the original emitted at that point. Real GL forces the same
//   split for the same reason.
//
//   Vertices are deduplicated on EXACT position. Gears carry no texture
//   coordinates, so position is the whole vertex; if that ever stops being
//   true this has to compare s and t as well or two corners that differ only
//   in texture coordinate will be merged.

#define GRP_MAX     512
#define GRP_MAXIDX  4096

long g_grp_mode[GRP_MAX];
long g_grp_nx[GRP_MAX];
long g_grp_ny[GRP_MAX];
long g_grp_nz[GRP_MAX];
long g_grp_start[GRP_MAX];
long g_grp_count[GRP_MAX];
long g_grp_n;

long g_gidx[GRP_MAXIDX];
long g_gidx_n;

// Which groups belong to which gear.
long g_gear_grp0[3];
long g_gear_grpn[3];
long g_gear_buf[3];

// The buffer being built, and a shadow copy of its positions so a vertex can
// be looked up before it is added. glBufferVertex does not deduplicate -- a
// buffer is storage, and deciding what counts as "the same vertex" belongs to
// whoever is loading the mesh.
long g_vb_x[GL_BUFCAP];
long g_vb_y[GL_BUFCAP];
long g_vb_z[GL_BUFCAP];
long g_vb_n;
long g_vb_buf;

long g_vb_dupes;               // how much sharing the dedup actually found

void vb_start(long buf) {
    g_vb_buf = buf;
    g_vb_n = 0;
}

long vb_add(long x, long y, long z) {
    long i;
    i = 0;
    while (i < g_vb_n) {
        if (g_vb_x[i] == x && g_vb_y[i] == y && g_vb_z[i] == z) {
            g_vb_dupes = g_vb_dupes + 1;
            return i;
        }
        i = i + 1;
    }
    g_vb_x[g_vb_n] = x; g_vb_y[g_vb_n] = y; g_vb_z[g_vb_n] = z;
    g_vb_n = g_vb_n + 1;
    return glBufferVertex(&g_gls, g_vb_buf, x, y, z);
}

// Open a group. Every group carries the normal that was current when it
// opened, because that is what the original had current when it emitted these
// quads.
void grp_open(long mode, long nx, long ny, long nz) {
    if (g_grp_n >= GRP_MAX) { fail("group table full"); return; }
    g_grp_mode[g_grp_n] = mode;
    g_grp_nx[g_grp_n] = nx; g_grp_ny[g_grp_n] = ny; g_grp_nz[g_grp_n] = nz;
    g_grp_start[g_grp_n] = g_gidx_n;
    g_grp_count[g_grp_n] = 0;
    g_grp_n = g_grp_n + 1;
}

void grp_idx(long v) {
    if (g_gidx_n >= GRP_MAXIDX) { fail("index list full"); return; }
    g_gidx[g_gidx_n] = v;
    g_gidx_n = g_gidx_n + 1;
    g_grp_count[g_grp_n - 1] = g_grp_count[g_grp_n - 1] + 1;
}

// One quad of a strip whose normal has just changed: the previous pair, then
// the new pair, as a four-vertex strip.
// The normal goes in a separate call because nano_cc stops at six arguments
// and position-pair plus normal is seven.
long g_grp_cnx; long g_grp_cny; long g_grp_cnz;

void grp_setn(long nx, long ny, long nz) {
    g_grp_cnx = nx; g_grp_cny = ny; g_grp_cnz = nz;
}

void grp_quad_strip(long p0, long p1, long n0, long n1) {
    grp_open(GL_QUAD_STRIP, g_grp_cnx, g_grp_cny, g_grp_cnz);
    grp_idx(p0); grp_idx(p1); grp_idx(n0); grp_idx(n1);
}

void gear_vbo(long which, long buf, long inner_radius, long outer_radius,
              long width, long teeth) {
    long i;
    long r0;
    long r1;
    long r2;
    long angle;
    long da;
    long hw;
    long full;
    long tooth_depth;
    long p0;
    long p1;

    tooth_depth = (GL_ONE * 7) / 10;
    r0 = inner_radius;
    r1 = outer_radius - tooth_depth / 2;
    r2 = outer_radius + tooth_depth / 2;
    full = 360 * GL_ONE;
    da = full / teeth / 4;
    hw = width / 2;

    vb_start(buf);
    g_gear_buf[which] = buf;
    g_gear_grp0[which] = g_grp_n;

    // front face -- one normal for the whole strip, so one draw call
    grp_open(GL_QUAD_STRIP, 0, 0, GL_ONE);
    i = 0;
    while (i <= teeth) {
        angle = i * full / teeth;
        grp_idx(vb_add(fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), hw));
        grp_idx(vb_add(fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), hw));
        grp_idx(vb_add(fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), hw));
        grp_idx(vb_add(fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                       fx_mul(r1, gl_sin_fx(angle + 3 * da)), hw));
        i = i + 1;
    }

    // front sides of the teeth
    grp_open(GL_QUADS, 0, 0, GL_ONE);
    i = 0;
    while (i < teeth) {
        angle = i * full / teeth;
        grp_idx(vb_add(fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), hw));
        grp_idx(vb_add(fx_mul(r2, gl_cos_fx(angle + da)),
                       fx_mul(r2, gl_sin_fx(angle + da)), hw));
        grp_idx(vb_add(fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                       fx_mul(r2, gl_sin_fx(angle + 2 * da)), hw));
        grp_idx(vb_add(fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                       fx_mul(r1, gl_sin_fx(angle + 3 * da)), hw));
        i = i + 1;
    }

    // back face
    grp_open(GL_QUAD_STRIP, 0, 0, 0 - GL_ONE);
    i = 0;
    while (i <= teeth) {
        angle = i * full / teeth;
        grp_idx(vb_add(fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), 0 - hw));
        grp_idx(vb_add(fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), 0 - hw));
        grp_idx(vb_add(fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                       fx_mul(r1, gl_sin_fx(angle + 3 * da)), 0 - hw));
        grp_idx(vb_add(fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), 0 - hw));
        i = i + 1;
    }

    // back sides of the teeth
    grp_open(GL_QUADS, 0, 0, 0 - GL_ONE);
    i = 0;
    while (i < teeth) {
        angle = i * full / teeth;
        grp_idx(vb_add(fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                       fx_mul(r1, gl_sin_fx(angle + 3 * da)), 0 - hw));
        grp_idx(vb_add(fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                       fx_mul(r2, gl_sin_fx(angle + 2 * da)), 0 - hw));
        grp_idx(vb_add(fx_mul(r2, gl_cos_fx(angle + da)),
                       fx_mul(r2, gl_sin_fx(angle + da)), 0 - hw));
        grp_idx(vb_add(fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), 0 - hw));
        i = i + 1;
    }

    // outward faces of the teeth -- the normal changes once per quad, so each
    // quad is its own four-index strip carrying the pair before it.
    i = 0;
    p0 = -1; p1 = -1;
    while (i < teeth) {
        long u;
        long v;
        long len;
        long q0;
        long q1;

        angle = i * full / teeth;

        q0 = vb_add(fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), hw);
        q1 = vb_add(fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), 0 - hw);
        // The first pair of the whole strip opens it; afterwards this pair is
        // the quad closing the PREVIOUS tooth, under the normal left current
        // at the end of the last iteration (cos, sin of the previous angle).
        if (p0 >= 0) {
            grp_setn(gl_cos_fx((i - 1) * full / teeth),
                     gl_sin_fx((i - 1) * full / teeth), 0);
            grp_quad_strip(p0, p1, q0, q1);
        }
        p0 = q0; p1 = q1;

        u = fx_mul(r2, gl_cos_fx(angle + da)) - fx_mul(r1, gl_cos_fx(angle));
        v = fx_mul(r2, gl_sin_fx(angle + da)) - fx_mul(r1, gl_sin_fx(angle));
        len = fx_sqrt(fx_mul(u, u) + fx_mul(v, v));
        if (len) { u = fx_div(u, len); v = fx_div(v, len); }
        q0 = vb_add(fx_mul(r2, gl_cos_fx(angle + da)),
                    fx_mul(r2, gl_sin_fx(angle + da)), hw);
        q1 = vb_add(fx_mul(r2, gl_cos_fx(angle + da)),
                    fx_mul(r2, gl_sin_fx(angle + da)), 0 - hw);
        grp_setn(v, 0 - u, 0);
        grp_quad_strip(p0, p1, q0, q1);
        p0 = q0; p1 = q1;

        q0 = vb_add(fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                    fx_mul(r2, gl_sin_fx(angle + 2 * da)), hw);
        q1 = vb_add(fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                    fx_mul(r2, gl_sin_fx(angle + 2 * da)), 0 - hw);
        grp_setn(gl_cos_fx(angle), gl_sin_fx(angle), 0);
        grp_quad_strip(p0, p1, q0, q1);
        p0 = q0; p1 = q1;

        u = fx_mul(r1, gl_cos_fx(angle + 3 * da)) - fx_mul(r2, gl_cos_fx(angle + 2 * da));
        v = fx_mul(r1, gl_sin_fx(angle + 3 * da)) - fx_mul(r2, gl_sin_fx(angle + 2 * da));
        len = fx_sqrt(fx_mul(u, u) + fx_mul(v, v));
        if (len) { u = fx_div(u, len); v = fx_div(v, len); }
        q0 = vb_add(fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                    fx_mul(r1, gl_sin_fx(angle + 3 * da)), hw);
        q1 = vb_add(fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                    fx_mul(r1, gl_sin_fx(angle + 3 * da)), 0 - hw);
        grp_setn(v, 0 - u, 0);
        grp_quad_strip(p0, p1, q0, q1);
        p0 = q0; p1 = q1;

        i = i + 1;
    }
    // the closing pair, under the last normal set inside the loop
    {
        long q0;
        long q1;
        q0 = vb_add(fx_mul(r1, gl_cos_fx(0)), fx_mul(r1, gl_sin_fx(0)), hw);
        q1 = vb_add(fx_mul(r1, gl_cos_fx(0)), fx_mul(r1, gl_sin_fx(0)), 0 - hw);
        grp_setn(gl_cos_fx((teeth - 1) * full / teeth),
                 gl_sin_fx((teeth - 1) * full / teeth), 0);
        grp_quad_strip(p0, p1, q0, q1);
    }

    // inside radius cylinder -- normal per angle step, so again one quad per
    // draw call
    p0 = -1; p1 = -1;
    i = 0;
    while (i <= teeth) {
        long q0;
        long q1;
        angle = i * full / teeth;
        q0 = vb_add(fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), 0 - hw);
        q1 = vb_add(fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), hw);
        if (p0 >= 0) {
            grp_setn(0 - gl_cos_fx(angle), 0 - gl_sin_fx(angle), 0);
            grp_quad_strip(p0, p1, q0, q1);
        }
        p0 = q0; p1 = q1;
        i = i + 1;
    }

    g_gear_grpn[which] = g_grp_n - g_gear_grp0[which];
}

void draw_gear_vbo(long which) {
    long g;
    long end;
    g = g_gear_grp0[which];
    end = g + g_gear_grpn[which];
    while (g < end) {
        glNormal3x(&g_gls, g_grp_nx[g], g_grp_ny[g], g_grp_nz[g]);
        glDrawElements(&g_gls, g_gear_buf[which], &g_gidx[g_grp_start[g]],
                       g_grp_count[g], g_grp_mode[g]);
        g = g + 1;
    }
}

long g_vbuf1;
long g_vbuf2;
long g_vbuf3;

void build_gears_vbo() {
    g_grp_n = 0;
    g_gidx_n = 0;
    g_vb_dupes = 0;
    g_vbuf1 = glGenBuffer(&g_gls);
    gear_vbo(0, g_vbuf1, GL_ONE, 4 * GL_ONE, GL_ONE, 20);
    g_vbuf2 = glGenBuffer(&g_gls);
    gear_vbo(1, g_vbuf2, GL_ONE / 2, 2 * GL_ONE, 2 * GL_ONE, 10);
    g_vbuf3 = glGenBuffer(&g_gls);
    gear_vbo(2, g_vbuf3, (GL_ONE * 13) / 10, 2 * GL_ONE, GL_ONE / 2, 10);
}

// draw_frame, with each glCallList replaced by its buffer.
void draw_frame_vbo(long angle) {
    gl_clear(&g_gl);
    setup_projection();

    glPushMatrix(&g_gls);
    glRotatex(&g_gls, g_view_rotx, GL_ONE, 0, 0);
    glRotatex(&g_gls, g_view_roty, 0, GL_ONE, 0);
    glRotatex(&g_gls, g_view_rotz, 0, 0, GL_ONE);

    glPushMatrix(&g_gls);
    glTranslatex(&g_gls, 0 - 3 * GL_ONE, 0 - 2 * GL_ONE, 0);
    glRotatex(&g_gls, angle, 0, 0, GL_ONE);
    glMaterialx(&g_gls, GL_FRONT, GL_AMBIENT_AND_DIFFUSE,
                (GL_ONE * 8) / 10, GL_ONE / 10, 0);
    draw_gear_vbo(0);
    glPopMatrix(&g_gls);

    glPushMatrix(&g_gls);
    glTranslatex(&g_gls, (31 * GL_ONE) / 10, 0 - 2 * GL_ONE, 0);
    glRotatex(&g_gls, 0 - 2 * angle - 9 * GL_ONE, 0, 0, GL_ONE);
    glMaterialx(&g_gls, GL_FRONT, GL_AMBIENT_AND_DIFFUSE,
                0, (GL_ONE * 8) / 10, (GL_ONE * 2) / 10);
    draw_gear_vbo(1);
    glPopMatrix(&g_gls);

    glPushMatrix(&g_gls);
    glTranslatex(&g_gls, 0 - (31 * GL_ONE) / 10, (42 * GL_ONE) / 10, 0);
    glRotatex(&g_gls, 0 - 2 * angle - 25 * GL_ONE, 0, 0, GL_ONE);
    glMaterialx(&g_gls, GL_FRONT, GL_AMBIENT_AND_DIFFUSE,
                (GL_ONE * 2) / 10, (GL_ONE * 2) / 10, GL_ONE);
    draw_gear_vbo(2);
    glPopMatrix(&g_gls);

    glPopMatrix(&g_gls);
}

void build_3d_window() {
    wm_init(rgb(24, 28, 38));
    wmin_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);
    g_win3d = wm_create(60, 40, VPW + WM_BORDER * 2 + 8,
                        VPH + WM_TITLE_H + WM_BORDER + 8, "gears");
    wm_decorate(g_win3d);
    if (!gl_bind(&g_gl, g_win3d, VPX, VPY, VPW, VPH)) fail("gl_bind failed");
    gl_state_init(&g_gls, &g_gl);
    wm_present();
}

// ---------- 1. the lists hold what was recorded ----------

// The command count is arithmetic, not a measurement: a gear with T teeth
// emits 4(T+1) + 4T + 4(T+1) + 4T front and back, 8T + 2 outward, 2(T+1)
// inside, plus 6 glBegin, 6 glEnd, 2 face normals, 4T tooth normals, T + 1
// cylinder normals, and one colour. Writing it out is the point: a count
// taken from the implementation would agree with any bug in it.
long expected_commands(long teeth) {
    long verts;
    long normals;
    verts = 4 * (teeth + 1) + 4 * teeth
          + 4 * (teeth + 1) + 4 * teeth
          + 8 * teeth + 2
          + 2 * (teeth + 1);
    normals = 2 + 4 * teeth + (teeth + 1);
    return verts + normals + 6 + 6 + 1;
}

void test_lists() {
    puts("\n-- 1. a display list holds exactly what was recorded --\n");

    expect("gear 1 commands recorded", glListSize(g_gear1), expected_commands(20));
    expect("gear 2 commands recorded", glListSize(g_gear2), expected_commands(10));
    expect("gear 3 commands recorded", glListSize(g_gear3), expected_commands(10));
    expect("nothing overflowed a list", g_list_over, 0);
    expect_true("the three names are distinct",
                g_gear1 != g_gear2 && g_gear2 != g_gear3 && g_gear1 != g_gear3);
}

// ---------- 2. a list draws what the calls draw ----------

long render_gear1(long use_list) {
    gl_clear(&g_gl);
    setup_projection();
    glPushMatrix(&g_gls);
    glRotatex(&g_gls, 20 * GL_ONE, GL_ONE, 0, 0);
    glRotatex(&g_gls, 30 * GL_ONE, 0, GL_ONE, 0);

    if (use_list) {
        glCallList(&g_gls, g_gear1);
    } else {
        glMaterialx(&g_gls, GL_FRONT, GL_AMBIENT_AND_DIFFUSE,
                    (GL_ONE * 8) / 10, GL_ONE / 10, 0);
        gear(GL_ONE, 4 * GL_ONE, GL_ONE, 20, (GL_ONE * 7) / 10);
    }

    glPopMatrix(&g_gls);
    return win_hash(g_win3d, VPX, VPY, VPW, VPH);
}

void test_list_matches_immediate() {
    long from_list;
    long from_calls;
    long drawn;
    long empty;

    puts("\n-- 2. a recorded gear draws what the calls draw --\n");

    from_list = render_gear1(1);
    drawn = win_drawn(g_win3d, VPX, VPY, VPW, VPH);
    from_calls = render_gear1(0);

    printf("  %d pixels drawn\n", drawn);

    gl_clear(&g_gl);
    empty = win_hash(g_win3d, VPX, VPY, VPW, VPH);

    // The check...
    expect_true("glCallList draws the same pixels as the calls themselves",
                from_list == from_calls);
    // ...and the two lines that stop it being satisfied by a blank viewport.
    expect_true("...and something was actually drawn", drawn > 2000);
    expect_true("...and it is not the empty viewport", from_list != empty);
}

// ---------- 3. culling costs triangles, not pixels ----------

long render_scene_cull(long cull, long depth) {
    g_gl.cull = cull;
    g_gl.depth = depth;
    draw_frame(0);
    return win_hash(g_win3d, VPX, VPY, VPW, VPH);
}

void test_culling() {
    long with_cull;
    long without_cull;
    long culled;
    long submitted;
    long nodepth_cull;
    long nodepth_plain;
    long differ;

    puts("\n-- 3. backface culling changes the cost, not the picture --\n");

    // Which winding is "front" is a fact about the model and this
    // renderer's handedness, and it is measurable rather than arguable:
    // draw with culling off (the true solid), then with each winding, and
    // see which one agrees. Reasoning about it got me the wrong answer once
    // already.
    {
        long d_ccw;
        long d_cw;
        render_scene_cull(0, 1);
        win_snap(g_win3d, VPX, VPY, VPW, VPH);
        g_gl.frontcw = 0;
        render_scene_cull(1, 1);
        d_ccw = win_diff(g_win3d, VPX, VPY, VPW, VPH);
        g_gl.frontcw = 1;
        render_scene_cull(1, 1);
        d_cw = win_diff(g_win3d, VPX, VPY, VPW, VPH);
        printf("  winding check: CCW differs by %d px, CW by %d px\n", d_ccw, d_cw);

        // This is the assertion that pins the handedness. If a change to
        // gluLookAtx or the frustum ever mirrors the world, these two swap
        // over and this goes red -- which is a much earlier warning than
        // noticing that text in the scene reads backwards.
        expect_true("counter-clockwise is the front face for a GL-authored model",
                    d_ccw * 20 < d_cw);
        g_gl.frontcw = 0;
    }

    g_gl.tris_in = 0; g_gl.tris_culled = 0;
    with_cull = render_scene_cull(1, 1);
    culled = g_gl.tris_culled;
    submitted = g_gl.tris_in;
    win_snap(g_win3d, VPX, VPY, VPW, VPH);

    without_cull = render_scene_cull(0, 1);
    differ = win_diff(g_win3d, VPX, VPY, VPW, VPH);

    printf("  %d triangles submitted, %d back-facing\n", submitted, culled);
    printf("  %d of %d pixels differ with culling off\n", differ, VPW * VPH);

    // Not "identical": a pixel on the silhouette can be covered by a
    // back-facing triangle and by no front-facing one, because coverage is
    // decided per pixel centre and the two edges do not round the same way.
    // A handful of those is geometry; thousands is a winding error, and the
    // number is printed either way so the distinction is visible.
    expect_true("a closed solid looks all but identical with culling on or off",
                differ * 200 < VPW * VPH);
    expect_true("...and culling did remove something", culled > 0);
    expect_true("...roughly half of a closed solid faces away",
                culled * 3 > submitted && culled * 3 < submitted * 2);

    // The control. If the equality above were being satisfied by an empty
    // viewport, or by culling doing nothing at all, then turning the depth
    // test off would not change anything either -- and it must, because
    // without a depth buffer the back faces overwrite the front ones.
    nodepth_cull  = render_scene_cull(1, 0);
    nodepth_plain = render_scene_cull(0, 0);
    expect_true("...and with no depth buffer it does NOT look the same",
                nodepth_cull != nodepth_plain);

    g_gl.cull = 1;
    g_gl.depth = 1;
    g_gl.frontcw = 0;
}

// ---------- 4. the teeth are there ----------

// A gear without teeth is a disc, and a disc is a perfectly plausible
// picture. So: render gear 1 alone, face on, and count lit pixels in the ring
// between the root radius and the tip radius. Twenty teeth must put something
// there, and the gaps between them must leave something empty.
void test_teeth() {
    long r1px;
    long r2px;
    long inside;
    long between;
    long outside;
    long j;

    puts("\n-- 4. the teeth are actually there --\n");

    gl_clear(&g_gl);
    setup_projection();
    glPushMatrix(&g_gls);
    glCallList(&g_gls, g_gear1);
    glPopMatrix(&g_gls);

    // The gear sits at the origin, 40 units away, projected by a frustum with
    // near 5 and half-width 1 over VPW/2 pixels. A radius r maps to
    // r * (near/40) * (VPW/2) pixels.
    // ndc = (near / z) * R / r_frustum, with r_frustum = 1, and the screen
    // scale is VPW/2. The near plane is the factor I left out first time
    // round, which put the root radius at 13 pixels instead of 68 and made
    // the whole gear read as "beyond the tips".
    //
    // The tip radius is taken at the NEAR face of the gear, z = 40 - width/2,
    // because that is the largest the silhouette gets.
    r1px = fx_to_int(fx_mul(fx_div(fx_mul(g_r1, 5 * GL_ONE), 40 * GL_ONE),
                            fx_from_int(VPW / 2)));
    r2px = fx_to_int(fx_mul(fx_div(fx_mul(g_r2, 5 * GL_ONE), 40 * GL_ONE - GL_ONE / 2),
                            fx_from_int(VPW / 2))) + 2;
    printf("  root radius %d px, tip radius %d px\n", r1px, r2px);

    inside = 0;
    between = 0;
    outside = 0;
    j = 0;
    while (j < VPH) {
        long i;
        i = 0;
        while (i < VPW) {
            long dx;
            long dy;
            long d2;
            long lit;
            dx = i - VPW / 2;
            dy = j - VPH / 2;
            d2 = dx * dx + dy * dy;
            lit = g_win[g_win3d].pix[(VPY + j) * g_win[g_win3d].w + VPX + i] != g_gl.bg;
            if (lit) {
                if (d2 < r1px * r1px) inside = inside + 1;
                else if (d2 <= r2px * r2px) between = between + 1;
                else outside = outside + 1;
            }
            i = i + 1;
        }
        j = j + 1;
    }

    printf("  lit pixels: %d inside the root, %d in the tooth ring, %d beyond the tips\n",
           inside, between, outside);

    expect_true("the body of the gear is drawn", inside > 1000);
    expect_true("the teeth reach past the root radius", between > 200);
    expect_true("nothing is drawn beyond the tip radius", outside == 0);
}

// ---------- 5. the light is doing something ----------

void test_lighting() {
    long lit;
    long flat;

    puts("\n-- 5. flat shading gives a face its own colour --\n");

    g_gl.lighting = 1;
    draw_frame(0);
    lit = win_shades(g_win3d, VPX, VPY, VPW, VPH);

    g_gl.lighting = 0;
    draw_frame(0);
    flat = win_shades(g_win3d, VPX, VPY, VPW, VPH);

    printf("  %d distinct colours lit, %d unlit\n", lit, flat);

    // Unlit: the background plus the three material colours. Lit: every face
    // orientation gets its own shade of those three.
    expect_true("unlit, there are only the material colours", flat <= 6);
    expect_true("lit, each face orientation has its own shade", lit > 20);

    g_gl.lighting = 1;
}

// ---------- 6. it turns ----------

void test_animation() {
    long hashes[24];
    long n;
    long i;
    long distinct;
    long t0;
    long t1;

    puts("\n-- 6. thirty degrees of rotation is thirty distinct frames --\n");

    n = 0;
    t0 = g_ticks;
    i = 0;
    while (i < 24) {
        draw_frame(i * 2 * GL_ONE);
        hashes[n] = win_hash(g_win3d, VPX, VPY, VPW, VPH);
        n = n + 1;
        i = i + 1;
    }
    t1 = g_ticks;

    distinct = 0;
    i = 0;
    while (i < n) {
        long k;
        long seen;
        seen = 0;
        k = 0;
        while (k < i) {
            if (hashes[k] == hashes[i]) seen = 1;
            k = k + 1;
        }
        if (!seen) distinct = distinct + 1;
        i = i + 1;
    }

    printf("  %d frames, %d distinct, %d ticks\n", n, distinct, t1 - t0);
    printf("  %d triangles per frame\n", g_gl.tris_in);

    // One number over all 24 frames, printed so that a change to the
    // rasteriser can be checked against the build before it rather than
    // against a description of what it should do.
    //
    // The incremental edge functions are supposed to be the same arithmetic,
    // not an approximation of it. "The pixel counts still match" is much
    // weaker than it sounds -- a shifted picture has the same count -- so this
    // hashes every pixel of every frame and the two builds have to agree
    // digit for digit.
    {
        long sig;
        sig = 5381;
        i = 0;
        while (i < n) {
            sig = ((sig * 33) + hashes[i]) & 0xFFFFFFFF;
            i = i + 1;
        }
        printf("  frame signature over all %d frames: %d\n", n, sig);
    }

    expect("every frame differs from every other", distinct, n);
}

// ---------- 7. what a list saves ----------
//
// Printed, not asserted. Which is faster is a fact about this machine on this
// day; that the display list is not SLOWER is the only thing worth stating,
// and even that is a measurement rather than a property.

void test_cost() {
    long t0;
    long t1;
    long list_ticks;
    long call_ticks;
    long i;

    puts("\n-- 7. what the display list buys --\n");

    t0 = g_ticks;
    i = 0;
    while (i < 8) { render_gear1(1); i = i + 1; }
    t1 = g_ticks;
    list_ticks = t1 - t0;

    t0 = g_ticks;
    i = 0;
    while (i < 8) { render_gear1(0); i = i + 1; }
    t1 = g_ticks;
    call_ticks = t1 - t0;

    printf("  8 frames from the list: %d ticks\n", list_ticks);
    printf("  8 frames rebuilt each time: %d ticks\n", call_ticks);
    printf("  (the difference is the trigonometry for %d vertices a frame)\n",
           expected_commands(20));
}

void run_tests() {
    printf("FB: %dx%d at %d bpp\n", fb_width, fb_height, fb_bpp);
    printf("viewport %dx%d\n", VPW, VPH);

    build_3d_window();

    g_view_rotx = 20 * GL_ONE;
    g_view_roty = 30 * GL_ONE;
    g_view_rotz = 0;

    // gears.c's own light: {5, 5, 10, 0}, a direction because w is zero.
    glLightDirx(&g_gls, 5 * GL_ONE, 5 * GL_ONE, 10 * GL_ONE);
    // gears.c was written against standard GL, which looks along -z, and I
    // assumed that meant its triangles arrive wound the other way here. They
    // do not: gluLookAtx and gl_frustum already build the basis so the
    // handedness works out, so a model authored for GL keeps its winding.
    // Measured, not reasoned -- see the winding check in test 3, which put
    // CCW at 146 pixels of difference and CW at 10,980.
    glFrontFace(&g_gls, GL_CCW);
    glEnable(&g_gls, GL_CULL_FACE);
    glEnable(&g_gls, GL_LIGHTING);
    glEnable(&g_gls, GL_LIGHT0);
    glEnable(&g_gls, GL_DEPTH_TEST);
    glEnable(&g_gls, GL_NORMALIZE);

    build_gears();

    test_lists();
    test_list_matches_immediate();
    test_culling();
    test_teeth();
    test_lighting();
    test_animation();
    test_cost();

    printf("\nheap: %d pages mapped, %d bytes free\n", heap_pages, heap_bytes_free());
    expect("no matrix stack or glBegin overflows", g_gls.overflow, 0);

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else puts("\nPASS: gears, in fixed point, from display lists\n");

    puts("\nGEARSTEST DONE\n");
}

// ============================================================
// where a gears frame's time actually goes
// ============================================================
//
// Reported: "gearsrun was high use too for what it is so its more than widget
// issue". It is. This demo has one window, no HUD and no panel, so none of
// the interface work K24c removed was ever in it -- and it still runs the
// host at about seventy percent of a core.
//
// The three numbers below are what decide where to look next. gltex's frame
// is dominated by the fill; this one may not be, because 892 triangles that
// each cover forty-odd pixels is a very different shape of frame.
// Forty and not ten. The lit-versus-unlit difference is the smallest thing
// this report tries to resolve, and at ten rounds the host's own jitter is
// bigger than it -- one run put lighting at 645 us and the next at MINUS 140,
// which is not a cost, it is noise wearing a number's clothes. A negative
// answer is the only reason this constant is what it is.
#define GEARS_ROUNDS 40

// The box a REAL frame dirtied and the box it damaged, captured once so the
// clear and the flip can each be timed against the work an actual frame gives
// them. See gears_time_us for why they cannot simply be repeated.
long g_bx0; long g_by0; long g_bx1; long g_by1;
long g_dx0; long g_dy0; long g_dx1; long g_dy1;

long gears_time_us(long what) {
    long a;
    long b;
    long r;
    a = pm_timer_read();
    r = 0;
    while (r < GEARS_ROUNDS) {
        if (what == 0) draw_frame(r * 2 * GL_ONE);
        else if (what == 1) {
            // Restore the dirty box before every clear. Repeating a bare
            // gl_clear does NOT measure a clear: the first one empties the
            // box and the other thirty-nine find nothing to do and return at
            // the first line -- which is precisely the optimisation this
            // renderer just gained. Ten rounds reported 26 us and forty
            // reported 7, and both are the same single clear divided by a
            // different number. The 1:4 ratio between them is what gave it
            // away; a real per-call cost does not move with the loop count.
            g_gl.cx0 = g_bx0; g_gl.cy0 = g_by0;
            g_gl.cx1 = g_bx1; g_gl.cy1 = g_by1;
            gl_clear(&g_gl);
        }
        else if (what == 2) {
            // Same trap on the other side: gl_flush clears the damage box and
            // wm_present repaints only damaged rectangles, so an unprimed
            // repeat times an early return.
            g_gl.dx0 = g_dx0; g_gl.dy0 = g_dy0;
            g_gl.dx1 = g_dx1; g_gl.dy1 = g_dy1;
            gl_flush(&g_gl); wm_present();
        }
        else if (what == 4) {
            // Transform and cull only: every triangle goes through m4_apply
            // three times and through the view-space backface test, and then
            // stops. What is left out is projection, clipping, the raster
            // setup and the fill.
            long save;
            save = g_gl.xformonly;
            g_gl.xformonly = 1;
            draw_frame(r * 2 * GL_ONE);
            g_gl.xformonly = save;
        }
        else { draw_frame(r * 2 * GL_ONE); gl_flush(&g_gl); wm_present(); }
        r = r + 1;
    }
    b = pm_timer_read();
    return ((pm_timer_delta(a, b) * 1000000) / PM_TMR_HZ) / GEARS_ROUNDS;
}

// Every interesting number in this report is a DIFFERENCE -- lit minus unlit
// is the lighting, solid minus wireframe is the fill -- and a difference of
// two separately-timed runs inherits the drift of both. Under a TCG qemu on a
// loaded host, draw_frame alone came back at 11012, 11824, 11961 and 13699 us
// on four runs of identical code. Subtract two of those and a 400 us stage
// can read anywhere from -1300 to +2700; that is how "lighting" managed to
// measure MINUS 140 us on one run and 1499 on another.
//
// So the two variants are interleaved INSIDE one loop, timed separately round
// by round. Whatever the host is doing at round 19 is done to both of them,
// and the difference survives it. g_ab is the second total; the return value
// is the first.
long g_ab;

long gears_pair_us(long mode) {
    long r;
    long t0;
    long t1;
    long t2;
    long suma;
    long sumb;
    long save;

    suma = 0; sumb = 0;
    r = 0;
    while (r < GEARS_ROUNDS) {
        long ang;
        ang = r * 2 * GL_ONE;

        // A: the frame as it is actually drawn.
        t0 = pm_timer_read();
        draw_frame(ang);
        t1 = pm_timer_read();

        // B: the same frame with one stage removed -- or, for mode 4, the
        // same frame drawn the other way, which is the only comparison in
        // here where BOTH sides produce a picture and the pictures match.
        if (mode == 4) { }
        else if (mode == 0) { save = g_gl.lighting; g_gl.lighting = 0; }
        else if (mode == 1) { save = g_gl.wire; g_gl.wire = 1; }
        else if (mode == 2) { save = g_gl.xformonly; g_gl.xformonly = 1; }
        else if (mode == 3) { save = g_gl.twodiv; g_gl.twodiv = 1; }
        if (mode == 4) draw_frame_vbo(ang);
        else draw_frame(ang);
        if (mode == 0) g_gl.lighting = save;
        else if (mode == 1) g_gl.wire = save;
        else if (mode == 2) g_gl.xformonly = save;
        else if (mode == 3) g_gl.twodiv = save;
        t2 = pm_timer_read();

        suma = suma + pm_timer_delta(t0, t1);
        sumb = sumb + pm_timer_delta(t1, t2);
        r = r + 1;
    }
    g_ab = ((sumb * 1000000) / PM_TMR_HZ) / GEARS_ROUNDS;
    return ((suma * 1000000) / PM_TMR_HZ) / GEARS_ROUNDS;
}

// How many DISTINCT model-space positions a display list holds, against how
// many GLC_VERTEX commands it replays. That ratio is the hard ceiling on what
// caching a transformed vertex can save: a vertex submitted once cannot be
// transformed fewer than once, no matter how the cache is built.
//
// O(n^2) and it runs once at start-up, which is the right trade for a number
// that decides whether a milestone is worth doing.
long g_vx[GL_LISTCAP];
long g_vy[GL_LISTCAP];
long g_vz[GL_LISTCAP];

long list_distinct_verts(long list, long *submitted) {
    long i;
    long base;
    long n;
    long distinct;
    long subs;

    base = (list - 1) * GL_LISTCAP;
    n = glListSize(list);
    distinct = 0;
    subs = 0;
    i = 0;
    while (i < n) {
        if (g_list_op[base + i] == GLC_VERTEX) {
            long k;
            long seen;
            subs = subs + 1;
            seen = 0;
            k = 0;
            while (k < distinct) {
                if (g_vx[k] == g_list_a[base + i] &&
                    g_vy[k] == g_list_b[base + i] &&
                    g_vz[k] == g_list_c[base + i]) { seen = 1; k = distinct; }
                k = k + 1;
            }
            if (!seen) {
                g_vx[distinct] = g_list_a[base + i];
                g_vy[distinct] = g_list_b[base + i];
                g_vz[distinct] = g_list_c[base + i];
                distinct = distinct + 1;
            }
        }
        i = i + 1;
    }
    *submitted = subs;
    return distinct;
}

void frame_cost_report() {
    long frame;
    long clear;
    long flip;
    long whole;
    long unlit;
    long wire;
    long px;
    long scene;
    long xform;
    long xf;
    long lit_d;
    long fill_d;
    long rest_d;
    long fa0;
    long fa1;
    long fa2;
    long dv;
    long dsub;
    long fa3;
    long div_d;
    long fa4;
    long vbo_t;

    draw_frame(0);
    gl_flush(&g_gl);
    wm_present();

    px = g_gl.pixels;
    g_gls.verts = 0;
    draw_frame(0);
    px = g_gl.pixels - px;
    // The transform count that matters is the glapi one, not gl_tri's. A quad
    // strip already carries transformed vertices forward in vbuf, so a vertex
    // shared by two triangles of the same strip is transformed ONCE -- the
    // three-per-triangle figure only applies to the low-level gl_tri entry
    // point, which gears does not use. What is left to save is the vertices
    // that appear in more than one primitive.
    xf = g_gls.verts;

    // draw_frame clears and then draws, so both boxes are live right here:
    // the dirt this frame left for the NEXT clear to undo, and the damage it
    // left for the next present to blit. Capture them, because the timing
    // loops below consume them and would otherwise be timing an early return.
    g_bx0 = g_gl.cx0; g_by0 = g_gl.cy0; g_bx1 = g_gl.cx1; g_by1 = g_gl.cy1;
    g_dx0 = g_gl.dx0; g_dy0 = g_gl.dy0; g_dx1 = g_gl.dx1; g_dy1 = g_gl.dy1;

    clear = gears_time_us(1);
    flip  = gears_time_us(2);
    whole = gears_time_us(3);

    // Each of these three runs both variants round by round, so `frame` comes
    // back three times over from three separate paired runs. Keeping the last
    // one and reporting each difference against the `frame` measured IN THE
    // SAME loop is the point -- pairing across loops would put the drift back.
    fa0 = gears_pair_us(0); unlit = g_ab;   lit_d  = fa0 - unlit;
    fa1 = gears_pair_us(1); wire  = g_ab;   fill_d = fa1 - wire;
    fa2 = gears_pair_us(2); xform = g_ab;   rest_d = fa2 - xform;
    // Mode 3 draws the SAME picture twice, once with an extra divide per
    // covered fragment. B minus A is therefore one divide per fragment and
    // nothing else -- the cleanest way to price the thing without guessing.
    fa3 = gears_pair_us(3); div_d = g_ab - fa3;
    // Display lists against vertex buffers, same picture both sides.
    fa4 = gears_pair_us(4); vbo_t = g_ab;
    frame = fa2;

    // The same frame with the lighting turned off. Not a proposal to turn it
    // off -- it is what makes a gear look like a gear -- but a measurement of
    // what per-triangle work costs, because gl_shade normalises the face
    // normal and v3_norm runs a bit-at-a-time integer square root. 892
    // triangles a frame is 892 square roots a frame, on geometry that only
    // rotates.
    {
        long save;
        save = g_gl.lighting;
        g_gl.lighting = 0;
        unlit = gears_time_us(0);
        g_gl.lighting = save;

        // And in wireframe, which submits exactly the same geometry through
        // exactly the same transform, projection, clipping and backface test
        // and then draws outlines instead of filling. The difference between
        // the two is the FILL; what is left is everything that happens per
        // triangle before a pixel is written.
        save = g_gl.wire;
        g_gl.wire = 1;
        wire = gears_time_us(0);
        g_gl.wire = save;
        draw_frame(0);
    }

    puts("\n-- what one gears frame costs --\n");
    printf("  %d triangles submitted, %d drawn\n", g_gl.tris_in, g_gl.tris_drawn);
    printf("  %d pixels written, of which the clear wrote %d\n", px, g_gl.clearpix);
    printf("  the rasteriser walked %d iterations to cover %d\n",
           g_gl.steps, g_gl.covered);
    // The two numbers that decide whether a hierarchical depth buffer is worth
    // building. A fragment that is covered but never written was rejected by
    // the z test -- it is OVERDRAW, and it is the only thing a coarse z pass
    // can save. If that count is small, no amount of hierarchy helps, and the
    // answer is a measurement rather than an opinion.
    scene = px - g_gl.clearpix;
    if (g_gl.covered > 0)
        printf("  %d of those covered fragments lost the depth test: %d%% overdraw\n",
               g_gl.covered - scene, (g_gl.covered - scene) * 100 / g_gl.covered);
    printf("  gl_clear      %d us\n", clear);
    printf("  draw_frame    %d us\n", frame);
    printf("  flush + flip  %d us\n", flip);
    printf("  a whole frame %d us\n", whole);
    if (g_gl.tris_drawn > 0)
        printf("  which is %d us per drawn triangle, covering %d pixels each\n",
               whole / g_gl.tris_drawn, g_gl.covered / g_gl.tris_drawn);
    // Each stage against the frame it was interleaved with, not against a
    // frame timed in a different loop. See gears_pair_us.
    printf("  lighting        %d us  (frame %d, unlit %d)\n", lit_d, fa0, unlit);
    printf("  the fill        %d us  (frame %d, wireframe %d)\n", fill_d, fa1, wire);
    printf("  transform+cull  %d us  over %d vertex transforms submitted\n",
           xform, xf);
    printf("  everything else %d us  (project, clip, raster setup, lines)\n",
           rest_d - fill_d);
    printf("  one divide per fragment: %d us over %d fragments\n",
           div_d, g_gl.covered);
    printf("  display lists %d us vs vertex buffers %d us, saving %d us (%d%%)\n",
           fa4, vbo_t, fa4 - vbo_t,
           fa4 > 0 ? ((fa4 - vbo_t) * 100) / fa4 : 0);
    dv = list_distinct_verts(g_gear1, &dsub);
    printf("  gear 1 list: %d vertex commands, %d distinct positions\n", dsub, dv);
}


// ============================================================
// 8. the same gears through vertex buffers
// ============================================================
void test_vbo_gears() {
    long hash_list;
    long hash_vbo;
    long verts_list;
    long verts_vbo;
    long tris_list;
    long tris_vbo;
    long drawn;

    puts("\n-- 8. the same gears, through vertex buffers --\n");

    build_gears_vbo();
    printf("  %d groups, %d indices, %d duplicate corners found\n",
           g_grp_n, g_gidx_n, g_vb_dupes);
    printf("  gear 1 buffer holds %d vertices\n", glBufferSize(g_vbuf1));

    // The old way.
    g_gls.verts = 0; g_gls.tris = 0;
    draw_frame(0);
    verts_list = g_gls.verts;
    tris_list = g_gls.tris;
    hash_list = win_hash(g_win3d, VPX, VPY, VPW, VPH);
    drawn = win_drawn(g_win3d, VPX, VPY, VPW, VPH);

    // The new way.
    g_gls.verts = 0; g_gls.tris = 0;
    draw_frame_vbo(0);
    verts_vbo = g_gls.verts;
    tris_vbo = g_gls.tris;
    hash_vbo = win_hash(g_win3d, VPX, VPY, VPW, VPH);

    expect_true("the gears are actually on screen", drawn > 5000);
    expect_true("vertex buffers draw a BIT-IDENTICAL frame", hash_vbo == hash_list);
    expect("...from the same number of triangles", tris_vbo, tris_list);
    printf("  %d vertex transforms through display lists, %d through buffers\n",
           verts_list, verts_vbo);
    expect_true("...having transformed fewer vertices", verts_vbo < verts_list);

    // Rotated, because a frame that matches in one pose can still be wrong in
    // another -- the cache is keyed on the matrix and pose 0 is the one every
    // buffer was first transformed under.
    g_gls.verts = 0;
    draw_frame(37 * GL_ONE);
    hash_list = win_hash(g_win3d, VPX, VPY, VPW, VPH);
    verts_list = g_gls.verts;
    g_gls.verts = 0;
    draw_frame_vbo(37 * GL_ONE);
    hash_vbo = win_hash(g_win3d, VPX, VPY, VPW, VPH);
    verts_vbo = g_gls.verts;
    expect_true("...and still identical once the gears have turned",
                hash_vbo == hash_list);
    expect_true("...still transforming fewer", verts_vbo < verts_list);

    // Each gear is transformed once per frame however many draw calls it
    // takes. Three gears, so three transforms of three buffers.
    g_buf_hit = 0; g_buf_miss = 0;
    draw_frame_vbo(51 * GL_ONE);
    expect("a frame transforms each of the three buffers exactly once",
           g_buf_miss, 3);
    expect_true("...and every other draw call reuses the cache",
                g_buf_hit == g_grp_n - 3);
}

// After the tests, leave it running: the gears turn until the machine is
// switched off, which is what the program is for.
void spin_forever() {
    long angle;
    angle = 0;
    for (;;) {
        draw_frame(angle);
        gl_flush(&g_gl);
        wm_present();
        angle = angle + 2 * GL_ONE;
        if (angle >= 360 * GL_ONE) angle = angle - 360 * GL_ONE;
        sleep_ms(16);
    }
}

int main() {
    serial_init();
    g_fail = 0;

    puts("\nnano-os: gears\n");

    if (!fb_init(1024, 768)) { puts("fb_init failed\n"); for (;;) { } }
    if (!mm_init())          { puts("mm_init failed\n"); for (;;) { } }

    // acpi_init BEFORE mm_protect_null: the pointer to the EBDA, where the
    // RSDP is looked for, lives at physical 0x40E -- inside the page
    // mm_protect_null unmaps.
    if (!acpi_init()) puts("acpi_init found nothing -- the frame report cannot time\n");
    mm_protect_null();

    kbd_init();
    interrupts_init(100);

    run_tests();

    test_vbo_gears();
    if (acpi_pm_tmr) frame_cost_report();
    puts("gears running; the machine is now interactive\n");
    spin_forever();
    return 0;
}
