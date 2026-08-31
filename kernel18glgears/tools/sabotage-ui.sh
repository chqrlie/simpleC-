#!/bin/sh
# sabotage-ui.sh -- break the widget layer on purpose and check the suite says so.
#
# Same method as tools/sabotage-wmin.sh, and the same warning applies: the grep
# below is for FAIL ANYWHERE in the line, not anchored to the start, because
# expect() prints its got/wanted diagnostic first. An anchored grep reported
# eight of eleven deliberately broken builds as passing last time.
#
#   sh tools/sabotage-ui.sh        # all of them
#   sh tools/sabotage-ui.sh 4      # just number 4
#
# Run from the kernel/ directory.

set -e
cd "$(dirname "$0")/.."

QEMU=${QEMU:-qemu-system-x86_64}
LOG=sabotage-ui.log
ONLY="$1"

run_image() {
    rm -f "$LOG"
    $QEMU -kernel ui.elf -no-reboot -display none \
          -serial "file:$LOG" -monitor none >/dev/null 2>&1 &
    qpid=$!
    n=0
    while [ $n -lt 220 ]; do
        if [ -f "$LOG" ] && tr -d '\r' < "$LOG" | grep -q "UITEST DONE"; then break; fi
        if ! kill -0 $qpid 2>/dev/null; then break; fi
        sleep 1
        n=$((n + 1))
    done
    kill $qpid 2>/dev/null || true
    wait $qpid 2>/dev/null || true
}

report() {
    tr -d '\r' < "$LOG" > "$LOG.clean"
    if ! grep -q "UITEST DONE" "$LOG.clean"; then
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
    if make ui.elf >/dev/null 2>&1; then
        run_image
        report
    else
        echo "    (does not build -- a loud failure, which is the safe kind)"
    fi
    mv "$file.orig" "$file"
}

want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

echo "=== K14 sabotage matrix ==="
echo

# Baseline. If the unmodified tree is not clean, nothing below means anything.
if [ -z "$ONLY" ]; then
    echo "0. the tree as it stands (must be clean)"
    make ui.elf >/dev/null 2>&1
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

want 1 && sabotage nano-ui.h \
  's|if (act \&\& ui->mreleased \&\& hot) clicked = 1;|if (act \&\& ui->mreleased) clicked = 1;|' \
  "1. a button fires on release wherever the pointer ended up"

want 2 && sabotage nano-ui.h \
  's|^    ui->mpressed = 0;$|    ;|' \
  "2. the press edge is never consumed, so one press lasts forever"

want 3 && sabotage nano-ui.h \
  's|if (hot \&\& ui->mpressed \&\& ui->active < 0) {$|if (hot \&\& ui->mpressed) {|' \
  "3. a second widget can steal a pointer another one already owns"

want 4 && sabotage nano-ui.h \
  's|    if (act \&\& ui->mdown) {$|    if (act \&\& ui->mdown \&\& hot) {|' \
  "4. the slider stops tracking the moment the pointer leaves it"

# All four clamp lines at once, deliberately. The slider clamps twice -- once
# on the track position and again on the resulting value -- and each is
# sufficient on its own, so removing either one is invisible. That is genuine
# redundancy rather than an untested line, and the matrix is what revealed it.
# The property worth testing is "the slider stays in range", so the sabotage
# removes all of it.
want 5 && sabotage nano-ui.h \
  's|        if (rel < 0) rel = 0;|        ;|; s|        if (rel > track) rel = track;|        ;|; s|        if (\*v < lo) \*v = lo;|        ;|; s|        if (\*v > hi) \*v = hi;|        ;|' \
  "5. the slider is not clamped to its range at all (both clamps removed)"

want 6 && sabotage nano-ui.h \
  's|    if (g_ui_last\[id\] == state) return 0;|    if (0) return 0;|' \
  "6. every widget invalidates every frame (the whole claim of K14)"

want 7 && sabotage nano-ui.h \
  's|        else if (ui->focus == id) ui->focus = -1;|        ;|' \
  "7. a text field is never unfocused by clicking elsewhere"

want 8 && sabotage nano-ui.h \
  's|            if (n < cap - 1) {|            if (n < cap) {|' \
  "8. the text field writes one past the end of the caller's buffer"

want 9 && sabotage nano-ui.h \
  's|        ui_text_clip(ui, ui->x + 3, ui->y, UI_ROW_H, buf + off, w - 6);|        wm_win_text(ui->win, ui->x + 3, ui->y + 6, buf, ui->fg);|' \
  "9. the text field draws its whole string, outside its own rectangle"

want 10 && sabotage nano-ui.h \
  's|    while (i < n) { hash = ((hash \* 33) + (buf\[i\] \& 255)) \& 0xFFFFFFF; i = i + 1; }|    while (i < n) { hash = hash + 1; i = i + 1; }|' \
  "10. the text field's state hash counts characters instead of reading them"

want 11 && sabotage nano-ui.h \
  's|^    ui->hot = -1;$|    ;|' \
  "11. hot is never reset, so it survives the pointer leaving"

echo
echo "=== done; nano-ui.h is unchanged ==="
make ui.elf >/dev/null 2>&1
