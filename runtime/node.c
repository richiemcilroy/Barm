/* node.c — the natives Node.js's built-ins run on (runtime/node), as functions on
 * globalThis.__barm_native: process, timers (on Barm's event loop), os, tty and fs. Linked with
 * js.c by programs that import npm packages. The JS side of each is documented where it's used
 * (runtime/node/internal/bootstrap/process.js, internal/bindings/timers.js, ...os.js, ...fs.js).
 *
 * Errors: a failing call throws what Node.js throws, an Error with errno (negative), code,
 * syscall and path, and the message libuv gives ("ENOENT: no such file or directory, open 'x'"). */

#include "js.h"

#include <JavaScriptCore/JavaScriptCore.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#else
#include <linux/if_packet.h>
#include <sys/sysinfo.h>
#endif

extern char **environ;

#define NATIVE(name) static JSValueRef name(JSContextRef ctx, JSObjectRef fn_, JSObjectRef self_, size_t n, const JSValueRef a[], JSValueRef *exc)
#define UNUSED (void)fn_; (void)self_; (void)n; (void)a; (void)exc

/* ------------------------------------------------------------------ values */

static JSValueRef undef(JSContextRef ctx) { return JSValueMakeUndefined(ctx); }
static JSValueRef num(JSContextRef ctx, double d) { return JSValueMakeNumber(ctx, d); }

static JSValueRef str(JSContextRef ctx, const char *s) {
    (void)ctx;
    return bm_js_str(s, strlen(s));
}

static double arg_num(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i, double dflt) {
    if (i >= n || JSValueIsUndefined(ctx, a[i])) return dflt;
    double d = JSValueToNumber(ctx, a[i], NULL);
    return d != d ? dflt : d;
}

/* argument i as a NUL-terminated UTF-8 string (malloc'd; NULL if it's not a string) */
static char *arg_cstr(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i) {
    if (i >= n || !JSValueIsString(ctx, a[i])) return NULL;
    JSStringRef s = JSValueToStringCopy(ctx, a[i], NULL);
    size_t cap = JSStringGetMaximumUTF8CStringSize(s);
    char *out = malloc(cap);
    JSStringGetUTF8CString(s, out, cap);
    JSStringRelease(s);
    return out;
}

static void set(JSContextRef ctx, JSObjectRef obj, const char *key, JSValueRef v) {
    JSStringRef k = JSStringCreateWithUTF8CString(key);
    JSObjectSetProperty(ctx, obj, k, v, kJSPropertyAttributeNone, NULL);
    JSStringRelease(k);
}

static JSObjectRef array(JSContextRef ctx, size_t count, const JSValueRef *items) {
    return JSObjectMakeArray(ctx, count, items, NULL);
}

/* ------------------------------------------------------------------ errors, as libuv names them */

static const struct { int err; const char *code, *message; } bm_uv_errors[] = {
    {E2BIG, "E2BIG", "argument list too long"},
    {EACCES, "EACCES", "permission denied"},
    {EADDRINUSE, "EADDRINUSE", "address already in use"},
    {EADDRNOTAVAIL, "EADDRNOTAVAIL", "address not available"},
    {EAGAIN, "EAGAIN", "resource temporarily unavailable"},
    {EBADF, "EBADF", "bad file descriptor"},
    {EBUSY, "EBUSY", "resource busy or locked"},
    {ECANCELED, "ECANCELED", "operation canceled"},
    {ECONNABORTED, "ECONNABORTED", "software caused connection abort"},
    {ECONNREFUSED, "ECONNREFUSED", "connection refused"},
    {ECONNRESET, "ECONNRESET", "connection reset by peer"},
    {EEXIST, "EEXIST", "file already exists"},
    {EFAULT, "EFAULT", "bad address in system call argument"},
    {EFBIG, "EFBIG", "file too large"},
    {EHOSTUNREACH, "EHOSTUNREACH", "host is unreachable"},
    {EINTR, "EINTR", "interrupted system call"},
    {EINVAL, "EINVAL", "invalid argument"},
    {EIO, "EIO", "i/o error"},
    {EISDIR, "EISDIR", "illegal operation on a directory"},
    {ELOOP, "ELOOP", "too many symbolic links encountered"},
    {EMFILE, "EMFILE", "too many open files"},
    {EMLINK, "EMLINK", "too many links"},
    {ENAMETOOLONG, "ENAMETOOLONG", "name too long"},
    {ENFILE, "ENFILE", "file table overflow"},
    {ENODEV, "ENODEV", "no such device"},
    {ENOENT, "ENOENT", "no such file or directory"},
    {ENOMEM, "ENOMEM", "not enough memory"},
    {ENOSPC, "ENOSPC", "no space left on device"},
    {ENOSYS, "ENOSYS", "function not implemented"},
    {ENOTCONN, "ENOTCONN", "socket is not connected"},
    {ENOTDIR, "ENOTDIR", "not a directory"},
    {ENOTEMPTY, "ENOTEMPTY", "directory not empty"},
    {ENOTSUP, "ENOTSUP", "operation not supported on socket"},
    {ENOTTY, "ENOTTY", "inappropriate ioctl for device"},
    {ENXIO, "ENXIO", "no such device or address"},
    {EPERM, "EPERM", "operation not permitted"},
    {EPIPE, "EPIPE", "broken pipe"},
    {ERANGE, "ERANGE", "result too large"},
    {EROFS, "EROFS", "read-only file system"},
    {ESPIPE, "ESPIPE", "invalid seek"},
    {ESRCH, "ESRCH", "no such process"},
    {ETIMEDOUT, "ETIMEDOUT", "connection timed out"},
    {ETXTBSY, "ETXTBSY", "text file is busy"},
    {EXDEV, "EXDEV", "cross-device link not permitted"},
};

static void bm_uv_name(int err, const char **code, const char **message) {
    for (size_t i = 0; i < sizeof bm_uv_errors / sizeof *bm_uv_errors; i++) {
        if (bm_uv_errors[i].err == err) {
            *code = bm_uv_errors[i].code;
            *message = bm_uv_errors[i].message;
            return;
        }
    }
    *code = "UNKNOWN";
    *message = "unknown error";
}

/* The Error Node.js throws for a failed system call (uvException): "CODE: message, syscall
 * 'path' -> 'dest'", with errno (negative), code, syscall, path and dest. */
static JSValueRef bm_node_errno(JSContextRef ctx, int err, const char *syscall, const char *path, const char *dest) {
    const char *code, *message;
    bm_uv_name(err, &code, &message);
    char text[4096];
    int len = snprintf(text, sizeof text, "%s: %s, %s", code, message, syscall);
    if (path && len < (int)sizeof text) len += snprintf(text + len, sizeof text - (size_t)len, " '%s'", path);
    if (dest && len < (int)sizeof text) snprintf(text + len, sizeof text - (size_t)len, " -> '%s'", dest);
    JSValueRef msg = str(ctx, text);
    JSObjectRef e = JSObjectMakeError(ctx, 1, &msg, NULL);
    set(ctx, e, "errno", num(ctx, -err));
    set(ctx, e, "code", str(ctx, code));
    set(ctx, e, "syscall", str(ctx, syscall));
    if (path) set(ctx, e, "path", str(ctx, path));
    if (dest) set(ctx, e, "dest", str(ctx, dest));
    return e;
}

/* throws the errno error; returns undefined for the native to return */
static JSValueRef bm_node_throw(JSContextRef ctx, JSValueRef *exc, int err, const char *syscall, const char *path, const char *dest) {
    *exc = bm_node_errno(ctx, err, syscall, path, dest);
    return undef(ctx);
}

/* For os.js: fills ctx (a JS object) with { errno, code, message, syscall } and returns undefined. */
static JSValueRef bm_node_ctx_error(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i, int err, const char *syscall) {
    if (i < n && JSValueIsObject(ctx, a[i])) {
        JSObjectRef c = (JSObjectRef)a[i];
        const char *code, *message;
        bm_uv_name(err, &code, &message);
        set(ctx, c, "errno", num(ctx, -err));
        set(ctx, c, "code", str(ctx, code));
        set(ctx, c, "message", str(ctx, message));
        set(ctx, c, "syscall", str(ctx, syscall));
    }
    return undef(ctx);
}

/* ------------------------------------------------------------------ process */

static const char *bm_node_platform(void) {
#if defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

static const char *bm_node_arch(void) {
#if defined(__aarch64__)
    return "arm64";
#elif defined(__x86_64__)
    return "x64";
#else
    return "unknown";
#endif
}

static void bm_node_exec_path(char *out, size_t cap) {
#if defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    char raw[4096];
    uint32_t rs = sizeof raw;
    if (_NSGetExecutablePath(raw, &rs) == 0 && realpath(raw, out)) return;
    (void)size;
#else
    ssize_t k = readlink("/proc/self/exe", out, cap - 1);
    if (k > 0) {
        out[k] = 0;
        return;
    }
#endif
    snprintf(out, cap, "%s", bm_argc > 0 ? bm_argv[0] : "barm");
}

NATIVE(n_info) {
    UNUSED;
    JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
    /* Node.js: [node, script, args...]; a Barm program is both */
    size_t count = (size_t)bm_argc + 1;
    JSValueRef *items = malloc(sizeof(JSValueRef) * count);
    char exe[4096];
    bm_node_exec_path(exe, sizeof exe);
    items[0] = str(ctx, exe);
    for (int i = 0; i < bm_argc; i++) items[i + 1] = str(ctx, i == 0 ? exe : bm_argv[i]);
    set(ctx, o, "argv", array(ctx, count, items));
    free(items);
    set(ctx, o, "execArgv", array(ctx, 0, NULL));
    set(ctx, o, "execPath", str(ctx, exe));
    set(ctx, o, "pid", num(ctx, getpid()));
    set(ctx, o, "ppid", num(ctx, getppid()));
    set(ctx, o, "platform", str(ctx, bm_node_platform()));
    set(ctx, o, "arch", str(ctx, bm_node_arch()));
    const char *base = strrchr(exe, '/');
    set(ctx, o, "title", str(ctx, base ? base + 1 : exe));
    JSObjectRef env = JSObjectMake(ctx, NULL, NULL);
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        char key[1024];
        size_t kl = (size_t)(eq - *e) < sizeof key - 1 ? (size_t)(eq - *e) : sizeof key - 1;
        memcpy(key, *e, kl);
        key[kl] = 0;
        set(ctx, env, key, str(ctx, eq + 1));
    }
    set(ctx, o, "env", env);
    return o;
}

NATIVE(n_cwd) {
    UNUSED;
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) return bm_node_throw(ctx, exc, errno, "uv_cwd", NULL, NULL);
    return str(ctx, buf);
}

NATIVE(n_chdir) {
    UNUSED;
    char *path = arg_cstr(ctx, n, a, 0);
    if (!path) return undef(ctx);
    if (chdir(path) != 0) {
        char cwd[4096];
        JSValueRef r = bm_node_throw(ctx, exc, errno, "chdir", getcwd(cwd, sizeof cwd) ? cwd : "", path);
        free(path);
        return r;
    }
    free(path);
    return undef(ctx);
}

NATIVE(n_exit) {
    UNUSED;
    bm_out_flush();
    exit((int)arg_num(ctx, n, a, 0, 0));
}

NATIVE(n_umask) {
    UNUSED;
    double m = arg_num(ctx, n, a, 0, -1);
    mode_t old;
    if (m < 0) {
        old = umask(0);
        umask(old);
    } else {
        old = umask((mode_t)m);
    }
    return num(ctx, old);
}

/* nanoseconds since the program started */
NATIVE(n_hrtime) {
    UNUSED;
    return num(ctx, bm_performance_now() * 1e6);
}

/* write(fd, string | bytes): stdout through Barm's buffer (in order with the program's own
 * output), other fds directly */
NATIVE(n_write) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, 1);
    if (n < 2) return num(ctx, 0);
    uint8_t *p = NULL;
    size_t len = 0;
    char *text = NULL;
    if (JSValueIsString(ctx, a[1])) {
        JSStringRef s = JSValueToStringCopy(ctx, a[1], NULL);
        size_t cap = JSStringGetMaximumUTF8CStringSize(s);
        text = malloc(cap);
        len = JSStringGetUTF8CString(s, text, cap) - 1;
        JSStringRelease(s);
        p = (uint8_t *)text;
    } else if (!bm_js_bytes_view(a[1], &p, &len)) {
        return num(ctx, 0);
    }
    if (fd == 1) {
        bm_out_write((const char *)p, len);
    } else if (fd == 2) {
        bm_out_flush();
        bm_write_fd(2, (const char *)p, len);
    } else {
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(fd, p + off, len - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                free(text);
                return bm_node_throw(ctx, exc, errno, "write", NULL, NULL);
            }
            off += (size_t)w;
        }
    }
    free(text);
    return num(ctx, (double)len);
}

NATIVE(n_isatty) {
    UNUSED;
    return JSValueMakeBoolean(ctx, isatty((int)arg_num(ctx, n, a, 0, -1)) == 1);
}

