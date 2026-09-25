// nano-srv.h — servers and a supervisor that restarts them.
//
// Compiled by nano_cc with --kernel. Include AFTER nano-thread.h.
//
// WHAT THIS IS AND IS NOT. This is the microkernel *structure*: named services
// with their own threads, a registry, health checks, and a supervisor that
// restarts anything that dies or stops answering. A fault inside a server now
// kills that server rather than the machine (see thread_fault_kill).
//
// It is NOT isolation. Every server here runs in ring 0 in the SAME address
// space, so:
//
//   * a crash is contained -- the supervisor sees it and restarts
//   * a hang is contained -- the heartbeat goes stale and the supervisor acts
//   * CORRUPTION IS NOT CONTAINED. A wild pointer in one server lands in
//     another's data, that server keeps answering its health check perfectly,
//     and the damage is already done by the time anything notices.
//
// That last line is the whole reason real microkernels give each server its
// own address space and run them unprivileged. Health checks catch crashes and
// hangs; only address spaces catch corruption. Calling this fault-tolerant
// without saying which of the three it handles would be a lie.
//
// There is also a dependency nobody can design away: a supervisor cannot
// restart the thing it needs in order to restart anything. This one allocates
// a stack from the heap, so it can restart a filesystem or a driver, and it
// could not restart the memory manager. MINIX 3 has the same problem and
// handles the VM server specially for exactly this reason.

#ifndef NANO_SRV_H
#define NANO_SRV_H

#define MAX_SERVERS 12

#define S_EMPTY    0
#define S_STOPPED  1
#define S_RUNNING  2
#define S_FAILED   3       // crashed, or stopped answering

struct Server {
    long entry;            // address of the server's main loop
    long arg;
    long state;
    long thread;           // -1 when not running
    long restarts;
    long faults;           // how many died from a CPU exception
    long hangs;            // how many were restarted for going quiet
    long beat;             // the server bumps this; the supervisor watches it
    long last_beat;
    long last_check;       // tick of the last supervisor look
    long deadline;         // ticks allowed between beats, 0 = do not check
    char name[24];
};

struct Server g_srv[MAX_SERVERS];
long g_nsrv;
long g_sup_thread;
long g_sup_checks;
long g_sup_restarts;
long g_sup_on;

void srv_name(long i, char *nm) {
    long k;
    k = 0;
    while (k < 23 && nm[k]) { g_srv[i].name[k] = nm[k]; k = k + 1; }
    g_srv[i].name[k] = 0;
}

long srv_register(char *nm, long entry, long arg, long deadline_ticks) {
    long i;
    i = 0;
    while (i < MAX_SERVERS) {
        if (g_srv[i].state == S_EMPTY) {
            g_srv[i].entry = entry;
            g_srv[i].arg = arg;
            g_srv[i].thread = -1;
            g_srv[i].restarts = 0;
            g_srv[i].faults = 0;
            g_srv[i].hangs = 0;
            g_srv[i].beat = 0;
            g_srv[i].last_beat = 0;
            g_srv[i].last_check = 0;
            g_srv[i].deadline = deadline_ticks;
            g_srv[i].state = S_STOPPED;
            srv_name(i, nm);
            if (i >= g_nsrv) g_nsrv = i + 1;
            return i;
        }
        i = i + 1;
    }
    return -1;
}

// Which server is a given thread running? -1 if none. The supervisor needs
// this to tell an ordinary thread's death from a server's.
long srv_of_thread(long tid) {
    long i;
    i = 0;
    while (i < g_nsrv) {
        if (g_srv[i].thread == tid && g_srv[i].state == S_RUNNING) return i;
        i = i + 1;
    }
    return -1;
}

// A server calls this from its loop to say it is still alive. A server that
// stops calling it has hung, whether or not it has crashed.
void srv_beat(long id) {
    if (id >= 0 && id < MAX_SERVERS) g_srv[id].beat = g_srv[id].beat + 1;
}

// The id of the server the calling thread belongs to, so a server's loop does
// not have to be told its own number.
long srv_self() { return srv_of_thread(g_current); }

