// nano-mouse.h — the PS/2 mouse: an 8042 aux device, a three-byte packet, and
// a decoder that is deliberately not an interrupt handler.
//
// The split matters. `mouse_byte` takes one byte and advances a state machine;
// it does no I/O, touches no hardware, and acknowledges nothing. The interrupt
// handler in nano-int.h reads port 0x60, hands the byte to it, and sends the
// EOI. That is the whole coupling.
//
// The reason is that the byte stream is where the bugs live. Sign extension,
// the overflow bits, losing a byte and decoding every packet after it against
// the wrong offsets -- all of that is a pure function of a sequence of bytes,
// and a pure function can be fed a sequence of bytes by a test. Wire the
// decoder into the IRQ instead and the only way to exercise it is to move a
// real mouse and look at the screen, which is not a test, it is a hope.
//
// Requires nano-kernel.h (inb/outb). Must be included BEFORE nano-int.h, so
// that the dispatcher there can see `mouse_byte` and `mouse_hw_init`.

#ifndef NANO_MOUSE_H
#define NANO_MOUSE_H

// The 8042 keyboard controller, which despite the name is also where the mouse
// is. One data port shared by both devices, one command/status port.
#define PS2_DATA   0x60
#define PS2_CMD    0x64

// Status register bits, read from 0x64.
#define PS2_OUTFULL 1     // there is a byte waiting in 0x60
#define PS2_INFULL  2     // 0x60 still holds a byte the controller has not read

// Controller commands.
#define PS2_DISABLE_AUX 0xA7
#define PS2_ENABLE_AUX  0xA8
#define PS2_READ_CFG    0x20
#define PS2_WRITE_CFG   0x60
#define PS2_TO_AUX      0xD4    // the next byte written to 0x60 goes to the mouse

// Mouse commands, sent through PS2_TO_AUX.
#define MS_SET_DEFAULTS 0xF6
#define MS_ENABLE       0xF4
#define MS_ACK          0xFA

// ---------- state ----------

long g_mouse_x;             // screen position, always inside the bounds
long g_mouse_y;
long g_mouse_btn;           // bit 0 left, bit 1 right, bit 2 middle
long g_mouse_maxx;          // set by mouse_bounds; the driver does not know
long g_mouse_maxy;          // about the framebuffer and does not need to

long g_mouse_present;       // the controller answered as expected
long g_mouse_packets;       // complete three-byte packets seen
long g_mouse_resync;        // first bytes rejected because bit 3 was clear
long g_mouse_dropped_ovf;   // packets discarded for the X/Y overflow bits
long g_mouse_moved;         // set on any change; cleared by whoever acts on it

// Packet assembly.
long g_mp_idx;
long g_mp0;
long g_mp1;

// ---------- the event queue ----------
//
// The window manager polls, and between two polls a lot can happen. Losing a
// motion event is invisible -- the next one carries the current position
// anyway. Losing a button press is not: a click that never arrives is a
// window that will not come to the front, and the user's conclusion is that
// the machine is broken.
//
// So motion is coalesced rather than queued. If the newest event in the ring
// has the same button state as the one arriving, its coordinates are simply
// overwritten. The ring therefore only ever grows on a button change, and a
// user cannot generate 64 of those before the next poll.

#define MOUSE_RING 64

struct MEvent { long x; long y; long btn; };

struct MEvent g_mev[MOUSE_RING];
long g_mev_head;
long g_mev_tail;
long g_mev_dropped;

long mouse_events_pending() { return g_mev_head != g_mev_tail; }

void mouse_push(long x, long y, long btn) {
    long next;
    long last;

    // Coalesce with the newest queued event if the buttons have not changed.
    if (g_mev_head != g_mev_tail) {
        last = g_mev_head - 1;
        if (last < 0) last = MOUSE_RING - 1;
        if (g_mev[last].btn == btn) {
            g_mev[last].x = x;
            g_mev[last].y = y;
            return;
        }
    }

    next = (g_mev_head + 1) % MOUSE_RING;
    if (next == g_mev_tail) { g_mev_dropped = g_mev_dropped + 1; return; }
    g_mev[g_mev_head].x = x;
    g_mev[g_mev_head].y = y;
    g_mev[g_mev_head].btn = btn;
    g_mev_head = next;
}

