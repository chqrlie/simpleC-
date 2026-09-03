// gltex.c — textures: upload, sampling, wrapping, and perspective correction.
//
// K16 gave the renderer an OpenGL-shaped front end. This adds the one feature
// that separates "draws coloured triangles" from "draws a scene": a texture,
// mapped correctly.
//
// The word doing the work there is CORRECTLY. Interpolating texture
// coordinates linearly across a triangle is easy, three additions per pixel,
// and it is wrong -- famously so, because it is what gave the PlayStation 1 its
// swimming, wobbling floors. s and t are not linear in screen space. s/z, t/z
// and 1/z are, so those get interpolated and the divide happens per pixel.
//
// Two checks make that claim stand up, and neither of them is a screenshot:
//
//   1. THE DIAGONAL INVARIANT. A quad can be split into two triangles along
//      either diagonal. Perspective-correct mapping gives the same picture
//      both ways, because both triangulations describe the same projective
//      map. Affine mapping does not -- the seam moves. So: render both, count
//      the pixels that differ.
//
//   2. AGAINST AN ANALYTIC ANSWER. For a floor plane and a camera at the
//      origin, the texture coordinate under any given screen pixel has a
//      closed form: intersect the ray with the plane. The texture is built so
//      that its texels encode their own coordinates, so the rendered pixel can
//      be read back and compared to a number the renderer had nothing to do
//      with. That is the same shape of test as checking the frustum planes
//      against the rasteriser in K16, and it is worth more than either the
//      screenshot or the invariant.
//
// nano-kernel.h first, as in every graphics image: it only mirrors console
// output onto the framebuffer if NANO_FB_H is already defined, and here it
// must not, because this image reads the framebuffer back.
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

// ============================================================
// the viewport and the textures
// ============================================================

#define VPX  (WM_BORDER + 4)
#define VPY  (WM_TITLE_H + 4)
#define VPW  256
#define VPH  192

struct GLCtx g_gl;
long g_win3d;

// A 16x16 texture whose texels ENCODE THEIR OWN COORDINATES: texel (u,v) is
// red = u*17, green = v*17. 17 because 15*17 = 255, so the whole byte range is
// used and every texel is distinguishable.
//
// This is what makes the analytic test possible. Read a pixel off the screen,
// divide by 17, and you know exactly which texel the renderer chose -- without
// having to trust anything the renderer says about itself.
#define COORD_N 16
long g_coord_tex[COORD_N * COORD_N];
long g_coord_name;

void build_coord_texture() {
    long v;
    v = 0;
    while (v < COORD_N) {
        long u;
        u = 0;
        while (u < COORD_N) {
            g_coord_tex[v * COORD_N + u] = rgb(u * 17, v * 17, 0);
            u = u + 1;
        }
        v = v + 1;
    }
}

// A checkerboard, for the eye and for the demo.
#define CHECK_N 32
long g_check_tex[CHECK_N * CHECK_N];
long g_check_name;

void build_check_texture() {
    long v;
    v = 0;
    while (v < CHECK_N) {
        long u;
        u = 0;
        while (u < CHECK_N) {
            long on;
            on = ((u >> 2) + (v >> 2)) & 1;
            // A grid line every four texels on top of the checks, so that a
            // mis-mapped texture is obvious to the eye as well as to the tests.
            if ((u & 3) == 0 || (v & 3) == 0)
                g_check_tex[v * CHECK_N + u] = rgb(40, 40, 48);
            else
                g_check_tex[v * CHECK_N + u] = on ? rgb(220, 210, 190)
                                                  : rgb(150, 90, 70);
            u = u + 1;
        }
        v = v + 1;
    }
}

// Read a pixel out of the window's backing buffer, in VIEWPORT coordinates.
long vp_pixel(long x, long y) {
    return g_win[g_win3d].pix[(VPY + y) * g_win[g_win3d].w + VPX + x];
}

// A viewport-sized scratch buffer, from the heap rather than a global array.
// A global would be VPW*VPH*8 = 393 KB of zeroes written into the kernel
// BINARY -- the exact trap K8 hit, where --kernel emitted 19 MB of zero bytes
// for uninitialised globals.
long *g_scratch;

long win_hash(long x, long y, long w, long h) {
    long hash;
    long j;
    hash = 5381;
    j = 0;
    while (j < h) {
        long i;
        i = 0;
        while (i < w) {
            hash = ((hash * 33) + vp_pixel(x + i, y + j)) & 0xFFFFFFFF;
            i = i + 1;
        }
        j = j + 1;
    }
    return hash;
}

void build_3d_window() {
    wm_init(rgb(24, 28, 38));
    wmin_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);
    gl_tex_init();
    g_win3d = wm_create(80, 60, VPW + WM_BORDER * 2 + 8,
                        VPH + WM_TITLE_H + WM_BORDER + 8, "textured");
    wm_decorate(g_win3d);
    if (!gl_bind(&g_gl, g_win3d, VPX, VPY, VPW, VPH)) fail("gl_bind failed");
    gl_state_init(&g_gls, &g_gl);
    wm_present();
}

// ============================================================
// 1. upload, and a texel is where it says it is
// ============================================================

