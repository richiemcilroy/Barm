/* load — a small keep-alive HTTP/1.1 load generator (wrk-style), for bench/http.
 *
 *   load [-c conns] [-t threads] [-d seconds] [-w warmup] [-P pipeline] [-R rate] [-m method] [-b body] [-j] url
 *
 * Each thread drives its share of the connections with kqueue/epoll. Closed loop (default):
 * every connection keeps `pipeline` requests in flight, and latency runs from write to the last
 * byte of the response. Open loop (-R, like wrk2): requests go out on a fixed schedule totalling
 * `rate` per second, and latency runs from each request's *scheduled* time, so a server that
 * falls behind is charged for the queueing it causes (no coordinated omission).
 * Prints a summary (or one JSON object with -j).
 */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#define KQ 1
#else
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#define MAXPIPE 64

typedef struct conn {
    int fd;
    char in[65536];
    size_t in_len;
    uint64_t sent[MAXPIPE];  /* send (or scheduled) times of in-flight requests (ring) */
    int head, inflight;
    uint64_t next;           /* open loop: when the next request is due */
} conn;

typedef struct worker {
    pthread_t th;
    int nconns, first;  /* first: index of this thread's first connection among all */
    conn *conns;
    uint64_t done, errors, non2xx, bytes;
    uint32_t *lat;  /* microseconds, measured phase only */
    size_t nlat, cap;
} worker;

static struct sockaddr_in addr;
static char *req;
static size_t req_len;
static int pipeline = 1;
static double rate;          /* open loop: requests per second in total (0: closed loop) */
static int total_conns;
static _Atomic int phase;  /* 0 warmup, 1 measure, 2 stop */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int dial(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

/* Open loop: sends the requests that are due, each stamped with its scheduled time. */
static bool send_due(conn *c, uint64_t now, uint64_t interval) {
    static __thread char buf[65536];
    size_t len = 0;
    int n = 0;
    while (c->next <= now && c->inflight + n < MAXPIPE && len + req_len <= sizeof buf) {
        memcpy(buf + len, req, req_len);
        len += req_len;
        c->sent[(c->head + c->inflight + n) % MAXPIPE] = c->next;
        c->next += interval;
        n++;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(c->fd, buf + off, len - off);
        if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; return false; }
        off += (size_t)w;
    }
    c->inflight += n;
    return true;
}

static bool send_reqs(conn *c, int n) {
    static __thread char buf[65536];
    size_t len = 0;
    for (int i = 0; i < n; i++) { memcpy(buf + len, req, req_len); len += req_len; }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(c->fd, buf + off, len - off);
        if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; return false; }
        off += (size_t)w;
    }
    uint64_t t = now_ns();
    for (int i = 0; i < n; i++) { c->sent[(c->head + c->inflight) % MAXPIPE] = t; c->inflight++; }
    return true;
}

static void record(worker *w, uint64_t ns) {
    if (w->nlat == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 1 << 16;
        w->lat = realloc(w->lat, w->cap * sizeof *w->lat);
    }
    w->lat[w->nlat++] = (uint32_t)(ns / 1000);
}

/* Consumes complete responses; returns how many, or -1 on a protocol error. */
static int parse(worker *w, conn *c) {
    int got = 0;
    size_t pos = 0;
    while (pos < c->in_len) {
        char *s = c->in + pos;
        size_t avail = c->in_len - pos;
        char *end = memmem(s, avail, "\r\n\r\n", 4);
        if (!end) break;
        size_t hlen = (size_t)(end + 4 - s);
        long clen = 0;
        bool chunked = false;
        for (char *p = s; p < end;) {
            char *nl = memchr(p, '\n', (size_t)(end + 2 - p));
            if (!nl) break;
            if (strncasecmp(p, "content-length:", 15) == 0) clen = strtol(p + 15, NULL, 10);
            else if (strncasecmp(p, "transfer-encoding:", 18) == 0 && memmem(p, (size_t)(nl - p), "chunked", 7)) chunked = true;
            p = nl + 1;
        }
        if (chunked) { /* body length = through the terminating 0-size chunk */
            size_t q = hlen;
            bool complete = false;
            for (;;) {
                char *nl = memmem(s + q, avail - q, "\r\n", 2);
                if (!nl) break;
                long sz = strtol(s + q, NULL, 16);
                size_t next = (size_t)(nl + 2 - s) + (size_t)sz + 2;
                if (next > avail) break;
                q = next;
                if (sz == 0) { complete = true; break; }
            }
            if (!complete) break;
            clen = (long)(q - hlen);
        }
        if (avail < hlen + (size_t)clen) break;
        if (avail < 12 || memcmp(s, "HTTP/1.", 7) != 0) return -1;
        int status = atoi(s + 9);
        if (status < 200 || status > 299) w->non2xx++;
        uint64_t t = c->sent[c->head];
        c->head = (c->head + 1) % MAXPIPE;
        c->inflight--;
        if (atomic_load_explicit(&phase, memory_order_relaxed) == 1) {
            w->done++;
            w->bytes += hlen + (size_t)clen;
            record(w, now_ns() - t);
        }
        pos += hlen + (size_t)clen;
        got++;
    }
    memmove(c->in, c->in + pos, c->in_len - pos);
    c->in_len -= pos;
    return got;
}

