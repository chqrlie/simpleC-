// glapi.c — the OpenGL-shaped API, a camera with a real view frustum, and a
// 3D viewport that is a widget.
//
// K15 proved a triangle could be rasterised in 16.16 into a window's backing
// buffer. This is the layer above it: glBegin/glEnd with strips, fans, quads
// and quad strips; a matrix stack with glPushMatrix, glRotatex and
// gluLookAtx; six clipping planes extracted from the live projection *
// modelview matrix the way Mark Morley's article describes; and ui_glview,
// which claims a rectangle from the widget layout and hands its interior to
// the renderer.
//
// The three claims this file exists to check, in the order they matter:
//
//   1. A strip, a fan, a quad and a quad strip put EXACTLY the same pixels on
//      screen as the equivalent list of triangles. Not "the same number of
//      triangles" -- the same pixels. Winding is the thing that goes wrong in
//      primitive assembly and a count cannot see it.
//
//   2. The frustum test agrees with the rasteriser. Every plane comes out of
//      the same matrix the vertices go through, so the check is not "is this
//      the number I expected" but "do two independent parts of the renderer
//      say the same thing about the same point".
//
//   3. Frustum culling never changes the picture. Same scene, culling on and
//      off, identical framebuffer -- and far fewer triangles submitted. A
//      culling optimisation that alters the image is not an optimisation.
//
// nano-kernel.h first, as in every graphics image: it only mirrors console
// output onto the framebuffer if NANO_FB_H is already defined, and here it
// must not, because this image reads the framebuffer back and hashes it.
#include "nano-kernel.h"
#include "nano-fb.h"
#include "nano-mouse.h"
#include "nano-int.h"
#include "nano-mm.h"
#include "nano-wm.h"
#include "nano-wmin.h"
#include "nano-term.h"
#include "nano-ui.h"
#include "nano-gl.h"
#include "nano-glapi.h"

long g_fail;

void fail(char *msg) {
    printf("FAIL: %s\n", msg);
    g_fail = g_fail + 1;
}

void expect(char *what, long got, long want) {
    if (got == want) printf("  ok  %s = %d\n", what, got);
    else {
        printf("  got %d, wanted %d\n", got, want);
        fail(what);
    }
}

void expect_near(char *what, long got, long want, long tol) {
    long d;
    d = got - want;
    if (d < 0) d = 0 - d;
    if (d <= tol) printf("  ok  %s = %d (wanted %d, within %d)\n", what, got, want, tol);
    else {
        printf("  got %d, wanted %d, off by %d (tolerance %d)\n", got, want, d, tol);
        fail(what);
    }
}

void expect_true(char *what, long got) {
    if (got) printf("  ok  %s\n", what);
    else fail(what);
}

// A hash of a rectangle of a window's BACKING BUFFER, not of the screen. The
// pixel-equality tests below have to compare what was rendered, and the screen
// only shows what the compositor has been told about.
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

// ============================================================
// the scene
// ============================================================

#define VPX  (WM_BORDER + 4)
#define VPY  (WM_TITLE_H + 4)
#define VPW  300
#define VPH  220

struct GLCtx g_gl;
long g_win3d;

// The cube as six QUADS, which is how it would be written in OpenGL and which
// exercises quad assembly rather than going round it. The winding is the one
// K15 derived by computing all twelve face normals -- each quad (a,b,c,d)
// decomposes to (a,b,c) and (a,c,d), and both must point out of the cube.
long g_quad[24];

void build_cube_quads() {
    long k;
    k = 0;
    g_quad[k]=0; k=k+1; g_quad[k]=3; k=k+1; g_quad[k]=2; k=k+1; g_quad[k]=1; k=k+1; // back
    g_quad[k]=4; k=k+1; g_quad[k]=5; k=k+1; g_quad[k]=6; k=k+1; g_quad[k]=7; k=k+1; // front
    g_quad[k]=0; k=k+1; g_quad[k]=4; k=k+1; g_quad[k]=7; k=k+1; g_quad[k]=3; k=k+1; // left
    g_quad[k]=1; k=k+1; g_quad[k]=2; k=k+1; g_quad[k]=6; k=k+1; g_quad[k]=5; k=k+1; // right
    g_quad[k]=0; k=k+1; g_quad[k]=1; k=k+1; g_quad[k]=5; k=k+1; g_quad[k]=4; k=k+1; // bottom
    g_quad[k]=3; k=k+1; g_quad[k]=7; k=k+1; g_quad[k]=6; k=k+1; g_quad[k]=2; k=k+1; // top
}

// The eight corners, in the numbering the face table above assumes. Written
// out rather than generated from the bit pattern of the index: the bit pattern
// numbers them in a different order and the two have to agree exactly, so the
// short clever version would need four corrections anyway.
long g_cx[8];
long g_cy[8];
long g_cz[8];

void corner(long i, long x, long y, long z) {
    g_cx[i] = x * GL_ONE; g_cy[i] = y * GL_ONE; g_cz[i] = z * GL_ONE;
}

void build_cube_verts() {
    corner(0, -1, -1, -1);  corner(1,  1, -1, -1);
    corner(2,  1,  1, -1);  corner(3, -1,  1, -1);
    corner(4, -1, -1,  1);  corner(5,  1, -1,  1);
    corner(6,  1,  1,  1);  corner(7, -1,  1,  1);
}

long g_face_r[6];
long g_face_g[6];
long g_face_b[6];

void face(long i, long r, long g, long b) {
    g_face_r[i] = r; g_face_g[i] = g; g_face_b[i] = b;
}

void gl_cube(struct GlState *st) {
    long f;
    f = 0;
    while (f < 6) {
        long v;
        glColor3ub(st, g_face_r[f], g_face_g[f], g_face_b[f]);
        glBegin(st, GL_QUADS);
        v = 0;
        while (v < 4) {
            long i;
            i = g_quad[f * 4 + v];
            glVertex3x(st, g_cx[i], g_cy[i], g_cz[i]);
            v = v + 1;
        }
        glEnd(st);
        f = f + 1;
    }
}

// A four-sided pyramid as a TRIANGLE_FAN: apex first, then the base ring.
void gl_pyramid(struct GlState *st) {
    glBegin(st, GL_TRIANGLE_FAN);
    glVertex3x(st, 0, GL_ONE + GL_ONE / 2, 0);
    glVertex3x(st, 0 - GL_ONE, 0 - GL_ONE, 0 - GL_ONE);
    glVertex3x(st, GL_ONE,     0 - GL_ONE, 0 - GL_ONE);
    glVertex3x(st, GL_ONE,     0 - GL_ONE, GL_ONE);
    glVertex3x(st, 0 - GL_ONE, 0 - GL_ONE, GL_ONE);
    glVertex3x(st, 0 - GL_ONE, 0 - GL_ONE, 0 - GL_ONE);
    glEnd(st);
}

// A ground strip as a QUAD_STRIP, the primitive it exists for.
void gl_ground(struct GlState *st, long half, long z0, long z1) {
    long x;
    glBegin(st, GL_QUAD_STRIP);
    x = 0 - half;
    while (x <= half) {
        glVertex3x(st, x * GL_ONE, 0 - GL_ONE, z0 * GL_ONE);
        glVertex3x(st, x * GL_ONE, 0 - GL_ONE, z1 * GL_ONE);
        x = x + 2;
    }
    glEnd(st);
}

void build_3d_window() {
    wm_init(rgb(24, 28, 38));
    wmin_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);
    g_win3d = wm_create(80, 60, VPW + WM_BORDER * 2 + 8,
                        VPH + WM_TITLE_H + WM_BORDER + 8, "scene");
    wm_decorate(g_win3d);
    if (!gl_bind(&g_gl, g_win3d, VPX, VPY, VPW, VPH)) fail("gl_bind failed");
    gl_state_init(&g_gls, &g_gl);
    wm_present();
}