NATIVE(n_window_size) {
    UNUSED;
    struct winsize ws;
    if (ioctl((int)arg_num(ctx, n, a, 0, 1), TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return undef(ctx);
    JSValueRef items[2] = {num(ctx, ws.ws_col), num(ctx, ws.ws_row)};
    return array(ctx, 2, items);
}

static double bm_node_rss(void) {
#if defined(__APPLE__)
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) return (double)info.resident_size;
    return 0;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, resident = 0;
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    return (double)resident * (double)sysconf(_SC_PAGESIZE);
#endif
}

static double bm_node_free_mem(void);

NATIVE(n_memory_usage) {
    UNUSED;
    JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
    double rss = bm_node_rss();
    set(ctx, o, "rss", num(ctx, rss));
    /* JavaScriptCore doesn't report its heap through the public API */
    set(ctx, o, "heapTotal", num(ctx, 0));
    set(ctx, o, "heapUsed", num(ctx, 0));
    set(ctx, o, "external", num(ctx, 0));
    set(ctx, o, "arrayBuffers", num(ctx, 0));
    set(ctx, o, "available", num(ctx, bm_node_free_mem()));
    return o;
}

NATIVE(n_cpu_usage) {
    UNUSED;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    JSValueRef items[2] = {
        num(ctx, (double)ru.ru_utime.tv_sec * 1e6 + (double)ru.ru_utime.tv_usec),
        num(ctx, (double)ru.ru_stime.tv_sec * 1e6 + (double)ru.ru_stime.tv_usec),
    };
    return array(ctx, 2, items);
}

NATIVE(n_ids) {
    UNUSED;
    JSValueRef items[4] = {num(ctx, getuid()), num(ctx, getgid()), num(ctx, geteuid()), num(ctx, getegid())};
    return array(ctx, 4, items);
}

static const struct { const char *name; int sig; } bm_signals[] = {
    {"SIGHUP", SIGHUP}, {"SIGINT", SIGINT}, {"SIGQUIT", SIGQUIT}, {"SIGILL", SIGILL}, {"SIGTRAP", SIGTRAP},
    {"SIGABRT", SIGABRT}, {"SIGBUS", SIGBUS}, {"SIGFPE", SIGFPE}, {"SIGKILL", SIGKILL}, {"SIGUSR1", SIGUSR1},
    {"SIGSEGV", SIGSEGV}, {"SIGUSR2", SIGUSR2}, {"SIGPIPE", SIGPIPE}, {"SIGALRM", SIGALRM}, {"SIGTERM", SIGTERM},
    {"SIGCHLD", SIGCHLD}, {"SIGCONT", SIGCONT}, {"SIGSTOP", SIGSTOP}, {"SIGTSTP", SIGTSTP}, {"SIGTTIN", SIGTTIN},
    {"SIGTTOU", SIGTTOU}, {"SIGURG", SIGURG}, {"SIGXCPU", SIGXCPU}, {"SIGXFSZ", SIGXFSZ}, {"SIGVTALRM", SIGVTALRM},
    {"SIGPROF", SIGPROF}, {"SIGWINCH", SIGWINCH}, {"SIGIO", SIGIO}, {"SIGSYS", SIGSYS},
};

NATIVE(n_kill) {
    UNUSED;
    int pid = (int)arg_num(ctx, n, a, 0, 0);
    int sig = SIGTERM;
    if (n > 1 && JSValueIsNumber(ctx, a[1])) {
        sig = (int)JSValueToNumber(ctx, a[1], NULL);
    } else if (n > 1 && JSValueIsString(ctx, a[1])) {
        char *name = arg_cstr(ctx, n, a, 1);
        sig = -1;
        for (size_t i = 0; i < sizeof bm_signals / sizeof *bm_signals; i++)
            if (strcmp(bm_signals[i].name, name) == 0) sig = bm_signals[i].sig;
        free(name);
        if (sig < 0) return bm_node_throw(ctx, exc, EINVAL, "kill", NULL, NULL);
    }
    if (kill(pid, sig) != 0) return bm_node_throw(ctx, exc, errno, "kill", NULL, NULL);
    return JSValueMakeBoolean(ctx, true);
}

/* ------------------------------------------------------------------ timers, on Barm's loop
 *
 * One Barm timer stands for all of Node.js's (internal/timers keeps the lists): when it fires,
 * onTimer runs what's due and re-arms it. Immediates run in the loop's check phase (onCheck). */

static JSObjectRef bm_node_on_timer, bm_node_on_check;
static bm_int bm_node_timer_id;     /* the pending Barm timer (0: none) */
static double bm_node_timer_due;    /* when it fires (ms, bm_performance_now's clock) */
static bool bm_node_timer_refed = true;

static void bm_node_report(JSValueRef exc) {
    bm_str text = bm_js_error_text(exc);
    bm_out_flush();
    bm_err_cstr(text.p->data);
    bm_err_cstr("\n");
    exit(1);
}

static void bm_node_call(JSObjectRef fn) {
    JSValueRef exc = NULL;
    JSObjectCallAsFunction(bm_js_ctx, fn, NULL, 0, NULL, &exc);
    if (exc) bm_node_report(exc);
}

static void bm_node_timer_fire(bm_env *env) {
    (void)env;
    bm_node_timer_id = 0;
    if (bm_node_on_timer) bm_node_call(bm_node_on_timer);
}

static void bm_node_timer_arm(double ms) {
    if (bm_node_timer_id) bm_clear_timer(bm_node_timer_id);
    bm_fn cb = {(void *)bm_node_timer_fire, NULL};
    bm_node_timer_id = bm_set_timer(cb, ms, false);
    bm_node_timer_due = bm_performance_now() + ms;
    if (!bm_node_timer_refed) bm_native_timerUnref(bm_node_timer_id);
}

static void bm_node_check(void) {
    if (bm_node_on_check) bm_node_call(bm_node_on_check);
}

NATIVE(n_now) {
    UNUSED;
    return num(ctx, (double)(int64_t)bm_performance_now());
}

NATIVE(n_timer_setup) {
    UNUSED;
    if (n < 2) return undef(ctx);
    bm_node_on_timer = (JSObjectRef)a[0];
    bm_node_on_check = (JSObjectRef)a[1];
    JSValueProtect(ctx, a[0]);
    JSValueProtect(ctx, a[1]);
    bm_loop_check = bm_node_check;
    return undef(ctx);
}

NATIVE(n_timer_schedule) {
    UNUSED;
    bm_node_timer_arm(arg_num(ctx, n, a, 0, 1));
    return undef(ctx);
}

NATIVE(n_timer_ref) {
    UNUSED;
    bool ref = n > 0 && JSValueToBoolean(ctx, a[0]);
    if (ref == bm_node_timer_refed) return undef(ctx);
    bm_node_timer_refed = ref;
    if (!bm_node_timer_id) return undef(ctx);
    if (!ref) {
        bm_native_timerUnref(bm_node_timer_id);
    } else {
        /* (Barm's timers can't be ref'd again: arm a new one for the same moment) */
        double left = bm_node_timer_due - bm_performance_now();
        bm_node_timer_arm(left > 1 ? left : 1);
    }
    return undef(ctx);
}

NATIVE(n_request_check) {
    UNUSED;
    bm_loop_check_pending = true;
    bm_loop_check_ref = n > 0 && JSValueToBoolean(ctx, a[0]);
    return undef(ctx);
}

/* ------------------------------------------------------------------ os (Node.js's node_os.cc) */

NATIVE(n_os_info) {
    UNUSED;
    struct utsname u;
    if (uname(&u) != 0) memset(&u, 0, sizeof u);
    JSValueRef items[4] = {str(ctx, u.sysname), str(ctx, u.version), str(ctx, u.release), str(ctx, u.machine)};
    return array(ctx, 4, items);
}

NATIVE(n_os_hostname) {
    UNUSED;
    char buf[256];
    if (gethostname(buf, sizeof buf) != 0) return bm_node_ctx_error(ctx, n, a, 0, errno, "uv_os_gethostname");
    buf[sizeof buf - 1] = 0;
    return str(ctx, buf);
}

static const char *bm_node_home(void) {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : NULL;
}

NATIVE(n_os_homedir) {
    UNUSED;
    const char *h = bm_node_home();
    if (!h) return bm_node_ctx_error(ctx, n, a, 0, ENOENT, "uv_os_homedir");
    return str(ctx, h);
}

NATIVE(n_os_uptime) {
    UNUSED;
#if defined(__APPLE__)
    struct timeval boot;
    size_t len = sizeof boot;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boot, &len, NULL, 0) != 0) return bm_node_ctx_error(ctx, n, a, 0, errno, "uv_uptime");
    return num(ctx, (double)(time(NULL) - boot.tv_sec));
#else
    struct sysinfo si;
    if (sysinfo(&si) != 0) return bm_node_ctx_error(ctx, n, a, 0, errno, "uv_uptime");
    return num(ctx, (double)si.uptime);
#endif
}

NATIVE(n_os_totalmem) {
    UNUSED;
#if defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    return num(ctx, (double)mem);
#else
    struct sysinfo si;
    return num(ctx, sysinfo(&si) == 0 ? (double)si.totalram * si.mem_unit : 0);
#endif
}

static double bm_node_free_mem(void) {
#if defined(__APPLE__)
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) return 0;
    return (double)vm.free_count * (double)sysconf(_SC_PAGESIZE);
#else
    FILE *f = fopen("/proc/meminfo", "r");
    double kb = 0;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemAvailable: %lf kB", &kb) == 1) break;
        }
        fclose(f);
    }
    return kb * 1024;
#endif
}

NATIVE(n_os_freemem) {
    UNUSED;
    return num(ctx, bm_node_free_mem());
}

NATIVE(n_os_loadavg) {
    UNUSED;
    double avg[3] = {0, 0, 0};
    getloadavg(avg, 3);
    JSValueRef items[3] = {num(ctx, avg[0]), num(ctx, avg[1]), num(ctx, avg[2])};
    return array(ctx, 3, items);
}

/* [model, speed (MHz), user, nice, sys, idle, irq (ms), ...] */
NATIVE(n_os_cpus) {
    UNUSED;
#if defined(__APPLE__)
    char model[256] = "";
    size_t len = sizeof model;
    sysctlbyname("machdep.cpu.brand_string", model, &len, NULL, 0);
    uint64_t freq = 0;
    len = sizeof freq;
    if (sysctlbyname("hw.cpufrequency", &freq, &len, NULL, 0) != 0) freq = 0;
    natural_t count = 0;
    processor_info_array_t info;
    mach_msg_type_number_t info_count;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &info_count) != KERN_SUCCESS) return array(ctx, 0, NULL);
    long tick = sysconf(_SC_CLK_TCK);
    double mult = tick > 0 ? 1000.0 / (double)tick : 10;
    JSValueRef *items = malloc(sizeof(JSValueRef) * count * 7);
    for (natural_t i = 0; i < count; i++) {
        processor_cpu_load_info_t l = (processor_cpu_load_info_t)info + i;
        items[i * 7 + 0] = str(ctx, model);
        items[i * 7 + 1] = num(ctx, (double)(freq / 1000000));
        items[i * 7 + 2] = num(ctx, l->cpu_ticks[CPU_STATE_USER] * mult);
        items[i * 7 + 3] = num(ctx, l->cpu_ticks[CPU_STATE_NICE] * mult);
        items[i * 7 + 4] = num(ctx, l->cpu_ticks[CPU_STATE_SYSTEM] * mult);
        items[i * 7 + 5] = num(ctx, l->cpu_ticks[CPU_STATE_IDLE] * mult);
        items[i * 7 + 6] = num(ctx, 0);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)info, info_count * sizeof(int));
    JSObjectRef out = array(ctx, count * 7, items);
    free(items);
    return out;
#else
    char model[256] = "unknown";
    double mhz = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "model name", 10)) {
                char *c = strchr(line, ':');
                if (c) {
                    snprintf(model, sizeof model, "%s", c + 2);
                    model[strcspn(model, "\n")] = 0;
                }
            } else if (!strncmp(line, "cpu MHz", 7)) {
                char *c = strchr(line, ':');
                if (c && mhz == 0) mhz = atof(c + 1);
            }
        }
        fclose(f);
    }
    f = fopen("/proc/stat", "r");
    if (!f) return array(ctx, 0, NULL);
    long tick = sysconf(_SC_CLK_TCK);
    double mult = tick > 0 ? 1000.0 / (double)tick : 10;
    JSValueRef items[1024 * 7];
    size_t k = 0;
    char line[512];
    while (fgets(line, sizeof line, f) && k + 7 <= sizeof items / sizeof *items) {
        unsigned long long user, nice, sys, idle, iowait, irq;
        int id;
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu", &id, &user, &nice, &sys, &idle, &iowait, &irq) != 7) continue;
        items[k++] = str(ctx, model);
        items[k++] = num(ctx, (double)(long)mhz);
        items[k++] = num(ctx, (double)user * mult);
        items[k++] = num(ctx, (double)nice * mult);
        items[k++] = num(ctx, (double)sys * mult);
        items[k++] = num(ctx, (double)idle * mult);
        items[k++] = num(ctx, (double)irq * mult);
    }
    fclose(f);
    return array(ctx, k, items);
#endif
}

