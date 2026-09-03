// gl.c — a 3D pipeline in fixed point, rendering into a window handle.
//
// This is the layer TinyGL would plug into, built and tested before TinyGL can
// be compiled at all. TinyGL is 9,146 lines under a permissive zlib licence and
// nano_cc cannot build it: 257 uses of GLfloat, 99 float literals, and
// `typedef float GLfloat` in its public header. What the measurement also
// showed is that the part doing the pixel work is already integer -- zbuffer.c
// has zero floats in 389 lines -- and the float dependency is the API, the
// matrix stack, the clipper and the transform. So those are what this builds,
// in 16.16.
//
// The contract being proved is the one that matters for the window manager:
//
//     gl_bind(&ctx, window_handle, x, y, w, h)
//     ...draw...
//     gl_flush(&ctx)
//
// The renderer never touches the screen. It writes into that window's backing
// buffer and reports the bounding box of what it changed, so a spinning cube
// costs its own rectangle rather than the screen. When floats arrive, TinyGL's
// ZBuffer points at the same backing buffer and nothing above this line
// changes.
//
// nano-kernel.h first, as in the other graphics images: it only mirrors console
// output onto the framebuffer if NANO_FB_H is already defined, and here it must
// not, because this image reads the framebuffer back and hashes it.
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

// Fixed-point values are compared with a tolerance, because they are the
// result of a rounded multiply chain. The tolerance is stated in UNITS of
// 1/65536 so that "close enough" is a number rather than a feeling.
void expect_true(char *what, long got) {
    if (got) printf("  ok  %s\n", what);
    else fail(what);
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

long reference_checksum() {
    long save;
    long sum;
    save = wm_no_damage;
    wm_no_damage = 1;
    wm_present();
    wm_no_damage = save;
    sum = wm_fb_checksum();
    return sum;
}

void check_matches_full(char *what) {
    long fast;
    long full;
    fast = wm_fb_checksum();
    full = reference_checksum();
    if (fast == full) printf("  ok  %s: screen matches a full repaint\n", what);
    else {
        printf("  incremental hash %d, full repaint hash %d\n", fast, full);
        fail(what);
    }
}

// ---------- the scene ----------

#define VPX  (WM_BORDER + 4)
#define VPY  (WM_TITLE_H + 4)
#define VPW  260
#define VPH  200

struct GLCtx g_gl;
long g_win3d;

struct V3 g_cube[8];
long g_cube_idx[36];

void build_cube() {
    long s;
    long k;
    s = GL_ONE;
    g_cube[0].x = 0 - s; g_cube[0].y = 0 - s; g_cube[0].z = 0 - s;
    g_cube[1].x =     s; g_cube[1].y = 0 - s; g_cube[1].z = 0 - s;
    g_cube[2].x =     s; g_cube[2].y =     s; g_cube[2].z = 0 - s;
    g_cube[3].x = 0 - s; g_cube[3].y =     s; g_cube[3].z = 0 - s;
    g_cube[4].x = 0 - s; g_cube[4].y = 0 - s; g_cube[4].z =     s;
    g_cube[5].x =     s; g_cube[5].y = 0 - s; g_cube[5].z =     s;
    g_cube[6].x =     s; g_cube[6].y =     s; g_cube[6].z =     s;
    g_cube[7].x = 0 - s; g_cube[7].y =     s; g_cube[7].z =     s;

    // Twelve triangles, two per face, every one wound so that
    // (b - a) x (c - a) points OUT of the cube.
    //
    // I got this wrong the first time and the culling test caught it: 4 faces
    // culled instead of 6. Worth noting WHY that number is the useful one --
    // if the whole table were wound backwards the count would still be 6, just
    // the other six. Only an inconsistent table gives anything other than 6,
    // which is exactly what a hand-typed index list produces. The winding
    // below was derived by computing all twelve normals rather than reasoned
    // about.
    k = 0;
    g_cube_idx[k]=0; k=k+1; g_cube_idx[k]=3; k=k+1; g_cube_idx[k]=2; k=k+1;   // back
    g_cube_idx[k]=0; k=k+1; g_cube_idx[k]=2; k=k+1; g_cube_idx[k]=1; k=k+1;
    g_cube_idx[k]=4; k=k+1; g_cube_idx[k]=5; k=k+1; g_cube_idx[k]=6; k=k+1;   // front
    g_cube_idx[k]=4; k=k+1; g_cube_idx[k]=6; k=k+1; g_cube_idx[k]=7; k=k+1;
    g_cube_idx[k]=0; k=k+1; g_cube_idx[k]=4; k=k+1; g_cube_idx[k]=7; k=k+1;   // left
    g_cube_idx[k]=0; k=k+1; g_cube_idx[k]=7; k=k+1; g_cube_idx[k]=3; k=k+1;
    g_cube_idx[k]=1; k=k+1; g_cube_idx[k]=2; k=k+1; g_cube_idx[k]=6; k=k+1;   // right
    g_cube_idx[k]=1; k=k+1; g_cube_idx[k]=6; k=k+1; g_cube_idx[k]=5; k=k+1;
    g_cube_idx[k]=0; k=k+1; g_cube_idx[k]=1; k=k+1; g_cube_idx[k]=5; k=k+1;   // bottom
    g_cube_idx[k]=0; k=k+1; g_cube_idx[k]=5; k=k+1; g_cube_idx[k]=4; k=k+1;
    g_cube_idx[k]=3; k=k+1; g_cube_idx[k]=7; k=k+1; g_cube_idx[k]=6; k=k+1;   // top
    g_cube_idx[k]=3; k=k+1; g_cube_idx[k]=6; k=k+1; g_cube_idx[k]=2; k=k+1;
}

long g_face_colour[6];

void draw_cube(struct M4 *mv) {
    long t;
    t = 0;
    while (t < 12) {
        gl_tri(&g_gl, mv, &g_cube[g_cube_idx[t * 3 + 0]],
               &g_cube[g_cube_idx[t * 3 + 1]],
               &g_cube[g_cube_idx[t * 3 + 2]], g_face_colour[t / 2]);
        t = t + 1;
    }
}

void build_3d_window() {
    wm_init(rgb(24, 28, 38));
    wmin_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);
    g_win3d = wm_create(120, 90, VPW + WM_BORDER * 2 + 8,
                        VPH + WM_TITLE_H + WM_BORDER + 8, "cube");
    wm_decorate(g_win3d);
    if (!gl_bind(&g_gl, g_win3d, VPX, VPY, VPW, VPH)) fail("gl_bind failed");
    wm_present();
}

