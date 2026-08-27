// nano-thread.h — threads, a preemptive scheduler, and locking.
//
// Compiled by nano_cc with --kernel. Include AFTER nano-kernel.h and
// nano-mm.h, and BEFORE nano-int.h -- the interrupt dispatcher switches on this
// file's include guard to decide whether the timer schedules or just counts.
//
// The whole context switch is one instruction. isr_common has already pushed
// every register onto the current thread's stack before the dispatcher runs,
// so the entire machine state IS that stack -- and moving the stack pointer
// moves all of it. The scheduler's only job is to decide which stack to
// return, and `mov %rax, %rsp` in isr.s does the rest.
//
// That is also why a brand-new thread is indistinguishable from a preempted
// one: thread_create builds a stack that looks exactly like a thread caught
// mid-interrupt, so it resumes through the same path as everything else.
//
// SCOPE. One CPU. The MADT reports one processor and there is no local APIC
// bring-up here, so this is concurrency, not parallelism. The consequence is
// stated at every lock: with preemption arriving only from the timer, masking
// interrupts IS mutual exclusion, and a spin loop would be strictly worse.
// On more than one core that stops being true and each lock needs a real
// atomic test-and-set -- the places are marked.

#ifndef NANO_THREAD_H
#define NANO_THREAD_H

extern long irq_save();
extern void irq_restore(long were_enabled);
extern void switch_to_first(long rsp);

#define MAX_THREADS   32
#define THREAD_STACK  32768        // 8 pages each

#define STACK_CANARY 0x5441434B47554152   // "TACKGUAR"

#define T_UNUSED   0
#define T_READY    1
#define T_RUNNING  2
#define T_BLOCKED  3
#define T_DONE     4

struct Thread {
    long rsp;                  // saved stack pointer while not running
    long state;
    long id;
    long stack_base;           // for freeing, and for a stack-overflow check
    long entry;
    long arg;
    long retval;
    long joiner;               // id of a thread waiting on this one, or -1
    long slices;               // how many times it has been scheduled
    char name[32];
};

struct Thread g_threads[MAX_THREADS];
long g_current;                // index of the running thread
long g_nthreads;
long g_switches;
long g_sched_on;               // 0 until the scheduler is allowed to preempt

// The stack a new thread starts on has to look exactly like one that was
// interrupted, because that is the only way back into a thread. From low to
// high: the fifteen registers isr_common pushes, then the vector and error
// code it adds, then the five words the CPU itself pushes for iretq.
//
// Getting the order wrong here produces a thread that starts with garbage in
// its registers and jumps somewhere arbitrary -- which, before the exception
// reporter existed, would have been a silent reboot.
long thread_build_stack(long stack_top, long entry, long arg) {
    long *s;
    long i;

    s = (long *)stack_top;

    // the CPU's own iretq frame, highest first
    s = s - 1; s[0] = 0x10;                  // ss
    s = s - 1; s[0] = stack_top;             // rsp after iretq
    s = s - 1; s[0] = 0x202;                 // rflags, with IF set so the
                                             // thread runs interruptible
    s = s - 1; s[0] = 0x08;                  // cs
    s = s - 1; s[0] = entry;                 // rip

    s = s - 1; s[0] = 0;                     // error code
    s = s - 1; s[0] = 32;                    // vector (looks like a timer IRQ)

    // the fifteen general registers, in the order isr_common pushes them:
    // rax first (highest address), r15 last (lowest)
    s = s - 1; s[0] = 0;                     // rax
    s = s - 1; s[0] = 0;                     // rbx
    s = s - 1; s[0] = 0;                     // rcx
    s = s - 1; s[0] = 0;                     // rdx
    s = s - 1; s[0] = 0;                     // rsi
    s = s - 1; s[0] = arg;                   // rdi = the thread's argument
    s = s - 1; s[0] = 0;                     // rbp
    i = 0;
    while (i < 8) { s = s - 1; s[0] = 0; i = i + 1; }   // r8..r15

    return (long)s;
}

long thread_find_slot() {
    long i;
    i = 0;
    while (i < MAX_THREADS) {
        if (g_threads[i].state == T_UNUSED || g_threads[i].state == T_DONE) return i;
        i = i + 1;
    }
    return -1;
}

void thread_name(long id, char *nm) {
    long i;
    i = 0;
    while (i < 31 && nm[i]) { g_threads[id].name[i] = nm[i]; i = i + 1; }
    g_threads[id].name[i] = 0;
}

