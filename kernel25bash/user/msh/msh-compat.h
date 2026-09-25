// msh-compat.h — the POSIX surface miniShell is written against, on nanoOS.
//
// miniShell's main.c is a hosted Unix program: it includes <unistd.h>,
// <dirent.h>, <sys/stat.h>, <sys/wait.h> and <signal.h> and calls about
// thirty things that this kernel has never had. This file is the whole of
// that gap, so that main.c itself stays the client's source rather than
// becoming a fork of it -- the build strips its POSIX #includes and puts this
// in their place, exactly the way user/cc.elf is built from simpleC++.c.
//
// It sits ON TOP of nano-libc.h (malloc, printf, FILE, ctype) and os-base.h
// (read/write/open/close/lseek, the string and memory functions). Only what
// those two do not already provide is here.
//
// THREE GROUPS, and the difference matters when reading what the port can do:
//
//   1. Real, backed by a syscall that exists. open, read, write, pipe, spawn,
//      wait, readdir, mkdir, chdir. These behave.
//
//   2. Honestly degenerate. There are no signals to deliver, so signal() is a
//      table that records the handler and never calls it; isatty() is true for
//      0/1/2 because the shell's own pane is the only terminal there is.
//      These return plausible values because the alternative -- failing -- is
//      a lie of the opposite kind.
//
//   3. ABSENT, and loudly. fork() returns -1. There is no copy-on-write in
//      this kernel, so a child that keeps running the shell's own code cannot
//      be made; the four call sites that need it are subshells, $(...),
//      here-documents and background jobs. They report by name rather than
//      failing as a generic error, because the parser still ACCEPTS that
//      syntax and a silent failure there looks like a bug in the user's
//      script rather than a missing feature.

#ifndef MSH_COMPAT_H
#define MSH_COMPAT_H

// ---------- syscall numbers this kernel grew after os-base.h was written ----
#define SYS_TRUNCATE 19
#define SYS_SYNC     20
#define SYS_MKDIR    21
#define SYS_RENAME   22
#define SYS_READDIR  23
#define SYS_ISDIR    24
#define SYS_SPAWN    25
#define SYS_WAIT     26
#define SYS_PIPE     27
#define SYS_DUP      28
#define SYS_DUP2     29

// The two errno values miniShell tests for. os-base.h has the `errno`
// variable but no names for what it can hold. EINTR can never actually occur
// here -- a syscall cannot be interrupted by a signal that cannot be
// delivered -- so the retry loops guarded by it simply never retry.
#define EINTR   4
#define ENOENT  2

typedef long pid_t;
typedef long mode_t;
typedef long off_t;
typedef long time_t;
typedef long ssize_t;

// ---------- the pieces of <sys/stat.h> miniShell actually touches ----------
// It reads st_mode, st_size and st_mtime and nothing else, so the struct is
// those three. A full stat buffer here would be padding that no code reads
// and that nothing can fill in truthfully.
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_IFIFO  0010000
#define S_IFBLK  0060000
#define S_IFCHR  0020000

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)

struct stat {
    long st_mode;
    long st_size;
    long st_mtime;
};

// There are no symlinks and no mtimes in this filesystem. st_mtime is 0 for
// every file, which miniShell uses only for sorting -- a stable order rather
// than a wrong one.
int stat(const char *path, struct stat *st) {
    long ino;
    if (!st) return -1;
    ino = syscall4(SYS_ISDIR, (long)path, 0, 0);
    if (ino < 0) return -1;
    st->st_mtime = 0;
    if (ino > 0) { st->st_mode = S_IFDIR; st->st_size = 0; return 0; }
    {
        int fd;
        fd = open(path, O_RDONLY);
        if (fd < 0) return -1;
        st->st_mode = S_IFREG;
        st->st_size = syscall1(SYS_fsize, fd);
        close(fd);
        return 0;
    }
}
// No symlinks exist, so lstat and stat cannot disagree.
int lstat(const char *path, struct stat *st) { return stat(path, st); }

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
// Every file is readable and writable by the only user there is, so access()
// is a question about existence however it is asked.
int access(const char *path, int mode) {
    struct stat sb;
    if (mode) { /* mode is deliberately not consulted; see above */ }
    return stat(path, &sb) == 0 ? 0 : -1;
}