// ============================================================
// 1. the arithmetic the API adds
// ============================================================
void test_api_math() {
    struct M4 a;
    struct V3 v;
    struct V3 o1;
    struct V3 o2;
    long i;

    puts("-- 1. angles, arbitrary axes and the stack --\n");

    // The interpolated sine has to agree with the table at whole degrees and
    // sit between neighbours in between. Rounding to whole degrees instead is
    // visible as a stutter, which is why this exists at all.
    expect("sin_fx(30 deg) matches the table", gl_sin_fx(30 << GL_FRAC), gl_sin(30));
    expect("sin_fx(90 deg) is exactly 1.0", gl_sin_fx(90 << GL_FRAC), GL_ONE);
    expect_true("sin_fx(30.5) lies between sin(30) and sin(31)",
                gl_sin_fx((30 << GL_FRAC) + GL_HALF) > gl_sin(30) &&
                gl_sin_fx((30 << GL_FRAC) + GL_HALF) < gl_sin(31));
    expect("cos_fx(0) is exactly 1.0", gl_cos_fx(0), GL_ONE);
    expect("sin_fx of a negative angle", gl_sin_fx(0 - (30 << GL_FRAC)),
           0 - gl_sin(30));

    // glRotatex about the y axis must be m4_rot_y. Two independent routes to
    // the same matrix: one a hand-written special case, the other Rodrigues.
    v.x = GL_ONE; v.y = GL_ONE / 2; v.z = 2 * GL_ONE;
    gl_state_init(&g_gls, &g_gl);
    glRotatex(&g_gls, 37 << GL_FRAC, 0, GL_ONE, 0);
    m4_apply(&o1, &g_gls.mv[0], &v);
    m4_rot_y(&a, 37);
    m4_apply(&o2, &a, &v);
    expect_near("glRotatex about +y equals m4_rot_y, x", o1.x, o2.x, 40);
    expect_near("...and z", o1.z, o2.z, 40);

    // The axis is normalised: (0,7,0) is the same rotation as (0,1,0).
    gl_state_init(&g_gls, &g_gl);
    glRotatex(&g_gls, 37 << GL_FRAC, 0, 7 * GL_ONE, 0);
    m4_apply(&o2, &g_gls.mv[0], &v);
    expect_near("an unnormalised axis is normalised, x", o2.x, o1.x, 8);
    expect_near("...and z", o2.z, o1.z, 8);

    // A rotation about (1,1,1) three times by 120 degrees is the identity --
    // a check of the off-diagonal terms that a single axis cannot make.
    gl_state_init(&g_gls, &g_gl);
    i = 0;
    while (i < 3) {
        glRotatex(&g_gls, 120 << GL_FRAC, GL_ONE, GL_ONE, GL_ONE);
        i = i + 1;
    }
    m4_apply(&o1, &g_gls.mv[0], &v);
    expect_near("three 120-degree turns about (1,1,1) restore x", o1.x, v.x, 200);
    expect_near("...and y", o1.y, v.y, 200);
    expect_near("...and z", o1.z, v.z, 200);

    // GL post-multiplies, so the transform written LAST is applied to the
    // vertex FIRST. Getting this backwards puts every child of a scenegraph
    // node in the wrong place, and looks like a bug in the node, not the API.
    gl_state_init(&g_gls, &g_gl);
    glTranslatex(&g_gls, 10 * GL_ONE, 0, 0);
    glScalex(&g_gls, 2 * GL_ONE, 2 * GL_ONE, 2 * GL_ONE);
    v.x = GL_ONE; v.y = 0; v.z = 0;
    m4_apply(&o1, &g_gls.mv[0], &v);
    expect("translate then scale: the scale happens first", o1.x, 12 * GL_ONE);

    // Push, change, pop, and the matrix is back.
    gl_state_init(&g_gls, &g_gl);
    glTranslatex(&g_gls, GL_ONE, 0, 0);
    glPushMatrix(&g_gls);
    glTranslatex(&g_gls, 5 * GL_ONE, 0, 0);
    m4_apply(&o1, &g_gls.mv[g_gls.mvsp], &v);
    expect("inside a push", o1.x, 7 * GL_ONE);
    glPopMatrix(&g_gls);
    m4_apply(&o1, &g_gls.mv[g_gls.mvsp], &v);
    expect("after the pop", o1.x, 2 * GL_ONE);
    expect("...and nothing was counted as an error", g_gls.overflow, 0);

    // An unbalanced pop is COUNTED, not clamped silently. A scenegraph with
    // one missing push renders almost right, which is the worst kind of wrong.
    glPopMatrix(&g_gls);
    expect("an unbalanced pop is reported", g_gls.overflow, 1);

    i = 0;
    while (i < GL_MV_DEPTH + 2) { glPushMatrix(&g_gls); i = i + 1; }
    expect_true("overrunning the stack is reported too", g_gls.overflow > 1);
    expect("...and the stack pointer stayed in range", g_gls.mvsp, GL_MV_DEPTH - 1);
}

// ============================================================
// 2. primitive assembly -- counts, and then pixels
// ============================================================

// Six vertices in a fixed pattern, so every mode sees the same input.
void feed(struct GlState *st, long mode, long n) {
    long i;
    glBegin(st, mode);
    i = 0;
    while (i < n) {
        glVertex3x(st, (i - 3) * GL_ONE, (i & 1) * GL_ONE, 6 * GL_ONE);
        i = i + 1;
    }
    glEnd(st);
}

long tris_for(long mode, long n) {
    gl_state_init(&g_gls, &g_gl);
    feed(&g_gls, mode, n);
    return g_gls.tris;
}

long lines_for(long mode, long n) {
    gl_state_init(&g_gls, &g_gl);
    feed(&g_gls, mode, n);
    return g_gls.lines;
}

void test_prim_counts() {
    puts("\n-- 2. primitive assembly --\n");

    expect("GL_TRIANGLES, 6 vertices", tris_for(GL_TRIANGLES, 6), 2);
    // GL drops an incomplete primitive rather than inventing a vertex.
    expect("GL_TRIANGLES, 7 vertices: the odd one is dropped",
           tris_for(GL_TRIANGLES, 7), 2);
    expect("GL_TRIANGLE_STRIP, 6 vertices", tris_for(GL_TRIANGLE_STRIP, 6), 4);
    expect("GL_TRIANGLE_FAN, 6 vertices", tris_for(GL_TRIANGLE_FAN, 6), 4);
    expect("GL_POLYGON, 6 vertices", tris_for(GL_POLYGON, 6), 4);
    expect("GL_QUADS, 8 vertices", tris_for(GL_QUADS, 8), 4);
    expect("GL_QUADS, 7 vertices: the partial quad is dropped",
           tris_for(GL_QUADS, 7), 2);
    expect("GL_QUAD_STRIP, 6 vertices", tris_for(GL_QUAD_STRIP, 6), 4);
    expect("GL_TRIANGLE_STRIP, 2 vertices: no triangle yet",
           tris_for(GL_TRIANGLE_STRIP, 2), 0);

    expect("GL_LINES, 6 vertices", lines_for(GL_LINES, 6), 3);
    expect("GL_LINE_STRIP, 6 vertices", lines_for(GL_LINE_STRIP, 6), 5);
    // The loop's closing segment is the one it is named for.
    expect("GL_LINE_LOOP, 6 vertices", lines_for(GL_LINE_LOOP, 6), 6);

    gl_state_init(&g_gls, &g_gl);
    feed(&g_gls, GL_POINTS, 6);
    expect("GL_POINTS, 6 vertices", g_gls.points, 6);

    // glBegin inside glBegin is an error in GL and is counted here.
    gl_state_init(&g_gls, &g_gl);
    glBegin(&g_gls, GL_TRIANGLES);
    glBegin(&g_gls, GL_QUADS);
    expect("nested glBegin is reported", g_gls.overflow, 1);
    glEnd(&g_gls);
}

