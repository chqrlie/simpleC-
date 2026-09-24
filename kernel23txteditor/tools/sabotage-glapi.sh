#!/bin/sh
# sabotage-glapi.sh -- break the GL layer on purpose and check the suite says so.
#
# Same method and the same warning as the other two matrices: the grep is for
# FAIL ANYWHERE in the line, not anchored to the start, because expect() prints
# its got/wanted diagnostic first. An anchored grep once reported eight of
# eleven deliberately broken builds as passing.
#
#   sh tools/sabotage-glapi.sh        # all of them
#   sh tools/sabotage-glapi.sh 4      # just number 4
#
# Run from the kernel/ directory.

set -e
cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
LOG=sabotage-glapi.log
ONLY="$1"

run_image() {
    rm -f "$LOG"
    $QEMU -kernel glapi.elf -no-reboot -display none \
          -serial "file:$LOG" -monitor none >/dev/null 2>&1 &
    qpid=$!
    n=0
    while [ $n -lt 260 ]; do
        if [ -f "$LOG" ] && tr -d '\r' < "$LOG" | grep -q "GLAPITEST DONE"; then break; fi
        if ! kill -0 $qpid 2>/dev/null; then break; fi
        sleep 1
        n=$((n + 1))
    done
    kill $qpid 2>/dev/null || true
    wait $qpid 2>/dev/null || true
}

report() {
    tr -d '\r' < "$LOG" > "$LOG.clean"
    if ! grep -q "GLAPITEST DONE" "$LOG.clean"; then
        echo "    the image did not finish"
        return
    fi
    n=$(grep -c "FAIL:" "$LOG.clean" || true)
    if [ "$n" = "0" ]; then
        echo "    *** NOT CAUGHT -- the suite passed with this bug in place ***"
    else
        echo "    caught by $n check(s):"
        grep "FAIL:" "$LOG.clean" | sed 's/^/      /'
    fi
}

sabotage() {
    file="$1"; script="$2"; desc="$3"
    echo "$desc"
    cp "$file" "$file.orig"
    sed -i "$script" "$file"
    if cmp -s "$file" "$file.orig"; then
        echo "    the sabotage did not match anything -- the code has moved"
        mv "$file.orig" "$file"
        return
    fi
    if make glapi.elf >/dev/null 2>&1; then
        run_image
        report
    else
        echo "    (does not build -- a loud failure, which is the safe kind)"
    fi
    mv "$file.orig" "$file"
}

want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

echo "=== K16 sabotage matrix ==="
echo

# Baseline. If the unmodified tree is not clean, nothing below means anything.
if [ -z "$ONLY" ]; then
    echo "0. the tree as it stands (must be clean)"
    make glapi.elf >/dev/null 2>&1
    run_image
    tr -d '\r' < "$LOG" > "$LOG.clean"
    if grep -q "FAIL:" "$LOG.clean"; then
        echo "    the unmodified tree already fails -- stopping"
        grep "FAIL:" "$LOG.clean" | sed 's/^/      /'
        exit 1
    fi
    echo "    clean"
    echo
fi

# --- primitive assembly ---

# The one a triangle count cannot see. Same number of triangles, half the strip
# facing the wrong way.
want 1 && sabotage nano-glapi.h \
  's|            if (st->total \& 1) gl_emit_tri(st, \&st->vbuf\[1\], \&st->vbuf\[0\], \&p);|            if (0) gl_emit_tri(st, \&st->vbuf[1], \&st->vbuf[0], \&p);|' \
  "1. a triangle strip does not alternate its winding"

want 2 && sabotage nano-glapi.h \
  's|            gl_emit_tri(st, \&st->vbuf\[0\], \&st->vbuf\[3\], \&st->vbuf\[2\]);|            gl_emit_tri(st, \&st->vbuf[0], \&st->vbuf[2], \&st->vbuf[3]);|' \
  "2. a quad strip pairs its far vertices the wrong way round"