// A quad square-on to the camera at a constant depth, filling a known part of
// the viewport. At constant z there is no perspective at all, so any mapping
// gets this right -- which is the point: it isolates ORIENTATION from
// correction. A swapped s and t, or a flipped row order, shows up here and
// nowhere else.
void draw_flat_quad(long z, long srep) {
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glColor3ub(&g_gls, 255, 255, 255);
    glDisable(&g_gls, GL_LIGHTING);
    glEnable(&g_gls, GL_TEXTURE_2D);
    glBindTexture(&g_gls, GL_TEXTURE_2D, g_coord_name);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);

    glBegin(&g_gls, GL_QUADS);
    glTexCoord2x(&g_gls, 0, 0);
    glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, z);
    glTexCoord2x(&g_gls, 0, srep);
    glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE, z);
    glTexCoord2x(&g_gls, srep, srep);
    glVertex3x(&g_gls, GL_ONE, GL_ONE, z);
    glTexCoord2x(&g_gls, srep, 0);
    glVertex3x(&g_gls, GL_ONE, 0 - GL_ONE, z);
    glEnd(&g_gls);
}

// The texel a screen pixel actually shows, decoded from the coordinate
// texture. Returns 0 on a background pixel.
long read_uv(long x, long y, long *u, long *v) {
    long c;
    c = vp_pixel(x, y);
    if (c == g_gl.bg) return 0;
    *u = ((c >> 16) & 255) / 17;
    *v = ((c >> 8) & 255) / 17;
    return 1;
}

void test_upload() {
    long u;
    long v;
    long ok;

    puts("-- 1. a texel is where it says it is --\n");

    expect_true("a texture name was allocated", g_coord_name >= 0);

    // Non-power-of-two is REJECTED, not rounded. Rounding gives a texture
    // that samples wrongly everywhere, which reads as a bug in the mapping
    // rather than a bug in the upload.
    {
        long name;
        name = glGenTexture();
        glBindTexture(&g_gls, GL_TEXTURE_2D, name);
        expect("a 17-wide texture is refused", glTexImage2D(&g_gls, 17, 16, g_coord_tex), 0);
        expect("...and a 16x16 one is accepted", glTexImage2D(&g_gls, 16, 16, g_coord_tex), 1);
        g_tex[name].used = 0;
    }

    draw_flat_quad(3 * GL_ONE, GL_ONE);

    // The quad spans the middle of the viewport. Its top-left corner in
    // texture space is (0,1) -- t grows upward with y, and y grows upward in
    // the world while the screen row index grows downward.
    ok = read_uv(VPW / 2, VPH / 2, &u, &v);
    expect_true("the middle of the quad is textured", ok);

    // Sample four corners of the quad, well inside it, and check each lands in
    // the right quadrant of the texture. Four samples and not one, because a
    // transposed mapping (s and t swapped) is symmetric about the diagonal and
    // a single centre sample cannot see it.
    {
        long qx;
        long qy;
        long lu; long lv; long ru; long rv;
        // The quad is 2 units wide at z=3, focal = VPW, so it is
        // 2/3 * VPW pixels across, centred.
        qx = VPW / 2 - VPW / 4;
        qy = VPH / 2 - VPH / 5;
        read_uv(qx, qy, &lu, &lv);
        read_uv(VPW - qx, qy, &ru, &rv);
        expect_true("s grows to the RIGHT across the screen", ru > lu);
        expect_true("...and t is the same along a horizontal line",
                    lv == rv || (lv - rv) * (lv - rv) <= 1);

        read_uv(qx, qy, &lu, &lv);
        read_uv(qx, VPH - qy, &ru, &rv);
        expect_true("t grows UPWARD in the world, so downward on the screen",
                    rv < lv);
        expect_true("...and s is the same down a vertical line",
                    lu == ru || (lu - ru) * (lu - ru) <= 1);
    }

    // GL_REPEAT. Four repeats across the quad means the texel coordinate
    // sweeps 0..15 four times, so a point and the same point one period to the
    // right show the same texel.
    //
    // The period is MEASURED, not calculated. My first attempt worked it out
    // from the viewport width and got 32 pixels when the quad is 170 wide, so
    // a period is 42.5 -- the test failed on correct output, which is the
    // second time on this project that a number I derived on paper was the
    // wrong half of the comparison. Scan for the quad's actual extent.
    {
        long xa;
        long xb;
        long period;
        long x;
        long u0; long v0; long u1; long v1; long u2; long v2;
        draw_flat_quad(3 * GL_ONE, 4 * GL_ONE);
        xa = -1;
        xb = -1;
        x = 0;
        while (x < VPW) {
            if (vp_pixel(x, VPH / 2) != g_gl.bg) {
                if (xa < 0) xa = x;
                xb = x;
            }
            x = x + 1;
        }
        period = (xb - xa + 1) / 4;
        printf("  the quad spans x %d..%d, so one repeat is %d pixels\n",
               xa, xb, period);
        expect_true("the quad is wide enough for four repeats to mean anything",
                    period > 20);
        read_uv(xa + 6, VPH / 2, &u0, &v0);
        read_uv(xa + 6 + period, VPH / 2, &u1, &v1);
        read_uv(xa + 6 + period * 2, VPH / 2, &u2, &v2);
        printf("  one, two and three periods along: u %d, %d, %d\n", u0, u1, u2);
        expect_near("the texture repeats: same texel one period along", u1, u0, 1);
        expect_near("...and two periods along", u2, u0, 1);
    }
}

// ============================================================
// 2. the floor, and the analytic answer
// ============================================================

// A floor plane at y = -1, spanning x in [-4,4] and z in [2,12], textured so
// that s runs across x and t runs along z.
#define FLOOR_Y  (0 - GL_ONE)
#define FLOOR_X  (4 * GL_ONE)
#define FLOOR_Z0 (2 * GL_ONE)
#define FLOOR_Z1 (12 * GL_ONE)

void floor_vertex(long x, long z) {
    glTexCoord2x(&g_gls, fx_div(x + FLOOR_X, 2 * FLOOR_X),
                         fx_div(z - FLOOR_Z0, FLOOR_Z1 - FLOOR_Z0));
    glVertex3x(&g_gls, x, FLOOR_Y, z);
}

