// srv.c — servers, a supervisor, and faults that do not stop the machine.
//
// Headless. Three failure modes are provoked on purpose and each has to be
// survived rather than merely reported:
//
//   1. a server that dereferences a null pointer
//   2. a server that stops answering but is still technically running
//   3. a server that faults repeatedly, to show the restart is not a one-off
//
// And the thing that makes all three worth testing: a healthy server has to
// keep counting throughout. A recovery test where nothing else is running
// proves the machine survived; it does not prove the *rest of the system* did.

#include "nano-kernel.h"
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-int.h"
#include "nano-srv.h"

long g_ticker_count;
long g_crash_now;
long g_hang_now;

// A healthy server. Its only job is to keep counting, so that "did everything
// else survive?" has an answer that is a number rather than an impression.
void ticker_server(long unused) {
    long me;
    me = srv_self();
    for (;;) {
        g_ticker_count = g_ticker_count + 1;
        srv_beat(me);
        thread_sleep_ms(20);
    }
}

// Faults when asked. A null dereference is the most ordinary way a driver
// dies, which is exactly why it is the one to test.
void crasher_server(long unused) {
    long me;
    me = srv_self();
    for (;;) {
        srv_beat(me);
        if (g_crash_now) {
            long *p;
            g_crash_now = g_crash_now - 1;
            p = (long *)0;
            p[0] = 1;                    // the fault -- see mm_protect_null
        }
        thread_sleep_ms(20);
    }
}

// Stops beating when asked, but keeps running. Nothing has crashed: the thread
// is alive and scheduled. Only the missing heartbeat gives it away, which is
// the case a crash-only supervisor misses entirely.
void hanger_server(long unused) {
    long me;
    me = srv_self();
    for (;;) {
        if (!g_hang_now) srv_beat(me);
        thread_sleep_ms(20);
    }
}

// Pad by hand: the kernel printf has no field width, and "%-9s" there prints
// literally and then shifts every following argument by one.
void pad_to(char *s, long width) {
    long n;
    n = 0;
    while (s[n]) { putc(s[n]); n = n + 1; }
    while (n < width) { putc(' '); n = n + 1; }
}

void print_servers() {
    long i;
    puts("name      state    thr restarts faults hangs beats\n");
    i = 0;
    while (i < g_nsrv) {
        pad_to(g_srv[i].name, 10);
        pad_to(srv_state_name(g_srv[i].state), 9);
        printf("%d   %d", g_srv[i].thread, g_srv[i].restarts);
        printf("        %d      %d", g_srv[i].faults, g_srv[i].hangs);
        printf("     %d\n", g_srv[i].beat);
        i = i + 1;
    }
}

void main_thread(long unused) {
    long tick;
    long crash_srv;
    long hang_srv;

    puts("scheduler running\n");
    srv_init(50);                        // supervisor looks every 50 ms

    srv_register("ticker", (long)ticker_server, 0, 30);
    crash_srv = srv_register("crasher", (long)crasher_server, 0, 30);
    hang_srv = srv_register("hanger", (long)hanger_server, 0, 30);

    srv_start(0);
    srv_start(crash_srv);
    srv_start(hang_srv);
    thread_sleep_ms(200);

    puts("\n-- all three servers up --\n");
    print_servers();
    tick = g_ticker_count;
    if (tick > 0) puts("healthy server is running\n");
    else puts("TICKER NOT RUNNING\n");

    // --- 1. a server faults ---
    puts("\n-- making the crasher dereference a null pointer --\n");
    g_crash_now = 1;
    thread_sleep_ms(600);
    print_servers();
    if (g_srv[crash_srv].restarts >= 1) puts("crash recovery ok: it was restarted\n");
    else puts("CRASHER WAS NOT RESTARTED\n");
    if (g_ticker_count > tick + 5)
        puts("isolation ok: the healthy server kept running through it\n");
    else puts("THE REST OF THE SYSTEM STOPPED TOO\n");
    if (g_srv[crash_srv].state == S_RUNNING) puts("crasher is running again\n");
    else puts("CRASHER DID NOT COME BACK\n");

    // --- 2. a server hangs without crashing ---
    tick = g_ticker_count;
    puts("\n-- making the hanger stop answering (still scheduled) --\n");
    g_hang_now = 1;
    thread_sleep_ms(700);
    g_hang_now = 0;
    thread_sleep_ms(300);
    print_servers();
    if (g_srv[hang_srv].hangs >= 1) puts("hang detected: the heartbeat went stale\n");
    else puts("HANG NOT DETECTED\n");
    if (g_srv[hang_srv].restarts >= 1) puts("hang recovery ok: it was restarted\n");
    else puts("HANGER WAS NOT RESTARTED\n");
    if (g_ticker_count > tick + 5) puts("healthy server unaffected again\n");
    else puts("THE REST OF THE SYSTEM STOPPED TOO\n");

    // --- 3. repeated faults ---
    tick = g_ticker_count;
    puts("\n-- crashing it three more times --\n");
    {
        long before;
        long i;
        before = g_srv[crash_srv].restarts;
        i = 0;
        while (i < 3) {
            g_crash_now = 1;
            thread_sleep_ms(400);
            i = i + 1;
        }
        printf("restarts went from %d to %d\n", before, g_srv[crash_srv].restarts);
        if (g_srv[crash_srv].restarts >= before + 3) puts("repeat recovery ok\n");
        else puts("REPEATED CRASHES WERE NOT ALL RECOVERED\n");
    }
    if (g_ticker_count > tick + 10) puts("healthy server still counting\n");
    else puts("THE REST OF THE SYSTEM STOPPED TOO\n");

    // --- 4. the heap did not leak a stack per restart ---
    // Every restart allocates a new 32 KiB stack and frees the old one. If the
    // old ones were not freed, this many restarts would be visible.
    printf("\nheap: %d pages, %d blocks in use\n", heap_pages, heap_blocks(0));
    printf("supervisor: %d checks, %d restarts, %d thread faults\n",
           g_sup_checks, g_sup_restarts, g_thread_faults);
    printf("ticker reached %d\n", g_ticker_count);

    puts("\nserver bring-up complete\n");
    cpu_halt_forever();
}

int main() {
    serial_init();
    vga_clear();
    kbd_init();
    interrupts_init(100);
    if (!mm_init()) { puts("mm_init failed\n"); cpu_halt_forever(); }
    // Without this, `*(long *)0 = 1` quietly writes into the interrupt vector
    // table and the crash test never crashes.
    if (!mm_protect_null()) puts("could not unmap page 0\n");
    thread_init();

    puts("nano-os server bring-up\n");
    g_ticker_count = 0;
    g_crash_now = 0;
    g_hang_now = 0;

    thread_create((long)main_thread, 0, "main");
    sched_start();
    return 0;
}