// Render one shape into a cleared viewport and hash the result.
//
// A note on winding, because I got it backwards writing this. View space here
// is +z forward, so a surface facing the camera has a normal pointing at -z,
// and its vertices go round the other way from the direction they appear to on
// screen. There is no reasoning to be done: take the winding from the cube
// table K15 derived by computing normals, and check the culling counter.
long render_hash_tris(long a) {
    // `a` selects which spelling of the SAME geometry to use.
    long z;
    z = 5 * GL_ONE;
    gl_state_init(&g_gls, &g_gl);
    glColor3ub(&g_gls, 200, 120, 60);
    gl_clear(&g_gl);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    glTranslatex(&g_gls, 0, 0, z);

    if (a == 0) {                       // one quad
        glBegin(&g_gls, GL_QUADS);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glEnd(&g_gls);
    } else if (a == 1) {                // ...as two triangles
        glBegin(&g_gls, GL_TRIANGLES);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
        glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glEnd(&g_gls);
    } else if (a == 2) {                // a strip
        glBegin(&g_gls, GL_TRIANGLE_STRIP);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
        glEnd(&g_gls);
    } else if (a == 3) {                // ...spelled out, WITH the strip's swap
        glBegin(&g_gls, GL_TRIANGLES);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
        glEnd(&g_gls);
    } else if (a == 4) {                // ...and WITHOUT it -- the control
        glBegin(&g_gls, GL_TRIANGLES);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
        glEnd(&g_gls);
    } else if (a == 5) {                // a fan
        glBegin(&g_gls, GL_TRIANGLE_FAN);
        glVertex3x(&g_gls, 0,          0,          0);
        glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
        glEnd(&g_gls);
    } else if (a == 6) {                // ...as triangles
        glBegin(&g_gls, GL_TRIANGLES);
        glVertex3x(&g_gls, 0,          0,          0);
        glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0,          0,          0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
        glEnd(&g_gls);
    } else if (a == 7) {                // a quad strip, three pairs
        glBegin(&g_gls, GL_QUAD_STRIP);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE,     0);
        glVertex3x(&g_gls, 0,          0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0,          GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
        glEnd(&g_gls);
    } else {                            // ...as two quads
        glBegin(&g_gls, GL_QUADS);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE,     0);
        glVertex3x(&g_gls, 0,          GL_ONE,     0);
        glVertex3x(&g_gls, 0,          0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0,          0 - GL_ONE, 0);
        glVertex3x(&g_gls, 0,          GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
        glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
        glEnd(&g_gls);
    }
    return win_hash(g_win3d, VPX, VPY, VPW, VPH);
}

void test_prim_pixels() {
    long quad;
    long tris;
    long strip;
    long strip_as_tris;
    long strip_unswapped;
    long fan;
    long fan_as_tris;
    long qstrip;
    long qstrip_as_quads;

    puts("\n-- 3. the same geometry, spelled four ways --\n");

    quad            = render_hash_tris(0);
    tris            = render_hash_tris(1);
    strip           = render_hash_tris(2);
    strip_as_tris   = render_hash_tris(3);
    strip_unswapped = render_hash_tris(4);
    fan             = render_hash_tris(5);
    fan_as_tris     = render_hash_tris(6);
    qstrip          = render_hash_tris(7);
    qstrip_as_quads = render_hash_tris(8);

    // Counts cannot see a winding error; pixels can. A strip whose alternate
    // triangles are not swapped has the same triangle count and half of them
    // face away from the camera, which culling then removes -- so the failure
    // is "half my strip is missing", and only a pixel comparison catches it.
    expect_true("a quad draws the same pixels as two triangles", quad == tris);
    expect_true("a strip draws the same pixels as its triangles",
                strip == strip_as_tris);
    expect_true("a fan draws the same pixels as its triangles", fan == fan_as_tris);
    expect_true("a quad strip draws the same pixels as two quads",
                qstrip == qstrip_as_quads);

    // The controls, which are the reason the four checks above mean anything.
    //
    // Four equalities between hashes are also satisfied by four empty
    // viewports. So: the same strip wound the other way must NOT match, and
    // none of the four may equal an empty render. Without these two lines a
    // renderer that drew nothing at all would score five out of five.
    expect_true("...and the SAME strip wound wrongly does NOT match",
                strip != strip_unswapped);
    {
        long empty;
        gl_clear(&g_gl);
        empty = win_hash(g_win3d, VPX, VPY, VPW, VPH);
        expect_true("...and none of them is an empty viewport",
                    empty != quad && empty != strip && empty != fan &&
                    empty != qstrip);
    }
}

// ============================================================
// 4. the view frustum
// ============================================================

struct Frustum g_frustum;

void test_frustum_planes() {
    struct V3 v;
    struct M4 clip;

    puts("\n-- 4. six planes out of the matrix --\n");

    gl_state_init(&g_gls, &g_gl);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    gl_clip_matrix(&g_gls, &clip);
    gl_frustum_extract(&g_frustum, &clip);

    // With the modelview at identity the planes are in view space, where the
    // near and far planes are flat slabs at known depths. Their distances are
    // the numbers gl_bind chose, arrived at by a completely different route --
    // through the projection matrix and back out again.
    v.x = 0; v.y = 0; v.z = g_gl.near;
    expect_near("a point exactly on the near plane is 0 away",
                gl_plane_dist(&g_frustum.p[4], &v), 0, 64);
    v.z = g_gl.near + GL_ONE;
    expect_near("...one unit in front of it is 1.0 away",
                gl_plane_dist(&g_frustum.p[4], &v), GL_ONE, 400);
    v.z = g_gl.far;
    expect_near("a point on the far plane is 0 away from it",
                gl_plane_dist(&g_frustum.p[5], &v), 0, 4000);

    v.x = 0; v.y = 0; v.z = 4 * GL_ONE;
    expect_true("a point down the middle is inside",
                gl_frustum_point(&g_frustum, &v) == GL_INSIDE);
    v.z = g_gl.near / 2;
    expect_true("a point inside the near plane is out",
                gl_frustum_point(&g_frustum, &v) == GL_OUTSIDE);
    v.z = g_gl.far + GL_ONE;
    expect_true("a point beyond the far plane is out",
                gl_frustum_point(&g_frustum, &v) == GL_OUTSIDE);
    v.z = 4 * GL_ONE; v.x = 40 * GL_ONE;
    expect_true("a point far off to the right is out",
                gl_frustum_point(&g_frustum, &v) == GL_OUTSIDE);

    // Spheres. The interesting answer is the middle one: a sphere the frustum
    // only touches must be INTERSECT, not OUTSIDE, or objects vanish as they
    // enter the screen.
    v.x = 0; v.y = 0; v.z = 4 * GL_ONE;
    expect_true("a small sphere in the middle is wholly inside",
                gl_frustum_sphere(&g_frustum, &v, GL_ONE / 4) == GL_INSIDE);
    expect_true("a huge sphere around the camera intersects",
                gl_frustum_sphere(&g_frustum, &v, 20 * GL_ONE) == GL_INTERSECT);
    v.x = 40 * GL_ONE;
    expect_true("a small sphere far off to the side is out",
                gl_frustum_sphere(&g_frustum, &v, GL_ONE) == GL_OUTSIDE);

    {
        struct V3 half;
        half.x = GL_ONE; half.y = GL_ONE; half.z = GL_ONE;
        v.x = 0; v.y = 0; v.z = 4 * GL_ONE;
        expect_true("a box in the middle is inside",
                    gl_frustum_box(&g_frustum, &v, &half) == GL_INSIDE);
        v.x = 40 * GL_ONE;
        expect_true("a box far off to the side is out",
                    gl_frustum_box(&g_frustum, &v, &half) == GL_OUTSIDE);
    }
}