/* [name, address, netmask, family, mac, internal, scopeid, ...] */
NATIVE(n_os_interfaces) {
    UNUSED;
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) return bm_node_ctx_error(ctx, n, a, 0, errno, "uv_interface_addresses");
    size_t cap = 64, k = 0;
    JSValueRef *items = malloc(sizeof(JSValueRef) * cap * 7);
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !(i->ifa_flags & IFF_UP)) continue;
        int fam = i->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6) continue;
        char addr[INET6_ADDRSTRLEN] = "", mask[INET6_ADDRSTRLEN] = "", mac[18] = "00:00:00:00:00:00";
        double scope = -1;
        if (fam == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, addr, sizeof addr);
            if (i->ifa_netmask) inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_netmask)->sin_addr, mask, sizeof mask);
        } else {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)i->ifa_addr;
            inet_ntop(AF_INET6, &s6->sin6_addr, addr, sizeof addr);
            if (i->ifa_netmask) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)i->ifa_netmask)->sin6_addr, mask, sizeof mask);
            scope = s6->sin6_scope_id;
        }
        /* the interface's hardware address */
        for (struct ifaddrs *j = list; j; j = j->ifa_next) {
            if (!j->ifa_addr || strcmp(j->ifa_name, i->ifa_name) != 0) continue;
#if defined(__APPLE__)
            if (j->ifa_addr->sa_family == AF_LINK) {
                struct sockaddr_dl *dl = (struct sockaddr_dl *)j->ifa_addr;
                unsigned char *m = (unsigned char *)LLADDR(dl);
                if (dl->sdl_alen == 6) snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            }
#else
            if (j->ifa_addr->sa_family == AF_PACKET) {
                struct sockaddr_ll *ll = (struct sockaddr_ll *)j->ifa_addr;
                unsigned char *m = ll->sll_addr;
                if (ll->sll_halen == 6) snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            }
#endif
        }
        if (k + 7 > cap * 7) {
            cap *= 2;
            items = realloc(items, sizeof(JSValueRef) * cap * 7);
        }
        items[k++] = str(ctx, i->ifa_name);
        items[k++] = str(ctx, addr);
        items[k++] = str(ctx, mask);
        items[k++] = str(ctx, fam == AF_INET ? "IPv4" : "IPv6");
        items[k++] = str(ctx, mac);
        items[k++] = JSValueMakeBoolean(ctx, (i->ifa_flags & IFF_LOOPBACK) != 0);
        items[k++] = num(ctx, scope);
    }
    freeifaddrs(list);
    JSObjectRef out = array(ctx, k, items);
    free(items);
    return out;
}

NATIVE(n_os_userinfo) {
    UNUSED;
    struct passwd *pw = getpwuid(geteuid());
    if (!pw) return bm_node_ctx_error(ctx, n, a, 1, errno ? errno : ENOENT, "uv_os_get_passwd");
    JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
    set(ctx, o, "uid", num(ctx, pw->pw_uid));
    set(ctx, o, "gid", num(ctx, pw->pw_gid));
    set(ctx, o, "username", str(ctx, pw->pw_name));
    set(ctx, o, "homedir", str(ctx, pw->pw_dir));
    set(ctx, o, "shell", pw->pw_shell ? str(ctx, pw->pw_shell) : JSValueMakeNull(ctx));
    return o;
}

NATIVE(n_os_getpriority) {
    UNUSED;
    errno = 0;
    int p = getpriority(PRIO_PROCESS, (id_t)arg_num(ctx, n, a, 0, 0));
    if (p == -1 && errno) return bm_node_ctx_error(ctx, n, a, 1, errno, "uv_os_getpriority");
    return num(ctx, p);
}

NATIVE(n_os_setpriority) {
    UNUSED;
    if (setpriority(PRIO_PROCESS, (id_t)arg_num(ctx, n, a, 0, 0), (int)arg_num(ctx, n, a, 1, 0)) != 0)
        return bm_node_ctx_error(ctx, n, a, 2, errno, "uv_os_setpriority");
    return num(ctx, 0);
}

NATIVE(n_os_parallelism) {
    UNUSED;
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    return num(ctx, c > 0 ? (double)c : 1);
}

/* ------------------------------------------------------------------ tty */

NATIVE(n_set_raw_mode) {
    UNUSED;
    static struct termios saved;
    static bool have_saved;
    int fd = (int)arg_num(ctx, n, a, 0, 0);
    bool raw = n > 1 && JSValueToBoolean(ctx, a[1]);
    struct termios t;
    if (tcgetattr(fd, &t) != 0) return num(ctx, -errno);
    if (raw) {
        if (!have_saved) {
            saved = t;
            have_saved = true;
        }
        t.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        t.c_oflag |= ONLCR;
        t.c_cflag |= CS8;
        t.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
    } else if (have_saved) {
        t = saved;
    }
    return num(ctx, tcsetattr(fd, TCSADRAIN, &t) == 0 ? 0 : -errno);
}

/* ------------------------------------------------------------------ fs (synchronous; the JS binding
 * runs them for callbacks and promises too, completing on the next loop turn) */

#include <dirent.h>
#include <limits.h>
#if defined(__APPLE__)
#include <copyfile.h>
#include <sys/mount.h>
#else
#include <sys/statfs.h>
#include <sys/sendfile.h>
#endif

/* a path argument: a string, or bytes (a Buffer path); malloc'd and NUL-terminated */
static char *arg_path(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i) {
    char *s = arg_cstr(ctx, n, a, i);
    if (s) return s;
    uint8_t *p;
    size_t len;
    if (i < n && bm_js_bytes_view(a[i], &p, &len)) {
        s = malloc(len + 1);
        memcpy(s, p, len);
        s[len] = 0;
        return s;
    }
    return NULL;
}

#define PATH_ARG(var, i) char *var = arg_path(ctx, n, a, i); if (!var) return bm_node_throw(ctx, exc, EINVAL, "open", NULL, NULL)
#define FAIL(syscall, path) do { int e_ = errno; JSValueRef r_ = bm_node_throw(ctx, exc, e_, syscall, path, NULL); return r_; } while (0)

NATIVE(n_fs_open) {
    UNUSED;
    PATH_ARG(path, 0);
    int fd = open(path, (int)arg_num(ctx, n, a, 1, O_RDONLY) | O_CLOEXEC, (mode_t)arg_num(ctx, n, a, 2, 0666));
    if (fd < 0) { int e = errno; JSValueRef r = bm_node_throw(ctx, exc, e, "open", path, NULL); free(path); return r; }
    free(path);
    return num(ctx, fd);
}

NATIVE(n_fs_close) {
    UNUSED;
    if (close((int)arg_num(ctx, n, a, 0, -1)) != 0 && errno != EINTR) return bm_node_throw(ctx, exc, errno, "close", NULL, NULL);
    return undef(ctx);
}

/* read(fd, buffer, offset, length, position): position < 0 reads at the current offset */
NATIVE(n_fs_read) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, -1);
    uint8_t *p;
    size_t len;
    if (n < 2 || !bm_js_bytes_view(a[1], &p, &len)) return bm_node_throw(ctx, exc, EINVAL, "read", NULL, NULL);
    size_t off = (size_t)arg_num(ctx, n, a, 2, 0), want = (size_t)arg_num(ctx, n, a, 3, (double)len);
    double pos = arg_num(ctx, n, a, 4, -1);
    if (off > len) off = len;
    if (want > len - off) want = len - off;
    for (;;) {
        ssize_t r = pos >= 0 ? pread(fd, p + off, want, (off_t)pos) : read(fd, p + off, want);
        if (r >= 0) return num(ctx, (double)r);
        if (errno != EINTR) return bm_node_throw(ctx, exc, errno, "read", NULL, NULL);
    }
}

/* write(fd, buffer, offset, length, position) */
NATIVE(n_fs_write) {
    UNUSED;
    int fd = (int)arg_num(ctx, n, a, 0, -1);
    uint8_t *p;
    size_t len;
    if (n < 2 || !bm_js_bytes_view(a[1], &p, &len)) return bm_node_throw(ctx, exc, EINVAL, "write", NULL, NULL);
    size_t off = (size_t)arg_num(ctx, n, a, 2, 0), want = (size_t)arg_num(ctx, n, a, 3, (double)len);
    double pos = arg_num(ctx, n, a, 4, -1);
    if (off > len) off = len;
    if (want > len - off) want = len - off;
    for (;;) {
        ssize_t w = pos >= 0 ? pwrite(fd, p + off, want, (off_t)pos) : write(fd, p + off, want);
        if (w >= 0) return num(ctx, (double)w);
        if (errno != EINTR) return bm_node_throw(ctx, exc, errno, "write", NULL, NULL);
    }
}

/* Node.js's 18 stat fields: dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks, then
 * atime, mtime, ctime and birthtime as (seconds, nanoseconds) */
static JSValueRef bm_node_stat_array(JSContextRef ctx, const struct stat *st) {
#if defined(__APPLE__)
    struct timespec at = st->st_atimespec, mt = st->st_mtimespec, ct = st->st_ctimespec, bt = st->st_birthtimespec;
#else
    struct timespec at = st->st_atim, mt = st->st_mtim, ct = st->st_ctim, bt = st->st_ctim;
#endif
    JSValueRef v[18] = {
        num(ctx, (double)st->st_dev), num(ctx, st->st_mode), num(ctx, (double)st->st_nlink), num(ctx, st->st_uid),
        num(ctx, st->st_gid), num(ctx, (double)st->st_rdev), num(ctx, (double)st->st_blksize), num(ctx, (double)st->st_ino),
        num(ctx, (double)st->st_size), num(ctx, (double)st->st_blocks),
        num(ctx, (double)at.tv_sec), num(ctx, (double)at.tv_nsec), num(ctx, (double)mt.tv_sec), num(ctx, (double)mt.tv_nsec),
        num(ctx, (double)ct.tv_sec), num(ctx, (double)ct.tv_nsec), num(ctx, (double)bt.tv_sec), num(ctx, (double)bt.tv_nsec),
    };
    return array(ctx, 18, v);
}

/* stat(path, throwIfNoEntry) / lstat: the 18 fields, or undefined for a missing entry when
 * throwIfNoEntry is false */
static JSValueRef bm_node_stat(JSContextRef ctx, size_t n, const JSValueRef a[], JSValueRef *exc, bool link) {
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return bm_node_throw(ctx, exc, EINVAL, link ? "lstat" : "stat", NULL, NULL);
    struct stat st;
    int r = link ? lstat(path, &st) : stat(path, &st);
    if (r != 0) {
        int e = errno;
        bool quiet = n > 1 && JSValueIsBoolean(ctx, a[1]) && !JSValueToBoolean(ctx, a[1]) && (e == ENOENT || e == ENOTDIR);
        JSValueRef out = quiet ? undef(ctx) : bm_node_throw(ctx, exc, e, link ? "lstat" : "stat", path, NULL);
        free(path);
        return out;
    }
    free(path);
    return bm_node_stat_array(ctx, &st);
}

NATIVE(n_fs_stat) { UNUSED; return bm_node_stat(ctx, n, a, exc, false); }
NATIVE(n_fs_lstat) { UNUSED; return bm_node_stat(ctx, n, a, exc, true); }

NATIVE(n_fs_fstat) {
    UNUSED;
    struct stat st;
    if (fstat((int)arg_num(ctx, n, a, 0, -1), &st) != 0) return bm_node_throw(ctx, exc, errno, "fstat", NULL, NULL);
    return bm_node_stat_array(ctx, &st);
}

/* Node.js's dirent types (UV_DIRENT_*) */
static int bm_node_dirent_type(unsigned char t) {
    switch (t) {
    case DT_REG: return 1;
    case DT_DIR: return 2;
    case DT_LNK: return 3;
    case DT_FIFO: return 4;
    case DT_SOCK: return 5;
    case DT_CHR: return 6;
    case DT_BLK: return 7;
    default: return 0;
    }
}

/* readdir(path, withTypes): names, or [names, types] */
NATIVE(n_fs_readdir) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return bm_node_throw(ctx, exc, EINVAL, "scandir", NULL, NULL);
    bool types = n > 1 && JSValueToBoolean(ctx, a[1]);
    DIR *d = opendir(path);
    if (!d) { int e = errno; JSValueRef r = bm_node_throw(ctx, exc, e, "scandir", path, NULL); free(path); return r; }
    size_t cap = 64, k = 0;
    JSValueRef *names = malloc(sizeof(JSValueRef) * cap), *kinds = malloc(sizeof(JSValueRef) * cap);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (k == cap) {
            cap *= 2;
            names = realloc(names, sizeof(JSValueRef) * cap);
            kinds = realloc(kinds, sizeof(JSValueRef) * cap);
        }
        names[k] = str(ctx, e->d_name);
        kinds[k] = num(ctx, bm_node_dirent_type(e->d_type));
        k++;
    }
    closedir(d);
    free(path);
    JSObjectRef list = array(ctx, k, names);
    JSValueRef out = list;
    if (types) {
        JSValueRef pair[2] = {list, array(ctx, k, kinds)};
        out = array(ctx, 2, pair);
    }
    free(names);
    free(kinds);
    return out;
}

/* mkdir(path, mode, recursive): with recursive, the first directory it made (else undefined) */
NATIVE(n_fs_mkdir) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return bm_node_throw(ctx, exc, EINVAL, "mkdir", NULL, NULL);
    mode_t mode = (mode_t)arg_num(ctx, n, a, 1, 0777);
    bool recursive = n > 2 && JSValueToBoolean(ctx, a[2]);
    if (!recursive) {
        if (mkdir(path, mode) != 0) { int e = errno; JSValueRef r = bm_node_throw(ctx, exc, e, "mkdir", path, NULL); free(path); return r; }
        free(path);
        return undef(ctx);
    }
    char *first = NULL;
    size_t len = strlen(path);
    for (size_t i = 1; i <= len; i++) {
        if (i < len && path[i] != '/') continue;
        char c = path[i];
        path[i] = 0;
        if (mkdir(path, mode) == 0) {
            if (!first) first = strdup(path);
        } else if (errno != EEXIST) {
            int e = errno;
            path[i] = c;
            JSValueRef r = bm_node_throw(ctx, exc, e, "mkdir", path, NULL);
            free(path);
            free(first);
            return r;
        } else {
            struct stat st;
            if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
                path[i] = c;
                JSValueRef r = bm_node_throw(ctx, exc, i == len ? EEXIST : ENOTDIR, "mkdir", path, NULL);
                free(path);
                free(first);
                return r;
            }
        }
        path[i] = c;
    }
    free(path);
    JSValueRef out = first ? str(ctx, first) : undef(ctx);
    free(first);
    return out;
}