// `split` chooses which diagonal the quad is cut along. Both cover exactly the
// same quad; a perspective-correct renderer draws the same picture either way.
void draw_floor(long split) {
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glColor3ub(&g_gls, 255, 255, 255);
    glDisable(&g_gls, GL_LIGHTING);
    glEnable(&g_gls, GL_TEXTURE_2D);
    glBindTexture(&g_gls, GL_TEXTURE_2D, g_coord_name);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);

    glBegin(&g_gls, GL_TRIANGLES);
    if (!split) {
        floor_vertex(0 - FLOOR_X, FLOOR_Z0);
        floor_vertex(0 - FLOOR_X, FLOOR_Z1);
        floor_vertex(FLOOR_X,     FLOOR_Z1);

        floor_vertex(0 - FLOOR_X, FLOOR_Z0);
        floor_vertex(FLOOR_X,     FLOOR_Z1);
        floor_vertex(FLOOR_X,     FLOOR_Z0);
    } else {
        floor_vertex(0 - FLOOR_X, FLOOR_Z1);
        floor_vertex(FLOOR_X,     FLOOR_Z1);
        floor_vertex(FLOOR_X,     FLOOR_Z0);

        floor_vertex(0 - FLOOR_X, FLOOR_Z1);
        floor_vertex(FLOOR_X,     FLOOR_Z0);
        floor_vertex(0 - FLOOR_X, FLOOR_Z0);
    }
    glEnd(&g_gls);
}

// THE ANALYTIC ANSWER. For a screen pixel, where does the ray through it meet
// the floor, and what texture coordinate is there?
//
// The camera is at the origin looking down +z, so the ray direction through a
// pixel is (px, py, 1) with px = (sx - vw/2)/focal and py = (vh/2 - sy)/focal
// -- which is just the projection read backwards. It meets y = FLOOR_Y at
// distance d = FLOOR_Y / py, and everything else follows.
//
// Nothing in here consults the renderer. That is the entire value of it.
long floor_uv_at(long sx, long sy, long *u, long *v) {
    long px;
    long py;
    long d;
    long wx;
    long wz;
    long s;
    long t;

    px = fx_div(fx_from_int(sx - VPW / 2), fx_from_int(g_gl.focal));
    py = fx_div(fx_from_int(VPH / 2 - sy), fx_from_int(g_gl.focal));
    if (py >= 0) return 0;                     // at or above the horizon
    d = fx_div(FLOOR_Y, py);
    if (d < FLOOR_Z0 || d > FLOOR_Z1) return 0;
    wz = d;
    wx = fx_mul(px, d);
    if (wx < 0 - FLOOR_X || wx > FLOOR_X) return 0;

    s = fx_div(wx + FLOOR_X, 2 * FLOOR_X);
    t = fx_div(wz - FLOOR_Z0, FLOOR_Z1 - FLOOR_Z0);
    *u = ((s * COORD_N) >> GL_FRAC) & (COORD_N - 1);
    *v = ((t * COORD_N) >> GL_FRAC) & (COORD_N - 1);
    return 1;
}

void test_perspective() {
    long checked;
    long wrong;
    long worst;
    long y;

    puts("\n-- 2. perspective correction, against an analytic answer --\n");

    draw_floor(0);

    checked = 0;
    wrong = 0;
    worst = 0;
    y = 0;
    while (y < VPH) {
        long x;
        x = 0;
        while (x < VPW) {
            long eu; long ev;
            long gu; long gv;
            if (floor_uv_at(x, y, &eu, &ev) && read_uv(x, y, &gu, &gv)) {
                long du;
                long dv;
                // Compare with wrap: texel 0 and texel 15 are adjacent.
                du = gu - eu; if (du < 0) du = 0 - du;
                if (du > COORD_N / 2) du = COORD_N - du;
                dv = gv - ev; if (dv < 0) dv = 0 - dv;
                if (dv > COORD_N / 2) dv = COORD_N - dv;
                checked = checked + 1;
                if (du > worst) worst = du;
                if (dv > worst) worst = dv;
                // One texel of slack: the analytic answer uses the pixel's
                // nominal centre and the rasteriser samples on the integer
                // grid, so half a pixel of disagreement is expected and is not
                // a mapping error.
                if (du > 1 || dv > 1) wrong = wrong + 1;
            }
            x = x + 1;
        }
        y = y + 1;
    }

    printf("  %d floor pixels checked against the closed form\n", checked);
    printf("  %d disagreed by more than one texel; worst was %d\n", wrong, worst);
    expect_true("enough of the floor was drawn to mean something", checked > 4000);
    expect("the rendered texture matches the analytic one everywhere", wrong, 0);

    // THE DIAGONAL INVARIANT. Two triangulations of one quad describe the same
    // projective map, so a perspective-correct renderer draws the same
    // picture. An affine one does not -- the texture kinks along whichever
    // diagonal was used, and swapping the diagonal moves the kink.
    {
        long a;
        long b;
        long diff;
        long covered;
        long j;

        draw_floor(0);
        a = win_hash(0, 0, VPW, VPH);
        draw_floor(1);
        b = win_hash(0, 0, VPW, VPH);

        // Count the pixels that differ, rather than requiring equal hashes:
        // the two triangulations round differently on the shared edge, so a
        // handful of pixels legitimately differ. An affine renderer differs
        // over the whole quad.
        diff = 0;
        covered = 0;
        draw_floor(0);
        j = 0;
        while (j < VPH) {
            long i;
            i = 0;
            while (i < VPW) {
                g_scratch[j * VPW + i] = vp_pixel(i, j);
                i = i + 1;
            }
            j = j + 1;
        }
        draw_floor(1);
        j = 0;
        while (j < VPH) {
            long i;
            i = 0;
            while (i < VPW) {
                long p;
                p = vp_pixel(i, j);
                if (p != g_gl.bg || g_scratch[j * VPW + i] != g_gl.bg) {
                    covered = covered + 1;
                    if (p != g_scratch[j * VPW + i]) diff = diff + 1;
                }
                i = i + 1;
            }
            j = j + 1;
        }
        printf("  the two triangulations: %d covered pixels, %d differ (%d per mille)\n",
               covered, diff, covered ? diff * 1000 / covered : 0);
        expect_true("the quad actually covered a lot of the viewport",
                    covered > 4000);
        // Three per cent, not one. The pixels that legitimately differ are the
        // ones on the shared diagonal, where the two triangulations round the
        // edge test differently -- about seven per mille here. An affine
        // renderer differs across the whole quad, so this discriminates
        // enormously either way and there is no reason to sit on a knife edge.
        expect_true("splitting the quad the other way draws the same picture",
                    diff * 33 < covered);
        expect_true("...and the two hashes are at least both non-empty",
                    a != 0 && b != 0);
    }
}