// ---------- <dirent.h> ----------
// SYS_READDIR returns the INODE of entry `index`, or 0 when there are no more,
// and writes the name into the caller's buffer. It does NOT return a boolean:
// treating a nonzero inode as "true" and 1 as "the only valid answer" is a
// mistake I have already made once here, and it made every listing look empty.
#define MSH_NAME_MAX 128

struct dirent {
    char d_name[MSH_NAME_MAX];
};

typedef struct {
    int  fd;
    long index;
    struct dirent ent;
    char path[256];
} DIR;

DIR g_msh_dirs[8];
long g_msh_dir_used[8];

DIR *opendir(const char *path) {
    long i;
    long ino;
    ino = syscall4(SYS_ISDIR, (long)path, 0, 0);
    if (ino <= 0) return 0;                 // not a directory, or absent
    i = 0;
    while (i < 8) {
        if (!g_msh_dir_used[i]) {
            long k;
            g_msh_dir_used[i] = 1;
            g_msh_dirs[i].index = 0;
            g_msh_dirs[i].fd = (int)i;
            k = 0;
            while (path[k] && k < 255) { g_msh_dirs[i].path[k] = path[k]; k = k + 1; }
            g_msh_dirs[i].path[k] = 0;
            return &g_msh_dirs[i];
        }
        i = i + 1;
    }
    return 0;
}

struct dirent *readdir(DIR *d) {
    long ino;
    if (!d) return 0;
    ino = syscall4(SYS_READDIR, (long)d->path, d->index, (long)d->ent.d_name);
    if (ino <= 0) return 0;
    d->index = d->index + 1;
    return &d->ent;
}

int closedir(DIR *d) {
    if (!d) return -1;
    g_msh_dir_used[d->fd] = 0;
    return 0;
}

// ---------- directories and the working directory ----------
int mkdir(const char *path, mode_t mode) {
    if (mode) { }
    return (int)syscall4(SYS_MKDIR, (long)path, 0, 0);
}
int rmdir(const char *path) { return unlink(path); }

char g_msh_cwd[256];

char *getcwd(char *buf, long size) {
    long i;
    if (!g_msh_cwd[0]) { g_msh_cwd[0] = '/'; g_msh_cwd[1] = 0; }
    if (!buf) return g_msh_cwd;
    i = 0;
    while (g_msh_cwd[i] && i < size - 1) { buf[i] = g_msh_cwd[i]; i = i + 1; }
    buf[i] = 0;
    return buf;
}

int chdir(const char *path) {
    long ino;
    long i;
    ino = syscall4(SYS_ISDIR, (long)path, 0, 0);
    if (ino <= 0) return -1;
    i = 0;
    while (path[i] && i < 255) { g_msh_cwd[i] = path[i]; i = i + 1; }
    g_msh_cwd[i] = 0;
    return 0;
}

// The kernel's pipe buffer. A writer that exceeds it blocks until a reader
// drains it, which is fine between two running processes and a deadlock when
// the shell is filling a pipe nobody is reading yet.
#define MSH_PIPE_CAP 4096

// ---------- processes ----------
long pipe(int *fds) {
    long tmp[2];
    long r;
    r = syscall4(SYS_PIPE, (long)tmp, 0, 0);
    if (r < 0) return -1;
    fds[0] = (int)tmp[0];
    fds[1] = (int)tmp[1];
    return 0;
}

// The shell polls rather than blocks: SYS_WAIT reports -1 while the child is
// still alive, so "wait" here means yield until it is not.
long msh_wait_pid(long pid) {
    long st;
    for (;;) {
        st = syscall4(SYS_WAIT, pid, 0, 0);
        if (st >= 0) return st;
        syscall4(SYS_yield, 0, 0, 0);
    }
}

#define WNOHANG 1
int msh_last_status;