// The check that is worth more than all the others in this section: the
// frustum's verdict against the rasteriser's, for a few thousand points.
//
// These are two genuinely independent computations. One extracts six planes
// from the matrix and takes six dot products. The other pushes the point
// through the projection, divides by w, and asks whether the result landed in
// the viewport. They are only allowed to disagree within two pixels of an
// edge, where fixed-point rounding decides ties.
void test_frustum_agrees() {
    struct M4 clip;
    struct V3 v;
    long checked;
    long disagreed;
    long ix;

    puts("\n-- 5. the frustum against the rasteriser --\n");

    gl_state_init(&g_gls, &g_gl);
    glLoadIdentity(&g_gls);
    gl_clip_matrix(&g_gls, &clip);
    gl_frustum_extract(&g_frustum, &clip);

    checked = 0;
    disagreed = 0;
    ix = 0 - 20;
    while (ix <= 20) {
        long iy;
        iy = 0 - 20;
        while (iy <= 20) {
            long iz;
            iz = 1;
            while (iz <= 40) {
                long by_planes;
                long by_raster;
                long sx; long sy; long sz;
                long margin;

                v.x = ix * (GL_ONE / 2);
                v.y = iy * (GL_ONE / 2);
                v.z = iz * (GL_ONE * 3 / 2);

                by_planes = (gl_frustum_point(&g_frustum, &v) == GL_INSIDE);

                by_raster = 0;
                margin = 0;
                if (v.z >= g_gl.near && v.z <= g_gl.far &&
                    gl_project(&g_gl, &v, &sx, &sy, &sz)) {
                    long m;
                    by_raster = (sx >= 0 && sx < g_gl.vw && sy >= 0 && sy < g_gl.vh);
                    // How many pixels from the nearest viewport edge.
                    margin = sx; m = g_gl.vw - 1 - sx; if (m < margin) margin = m;
                    m = sy;                            if (m < margin) margin = m;
                    m = g_gl.vh - 1 - sy;              if (m < margin) margin = m;
                    if (margin < 0) margin = 0 - margin;
                }

                // Skip the samples sitting on a boundary in either dimension.
                if (margin > 2 &&
                    v.z > g_gl.near + GL_ONE / 8 && v.z < g_gl.far - GL_ONE / 8) {
                    checked = checked + 1;
                    if (by_planes != by_raster) disagreed = disagreed + 1;
                }
                iz = iz + 1;
            }
            iy = iy + 1;
        }
        ix = ix + 1;
    }

    printf("  %d points away from every boundary\n", checked);
    expect_true("enough of them to mean something", checked > 2000);
    expect("the planes and the rasteriser never disagree", disagreed, 0);
}

// ============================================================
// 6. culling must not change the picture
// ============================================================

#define NOBJ 25
struct V3 g_obj[NOBJ];

void build_scene() {
    long i;
    i = 0;
    while (i < NOBJ) {
        g_obj[i].x = ((i % 5) - 2) * 4 * GL_ONE;
        g_obj[i].y = 0;
        g_obj[i].z = (i / 5) * 4 * GL_ONE;
        i = i + 1;
    }
}

struct Camera g_cam;
long g_culled;

// Draw the whole scene with the camera as it stands. `cull` selects whether
// objects outside the frustum are skipped.
void draw_scene(long cull) {
    struct M4 clip;
    long i;

    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    cam_apply(&g_gls, &g_cam);

    // The planes come out of projection * modelview, so they are in WORLD
    // space here and an object's world-space centre can be tested directly --
    // no per-object transform, which is the entire point of extracting them
    // from the combined matrix rather than building them from the camera.
    gl_clip_matrix(&g_gls, &clip);
    gl_frustum_extract(&g_frustum, &clip);

    g_culled = 0;
    i = 0;
    while (i < NOBJ) {
        // A cube of half-extent 1 fits inside a sphere of radius sqrt(3).
        if (cull && gl_frustum_sphere(&g_frustum, &g_obj[i], 113512) == GL_OUTSIDE) {
            g_culled = g_culled + 1;
        } else {
            glPushMatrix(&g_gls);
            glTranslatex(&g_gls, g_obj[i].x, g_obj[i].y, g_obj[i].z);
            gl_cube(&g_gls);
            glPopMatrix(&g_gls);
        }
        i = i + 1;
    }
    gl_flush(&g_gl);
}

// Draw one object on its own and report how many pixels it put in the buffer.
// This is the second opinion the culling test needs: the rasteriser's own
// answer to "would this object have been visible", arrived at without
// consulting a single plane.
long object_pixels(long i) {
    long before;
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    cam_apply(&g_gls, &g_cam);
    glTranslatex(&g_gls, g_obj[i].x, g_obj[i].y, g_obj[i].z);
    before = g_gl.pixels;
    gl_cube(&g_gls);
    return g_gl.pixels - before;
}

void cull_at(long yaw, char *what) {
    long h_off;
    long h_on;
    long tris_off;
    long tris_on;
    long unsound;
    long kept_but_empty;
    long rejected;
    long i;

    cam_init(&g_cam);
    g_cam.eye.x = 0; g_cam.eye.y = GL_ONE; g_cam.eye.z = 0 - 6 * GL_ONE;
    g_cam.yaw = yaw << GL_FRAC;

    draw_scene(0);
    tris_off = g_gl.tris_in;
    h_off = win_hash(g_win3d, VPX, VPY, VPW, VPH);

    draw_scene(1);
    tris_on = g_gl.tris_in;
    h_on = win_hash(g_win3d, VPX, VPY, VPW, VPH);
    rejected = g_culled;

    printf("  %s: %d of %d objects rejected, %d triangles down to %d\n",
           what, rejected, NOBJ, tris_off, tris_on);

    expect_true("the picture is bit-for-bit identical", h_on == h_off);
    expect_true("...and fewer triangles were submitted", tris_on < tris_off);

    // Soundness, object by object. Whatever the frustum threw away must draw
    // nothing when drawn on its own -- otherwise culling is removing something
    // that would have been seen, and the equal hashes above were luck.
    unsound = 0;
    kept_but_empty = 0;
    i = 0;
    while (i < NOBJ) {
        long out;
        long px;
        out = (gl_frustum_sphere(&g_frustum, &g_obj[i], 113512) == GL_OUTSIDE);
        px = object_pixels(i);
        if (out) { if (px > 0) unsound = unsound + 1; }
        else if (px == 0) kept_but_empty = kept_but_empty + 1;
        i = i + 1;
    }
    printf("  %d rejected objects would have drawn pixels; "
           "%d kept objects drew none\n", unsound, kept_but_empty);
    expect("nothing the frustum rejected would have been visible", unsound, 0);
    expect_true("...and it did reject something", rejected > 0);
}

void test_culling() {
    puts("\n-- 6. culling changes the cost, never the picture --\n");

    // Two camera angles, because the interesting failure differs. Looking into
    // the grid, most objects are legitimately visible and culling is modest --
    // that is the case where a too-eager frustum would drop something real.
    // Looking away from it, almost everything should go.
    cull_at(20, "looking into the grid");
    cull_at(160, "looking away from it");

    // The property above is only true because the rasteriser has a far plane.
    // Without one, an object past the far plane is rejected by the frustum but
    // WOULD have drawn pixels, and culling starts changing the image. This
    // checks the far clip is really there.
    {
        long before;
        long after;
        gl_clear(&g_gl);
        gl_state_init(&g_gls, &g_gl);
        glLoadIdentity(&g_gls);
        glTranslatex(&g_gls, 0, 0, g_gl.far + 8 * GL_ONE);
        before = g_gl.pixels;
        gl_cube(&g_gls);
        after = g_gl.pixels;
        expect("a cube beyond the far plane rasterises nothing", after - before, 0);

        gl_clear(&g_gl);
        gl_state_init(&g_gls, &g_gl);
        glLoadIdentity(&g_gls);
        glTranslatex(&g_gls, 0, 0, 8 * GL_ONE);
        before = g_gl.pixels;
        gl_cube(&g_gls);
        after = g_gl.pixels;
        expect_true("...and the same cube inside it does", after - before > 100);
    }
}

// ============================================================
// 7. the camera
// ============================================================

// The bounding box of everything drawn since the last clear, read back out of
// the window buffer. Used to measure apparent size, which is how the camera
// tests avoid asserting numbers I picked.
long g_bx0; long g_by0; long g_bx1; long g_by1;

