#!/bin/sh
# sabotage-wmin.sh -- break K13 on purpose, one bug at a time, and check that
# the suite notices.
#
# A test suite that is green tells you nothing on its own. It might be green
# because the code is right, or because the checks do not actually look at the
# thing they claim to. The only way to tell the two apart is to introduce the
# bug the check exists to catch and watch it go red.
#
# Each entry below is a one-line edit to a header, chosen to be exactly the
# mistake a person would plausibly make. The script applies it, rebuilds, runs
# the image headless, records which checks failed, and puts the file back.
#
#   sh tools/sabotage-wmin.sh          # all of them
#   sh tools/sabotage-wmin.sh 3        # just number 3
#
# Run from the kernel/ directory.

set -e
cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
LOG=sabotage.log
ONLY="$1"

# Boot the image and wait for it to say it is done, rather than sleeping a
# fixed time. The image ends in an interactive event loop and never exits, so
# something has to stop it either way.
run_image() {
    rm -f "$LOG"
    $QEMU -kernel wmin.elf -no-reboot -display none \
          -serial "file:$LOG" -monitor none >/dev/null 2>&1 &
    qpid=$!
    n=0
    while [ $n -lt 200 ]; do
        if [ -f "$LOG" ] && tr -d '\r' < "$LOG" | grep -q "WMINTEST DONE"; then
            break
        fi
        if ! kill -0 $qpid 2>/dev/null; then break; fi
        sleep 1
        n=$((n + 1))
    done
    kill $qpid 2>/dev/null || true
    wait $qpid 2>/dev/null || true
}

# Note the grep is for FAIL: ANYWHERE in the line, not anchored to the start.
# expect() prints the got/wanted diagnostic first, so an anchored grep counts
# only the handful of bare fail() calls -- which is how the first run of this
# script reported eight deliberately broken builds as passing. The harness was
# wrong, not the suite, and a harness that under-reports failures is worse than
# no harness at all, because it certifies the code.
report() {
    tr -d '\r' < "$LOG" > "$LOG.clean"
    if ! grep -q "WMINTEST DONE" "$LOG.clean"; then
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

# $1 file  $2 sed script  $3 description
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
    if make wmin.elf >/dev/null 2>&1; then
        run_image
        report
    else
        echo "    (does not build -- a loud failure, which is the safe kind)"
    fi
    mv "$file.orig" "$file"
}

want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

echo "=== K13 sabotage matrix ==="
echo

# Baseline. An unmodified tree must report zero failures; if it does not, every
# "caught" below is meaningless because something else is already broken.
if [ -z "$ONLY" ]; then
    echo "0. the tree as it stands (must be clean)"
    make wmin.elf >/dev/null 2>&1
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

want 1 && sabotage nano-mouse.h \
  's|if (g_mp0 \& 0x10) dx = dx - 256;|/* sabotage: no sign extension */|' \
  "1. dx is never sign-extended (the classic PS/2 mouse bug)"

want 2 && sabotage nano-mouse.h \
  's|mouse_apply(dx, 0 - dy, g_mp0 \& 7);|mouse_apply(dx, dy, g_mp0 \& 7);|' \
  "2. the Y axis is not flipped (mouse Y grows up, screens grow down)"

want 3 && sabotage nano-mouse.h \
  's|if (!(b \& 8)) {|if (0) {|' \
  "3. byte 0 is not checked for its always-set bit 3 (no resync)"

want 4 && sabotage nano-mouse.h \
  's|if (g_mp0 \& 0xC0) { g_mouse_dropped_ovf|if (0) { g_mouse_dropped_ovf|' \
  "4. the X/Y overflow bits are ignored"

want 5 && sabotage nano-mouse.h \
  's|if (g_mev\[last\].btn == btn) {|if (1) {|' \
  "5. motion coalescing also swallows button changes"

want 6 && sabotage nano-wm.h \
  's|if (g_cur_painted) {$|if (0) {|' \
  "6. the pointer is never erased before the next frame"

want 7 && sabotage nano-wm.h \
  's|if (old >= 0 \&\& old < WM_MAXWIN \&\& g_win\[old\].used) {|if (0) {|' \
  "7. focus repaints only the window gaining it, not the one losing it"

want 8 && sabotage nano-wmin.h \
  's|if (!g_win\[hnd\].fixed) {|if (0) {|' \
  "8. the close box is not tested before the title bar it sits in"

want 9 && sabotage nano-wmin.h \
  's|if (part == WM_PART_TITLE) {|if (1) {|' \
  "9. a press anywhere in a window starts a drag, not just the title bar"

want 10 && sabotage nano-term.h \
  "s|while (i < n) { g_term\[ti\].shown\[i\] = TERM_DIRTY; i = i + 1; }|while (i < n) { g_term[ti].shown[i] = ' '; i = i + 1; }|" \
  "10. the terminal's shown grid starts out agreeing with its cells grid"

want 11 && sabotage nano-term.h \
  's|            g_term\[ti\].shown\[g_term\[ti\].scy \* cols + g_term\[ti\].scx\] = TERM_DIRTY;|            ;|' \
  "11. the cell the text cursor left is not marked for redraw"

echo
echo "=== done; nano-mouse.h, nano-wm.h, nano-wmin.h and nano-term.h are unchanged ==="
make wmin.elf >/dev/null 2>&1