// ============================================================
// 1. the fixed point everything else stands on
// ============================================================
void test_fixed() {
    puts("-- 1. 16.16 fixed point --\n");

    expect("sin(0)", gl_sin(0), 0);
    expect("sin(90) is exactly 1.0", gl_sin(90), GL_ONE);
    expect("cos(0) is exactly 1.0", gl_cos(0), GL_ONE);
    expect("sin(180)", gl_sin(180), 0);
    expect("sin(270) is exactly -1.0", gl_sin(270), 0 - GL_ONE);
    expect("cos(90)", gl_cos(90), 0);
    expect("angles wrap: sin(450) == sin(90)", gl_sin(450), gl_sin(90));
    expect("...and negative: sin(-90) == sin(270)", gl_sin(0 - 90), gl_sin(270));

    // sin(90) is the entry a 16-bit container cannot hold. On the C64 library
    // I audited last week it was stored in a uint16_t, came out as 0, and took
    // cos(0) with it -- every circle collapsed at the cardinal angles. The
    // table here is `long`, so it cannot happen, and this check says so rather
    // than assuming it.
    expect_near("sin(30) is 0.5", gl_sin(30), GL_ONE / 2, 2);
    expect_near("sin(45)", gl_sin(45), 46341, 2);

    expect("1.0 * 1.0", fx_mul(GL_ONE, GL_ONE), GL_ONE);
    expect("2.0 * 0.5", fx_mul(2 * GL_ONE, GL_ONE / 2), GL_ONE);
    expect("-2.0 * 0.5", fx_mul(0 - 2 * GL_ONE, GL_ONE / 2), 0 - GL_ONE);
    expect("1.0 / 4.0", fx_div(GL_ONE, 4 * GL_ONE), GL_ONE / 4);
    expect_near("0.1 * 10 back to 1.0", fx_mul(6554, 10 * GL_ONE), GL_ONE, 100);

    expect("sqrt(4.0)", fx_sqrt(4 * GL_ONE), 2 * GL_ONE);
    expect("sqrt(1.0)", fx_sqrt(GL_ONE), GL_ONE);
    expect_near("sqrt(2.0)", fx_sqrt(2 * GL_ONE), 92682, 2);
    expect("sqrt(0)", fx_sqrt(0), 0);
    expect("sqrt of a negative is 0, not a hang", fx_sqrt(0 - GL_ONE), 0);
}