void measure_drawn() {
    long j;
    g_bx0 = VPW; g_by0 = VPH; g_bx1 = -1; g_by1 = -1;
    j = 0;
    while (j < VPH) {
        long i;
        i = 0;
        while (i < VPW) {
            if (g_win[g_win3d].pix[(VPY + j) * g_win[g_win3d].w + VPX + i] != g_gl.bg) {
                if (i < g_bx0) g_bx0 = i;
                if (i > g_bx1) g_bx1 = i;
                if (j < g_by0) g_by0 = j;
                if (j > g_by1) g_by1 = j;
            }
            i = i + 1;
        }
        j = j + 1;
    }
}

void draw_one_cube_at(long x, long y, long z) {
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    cam_apply(&g_gls, &g_cam);
    glTranslatex(&g_gls, x, y, z);
    gl_cube(&g_gls);
}

// A flat square facing the camera, for measuring apparent size.
//
// Not a cube: a cube is two units DEEP, so its silhouette is set by whichever
// face is nearest and halving the distance to its centre does not halve the
// distance to that face. The ratio comes out 2.14, not 2, and the test would
// have been measuring the cube's thickness rather than the projection. The
// invariant is about a point, so measure something flat.
void draw_quad_at(long x, long y, long z) {
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glColor3ub(&g_gls, 200, 200, 90);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    cam_apply(&g_gls, &g_cam);
    glTranslatex(&g_gls, x, y, z);
    glBegin(&g_gls, GL_QUADS);
    glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 0);
    glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE,     0);
    glVertex3x(&g_gls, GL_ONE,     GL_ONE,     0);
    glVertex3x(&g_gls, GL_ONE,     0 - GL_ONE, 0);
    glEnd(&g_gls);
}

void test_camera() {
    long w_far;
    long w_near;

    puts("\n-- 7. a camera you can fly --\n");

    cam_init(&g_cam);
    g_cam.eye.x = 0; g_cam.eye.y = 0; g_cam.eye.z = 0;

    // Twice as close is twice as big. An invariant of perspective, not a
    // number I read off a screenshot -- it holds whatever the focal length,
    // the viewport size or the near plane happen to be.
    draw_quad_at(0, 0, 16 * GL_ONE);
    measure_drawn();
    w_far = g_bx1 - g_bx0;
    draw_quad_at(0, 0, 8 * GL_ONE);
    measure_drawn();
    w_near = g_bx1 - g_bx0;
    printf("  a square 16 units away is %d pixels wide, at 8 units %d\n",
           w_far, w_near);
    expect_true("...it was actually drawn", w_far > 10);
    expect_near("halving the distance doubles the apparent width",
                w_near, w_far * 2, 3);

    // Walking forward is the same as bringing the object closer.
    cam_move(&g_cam, 8 * GL_ONE, 0, 0);
    draw_quad_at(0, 0, 16 * GL_ONE);
    measure_drawn();
    expect_near("walking 8 units forward looks like standing 8 units closer",
                g_bx1 - g_bx0, w_near, 2);

    // Turning right by ninety degrees takes what was ahead off the screen and
    // brings what was to the right into view. Checking both directions
    // matters: a camera that shows nothing at all would pass the first half.
    cam_init(&g_cam);
    draw_one_cube_at(0, 0, 10 * GL_ONE);
    measure_drawn();
    expect_true("looking along +z, a cube at +z is visible", g_bx1 >= 0);
    cam_look(&g_cam, 90 << GL_FRAC, 0);
    draw_one_cube_at(0, 0, 10 * GL_ONE);
    measure_drawn();
    expect_true("after turning 90 degrees it is gone", g_bx1 < 0);
    draw_one_cube_at(10 * GL_ONE, 0, 0);
    measure_drawn();
    expect_true("...and a cube at +x is now in front", g_bx1 >= 0);

    // HANDEDNESS. Every check above survives a MIRRORED world, because every
    // one of them puts the object on the view axis or measures a size. Build
    // gluLookAtx's basis with its cross products the other way round and the
    // world is reflected -- and the sabotage matrix showed the whole suite
    // passing with exactly that bug in place.
    //
    // The question a mirror cannot survive is which SIDE something lands on.
    cam_init(&g_cam);
    draw_quad_at(2 * GL_ONE, 0, 10 * GL_ONE);
    measure_drawn();
    expect_true("something to the right of the camera appears right of centre",
                g_bx1 >= 0 && g_bx0 > VPW / 2);
    draw_quad_at(0 - 2 * GL_ONE, 0, 10 * GL_ONE);
    measure_drawn();
    expect_true("...and something to its left, left of centre",
                g_bx1 >= 0 && g_bx1 < VPW / 2);
    draw_quad_at(0, 2 * GL_ONE, 10 * GL_ONE);
    measure_drawn();
    expect_true("something above appears above centre",
                g_by1 >= 0 && g_by1 < VPH / 2);
    draw_quad_at(0, 0 - 2 * GL_ONE, 10 * GL_ONE);
    measure_drawn();
    expect_true("...and something below, below centre",
                g_by1 >= 0 && g_by0 > VPH / 2);

    // Pitch is clamped short of vertical. At exactly 90 the forward vector is
    // parallel to `up`, the cross product is zero, and gluLookAt builds a
    // matrix of zeroes -- a black viewport that looks like a renderer fault.
    cam_init(&g_cam);
    cam_look(&g_cam, 0, 200 << GL_FRAC);
    expect("pitch clamps below vertical", g_cam.pitch >> GL_FRAC, 89);
    cam_look(&g_cam, 0, 0 - (400 << GL_FRAC));
    expect("...in both directions", g_cam.pitch >> GL_FRAC, 0 - 89);
    cam_init(&g_cam);
    cam_look(&g_cam, 725 << GL_FRAC, 0);
    expect("yaw wraps", g_cam.yaw >> GL_FRAC, 5);

    // Strafing must not change height, whatever the camera is looking at.
    cam_init(&g_cam);
    cam_look(&g_cam, 30 << GL_FRAC, 40 << GL_FRAC);
    cam_move(&g_cam, 0, 5 * GL_ONE, 0);
    expect("strafing while looking up does not change height", g_cam.eye.y, 0);
}

// ============================================================
// 8. the viewport as a widget
// ============================================================

struct Ui g_ui;
struct GlView g_view;
long g_panel_win;

#define PPX (WM_BORDER + 8)
#define PPY (WM_TITLE_H + 8)
#define PPW 170

// One widget frame, with the pointer placed and the button state given. The
// viewport is the first widget, so a hit inside it is a hit on the view.
void view_frame(long mx, long my, long down, long pressed) {
    long released;
    released = 0;
    if (!down && g_ui.mdown) released = 1;
    g_mouse_x = mx;
    g_mouse_y = my;
    wm_cursor_move(mx, my);
    ui_begin(&g_ui, g_win3d, WM_BORDER + 2, WM_TITLE_H + 2, VPW + 4);
    ui_input(&g_ui, down, pressed, released, 0);
    ui_glview(&g_ui, &g_view, VPH + 4);
    ui_end(&g_ui);
}

void view_key(long mx, long my, long key) {
    g_mouse_x = mx;
    g_mouse_y = my;
    wm_cursor_move(mx, my);
    ui_begin(&g_ui, g_win3d, WM_BORDER + 2, WM_TITLE_H + 2, VPW + 4);
    ui_input(&g_ui, 0, 0, 0, key);
    ui_glview(&g_ui, &g_view, VPH + 4);
    ui_end(&g_ui);
}