// Returns 1 and fills *out, or returns 0 if the queue is empty.
long mouse_pop(struct MEvent *out) {
    if (g_mev_head == g_mev_tail) return 0;
    out->x = g_mev[g_mev_tail].x;
    out->y = g_mev[g_mev_tail].y;
    out->btn = g_mev[g_mev_tail].btn;
    g_mev_tail = (g_mev_tail + 1) % MOUSE_RING;
    return 1;
}

// ---------- the decoder ----------

void mouse_bounds(long w, long h) {
    g_mouse_maxx = w - 1;
    g_mouse_maxy = h - 1;
    if (g_mouse_maxx < 0) g_mouse_maxx = 0;
    if (g_mouse_maxy < 0) g_mouse_maxy = 0;
    if (g_mouse_x > g_mouse_maxx) g_mouse_x = g_mouse_maxx;
    if (g_mouse_y > g_mouse_maxy) g_mouse_y = g_mouse_maxy;
}

// Apply one decoded movement. Kept separate so that the clamping is in one
// place: a cursor that can leave the screen is a cursor you cannot get back,
// because the only way to move it is relative.
void mouse_apply(long dx, long dy, long btn) {
    long ox;
    long oy;
    long ob;
    ox = g_mouse_x; oy = g_mouse_y; ob = g_mouse_btn;

    g_mouse_x = g_mouse_x + dx;
    g_mouse_y = g_mouse_y + dy;
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_x > g_mouse_maxx) g_mouse_x = g_mouse_maxx;
    if (g_mouse_y > g_mouse_maxy) g_mouse_y = g_mouse_maxy;
    g_mouse_btn = btn;

    if (g_mouse_x != ox || g_mouse_y != oy || g_mouse_btn != ob) {
        g_mouse_moved = 1;
        mouse_push(g_mouse_x, g_mouse_y, g_mouse_btn);
    }
}

// Put the pointer somewhere absolutely. The protocol has no way to say this --
// every packet is relative -- so this exists for bring-up (start in the middle
// of the screen rather than the top-left corner) and for tests, which need a
// known starting point before injecting a movement.
void mouse_warp(long x, long y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > g_mouse_maxx) x = g_mouse_maxx;
    if (y > g_mouse_maxy) y = g_mouse_maxy;
    g_mouse_x = x;
    g_mouse_y = y;
    mouse_push(g_mouse_x, g_mouse_y, g_mouse_btn);
}

// One byte of the stream.
//
// Packet layout, three bytes:
//   byte 0   YO XO YS XS  1  MB RB LB
//   byte 1   dx, as an 8-bit magnitude whose sign is XS in byte 0
//   byte 2   dy, likewise, and positive means the mouse moved AWAY from you
//
// Bit 3 of byte 0 is always 1. That is the only thing in the protocol that
// says where a packet starts, and it is the difference between a driver that
// survives a dropped byte and one that moves the cursor at random forever
// after.
void mouse_byte(long b) {
    long dx;
    long dy;

    b = b & 0xFF;

    if (g_mp_idx == 0) {
        if (!(b & 8)) {
            // Not a first byte. Stay at index 0 and keep discarding until one
            // arrives; anything else re-syncs onto a boundary that is wrong by
            // one or two bytes and stays wrong.
            g_mouse_resync = g_mouse_resync + 1;
            return;
        }
        g_mp0 = b;
        g_mp_idx = 1;
        return;
    }

    if (g_mp_idx == 1) { g_mp1 = b; g_mp_idx = 2; return; }

    g_mp_idx = 0;
    g_mouse_packets = g_mouse_packets + 1;

    // X or Y overflowed 9 bits inside the mouse. The magnitude byte is then
    // meaningless, and using it anyway makes the cursor jump the width of the
    // screen. There is no recovering the real number, so the packet goes.
    if (g_mp0 & 0xC0) { g_mouse_dropped_ovf = g_mouse_dropped_ovf + 1; return; }

    // Sign extension by hand. nano_cc has no `signed char`, and this arrives
    // as an int from inb() in the range 0..255, so 0xFF has to be turned into
    // -1 explicitly. Getting this wrong gives a cursor that moves right when
    // the mouse moves left, which is the classic symptom.
    dx = g_mp1;
    if (g_mp0 & 0x10) dx = dx - 256;
    dy = b;
    if (g_mp0 & 0x20) dy = dy - 256;

    // The mouse's Y axis grows upward and the screen's grows downward.
    mouse_apply(dx, 0 - dy, g_mp0 & 7);
}