// Returns a thread id, or -1. The entry point is passed as an address rather
// than a function pointer, because nano_cc has none -- callers write
// (long)&func... which it also has no syntax for, so entry points are exported
// from the caller with a small accessor. See threads.c.
long thread_create(long entry, long arg, char *nm) {
    long slot;
    long stack;
    long flags;

    flags = irq_save();
    slot = thread_find_slot();
    if (slot < 0) { irq_restore(flags); return -1; }

    stack = (long)kmalloc(THREAD_STACK);
    if (!stack) { irq_restore(flags); return -1; }

    g_threads[slot].stack_base = stack;
    // A canary at the low end of the stack. A thread that runs off the bottom
    // corrupts whatever kmalloc put there, and the symptom appears somewhere
    // else entirely; this turns it into a message naming the thread.
    {
        long *canary;
        canary = (long *)stack;
        canary[0] = STACK_CANARY;
    }
    g_threads[slot].entry = entry;
    g_threads[slot].arg = arg;
    g_threads[slot].retval = 0;
    g_threads[slot].joiner = -1;
    g_threads[slot].slices = 0;
    g_threads[slot].id = slot;
    thread_name(slot, nm);
    // 16-byte align the top, and leave a word spare: the ABI wants rsp+8
    // aligned to 16 at a function's first instruction.
    g_threads[slot].rsp = thread_build_stack((stack + THREAD_STACK) & ~15, entry, arg);
    g_threads[slot].state = T_READY;
    if (slot >= g_nthreads) g_nthreads = slot + 1;

    irq_restore(flags);
    return slot;
}

// Pick the next runnable thread, round-robin from the one after the current.
// Returns -1 if nothing at all can run, which on this kernel means everything
// is blocked and only an interrupt can change that.
long sched_pick() {
    long i;
    long n;
    n = 0;
    i = g_current;
    while (n < MAX_THREADS) {
        i = i + 1;
        if (i >= MAX_THREADS) i = 0;
        if (g_threads[i].state == T_READY || g_threads[i].state == T_RUNNING) return i;
        n = n + 1;
    }
    return -1;
}

// Called from the timer interrupt with the current thread's saved stack.
// Returns the stack to resume on -- the same one if no switch is wanted.
long g_stack_smashed;

long sched_switch(long cur_rsp) {
    long next;

    if (!g_sched_on) return cur_rsp;

    // Check the outgoing thread's canary while we are already touching it.
    {
        long *canary;
        canary = (long *)g_threads[g_current].stack_base;
        if (g_threads[g_current].stack_base && canary[0] != STACK_CANARY) {
            g_stack_smashed = g_stack_smashed + 1;
            printf("\n*** thread %d (%s) overflowed its stack\n",
                   g_current, g_threads[g_current].name);
            g_threads[g_current].state = T_DONE;
        }
    }

    g_threads[g_current].rsp = cur_rsp;
    if (g_threads[g_current].state == T_RUNNING) g_threads[g_current].state = T_READY;

    next = sched_pick();
    if (next < 0) return cur_rsp;             // nothing runnable; carry on
    if (next == g_current) {
        g_threads[g_current].state = T_RUNNING;
        return cur_rsp;
    }

    g_current = next;
    g_threads[next].state = T_RUNNING;
    g_threads[next].slices = g_threads[next].slices + 1;
    g_switches = g_switches + 1;
    return g_threads[next].rsp;
}

// Give up the rest of this time slice. Implemented as a software interrupt on
// the timer's own vector, so it goes through exactly the same path a real
// preemption does -- one code path to be right rather than two.
extern void yield_now();

void thread_yield() {
    if (!g_sched_on) return;
    yield_now();
}

void thread_exit(long value) {
    long flags;
    flags = irq_save();
    g_threads[g_current].retval = value;
    g_threads[g_current].state = T_DONE;
    if (g_threads[g_current].joiner >= 0) {
        long j;
        j = g_threads[g_current].joiner;
        if (g_threads[j].state == T_BLOCKED) g_threads[j].state = T_READY;
    }
    irq_restore(flags);
    // Nothing more to do on this stack; the next tick will schedule elsewhere
    // and never come back here.
    for (;;) thread_yield();
}

long thread_join(long id) {
    long r;
    long f;
    if (id < 0 || id >= MAX_THREADS) return 0;
    while (g_threads[id].state != T_DONE && g_threads[id].state != T_UNUSED) {
        g_threads[id].joiner = g_current;
        thread_yield();
    }
    r = g_threads[id].retval;

    // Reclaim the stack here rather than in thread_exit. A thread cannot free
    // the stack it is standing on -- the very next call would write into
    // memory the heap has already handed to somebody else. The joiner is the
    // first point at which the stack is provably idle.
    //
    // A consequence worth knowing: a thread that is never joined leaks its
    // stack. That is the same bargain pthreads makes, and the reason detach
    // exists.
    f = irq_save();
    if (g_threads[id].stack_base) {
        kfree((void *)g_threads[id].stack_base);
        g_threads[id].stack_base = 0;
    }
    irq_restore(f);
    return r;
}