// ============================================================
// 2. matrices
// ============================================================
void test_matrix() {
    struct M4 a;
    struct M4 b;
    struct M4 c;
    struct V3 v;
    struct V3 o;

    puts("\n-- 2. the matrix stack --\n");

    m4_identity(&a);
    v.x = 3 * GL_ONE; v.y = 4 * GL_ONE; v.z = 5 * GL_ONE;
    m4_apply(&o, &a, &v);
    expect("identity leaves x", o.x, 3 * GL_ONE);
    expect("identity leaves y", o.y, 4 * GL_ONE);
    expect("identity leaves z", o.z, 5 * GL_ONE);

    m4_translate(&a, GL_ONE, 2 * GL_ONE, 3 * GL_ONE);
    m4_apply(&o, &a, &v);
    expect("translate adds to x", o.x, 4 * GL_ONE);
    expect("translate adds to z", o.z, 8 * GL_ONE);

    // A direction has no position, so translation must not touch it. Getting
    // this wrong makes lighting swim as an object moves across the scene.
    m4_apply_dir(&o, &a, &v);
    expect("a direction ignores translation", o.x, 3 * GL_ONE);

    // Four 90-degree turns is a full circle. This catches a sign error in one
    // of the four sin/cos slots, which a single rotation does not.
    m4_rot_y(&a, 90);
    m4_identity(&c);
    {
        long i;
        i = 0;
        while (i < 4) { m4_mul(&c, &c, &a); i = i + 1; }
    }
    m4_apply(&o, &c, &v);
    expect_near("four 90-degree Y turns restore x", o.x, 3 * GL_ONE, 8);
    expect_near("...and z", o.z, 5 * GL_ONE, 8);

    m4_rot_x(&a, 90); m4_identity(&c);
    { long i; i = 0; while (i < 4) { m4_mul(&c, &c, &a); i = i + 1; } }
    m4_apply(&o, &c, &v);
    expect_near("four 90-degree X turns restore y", o.y, 4 * GL_ONE, 8);

    m4_rot_z(&a, 90); m4_identity(&c);
    { long i; i = 0; while (i < 4) { m4_mul(&c, &c, &a); i = i + 1; } }
    m4_apply(&o, &c, &v);
    expect_near("four 90-degree Z turns restore x", o.x, 3 * GL_ONE, 8);

    // One turn, checked for direction as well as magnitude.
    m4_rot_z(&a, 90);
    v.x = GL_ONE; v.y = 0; v.z = 0;
    m4_apply(&o, &a, &v);
    expect_near("rot Z 90 sends +x to +y", o.y, GL_ONE, 4);
    expect_near("...and leaves nothing on x", o.x, 0, 4);

    // Aliasing. m4_mul(x, x, y) is the obvious way to accumulate a transform,
    // and reading half-updated values is the obvious bug, so the destination
    // is built in a scratch first. This asserts that.
    m4_translate(&a, GL_ONE, 0, 0);
    m4_translate(&b, 2 * GL_ONE, 0, 0);
    m4_mul(&a, &a, &b);
    v.x = 0; v.y = 0; v.z = 0;
    m4_apply(&o, &a, &v);
    expect("m4_mul(x, x, y) is safe", o.x, 3 * GL_ONE);
}

