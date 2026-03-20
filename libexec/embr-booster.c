/*
 * embr-booster — priority-scheduling C proxy between Emacs and embr.py.
 *
 * Sits between Emacs and embr.py on stdin/stdout, providing:
 *   - Priority-based message scheduling (P0/P1 ordered > P2 > P3)
 *   - Mousemove coalescing (P2: replace-latest)
 *   - Frame notification coalescing (P3: replace-latest)
 *   - Input-priority windowing (suppress frames after input)
 *   - Frame rate limiting
 *   - Backpressure via bounded queues (P0/P1 never dropped)
 *   - Control-plane hints to daemon (desired_fps, frame_shed_level)
 *
 * Usage: embr-booster [OPTIONS] -- COMMAND [ARGS...]
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <libgen.h>
#include <time.h>
#include <unistd.h>

/* ── Constants ──────────────────────────────────────────────────── */

#define LINE_BUF_SIZE       (64 * 1024)
#define MAX_LINE_LEN        (LINE_BUF_SIZE - 1)
#define MAX_EPOLL_EVENTS    8
#define SHUTDOWN_TIMEOUT_MS 2000
#define QUEUE_DEPTH_INTERVAL_MS 1000

/* ── Log levels ─────────────────────────────────────────────────── */

enum log_level {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
    LOG_TRACE = 4,
};

static enum log_level g_log_level = LOG_WARN;

static const char *log_level_names[] = {
    "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

#define LOG(level, ...) do { \
    if ((level) <= g_log_level) log_msg(level, __VA_ARGS__); \
} while (0)

static void log_msg(enum log_level level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "embr-booster [%s] ", log_level_names[level]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ── Monotonic clock ────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ── Priority classes ───────────────────────────────────────────── */

enum priority {
    PRIO_CMD  = 0,  /* P0+P1: ordered command FIFO (never dropped) */
    PRIO_P2   = 1,  /* mousemove — coalesce to latest */
    PRIO_P3   = 2,  /* frame notifications — coalesce to latest */
    PRIO_COUNT = 3,
};

/* Classification result (for telemetry) */
enum msg_class {
    CLASS_P0 = 0,
    CLASS_P1 = 1,
    CLASS_P2 = 2,
    CLASS_P3 = 3,
};

/* ── Message ────────────────────────────────────────────────────── */

typedef struct {
    char *data;     /* heap-allocated JSON line (includes trailing \n) */
    size_t len;
} msg_t;

/* ── Ring buffer ────────────────────────────────────────────────── */

typedef struct {
    msg_t *buf;
    int capacity;
    int head;       /* next read position */
    int tail;       /* next write position */
    int count;
} ring_t;

static int ring_init(ring_t *r, int capacity) {
    r->buf = calloc((size_t)capacity, sizeof(msg_t));
    if (!r->buf) return -1;
    r->capacity = capacity;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    return 0;
}

static void ring_free(ring_t *r) {
    if (!r->buf) return;
    for (int i = 0; i < r->count; i++) {
        int idx = (r->head + i) % r->capacity;
        free(r->buf[idx].data);
    }
    free(r->buf);
    r->buf = NULL;
}

static int ring_empty(const ring_t *r) {
    return r->count == 0;
}

static int ring_full(const ring_t *r) {
    return r->count == r->capacity;
}

static int ring_push(ring_t *r, char *data, size_t len) {
    if (ring_full(r)) return -1;
    r->buf[r->tail].data = data;
    r->buf[r->tail].len = len;
    r->tail = (r->tail + 1) % r->capacity;
    r->count++;
    return 0;
}

static msg_t *ring_peek(const ring_t *r) {
    if (ring_empty(r)) return NULL;
    return &r->buf[r->head];
}

static void ring_pop(ring_t *r) {
    if (ring_empty(r)) return;
    r->head = (r->head + 1) % r->capacity;
    r->count--;
}

/* Replace the single entry for coalescing slots.
 * For P2/P3: capacity=1, so this replaces the only entry. */
static void ring_replace_tail(ring_t *r, char *data, size_t len) {
    if (ring_empty(r)) return;
    int idx = (r->tail - 1 + r->capacity) % r->capacity;
    free(r->buf[idx].data);
    r->buf[idx].data = data;
    r->buf[idx].len = len;
}

/* ── Line buffer ────────────────────────────────────────────────── */

typedef struct {
    char buf[LINE_BUF_SIZE];
    size_t len;
} linebuf_t;

/* ── Write buffer for partial writes ───────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t pos;     /* bytes already written */
} writebuf_t;

/* ── Direction ──────────────────────────────────────────────────── */

enum direction {
    DIR_DOWNSTREAM = 0,  /* Emacs -> Python */
    DIR_UPSTREAM   = 1,  /* Python -> Emacs */
};

/* ── Configuration ──────────────────────────────────────────────── */

typedef struct {
    int queue_capacity;
    int queue_capacity_p2;
    int queue_capacity_p3;
    int input_priority_window_ms;
    int frame_forward_max_hz;
    const char *stats_path;
    const char *frame_path;  /* E3: inotify frame detection path */
    int child_argc;
    char **child_argv;
} config_t;

/* ── Stats / telemetry ──────────────────────────────────────────── */

static FILE *g_stats_file = NULL;

static void stats_log(const char *event, const char *extra) {
    if (!g_stats_file) return;
    fprintf(g_stats_file, "{\"ts_ms\":%" PRIu64 ",\"event\":\"%s\"%s%s}\n",
            (uint64_t)now_ms(), event,
            extra ? "," : "", extra ? extra : "");
    fflush(g_stats_file);
}