pid_t waitpid(pid_t pid, int *status, int options) {
    long st;
    if (pid <= 0) return -1;
    if (options & WNOHANG) {
        st = syscall4(SYS_WAIT, pid, 0, 0);
        if (st < 0) return 0;                       // still running
    } else {
        st = msh_wait_pid(pid);
    }
    // The exit code, shifted the way the W* macros below expect it.
    if (status) *status = (int)((st & 0xFF) << 8);
    return pid;
}

#define WIFEXITED(s)    1
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  0
#define WTERMSIG(s)     0
#define WIFSTOPPED(s)   0
#define WSTOPSIG(s)     0

// fork(): see the header comment. -1 is the truth here, not a placeholder.
pid_t fork(void) { return -1; }

void _exit(int code) { exit(code); }

int getpid(void) { return (int)syscall4(SYS_getpid, 0, 0, 0); }

// dup and dup2 are real: the kernel grew them for this port. They are what
// lets a BUILTIN be redirected -- the shell saves its own fd 1, points it at
// the file, runs the builtin in its own process, and puts fd 1 back. There is
// no child involved, so spawn's descriptor arguments do not help there.
//
// One difference from POSIX, worth knowing: the pair do NOT share a file
// offset. See the note in nano-proc.h. Every use here closes one of the two
// straight away, so it does not arise.
int dup(int fd)        { return (int)syscall4(SYS_DUP,  fd, 0, 0); }
int dup2(int a, int b) { return (int)syscall4(SYS_DUP2, a,  b, 0); }

int execvp(const char *path, char **argv) {
    if (path || argv) { }
    return -1;
}

// SYS_SPAWN needs five arguments and os-base.h only declares syscall4.
// ustart.s has had syscall6 all along; this is the declaration for it.
extern long syscall6(long nr, long a, long b, long c, long d, long e);

// argv[] straight through to SYS_SPAWN, with the child's stdout and stdin
// named explicitly. -1 for either means "inherit the console".
long msh_spawn(const char *path, long argc, char **argv, long outfd, long infd) {
    return syscall6(SYS_SPAWN, (long)path, argc, (long)argv, outfd, infd);
}

// ---------- signals: recorded, never delivered ----------
#define SIGINT   2
#define SIGQUIT  3
#define SIGPIPE 13
#define SIG_DFL 0
#define SIG_IGN 1

long g_msh_sig[32];

// Returns the previous handler, as signal() does, so code that saves and
// restores it behaves. Nothing ever raises these -- there is no mechanism to
// deliver them -- so a handler set here is recorded and not run. Ctrl-C is
// handled by the shell's own input loop instead.
long signal(int sig, long handler) {
    long prev;
    if (sig < 0 || sig >= 32) return -1;
    prev = g_msh_sig[sig];
    g_msh_sig[sig] = handler;
    return prev;
}

// ---------- environment ----------
// A fixed table rather than a grown one: the shell sets a handful of variables
// and this avoids a malloc'd environ that nothing ever frees.
#define MSH_ENV_MAX 32
char g_msh_env_name[MSH_ENV_MAX][64];
char g_msh_env_val[MSH_ENV_MAX][256];
long g_msh_env_used[MSH_ENV_MAX];

long msh_env_find(const char *name) {
    long i;
    i = 0;
    while (i < MSH_ENV_MAX) {
        if (g_msh_env_used[i] && !strcmp(g_msh_env_name[i], name)) return i;
        i = i + 1;
    }
    return -1;
}

char *getenv(const char *name) {
    long i;
    i = msh_env_find(name);
    if (i < 0) return 0;
    return g_msh_env_val[i];
}

int setenv(const char *name, const char *val, int overwrite) {
    long i;
    long k;
    i = msh_env_find(name);
    if (i >= 0 && !overwrite) return 0;
    if (i < 0) {
        i = 0;
        while (i < MSH_ENV_MAX && g_msh_env_used[i]) i = i + 1;
        if (i >= MSH_ENV_MAX) return -1;
        g_msh_env_used[i] = 1;
        k = 0;
        while (name[k] && k < 63) { g_msh_env_name[i][k] = name[k]; k = k + 1; }
        g_msh_env_name[i][k] = 0;
    }
    k = 0;
    while (val[k] && k < 255) { g_msh_env_val[i][k] = val[k]; k = k + 1; }
    g_msh_env_val[i][k] = 0;
    return 0;
}