static void *run(void *arg) {
    worker *w = arg;
#ifdef KQ
    int q = kqueue();
#else
    int q = epoll_create1(0);
#endif
    for (int i = 0; i < w->nconns; i++) {
        conn *c = &w->conns[i];
        c->fd = dial();
        if (c->fd < 0) { perror("connect"); exit(1); }
#ifdef KQ
        struct kevent ev;
        EV_SET(&ev, c->fd, EVFILT_READ, EV_ADD, 0, 0, c);
        kevent(q, &ev, 1, NULL, 0, NULL);
#else
        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
        epoll_ctl(q, EPOLL_CTL_ADD, c->fd, &ev);
#endif
        if (rate <= 0) send_reqs(c, pipeline);
    }
    /* open loop: connection k of all sends every `interval`, staggered by k/rate */
    uint64_t interval = rate > 0 ? (uint64_t)((double)total_conns * 1e9 / rate) : 0;
    if (rate > 0) {
        uint64_t t0 = now_ns();
        for (int i = 0; i < w->nconns; i++) w->conns[i].next = t0 + (uint64_t)((double)(w->first + i) * 1e9 / rate);
#ifndef KQ
        prctl(PR_SET_TIMERSLACK, 1UL); /* wake on schedule, not 50 µs late */
#endif
    }
    while (atomic_load_explicit(&phase, memory_order_relaxed) < 2) {
        uint64_t wait_ns = 50000000;
        if (rate > 0) {
            uint64_t now = now_ns(), soonest = UINT64_MAX;
            for (int i = 0; i < w->nconns; i++) {
                conn *c = &w->conns[i];
                if (c->next <= now && !send_due(c, now, interval)) { perror("write"); exit(1); }
                if (c->next < soonest) soonest = c->next;
            }
            now = now_ns();
            wait_ns = soonest > now ? soonest - now : 0;
#ifdef KQ
            /* macOS coalesces timers (a wakeup can be ms late): poll when a send is near */
            if (wait_ns < 2000000) wait_ns = 0;
#endif
        }
#ifdef KQ
        struct kevent evs[256];
        struct timespec to = { (time_t)(wait_ns / 1000000000u), (long)(wait_ns % 1000000000u) };
        int n = kevent(q, NULL, 0, evs, 256, &to);
#else
        struct epoll_event evs[256];
        struct timespec to = { (time_t)(wait_ns / 1000000000u), (long)(wait_ns % 1000000000u) };
        int n = (int)syscall(SYS_epoll_pwait2, q, evs, 256, &to, NULL, 0);
        if (n < 0 && errno == ENOSYS) n = epoll_wait(q, evs, 256, (int)(wait_ns / 1000000));
#endif
        for (int i = 0; i < n; i++) {
#ifdef KQ
            conn *c = evs[i].udata;
#else
            conn *c = evs[i].data.ptr;
#endif
            for (;;) {
                ssize_t r = read(c->fd, c->in + c->in_len, sizeof c->in - c->in_len);
                if (r > 0) { c->in_len += (size_t)r; if (c->in_len < sizeof c->in) continue; }
                else if (r == 0) { w->errors++; fprintf(stderr, "connection closed by server\n"); exit(1); }
                else if (errno != EAGAIN && errno != EINTR) { w->errors++; perror("read"); exit(1); }
                break;
            }
            int got = parse(w, c);
            if (got < 0) { fprintf(stderr, "bad response\n"); exit(1); }
            if (got && rate <= 0) send_reqs(c, got);
        }
    }
    return NULL;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    int conns = 64, threads = 4;
    double secs = 5, warm = 1;
    const char *method = "GET", *body = NULL;
    bool json = false;
    int opt;
    while ((opt = getopt(argc, argv, "c:t:d:w:P:R:m:b:j")) != -1) {
        switch (opt) {
        case 'c': conns = atoi(optarg); break;
        case 't': threads = atoi(optarg); break;
        case 'd': secs = atof(optarg); break;
        case 'w': warm = atof(optarg); break;
        case 'P': pipeline = atoi(optarg); break;
        case 'R': rate = atof(optarg); break;
        case 'm': method = optarg; break;
        case 'b': body = optarg; break;
        case 'j': json = true; break;
        default: fprintf(stderr, "usage: load [-c conns] [-t threads] [-d secs] [-w warmup] [-P pipeline] [-m method] [-b body] [-j] http://host:port/path\n"); return 2;
        }
    }
    if (optind >= argc || strncmp(argv[optind], "http://", 7) != 0) { fprintf(stderr, "load: need an http:// url\n"); return 2; }
    if (pipeline < 1 || pipeline > MAXPIPE) { fprintf(stderr, "load: pipeline must be 1..%d\n", MAXPIPE); return 2; }
    if (threads > conns) threads = conns;
    char host[256] = {0}, path[1024] = "/";
    int port = 80;
    const char *u = argv[optind] + 7, *slash = strchr(u, '/'), *colon = strchr(u, ':');
    size_t hl = slash ? (size_t)(slash - u) : strlen(u);
    if (colon && (!slash || colon < slash)) { hl = (size_t)(colon - u); port = atoi(colon + 1); }
    memcpy(host, u, hl < 255 ? hl : 255);
    if (slash) snprintf(path, sizeof path, "%s", slash);
    struct hostent *he = gethostbyname(host);
    if (!he) { fprintf(stderr, "load: can't resolve %s\n", host); return 1; }
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof addr.sin_addr);

    char buf[4096];
    int n = body ? snprintf(buf, sizeof buf, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s", method, path, host, port, strlen(body), body)
                 : snprintf(buf, sizeof buf, "%s %s HTTP/1.1\r\nHost: %s:%d\r\n\r\n", method, path, host, port);
    req = buf;
    req_len = (size_t)n;
    signal(SIGPIPE, SIG_IGN);

    worker *ws = calloc((size_t)threads, sizeof *ws);
    total_conns = conns;
    for (int i = 0, first = 0; i < threads; i++) {
        ws[i].nconns = conns / threads + (i < conns % threads);
        ws[i].first = first;
        first += ws[i].nconns;
        ws[i].conns = calloc((size_t)ws[i].nconns, sizeof(conn));
    }
    for (int i = 0; i < threads; i++) pthread_create(&ws[i].th, NULL, run, &ws[i]);
    usleep((useconds_t)(warm * 1e6));
    atomic_store(&phase, 1);
    uint64_t t0 = now_ns();
    usleep((useconds_t)(secs * 1e6));
    atomic_store(&phase, 2);
    double elapsed = (double)(now_ns() - t0) / 1e9;
    for (int i = 0; i < threads; i++) pthread_join(ws[i].th, NULL);

    uint64_t done = 0, non2xx = 0, bytes = 0;
    size_t nlat = 0;
    for (int i = 0; i < threads; i++) { done += ws[i].done; non2xx += ws[i].non2xx; bytes += ws[i].bytes; nlat += ws[i].nlat; }
    uint32_t *lat = malloc((nlat ? nlat : 1) * sizeof *lat);
    size_t k = 0;
    double sum = 0;
    for (int i = 0; i < threads; i++) {
        memcpy(lat + k, ws[i].lat, ws[i].nlat * sizeof *lat);
        k += ws[i].nlat;
    }
    for (size_t i = 0; i < nlat; i++) sum += lat[i];
    qsort(lat, nlat, sizeof *lat, cmp_u32);
#define PCT(p) (nlat ? lat[(size_t)((double)(nlat - 1) * (p))] : 0)
    double rps = (double)done / elapsed;
    double avg = nlat ? sum / (double)nlat : 0;
    if (json) {
        printf("{\"rps\":%.0f,\"requests\":%llu,\"non2xx\":%llu,\"mb_per_s\":%.2f,\"avg_us\":%.1f,\"p50_us\":%u,\"p90_us\":%u,\"p99_us\":%u,\"p999_us\":%u,\"max_us\":%u}\n", rps,
               (unsigned long long)done, (unsigned long long)non2xx, (double)bytes / elapsed / 1e6, avg, PCT(0.5), PCT(0.9), PCT(0.99), PCT(0.999), nlat ? lat[nlat - 1] : 0);
    } else {
        if (rate > 0) printf("%d connections, %d threads, open loop at %.0f req/s, %.1fs\n", conns, threads, rate, elapsed);
        else printf("%d connections, %d threads, pipeline %d, %.1fs\n", conns, threads, pipeline, elapsed);
        printf("  requests/s  %.0f   (%llu requests, %llu non-2xx, %.1f MB/s)\n", rps, (unsigned long long)done, (unsigned long long)non2xx, (double)bytes / elapsed / 1e6);
        printf("  latency     avg %.0fus  p50 %uus  p90 %uus  p99 %uus  p99.9 %uus  max %uus\n", avg, PCT(0.5), PCT(0.9), PCT(0.99), PCT(0.999), nlat ? lat[nlat - 1] : 0);
    }
    return 0;
}
