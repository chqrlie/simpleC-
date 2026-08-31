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
    mm_protect_null();

    kbd_init();
    interrupts_init(100);

    run_tests();

    puts("gears running; the machine is now interactive\n");
    spin_forever();
    return 0;
}