want 3 && sabotage nano-glapi.h \
  's|            gl_emit_tri(st, \&st->first, \&st->vbuf\[0\], \&p);|            gl_emit_tri(st, \&st->vbuf[0], \&st->first, \&p);|' \
  "3. a triangle fan swaps its anchor and its previous vertex"

want 4 && sabotage nano-glapi.h \
  's|            gl_emit_tri(st, \&st->vbuf\[0\], \&st->vbuf\[2\], \&st->vbuf\[3\]);|            ;|' \
  "4. a quad emits only its first triangle"

want 5 && sabotage nano-glapi.h \
  's|    if (st->prim == GL_LINE_LOOP \&\& st->total > 2 \&\& st->c) {|    if (0) {|' \
  "5. a line loop never closes"

# --- matrices ---

want 6 && sabotage nano-glapi.h \
  's|    m4_mul(t, t, m);|    m4_mul(t, m, t);|' \
  "6. the matrix stack pre-multiplies instead of post-multiplying"

want 7 && sabotage nano-glapi.h \
  's|    v3_norm(\&u, \&k);|    u = k;|' \
  "7. glRotatex does not normalise its axis"

want 8 && sabotage nano-glapi.h \
  's|    if (st->mvsp == 0) { st->overflow = st->overflow + 1; return; }|    if (st->mvsp == 0) return;|' \
  "8. an unbalanced glPopMatrix is swallowed silently"

want 9 && sabotage nano-glapi.h \
  's|    v3_cross(\&d, up, \&f);|    v3_cross(\&d, \&f, up);|' \
  "9. gluLookAtx builds a mirrored basis"

# --- the frustum ---

want 10 && sabotage nano-gl.h \
  's|        f->p\[i \* 2 + 1\].a = clip->m\[12\] - clip->m\[r\];|        f->p[i * 2 + 1].a = clip->m[12] + clip->m[r];|' \
  "10. the right/top/far planes are added instead of subtracted"

want 11 && sabotage nano-gl.h \
  's|^void gl_plane_norm(struct Plane \*p) {|void gl_plane_norm(struct Plane *p) { return;|' \
  "11. the frustum planes are never normalised, so distances are not distances"

want 12 && sabotage nano-gl.h \
  's|        if (d < 0 - radius) return GL_OUTSIDE;|        if (d < 0) return GL_OUTSIDE;|' \
  "12. the sphere test ignores the radius and culls objects that are visible"

want 13 && sabotage nano-gl.h \
  's|                if (d >= c->izfar \&\& (!c->depth |                if ((!c->depth |' \
  "13. the rasteriser has no far clip, so culling changes the picture"

# --- the widget ---

want 14 && sabotage nano-ui.h \
  '/^long ui_glview/,/^}$/ s|    if (act \&\& ui->mdown) {|    if (act \&\& ui->mdown \&\& hot) {|' \
  "14. a drag stops the moment the pointer leaves the viewport"

want 15 && sabotage nano-ui.h \
  's|        if (v->seen) { v->dx = ui->mx - v->px; v->dy = ui->my - v->py; }|        { v->dx = ui->mx - v->px; v->dy = ui->my - v->py; }|' \
  "15. the first frame of a drag reports motion from a stale sample"

want 16 && sabotage nano-ui.h \
  '/^long ui_glview/,/^}$/ s|        else if (!hot \&\& ui->focus == id) ui->focus = -1;|        ;|' \
  "16. clicking away never unfocuses the viewport, so it keeps taking keys"

want 17 && sabotage nano-ui.h \
  's|    ui_track(ui, ui_next_id(ui), ui_hash_str(s));|    ui_track(ui, ui_next_id(ui), (long)s);|' \
  "17. a label remembers the address of its text, not the text (the K14 bug)"

want 18 && sabotage nano-ui.h \
  '/^long ui_glview/,/^}$/ s|        if (hot \&\& ui->active < 0) { ui->active = id; ui->focus = id; }|        if (hot) { ui->active = id; ui->focus = id; }|' \
  "18. the viewport takes a pointer another widget already owns"

echo
echo "=== done; the tree is unchanged ==="
make glapi.elf >/dev/null 2>&1