#define PATH_OP(name, syscall, call) \
    NATIVE(name) { \
        UNUSED; \
        char *path = arg_path(ctx, n, a, 0); \
        if (!path) return bm_node_throw(ctx, exc, EINVAL, syscall, NULL, NULL); \
        if ((call) != 0) { int e = errno; JSValueRef r = bm_node_throw(ctx, exc, e, syscall, path, NULL); free(path); return r; } \
        free(path); \
        return undef(ctx); \
    }

PATH_OP(n_fs_rmdir, "rmdir", rmdir(path))
PATH_OP(n_fs_unlink, "unlink", unlink(path))
PATH_OP(n_fs_chmod, "chmod", chmod(path, (mode_t)arg_num(ctx, n, a, 1, 0)))
PATH_OP(n_fs_chown, "chown", chown(path, (uid_t)arg_num(ctx, n, a, 1, -1), (gid_t)arg_num(ctx, n, a, 2, -1)))
PATH_OP(n_fs_lchown, "lchown", lchown(path, (uid_t)arg_num(ctx, n, a, 1, -1), (gid_t)arg_num(ctx, n, a, 2, -1)))
PATH_OP(n_fs_access, "access", access(path, (int)arg_num(ctx, n, a, 1, F_OK)))

static struct timespec bm_node_ts(double secs) {
    struct timespec t;
    t.tv_sec = (time_t)secs;
    t.tv_nsec = (long)((secs - (double)t.tv_sec) * 1e9);
    if (t.tv_nsec < 0) { t.tv_sec--; t.tv_nsec += 1000000000; }
    return t;
}

/* utimes(path, atime, mtime) / lutimes, in seconds */
static JSValueRef bm_node_utimes(JSContextRef ctx, size_t n, const JSValueRef a[], JSValueRef *exc, int flags, const char *syscall) {
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return bm_node_throw(ctx, exc, EINVAL, syscall, NULL, NULL);
    struct timespec t[2] = {bm_node_ts(arg_num(ctx, n, a, 1, 0)), bm_node_ts(arg_num(ctx, n, a, 2, 0))};
    if (utimensat(AT_FDCWD, path, t, flags) != 0) { int e = errno; JSValueRef r = bm_node_throw(ctx, exc, e, syscall, path, NULL); free(path); return r; }
    free(path);
    return undef(ctx);
}
NATIVE(n_fs_utimes) { UNUSED; return bm_node_utimes(ctx, n, a, exc, 0, "utime"); }
NATIVE(n_fs_lutimes) { UNUSED; return bm_node_utimes(ctx, n, a, exc, AT_SYMLINK_NOFOLLOW, "lutime"); }

NATIVE(n_fs_futimes) {
    UNUSED;
    struct timespec t[2] = {bm_node_ts(arg_num(ctx, n, a, 1, 0)), bm_node_ts(arg_num(ctx, n, a, 2, 0))};
    if (futimens((int)arg_num(ctx, n, a, 0, -1), t) != 0) return bm_node_throw(ctx, exc, errno, "futime", NULL, NULL);
    return undef(ctx);
}

#define FD_OP(name, syscall, call) \
    NATIVE(name) { \
        UNUSED; \
        int fd = (int)arg_num(ctx, n, a, 0, -1); \
        if ((call) != 0) return bm_node_throw(ctx, exc, errno, syscall, NULL, NULL); \
        return undef(ctx); \
    }

FD_OP(n_fs_fsync, "fsync", fsync(fd))
#if defined(__APPLE__)
FD_OP(n_fs_fdatasync, "fdatasync", fsync(fd))
#else
FD_OP(n_fs_fdatasync, "fdatasync", fdatasync(fd))
#endif
FD_OP(n_fs_ftruncate, "ftruncate", ftruncate(fd, (off_t)arg_num(ctx, n, a, 1, 0)))
FD_OP(n_fs_fchmod, "fchmod", fchmod(fd, (mode_t)arg_num(ctx, n, a, 1, 0)))
FD_OP(n_fs_fchown, "fchown", fchown(fd, (uid_t)arg_num(ctx, n, a, 1, -1), (gid_t)arg_num(ctx, n, a, 2, -1)))

/* two-path operations: rename(from, to), link, symlink(target, path) */
static JSValueRef bm_node_two_paths(JSContextRef ctx, size_t n, const JSValueRef a[], JSValueRef *exc, const char *syscall, int which) {
    char *from = arg_path(ctx, n, a, 0), *to = arg_path(ctx, n, a, 1);
    if (!from || !to) { free(from); free(to); return bm_node_throw(ctx, exc, EINVAL, syscall, NULL, NULL); }
    int r = which == 0 ? rename(from, to) : which == 1 ? link(from, to) : symlink(from, to);
    JSValueRef out = undef(ctx);
    if (r != 0) out = bm_node_throw(ctx, exc, errno, syscall, from, to);
    free(from);
    free(to);
    return out;
}
NATIVE(n_fs_rename) { UNUSED; return bm_node_two_paths(ctx, n, a, exc, "rename", 0); }
NATIVE(n_fs_link) { UNUSED; return bm_node_two_paths(ctx, n, a, exc, "link", 1); }
NATIVE(n_fs_symlink) { UNUSED; return bm_node_two_paths(ctx, n, a, exc, "symlink", 2); }

NATIVE(n_fs_readlink) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return bm_node_throw(ctx, exc, EINVAL, "readlink", NULL, NULL);
    char buf[PATH_MAX + 1];
    ssize_t k = readlink(path, buf, PATH_MAX);
    if (k < 0) { int e = errno; JSValueRef r = bm_node_throw(ctx, exc, e, "readlink", path, NULL); free(path); return r; }
    buf[k] = 0;
    free(path);
    return str(ctx, buf);
}

NATIVE(n_fs_realpath) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return bm_node_throw(ctx, exc, EINVAL, "realpath", NULL, NULL);
    char buf[PATH_MAX + 1];
    if (!realpath(path, buf)) { int e = errno; JSValueRef r = bm_node_throw(ctx, exc, e, "realpath", path, NULL); free(path); return r; }
    free(path);
    return str(ctx, buf);
}

/* copyFile(src, dest, mode): COPYFILE_EXCL (1) fails if dest exists; FICLONE modes clone
 * where the file system can, else copy */
NATIVE(n_fs_copyfile) {
    UNUSED;
    char *src = arg_path(ctx, n, a, 0), *dest = arg_path(ctx, n, a, 1);
    if (!src || !dest) { free(src); free(dest); return bm_node_throw(ctx, exc, EINVAL, "copyfile", NULL, NULL); }
    int mode = (int)arg_num(ctx, n, a, 2, 0);
    JSValueRef out = undef(ctx);
    if ((mode & 1) && access(dest, F_OK) == 0) {
        out = bm_node_throw(ctx, exc, EEXIST, "copyfile", src, dest);
    } else {
#if defined(__APPLE__)
        if (copyfile(src, dest, NULL, COPYFILE_ALL | ((mode & 2) ? COPYFILE_CLONE : 0)) != 0) out = bm_node_throw(ctx, exc, errno, "copyfile", src, dest);
#else
        int in = open(src, O_RDONLY | O_CLOEXEC);
        struct stat st;
        int o = in >= 0 && fstat(in, &st) == 0 ? open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777) : -1;
        if (in < 0 || o < 0) {
            out = bm_node_throw(ctx, exc, errno, "copyfile", src, dest);
        } else {
            off_t left = st.st_size;
            while (left > 0) {
                ssize_t k = sendfile(o, in, NULL, (size_t)left);
                if (k <= 0) { if (k < 0 && errno == EINTR) continue; if (k < 0) out = bm_node_throw(ctx, exc, errno, "copyfile", src, dest); break; }
                left -= k;
            }
        }
        if (in >= 0) close(in);
        if (o >= 0) close(o);
#endif
    }
    free(src);
    free(dest);
    return out;
}

NATIVE(n_fs_mkdtemp) {
    UNUSED;
    char *prefix = arg_path(ctx, n, a, 0);
    if (!prefix) return bm_node_throw(ctx, exc, EINVAL, "mkdtemp", NULL, NULL);
    size_t pl = strlen(prefix);
    char *tmpl = malloc(pl + 7);
    memcpy(tmpl, prefix, pl);
    memcpy(tmpl + pl, "XXXXXX", 7);
    JSValueRef out;
    if (!mkdtemp(tmpl)) {
        char shown[PATH_MAX + 8];
        snprintf(shown, sizeof shown, "%sXXXXXX", prefix);
        out = bm_node_throw(ctx, exc, errno, "mkdtemp", shown, NULL);
    } else {
        out = str(ctx, tmpl);
    }
    free(tmpl);
    free(prefix);
    return out;
}

/* statfs(path): [type, bsize, blocks, bfree, bavail, files, ffree] */
NATIVE(n_fs_statfs) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return bm_node_throw(ctx, exc, EINVAL, "statfs", NULL, NULL);
    struct statfs s;
    if (statfs(path, &s) != 0) { int e = errno; JSValueRef r = bm_node_throw(ctx, exc, e, "statfs", path, NULL); free(path); return r; }
    free(path);
    JSValueRef v[7] = {num(ctx, (double)s.f_type), num(ctx, (double)s.f_bsize), num(ctx, (double)s.f_blocks), num(ctx, (double)s.f_bfree),
                       num(ctx, (double)s.f_bavail), num(ctx, (double)s.f_files), num(ctx, (double)s.f_ffree)};
    return array(ctx, 7, v);
}

/* removes path and, if it's a directory, everything in it */
static int bm_node_rm_tree(char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    size_t pl = strlen(path);
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t nl = strlen(e->d_name);
        char *child = malloc(pl + nl + 2);
        memcpy(child, path, pl);
        child[pl] = '/';
        memcpy(child + pl + 1, e->d_name, nl + 1);
        if (bm_node_rm_tree(child) != 0) rc = -1;
        free(child);
        if (rc) break;
    }
    int keep = errno;
    closedir(d);
    errno = keep;
    if (rc) return rc;
    return rmdir(path);
}

/* rmSync(path, maxRetries, recursive, retryDelay) */
NATIVE(n_fs_rm) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return bm_node_throw(ctx, exc, EINVAL, "rm", NULL, NULL);
    bool recursive = n > 2 && JSValueToBoolean(ctx, a[2]);
    struct stat st;
    int rc;
    if (lstat(path, &st) != 0) rc = -1;
    else if (S_ISDIR(st.st_mode) && !recursive) { errno = EISDIR; rc = -1; }
    else rc = recursive ? bm_node_rm_tree(path) : unlink(path);
    JSValueRef out = undef(ctx);
    if (rc != 0) {
        int e = errno;
        if (e == EISDIR) {
            /* Node.js: ERR_FS_EISDIR "Path is a directory: rm returned EISDIR (is a directory) path" */
            char text[PATH_MAX + 128];
            snprintf(text, sizeof text, "Path is a directory: rm returned EISDIR (is a directory) %s", path);
            JSValueRef msg = str(ctx, text);
            JSObjectRef err = JSObjectMakeError(ctx, 1, &msg, NULL);
            set(ctx, err, "code", str(ctx, "ERR_FS_EISDIR"));
            set(ctx, err, "errno", num(ctx, EISDIR));
            set(ctx, err, "syscall", str(ctx, "rm"));
            set(ctx, err, "path", str(ctx, path));
            *exc = err;
        } else {
            out = bm_node_throw(ctx, exc, e, "rm", path, NULL);
        }
    }
    free(path);
    return out;
}

/* whole files: readFileUtf8(path | fd, flags) and writeFileUtf8(path | fd, data, flags, mode) */
static int bm_node_open_arg(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i, int flags, int mode, bool *owned, char **path) {
    *path = NULL;
    if (i < n && JSValueIsNumber(ctx, a[i])) {
        *owned = false;
        return (int)JSValueToNumber(ctx, a[i], NULL);
    }
    *owned = true;
    *path = arg_path(ctx, n, a, i);
    if (!*path) { errno = EINVAL; return -1; }
    return open(*path, flags | O_CLOEXEC, mode);
}

NATIVE(n_fs_read_utf8) {
    UNUSED;
    bool owned;
    char *path;
    int fd = bm_node_open_arg(ctx, n, a, 0, (int)arg_num(ctx, n, a, 1, O_RDONLY), 0666, &owned, &path);
    if (fd < 0) { int e = errno; JSValueRef r = bm_node_throw(ctx, exc, e, "open", path, NULL); free(path); return r; }
    bm_sb sb = {0};
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) bm_sb_grow(&sb, (size_t)st.st_size + 1);
    else if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (owned) close(fd);
        JSValueRef r = bm_node_throw(ctx, exc, EISDIR, "read", NULL, NULL);
        free(path);
        return r;
    }
    for (;;) {
        if (sb.cap - sb.len < 65536) bm_sb_grow(&sb, sb.len + 65536);
        ssize_t k = read(fd, sb.data + sb.len, sb.cap - sb.len);
        if (k > 0) { sb.len += (size_t)k; continue; }
        if (k == 0) break;
        if (errno == EINTR) continue;
        int e = errno;
        if (owned) close(fd);
        bm_sb_free(&sb);
        JSValueRef r = bm_node_throw(ctx, exc, e, "read", NULL, NULL);
        free(path);
        return r;
    }
    if (owned) close(fd);
    free(path);
    JSValueRef out = bm_js_str(sb.data ? sb.data : "", sb.len);
    bm_sb_free(&sb);
    return out;
}