// Turn the scheduler on and jump into the first thread. Does not return.
void sched_start() {
    long first;
    cli_();
    first = sched_pick();
    if (first < 0) return;
    g_current = first;
    g_threads[first].state = T_RUNNING;
    g_threads[first].slices = 1;
    g_sched_on = 1;
    switch_to_first(g_threads[first].rsp);
}

void thread_init() {
    long i;
    i = 0;
    while (i < MAX_THREADS) { g_threads[i].state = T_UNUSED; i = i + 1; }
    g_current = 0;
    g_nthreads = 0;
    g_switches = 0;
    g_sched_on = 0;
    g_stack_smashed = 0;
}

// ---------- locks ----------
//
// On one core with preemption only from the timer, masking interrupts IS
// mutual exclusion: nothing else can be running to contend for the lock. That
// makes this both correct and cheaper than any spin loop.
//
// >>> On more than one core this is NOT enough. The other cores keep running
// >>> with interrupts of their own, and each of these needs a real atomic
// >>> test-and-set on `held`. The field is already here for that.
struct Spinlock {
    long held;
    long flags;
    long owner;
};

void spin_init(struct Spinlock *l) { l->held = 0; l->flags = 0; l->owner = -1; }

void spin_lock(struct Spinlock *l) {
    long f;
    f = irq_save();
    // <<< SMP: an atomic exchange on l->held goes here, spinning until it wins
    l->held = 1;
    l->flags = f;
    l->owner = g_current;
}

void spin_unlock(struct Spinlock *l) {
    long f;
    f = l->flags;
    l->held = 0;
    l->owner = -1;
    irq_restore(f);
}

// A mutex a thread can hold across a yield. Interrupts stay ON while it is
// held, so the holder can be preempted -- which is the whole point, and also
// why it cannot simply be a spinlock.
struct Mutex {
    long locked;
    long owner;
    long waiters;
};

void mutex_init(struct Mutex *m) { m->locked = 0; m->owner = -1; m->waiters = 0; }

void mutex_lock(struct Mutex *m) {
    for (;;) {
        long f;
        f = irq_save();
        if (!m->locked) {
            m->locked = 1;
            m->owner = g_current;
            irq_restore(f);
            return;
        }
        m->waiters = m->waiters + 1;
        irq_restore(f);
        // Yield rather than spin: on one core, spinning here burns the rest of
        // a slice that the holder needs in order to release it.
        thread_yield();
    }
}

long mutex_trylock(struct Mutex *m) {
    long f;
    long got;
    f = irq_save();
    got = 0;
    if (!m->locked) { m->locked = 1; m->owner = g_current; got = 1; }
    irq_restore(f);
    return got;
}

void mutex_unlock(struct Mutex *m) {
    long f;
    f = irq_save();
    m->locked = 0;
    m->owner = -1;
    irq_restore(f);
}

// ---------- a pthreads-shaped surface ----------
// The core of the API, with the same meanings. Not the whole of pthreads --
// detach, cancellation, barriers, thread-local storage and attributes are not
// here, and calling this "pthreads" without saying that would be a lie.
typedef long pthread_t;
typedef struct Mutex pthread_mutex_t;

long pthread_create(pthread_t *out, long entry, long arg) {
    long id;
    id = thread_create(entry, arg, "pthread");
    if (id < 0) return -1;
    if (out) *out = id;
    return 0;
}

long pthread_join(pthread_t id, long *retval) {
    long r;
    r = thread_join(id);
    if (retval) *retval = r;
    return 0;
}

pthread_t pthread_self() { return g_current; }
void pthread_exit(long v) { thread_exit(v); }
long pthread_yield_() { thread_yield(); return 0; }

long pthread_mutex_init(pthread_mutex_t *m) { mutex_init(m); return 0; }
long pthread_mutex_lock(pthread_mutex_t *m) { mutex_lock(m); return 0; }
long pthread_mutex_trylock(pthread_mutex_t *m) { return mutex_trylock(m) ? 0 : -1; }
long pthread_mutex_unlock(pthread_mutex_t *m) { mutex_unlock(m); return 0; }

#endif