// ============================================================
// 3. the clipper carries texture coordinates
// ============================================================

// The bug struct Vtx exists to prevent: the near-plane clipper invents a
// vertex partway along an edge, and if it interpolates the position but not
// the texture coordinate, the new vertex gets whatever was in the slot before.
//
// That only shows when you walk INTO something, which is exactly when nobody
// is looking at the far corner of the screen. So it gets a test: the same
// floor, moved so its near edge is BEHIND the camera, checked against the same
// analytic answer.
void draw_floor_clipped() {
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glColor3ub(&g_gls, 255, 255, 255);
    glDisable(&g_gls, GL_LIGHTING);
    glEnable(&g_gls, GL_TEXTURE_2D);
    glBindTexture(&g_gls, GL_TEXTURE_2D, g_coord_name);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);

    glBegin(&g_gls, GL_TRIANGLES);
    // Same plane, but the near edge is at z = -6, well behind the near plane.
    glTexCoord2x(&g_gls, 0, fx_div(0 - 6 * GL_ONE - FLOOR_Z0, FLOOR_Z1 - FLOOR_Z0));
    glVertex3x(&g_gls, 0 - FLOOR_X, FLOOR_Y, 0 - 6 * GL_ONE);
    glTexCoord2x(&g_gls, 0, GL_ONE);
    glVertex3x(&g_gls, 0 - FLOOR_X, FLOOR_Y, FLOOR_Z1);
    glTexCoord2x(&g_gls, GL_ONE, GL_ONE);
    glVertex3x(&g_gls, FLOOR_X, FLOOR_Y, FLOOR_Z1);

    glTexCoord2x(&g_gls, 0, fx_div(0 - 6 * GL_ONE - FLOOR_Z0, FLOOR_Z1 - FLOOR_Z0));
    glVertex3x(&g_gls, 0 - FLOOR_X, FLOOR_Y, 0 - 6 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, GL_ONE);
    glVertex3x(&g_gls, FLOOR_X, FLOOR_Y, FLOOR_Z1);
    glTexCoord2x(&g_gls, GL_ONE, fx_div(0 - 6 * GL_ONE - FLOOR_Z0, FLOOR_Z1 - FLOOR_Z0));
    glVertex3x(&g_gls, FLOOR_X, FLOOR_Y, 0 - 6 * GL_ONE);
    glEnd(&g_gls);
}

void test_clipped_texture() {
    long checked;
    long wrong;
    long y;

    puts("\n-- 3. the near-plane clipper carries texture coordinates --\n");

    draw_floor_clipped();

    checked = 0;
    wrong = 0;
    y = 0;
    while (y < VPH) {
        long x;
        x = 0;
        while (x < VPW) {
            long eu; long ev;
            long gu; long gv;
            // Only the band the un-clipped floor also covers, where the
            // analytic answer is known to apply.
            if (floor_uv_at(x, y, &eu, &ev) && read_uv(x, y, &gu, &gv)) {
                long du; long dv;
                du = gu - eu; if (du < 0) du = 0 - du;
                if (du > COORD_N / 2) du = COORD_N - du;
                dv = gv - ev; if (dv < 0) dv = 0 - dv;
                if (dv > COORD_N / 2) dv = COORD_N - dv;
                checked = checked + 1;
                if (du > 1 || dv > 1) wrong = wrong + 1;
            }
            x = x + 1;
        }
        y = y + 1;
    }

    printf("  %d pixels of a clipped quad checked, %d wrong\n", checked, wrong);
    expect_true("the clipped quad still covers the floor band", checked > 4000);
    expect("a clipped triangle keeps its texture mapping", wrong, 0);
}