void test_widget() {
    long inx;
    long iny;
    long outx;
    long outy;

    puts("\n-- 8. the viewport is a widget --\n");

    ui_init(&g_ui);
    ui_glview_init(&g_view);
    wm_cursor_show(1);

    inx = g_win[g_win3d].x + WM_BORDER + 60;
    iny = g_win[g_win3d].y + WM_TITLE_H + 60;
    outx = g_win[g_win3d].x + g_win[g_win3d].w + 40;
    outy = g_win[g_win3d].y + 10;

    view_frame(outx, outy, 0, 0);
    expect("the pointer outside: not hot", g_view.hot, 0);
    view_frame(inx, iny, 0, 0);
    expect("the pointer inside: hot", g_view.hot, 1);
    expect("...but not yet focused", g_view.focused, 0);

    // Press inside: it takes the pointer and the focus.
    view_frame(inx, iny, 1, 1);
    expect("a press inside takes the pointer", g_view.active, 1);
    expect("...and the focus", g_view.focused, 1);
    // No motion has been measured yet, so the first frame of a drag must
    // report zero. Reporting the distance from the widget's origin instead is
    // the bug that makes a camera jump on every click.
    expect("the first frame of a drag has no motion yet", g_view.dx, 0);

    view_frame(inx + 10, iny + 4, 1, 0);
    expect("the second frame reports the motion since the first", g_view.dx, 10);
    expect("...in y as well", g_view.dy, 4);

    // Dragging off the widget must keep working. A camera that stops turning
    // because the pointer left a rectangle is the single most irritating
    // behaviour a 3D view can have.
    view_frame(outx, outy, 1, 0);
    expect_true("a drag continues outside the viewport", g_view.active == 1);
    expect_true("...and still reports motion", g_view.dx != 0);

    // The release frame itself still reports `active` -- the pointer is given
    // up at ui_end, after every widget has been asked. It is the NEXT frame
    // that must show it gone, which is why this checks two frames and not one.
    view_frame(outx, outy, 0, 0);
    expect("releasing outside is not a click", g_view.clicked, 0);
    view_frame(outx, outy, 0, 0);
    expect("...and the pointer has been given up", g_view.active, 0);

    // A press that starts OUTSIDE must not drag the view even when it passes
    // over it -- otherwise dragging a slider on another panel spins the camera.
    view_frame(outx, outy, 1, 1);
    view_frame(inx, iny, 1, 0);
    expect("a press begun outside never takes the view", g_view.active, 0);
    expect("...and reports no motion", g_view.dx, 0);
    view_frame(inx, iny, 0, 0);

    // Keys only while focused.
    view_frame(inx, iny, 1, 1);
    view_frame(inx, iny, 0, 0);
    view_key(inx, iny, 'w');
    expect("a focused view receives keys", g_view.key, 'w');
    view_frame(outx, outy, 1, 1);
    view_frame(outx, outy, 0, 0);
    view_key(inx, iny, 'w');
    expect("clicking away removes the focus, and the keys", g_view.key, 0);

    expect("the interior belongs to the renderer, not the widget",
           g_view.w, VPW + 4 - 2);
}

// A widget drawn ON TOP of the 3D, which is the other half of what was asked
// for. Immediate mode makes this almost free: draw the scene into the backing
// buffer, then draw the panel into the same buffer afterwards. Ordering the
// writes is the whole mechanism -- there is no compositing, no z-order for
// widgets, no surface.
void test_overlay() {
    long under_before;
    long under_after;
    long away_before;
    long away_after;
    long hud_on;

    puts("\n-- 9. widgets on top of the 3D --\n");

    ui_init(&g_ui);
    ui_glview_init(&g_view);
    hud_on = 1;
    cam_init(&g_cam);
    g_cam.eye.y = GL_ONE;
    g_cam.eye.z = 0 - 6 * GL_ONE;

    g_mouse_x = 0; g_mouse_y = 0;
    wm_cursor_move(0, 0);

    // Two points: one that the HUD will land on, one well away from it.
    // Frame one is the scene alone.
    draw_scene(1);
    under_before = g_win[g_win3d].pix[(VPY + 12) * g_win[g_win3d].w + VPX + 100];
    away_before  = g_win[g_win3d].pix[(VPY + 150) * g_win[g_win3d].w + VPX + 150];

    // Frame two: the same scene, then a HUD over its top-left corner.
    draw_scene(1);
    ui_begin(&g_ui, g_win3d, VPX, VPY, VPW);
    ui_input(&g_ui, 0, 0, 0, 0);
    ui_move_to(&g_ui, VPX + 10, VPY + 10, 120);
    ui_checkbox(&g_ui, "hud", &hud_on);
    ui_end(&g_ui);
    under_after = g_win[g_win3d].pix[(VPY + 12) * g_win[g_win3d].w + VPX + 100];
    away_after  = g_win[g_win3d].pix[(VPY + 150) * g_win[g_win3d].w + VPX + 150];

    expect_true("the HUD covers the 3D underneath it", under_after == g_ui.panel);
    // Which only means something if there WAS something else there first.
    expect_true("...and there was 3D under it before", under_before != g_ui.panel);
    expect_true("...and the scene outside the HUD is untouched",
                away_after == away_before);
}

// A HUD widget sitting inside the viewport must be CLICKABLE, and clicking it
// must not also drag the camera.
//
// This is the bug the live screenshot caught and no unit test would have: the
// viewport fills its whole rectangle, so the checkbox drawn on top of it is
// inside it, and the viewport was being offered the press first. The checkbox
// highlighted on hover and did absolutely nothing -- it looked like a widget
// and behaved like a picture of one.
//
// The fix is an order: HUD asked first, viewport last. The frame below builds
// them in that order, and the two checks are the two halves of the property --
// the overlay gets the press, and the viewport does not also get it.
long g_hud_v;

void hud_frame(long mx, long my, long down, long pressed) {
    long released;
    released = 0;
    if (!down && g_ui.mdown) released = 1;
    g_mouse_x = mx;
    g_mouse_y = my;
    wm_cursor_move(mx, my);

    ui_begin(&g_ui, g_win3d, VPX + 8, VPY + 8, 130);
    ui_input(&g_ui, down, pressed, released, 0);
    ui_checkbox(&g_ui, "wireframe", &g_hud_v);
    ui_window(&g_ui, g_win3d, VPX - 1, VPY - 1, VPW + 2);
    ui_glview(&g_ui, &g_view, VPH + 2);
    ui_end(&g_ui);
}

void test_hud_input() {
    long cbx;
    long cby;
    long farx;
    long fary;

    puts("\n-- 9b. a widget on top of the viewport is still a widget --\n");

    ui_init(&g_ui);
    ui_glview_init(&g_view);
    g_hud_v = 0;

    // The checkbox box itself, in screen coordinates: six pixels into the
    // widget, which is inside the viewport by construction.
    cbx = g_win[g_win3d].x + VPX + 8 + UI_BOX / 2;
    cby = g_win[g_win3d].y + VPY + 8 + UI_ROW_H / 2;
    farx = g_win[g_win3d].x + VPX + 200;
    fary = g_win[g_win3d].y + VPY + 150;

    hud_frame(cbx, cby, 0, 0);
    hud_frame(cbx, cby, 1, 1);
    expect("pressing the HUD does not hand the pointer to the viewport",
           g_view.active, 0);
    hud_frame(cbx, cby, 0, 0);
    expect("...and the release toggles it", g_hud_v, 1);
    hud_frame(cbx, cby, 0, 0);
    expect("...without the camera having been dragged", g_view.dx, 0);

    // The control: the same press, a couple of hundred pixels away from the
    // HUD, must reach the viewport. Without this the check above passes for a
    // viewport that never takes any input at all.
    hud_frame(farx, fary, 0, 0);
    hud_frame(farx, fary, 1, 1);
    expect("a press on the bare viewport does reach it", g_view.active, 1);
    hud_frame(farx + 20, fary, 1, 0);
    expect("...and drags the camera", g_view.dx, 20);
    hud_frame(farx + 20, fary, 0, 0);
    hud_frame(farx + 20, fary, 0, 0);
    expect("...and the checkbox was not toggled by it", g_hud_v, 1);
}