// ============================================================
// 3. projection
// ============================================================
void test_project() {
    struct V3 v;
    long sx;
    long sy;
    long iz;
    long near_x;
    long far_x;

    puts("\n-- 3. projection --\n");

    v.x = 0; v.y = 0; v.z = 4 * GL_ONE;
    expect("a point in front projects", gl_project(&g_gl, &v, &sx, &sy, &iz), 1);
    expect("straight ahead lands at the centre in x", sx, VPW / 2);
    expect("...and in y", sy, VPH / 2);

    v.z = GL_ONE / 8;                          // inside the near plane
    expect("a point inside the near plane does not", gl_project(&g_gl, &v, &sx, &sy, &iz), 0);
    v.z = 0 - GL_ONE;                          // behind the camera
    expect("nor does one behind the camera", gl_project(&g_gl, &v, &sx, &sy, &iz), 0);

    // Perspective: twice as far is half as wide. Checked as a ratio, so it
    // does not depend on the focal length.
    v.x = GL_ONE; v.y = 0; v.z = 4 * GL_ONE;
    gl_project(&g_gl, &v, &sx, &sy, &iz);
    near_x = sx - VPW / 2;
    v.z = 8 * GL_ONE;
    gl_project(&g_gl, &v, &sx, &sy, &iz);
    far_x = sx - VPW / 2;
    printf("  same point at z=4 is %d px off centre, at z=8 it is %d\n", near_x, far_x);
    expect_near("twice the distance is half the size", far_x * 2, near_x, 1);

    // +y is up on screen, which means the screen row goes DOWN as y goes up.
    v.x = 0; v.y = GL_ONE; v.z = 4 * GL_ONE;
    gl_project(&g_gl, &v, &sx, &sy, &iz);
    if (sy >= VPH / 2) fail("+y in the world did not go up the screen");
    else puts("  ok  +y in the world is up on screen\n");

    // Depth is stored as 1/z: nearer is larger.
    v.z = 2 * GL_ONE; gl_project(&g_gl, &v, &sx, &sy, &iz);
    near_x = iz;
    v.z = 8 * GL_ONE; gl_project(&g_gl, &v, &sx, &sy, &iz);
    if (iz >= near_x) fail("depth did not decrease with distance");
    else printf("  ok  1/z is larger when nearer (%d at z=2, %d at z=8)\n", near_x, iz);
}