// ============================================================
// 4. modulation, and turning it off
// ============================================================
void test_modulate() {
    long plain;
    long tinted;
    long lit;
    long flat;

    puts("\n-- 4. the texture environment --\n");

    // White primary colour, no lighting: the texel arrives unchanged. That is
    // the check every other one in this file leans on.
    draw_flat_quad(3 * GL_ONE, GL_ONE);
    plain = vp_pixel(VPW / 2, VPH / 2);

    // A red primary colour: GL_MODULATE multiplies componentwise, so green and
    // blue are gone and red is scaled by 1.
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glColor3ub(&g_gls, 255, 0, 0);
    glDisable(&g_gls, GL_LIGHTING);
    glEnable(&g_gls, GL_TEXTURE_2D);
    glBindTexture(&g_gls, GL_TEXTURE_2D, g_coord_name);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    glBegin(&g_gls, GL_QUADS);
    glTexCoord2x(&g_gls, 0, 0);        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 3 * GL_ONE);
    glTexCoord2x(&g_gls, 0, GL_ONE);   glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE, 3 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, GL_ONE); glVertex3x(&g_gls, GL_ONE, GL_ONE, 3 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, 0);   glVertex3x(&g_gls, GL_ONE, 0 - GL_ONE, 3 * GL_ONE);
    glEnd(&g_gls);
    tinted = vp_pixel(VPW / 2, VPH / 2);

    expect("a red primary colour keeps the texel's red",
           (tinted >> 16) & 255, (plain >> 16) & 255);
    expect("...and removes its green", (tinted >> 8) & 255, 0);

    // And the other way round, which is what actually pins the red channel's
    // modulation. "Keeps its red" is also true of a renderer that ignores the
    // primary colour in red entirely -- the sabotage matrix showed exactly
    // that slipping past, caught only by an unrelated lighting check.
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glColor3ub(&g_gls, 0, 255, 255);
    glDisable(&g_gls, GL_LIGHTING);
    glEnable(&g_gls, GL_TEXTURE_2D);
    glBindTexture(&g_gls, GL_TEXTURE_2D, g_coord_name);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    glBegin(&g_gls, GL_QUADS);
    glTexCoord2x(&g_gls, 0, 0);           glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 3 * GL_ONE);
    glTexCoord2x(&g_gls, 0, GL_ONE);      glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE, 3 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, GL_ONE); glVertex3x(&g_gls, GL_ONE, GL_ONE, 3 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, 0);      glVertex3x(&g_gls, GL_ONE, 0 - GL_ONE, 3 * GL_ONE);
    glEnd(&g_gls);
    {
        long cyan;
        cyan = vp_pixel(VPW / 2, VPH / 2);
        expect("a primary colour with no red removes the texel's red",
               (cyan >> 16) & 255, 0);
        expect("...and keeps its green", (cyan >> 8) & 255, (plain >> 8) & 255);
    }

    // Lighting on. The default light points straight down the view axis, and
    // this quad faces straight down the view axis, so it is fully lit and
    // NOTHING IS DARKENED -- my first version of this check asserted a
    // darkening that correctly did not happen. Tilt the light so the surface
    // is at an angle to it.
    //
    // The invariant that actually matters is not "darker" but "scaled": GL
    // MODULATES, so every channel of every texel is multiplied by the same
    // number. Two different texels are sampled to check that, because a
    // renderer that simply replaced the texel with the lit primary colour
    // would pass a one-sample darkening test perfectly.
    {
        long lit2;
        long plain2;
        long px;
        long py;
        struct V3 save;
        save = g_gl.light;

        draw_flat_quad(3 * GL_ONE, GL_ONE);
        px = VPW / 2 - 30;
        py = VPH / 2 - 20;
        plain = vp_pixel(VPW / 2, VPH / 2);
        plain2 = vp_pixel(px, py);

        g_gl.light.x = 46341; g_gl.light.y = 0; g_gl.light.z = 0 - 46341;
        gl_clear(&g_gl);
        gl_state_init(&g_gls, &g_gl);
        glColor3ub(&g_gls, 255, 255, 255);
        glEnable(&g_gls, GL_LIGHTING);
        glEnable(&g_gls, GL_TEXTURE_2D);
        glBindTexture(&g_gls, GL_TEXTURE_2D, g_coord_name);
        glMatrixMode(&g_gls, GL_MODELVIEW);
        glLoadIdentity(&g_gls);
        glBegin(&g_gls, GL_QUADS);
        glTexCoord2x(&g_gls, 0, 0);           glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 3 * GL_ONE);
        glTexCoord2x(&g_gls, 0, GL_ONE);      glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE, 3 * GL_ONE);
        glTexCoord2x(&g_gls, GL_ONE, GL_ONE); glVertex3x(&g_gls, GL_ONE, GL_ONE, 3 * GL_ONE);
        glTexCoord2x(&g_gls, GL_ONE, 0);      glVertex3x(&g_gls, GL_ONE, 0 - GL_ONE, 3 * GL_ONE);
        glEnd(&g_gls);
        lit = vp_pixel(VPW / 2, VPH / 2);
        lit2 = vp_pixel(px, py);
        g_gl.light = save;

        printf("  unlit red %d and %d, lit %d and %d\n",
               (plain >> 16) & 255, (plain2 >> 16) & 255,
               (lit >> 16) & 255, (lit2 >> 16) & 255);
        expect_true("a light at an angle darkens a textured surface",
                    ((lit >> 16) & 255) < ((plain >> 16) & 255));
        expect_true("...and does not extinguish it", ((lit >> 16) & 255) > 0);
        expect_true("...and the two texels are still different from each other",
                    ((lit >> 16) & 255) != ((lit2 >> 16) & 255));
        // Same scale factor for both texels, to within the rounding of an
        // 8-bit channel: lit1/plain1 == lit2/plain2.
        expect_near("lighting SCALES the texture rather than replacing it",
                    (((lit >> 16) & 255) * 256) / (((plain >> 16) & 255) + 1),
                    (((lit2 >> 16) & 255) * 256) / (((plain2 >> 16) & 255) + 1),
                    12);
    }

    // Texturing off: back to a flat colour, and nothing of the texture left.
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glColor3ub(&g_gls, 40, 200, 90);
    glDisable(&g_gls, GL_LIGHTING);
    glDisable(&g_gls, GL_TEXTURE_2D);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    glBegin(&g_gls, GL_QUADS);
    glTexCoord2x(&g_gls, 0, 0);        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 3 * GL_ONE);
    glTexCoord2x(&g_gls, 0, GL_ONE);   glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE, 3 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, GL_ONE); glVertex3x(&g_gls, GL_ONE, GL_ONE, 3 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, 0);   glVertex3x(&g_gls, GL_ONE, 0 - GL_ONE, 3 * GL_ONE);
    glEnd(&g_gls);
    flat = vp_pixel(VPW / 2, VPH / 2);
    expect("glDisable(GL_TEXTURE_2D) gives the flat colour back",
           flat, rgb(40, 200, 90));

    // A bound name that was never uploaded must not be drawn from. It must
    // fall back to the flat colour, not read a null pointer.
    {
        long name;
        name = glGenTexture();
        gl_clear(&g_gl);
        gl_state_init(&g_gls, &g_gl);
        glColor3ub(&g_gls, 40, 200, 90);
        glDisable(&g_gls, GL_LIGHTING);
        glEnable(&g_gls, GL_TEXTURE_2D);
        glBindTexture(&g_gls, GL_TEXTURE_2D, name);
        g_tex[name].pix = 0;
        g_tex[name].w = 1; g_tex[name].h = 1;
        g_tex[name].wmask = 0; g_tex[name].hmask = 0;
        glMatrixMode(&g_gls, GL_MODELVIEW);
        glLoadIdentity(&g_gls);
        glBegin(&g_gls, GL_QUADS);
        glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 3 * GL_ONE);
        glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE, 3 * GL_ONE);
        glVertex3x(&g_gls, GL_ONE, GL_ONE, 3 * GL_ONE);
        glVertex3x(&g_gls, GL_ONE, 0 - GL_ONE, 3 * GL_ONE);
        glEnd(&g_gls);
        g_tex[name].used = 0;
        puts("  a texture bound but never uploaded did not fault\n");
    }
}