NATIVE(n_fs_write_utf8) {
    UNUSED;
    if (n < 2) return undef(ctx);
    bool owned;
    char *path;
    int fd = bm_node_open_arg(ctx, n, a, 0, (int)arg_num(ctx, n, a, 2, O_WRONLY | O_CREAT | O_TRUNC), (int)arg_num(ctx, n, a, 3, 0666), &owned, &path);
    if (fd < 0) { int e = errno; JSValueRef r = bm_node_throw(ctx, exc, e, "open", path, NULL); free(path); return r; }
    JSStringRef s = JSValueToStringCopy(ctx, a[1], NULL);
    size_t cap = JSStringGetMaximumUTF8CStringSize(s);
    char *text = malloc(cap);
    size_t len = JSStringGetUTF8CString(s, text, cap) - 1;
    JSStringRelease(s);
    size_t off = 0;
    JSValueRef out = undef(ctx);
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            out = bm_node_throw(ctx, exc, errno, "write", NULL, NULL);
            break;
        }
        off += (size_t)w;
    }
    free(text);
    if (owned) close(fd);
    free(path);
    return out;
}

NATIVE(n_fs_exists) {
    UNUSED;
    char *path = arg_path(ctx, n, a, 0);
    if (!path) return JSValueMakeBoolean(ctx, false);
    bool ok = access(path, F_OK) == 0;
    free(path);
    return JSValueMakeBoolean(ctx, ok);
}

/* internalModuleStat(path): 0 file, 1 directory, negative errno */
NATIVE(n_fs_module_stat) {
    UNUSED;
    char *path = arg_path(ctx, n, a, n > 1 ? 1 : 0);
    if (!path) return num(ctx, -ENOENT);
    struct stat st;
    int r = stat(path, &st) == 0 ? (S_ISDIR(st.st_mode) ? 1 : 0) : -errno;
    free(path);
    return num(ctx, r);
}

void bm_node_fs_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef fs = JSObjectMake(ctx, NULL, NULL);
    bm_js_def(ctx, fs, "open", n_fs_open);
    bm_js_def(ctx, fs, "close", n_fs_close);
    bm_js_def(ctx, fs, "read", n_fs_read);
    bm_js_def(ctx, fs, "write", n_fs_write);
    bm_js_def(ctx, fs, "stat", n_fs_stat);
    bm_js_def(ctx, fs, "lstat", n_fs_lstat);
    bm_js_def(ctx, fs, "fstat", n_fs_fstat);
    bm_js_def(ctx, fs, "readdir", n_fs_readdir);
    bm_js_def(ctx, fs, "mkdir", n_fs_mkdir);
    bm_js_def(ctx, fs, "rmdir", n_fs_rmdir);
    bm_js_def(ctx, fs, "unlink", n_fs_unlink);
    bm_js_def(ctx, fs, "chmod", n_fs_chmod);
    bm_js_def(ctx, fs, "chown", n_fs_chown);
    bm_js_def(ctx, fs, "lchown", n_fs_lchown);
    bm_js_def(ctx, fs, "access", n_fs_access);
    bm_js_def(ctx, fs, "utimes", n_fs_utimes);
    bm_js_def(ctx, fs, "lutimes", n_fs_lutimes);
    bm_js_def(ctx, fs, "futimes", n_fs_futimes);
    bm_js_def(ctx, fs, "fsync", n_fs_fsync);
    bm_js_def(ctx, fs, "fdatasync", n_fs_fdatasync);
    bm_js_def(ctx, fs, "ftruncate", n_fs_ftruncate);
    bm_js_def(ctx, fs, "fchmod", n_fs_fchmod);
    bm_js_def(ctx, fs, "fchown", n_fs_fchown);
    bm_js_def(ctx, fs, "rename", n_fs_rename);
    bm_js_def(ctx, fs, "link", n_fs_link);
    bm_js_def(ctx, fs, "symlink", n_fs_symlink);
    bm_js_def(ctx, fs, "readlink", n_fs_readlink);
    bm_js_def(ctx, fs, "realpath", n_fs_realpath);
    bm_js_def(ctx, fs, "copyFile", n_fs_copyfile);
    bm_js_def(ctx, fs, "mkdtemp", n_fs_mkdtemp);
    bm_js_def(ctx, fs, "statfs", n_fs_statfs);
    bm_js_def(ctx, fs, "rm", n_fs_rm);
    bm_js_def(ctx, fs, "readFileUtf8", n_fs_read_utf8);
    bm_js_def(ctx, fs, "writeFileUtf8", n_fs_write_utf8);
    bm_js_def(ctx, fs, "exists", n_fs_exists);
    bm_js_def(ctx, fs, "internalModuleStat", n_fs_module_stat);
    set(ctx, native, "fs", fs);
}

/* ------------------------------------------------------------------ crypto (runtime/crypto.c, through
 * bm_crypto: there when the program links the TLS archive) */

typedef struct { void *ctx; bool hmac; } bm_node_mac;

static void bm_node_mac_finalize(JSObjectRef o) {
    bm_node_mac *m = JSObjectGetPrivate(o);
    if (!m) return;
    if (m->ctx && bm_crypto) (m->hmac ? bm_crypto->hmac_free : bm_crypto->hash_free)(m->ctx);
    free(m);
}

static JSClassRef bm_node_mac_class(void) {
    static JSClassRef cls;
    if (!cls) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "CryptoHandle";
        def.finalize = bm_node_mac_finalize;
        cls = JSClassCreate(&def);
    }
    return cls;
}

static JSValueRef bm_node_crypto_missing(JSContextRef ctx, JSValueRef *exc) {
    JSValueRef msg = str(ctx, "crypto is not available in this program (it was built without the TLS library)");
    *exc = JSObjectMakeError(ctx, 1, &msg, NULL);
    return undef(ctx);
}

static bm_node_mac *bm_node_mac_of(JSContextRef ctx, size_t n, const JSValueRef a[]) {
    if (n < 1 || !JSValueIsObjectOfClass(ctx, a[0], bm_node_mac_class())) return NULL;
    return JSObjectGetPrivate((JSObjectRef)a[0]);
}

static JSValueRef bm_node_mac_new(JSContextRef ctx, void *c, bool hmac) {
    bm_node_mac *m = malloc(sizeof *m);
    m->ctx = c;
    m->hmac = hmac;
    return JSObjectMake(ctx, bm_node_mac_class(), m);
}

/* hashNew(name) -> handle, or null for a digest BoringSSL doesn't have */
NATIVE(n_crypto_hash_new) {
    UNUSED;
    if (!bm_crypto) return bm_node_crypto_missing(ctx, exc);
    char *name = arg_cstr(ctx, n, a, 0);
    void *c = name ? bm_crypto->hash_new(name) : NULL;
    free(name);
    return c ? bm_node_mac_new(ctx, c, false) : JSValueMakeNull(ctx);
}

/* hmacNew(name, key bytes) -> handle, or null */
NATIVE(n_crypto_hmac_new) {
    UNUSED;
    if (!bm_crypto) return bm_node_crypto_missing(ctx, exc);
    char *name = arg_cstr(ctx, n, a, 0);
    uint8_t *k = NULL;
    size_t kl = 0;
    if (n > 1) bm_js_bytes_view(a[1], &k, &kl);
    void *c = name ? bm_crypto->hmac_new(name, k ? k : (const uint8_t *)"", kl) : NULL;
    free(name);
    return c ? bm_node_mac_new(ctx, c, true) : JSValueMakeNull(ctx);
}

/* macUpdate(handle, bytes) */
NATIVE(n_crypto_update) {
    UNUSED;
    bm_node_mac *m = bm_node_mac_of(ctx, n, a);
    uint8_t *p;
    size_t len;
    if (!m || !m->ctx || n < 2 || !bm_js_bytes_view(a[1], &p, &len)) return undef(ctx);
    (m->hmac ? bm_crypto->hmac_update : bm_crypto->hash_update)(m->ctx, p, len);
    return undef(ctx);
}

/* macDigest(handle) -> Uint8Array (the handle is spent) */
NATIVE(n_crypto_digest) {
    UNUSED;
    bm_node_mac *m = bm_node_mac_of(ctx, n, a);
    if (!m || !m->ctx) return undef(ctx);
    uint8_t out[64];
    size_t len = (m->hmac ? bm_crypto->hmac_final : bm_crypto->hash_final)(m->ctx, out);
    (m->hmac ? bm_crypto->hmac_free : bm_crypto->hash_free)(m->ctx);
    m->ctx = NULL;
    return bm_js_bytes_copy(out, len);
}

/* hashCopy(handle) -> a handle with the same state */
NATIVE(n_crypto_hash_copy) {
    UNUSED;
    bm_node_mac *m = bm_node_mac_of(ctx, n, a);
    if (!m || !m->ctx || m->hmac) return JSValueMakeNull(ctx);
    void *c = bm_crypto->hash_copy(m->ctx);
    return c ? bm_node_mac_new(ctx, c, false) : JSValueMakeNull(ctx);
}

NATIVE(n_crypto_digest_size) {
    UNUSED;
    if (!bm_crypto) return num(ctx, -1);
    char *name = arg_cstr(ctx, n, a, 0);
    int size = name ? bm_crypto->digest_size(name) : -1;
    free(name);
    return num(ctx, size);
}

/* randomFill(view): fills it with cryptographically secure bytes */
NATIVE(n_crypto_random_fill) {
    UNUSED;
    uint8_t *p;
    size_t len;
    if (n < 1 || !bm_js_bytes_view(a[0], &p, &len)) return undef(ctx);
    if (bm_crypto) bm_crypto->random(p, len);
    else arc4random_buf(p, len);
    return undef(ctx);
}

static bool bm_node_bytes_arg(size_t n, const JSValueRef a[], size_t i, uint8_t **p, size_t *len) {
    static uint8_t empty;
    if (i < n && bm_js_bytes_view(a[i], p, len)) return true;
    *p = &empty;
    *len = 0;
    return false;
}

/* pbkdf2(digest, password, salt, iterations, keylen) -> Uint8Array, or null for an unknown digest */
NATIVE(n_crypto_pbkdf2) {
    UNUSED;
    if (!bm_crypto) return bm_node_crypto_missing(ctx, exc);
    char *digest = arg_cstr(ctx, n, a, 0);
    uint8_t *pass, *salt;
    size_t pl, sl;
    bm_node_bytes_arg(n, a, 1, &pass, &pl);
    bm_node_bytes_arg(n, a, 2, &salt, &sl);
    size_t keylen = (size_t)arg_num(ctx, n, a, 4, 0);
    uint8_t *out = malloc(keylen ? keylen : 1);
    bool ok = digest && bm_crypto->pbkdf2(digest, pass, pl, salt, sl, (uint32_t)arg_num(ctx, n, a, 3, 1), out, keylen);
    free(digest);
    JSValueRef r = ok ? bm_js_bytes_copy(out, keylen) : JSValueMakeNull(ctx);
    free(out);
    return r;
}

/* scrypt(password, salt, N, r, p, maxmem, keylen) -> Uint8Array, or null if the parameters fail */
NATIVE(n_crypto_scrypt) {
    UNUSED;
    if (!bm_crypto) return bm_node_crypto_missing(ctx, exc);
    uint8_t *pass, *salt;
    size_t pl, sl;
    bm_node_bytes_arg(n, a, 0, &pass, &pl);
    bm_node_bytes_arg(n, a, 1, &salt, &sl);
    size_t keylen = (size_t)arg_num(ctx, n, a, 6, 0);
    uint8_t *out = malloc(keylen ? keylen : 1);
    bool ok = bm_crypto->scrypt(pass, pl, salt, sl, (uint64_t)arg_num(ctx, n, a, 2, 16384), (uint64_t)arg_num(ctx, n, a, 3, 8),
                                (uint64_t)arg_num(ctx, n, a, 4, 1), (size_t)arg_num(ctx, n, a, 5, 32 << 20), out, keylen);
    JSValueRef r = ok ? bm_js_bytes_copy(out, keylen) : JSValueMakeNull(ctx);
    free(out);
    return r;
}

/* hkdf(digest, key, salt, info, keylen) -> Uint8Array, or null */
NATIVE(n_crypto_hkdf) {
    UNUSED;
    if (!bm_crypto) return bm_node_crypto_missing(ctx, exc);
    char *digest = arg_cstr(ctx, n, a, 0);
    uint8_t *key, *salt, *info;
    size_t kl, sl, il;
    bm_node_bytes_arg(n, a, 1, &key, &kl);
    bm_node_bytes_arg(n, a, 2, &salt, &sl);
    bm_node_bytes_arg(n, a, 3, &info, &il);
    size_t keylen = (size_t)arg_num(ctx, n, a, 4, 0);
    uint8_t *out = malloc(keylen ? keylen : 1);
    bool ok = digest && bm_crypto->hkdf(digest, key, kl, salt, sl, info, il, out, keylen);
    free(digest);
    JSValueRef r = ok ? bm_js_bytes_copy(out, keylen) : JSValueMakeNull(ctx);
    free(out);
    return r;
}

/* timingSafeEqual(a, b): same-length byte views, compared in constant time */
NATIVE(n_crypto_equal) {
    UNUSED;
    uint8_t *x, *y;
    size_t xl, yl;
    if (!bm_node_bytes_arg(n, a, 0, &x, &xl) || !bm_node_bytes_arg(n, a, 1, &y, &yl) || xl != yl) return JSValueMakeBoolean(ctx, false);
    if (bm_crypto) return JSValueMakeBoolean(ctx, bm_crypto->equal(x, y, xl));
    unsigned char d = 0;
    for (size_t i = 0; i < xl; i++) d |= x[i] ^ y[i];
    return JSValueMakeBoolean(ctx, d == 0);
}

/* ------------------------------------------------------------------ work off the loop (libuv's
 * uv_queue_work): a job runs on a worker thread, then its callback is called from the event loop
 * with what done() makes of it. A pending job keeps the program running. */

#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>