int unsetenv(const char *name) {
    long i;
    i = msh_env_find(name);
    if (i >= 0) g_msh_env_used[i] = 0;
    return 0;
}

// ---------- odds and ends ----------
// The shell's own pane is the only terminal, and it is where 0, 1 and 2 go
// unless something redirected them. Reporting "not a tty" would switch
// miniShell into its non-interactive mode and turn off the prompt.
int isatty(int fd) { return (fd >= 0 && fd <= 2) ? 1 : 0; }

int fileno(FILE *f) {
    if (f == stdin)  return 0;
    if (f == stdout) return 1;
    if (f == stderr) return 2;
    return -1;
}

// Output is unbuffered already (see nano-libc.h), so a buffering request is
// satisfied by definition rather than ignored. _IONBF is therefore the only
// mode that is honestly available, and it is the one miniShell asks for.
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
int setvbuf(FILE *f, char *buf, int mode, long size) {
    if (f || buf || mode || size) { }
    return 0;
}

long strtol(const char *s, char **end, int base) {
    long sign;
    long v;
    long i;
    sign = 1;
    v = 0;
    i = 0;
    while (s[i] == ' ' || s[i] == '\t') i = i + 1;
    if (s[i] == '-') { sign = -1; i = i + 1; }
    else if (s[i] == '+') i = i + 1;
    if (base == 0) {
        // 0x -> 16, leading 0 -> 8, otherwise 10. Same rule as C's, because
        // the callers here pass 0 and expect it.
        if (s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) { base = 16; i = i + 2; }
        else if (s[i] == '0' && s[i+1]) { base = 8; i = i + 1; }
        else base = 10;
    } else if (base == 16 && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) {
        i = i + 2;
    }
    for (;;) {
        long d;
        d = -1;
        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else if (s[i] >= 'a' && s[i] <= 'z') d = s[i] - 'a' + 10;
        else if (s[i] >= 'A' && s[i] <= 'Z') d = s[i] - 'A' + 10;
        if (d < 0 || d >= base) break;
        v = v * base + d;
        i = i + 1;
    }
    if (end) *end = (char *)(s + i);
    return sign * v;
}

// nano_cc parses `unsigned` and then ignores it, so there is no unsigned type
// to return and no point pretending. Callers here use it for small counts.
long strtoul(const char *s, char **end, int base) { return strtol(s, end, base); }

// An insertion sort, not qsort's usual quicksort. nano_cc has no function
// pointers, so a comparator cannot be passed at all; every caller in main.c
// sorts strings, and this sorts strings. Left named qsort so the call sites
// read the same, but it ignores the comparator argument -- which is why the
// ORDER it produces is fixed rather than caller-chosen.
void qsort(void *base, long n, long size, long cmp) {
    char **v;
    long i;
    if (cmp) { }
    if (size != 8) return;               // only arrays of pointers are sorted
    v = (char **)base;
    i = 1;
    while (i < n) {
        char *key;
        long j;
        key = v[i];
        j = i - 1;
        while (j >= 0 && strcmp(v[j], key) > 0) { v[j+1] = v[j]; j = j - 1; }
        v[j+1] = key;
        i = i + 1;
    }
}

// getline() over a FILE*, allocating on first use and growing as needed.
long getline(char **lineptr, long *n, FILE *f) {
    long len;
    int c;
    if (!lineptr || !n) return -1;
    if (!*lineptr) { *n = 128; *lineptr = (char *)malloc(*n); if (!*lineptr) return -1; }
    len = 0;
    for (;;) {
        c = fgetc(f);
        if (c < 0) break;
        if (len + 2 > *n) {
            char *bigger;
            bigger = (char *)realloc(*lineptr, *n * 2);
            if (!bigger) return -1;
            *lineptr = bigger;
            *n = *n * 2;
        }
        (*lineptr)[len] = (char)c;
        len = len + 1;
        if (c == '\n') break;
    }
    if (len == 0) return -1;             // EOF with nothing read
    (*lineptr)[len] = 0;
    return len;
}

#endif