// ============================================================
// 5. depth, and what a textured frame costs
// ============================================================
void test_depth_and_cost() {
    long near_c;
    long cost;

    puts("\n-- 5. depth and cost --\n");

    // A textured quad behind an untextured one: the near surface wins,
    // whichever order they are drawn in. Twice, both orders, because a depth
    // test that is simply ignored passes one of them.
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glDisable(&g_gls, GL_LIGHTING);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);

    glEnable(&g_gls, GL_TEXTURE_2D);
    glBindTexture(&g_gls, GL_TEXTURE_2D, g_coord_name);
    glColor3ub(&g_gls, 255, 255, 255);
    glBegin(&g_gls, GL_QUADS);
    glTexCoord2x(&g_gls, 0, 0);           glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 6 * GL_ONE);
    glTexCoord2x(&g_gls, 0, GL_ONE);      glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE, 6 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, GL_ONE); glVertex3x(&g_gls, GL_ONE, GL_ONE, 6 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, 0);      glVertex3x(&g_gls, GL_ONE, 0 - GL_ONE, 6 * GL_ONE);
    glEnd(&g_gls);

    glDisable(&g_gls, GL_TEXTURE_2D);
    glColor3ub(&g_gls, 10, 10, 200);
    glBegin(&g_gls, GL_QUADS);
    glVertex3x(&g_gls, 0 - GL_ONE / 2, 0 - GL_ONE / 2, 3 * GL_ONE);
    glVertex3x(&g_gls, 0 - GL_ONE / 2, GL_ONE / 2, 3 * GL_ONE);
    glVertex3x(&g_gls, GL_ONE / 2, GL_ONE / 2, 3 * GL_ONE);
    glVertex3x(&g_gls, GL_ONE / 2, 0 - GL_ONE / 2, 3 * GL_ONE);
    glEnd(&g_gls);
    near_c = vp_pixel(VPW / 2, VPH / 2);
    expect("the nearer flat quad hides the textured one",
           near_c, rgb(10, 10, 200));

    // ...and the other way round, textured drawn second.
    gl_clear(&g_gl);
    gl_state_init(&g_gls, &g_gl);
    glDisable(&g_gls, GL_LIGHTING);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    glDisable(&g_gls, GL_TEXTURE_2D);
    glColor3ub(&g_gls, 10, 10, 200);
    glBegin(&g_gls, GL_QUADS);
    glVertex3x(&g_gls, 0 - GL_ONE / 2, 0 - GL_ONE / 2, 3 * GL_ONE);
    glVertex3x(&g_gls, 0 - GL_ONE / 2, GL_ONE / 2, 3 * GL_ONE);
    glVertex3x(&g_gls, GL_ONE / 2, GL_ONE / 2, 3 * GL_ONE);
    glVertex3x(&g_gls, GL_ONE / 2, 0 - GL_ONE / 2, 3 * GL_ONE);
    glEnd(&g_gls);
    glEnable(&g_gls, GL_TEXTURE_2D);
    glBindTexture(&g_gls, GL_TEXTURE_2D, g_coord_name);
    glColor3ub(&g_gls, 255, 255, 255);
    glBegin(&g_gls, GL_QUADS);
    glTexCoord2x(&g_gls, 0, 0);           glVertex3x(&g_gls, 0 - GL_ONE, 0 - GL_ONE, 6 * GL_ONE);
    glTexCoord2x(&g_gls, 0, GL_ONE);      glVertex3x(&g_gls, 0 - GL_ONE, GL_ONE, 6 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, GL_ONE); glVertex3x(&g_gls, GL_ONE, GL_ONE, 6 * GL_ONE);
    glTexCoord2x(&g_gls, GL_ONE, 0);      glVertex3x(&g_gls, GL_ONE, 0 - GL_ONE, 6 * GL_ONE);
    glEnd(&g_gls);
    expect("...and still does when it is drawn second",
           vp_pixel(VPW / 2, VPH / 2), rgb(10, 10, 200));

    // A textured frame still costs its viewport and nothing else. Texturing
    // adds a divide per pixel, not a pixel.
    wm_cursor_show(0);
    draw_floor(0);
    gl_flush(&g_gl);
    wm_present();
    wm_reset_counters();
    draw_floor(0);
    gl_flush(&g_gl);
    wm_present();
    cost = wm_pixels;
    printf("  a textured frame: %d pixels; the viewport is %d, the screen %d\n",
           cost, VPW * VPH, wm_screen_pixels());
    expect("a textured frame costs its viewport and no more", cost, VPW * VPH);

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