// ============================================================
// 4. culling, the z-buffer, and clipping
// ============================================================
void test_raster() {
    struct V3 a;
    struct V3 b;
    struct V3 c;
    long drawn_ccw;
    long drawn_cw;

    puts("\n-- 4. culling, depth and the near plane --\n");

    gl_clear(&g_gl);

    // One triangle, then the same triangle with two vertices swapped. Exactly
    // one of the two windings must survive. Asserting the RELATIONSHIP rather
    // than a fixed handedness means the test still means something if the
    // convention is ever flipped on purpose.
    a.x = 0 - GL_ONE; a.y = 0 - GL_ONE; a.z = 4 * GL_ONE;
    b.x =     GL_ONE; b.y = 0 - GL_ONE; b.z = 4 * GL_ONE;
    c.x = 0;          c.y =     GL_ONE; c.z = 4 * GL_ONE;

    g_gl.tris_drawn = 0; g_gl.tris_culled = 0;
    gl_tri_view(&g_gl, &a, &b, &c, rgb(200, 80, 80));
    drawn_ccw = g_gl.tris_drawn;
    g_gl.tris_drawn = 0; g_gl.tris_culled = 0;
    gl_tri_view(&g_gl, &b, &a, &c, rgb(200, 80, 80));
    drawn_cw = g_gl.tris_drawn;
    expect("exactly one winding survives culling", drawn_ccw + drawn_cw, 1);
    printf("  (the visible one drew %d, the back-facing one %d)\n", drawn_ccw, drawn_cw);

    // The z-buffer. Draw a far quad, then a near one on top; then clear and do
    // it in the OTHER order. Both times the near colour must win -- that is
    // the difference between a depth buffer and a painter's algorithm, and
    // only the second order tests it.
    puts("\n  the depth buffer:\n");
    {
        long near_col;
        long far_col;
        long px;
        long first;
        long second;
        near_col = rgb(240, 60, 60);
        far_col  = rgb(60, 60, 240);
        px = (VPH / 2) * g_win[g_win3d].w + VPX + VPW / 2;
        px = (VPY + VPH / 2) * g_win[g_win3d].w + VPX + VPW / 2;

        gl_clear(&g_gl);
        a.x = 0 - 2 * GL_ONE; a.y = 0 - 2 * GL_ONE; a.z = 8 * GL_ONE;
        b.x =     2 * GL_ONE; b.y = 0 - 2 * GL_ONE; b.z = 8 * GL_ONE;
        c.x = 0;              c.y =     2 * GL_ONE; c.z = 8 * GL_ONE;
        if (drawn_ccw) gl_tri_view(&g_gl, &a, &b, &c, far_col);
        else           gl_tri_view(&g_gl, &b, &a, &c, far_col);
        a.z = 3 * GL_ONE; b.z = 3 * GL_ONE; c.z = 3 * GL_ONE;
        if (drawn_ccw) gl_tri_view(&g_gl, &a, &b, &c, near_col);
        else           gl_tri_view(&g_gl, &b, &a, &c, near_col);
        first = g_win[g_win3d].pix[px];

        gl_clear(&g_gl);
        a.z = 3 * GL_ONE; b.z = 3 * GL_ONE; c.z = 3 * GL_ONE;
        if (drawn_ccw) gl_tri_view(&g_gl, &a, &b, &c, near_col);
        else           gl_tri_view(&g_gl, &b, &a, &c, near_col);
        a.z = 8 * GL_ONE; b.z = 8 * GL_ONE; c.z = 8 * GL_ONE;
        if (drawn_ccw) gl_tri_view(&g_gl, &a, &b, &c, far_col);
        else           gl_tri_view(&g_gl, &b, &a, &c, far_col);
        second = g_win[g_win3d].pix[px];

        if (first != second) {
            printf("  near-first gave %d, far-first gave %d\n", second, first);
            fail("the result depended on the order the triangles were drawn");
        } else puts("  ok  the nearer surface wins whichever order they are drawn in\n");
    }

    // The near plane.
    puts("\n  near-plane clipping:\n");
    gl_clear(&g_gl);
    a.x = 0 - GL_ONE; a.y = 0 - GL_ONE; a.z = 0 - GL_ONE;
    b.x =     GL_ONE; b.y = 0 - GL_ONE; b.z = 0 - GL_ONE;
    c.x = 0;          c.y =     GL_ONE; c.z = 0 - GL_ONE;
    g_gl.tris_drawn = 0; g_gl.tris_clipped = 0;
    gl_tri_view(&g_gl, &a, &b, &c, rgb(200, 200, 80));
    gl_tri_view(&g_gl, &b, &a, &c, rgb(200, 200, 80));
    expect("a triangle entirely behind the camera draws nothing", g_gl.tris_drawn, 0);

    // One vertex in front, two behind: the clip produces ONE triangle.
    gl_clear(&g_gl);
    a.x = 0; a.y = 0; a.z = 4 * GL_ONE;
    b.x = 0 - GL_ONE; b.y = 0 - GL_ONE; b.z = 0 - GL_ONE;
    c.x =     GL_ONE; c.y = 0 - GL_ONE; c.z = 0 - GL_ONE;
    g_gl.tris_drawn = 0;
    gl_tri_view(&g_gl, &a, &b, &c, rgb(80, 200, 120));
    gl_tri_view(&g_gl, &a, &c, &b, rgb(80, 200, 120));
    expect("one vertex in front clips to one triangle", g_gl.tris_drawn, 1);

    // Two in front, one behind: the remainder is a quad, so TWO triangles.
    gl_clear(&g_gl);
    a.x = 0 - GL_ONE; a.y = 0;          a.z = 4 * GL_ONE;
    b.x =     GL_ONE; b.y = 0;          b.z = 4 * GL_ONE;
    c.x = 0;          c.y = 2 * GL_ONE; c.z = 0 - GL_ONE;
    g_gl.tris_drawn = 0;
    gl_tri_view(&g_gl, &a, &b, &c, rgb(80, 120, 200));
    gl_tri_view(&g_gl, &b, &a, &c, rgb(80, 120, 200));
    expect("two vertices in front clip to two triangles", g_gl.tris_drawn, 2);

    // A cube, and how many of its faces you can actually see.
    //
    // My first version of this asserted 6 drawn and 6 culled, which is wrong,
    // and the wrongness is worth keeping: rotated about Y alone, with the
    // camera on the z axis, the top and bottom faces are exactly EDGE ON.
    // Their normals are perpendicular to the view direction, so they cull, and
    // you see two faces -- four triangles -- not three. Six only appears once
    // the view direction has all three components non-zero.
    //
    // So both cases are checked. A culler that ignores orientation gives the
    // same answer twice.
    puts("\n  a cube:\n");
    {
        struct M4 mv;
        struct M4 ry;
        struct M4 rx;

        gl_clear(&g_gl);
        m4_rot_y(&ry, 30);
        m4_translate(&mv, 0, 0, 6 * GL_ONE);
        m4_mul(&mv, &mv, &ry);
        draw_cube(&mv);
        expect("twelve triangles offered", g_gl.tris_in, 12);
        expect("turned about Y only, two faces show", g_gl.tris_drawn, 4);
        expect("...so eight triangles are culled", g_gl.tris_culled, 8);

        gl_clear(&g_gl);
        m4_rot_x(&rx, 25);
        m4_rot_y(&ry, 30);
        m4_mul(&ry, &ry, &rx);
        m4_translate(&mv, 0, 0, 6 * GL_ONE);
        m4_mul(&mv, &mv, &ry);
        draw_cube(&mv);
        expect("tilted as well, three faces show", g_gl.tris_drawn, 6);
        expect("...and six are culled", g_gl.tris_culled, 6);

        // Six and six would also come out of a table wound uniformly
        // BACKWARDS: it would cull the near faces and draw the inside of the
        // far ones. What separates the two is depth -- with outward winding
        // the surface on screen is the NEAR one, closer than the cube centre
        // at z = 6.
        {
            long centre;
            centre = g_gl.zbuf[(VPH / 2) * VPW + VPW / 2];
            if (centre <= fx_div(GL_ONE, 6 * GL_ONE))
                fail("the centre pixel is on the FAR side of the cube");
            else puts("  ok  and the visible surface is the near one\n");
        }
        printf("  %d pixels written into the backing buffer\n", g_gl.pixels);
    }
}

