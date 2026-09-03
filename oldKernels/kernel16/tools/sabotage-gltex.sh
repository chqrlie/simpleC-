#!/bin/sh
# sabotage-gltex.sh -- break the texture layer on purpose and check the suite
# says so.
#
# Same method and the same warning as the other three matrices: the grep is for
# FAIL ANYWHERE in the line, not anchored to the start, because expect() prints
# its got/wanted diagnostic first. An anchored grep once reported eight of
# eleven deliberately broken builds as passing.
#
#   sh tools/sabotage-gltex.sh        # all of them
#   sh tools/sabotage-gltex.sh 4      # just number 4
#
# Run from the kernel/ directory.

set -e
cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
LOG=sabotage-gltex.log
ONLY="$1"

run_image() {
    rm -f "$LOG"
    $QEMU -kernel gltex.elf -no-reboot -display none \
          -serial "file:$LOG" -monitor none >/dev/null 2>&1 &
    qpid=$!
    n=0
    while [ $n -lt 260 ]; do
        if [ -f "$LOG" ] && tr -d '\r' < "$LOG" | grep -q "GLTEXTEST DONE"; then break; fi
        if [ -f "$LOG" ] && tr -d '\r' < "$LOG" | grep -q "halted."; then break; fi
        if ! kill -0 $qpid 2>/dev/null; then break; fi
        sleep 1
        n=$((n + 1))
    done
    kill $qpid 2>/dev/null || true
    wait $qpid 2>/dev/null || true
}

report() {
    tr -d '\r' < "$LOG" > "$LOG.clean"
    # A crash counts as caught. An image that faults has told you loudly that
    # something is wrong, which is the whole point -- the failure mode to fear
    # is the one that renders a plausible picture.
    if grep -q "EXCEPTION" "$LOG.clean"; then
        echo "    caught: the image FAULTED"
        grep -m2 "EXCEPTION\|faulting address" "$LOG.clean" | sed 's/^/      /'
        return
    fi
    if ! grep -q "GLTEXTEST DONE" "$LOG.clean"; then
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
    if make gltex.elf >/dev/null 2>&1; then
        run_image
        report
    else
        echo "    (does not build -- a loud failure, which is the safe kind)"
    fi
    mv "$file.orig" "$file"
}

want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

echo "=== K17 sabotage matrix ==="
echo

# Baseline. If the unmodified tree is not clean, nothing below means anything.
if [ -z "$ONLY" ]; then
    echo "0. the tree as it stands (must be clean)"
    make gltex.elf >/dev/null 2>&1
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

# --- perspective correction ---

# THE HEADLINE ONE. Interpolate s and t straight across the triangle instead of
# s/z and t/z, which is affine mapping -- the PlayStation 1 floor.
want 1 && sabotage nano-gl.h \
  's|                            texel = gl_texel(tx, uoz / d, voz / d);|                            texel = gl_texel(tx, (w0 * c->vs[0] + w1 * c->vs[1] + w2 * c->vs[2]) / area, (w0 * c->vt[0] + w1 * c->vt[1] + w2 * c->vt[2]) / area);|' \
  "1. affine texture mapping -- s interpolated directly, no perspective divide"

# The full 32.32 product shifted down to 16.16 before interpolation, which is
# the obvious way to write it and throws away the bits that matter far away.
want 2 && sabotage nano-gl.h \
  's|    c->rs\[0\] = a->s \* iz\[0\]; c->rt\[0\] = a->t \* iz\[0\];|    c->rs[0] = fx_mul(a->s, iz[0]); c->rt[0] = fx_mul(a->t, iz[0]);|; s|    c->rs\[1\] = b->s \* iz\[1\]; c->rt\[1\] = b->t \* iz\[1\];|    c->rs[1] = fx_mul(b->s, iz[1]); c->rt[1] = fx_mul(b->t, iz[1]);|; s|    c->rs\[2\] = v->s \* iz\[2\]; c->rt\[2\] = v->t \* iz\[2\];|    c->rs[2] = fx_mul(v->s, iz[2]); c->rt[2] = fx_mul(v->t, iz[2]);|' \
  "2. s/z rounded to 16.16 before interpolation instead of kept at 32.32"

# --- the clipper ---

want 3 && sabotage nano-gl.h \
  's|    o->s = p->s + fx_mul(q->s - p->s, t);|    o->s = p->s;|' \
  "3. the near-plane clipper does not interpolate the texture coordinate"

# --- sampling ---

want 4 && sabotage nano-gl.h \
  's|    return t->pix\[v \* t->w + u\];|    return t->pix[u * t->w + v];|' \
  "4. the texel lookup transposes u and v"

want 5 && sabotage nano-gl.h \
  's|    u = ((s \* t->w) >> GL_FRAC) \& t->wmask;|    u = (s * t->w) >> GL_FRAC;|' \
  "5. GL_REPEAT does not wrap, so s > 1 indexes off the end of the texture"

want 6 && sabotage nano-gl.h \
  's|        g_tex\[c->tex\].used \&\& g_tex\[c->tex\].pix) {|        g_tex[c->tex].used) {|' \
  "6. a texture name bound before upload is sampled through a null pointer"

# --- the texture environment ---

want 7 && sabotage nano-gl.h \
  's|                                   rgb(((texel >> 16) \& 255) \* cr / 255,|                                   rgb(((texel >> 16) \& 255),|' \
  "7. GL_MODULATE ignores the primary colour in the red channel"

# --- the API ---

want 8 && sabotage nano-glapi.h \
  's@    if (!gl_pow2(w) || !gl_pow2(h) || w < 1 || h < 1) return 0;@    ;@' \
  "8. glTexImage2D accepts a non-power-of-two texture"

want 9 && sabotage nano-glapi.h \
  's|    p.s = st->cs;|    p.s = 0;|' \
  "9. a vertex does not snapshot the current glTexCoord2x"

echo
echo "=== done; the tree is unchanged ==="
make gltex.elf >/dev/null 2>&1