struct Ui g_ui;
struct GlView g_view;
struct Camera g_cam;
struct Frustum g_frustum;
long g_panel_win;
long g_console;
long g_texon;
long g_light;
long g_wire;
long g_spin;
long g_prev_down;
long g_stat_tris;

#define PPX (WM_BORDER + 8)
#define PPY (WM_TITLE_H + 8)
#define PPW 170

long g_quad[24];
long g_cx[8];
long g_cy[8];
long g_cz[8];

void corner(long i, long x, long y, long z) {
    g_cx[i] = x * GL_ONE; g_cy[i] = y * GL_ONE; g_cz[i] = z * GL_ONE;
}

void build_cube() {
    long k;
    corner(0, -1, -1, -1);  corner(1,  1, -1, -1);
    corner(2,  1,  1, -1);  corner(3, -1,  1, -1);
    corner(4, -1, -1,  1);  corner(5,  1, -1,  1);
    corner(6,  1,  1,  1);  corner(7, -1,  1,  1);
    k = 0;
    g_quad[k]=0; k=k+1; g_quad[k]=3; k=k+1; g_quad[k]=2; k=k+1; g_quad[k]=1; k=k+1;
    g_quad[k]=4; k=k+1; g_quad[k]=5; k=k+1; g_quad[k]=6; k=k+1; g_quad[k]=7; k=k+1;
    g_quad[k]=0; k=k+1; g_quad[k]=4; k=k+1; g_quad[k]=7; k=k+1; g_quad[k]=3; k=k+1;
    g_quad[k]=1; k=k+1; g_quad[k]=2; k=k+1; g_quad[k]=6; k=k+1; g_quad[k]=5; k=k+1;
    g_quad[k]=0; k=k+1; g_quad[k]=1; k=k+1; g_quad[k]=5; k=k+1; g_quad[k]=4; k=k+1;
    g_quad[k]=3; k=k+1; g_quad[k]=7; k=k+1; g_quad[k]=6; k=k+1; g_quad[k]=2; k=k+1;
}

// Every face gets the whole texture, in the same corner order, so the
// checkerboard reads the same way round on all six.
void gl_tex_cube() {
    long f;
    f = 0;
    while (f < 6) {
        long v;
        glBegin(&g_gls, GL_QUADS);
        v = 0;
        while (v < 4) {
            long i;
            i = g_quad[f * 4 + v];
            if (v == 0) glTexCoord2x(&g_gls, 0, 0);
            if (v == 1) glTexCoord2x(&g_gls, 0, GL_ONE);
            if (v == 2) glTexCoord2x(&g_gls, GL_ONE, GL_ONE);
            if (v == 3) glTexCoord2x(&g_gls, GL_ONE, 0);
            glVertex3x(&g_gls, g_cx[i], g_cy[i], g_cz[i]);
            v = v + 1;
        }
        glEnd(&g_gls);
        f = f + 1;
    }
}

void demo_floor() {
    long x;
    glBegin(&g_gls, GL_QUAD_STRIP);
    x = -12;
    while (x <= 12) {
        glTexCoord2x(&g_gls, (x + 12) * (GL_ONE / 2), 0);
        glVertex3x(&g_gls, x * GL_ONE, 0 - 2 * GL_ONE, 0 - 6 * GL_ONE);
        glTexCoord2x(&g_gls, (x + 12) * (GL_ONE / 2), 12 * GL_ONE);
        glVertex3x(&g_gls, x * GL_ONE, 0 - 2 * GL_ONE, 20 * GL_ONE);
        x = x + 4;
    }
    glEnd(&g_gls);
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
    gluPerspectivex(&g_gls, 60 << GL_FRAC,
                    fx_div(fx_from_int(VPW), fx_from_int(VPH)), GL_ONE / 4);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    cam_apply(&g_gls, &g_cam);
    gl_clip_matrix(&g_gls, &clip);
    gl_frustum_extract(&g_frustum, &clip);

    if (g_texon) glEnable(&g_gls, GL_TEXTURE_2D);
    else         glDisable(&g_gls, GL_TEXTURE_2D);
    glColor3ub(&g_gls, 255, 255, 255);
    glBindTexture(&g_gls, GL_TEXTURE_2D, g_check_name);
    demo_floor();

    i = 0;
    while (i < 9) {
        struct V3 at;
        at.x = ((i % 3) - 1) * 5 * GL_ONE;
        at.y = 0;
        at.z = (i / 3) * 5 * GL_ONE;
        if (gl_frustum_sphere(&g_frustum, &at, 113512) != GL_OUTSIDE) {
            glPushMatrix(&g_gls);
            glTranslatex(&g_gls, at.x, at.y, at.z);
            glRotatex(&g_gls, (g_spin + i * 20) << GL_FRAC, 0, GL_ONE, 0);
            glBindTexture(&g_gls, GL_TEXTURE_2D,
                          (i & 1) ? g_coord_name : g_check_name);
            gl_tex_cube();
            glPopMatrix(&g_gls);
        }
        i = i + 1;
    }
    g_stat_tris = g_gl.tris_drawn;
    gl_flush(&g_gl);
}

