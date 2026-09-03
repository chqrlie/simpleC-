// threads.c — preemptive threads, locking, and a thread-safe heap.
//
// Headless. Each test is one that fails visibly if the thing it names is
// broken, rather than one that passes because nothing happened.
//
// The interesting one is the counter test. It runs the same increment twice,
// once without a lock and once with, and requires the unlocked version to LOSE
// updates. A concurrency test that only checks the locked case passes just as
// happily on a kernel where the threads never actually interleave.

#include "nano-kernel.h"
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-int.h"

// Forward declaration: `(long)spinner` needs the name registered before use,
// and a prototype is enough -- fnsig_add runs for prototypes too.
void spinner(long unused);

long g_counter;
long g_expected;
struct Mutex g_lock;
long g_done;
long g_order[64];
long g_norder;

// Increment a shared counter with a deliberate window between the read and the
// write. Without a lock the window guarantees a lost update rather than
// leaving it to luck, which is what makes this test say something.
void racer_unlocked(long n) {
    long i;
    i = 0;
    while (i < n) {
        long v;
        v = g_counter;
        thread_yield();                  // the window, opened on purpose
        g_counter = v + 1;
        i = i + 1;
    }
    g_done = g_done + 1;
    thread_exit(0);
}

void racer_locked(long n) {
    long i;
    i = 0;
    while (i < n) {
        long v;
        mutex_lock(&g_lock);
        v = g_counter;
        thread_yield();                  // the same window, now inside the lock
        g_counter = v + 1;
        mutex_unlock(&g_lock);
        i = i + 1;
    }
    g_done = g_done + 1;
    thread_exit(0);
}

// Records the order it ran in, so interleaving can be seen rather than assumed.
void interleaver(long id) {
    long i;
    i = 0;
    while (i < 8) {
        long f;
        f = irq_save();
        if (g_norder < 64) { g_order[g_norder] = id; g_norder = g_norder + 1; }
        irq_restore(f);
        thread_yield();
        i = i + 1;
    }
    g_done = g_done + 1;
    thread_exit(id * 100);
}

// Hammer the heap from several threads at once. Before kmalloc masked
// interrupts, two threads could find the same free block and both take it.
long g_alloc_bad;
void allocator(long n) {
    long i;
    i = 0;
    while (i < n) {
        char *p;
        long j;
        long sz;
        sz = 64 + (i % 7) * 32;
        p = (char *)kmalloc(sz);
        if (!p) { g_alloc_bad = g_alloc_bad + 1; break; }
        j = 0;
        while (j < sz) { p[j] = (char)(i & 255); j = j + 1; }
        thread_yield();
        j = 0;
        while (j < sz) {
            if (p[j] != (char)(i & 255)) { g_alloc_bad = g_alloc_bad + 1; j = sz; }
            else j = j + 1;
        }
        kfree(p);
        i = i + 1;
    }
    g_done = g_done + 1;
    thread_exit(0);
}