long srv_start(long i) {
    long t;
    if (i < 0 || i >= MAX_SERVERS) return 0;
    if (g_srv[i].state == S_RUNNING) return 1;
    t = thread_create(g_srv[i].entry, g_srv[i].arg, g_srv[i].name);
    if (t < 0) return 0;
    g_srv[i].thread = t;
    g_srv[i].state = S_RUNNING;
    g_srv[i].beat = 0;
    g_srv[i].last_beat = 0;
    g_srv[i].last_check = g_ticks;
    return 1;
}

void srv_stop(long i) {
    long f;
    if (i < 0 || i >= MAX_SERVERS) return;
    f = irq_save();
    if (g_srv[i].thread >= 0) g_threads[g_srv[i].thread].state = T_DONE;
    g_srv[i].state = S_STOPPED;
    irq_restore(f);
}

// Reclaim what a dead server left behind, then start it again.
//
// The stack is freed here rather than by the thread itself, for the same
// reason join does it: nothing can free the stack it is standing on. If the
// server died holding one of the locks the framework knows about, that is
// released too -- and anything it held that the framework does NOT know about
// stays stuck, which is the limitation address spaces exist to fix.
long srv_restart(long i) {
    long f;
    long old;
    if (i < 0 || i >= MAX_SERVERS) return 0;

    f = irq_save();
    old = g_srv[i].thread;
    if (old >= 0) {
        g_threads[old].state = T_DONE;
        if (g_threads[old].stack_base) {
            kfree((void *)g_threads[old].stack_base);
            g_threads[old].stack_base = 0;
        }
    }
    g_srv[i].thread = -1;
    g_srv[i].state = S_STOPPED;
    g_srv[i].restarts = g_srv[i].restarts + 1;
    irq_restore(f);

    return srv_start(i);
}

// The supervisor. Wakes periodically, and for each running server asks two
// questions: is its thread still alive, and has it produced a heartbeat since
// the last look? Either answer being no means a restart.
//
// It deliberately does NOT ask "is your state correct", because it cannot: a
// server whose memory has been corrupted by a neighbour answers a health check
// perfectly. See the note at the top of this file.
long g_sup_interval;

void supervisor(long unused) {
    for (;;) {
        long i;
        thread_sleep_ms(g_sup_interval);
        if (!g_sup_on) continue;
        g_sup_checks = g_sup_checks + 1;

        i = 0;
        while (i < g_nsrv) {
            if (g_srv[i].state == S_RUNNING) {
                long t;
                long dead;
                long quiet;
                t = g_srv[i].thread;
                dead = 0;
                quiet = 0;

                if (t < 0 || g_threads[t].state == T_DONE ||
                    g_threads[t].state == T_UNUSED) dead = 1;

                if (!dead && g_srv[i].deadline > 0 &&
                    g_ticks - g_srv[i].last_check >= g_srv[i].deadline) {
                    if (g_srv[i].beat == g_srv[i].last_beat) quiet = 1;
                    g_srv[i].last_beat = g_srv[i].beat;
                    g_srv[i].last_check = g_ticks;
                }

                if (dead || quiet) {
                    if (dead && t >= 0 && g_threads[t].faulted)
                        g_srv[i].faults = g_srv[i].faults + 1;
                    if (quiet) g_srv[i].hangs = g_srv[i].hangs + 1;
                    printf("supervisor: %s %s, restarting\n",
                           g_srv[i].name, dead ? "died" : "stopped answering");
                    g_srv[i].state = S_FAILED;
                    g_sup_restarts = g_sup_restarts + 1;
                    srv_restart(i);
                }
            }
            i = i + 1;
        }
    }
}

char *srv_state_name(long st) {
    if (st == S_EMPTY) return "empty  ";
    if (st == S_STOPPED) return "stopped";
    if (st == S_RUNNING) return "running";
    return "failed ";
}

long srv_init(long interval_ms) {
    long i;
    i = 0;
    while (i < MAX_SERVERS) { g_srv[i].state = S_EMPTY; i = i + 1; }
    g_nsrv = 0;
    g_sup_checks = 0;
    g_sup_restarts = 0;
    g_sup_interval = interval_ms;
    g_sup_on = 1;
    g_sup_thread = thread_create((long)supervisor, 0, "supervisor");
    return g_sup_thread >= 0;
}

#endif