// ---------- bring-up ----------

// The controller is slow relative to the CPU and will simply ignore a write
// issued while its input buffer is still full. Both waits are bounded: a
// missing or wedged controller must not hang the boot.
long ps2_wait_write() {
    long n;
    n = 0;
    while (n < 100000) {
        if (!(inb(PS2_CMD) & PS2_INFULL)) return 1;
        n = n + 1;
    }
    return 0;
}

long ps2_wait_read() {
    long n;
    n = 0;
    while (n < 100000) {
        if (inb(PS2_CMD) & PS2_OUTFULL) return 1;
        n = n + 1;
    }
    return 0;
}

// Write a byte to the mouse rather than the keyboard: prefix it with 0xD4.
long mouse_cmd(long cmd) {
    if (!ps2_wait_write()) return 0;
    outb(PS2_CMD, PS2_TO_AUX);
    if (!ps2_wait_write()) return 0;
    outb(PS2_DATA, cmd);
    if (!ps2_wait_read()) return 0;
    return (inb(PS2_DATA) & 0xFF) == MS_ACK;
}

void mouse_state_reset() {
    g_mouse_x = 0;
    g_mouse_y = 0;
    g_mouse_btn = 0;
    g_mouse_moved = 0;
    g_mouse_packets = 0;
    g_mouse_resync = 0;
    g_mouse_dropped_ovf = 0;
    g_mp_idx = 0;
    g_mp0 = 0;
    g_mp1 = 0;
    g_mev_head = 0;
    g_mev_tail = 0;
    g_mev_dropped = 0;
    g_mouse_maxx = 1023;
    g_mouse_maxy = 767;
}

// Returns 1 if the mouse acknowledged. Call with interrupts still off: the
// controller emits an ACK for every command, and an IRQ12 handler that runs
// while those ACKs are in flight feeds them to the packet decoder, where 0xFA
// has bit 3 set and is therefore a perfectly valid-looking first byte.
long mouse_hw_init() {
    long cfg;

    g_mouse_present = 0;
    mouse_state_reset();

    if (!ps2_wait_write()) return 0;
    outb(PS2_CMD, PS2_ENABLE_AUX);

    // Turn on IRQ12 (bit 1) and make sure the aux clock is not disabled
    // (bit 5). Read-modify-write: the keyboard's own bits live in the same
    // byte and clearing them stops the keyboard.
    if (!ps2_wait_write()) return 0;
    outb(PS2_CMD, PS2_READ_CFG);
    if (!ps2_wait_read()) return 0;
    cfg = inb(PS2_DATA) & 0xFF;
    cfg = cfg | 2;
    cfg = cfg & ~32;
    if (!ps2_wait_write()) return 0;
    outb(PS2_CMD, PS2_WRITE_CFG);
    if (!ps2_wait_write()) return 0;
    outb(PS2_DATA, cfg);

    if (!mouse_cmd(MS_SET_DEFAULTS)) return 0;
    if (!mouse_cmd(MS_ENABLE)) return 0;

    // Anything the controller queued during setup is not a packet.
    while (inb(PS2_CMD) & PS2_OUTFULL) { inb(PS2_DATA); }
    g_mp_idx = 0;

    g_mouse_present = 1;
    return 1;
}

#endif