// The first thread the scheduler runs: it drives the whole test and then stops
// the machine. Everything below runs with the scheduler already live.
void main_thread(long unused) {
    long t0;

    puts("scheduler running\n");

    // --- 1. threads really interleave ---
    {
        long a;
        long b;
        long c;
        long i;
        long distinct;
        g_norder = 0;
        g_done = 0;
        a = thread_create((long)interleaver, 1, "int-1");
        b = thread_create((long)interleaver, 2, "int-2");
        c = thread_create((long)interleaver, 3, "int-3");
        while (g_done < 3) thread_yield();
        puts("run order: ");
        i = 0;
        while (i < g_norder && i < 24) { print_int(g_order[i]); putc(' '); i = i + 1; }
        putc('\n');
        // if they had run to completion one at a time the order would be
        // 1 eight times, then 2 eight times, then 3
        distinct = 0;
        i = 1;
        while (i < 6 && i < g_norder) {
            if (g_order[i] != g_order[0]) distinct = 1;
            i = i + 1;
        }
        if (distinct) puts("interleave ok: threads share the CPU\n");
        else puts("NOT INTERLEAVING\n");
        printf("join returned %d %d %d\n",
               thread_join(a), thread_join(b), thread_join(c));
        if (thread_join(a) == 100 && thread_join(b) == 200 && thread_join(c) == 300)
            puts("join ok: return values came back\n");
        else puts("JOIN RETURNED THE WRONG VALUE\n");
    }

    // --- 2. without a lock, updates are lost ---
    {
        long a;
        long b;
        g_counter = 0;
        g_done = 0;
        g_expected = 200;
        a = thread_create((long)racer_unlocked, 100, "race-a");
        b = thread_create((long)racer_unlocked, 100, "race-b");
        thread_join(a);
        thread_join(b);
        printf("unlocked: counter %d, expected %d\n", g_counter, g_expected);
        if (g_counter < g_expected) puts("race ok: the unlocked version lost updates\n");
        else puts("NO RACE SEEN -- the test is not proving anything\n");
    }

    // --- 3. with a lock, none are ---
    {
        long a;
        long b;
        long c;
        mutex_init(&g_lock);
        g_counter = 0;
        g_done = 0;
        g_expected = 300;
        a = thread_create((long)racer_locked, 100, "lock-a");
        b = thread_create((long)racer_locked, 100, "lock-b");
        c = thread_create((long)racer_locked, 100, "lock-c");
        thread_join(a);
        thread_join(b);
        thread_join(c);
        printf("locked:   counter %d, expected %d\n", g_counter, g_expected);
        if (g_counter == g_expected) puts("mutex ok: every update landed\n");
        else puts("MUTEX DID NOT PROTECT THE COUNTER\n");
    }

    // --- 4. the heap survives concurrent use ---
    {
        long a;
        long b;
        long c;
        long blocks_before;
        blocks_before = heap_blocks(0);
        g_alloc_bad = 0;
        g_done = 0;
        a = thread_create((long)allocator, 40, "alloc-a");
        b = thread_create((long)allocator, 40, "alloc-b");
        c = thread_create((long)allocator, 40, "alloc-c");
        thread_join(a);
        thread_join(b);
        thread_join(c);
        printf("concurrent heap: %d corruptions, %d blocks in use (was %d)\n",
               g_alloc_bad, heap_blocks(0), blocks_before);
        if (g_alloc_bad == 0 && heap_blocks(0) == blocks_before)
            puts("heap ok: no overlap and nothing leaked\n");
        else puts("HEAP CORRUPTED UNDER THREADS\n");
    }

    // --- 5. preemption without any yield at all ---
    // The threads above all yield. This one does not: it spins, and only the
    // timer can take the CPU away from it.
    {
        long a;
        long sw0;
        sw0 = g_switches;
        g_done = 0;
        a = thread_create((long)spinner, 0, "spin");
        t0 = g_ticks;
        while (g_ticks < t0 + 30) thread_yield();
        printf("30 ticks with a non-yielding thread: %d switches\n",
               g_switches - sw0);
        if (g_switches - sw0 > 10) puts("preemption ok: the timer took the CPU back\n");
        else puts("NOT PREEMPTING -- only voluntary yields are switching\n");
        g_threads[a].state = T_DONE;
    }

    printf("\n%d switches, %d stack overflows detected\n",
           g_switches, g_stack_smashed);
    puts("thread bring-up complete\n");
    cpu_halt_forever();
}

// Never yields. Its only purpose is to be interrupted.
void spinner(long unused) {
    long i;
    i = 0;
    for (;;) { i = i + 1; }
}

int main() {
    serial_init();
    vga_clear();
    kbd_init();
    interrupts_init(100);
    if (!mm_init()) { puts("mm_init failed\n"); cpu_halt_forever(); }
    thread_init();

    puts("nano-os thread bring-up\n");
    printf("%d KiB RAM, %d frames free\n", mm_ram_total / 1024, mm_free_frames);

    thread_create((long)main_thread, 0, "main");
    sched_start();                     // does not return

    puts("UNREACHABLE\n");
    return 0;
}