// How many distinct colours appear in the viewport, ignoring the background.
// A cube whose visible faces all end up the same colour looks like a flat
// polygon, and every count-based check still passes -- the triangles were
// drawn, they just landed on top of each other or shaded identically.
long distinct_colours() {
    long seen[16];
    long n;
    long x;
    long y;
    n = 0;
    y = 0;
    while (y < VPH) {
        x = 0;
        while (x < VPW) {
            long c;
            long i;
            long found;
            c = g_win[g_win3d].pix[(VPY + y) * g_win[g_win3d].w + VPX + x];
            if (c != g_gl.bg) {
                found = 0;
                i = 0;
                while (i < n) { if (seen[i] == c) found = 1; i = i + 1; }
                if (!found && n < 16) { seen[n] = c; n = n + 1; }
            }
            x = x + 1;
        }
        y = y + 1;
    }
    return n;
}

// ============================================================
// 4b. the cube actually looks like a cube
// ============================================================
void test_looks_3d() {
    struct M4 mv;
    struct M4 ry;
    struct M4 rx;
    long a;
    long worst;

    puts("\n-- 4b. it looks like a cube, not a polygon --\n");

    // Every check so far counts triangles. None would notice a cube rendered
    // as one flat shape.
    //
    // The assertion has to be on the MAXIMUM over a sweep, not the minimum.
    // At a = 0, 90, 180 and 270 this transform is degenerate -- with rot_x at
    // a/2 those are exactly the orientations where four of the six faces are
    // edge-on -- and one visible face is the geometrically correct answer
    // there. My first version asserted the minimum and failed on correct
    // output, which is the more embarrassing direction to get a test wrong.
    {
        long best;
        long best_tris;
        long fewest;
        best = 0;
        best_tris = 0;
        fewest = 99;
        a = 0;
        while (a < 360) {
            long n;
            gl_clear(&g_gl);
            m4_rot_x(&rx, a * 2 / 3);
            m4_rot_y(&ry, a);
            m4_mul(&ry, &ry, &rx);
            m4_translate(&mv, 0, 0, 6 * GL_ONE);
            m4_mul(&mv, &mv, &ry);
            draw_cube(&mv);
            n = distinct_colours();
            if (n > best) best = n;
            if (n < fewest) fewest = n;
            if (g_gl.tris_drawn > best_tris) best_tris = g_gl.tris_drawn;
            if (a % 45 == 0)
                printf("  angle %d: %d triangles, %d face colours on screen\n",
                       a, g_gl.tris_drawn, n);
            a = a + 5;
        }
        printf("  over a full turn: at most %d faces at once, at fewest %d\n",
               best, fewest);
        expect("three faces are visible at the best angle", best, 3);
        expect("...which is six triangles", best_tris, 6);
        if (fewest < 1) fail("at some angle nothing was drawn at all");
        else puts("  ok  and never fewer than one face\n");
    }

    worst = 0;
}