// ============================================================
// 10. what a frame costs
// ============================================================
void test_cost() {
    long moving;
    long still;

    puts("\n-- 10. what a frame costs the screen --\n");

    ui_init(&g_ui);
    ui_glview_init(&g_view);
    cam_init(&g_cam);
    g_cam.eye.y = GL_ONE;
    g_cam.eye.z = 0 - 6 * GL_ONE;
    g_mouse_x = 0; g_mouse_y = 0;
    wm_cursor_move(0, 0);
    wm_cursor_show(0);

    // A warm-up frame. The label has never been drawn, so its first frame
    // legitimately invalidates -- measuring that one would be measuring
    // start-up, not a steady frame.
    draw_scene(1);
    ui_begin(&g_ui, g_win3d, VPX, VPY, VPW);
    ui_input(&g_ui, 0, 0, 0, 0);
    ui_move_to(&g_ui, VPX + 10, VPY + 10, 120);
    ui_label(&g_ui, "frustum");
    ui_end(&g_ui);
    wm_present();

    wm_reset_counters();
    draw_scene(1);
    ui_begin(&g_ui, g_win3d, VPX, VPY, VPW);
    ui_input(&g_ui, 0, 0, 0, 0);
    ui_move_to(&g_ui, VPX + 10, VPY + 10, 120);
    ui_label(&g_ui, "frustum");
    ui_end(&g_ui);
    wm_present();
    moving = wm_pixels;
    printf("  a frame with the scene re-rendered: %d pixels, "
           "%d widgets pushed damage\n", moving, g_ui.invalidations);
    printf("  the viewport is %d, the screen is %d\n", VPW * VPH, wm_screen_pixels());
    expect("a rendered frame costs its viewport and no more", moving, VPW * VPH);

    // Nothing moved: the scene is not re-rendered and no widget changed
    // state, so the frame costs nothing at all. This is the whole argument
    // for immediate mode with retained damage underneath, stated as a number.
    wm_reset_counters();
    ui_begin(&g_ui, g_win3d, VPX, VPY, VPW);
    ui_input(&g_ui, 0, 0, 0, 0);
    ui_move_to(&g_ui, VPX + 10, VPY + 10, 120);
    ui_label(&g_ui, "frustum");
    ui_end(&g_ui);
    wm_present();
    still = wm_pixels;
    printf("  an idle frame: %d pixels, %d widgets pushed damage\n",
           still, g_ui.invalidations);
    expect("an idle frame costs nothing", still, 0);

    wm_reset_counters();
    ui_begin(&g_ui, g_win3d, VPX, VPY, VPW);
    ui_input(&g_ui, 0, 0, 0, 0);
    ui_move_to(&g_ui, VPX + 10, VPY + 10, 120);
    ui_label(&g_ui, "frustum");
    ui_end(&g_ui);
    wm_present();
    printf("  a second idle frame: %d pixels, %d widgets pushed damage\n",
           wm_pixels, g_ui.invalidations);
    expect("...and so does the next one", wm_pixels, 0);

    // The bug this section actually found. A label used to remember the
    // ADDRESS of its text, so the same word written at two call sites was two
    // different addresses and the label repainted every frame forever. It hid
    // because the frames it sat in were repainting a 3D viewport anyway, and
    // the extra rectangle merged into the viewport's damage.
    //
    // Both directions matter: same text from a different pointer must be
    // silent, different text from the same pointer must repaint. A label that
    // remembers the pointer fails the first; one that remembers only its
    // length fails the second.
    {
        char buf[8];
        buf[0] = 'f'; buf[1] = 'r'; buf[2] = 'u'; buf[3] = 's';
        buf[4] = 't'; buf[5] = 'u'; buf[6] = 'm'; buf[7] = 0;

        wm_reset_counters();
        ui_begin(&g_ui, g_win3d, VPX, VPY, VPW);
        ui_input(&g_ui, 0, 0, 0, 0);
        ui_move_to(&g_ui, VPX + 10, VPY + 10, 120);
        ui_label(&g_ui, buf);
        ui_end(&g_ui);
        wm_present();
        expect("the same text from a different pointer is not a change",
               g_ui.invalidations, 0);

        buf[6] = 'M';
        wm_reset_counters();
        ui_begin(&g_ui, g_win3d, VPX, VPY, VPW);
        ui_input(&g_ui, 0, 0, 0, 0);
        ui_move_to(&g_ui, VPX + 10, VPY + 10, 120);
        ui_label(&g_ui, buf);
        ui_end(&g_ui);
        wm_present();
        expect("...but different text at the same pointer is", g_ui.invalidations, 1);
        expect("...and it cost exactly the label", wm_pixels, 120 * UI_ROW_H);
    }

    {
        long fast;
        long full;
        long save;
        fast = wm_fb_checksum();
        save = wm_no_damage;
        wm_no_damage = 1;
        wm_present();
        wm_no_damage = save;
        full = wm_fb_checksum();
        if (fast == full) puts("  ok  the screen matches a full repaint\n");
        else fail("the screen matches a full repaint");
    }
}

// ============================================================
// the interactive demo
// ============================================================

long g_console;
long g_wire;
long g_cull;
long g_light;
long g_fov;
long g_prev_down;
long g_spin;
long g_hud;
long g_stat_tris;

void build_desktop() {
    wm_init(rgb(20, 24, 34));
    wmin_init();
    term_init();
    ui_init(&g_ui);
    ui_glview_init(&g_view);
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);

    g_win3d = wm_create(30, 40, VPW + WM_BORDER * 2 + 8,
                        VPH + WM_TITLE_H + WM_BORDER + 8, "scene");
    wm_decorate(g_win3d);
    gl_bind(&g_gl, g_win3d, VPX, VPY, VPW, VPH);
    gl_state_init(&g_gls, &g_gl);

    // A light with a vertical component, for the demo only. The default in
    // gl_bind points straight down the view axis, which is right for showing a
    // cube and wrong for showing a ground plane: the ground's normal is then
    // exactly perpendicular to the light, so it renders at the ambient floor
    // and looks like a hole. Unit length, because gl_shade normalises the
    // surface normal and trusts the light.
    g_gl.light.x = 19860; g_gl.light.y = 0 - 52961; g_gl.light.z = 0 - 33101;
    g_gl.bg = rgb(26, 32, 46);

    g_panel_win = wm_create(370, 40, PPW + 16 + WM_BORDER,
                            PPY + 8 * (UI_ROW_H + UI_PAD) + 8, "camera");
    wm_decorate(g_panel_win);

    g_console = term_create(30, 320, 68, 16, "console");
    if (g_console >= 0) {
        term_puts(g_console, "nano-os K16 -- OpenGL-shaped, still no floats\n");
        term_puts(g_console, "glBegin/glEnd: TRIANGLES STRIP FAN QUADS\n");
        term_puts(g_console, "  QUAD_STRIP LINES LINE_LOOP POINTS\n");
        term_puts(g_console, "matrix stack, gluLookAtx, six frustum planes\n");
        term_puts(g_console, "drag inside the view to look, WASD to move\n\n");
        term_prompt(g_console);
        term_flush(g_console);
    }

    build_scene();
    cam_init(&g_cam);
    g_cam.eye.x = 0; g_cam.eye.y = 2 * GL_ONE; g_cam.eye.z = 0 - 7 * GL_ONE;
    g_cam.yaw = 0;
    g_cam.pitch = 0 - (8 << GL_FRAC);
    g_wire = 0;
    g_cull = 1;
    g_light = 1;
    g_hud = 1;
    g_spin = 0;
    g_fov = 60;
    g_prev_down = 0;

    wm_cursor_show(1);
    mouse_warp(fb_width / 2, fb_height / 2);
    wm_cursor_move(g_mouse_x, g_mouse_y);
    wm_set_focus(g_win3d);
    wm_present();
}