typedef struct bm_node_job {
    struct bm_node_job *next;
    void (*work)(struct bm_node_job *job);                          /* on a worker thread */
    size_t (*done)(JSContextRef ctx, struct bm_node_job *job, JSValueRef *args);  /* on the loop: the callback's arguments (up to 4) */
    void (*free)(struct bm_node_job *job);
    JSObjectRef callback;
} bm_node_job;

static pthread_mutex_t bm_node_work_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bm_node_work_cv = PTHREAD_COND_INITIALIZER;
static bm_node_job *bm_node_jobs, *bm_node_jobs_tail, *bm_node_done;
static int bm_node_workers, bm_node_idle;
static int bm_node_wake[2] = {-1, -1};
static bm_io bm_node_work_io;

static void *bm_node_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&bm_node_work_mu);
    for (;;) {
        while (!bm_node_jobs) {
            bm_node_idle++;
            pthread_cond_wait(&bm_node_work_cv, &bm_node_work_mu);
            bm_node_idle--;
        }
        bm_node_job *job = bm_node_jobs;
        bm_node_jobs = job->next;
        if (!bm_node_jobs) bm_node_jobs_tail = NULL;
        pthread_mutex_unlock(&bm_node_work_mu);
        job->work(job);
        pthread_mutex_lock(&bm_node_work_mu);
        job->next = bm_node_done;
        bm_node_done = job;
        char one = 1;
        ssize_t w = write(bm_node_wake[1], &one, 1);
        (void)w;
    }
    return NULL;
}

static void bm_node_work_ready(bm_io *h, bool readable, bool writable, bool broken) {
    (void)h; (void)readable; (void)writable; (void)broken;
    char buf[64];
    while (read(bm_node_wake[0], buf, sizeof buf) > 0) {}
    pthread_mutex_lock(&bm_node_work_mu);
    bm_node_job *done = bm_node_done;
    bm_node_done = NULL;
    pthread_mutex_unlock(&bm_node_work_mu);
    /* (oldest first) */
    bm_node_job *ordered = NULL;
    while (done) {
        bm_node_job *next = done->next;
        done->next = ordered;
        ordered = done;
        done = next;
    }
    while (ordered) {
        bm_node_job *job = ordered;
        ordered = job->next;
        bm_io_refs--;
        JSValueRef args[4];
        size_t nargs = job->done(bm_js_ctx, job, args);
        JSObjectRef cb = job->callback;
        if (job->free) job->free(job);
        free(job);
        JSValueRef exc = NULL;
        JSObjectCallAsFunction(bm_js_ctx, cb, NULL, nargs, args, &exc);
        JSValueUnprotect(bm_js_ctx, cb);
        if (exc) bm_node_report(exc);
    }
}

static void bm_node_queue(JSContextRef ctx, bm_node_job *job, JSValueRef callback) {
    if (bm_node_wake[0] < 0) {
        if (pipe(bm_node_wake) != 0) bm_trap("can't create a pipe for the work queue", NULL);
        for (int i = 0; i < 2; i++) {
            fcntl(bm_node_wake[i], F_SETFL, fcntl(bm_node_wake[i], F_GETFL) | O_NONBLOCK);
            fcntl(bm_node_wake[i], F_SETFD, FD_CLOEXEC);
        }
        bm_node_work_io.ready = bm_node_work_ready;
        bm_io_add(bm_node_wake[0], &bm_node_work_io, true, false);
    }
    job->callback = (JSObjectRef)callback;
    JSValueProtect(ctx, callback);
    bm_io_refs++;
    job->next = NULL;
    pthread_mutex_lock(&bm_node_work_mu);
    if (bm_node_jobs_tail) bm_node_jobs_tail->next = job;
    else bm_node_jobs = job;
    bm_node_jobs_tail = job;
    if (bm_node_idle == 0 && bm_node_workers < 4) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        if (pthread_create(&t, &attr, bm_node_worker, NULL) == 0) bm_node_workers++;
        pthread_attr_destroy(&attr);
    }
    pthread_cond_signal(&bm_node_work_cv);
    pthread_mutex_unlock(&bm_node_work_mu);
}

/* ------------------------------------------------------------------ dns (Node.js's cares_wrap.cc):
 * getaddrinfo and getnameinfo on the work queue, and DNS queries (the resolve* family) sent to a
 * server over UDP, then TCP if the answer was truncated; internal/bindings/cares_wrap.js builds
 * the queries and reads the answers. */

/* libuv's error for a getaddrinfo failure */
static int bm_node_eai(int r) {
    switch (r) {
#ifdef EAI_ADDRFAMILY
    case EAI_ADDRFAMILY: return -3000;
#endif
    case EAI_AGAIN: return -3001;
    case EAI_BADFLAGS: return -3002;
    case EAI_FAIL: return -3004;
    case EAI_FAMILY: return -3005;
    case EAI_MEMORY: return -3006;
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
    case EAI_NODATA: return -3007;
#endif
    case EAI_NONAME: return -3008;
#ifdef EAI_OVERFLOW
    case EAI_OVERFLOW: return -3009;
#endif
    case EAI_SERVICE: return -3010;
    case EAI_SOCKTYPE: return -3011;
#ifdef EAI_BADHINTS
    case EAI_BADHINTS: return -3013;
#endif
#ifdef EAI_PROTOCOL
    case EAI_PROTOCOL: return -3014;
#endif
    case EAI_SYSTEM: return -errno;
    default: return -3004;
    }
}

typedef struct {
    bm_node_job job;
    char *host;
    int family, flags, order;
    int err;
    struct addrinfo *res;
} bm_node_gai;

static void bm_node_gai_work(bm_node_job *j) {
    bm_node_gai *g = (bm_node_gai *)j;
    struct addrinfo hints = {0};
    hints.ai_family = g->family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = g->flags;
    int r = getaddrinfo(g->host, NULL, &hints, &g->res);
    g->err = r ? bm_node_eai(r) : 0;
}