static void stats_logf(const char *event, const char *fmt, ...) {
    if (!g_stats_file) return;
    char extra[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
    stats_log(event, extra);
}

/* ── Signal state ───────────────────────────────────────────────── */

static volatile sig_atomic_t g_got_sigchld = 0;
static volatile sig_atomic_t g_got_sigterm = 0;

static void handle_sigchld(int sig) { (void)sig; g_got_sigchld = 1; }
static void handle_sigterm(int sig) { (void)sig; g_got_sigterm = 1; }

/* ── Globals ────────────────────────────────────────────────────── */

static int g_epoll_fd = -1;
static pid_t g_child_pid = -1;

/* fds */
static int g_child_stdin  = -1;  /* we write to child */
static int g_child_stdout = -1;  /* we read from child (commands/responses) */
static int g_child_stderr = -1;  /* child stderr -> our stderr */
static int g_child_frame  = -1;  /* out-of-band frame channel (fd 3 in child) */

/* Line buffers for reading */
static linebuf_t g_emacs_lb;
static linebuf_t g_child_lb;
static linebuf_t g_child_err_lb;
static linebuf_t g_child_frame_lb;

/*
 * Queue architecture (per direction):
 *
 *   CMD queue (ring_t):  P0 + P1 messages in arrival order.
 *                        Never dropped.  When full, stop reading (backpressure).
 *   P2 slot (ring_t, capacity from config): mousemove, coalesce to latest.
 *   P3 slot (ring_t, capacity from config): frame notifications, coalesce to latest.
 *
 * Drain order: CMD first (preserving arrival order of P0/P1),
 * then P2, then P3.  This ensures commands are never reordered
 * relative to each other while low-priority traffic yields.
 */
static ring_t g_downstream[PRIO_COUNT];
static ring_t g_upstream[PRIO_COUNT];

/* Write buffers for partial writes */
static writebuf_t g_child_wb;
static writebuf_t g_emacs_wb;

/* Input-priority window */
static uint64_t g_input_priority_until = 0;
static uint64_t g_input_priority_window_ms_cfg = 125;
static int g_input_priority_active = 0;

/* Frame rate limiting */
static uint64_t g_last_frame_forward_ms = 0;
static uint64_t g_frame_interval_ms = 50;

/* EPOLLOUT tracking */
static int g_child_stdin_epollout = 0;
static int g_emacs_stdout_epollout = 0;

/* Backpressure: stop reading from Emacs stdin when CMD queue full */
static int g_emacs_stdin_registered = 0;

/* E3: inotify frame detection — bypass frame pipe entirely */
static int g_inotify_fd = -1;
static const char *g_frame_path = NULL;
static const char *g_frame_basename = NULL;

/* Telemetry: periodic queue depth emission */
static uint64_t g_last_queue_depth_ms = 0;

/* M6 control-plane escalation: pressure hints to daemon */
static int g_hint_shed_level = 0;  /* 0=none, 1=light, 2=heavy */
static uint64_t g_last_hint_ms = 0;
#define HINT_INTERVAL_MS 500
#define HINT_P2_THRESHOLD 2   /* P2 coalesces seen recently -> light pressure */

/* Pressure tracking for hints */
static int g_recent_coalesces = 0;  /* coalesce events since last hint check */
static int g_recent_p1_count = 0;   /* P1 messages since last hint check */

/* ── Forward declarations ───────────────────────────────────────── */

static void shutdown_child(void);

/* ── fd helpers ─────────────────────────────────────────────────── */

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int epoll_add(int fd, uint32_t events) {
    struct epoll_event ev = { .events = events, .data.fd = fd };
    return epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static int epoll_del(int fd) {
    return epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

/* ── Message classification ─────────────────────────────────────── */

/*
 * Classify downstream (Emacs -> Python) messages.
 * Returns the queue index (PRIO_CMD or PRIO_P2) and sets *cls
 * to the detailed class for telemetry.
 */
static enum priority classify_downstream(const char *line, size_t len,
                                          enum msg_class *cls) {
    (void)len;

    if (strstr(line, "\"cmd\":\"mousemove\"")) {
        *cls = CLASS_P2;
        return PRIO_P2;
    }

    /* P1 commands go into CMD queue but are tagged P1 for telemetry */
    if (strstr(line, "\"cmd\":\"key\"") ||
        strstr(line, "\"cmd\":\"type\"") ||
        strstr(line, "\"cmd\":\"click\"") ||
        strstr(line, "\"cmd\":\"mousedown\"") ||
        strstr(line, "\"cmd\":\"mouseup\"") ||
        strstr(line, "\"cmd\":\"scroll\"") ||
        strstr(line, "\"cmd\":\"new-tab\"") ||
        strstr(line, "\"cmd\":\"close-tab\"") ||
        strstr(line, "\"cmd\":\"switch-tab\"") ||
        strstr(line, "\"cmd\":\"list-tabs\"")) {
        *cls = CLASS_P1;
        return PRIO_CMD;
    }

    /* P0: navigate, back, forward, refresh, quit, init, etc. */
    *cls = CLASS_P0;
    return PRIO_CMD;
}

/*
 * Classify upstream (Python -> Emacs) messages.
 */
static enum priority classify_upstream(const char *line, size_t len,
                                        enum msg_class *cls) {
    (void)len;

    if (strstr(line, "\"frame\":true")) {
        *cls = CLASS_P3;
        return PRIO_P3;
    }

    *cls = CLASS_P0;
    return PRIO_CMD;
}

/* ── Queue helpers ──────────────────────────────────────────────── */

static int any_downstream_pending(void) {
    for (int i = 0; i < PRIO_COUNT; i++)
        if (!ring_empty(&g_downstream[i])) return 1;
    if (g_child_wb.data && g_child_wb.pos < g_child_wb.len) return 1;
    return 0;
}

static int any_upstream_pending(void) {
    for (int i = 0; i < PRIO_COUNT; i++)
        if (!ring_empty(&g_upstream[i])) return 1;
    if (g_emacs_wb.data && g_emacs_wb.pos < g_emacs_wb.len) return 1;
    return 0;
}

static void update_epollout_child(void) {
    int want = any_downstream_pending();
    if (want != g_child_stdin_epollout && g_child_stdin >= 0) {
        if (want) {
            struct epoll_event ev = { .events = EPOLLOUT, .data.fd = g_child_stdin };
            epoll_ctl(g_epoll_fd, g_child_stdin_epollout ? EPOLL_CTL_MOD : EPOLL_CTL_ADD,
                      g_child_stdin, &ev);
        } else {
            epoll_del(g_child_stdin);
        }
        g_child_stdin_epollout = want;
    }
}

static void update_epollout_emacs(void) {
    int want = any_upstream_pending();
    if (want != g_emacs_stdout_epollout) {
        if (want) {
            struct epoll_event ev = { .events = EPOLLOUT, .data.fd = STDOUT_FILENO };
            epoll_ctl(g_epoll_fd, g_emacs_stdout_epollout ? EPOLL_CTL_MOD : EPOLL_CTL_ADD,
                      STDOUT_FILENO, &ev);
        } else {
            epoll_del(STDOUT_FILENO);
        }
        g_emacs_stdout_epollout = want;
    }
}

/*
 * Backpressure: manage Emacs stdin EPOLLIN registration based on
 * downstream CMD queue capacity.
 */
static void update_backpressure(void) {
    int cmd_full = ring_full(&g_downstream[PRIO_CMD]);
    if (cmd_full && g_emacs_stdin_registered) {
        epoll_del(STDIN_FILENO);
        g_emacs_stdin_registered = 0;
        LOG(LOG_DEBUG, "backpressure: stopped reading from Emacs (CMD queue full)");
    } else if (!cmd_full && !g_emacs_stdin_registered) {
        epoll_add(STDIN_FILENO, EPOLLIN);
        g_emacs_stdin_registered = 1;
        LOG(LOG_DEBUG, "backpressure: resumed reading from Emacs");
    }
}

/* ── Telemetry: queue depth ─────────────────────────────────────── */

static void emit_queue_depth(void) {
    if (!g_stats_file) return;
    uint64_t now = now_ms();
    if (now - g_last_queue_depth_ms < QUEUE_DEPTH_INTERVAL_MS)
        return;
    g_last_queue_depth_ms = now;

    stats_logf("queue_depth",
        "\"down_cmd\":%d,\"down_p2\":%d,\"down_p3\":%d,"
        "\"up_cmd\":%d,\"up_p2\":%d,\"up_p3\":%d",
        g_downstream[PRIO_CMD].count, g_downstream[PRIO_P2].count,
        g_downstream[PRIO_P3].count,
        g_upstream[PRIO_CMD].count, g_upstream[PRIO_P2].count,
        g_upstream[PRIO_P3].count);
}

/* ── M6: Control-plane pressure hints ───────────────────────────── */

/*
 * Periodically evaluate pressure and send hints to the daemon.
 * Hints are additive JSON commands injected into the downstream queue.
 * The daemon treats unknown commands gracefully (returns error but
 * continues), and we add explicit support in embr.py.
 */
static void maybe_send_hint(void) {
    if (g_child_stdin < 0) return;
    uint64_t now = now_ms();
    if (now - g_last_hint_ms < HINT_INTERVAL_MS)
        return;

    int new_level = 0;
    /* Determine pressure level from recent activity */
    if (g_recent_coalesces > 5 || g_recent_p1_count > 20)
        new_level = 2;  /* heavy */
    else if (g_recent_coalesces > 0 || g_recent_p1_count > 5)
        new_level = 1;  /* light */

    g_recent_coalesces = 0;
    g_recent_p1_count = 0;
    g_last_hint_ms = now;

    if (new_level == g_hint_shed_level)
        return;

    g_hint_shed_level = new_level;

    /* Build hint JSON */
    char hint[256];
    int desired_fps;
    if (new_level == 2)
        desired_fps = 15;
    else if (new_level == 1)
        desired_fps = 30;
    else
        desired_fps = 0;  /* 0 = no override, use configured value */

    int n = snprintf(hint, sizeof(hint),
        "{\"cmd\":\"_booster_hint\",\"desired_fps\":%d,\"frame_shed_level\":%d}\n",
        desired_fps, new_level);

    if (n > 0 && (size_t)n < sizeof(hint)) {
        char *copy = malloc((size_t)n);
        if (copy) {
            memcpy(copy, hint, (size_t)n);
            if (ring_push(&g_downstream[PRIO_CMD], copy, (size_t)n) != 0) {
                free(copy);
            } else {
                stats_logf("hint_sent", "\"desired_fps\":%d,\"frame_shed_level\":%d",
                           desired_fps, new_level);
                LOG(LOG_DEBUG, "sent pressure hint: level=%d fps=%d",
                    new_level, desired_fps);
                update_epollout_child();
            }
        }
    }
}

/* ── Enqueue ────────────────────────────────────────────────────── */

/*
 * Enqueue a downstream message.  Returns 0 on success, -1 if the
 * CMD queue is full (signals extract_lines to stop).
 */
static int enqueue_downstream(const char *line, size_t len) {
    enum msg_class cls;
    enum priority p = classify_downstream(line, len, &cls);
    ring_t *r = &g_downstream[p];

    char *copy = malloc(len);
    if (!copy) {
        LOG(LOG_ERROR, "malloc failed for downstream message");
        return 0;  /* don't stall extraction on OOM */
    }
    memcpy(copy, line, len);

    stats_logf("rx_emacs", "\"class\":%d,\"len\":%zu", cls, len);

    if (p == PRIO_P2) {
        /* Mousemove: coalesce (replace-latest) */
        if (!ring_empty(r)) {
            ring_replace_tail(r, copy, len);
            g_recent_coalesces++;
            stats_logf("coalesce_mousemove", "\"reason\":\"replace_latest\"");
            LOG(LOG_TRACE, "coalesced mousemove");
        } else {
            if (ring_push(r, copy, len) != 0) {
                free(copy);
                stats_logf("drop_mousemove", "\"reason\":\"queue_full\"");
                LOG(LOG_DEBUG, "dropped mousemove (queue full)");
            }
        }
    } else {
        /* PRIO_CMD (P0/P1): never drop.  If queue is full, reject
         * the line so extract_lines stops and leaves it in the
         * buffer for the next drain cycle. */
        if (ring_push(r, copy, len) != 0) {
            free(copy);
            LOG(LOG_DEBUG, "downstream CMD queue full, pausing extraction");
            update_epollout_child();
            update_backpressure();
            return -1;
        }
    }

    /* P1 triggers input-priority window */
    if (cls == CLASS_P1) {
        g_input_priority_until = now_ms() + g_input_priority_window_ms_cfg;
        if (!g_input_priority_active) {
            g_input_priority_active = 1;
            stats_logf("input_priority_start", "\"cmd\":\"P1\"");
            LOG(LOG_TRACE, "input priority window started");
        }
        g_recent_p1_count++;
    }

    update_epollout_child();
    update_backpressure();
    return 0;
}

/*
 * Enqueue an upstream message.  Returns 0 on success, -1 if the
 * CMD queue is full (signals extract_lines to stop).
 */
static int enqueue_upstream(const char *line, size_t len) {
    enum msg_class cls;
    enum priority p = classify_upstream(line, len, &cls);
    ring_t *r = &g_upstream[p];

    char *copy = malloc(len);
    if (!copy) {
        LOG(LOG_ERROR, "malloc failed for upstream message");
        return 0;
    }
    memcpy(copy, line, len);

    stats_logf("rx_python", "\"class\":%d,\"len\":%zu", cls, len);

    if (p == PRIO_P3) {
        /* Frame: coalesce (replace-latest) */
        if (!ring_empty(r)) {
            ring_replace_tail(r, copy, len);
            g_recent_coalesces++;
            stats_logf("coalesce_frame", "\"reason\":\"replace_latest\"");
            LOG(LOG_TRACE, "coalesced frame notification");
        } else {
            if (ring_push(r, copy, len) != 0) {
                free(copy);
                stats_logf("drop_frame", "\"reason\":\"queue_full\"");
                LOG(LOG_DEBUG, "dropped frame (queue full)");
            }
        }
    } else {
        /* PRIO_CMD (responses/errors): never drop.  If queue is
         * full, reject so extract_lines pauses and the lines
         * stay in the read buffer until we drain. */
        if (ring_push(r, copy, len) != 0) {
            free(copy);
            LOG(LOG_DEBUG, "upstream CMD queue full, pausing extraction");
            update_epollout_emacs();
            return -1;
        }
    }

    update_epollout_emacs();
    return 0;
}

/* ── Drain helpers ──────────────────────────────────────────────── */

static ssize_t try_write(int fd, const char *data, size_t len) {
    ssize_t n = write(fd, data, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return n;
}

/*
 * Drain queues towards a writable fd.
 *
 * Order: CMD (P0+P1 in arrival order) -> P2 -> P3
 * For upstream, P3 is subject to input-priority suppression and rate limiting.
 *
 * P0/P1 are never reordered relative to each other because they share
 * a single FIFO.  P2/P3 may be delayed but that is intentional.
 */
static void drain_to_fd(int fd, ring_t queues[], writebuf_t *wb,
                         enum direction dir) {
    /* First, finish any partial write */
    if (wb->data && wb->pos < wb->len) {
        ssize_t n = try_write(fd, wb->data + wb->pos, wb->len - wb->pos);
        if (n < 0) {
            LOG(LOG_ERROR, "write error: %s", strerror(errno));
            return;
        }
        wb->pos += (size_t)n;
        if (wb->pos < wb->len)
            return;
        free(wb->data);
        wb->data = NULL;
        wb->len = 0;
        wb->pos = 0;
    }

    /* Try each queue in priority order: CMD -> P2 -> P3 */
    for (int p = 0; p < PRIO_COUNT; p++) {
        if (ring_empty(&queues[p]))
            continue;

        /* Input-priority suppression and rate limiting on P3 upstream */
        if (dir == DIR_UPSTREAM && p == PRIO_P3) {
            uint64_t now = now_ms();
            if (now < g_input_priority_until) {
                LOG(LOG_TRACE, "suppressing P3 (input priority active)");
                continue;
            }
            /* Emit input_priority_end when window expires */
            if (g_input_priority_active) {
                g_input_priority_active = 0;
                stats_log("input_priority_end", NULL);
                LOG(LOG_TRACE, "input priority window ended");
            }
            if (g_frame_interval_ms > 0 &&
                now - g_last_frame_forward_ms < g_frame_interval_ms) {
                LOG(LOG_TRACE, "suppressing P3 (rate limit)");
                continue;
            }
        }

        msg_t *msg = ring_peek(&queues[p]);
        if (!msg) continue;

        ssize_t n = try_write(fd, msg->data, msg->len);
        if (n < 0) {
            LOG(LOG_ERROR, "write error draining queue %d: %s", p, strerror(errno));
            return;
        }
        if ((size_t)n < msg->len) {
            /* Partial write — save remainder */
            size_t rem = msg->len - (size_t)n;
            wb->data = malloc(rem);
            if (wb->data) {
                memcpy(wb->data, msg->data + n, rem);
                wb->len = rem;
                wb->pos = 0;
            }
            free(msg->data);
            msg->data = NULL;
            ring_pop(&queues[p]);

            if (dir == DIR_UPSTREAM && p == PRIO_P3)
                g_last_frame_forward_ms = now_ms();
            if (dir == DIR_DOWNSTREAM)
                stats_logf("tx_python", "\"class\":%d", p);
            else
                stats_logf("tx_emacs", "\"class\":%d", p);
            return;
        }

        /* Full write succeeded */
        if (dir == DIR_UPSTREAM && p == PRIO_P3)
            g_last_frame_forward_ms = now_ms();
        if (dir == DIR_DOWNSTREAM)
            stats_logf("tx_python", "\"class\":%d", p);
        else
            stats_logf("tx_emacs", "\"class\":%d", p);

        free(msg->data);
        msg->data = NULL;
        ring_pop(&queues[p]);

        /* Drain one message per call for fairness */
        break;
    }

    /* After draining CMD, check if backpressure can be released */
    if (dir == DIR_DOWNSTREAM)
        update_backpressure();
}

/* ── Line extraction from read buffer ───────────────────────────── */

/*
 * Callback returns 0 on success, -1 if the queue is full and
 * extraction should stop.  Unprocessed lines stay in the buffer
 * and will be extracted on the next drain cycle.
 */
typedef int (*line_cb_t)(const char *line, size_t len);

static void extract_lines(linebuf_t *lb, line_cb_t cb) {
    char *start = lb->buf;
    char *end = lb->buf + lb->len;
    char *nl;

    while ((nl = memchr(start, '\n', (size_t)(end - start))) != NULL) {
        size_t line_len = (size_t)(nl - start + 1);
        if (cb(start, line_len) != 0) {
            /* Queue full — stop extracting, keep remaining data */
            break;
        }
        start = nl + 1;
    }

    /* Move remaining partial data to front */
    size_t remaining = (size_t)(end - start);
    if (remaining > 0 && start != lb->buf)
        memmove(lb->buf, start, remaining);
    lb->len = remaining;

    /* Oversized-line guardrail: if buffer is full with no newline,
     * the line exceeds MAX_LINE_LEN.  Discard and log error. */
    if (lb->len >= LINE_BUF_SIZE) {
        LOG(LOG_ERROR, "oversized line (>%d bytes), discarding", LINE_BUF_SIZE);
        stats_logf("error", "\"reason\":\"oversized_line\",\"len\":%zu", lb->len);
        lb->len = 0;
    }
}

/* ── Child process ──────────────────────────────────────────────── */

/*
 * Frame channel fd number.  The child inherits this fd as the write
 * end of the out-of-band frame pipe.  The daemon checks for this fd
 * and writes frame notifications there instead of stdout, achieving
 * true transport-level separation between commands and visual churn.
 */
#define FRAME_CHANNEL_FD 3

static int spawn_child(config_t *cfg) {
    int pipe_in[2], pipe_out[2], pipe_err[2], pipe_frame[2];

    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0 ||
        pipe(pipe_err) < 0 || pipe(pipe_frame) < 0) {
        perror("embr-booster: pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("embr-booster: fork");
        return -1;
    }

    if (pid == 0) {
        /* Child */
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_err[0]);
        close(pipe_frame[0]);

        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        dup2(pipe_err[1], STDERR_FILENO);
        dup2(pipe_frame[1], FRAME_CHANNEL_FD);

        close(pipe_in[0]);
        close(pipe_out[1]);
        close(pipe_err[1]);
        close(pipe_frame[1]);

        /* Tell daemon that fd 3 is the frame channel */
        setenv("EMBR_FRAME_FD", "1", 1);

        execvp(cfg->child_argv[0], cfg->child_argv);
        perror("embr-booster: execvp");
        _exit(127);
    }

    /* Parent */
    close(pipe_in[0]);
    close(pipe_out[1]);
    close(pipe_err[1]);
    close(pipe_frame[1]);

    g_child_pid = pid;
    g_child_stdin = pipe_in[1];
    g_child_stdout = pipe_out[0];
    g_child_stderr = pipe_err[0];
    g_child_frame = pipe_frame[0];

    set_nonblock(g_child_stdin);
    set_nonblock(g_child_stdout);
    set_nonblock(g_child_stderr);
    set_nonblock(g_child_frame);

    LOG(LOG_INFO, "spawned child pid %d: %s", pid, cfg->child_argv[0]);
    return 0;
}

static void shutdown_child(void) {
    if (g_child_pid <= 0) return;

    LOG(LOG_INFO, "shutting down child pid %d", g_child_pid);

    if (g_child_stdin >= 0) {
        close(g_child_stdin);
        g_child_stdin = -1;
    }

    /* Wait for graceful exit */
    uint64_t deadline = now_ms() + SHUTDOWN_TIMEOUT_MS;
    while (now_ms() < deadline) {
        int status;
        pid_t r = waitpid(g_child_pid, &status, WNOHANG);
        if (r > 0) {
            LOG(LOG_INFO, "child exited with status %d",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            stats_logf("child_exit", "\"status\":%d",
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            g_child_pid = -1;
            return;
        }
        usleep(10000);
    }

    /* SIGTERM */
    LOG(LOG_WARN, "child didn't exit, sending SIGTERM");
    kill(g_child_pid, SIGTERM);
    deadline = now_ms() + SHUTDOWN_TIMEOUT_MS;
    while (now_ms() < deadline) {
        int status;
        pid_t r = waitpid(g_child_pid, &status, WNOHANG);
        if (r > 0) {
            g_child_pid = -1;
            return;
        }
        usleep(10000);
    }

    /* SIGKILL */
    LOG(LOG_WARN, "child didn't respond to SIGTERM, sending SIGKILL");
    kill(g_child_pid, SIGKILL);
    waitpid(g_child_pid, NULL, 0);
    g_child_pid = -1;
}

/* ── CLI parsing ────────────────────────────────────────────────── */

static void usage(void) {
    fprintf(stderr,
        "Usage: embr-booster [OPTIONS] -- COMMAND [ARGS...]\n"
        "\n"
        "Options:\n"
        "  --log-level {error,warn,info,debug,trace}  (default: warn)\n"
        "  --queue-capacity N                          (default: 256)\n"
        "  --queue-capacity-p2 N                       (default: 32)\n"
        "  --queue-capacity-p3 N                       (default: 8)\n"
        "  --input-priority-window-ms N                (default: 125)\n"
        "  --frame-forward-max-hz N                    (default: 20)\n"
        "  --stats-jsonl PATH                          (optional perf log)\n"
        "  --frame-path PATH                           (inotify frame detection)\n"
    );
}

static int parse_log_level(const char *s) {
    if (strcmp(s, "error") == 0) return LOG_ERROR;
    if (strcmp(s, "warn") == 0)  return LOG_WARN;
    if (strcmp(s, "info") == 0)  return LOG_INFO;
    if (strcmp(s, "debug") == 0) return LOG_DEBUG;
    if (strcmp(s, "trace") == 0) return LOG_TRACE;
    return -1;
}

static int parse_args(int argc, char **argv, config_t *cfg) {
    cfg->queue_capacity = 256;
    cfg->queue_capacity_p2 = 32;
    cfg->queue_capacity_p3 = 8;
    cfg->input_priority_window_ms = 125;
    cfg->frame_forward_max_hz = 20;
    cfg->stats_path = NULL;
    cfg->frame_path = NULL;
    cfg->child_argc = 0;
    cfg->child_argv = NULL;

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            int lvl = parse_log_level(argv[++i]);
            if (lvl < 0) {
                fprintf(stderr, "embr-booster: unknown log level: %s\n", argv[i]);
                return -1;
            }
            g_log_level = (enum log_level)lvl;
        } else if (strcmp(argv[i], "--queue-capacity") == 0 && i + 1 < argc) {
            cfg->queue_capacity = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--queue-capacity-p2") == 0 && i + 1 < argc) {
            cfg->queue_capacity_p2 = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--queue-capacity-p3") == 0 && i + 1 < argc) {
            cfg->queue_capacity_p3 = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--input-priority-window-ms") == 0 && i + 1 < argc) {
            cfg->input_priority_window_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frame-forward-max-hz") == 0 && i + 1 < argc) {
            cfg->frame_forward_max_hz = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stats-jsonl") == 0 && i + 1 < argc) {
            cfg->stats_path = argv[++i];
        } else if (strcmp(argv[i], "--frame-path") == 0 && i + 1 < argc) {
            cfg->frame_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            exit(0);
        } else {
            fprintf(stderr, "embr-booster: unknown option: %s\n", argv[i]);
            usage();
            return -1;
        }
        i++;
    }

    if (i >= argc) {
        fprintf(stderr, "embr-booster: no child command specified after --\n");
        usage();
        return -1;
    }

    cfg->child_argc = argc - i;
    cfg->child_argv = &argv[i];
    return 0;
}

/* ── Callbacks for line extraction ──────────────────────────────── */

static int on_emacs_line(const char *line, size_t len) {
    LOG(LOG_TRACE, "emacs-> %.*s", (int)(len > 200 ? 200 : len), line);
    return enqueue_downstream(line, len);
}

static int on_child_line(const char *line, size_t len) {
    LOG(LOG_TRACE, "python-> %.*s", (int)(len > 200 ? 200 : len), line);
    return enqueue_upstream(line, len);
}

static int on_child_stderr_line(const char *line, size_t len) {
    (void)!write(STDERR_FILENO, line, len);
    return 0;
}

/*
 * Out-of-band frame channel: lines arriving here are always frame
 * notifications.  They go directly to P3 upstream, bypassing the
 * command classifier.  This provides true transport-level separation
 * between interactive control and visual churn.
 */
static int on_frame_channel_line(const char *line, size_t len) {
    LOG(LOG_TRACE, "frame-ch-> %.*s", (int)(len > 200 ? 200 : len), line);

    ring_t *r = &g_upstream[PRIO_P3];
    char *copy = malloc(len);
    if (!copy) return 0;
    memcpy(copy, line, len);

    stats_logf("rx_python", "\"class\":%d,\"len\":%zu,\"channel\":\"frame\"",
               CLASS_P3, len);

    if (!ring_empty(r)) {
        ring_replace_tail(r, copy, len);
        g_recent_coalesces++;
        stats_logf("coalesce_frame", "\"reason\":\"replace_latest\"");
    } else {
        if (ring_push(r, copy, len) != 0) {
            free(copy);
            stats_logf("drop_frame", "\"reason\":\"queue_full\"");
        }
    }

    update_epollout_emacs();
    return 0;
}

/* ── E3: inotify frame detection ────────────────────────────────── */

/*
 * When --frame-path is set, the booster watches the frame file's
 * directory for IN_MOVED_TO events (atomic rename).  On detection,
 * it synthesizes a minimal frame notification upstream, bypassing
 * both the frame pipe (fd 3) and the command pipe entirely.
 *
 * This eliminates frame notification traffic from all pipes — the
 * daemon still writes the JPEG to tmpfs, but the booster detects
 * it via inotify without any IPC.
 */
static void handle_inotify_events(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    ssize_t n = read(g_inotify_fd, buf, sizeof(buf));
    if (n <= 0) return;

    const char *ptr = buf;
    while (ptr < buf + n) {
        const struct inotify_event *ev =
            (const struct inotify_event *)ptr;

        if ((ev->mask & IN_MOVED_TO) && ev->len > 0 &&
            strcmp(ev->name, g_frame_basename) == 0) {

            /* Synthesize a minimal frame notification */
            const char *frame_json =
                "{\"frame\":true,\"inotify\":true}\n";
            size_t flen = strlen(frame_json);
            char *copy = malloc(flen);
            if (copy) {
                memcpy(copy, frame_json, flen);
                ring_t *r = &g_upstream[PRIO_P3];
                if (!ring_empty(r)) {
                    ring_replace_tail(r, copy, flen);
                    g_recent_coalesces++;
                } else {
                    if (ring_push(r, copy, flen) != 0)
                        free(copy);
                }
                update_epollout_emacs();
                stats_logf("rx_inotify", "\"class\":%d", CLASS_P3);
                LOG(LOG_TRACE, "inotify: frame file updated");
            }
        }
        ptr += sizeof(struct inotify_event) + ev->len;
    }
}

static int setup_inotify(const char *frame_path) {
    g_inotify_fd = inotify_init1(IN_NONBLOCK);
    if (g_inotify_fd < 0) {
        LOG(LOG_WARN, "inotify_init1 failed: %s", strerror(errno));
        return -1;
    }

    /* Watch the directory for IN_MOVED_TO (atomic rename) */
    char *path_copy = strdup(frame_path);
    char *dir_str = strdup(dirname(path_copy));
    free(path_copy);

    if (inotify_add_watch(g_inotify_fd, dir_str, IN_MOVED_TO) < 0) {
        LOG(LOG_WARN, "inotify_add_watch(%s) failed: %s",
            dir_str, strerror(errno));
        free(dir_str);
        close(g_inotify_fd);
        g_inotify_fd = -1;
        return -1;
    }

    /* Extract basename for matching */
    char *path_copy2 = strdup(frame_path);
    g_frame_basename = strdup(basename(path_copy2));
    free(path_copy2);
    g_frame_path = frame_path;

    LOG(LOG_INFO, "inotify watching %s for %s", dir_str, g_frame_basename);
    free(dir_str);
    return 0;
}

/* ── Main event loop ────────────────────────────────────────────── */

static int run(config_t *cfg) {
    int exit_status = 0;
    int emacs_eof = 0;
    int child_eof = 0;

    /* Configure frame rate limiting */
    if (cfg->frame_forward_max_hz > 0)
        g_frame_interval_ms = (uint64_t)(1000 / cfg->frame_forward_max_hz);
    else
        g_frame_interval_ms = 0;

    g_input_priority_window_ms_cfg = (uint64_t)cfg->input_priority_window_ms;

    /* Open stats file if requested */
    if (cfg->stats_path) {
        g_stats_file = fopen(cfg->stats_path, "w");
        if (!g_stats_file)
            LOG(LOG_WARN, "failed to open stats file %s: %s",
                cfg->stats_path, strerror(errno));
    }

    /* Initialize queues.
     * CMD (index 0): uses queue_capacity for both directions.
     * P2 (index 1): uses queue_capacity_p2.
     * P3 (index 2): uses queue_capacity_p3. */
    int caps[PRIO_COUNT] = {
        cfg->queue_capacity,
        cfg->queue_capacity_p2,
        cfg->queue_capacity_p3,
    };
    for (int i = 0; i < PRIO_COUNT; i++) {
        if (ring_init(&g_downstream[i], caps[i]) != 0 ||
            ring_init(&g_upstream[i], caps[i]) != 0) {
            LOG(LOG_ERROR, "failed to allocate queues");
            return 1;
        }
    }

    /* Set up signals */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    sa.sa_handler = handle_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Create epoll */
    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0) {
        perror("embr-booster: epoll_create1");
        return 1;
    }

    /* Spawn child */
    if (spawn_child(cfg) != 0)
        return 1;

    /* Set our stdin/stdout nonblocking */
    set_nonblock(STDIN_FILENO);
    set_nonblock(STDOUT_FILENO);

    /* Register readable fds */
    epoll_add(STDIN_FILENO, EPOLLIN);
    g_emacs_stdin_registered = 1;
    epoll_add(g_child_stdout, EPOLLIN);
    epoll_add(g_child_stderr, EPOLLIN);
    epoll_add(g_child_frame, EPOLLIN);

    /* E3: set up inotify frame detection if --frame-path is given.
     * Only if setup succeeds do we tell the daemon to suppress its
     * own frame emission.  If it fails, fd 3 / stdout remain active. */
    if (cfg->frame_path) {
        if (setup_inotify(cfg->frame_path) == 0) {
            epoll_add(g_inotify_fd, EPOLLIN);
            /* Tell daemon to stop emitting frame notifications —
             * inotify will handle frame detection from here. */
            const char *cmd = "{\"cmd\":\"_suppress_frames\"}\n";
            size_t clen = strlen(cmd);
            char *copy = malloc(clen);
            if (copy) {
                memcpy(copy, cmd, clen);
                ring_push(&g_downstream[PRIO_CMD], copy, clen);
                update_epollout_child();
            }
            LOG(LOG_INFO, "inotify active, sent _suppress_frames to daemon");
        }
    }

    LOG(LOG_INFO, "event loop starting (frame channel fd %d, inotify fd %d)",
        g_child_frame, g_inotify_fd);

    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (!g_got_sigterm) {
        /* Check for child exit */
        if (g_got_sigchld) {
            g_got_sigchld = 0;
            int status;
            pid_t r = waitpid(g_child_pid, &status, WNOHANG);
            if (r > 0) {
                if (WIFEXITED(status)) {
                    exit_status = WEXITSTATUS(status);
                    LOG(LOG_INFO, "child exited with status %d", exit_status);
                } else if (WIFSIGNALED(status)) {
                    exit_status = 128 + WTERMSIG(status);
                    LOG(LOG_INFO, "child killed by signal %d", WTERMSIG(status));
                }
                stats_logf("child_exit", "\"status\":%d", exit_status);
                g_child_pid = -1;
                child_eof = 1;
            }
        }

        /* Exit conditions */
        if (emacs_eof && child_eof && !any_upstream_pending())
            break;
        if (child_eof && !any_upstream_pending())
            break;

        /* Periodic telemetry */
        emit_queue_depth();

        /* M6: periodic pressure hints */
        maybe_send_hint();

        /* Compute epoll timeout */
        int timeout_ms = -1;
        if (!ring_empty(&g_upstream[PRIO_P3])) {
            uint64_t now = now_ms();
            uint64_t next_allowed = g_last_frame_forward_ms + g_frame_interval_ms;
            if (g_input_priority_until > next_allowed)
                next_allowed = g_input_priority_until;
            if (next_allowed > now)
                timeout_ms = (int)(next_allowed - now);
            else
                timeout_ms = 0;
        }

        int nfds = epoll_wait(g_epoll_fd, events, MAX_EPOLL_EVENTS, timeout_ms);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            perror("embr-booster: epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            /* ── Read from Emacs stdin ─────────────────────── */
            if (fd == STDIN_FILENO && (ev & EPOLLIN)) {
                ssize_t n = read(fd, g_emacs_lb.buf + g_emacs_lb.len,
                                 LINE_BUF_SIZE - g_emacs_lb.len);
                if (n > 0) {
                    g_emacs_lb.len += (size_t)n;
                    extract_lines(&g_emacs_lb, on_emacs_line);
                } else if (n == 0) {
                    LOG(LOG_INFO, "Emacs stdin EOF");
                    emacs_eof = 1;
                    if (g_emacs_stdin_registered) {
                        epoll_del(STDIN_FILENO);
                        g_emacs_stdin_registered = 0;
                    }
                    if (!any_downstream_pending()) {
                        shutdown_child();
                        child_eof = 1;
                    }
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG(LOG_ERROR, "read stdin: %s", strerror(errno));
                    emacs_eof = 1;
                    if (g_emacs_stdin_registered) {
                        epoll_del(STDIN_FILENO);
                        g_emacs_stdin_registered = 0;
                    }
                }
            }

            /* ── Read from child stdout ────────────────────── */
            if (fd == g_child_stdout && (ev & EPOLLIN)) {
                ssize_t n = read(fd, g_child_lb.buf + g_child_lb.len,
                                 LINE_BUF_SIZE - g_child_lb.len);
                if (n > 0) {
                    g_child_lb.len += (size_t)n;
                    extract_lines(&g_child_lb, on_child_line);
                } else if (n == 0) {
                    LOG(LOG_INFO, "child stdout EOF");
                    child_eof = 1;
                    epoll_del(g_child_stdout);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG(LOG_ERROR, "read child stdout: %s", strerror(errno));
                }
            }

            /* ── Read from child stderr ────────────────────── */
            if (fd == g_child_stderr && (ev & EPOLLIN)) {
                ssize_t n = read(fd, g_child_err_lb.buf + g_child_err_lb.len,
                                 LINE_BUF_SIZE - g_child_err_lb.len);
                if (n > 0) {
                    g_child_err_lb.len += (size_t)n;
                    extract_lines(&g_child_err_lb, on_child_stderr_line);
                } else if (n == 0) {
                    epoll_del(g_child_stderr);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG(LOG_ERROR, "read child stderr: %s", strerror(errno));
                }
            }

            /* ── Read from frame channel (out-of-band) ───── */
            if (fd == g_child_frame && (ev & EPOLLIN)) {
                ssize_t n = read(fd, g_child_frame_lb.buf + g_child_frame_lb.len,
                                 LINE_BUF_SIZE - g_child_frame_lb.len);
                if (n > 0) {
                    g_child_frame_lb.len += (size_t)n;
                    extract_lines(&g_child_frame_lb, on_frame_channel_line);
                } else if (n == 0) {
                    epoll_del(g_child_frame);
                    g_child_frame = -1;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG(LOG_ERROR, "read frame channel: %s", strerror(errno));
                }
            }

            /* ── E3: inotify frame detection ────────────── */
            if (fd == g_inotify_fd && (ev & EPOLLIN)) {
                handle_inotify_events();
            }

            /* ── Write to child stdin (EPOLLOUT) ───────────── */
            if (fd == g_child_stdin && (ev & EPOLLOUT)) {
                drain_to_fd(g_child_stdin, g_downstream, &g_child_wb,
                            DIR_DOWNSTREAM);
                update_epollout_child();

                /* After draining frees CMD queue space, re-extract any
                 * lines that were left in the read buffer due to
                 * backpressure. */
                if (g_emacs_lb.len > 0 && !ring_full(&g_downstream[PRIO_CMD]))
                    extract_lines(&g_emacs_lb, on_emacs_line);

                if (emacs_eof && !any_downstream_pending()) {
                    shutdown_child();
                    child_eof = 1;
                }
            }

            /* ── Write to Emacs stdout (EPOLLOUT) ──────────── */
            if (fd == STDOUT_FILENO && (ev & EPOLLOUT)) {
                drain_to_fd(STDOUT_FILENO, g_upstream, &g_emacs_wb,
                            DIR_UPSTREAM);
                update_epollout_emacs();

                /* Re-extract child lines held back by upstream CMD full. */
                if (g_child_lb.len > 0 && !ring_full(&g_upstream[PRIO_CMD]))
                    extract_lines(&g_child_lb, on_child_line);
            }

            /* ── Handle errors on any fd ───────────────────── */
            if (ev & (EPOLLERR | EPOLLHUP)) {
                if (fd == STDIN_FILENO) {
                    emacs_eof = 1;
                    if (g_emacs_stdin_registered) {
                        epoll_del(STDIN_FILENO);
                        g_emacs_stdin_registered = 0;
                    }
                } else if (fd == g_child_stdout) {
                    child_eof = 1;
                } else if (fd == g_child_stdin) {
                    LOG(LOG_INFO, "child stdin closed (EPOLLHUP)");
                    if (g_child_stdin_epollout) {
                        epoll_del(g_child_stdin);
                        g_child_stdin_epollout = 0;
                    }
                    close(g_child_stdin);
                    g_child_stdin = -1;
                }
            }
        }

        /* Timeout wakeup: drain rate-limited frames */
        if (nfds == 0 && any_upstream_pending()) {
            drain_to_fd(STDOUT_FILENO, g_upstream, &g_emacs_wb,
                        DIR_UPSTREAM);
            update_epollout_emacs();
        }
    }

    /* Cleanup */
    if (g_got_sigterm && g_child_pid > 0)
        shutdown_child();

    /* Drain remaining upstream to Emacs */
    for (int p = 0; p < PRIO_COUNT; p++) {
        while (!ring_empty(&g_upstream[p])) {
            msg_t *msg = ring_peek(&g_upstream[p]);
            if (msg && msg->data) {
                ssize_t n = write(STDOUT_FILENO, msg->data, msg->len);
                (void)n;
                free(msg->data);
                msg->data = NULL;
            }
            ring_pop(&g_upstream[p]);
        }
    }

    free(g_child_wb.data);
    free(g_emacs_wb.data);
    for (int i = 0; i < PRIO_COUNT; i++) {
        ring_free(&g_downstream[i]);
        ring_free(&g_upstream[i]);
    }

    if (g_epoll_fd >= 0) close(g_epoll_fd);
    if (g_child_stdin >= 0) close(g_child_stdin);
    if (g_child_stdout >= 0) close(g_child_stdout);
    if (g_child_stderr >= 0) close(g_child_stderr);
    if (g_child_frame >= 0) close(g_child_frame);
    if (g_inotify_fd >= 0) close(g_inotify_fd);
    if (g_stats_file) fclose(g_stats_file);

    return exit_status;
}

/* ── Entry point ────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    config_t cfg;

    if (parse_args(argc, argv, &cfg) != 0)
        return 1;

    LOG(LOG_INFO, "embr-booster starting (queues: %d/%d/%d, ipw: %dms, max-hz: %d)",
        cfg.queue_capacity, cfg.queue_capacity_p2, cfg.queue_capacity_p3,
        cfg.input_priority_window_ms, cfg.frame_forward_max_hz);

    return run(&cfg);
}
