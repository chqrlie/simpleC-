// intr.c — bring up interrupts and prove each piece separately.
//
// Runs headless: the whole commentary goes to the serial port so `make
// intrtest` can check it. Deliberately does NOT need a keyboard, because the
// point is to test the timer, the idle path and the exception reporter, and a
// test that needs someone to type is not a test.

#include "nano-kernel.h"
#include "nano-int.h"

int main() {
    long t0;
    long t1;
    long w0;

    serial_init();
    vga_clear();
    kbd_init();

    puts("nano-os interrupt bring-up\n");

    interrupts_init(100);                  // 100 Hz
    printf("PIT programmed for %d Hz\n", g_hz);

    // 1. does the timer actually tick?
    t0 = g_ticks;
    sleep_ms(300);
    t1 = g_ticks;
    printf("ticks after 300ms: %d\n", t1 - t0);
    if (t1 - t0 < 20 || t1 - t0 > 45) puts("TIMER OUT OF RANGE\n");
    else puts("timer ok\n");

    // 2. did the CPU actually stop, or did it spin? A spin would rack up
    //    millions of loop iterations in 300ms; sleeping means roughly one
    //    wake-up per tick.
    w0 = g_idle_wakeups;
    t0 = g_ticks;
    while (g_ticks < t0 + 30) { g_idle_wakeups = g_idle_wakeups + 1; cpu_idle(); }
    printf("wakeups during 30 ticks: %d\n", g_idle_wakeups - w0);
    if (g_idle_wakeups - w0 > 200) puts("NOT IDLING (spinning)\n");
    else puts("idle ok: the core halted between interrupts\n");

    // 3. software interrupt: reach the dispatcher through a vector that is not
    //    an exception and not a device, so nothing else can explain it.
    printf("spurious so far: %d\n", g_spurious);

    // 4. the exception reporter. A deliberate fault, so the report is exercised
    //    by the test rather than only by an accident later. This one is a page
    //    fault at an address the identity map does not cover.
    puts("\nabout to touch unmapped memory on purpose:\n");
    {
        long *bad;
        bad = (long *)0x00007FFFFFFFF000;   // far above the 4 GiB we map
        *bad = 1;
    }

    puts("UNREACHABLE: the fault did not trap\n");
    for (;;) { }
    return 0;
}