static size_t bm_node_gai_done(JSContextRef ctx, bm_node_job *j, JSValueRef *args) {
    bm_node_gai *g = (bm_node_gai *)j;
    args[0] = num(ctx, g->err);
    if (g->err) {
        args[1] = undef(ctx);
        return 2;
    }
    JSValueRef items[256];
    size_t count = 0;
    /* order: 0 as the resolver gave them, 1 IPv4 first, 2 IPv6 first */
    int passes[2] = { AF_INET, AF_INET6 };
    if (g->order == 2) { passes[0] = AF_INET6; passes[1] = AF_INET; }
    for (int pass = 0; pass < (g->order ? 2 : 1); pass++) {
        for (struct addrinfo *ai = g->res; ai && count < 256; ai = ai->ai_next) {
            if (g->order && ai->ai_family != passes[pass]) continue;
            char ip[INET6_ADDRSTRLEN];
            const void *addr = ai->ai_family == AF_INET ? (const void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr
                             : ai->ai_family == AF_INET6 ? (const void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr : NULL;
            if (!addr || !inet_ntop(ai->ai_family, addr, ip, sizeof ip)) continue;
            items[count++] = str(ctx, ip);
        }
    }
    args[1] = array(ctx, count, items);
    return 2;
}

static void bm_node_gai_free(bm_node_job *j) {
    bm_node_gai *g = (bm_node_gai *)j;
    if (g->res) freeaddrinfo(g->res);
    free(g->host);
}

/* getaddrinfo(hostname, family, flags, order, callback(err, addresses)) */
NATIVE(n_dns_getaddrinfo) {
    UNUSED;
    if (n < 5) return undef(ctx);
    bm_node_gai *g = calloc(1, sizeof *g);
    g->host = arg_cstr(ctx, n, a, 0);
    int family = (int)arg_num(ctx, n, a, 1, 0);
    g->family = family == 4 ? AF_INET : family == 6 ? AF_INET6 : AF_UNSPEC;
    g->flags = (int)arg_num(ctx, n, a, 2, 0);
    g->order = (int)arg_num(ctx, n, a, 3, 0);
    g->job.work = bm_node_gai_work;
    g->job.done = bm_node_gai_done;
    g->job.free = bm_node_gai_free;
    bm_node_queue(ctx, &g->job, a[4]);
    return num(ctx, 0);
}

typedef struct {
    bm_node_job job;
    struct sockaddr_storage addr;
    socklen_t len;
    int err;
    char host[NI_MAXHOST], service[NI_MAXSERV];
} bm_node_gni;

static void bm_node_gni_work(bm_node_job *j) {
    bm_node_gni *g = (bm_node_gni *)j;
    int r = getnameinfo((struct sockaddr *)&g->addr, g->len, g->host, sizeof g->host, g->service, sizeof g->service, NI_NAMEREQD);
    g->err = r ? bm_node_eai(r) : 0;
}

static size_t bm_node_gni_done(JSContextRef ctx, bm_node_job *j, JSValueRef *args) {
    bm_node_gni *g = (bm_node_gni *)j;
    args[0] = num(ctx, g->err);
    if (g->err) return 1;
    args[1] = str(ctx, g->host);
    args[2] = str(ctx, g->service);
    return 3;
}

/* getnameinfo(ip, port, callback(err, hostname, service)) -> 0, or UV_EINVAL for a bad ip */
NATIVE(n_dns_getnameinfo) {
    UNUSED;
    if (n < 3) return undef(ctx);
    char *ip = arg_cstr(ctx, n, a, 0);
    int port = (int)arg_num(ctx, n, a, 1, 0);
    bm_node_gni *g = calloc(1, sizeof *g);
    struct sockaddr_in *v4 = (struct sockaddr_in *)&g->addr;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&g->addr;
    if (ip && inet_pton(AF_INET, ip, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons((uint16_t)port);
        g->len = sizeof *v4;
    } else if (ip && inet_pton(AF_INET6, ip, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons((uint16_t)port);
        g->len = sizeof *v6;
    } else {
        free(ip);
        free(g);
        return num(ctx, -EINVAL);
    }
#ifdef __APPLE__
    ((struct sockaddr *)&g->addr)->sa_len = (uint8_t)g->len;
#endif
    free(ip);
    g->job.work = bm_node_gni_work;
    g->job.done = bm_node_gni_done;
    bm_node_queue(ctx, &g->job, a[2]);
    return num(ctx, 0);
}

/* canonicalizeIP(ip) -> its canonical form, or undefined if it isn't one */
NATIVE(n_dns_canonicalize) {
    UNUSED;
    char *ip = arg_cstr(ctx, n, a, 0);
    unsigned char buf[sizeof(struct in6_addr)];
    char out[INET6_ADDRSTRLEN];
    JSValueRef r = undef(ctx);
    if (ip && inet_pton(AF_INET, ip, buf) == 1 && inet_ntop(AF_INET, buf, out, sizeof out)) r = str(ctx, out);
    else if (ip && inet_pton(AF_INET6, ip, buf) == 1 && inet_ntop(AF_INET6, buf, out, sizeof out)) r = str(ctx, out);
    free(ip);
    return r;
}

/* ipv6Bytes(ip) -> 16 bytes, or undefined */
NATIVE(n_dns_ipv6_bytes) {
    UNUSED;
    char *ip = arg_cstr(ctx, n, a, 0);
    unsigned char buf[16];
    JSValueRef r = undef(ctx);
    if (ip && inet_pton(AF_INET6, ip, buf) == 1) r = bm_js_bytes_copy(buf, 16);
    free(ip);
    return r;
}

typedef struct {
    bm_node_job job;
    struct sockaddr_storage servers[8];
    socklen_t lens[8];
    int nservers;
    uint8_t *query;
    size_t query_len;
    int timeout_ms, tries;
    const char *err;
    uint8_t *answer;
    size_t answer_len;
} bm_node_dnsq;

static bool bm_node_dns_wait(int fd, short ev, int ms) {
    struct pollfd p = { fd, ev, 0 };
    return poll(&p, 1, ms) == 1 && (p.revents & ev);
}

/* one query over TCP (for an answer too big for UDP) */
static bool bm_node_dns_tcp(bm_node_dnsq *q, int i) {
    int fd = socket(q->servers[i].ss_family, SOCK_STREAM, 0);
    if (fd < 0) return false;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    bool ok = false;
    if (connect(fd, (struct sockaddr *)&q->servers[i], q->lens[i]) != 0 && errno != EINPROGRESS) goto out;
    if (!bm_node_dns_wait(fd, POLLOUT, q->timeout_ms)) goto out;
    uint8_t len[2] = { (uint8_t)(q->query_len >> 8), (uint8_t)q->query_len };
    if (write(fd, len, 2) != 2 || write(fd, q->query, q->query_len) != (ssize_t)q->query_len) goto out;
    size_t want = 0, got = 0;
    uint8_t head[2];
    size_t head_got = 0;
    while (head_got < 2) {
        if (!bm_node_dns_wait(fd, POLLIN, q->timeout_ms)) goto out;
        ssize_t r = read(fd, head + head_got, 2 - head_got);
        if (r <= 0) goto out;
        head_got += (size_t)r;
    }
    want = (size_t)head[0] << 8 | head[1];
    uint8_t *buf = malloc(want ? want : 1);
    while (got < want) {
        if (!bm_node_dns_wait(fd, POLLIN, q->timeout_ms)) { free(buf); goto out; }
        ssize_t r = read(fd, buf + got, want - got);
        if (r <= 0) { free(buf); goto out; }
        got += (size_t)r;
    }
    free(q->answer);
    q->answer = buf;
    q->answer_len = want;
    ok = true;
out:
    close(fd);
    return ok;
}

static void bm_node_dnsq_work(bm_node_job *j) {
    bm_node_dnsq *q = (bm_node_dnsq *)j;
    q->err = "ETIMEOUT";
    bool refused = false;
    for (int t = 0; t < q->tries; t++) {
        for (int i = 0; i < q->nservers; i++) {
            int fd = socket(q->servers[i].ss_family, SOCK_DGRAM, 0);
            if (fd < 0) continue;
            if (connect(fd, (struct sockaddr *)&q->servers[i], q->lens[i]) != 0 || send(fd, q->query, q->query_len, 0) < 0) {
                close(fd);
                continue;
            }
            uint8_t buf[4096];
            ssize_t r = -1;
            int left = q->timeout_ms << (t < 4 ? t : 4);
            while (bm_node_dns_wait(fd, POLLIN, left)) {
                r = recv(fd, buf, sizeof buf, 0);
                if (r < 0 && errno == ECONNREFUSED) { refused = true; break; }
                /* an answer to this query (its id) */
                if (r >= 12 && buf[0] == q->query[0] && buf[1] == q->query[1]) break;
                r = -1;
            }
            close(fd);
            if (r < 12) continue;
            q->answer = malloc((size_t)r);
            memcpy(q->answer, buf, (size_t)r);
            q->answer_len = (size_t)r;
            q->err = NULL;
            /* truncated: ask again over TCP */
            if (buf[2] & 0x02) bm_node_dns_tcp(q, i);
            return;
        }
    }
    if (refused) q->err = "ECONNREFUSED";
}

static size_t bm_node_dnsq_done(JSContextRef ctx, bm_node_job *j, JSValueRef *args) {
    bm_node_dnsq *q = (bm_node_dnsq *)j;
    if (q->err) {
        args[0] = str(ctx, q->err);
        return 1;
    }
    args[0] = JSValueMakeNull(ctx);
    args[1] = bm_js_bytes_copy(q->answer, q->answer_len);
    return 2;
}

static void bm_node_dnsq_free(bm_node_job *j) {
    bm_node_dnsq *q = (bm_node_dnsq *)j;
    free(q->query);
    free(q->answer);
}

/* query(servers [[ip, port], ...], packet, timeout ms, tries, callback(err code, answer bytes)) */
NATIVE(n_dns_query) {
    UNUSED;
    if (n < 5 || !JSValueIsObject(ctx, a[0])) return undef(ctx);
    bm_node_dnsq *q = calloc(1, sizeof *q);
    JSObjectRef list = (JSObjectRef)a[0];
    JSStringRef len_key = JSStringCreateWithUTF8CString("length");
    int count = (int)JSValueToNumber(ctx, JSObjectGetProperty(ctx, list, len_key, NULL), NULL);
    JSStringRelease(len_key);
    for (int i = 0; i < count && q->nservers < 8; i++) {
        JSValueRef pair = JSObjectGetPropertyAtIndex(ctx, list, (unsigned)i, NULL);
        if (!JSValueIsObject(ctx, pair)) continue;
        JSValueRef ipv = JSObjectGetPropertyAtIndex(ctx, (JSObjectRef)pair, 0, NULL);
        int port = (int)JSValueToNumber(ctx, JSObjectGetPropertyAtIndex(ctx, (JSObjectRef)pair, 1, NULL), NULL);
        char *ip = arg_cstr(ctx, 1, &ipv, 0);
        struct sockaddr_storage *ss = &q->servers[q->nservers];
        memset(ss, 0, sizeof *ss);
        struct sockaddr_in *v4 = (struct sockaddr_in *)ss;
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)ss;
        if (ip && inet_pton(AF_INET, ip, &v4->sin_addr) == 1) {
            v4->sin_family = AF_INET;
            v4->sin_port = htons((uint16_t)(port ? port : 53));
            q->lens[q->nservers++] = sizeof *v4;
        } else if (ip && inet_pton(AF_INET6, ip, &v6->sin6_addr) == 1) {
            v6->sin6_family = AF_INET6;
            v6->sin6_port = htons((uint16_t)(port ? port : 53));
            q->lens[q->nservers++] = sizeof *v6;
        }
        free(ip);
    }
    uint8_t *p;
    size_t len;
    if (!bm_js_bytes_view(a[1], &p, &len)) {
        free(q);
        return undef(ctx);
    }
    q->query = malloc(len);
    memcpy(q->query, p, len);
    q->query_len = len;
    q->timeout_ms = (int)arg_num(ctx, n, a, 2, 2000);
    if (q->timeout_ms <= 0) q->timeout_ms = 2000;
    q->tries = (int)arg_num(ctx, n, a, 3, 4);
    if (q->tries <= 0) q->tries = 4;
    q->job.work = bm_node_dnsq_work;
    q->job.done = bm_node_dnsq_done;
    q->job.free = bm_node_dnsq_free;
    bm_node_queue(ctx, &q->job, a[4]);
    return num(ctx, 0);
}

/* ------------------------------------------------------------------ the engine's heap (v8's
 * getHeapStatistics) */

#include <dlfcn.h>

/* heapStats() -> [live bytes, capacity, extra (external) bytes], from JavaScriptCore's
 * JSGetMemoryUsageStatistics (which walks the heap: not for frequent calls) */
NATIVE(n_heap_stats) {
    UNUSED;
    static JSObjectRef (*stats)(JSContextRef);
    static bool looked;
    if (!looked) {
        looked = true;
        stats = (JSObjectRef (*)(JSContextRef))dlsym(RTLD_DEFAULT, "JSGetMemoryUsageStatistics");
    }
    double v[3] = {0, 0, 0};
    if (stats) {
        JSObjectRef o = stats(ctx);
        const char *keys[3] = { "heapSize", "heapCapacity", "extraMemorySize" };
        for (int i = 0; i < 3; i++) {
            JSStringRef k = JSStringCreateWithUTF8CString(keys[i]);
            v[i] = JSValueToNumber(ctx, JSObjectGetProperty(ctx, o, k, NULL), NULL);
            JSStringRelease(k);
        }
    }
    JSValueRef items[3] = { num(ctx, v[0]), num(ctx, v[1]), num(ctx, v[2]) };
    return array(ctx, 3, items);
}

/* ------------------------------------------------------------------ vm (Node.js's node_contextify.cc)
 *
 * Scripts run with JSEvaluateScript, in this context or a new one. A new context is a global
 * context in the same group (values pass freely between them), whose global object's class
 * forwards lookups and assignments to the sandbox object first, as Node.js's contextified
 * global does; the sandbox is held on the global under a symbol key. */

static JSValueRef bm_vm_key;      /* the symbol the sandbox is held under */

/* (reading the key comes back through the global's callbacks: those calls find nothing here and
 * fall through to the property itself) */
static JSObjectRef bm_vm_sandbox(JSContextRef ctx, JSObjectRef global) {
    static bool busy;
    if (!bm_vm_key || busy) return NULL;
    busy = true;
    JSValueRef v = JSObjectGetPropertyForKey(ctx, global, bm_vm_key, NULL);
    busy = false;
    return v && JSValueIsObject(ctx, v) ? (JSObjectRef)v : NULL;
}

static JSValueRef bm_vm_get(JSContextRef ctx, JSObjectRef object, JSStringRef name, JSValueRef *exc) {
    JSObjectRef sandbox = bm_vm_sandbox(ctx, object);
    if (!sandbox || !JSObjectHasProperty(ctx, sandbox, name)) return NULL;
    return JSObjectGetProperty(ctx, sandbox, name, exc);
}

static bool bm_vm_has(JSContextRef ctx, JSObjectRef object, JSStringRef name) {
    JSObjectRef sandbox = bm_vm_sandbox(ctx, object);
    return sandbox && JSObjectHasProperty(ctx, sandbox, name);
}

static bool bm_vm_set(JSContextRef ctx, JSObjectRef object, JSStringRef name, JSValueRef value, JSValueRef *exc) {
    JSObjectRef sandbox = bm_vm_sandbox(ctx, object);
    if (!sandbox) return false;
    JSObjectSetProperty(ctx, sandbox, name, value, kJSPropertyAttributeNone, exc);
    return true;
}

static bool bm_vm_delete(JSContextRef ctx, JSObjectRef object, JSStringRef name, JSValueRef *exc) {
    JSObjectRef sandbox = bm_vm_sandbox(ctx, object);
    if (sandbox) JSObjectDeleteProperty(ctx, sandbox, name, exc);
    return false;
}

static void bm_vm_names(JSContextRef ctx, JSObjectRef object, JSPropertyNameAccumulatorRef names) {
    JSObjectRef sandbox = bm_vm_sandbox(ctx, object);
    if (!sandbox) return;
    JSPropertyNameArrayRef keys = JSObjectCopyPropertyNames(ctx, sandbox);
    size_t count = JSPropertyNameArrayGetCount(keys);
    for (size_t i = 0; i < count; i++) JSPropertyNameAccumulatorAddName(names, JSPropertyNameArrayGetNameAtIndex(keys, i));
    JSPropertyNameArrayRelease(keys);
}

static void bm_vm_context_finalize(JSObjectRef o) {
    JSGlobalContextRef c = JSObjectGetPrivate(o);
    if (c) JSGlobalContextRelease(c);
}

static JSClassRef bm_vm_context_class(void) {
    static JSClassRef cls;
    if (!cls) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "VMContext";
        def.finalize = bm_vm_context_finalize;
        cls = JSClassCreate(&def);
    }
    return cls;
}

static JSContextRef bm_vm_target(JSContextRef ctx, size_t n, const JSValueRef a[], size_t i) {
    if (i < n && JSValueIsObjectOfClass(ctx, a[i], bm_vm_context_class())) {
        JSGlobalContextRef c = JSObjectGetPrivate((JSObjectRef)a[i]);
        if (c) return c;
    }
    return ctx;
}

/* makeContext(sandbox) -> a context handle; the new global's lookups go to sandbox first */
NATIVE(n_vm_make_context) {
    UNUSED;
    if (n < 1 || !JSValueIsObject(ctx, a[0])) return undef(ctx);
    static JSClassRef global_class;
    if (!global_class) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "Object";
        def.getProperty = bm_vm_get;
        def.hasProperty = bm_vm_has;
        def.setProperty = bm_vm_set;
        def.deleteProperty = bm_vm_delete;
        def.getPropertyNames = bm_vm_names;
        global_class = JSClassCreate(&def);
    }
    if (!bm_vm_key) {
        JSStringRef d = JSStringCreateWithUTF8CString("vm sandbox");
        bm_vm_key = JSValueMakeSymbol(ctx, d);
        JSStringRelease(d);
        JSValueProtect(ctx, bm_vm_key);
    }
    JSGlobalContextRef c = JSGlobalContextCreateInGroup(JSContextGetGroup(ctx), global_class);
    JSObjectSetPropertyForKey(c, JSContextGetGlobalObject(c), bm_vm_key, a[0], kJSPropertyAttributeDontEnum, NULL);
    return JSObjectMake(ctx, bm_vm_context_class(), c);
}

/* the context's global object */
NATIVE(n_vm_global) {
    UNUSED;
    return JSContextGetGlobalObject(bm_vm_target(ctx, n, a, 0));
}

/* check(code, filename, line) -> null, or the SyntaxError */
NATIVE(n_vm_check) {
    UNUSED;
    JSStringRef code = JSValueToStringCopy(ctx, n > 0 ? a[0] : undef(ctx), exc);
    if (!code) return undef(ctx);
    JSStringRef url = n > 1 ? JSValueToStringCopy(ctx, a[1], NULL) : NULL;
    JSValueRef err = NULL;
    bool ok = JSCheckScriptSyntax(ctx, code, url, (int)arg_num(ctx, n, a, 2, 1), &err);
    JSStringRelease(code);
    if (url) JSStringRelease(url);
    return ok || !err ? JSValueMakeNull(ctx) : err;
}

/* run(code, filename, line, context?) -> the script's completion value (or what it throws) */
NATIVE(n_vm_run) {
    UNUSED;
    JSStringRef code = JSValueToStringCopy(ctx, n > 0 ? a[0] : undef(ctx), exc);
    if (!code) return undef(ctx);
    JSStringRef url = n > 1 ? JSValueToStringCopy(ctx, a[1], NULL) : NULL;
    JSContextRef target = bm_vm_target(ctx, n, a, 3);
    JSValueRef r = JSEvaluateScript(target, code, NULL, url, (int)arg_num(ctx, n, a, 2, 1), exc);
    JSStringRelease(code);
    if (url) JSStringRelease(url);
    return r ? r : undef(ctx);
}

/* fn(params [names], body, filename, line, context?) -> a function, as new Function makes one */
NATIVE(n_vm_fn) {
    UNUSED;
    if (n < 2) return undef(ctx);
    JSContextRef target = bm_vm_target(ctx, n, a, 4);
    JSStringRef names[64];
    unsigned count = 0;
    if (JSValueIsObject(ctx, a[0])) {
        JSStringRef len_key = JSStringCreateWithUTF8CString("length");
        unsigned len = (unsigned)JSValueToNumber(ctx, JSObjectGetProperty(ctx, (JSObjectRef)a[0], len_key, NULL), NULL);
        JSStringRelease(len_key);
        for (unsigned i = 0; i < len && count < 64; i++) {
            JSValueRef v = JSObjectGetPropertyAtIndex(ctx, (JSObjectRef)a[0], i, NULL);
            names[count++] = JSValueToStringCopy(ctx, v, NULL);
        }
    }
    JSStringRef body = JSValueToStringCopy(ctx, a[1], exc);
    JSStringRef url = n > 2 ? JSValueToStringCopy(ctx, a[2], NULL) : NULL;
    JSObjectRef f = body ? JSObjectMakeFunction(target, NULL, count, names, body, url, (int)arg_num(ctx, n, a, 3, 1), exc) : NULL;
    for (unsigned i = 0; i < count; i++) JSStringRelease(names[i]);
    if (body) JSStringRelease(body);
    if (url) JSStringRelease(url);
    return f ? (JSValueRef)f : undef(ctx);
}

static void bm_node_vm_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef v = JSObjectMake(ctx, NULL, NULL);
    bm_js_def(ctx, v, "makeContext", n_vm_make_context);
    bm_js_def(ctx, v, "global", n_vm_global);
    bm_js_def(ctx, v, "check", n_vm_check);
    bm_js_def(ctx, v, "run", n_vm_run);
    bm_js_def(ctx, v, "fn", n_vm_fn);
    set(ctx, native, "vm", v);
}