void render_scene() {
    struct M4 clip;
    long i;

    if (!g_win[g_win3d].used) return;
    g_gl.wire = g_wire;
    g_gl.lighting = g_light;

    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glMatrixMode(&g_gls, GL_PROJECTION);
    glLoadIdentity(&g_gls);
    gluPerspectivex(&g_gls, g_fov << GL_FRAC,
                    fx_div(fx_from_int(VPW), fx_from_int(VPH)), GL_ONE / 4);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    cam_apply(&g_gls, &g_cam);

    gl_clip_matrix(&g_gls, &clip);
    gl_frustum_extract(&g_frustum, &clip);

    // The ground, as one quad strip.
    g_gls.colour = rgb(52, 62, 78);
    gl_ground(&g_gls, 12, 0 - 4, 20);

    g_culled = 0;
    i = 0;
    while (i < NOBJ) {
        if (g_cull && gl_frustum_sphere(&g_frustum, &g_obj[i], 113512) == GL_OUTSIDE) {
            g_culled = g_culled + 1;
        } else {
            glPushMatrix(&g_gls);
            glTranslatex(&g_gls, g_obj[i].x, g_obj[i].y, g_obj[i].z);
            if (i % 7 == 3) {
                g_gls.colour = rgb(210, 160, 70);
                glRotatex(&g_gls, g_spin << GL_FRAC, 0, GL_ONE, 0);
                gl_pyramid(&g_gls);
            } else {
                glRotatex(&g_gls, (g_spin + i * 13) << GL_FRAC, 0, GL_ONE, 0);
                gl_cube(&g_gls);
            }
            glPopMatrix(&g_gls);
        }
        i = i + 1;
    }
    g_stat_tris = g_gl.tris_drawn;
    gl_flush(&g_gl);
}

void event_loop() {
    long last_tick;
    long redraw;
    last_tick = g_ticks;
    redraw = 1;
    for (;;) {
        struct MEvent e;
        long down;
        long pressed;
        long released;
        long key;
        char c;

        pressed = 0; released = 0; key = 0;
        down = g_prev_down;
        while (mouse_pop(&e)) {
            wm_input_mouse(e.x, e.y, e.btn);
            down = e.btn & 1;
            if (down && !g_prev_down) pressed = 1;
            if (!down && g_prev_down) released = 1;
            g_prev_down = down;
        }
        for (;;) {
            c = kbd_getchar_nb();
            if (c == 0) break;
            if (!wm_input_key(c)) key = c;
        }

        // One ui_begin per frame, then ui_window to move to the other window.
        // Two ui_begin calls would restart the id counter and the panel's
        // first widget would share an identity with the viewport.
        if (!g_win[g_win3d].used && !g_win[g_panel_win].used) {
            wm_present();
            cpu_idle();
            continue;
        }
        ui_begin(&g_ui, g_win[g_win3d].used ? g_win3d : g_panel_win,
                 WM_BORDER + 2, WM_TITLE_H + 2, VPW + 4);
        ui_input(&g_ui, down, pressed, released, key);

        g_view.dx = 0;
        g_view.dy = 0;
        g_view.key = 0;
        if (g_win[g_win3d].used) {
            // ORDER MATTERS, and it is the whole trick of putting widgets over
            // a 3D view:
            //
            //   1. the scene, into the backing buffer
            //   2. the HUD, into the same buffer, on top of it -- and asked
            //      for the pointer FIRST
            //   3. the viewport widget last, taking the pointer only if
            //      nothing above wanted it
            //
            // A viewport fills its whole rectangle, so anything drawn on top
            // of it is also inside it, and whichever widget is offered the
            // press first is the one that gets it. Build the viewport before
            // the HUD and the HUD becomes a picture of a checkbox: painted,
            // highlighted on hover, and completely dead to clicks.
            //
            // Each widget is still called exactly once. Calling it twice --
            // once for input, once to paint over the new scene -- would fire
            // its toggle twice, because a press edge lasts the whole frame.
            if (redraw) render_scene();

            ui_window(&g_ui, g_win3d, VPX + 8, VPY + 8, 130);
            if (g_hud) {
                if (ui_checkbox(&g_ui, "wireframe", &g_wire)) redraw = 1;
                if (ui_checkbox(&g_ui, "frustum cull", &g_cull)) redraw = 1;
            } else {
                // Skipped widgets still consume their ids, or every widget
                // after them changes identity when the HUD is toggled.
                ui_id(&g_ui, g_ui.id + 2);
            }

            ui_window(&g_ui, g_win3d, VPX - 1, VPY - 1, VPW + 2);
            ui_glview(&g_ui, &g_view, VPH + 2);

            if (g_view.dx || g_view.dy) {
                // A third of a degree per pixel, in 16.16, so a slow drag
                // still turns the camera.
                cam_look(&g_cam, g_view.dx * (GL_ONE / 3), 0 - g_view.dy * (GL_ONE / 3));
                redraw = 1;
            }
            if (g_view.key) {
                long step;
                step = GL_ONE / 2;
                if (g_view.key == 'w') cam_move(&g_cam, step, 0, 0);
                if (g_view.key == 's') cam_move(&g_cam, 0 - step, 0, 0);
                if (g_view.key == 'a') cam_move(&g_cam, 0, 0 - step, 0);
                if (g_view.key == 'd') cam_move(&g_cam, 0, step, 0);
                if (g_view.key == 'r') cam_move(&g_cam, 0, 0, step);
                if (g_view.key == 'f') cam_move(&g_cam, 0, 0, 0 - step);
                redraw = 1;
            }
        } else {
            // The viewport and its HUD are three ids. If the window is closed
            // they must still be reserved, or the panel's first widget
            // inherits the viewport's identity -- and its hover state.
            ui_id(&g_ui, 3);
        }

        if (g_win[g_panel_win].used) {
            ui_window(&g_ui, g_panel_win, PPX, PPY, PPW);
            ui_label(&g_ui, "field of view");
            if (ui_slider(&g_ui, &g_fov, 25, 100)) redraw = 1;
            ui_label(&g_ui, "triangles drawn");
            ui_progress(&g_ui, g_stat_tris, 0, NOBJ * 12);
            if (ui_checkbox(&g_ui, "lighting", &g_light)) redraw = 1;
            if (ui_checkbox(&g_ui, "show hud", &g_hud)) redraw = 1;
            if (ui_button(&g_ui, "reset view")) {
                cam_init(&g_cam);
                g_cam.eye.y = 2 * GL_ONE;
                g_cam.eye.z = 0 - 7 * GL_ONE;
                g_cam.pitch = 0 - (8 << GL_FRAC);
                redraw = 1;
            }
        }
        ui_end(&g_ui);

        if (g_ticks != last_tick) {
            last_tick = g_ticks;
            g_spin = (g_spin + 1) % 360;
            redraw = 1;
        } else if (!g_view.dx && !g_view.dy && !g_view.key) {
            redraw = 0;
        }

        wm_present();
        cpu_idle();
    }
}

void run_tests() {
    printf("FB: %dx%d at %d bpp\n", fb_width, fb_height, fb_bpp);
    printf("viewport %dx%d = %d pixels\n\n", VPW, VPH, VPW * VPH);

    build_cube_verts();
    build_cube_quads();
    build_scene();
    face(0, 220,  70,  70);
    face(1,  70, 200, 110);
    face(2,  80, 120, 230);
    face(3, 230, 190,  60);
    face(4, 200,  90, 210);
    face(5,  80, 210, 210);

    build_3d_window();

    test_api_math();
    test_prim_counts();
    test_prim_pixels();
    test_frustum_planes();
    test_frustum_agrees();
    test_culling();
    test_camera();
    test_widget();
    test_overlay();
    test_hud_input();
    test_cost();

    printf("\nheap: %d pages mapped, %d bytes free\n", heap_pages, heap_bytes_free());

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else puts("\nPASS: an OpenGL-shaped API, a real frustum, and a 3D widget\n");

    puts("\nGLAPITEST DONE\n");
}

int main() {
    serial_init();
    g_fail = 0;

    puts("\nnano-os: glBegin, a matrix stack and six clipping planes\n");

    if (!fb_init(1024, 768)) { puts("fb_init failed\n"); for (;;) { } }
    if (!mm_init())          { puts("mm_init failed\n"); for (;;) { } }
    mm_protect_null();

    kbd_init();
    interrupts_init(100);

    run_tests();

    build_desktop();
    puts("desktop up; the machine is now interactive\n");
    event_loop();
    return 0;
}