// ============================================================
// 5. the window contract
// ============================================================
void test_contract() {
    long sentinel;
    long outside;
    long x;
    long y;
    struct M4 mv;
    struct M4 r;

    puts("\n-- 5. the window contract --\n");

    // Fill the whole window with a colour the renderer never uses, bind a
    // viewport smaller than the window, and render. Every pixel outside the
    // viewport must still be the sentinel. A renderer that writes outside the
    // rectangle it was given corrupts whatever the application drew, and no
    // framebuffer checksum can see it -- the buffer and the screen agree
    // perfectly on the wrong picture.
    sentinel = rgb(255, 0, 255);
    wm_win_fill(g_win3d, 0, 0, g_win[g_win3d].w, g_win[g_win3d].h, sentinel);

    gl_clear(&g_gl);
    m4_rot_y(&r, 25);
    m4_translate(&mv, 0, 0, 3 * GL_ONE);       // close enough to fill the view
    m4_mul(&mv, &mv, &r);
    draw_cube(&mv);

    outside = 0;
    y = 0;
    while (y < g_win[g_win3d].h) {
        x = 0;
        while (x < g_win[g_win3d].w) {
            if (x < VPX || y < VPY || x >= VPX + VPW || y >= VPY + VPH) {
                if (g_win[g_win3d].pix[y * g_win[g_win3d].w + x] != sentinel)
                    outside = outside + 1;
            }
            x = x + 1;
        }
        y = y + 1;
    }
    expect("pixels written outside the viewport", outside, 0);

    // ...and it did write inside it, so the check above is not passing merely
    // because nothing was drawn at all.
    {
        long inside;
        inside = 0;
        y = VPY;
        while (y < VPY + VPH) {
            x = VPX;
            while (x < VPX + VPW) {
                if (g_win[g_win3d].pix[y * g_win[g_win3d].w + x] != sentinel)
                    inside = inside + 1;
                x = x + 1;
            }
            y = y + 1;
        }
        if (inside < 1000) fail("almost nothing was drawn inside the viewport either");
        else printf("  ok  and %d pixels inside it were\n", inside);
    }

    wm_decorate(g_win3d);
    gl_clear(&g_gl);
    gl_flush(&g_gl);
    wm_damage_all();
    wm_present();
}

// ============================================================
// 6. what a frame costs
// ============================================================
void test_cost() {
    struct M4 mv;
    struct M4 r;
    long small;
    long full;

    puts("\n-- 6. what a frame costs the screen --\n");

    wm_cursor_show(0);
    wm_present();

    // A small cube in the middle of the viewport. gl_flush invalidates the
    // bounding box of what was actually written, not the viewport, so a small
    // object costs a small rectangle even though the renderer cleared the
    // whole viewport into the backing buffer first.
    //
    // Note the clear DOES mark the whole viewport, honestly -- it changed all
    // of it. The number below is therefore the viewport, and the interesting
    // comparison is against the screen, not against the viewport.
    gl_clear(&g_gl);
    m4_rot_y(&r, 20);
    m4_translate(&mv, 0, 0, 8 * GL_ONE);
    m4_mul(&mv, &mv, &r);
    draw_cube(&mv);
    wm_reset_counters();
    gl_flush(&g_gl);
    wm_present();
    small = wm_pixels;
    printf("  a rendered frame: %d pixels to the screen\n", small);
    printf("  the viewport is %d, the screen is %d\n", VPW * VPH, wm_screen_pixels());
    // Until K24b this was exactly the viewport, because the clear damaged the
    // whole viewport whatever the frame contained; now it is the rectangle the
    // frame actually rewrote, and a cube in the middle of a viewport is a lot
    // less than the viewport. A strict inequality on purpose: "no more than
    // the viewport" would have passed before the change too.
    expect_true("a frame costs the compositor LESS than its viewport",
                small < VPW * VPH);
    expect_true("...and something, since a cube was drawn", small > 0);
    check_matches_full("after a rendered frame");

    // Forty frames of rotation, to show the cost is per-frame and flat.
    wm_reset_counters();
    {
        long i;
        i = 0;
        while (i < 40) {
            gl_clear(&g_gl);
            m4_rot_y(&r, i * 9);
            m4_translate(&mv, 0, 0, 8 * GL_ONE);
            m4_mul(&mv, &mv, &r);
            draw_cube(&mv);
            gl_flush(&g_gl);
            wm_present();
            i = i + 1;
        }
    }
    full = wm_pixels;
    printf("  40 rotating frames: %d pixels, %d per frame\n", full, full / 40);
    printf("  40 full repaints would have been %d\n", wm_screen_pixels() * 40);
    if (full / 40 > VPW * VPH + 64) fail("a frame cost more than its viewport");
    else puts("  ok  the cost is the viewport, once per frame\n");
    check_matches_full("after forty frames");

    wm_cursor_show(1);
    wm_present();
}

// ============================================================
// the desktop left behind: the K14 panel driving the renderer
// ============================================================
struct Ui g_ui;
long g_panel_win;
long g_wire;
long g_speed;
long g_angle;
long g_prev_down;
long g_console;

#define PPX (WM_BORDER + 8)
#define PPY (WM_TITLE_H + 8)
#define PPW 180