static void bm_node_dns_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef d = JSObjectMake(ctx, NULL, NULL);
    bm_js_def(ctx, d, "getaddrinfo", n_dns_getaddrinfo);
    bm_js_def(ctx, d, "getnameinfo", n_dns_getnameinfo);
    bm_js_def(ctx, d, "canonicalizeIP", n_dns_canonicalize);
    bm_js_def(ctx, d, "ipv6Bytes", n_dns_ipv6_bytes);
    bm_js_def(ctx, d, "query", n_dns_query);
    set(ctx, native, "dns", d);
    bm_js_def(ctx, native, "heapStats", n_heap_stats);
}

/* ------------------------------------------------------------------ zlib (runtime/compress.c, through
 * bm_zs: there when the program's bundle has zlib) */

static void bm_node_zs_finalize(JSObjectRef o) {
    bm_zstream *z = JSObjectGetPrivate(o);
    if (z && bm_zs) bm_zs->close(z);
}

static JSClassRef bm_node_zs_class(void) {
    static JSClassRef cls;
    if (!cls) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = "ZlibHandle";
        def.finalize = bm_node_zs_finalize;
        cls = JSClassCreate(&def);
    }
    return cls;
}

static bm_zstream *bm_node_zs_of(JSContextRef ctx, size_t n, const JSValueRef a[]) {
    if (n < 1 || !JSValueIsObjectOfClass(ctx, a[0], bm_node_zs_class())) return NULL;
    return JSObjectGetPrivate((JSObjectRef)a[0]);
}

/* an error as [message, code, errno], or null for none */
static JSValueRef bm_node_zs_error(JSContextRef ctx, bm_zs_error e) {
    if (!e.message) return JSValueMakeNull(ctx);
    JSValueRef items[3] = { str(ctx, e.message), str(ctx, e.code ? e.code : ""), num(ctx, e.err) };
    return array(ctx, 3, items);
}

/* open(mode) -> handle */
NATIVE(n_zlib_open) {
    UNUSED;
    if (!bm_zs) {
        JSValueRef msg = str(ctx, "zlib is not available in this program (it was built without the codecs)");
        *exc = JSObjectMakeError(ctx, 1, &msg, NULL);
        return undef(ctx);
    }
    bm_zstream *z = bm_zs->open((int)arg_num(ctx, n, a, 0, 0));
    return JSObjectMake(ctx, bm_node_zs_class(), z);
}

/* initZlib(handle, windowBits, level, memLevel, strategy, dictionary?) */
NATIVE(n_zlib_init_zlib) {
    UNUSED;
    bm_zstream *z = bm_node_zs_of(ctx, n, a);
    if (!z) return undef(ctx);
    uint8_t *dict = NULL;
    size_t dl = 0;
    if (n > 5) bm_js_bytes_view(a[5], &dict, &dl);
    bm_zs->init_zlib(z, (int)arg_num(ctx, n, a, 1, 15), (int)arg_num(ctx, n, a, 2, -1), (int)arg_num(ctx, n, a, 3, 8), (int)arg_num(ctx, n, a, 4, 0),
                     dict, dict ? dl : 0);
    return undef(ctx);
}

/* (a Uint32Array's values) */
static uint32_t *bm_node_u32s(JSValueRef v, size_t *count) {
    uint8_t *p;
    size_t len;
    if (!bm_js_bytes_view(v, &p, &len)) {
        *count = 0;
        return NULL;
    }
    *count = len / 4;
    return (uint32_t *)p;
}

/* initBrotli(handle, params Uint32Array, dictionary?) -> error or null */
NATIVE(n_zlib_init_brotli) {
    UNUSED;
    bm_zstream *z = bm_node_zs_of(ctx, n, a);
    if (!z || n < 2) return undef(ctx);
    size_t count;
    uint32_t *params = bm_node_u32s(a[1], &count);
    uint8_t *dict = NULL;
    size_t dl = 0;
    if (n > 2) bm_js_bytes_view(a[2], &dict, &dl);
    return bm_node_zs_error(ctx, bm_zs->init_brotli(z, params, count, dict, dict ? dl : 0));
}

/* initZstd(handle, params Uint32Array, pledgedSrcSize (-1: unknown), dictionary?) -> error or null */
NATIVE(n_zlib_init_zstd) {
    UNUSED;
    bm_zstream *z = bm_node_zs_of(ctx, n, a);
    if (!z || n < 2) return undef(ctx);
    size_t count;
    uint32_t *params = bm_node_u32s(a[1], &count);
    double pledged = arg_num(ctx, n, a, 2, -1);
    uint8_t *dict = NULL;
    size_t dl = 0;
    if (n > 3) bm_js_bytes_view(a[3], &dict, &dl);
    return bm_node_zs_error(ctx, bm_zs->init_zstd(z, params, count, pledged < 0 ? UINT64_MAX : (uint64_t)pledged, dict, dict ? dl : 0));
}

/* write(handle, flush, in|null, inOff, inLen, out, outOff, outLen, state Uint32Array) -> error or
 * null; state gets [avail out, avail in] */
NATIVE(n_zlib_write) {
    UNUSED;
    bm_zstream *z = bm_node_zs_of(ctx, n, a);
    if (!z || n < 9) return undef(ctx);
    uint8_t *in = NULL, *out, *state;
    size_t in_cap = 0, out_cap, state_len;
    if (!JSValueIsNull(ctx, a[2])) bm_js_bytes_view(a[2], &in, &in_cap);
    if (!bm_js_bytes_view(a[5], &out, &out_cap) || !bm_js_bytes_view(a[8], &state, &state_len) || state_len < 8) return undef(ctx);
    size_t in_off = in ? (size_t)arg_num(ctx, n, a, 3, 0) : 0, in_len = in ? (size_t)arg_num(ctx, n, a, 4, 0) : 0;
    size_t out_off = (size_t)arg_num(ctx, n, a, 6, 0), out_len = (size_t)arg_num(ctx, n, a, 7, 0);
    if (in_off > in_cap || in_len > in_cap - in_off || out_off > out_cap || out_len > out_cap - out_off) {
        JSValueRef msg = str(ctx, "zlib: write out of bounds");
        *exc = JSObjectMakeError(ctx, 1, &msg, NULL);
        return undef(ctx);
    }
    uint32_t avail_in, avail_out;
    bm_zs->write(z, (int)arg_num(ctx, n, a, 1, 0), in ? in + in_off : NULL, (uint32_t)in_len, out + out_off, (uint32_t)out_len, &avail_in, &avail_out);
    uint32_t *st = (uint32_t *)state;
    st[0] = avail_out;
    st[1] = avail_in;
    return bm_node_zs_error(ctx, bm_zs->check(z));
}

/* params(handle, level, strategy) -> error or null */
NATIVE(n_zlib_params) {
    UNUSED;
    bm_zstream *z = bm_node_zs_of(ctx, n, a);
    if (!z) return undef(ctx);
    return bm_node_zs_error(ctx, bm_zs->params(z, (int)arg_num(ctx, n, a, 1, 0), (int)arg_num(ctx, n, a, 2, 0)));
}

/* reset(handle) -> error or null */
NATIVE(n_zlib_reset) {
    UNUSED;
    bm_zstream *z = bm_node_zs_of(ctx, n, a);
    if (!z) return undef(ctx);
    return bm_node_zs_error(ctx, bm_zs->reset(z));
}

/* close(handle): frees the stream now */
NATIVE(n_zlib_close) {
    UNUSED;
    bm_zstream *z = bm_node_zs_of(ctx, n, a);
    if (!z) return undef(ctx);
    JSObjectSetPrivate((JSObjectRef)a[0], NULL);
    bm_zs->close(z);
    return undef(ctx);
}

/* crc32(bytes or string, value) -> number */
NATIVE(n_zlib_crc32) {
    UNUSED;
    uint32_t value = (uint32_t)arg_num(ctx, n, a, 1, 0);
    if (!bm_zs || n < 1) return num(ctx, value);
    uint8_t *p;
    size_t len;
    if (bm_js_bytes_view(a[0], &p, &len)) return num(ctx, bm_zs->crc32(value, p, len));
    char *s = arg_cstr(ctx, n, a, 0);
    uint32_t r = s ? bm_zs->crc32(value, (const uint8_t *)s, strlen(s)) : value;
    free(s);
    return num(ctx, r);
}

static void bm_node_zlib_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef z = JSObjectMake(ctx, NULL, NULL);
    set(ctx, z, "available", JSValueMakeBoolean(ctx, bm_zs != NULL));
    bm_js_def(ctx, z, "open", n_zlib_open);
    bm_js_def(ctx, z, "initZlib", n_zlib_init_zlib);
    bm_js_def(ctx, z, "initBrotli", n_zlib_init_brotli);
    bm_js_def(ctx, z, "initZstd", n_zlib_init_zstd);
    bm_js_def(ctx, z, "write", n_zlib_write);
    bm_js_def(ctx, z, "params", n_zlib_params);
    bm_js_def(ctx, z, "reset", n_zlib_reset);
    bm_js_def(ctx, z, "close", n_zlib_close);
    bm_js_def(ctx, z, "crc32", n_zlib_crc32);
    set(ctx, native, "zlib", z);
}

static void bm_node_crypto_install(JSContextRef ctx, JSObjectRef native) {
    JSObjectRef c = JSObjectMake(ctx, NULL, NULL);
    set(ctx, c, "available", JSValueMakeBoolean(ctx, bm_crypto != NULL));
    bm_js_def(ctx, c, "digestSize", n_crypto_digest_size);
    bm_js_def(ctx, c, "hashNew", n_crypto_hash_new);
    bm_js_def(ctx, c, "hmacNew", n_crypto_hmac_new);
    bm_js_def(ctx, c, "update", n_crypto_update);
    bm_js_def(ctx, c, "digest", n_crypto_digest);
    bm_js_def(ctx, c, "hashCopy", n_crypto_hash_copy);
    bm_js_def(ctx, c, "randomFill", n_crypto_random_fill);
    bm_js_def(ctx, c, "pbkdf2", n_crypto_pbkdf2);
    bm_js_def(ctx, c, "scrypt", n_crypto_scrypt);
    bm_js_def(ctx, c, "hkdf", n_crypto_hkdf);
    bm_js_def(ctx, c, "timingSafeEqual", n_crypto_equal);
    set(ctx, native, "crypto", c);
}

/* ------------------------------------------------------------------ install */


void bm_node_install(JSContextRef ctx, JSObjectRef native) {
    bm_js_def(ctx, native, "info", n_info);
    bm_js_def(ctx, native, "cwd", n_cwd);
    bm_js_def(ctx, native, "chdir", n_chdir);
    bm_js_def(ctx, native, "exit", n_exit);
    bm_js_def(ctx, native, "umask", n_umask);
    bm_js_def(ctx, native, "hrtime", n_hrtime);
    bm_js_def(ctx, native, "write", n_write);
    bm_js_def(ctx, native, "isatty", n_isatty);
    bm_js_def(ctx, native, "windowSize", n_window_size);
    bm_js_def(ctx, native, "memoryUsage", n_memory_usage);
    bm_js_def(ctx, native, "cpuUsage", n_cpu_usage);
    bm_js_def(ctx, native, "ids", n_ids);
    bm_js_def(ctx, native, "kill", n_kill);
    bm_js_def(ctx, native, "setRawMode", n_set_raw_mode);

    bm_js_def(ctx, native, "now", n_now);
    bm_js_def(ctx, native, "timerSetup", n_timer_setup);
    bm_js_def(ctx, native, "timerSchedule", n_timer_schedule);
    bm_js_def(ctx, native, "timerRef", n_timer_ref);
    bm_js_def(ctx, native, "requestCheck", n_request_check);

    JSObjectRef os = JSObjectMake(ctx, NULL, NULL);
    bm_js_def(ctx, os, "getOSInformation", n_os_info);
    bm_js_def(ctx, os, "getHostname", n_os_hostname);
    bm_js_def(ctx, os, "getHomeDirectory", n_os_homedir);
    bm_js_def(ctx, os, "getUptime", n_os_uptime);
    bm_js_def(ctx, os, "getTotalMem", n_os_totalmem);
    bm_js_def(ctx, os, "getFreeMem", n_os_freemem);
    bm_js_def(ctx, os, "getLoadAvg", n_os_loadavg);
    bm_js_def(ctx, os, "getCPUs", n_os_cpus);
    bm_js_def(ctx, os, "getInterfaceAddresses", n_os_interfaces);
    bm_js_def(ctx, os, "getUserInfo", n_os_userinfo);
    bm_js_def(ctx, os, "getPriority", n_os_getpriority);
    bm_js_def(ctx, os, "setPriority", n_os_setpriority);
    bm_js_def(ctx, os, "getAvailableParallelism", n_os_parallelism);
    set(ctx, native, "os", os);

    bm_node_fs_install(ctx, native);
    bm_node_crypto_install(ctx, native);
    bm_node_zlib_install(ctx, native);
    bm_node_dns_install(ctx, native);
    bm_node_vm_install(ctx, native);
}