void build_desktop() {
    wm_init(rgb(20, 24, 34));
    wmin_init();
    term_init();
    ui_init(&g_ui);
    ui_glview_init(&g_view);
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);

    g_win3d = wm_create(30, 40, VPW + WM_BORDER * 2 + 8,
                        VPH + WM_TITLE_H + WM_BORDER + 8, "textured");
    wm_decorate(g_win3d);
    gl_bind(&g_gl, g_win3d, VPX, VPY, VPW, VPH);
    gl_state_init(&g_gls, &g_gl);
    g_gl.light.x = 19860; g_gl.light.y = 0 - 52961; g_gl.light.z = 0 - 33101;
    g_gl.bg = rgb(26, 32, 46);

    g_panel_win = wm_create(340, 40, PPW + 16 + WM_BORDER,
                            PPY + 6 * (UI_ROW_H + UI_PAD) + 8, "texture");
    wm_decorate(g_panel_win);

    g_console = term_create(30, 300, 66, 15, "console");
    if (g_console >= 0) {
        term_puts(g_console, "nano-os K17 -- textures, in 16.16\n");
        term_puts(g_console, "perspective-correct: s/z, t/z and 1/z are\n");
        term_puts(g_console, "  what get interpolated, then one divide\n");
        term_puts(g_console, "  per pixel. affine gives PS1 floors.\n");
        term_puts(g_console, "drag inside the view to look, WASD to move\n\n");
        term_prompt(g_console);
        term_flush(g_console);
    }

    cam_init(&g_cam);
    g_cam.eye.x = 0; g_cam.eye.y = GL_ONE; g_cam.eye.z = 0 - 8 * GL_ONE;
    g_cam.pitch = 0 - (6 << GL_FRAC);
    g_texon = 1;
    g_light = 1;
    g_wire = 0;
    g_spin = 0;
    g_prev_down = 0;

    wm_cursor_show(1);
    mouse_warp(fb_width / 2, fb_height / 2);
    wm_cursor_move(g_mouse_x, g_mouse_y);
    wm_set_focus(g_win3d);
    wm_present();
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

        if (!g_win[g_win3d].used && !g_win[g_panel_win].used) {
            wm_present();
            cpu_idle();
            continue;
        }

        ui_begin(&g_ui, g_win[g_win3d].used ? g_win3d : g_panel_win,
                 VPX + 8, VPY + 8, 130);
        ui_input(&g_ui, down, pressed, released, key);

        g_view.dx = 0; g_view.dy = 0; g_view.key = 0;
        if (g_win[g_win3d].used) {
            // Scene, then HUD (which is offered the pointer first), then the
            // viewport widget last -- the ordering K16 arrived at.
            if (redraw) render_scene();
            ui_window(&g_ui, g_win3d, VPX + 8, VPY + 8, 130);
            if (ui_checkbox(&g_ui, "textures", &g_texon)) redraw = 1;
            if (ui_checkbox(&g_ui, "wireframe", &g_wire)) redraw = 1;

            ui_window(&g_ui, g_win3d, VPX - 1, VPY - 1, VPW + 2);
            ui_glview(&g_ui, &g_view, VPH + 2);

            if (g_view.dx || g_view.dy) {
                cam_look(&g_cam, g_view.dx * (GL_ONE / 3),
                         0 - g_view.dy * (GL_ONE / 3));
                redraw = 1;
            }
            if (g_view.key) {
                long step;
                step = GL_ONE / 2;
                if (g_view.key == 'w') cam_move(&g_cam, step, 0, 0);
                if (g_view.key == 's') cam_move(&g_cam, 0 - step, 0, 0);
                if (g_view.key == 'a') cam_move(&g_cam, 0, 0 - step, 0);
                if (g_view.key == 'd') cam_move(&g_cam, 0, step, 0);
                redraw = 1;
            }
        } else {
            ui_id(&g_ui, 3);
        }

        if (g_win[g_panel_win].used) {
            ui_window(&g_ui, g_panel_win, PPX, PPY, PPW);
            ui_label(&g_ui, "triangles drawn");
            ui_progress(&g_ui, g_stat_tris, 0, 9 * 12);
            if (ui_checkbox(&g_ui, "lighting", &g_light)) redraw = 1;
            if (ui_button(&g_ui, "reset view")) {
                cam_init(&g_cam);
                g_cam.eye.y = GL_ONE;
                g_cam.eye.z = 0 - 8 * GL_ONE;
                g_cam.pitch = 0 - (6 << GL_FRAC);
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

    g_scratch = (long *)kmalloc(VPW * VPH * 8);
    if (!g_scratch) fail("could not allocate the comparison buffer");

    build_coord_texture();
    build_check_texture();
    build_cube();
    build_3d_window();

    g_coord_name = glGenTexture();
    glBindTexture(&g_gls, GL_TEXTURE_2D, g_coord_name);
    if (!glTexImage2D(&g_gls, COORD_N, COORD_N, g_coord_tex))
        fail("uploading the coordinate texture");
    g_check_name = glGenTexture();
    glBindTexture(&g_gls, GL_TEXTURE_2D, g_check_name);
    if (!glTexImage2D(&g_gls, CHECK_N, CHECK_N, g_check_tex))
        fail("uploading the checkerboard");

    test_upload();
    test_perspective();
    test_clipped_texture();
    test_modulate();
    test_depth_and_cost();

    printf("\nheap: %d pages mapped, %d bytes free\n", heap_pages, heap_bytes_free());

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else puts("\nPASS: textures, mapped correctly, at the cost of a viewport\n");

    puts("\nGLTEXTEST DONE\n");
}

int main() {
    serial_init();
    g_fail = 0;

    puts("\nnano-os: perspective-correct textures in fixed point\n");

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