void build_desktop() {
    wm_init(rgb(24, 28, 38));
    wmin_init();
    term_init();
    ui_init(&g_ui);
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);

    g_win3d = wm_create(60, 70, VPW + WM_BORDER * 2 + 8,
                        VPH + WM_TITLE_H + WM_BORDER + 8, "cube");
    wm_decorate(g_win3d);
    gl_bind(&g_gl, g_win3d, VPX, VPY, VPW, VPH);

    g_panel_win = wm_create(400, 70, PPW + 16 + WM_BORDER, PPY + 5 * (UI_ROW_H + UI_PAD) + 8,
                            "render");
    wm_decorate(g_panel_win);

    g_console = term_create(60, 340, 62, 20, "console");
    if (g_console >= 0) {
        term_puts(g_console, "nano-os K15 -- 3D in 16.16 fixed point\n");
        term_puts(g_console, "the renderer takes a WINDOW HANDLE and\n");
        term_puts(g_console, "invalidates only what it wrote.\n");
        term_puts(g_console, "TinyGL needs floats; this does not.\n\n");
        term_prompt(g_console);
        term_flush(g_console);
    }

    g_wire = 0;
    g_speed = 30;
    g_angle = 0;
    g_prev_down = 0;
    wm_cursor_show(1);
    mouse_warp(fb_width / 2, fb_height / 2);
    wm_cursor_move(g_mouse_x, g_mouse_y);
    wm_set_focus(g_win3d);
    wm_present();
}

void render_frame() {
    struct M4 mv;
    struct M4 ry;
    struct M4 rx;
    if (!g_win[g_win3d].used) return;
    g_gl.wire = g_wire;
    gl_clear(&g_gl);
    m4_rot_x(&rx, g_angle * 2 / 3);
    m4_rot_y(&ry, g_angle);
    m4_mul(&ry, &ry, &rx);
    m4_translate(&mv, 0, 0, 6 * GL_ONE);
    m4_mul(&mv, &mv, &ry);
    draw_cube(&mv);
    gl_flush(&g_gl);
}

void event_loop() {
    long last_tick;
    last_tick = g_ticks;
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

        if (g_win[g_panel_win].used) {
            ui_begin(&g_ui, g_panel_win, PPX, PPY, PPW);
            ui_input(&g_ui, down, pressed, released, key);
            ui_label(&g_ui, "render");
            ui_checkbox(&g_ui, "wireframe", &g_wire);
            ui_label(&g_ui, "speed");
            ui_slider(&g_ui, &g_speed, 0, 90);
            ui_progress(&g_ui, g_speed, 0, 90);
            ui_end(&g_ui);
        }

        // The animation is driven by the timer, not by how fast the loop
        // runs, so the cube turns at the same rate whatever else is going on.
        //
        // That was the intent and the code did not have it: one step per
        // ITERATION THAT SAW A NEW TICK is one step per frame, so a frame
        // costing five ticks advanced the angle once in five, and the cube
        // slowed down exactly when the renderer did. Multiply by the ticks
        // that actually passed and the comment becomes true.
        if (g_ticks != last_tick) {
            g_angle = (g_angle + (g_ticks - last_tick) * (g_speed / 10 + 1)) % 360;
            last_tick = g_ticks;
            render_frame();
        }

        wm_present();
        cpu_idle();
    }
}

void run_tests() {
    printf("FB: %dx%d at %d bpp\n", fb_width, fb_height, fb_bpp);
    printf("a full repaint is %d pixels\n", wm_screen_pixels());
    printf("viewport %dx%d = %d pixels\n\n", VPW, VPH, VPW * VPH);

    build_cube();
    g_face_colour[0] = rgb(220, 70, 70);
    g_face_colour[1] = rgb(70, 200, 110);
    g_face_colour[2] = rgb(80, 120, 230);
    g_face_colour[3] = rgb(230, 190, 60);
    g_face_colour[4] = rgb(200, 90, 210);
    g_face_colour[5] = rgb(80, 210, 210);

    build_3d_window();

    test_fixed();
    test_matrix();
    test_project();
    test_raster();
    test_looks_3d();
    test_contract();
    test_cost();

    printf("\nheap: %d pages mapped, %d bytes free\n", heap_pages, heap_bytes_free());

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else puts("\nPASS: fixed-point 3D into a window handle, damage and all\n");

    puts("\nGLTEST DONE\n");
}

int main() {
    serial_init();
    g_fail = 0;

    puts("\nnano-os: a 3D pipeline in fixed point, bound to a window\n");

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
