/*
 * agent_736.c -- RemoteOps Agent (the "managed machine" / TCP server)
 * IE3090 Network Programming
 *
 * Registration number : IT24103736
 * TCP listening port  : 7000 + 2410 = 9410
 * Session ID tag      : SID:6373   (last four digits 3736, reversed)
 * Auth token          : OPS-3736
 * Log file            : remoteops_IT24103736.log
 * Storage path        : ./agentfiles/IT24103736/<filename>
 *
 * Concurrency model   : one POSIX thread per Controller connection
 *                       (pthread_create + detached threads).
 *
 * Build : make -f Makefile_736
 * Run   : ./agent_736
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------- Personalised values (derived from IT24103736) ---------- */
#define REG_NO        "IT24103736"
#define AGENT_PORT    9410
#define SID_TAG       "SID:6373"
#define AUTH_TOKEN    "OPS-3736"
#define LOG_FILE      "remoteops_IT24103736.log"
#define STORAGE_ROOT  "./agentfiles"
#define STORAGE_DIR   "./agentfiles/IT24103736"

/* ---------- Tunables ---------- */
#define MAX_LINE            4096              /* longest accepted command line  */
#define RBUF_SIZE           8192              /* per-connection receive buffer  */
#define MAX_FILE_SIZE       (10ULL * 1024 * 1024)   /* PUT limit -> ERR 004     */
#define DRAIN_LIMIT         (256ULL * 1024 * 1024)  /* max bytes we will discard */
#define MAX_CLIENTS         100
#define MAX_AUTH_FAILURES   3
#define MONITOR_INTERVAL_MS 2000              /* UDP datagram every 2 seconds   */
#define MAX_PROC_OUTPUT     32000

/* handler return codes */
enum { H_CONTINUE = 0, H_QUIT = 1, H_DROP = 2, H_LOST = -1 };

typedef struct {
    int fd;                         /* TCP socket of this controller           */
    struct sockaddr_in addr;        /* controller address (used for UDP too)   */
    char peer[64];                  /* "ip:port" text for the log              */
    char rbuf[RBUF_SIZE];           /* receive buffer for framing              */
    size_t rstart, rend;            /* unread region of rbuf                   */
    int authed;                     /* has this connection sent a valid AUTH?  */
    int auth_failures;
    pthread_t mon_thr;              /* UDP monitoring thread                   */
    int mon_active;
    int mon_port;
    atomic_int mon_stop;
} session_t;

static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;
static atomic_int client_count = 0;

/* ================================================================== */
/*  Logging                                                            */
/* ================================================================== */
static void log_event(const char *peer, const char *fmt, ...)
{
    char ts[32], msg[1024];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&log_mtx);          /* one writer at a time */
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "[%s] [%s] %s\n", ts, peer, msg);
        fclose(f);
    }
    printf("[%s] [%s] %s\n", ts, peer, msg);
    fflush(stdout);
    pthread_mutex_unlock(&log_mtx);
}

/* ================================================================== */
/*  Low level I/O helpers                                              */
/* ================================================================== */
static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Send one response line:  <body> SID:6373\n   (every OK/ERR line gets the tag) */
static int reply(session_t *s, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return -1; }

    const char *tail = " " SID_TAG "\n";
    size_t total = (size_t)n + strlen(tail) + 1;
    char *buf = malloc(total);
    if (!buf) { va_end(ap2); return -1; }
    vsnprintf(buf, total, fmt, ap2);
    va_end(ap2);
    strcat(buf, tail);

    size_t len = strlen(buf);
    buf[len - 1] = '\0';                    /* hide the '\n' while logging */
    log_event(s->peer, "TX: %.120s", buf);
    buf[len - 1] = '\n';

    int rc = send_all(s->fd, buf, len);
    free(buf);
    return rc;
}

/*
 * Framing: read exactly one '\n'-terminated line. Bytes that arrive after the
 * newline (the next command, or PUT body bytes) stay in s->rbuf for later.
 * A line may arrive in many small recv() pieces, or several lines may arrive
 * in one recv() -- both are handled here.
 * Returns length >= 0, -1 on disconnect/error, -2 if the line was too long.
 */
static int read_line(session_t *s, char *out, size_t cap)
{
    size_t olen = 0;
    int toolong = 0;
    for (;;) {
        while (s->rstart < s->rend) {
            char c = s->rbuf[s->rstart++];
            if (c == '\n') {
                out[olen] = '\0';
                if (olen > 0 && out[olen - 1] == '\r') out[--olen] = '\0';
                return toolong ? -2 : (int)olen;
            }
            if (olen + 1 < cap) out[olen++] = c;
            else toolong = 1;
        }
        ssize_t n = recv(s->fd, s->rbuf, sizeof s->rbuf, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;                 /* peer closed */
        s->rstart = 0;
        s->rend = (size_t)n;
    }
}

/*
 * Read exactly n raw body bytes (PUT). First use whatever is already buffered,
 * then recv() the rest. outfd < 0 means "discard".
 * Returns 0 ok, -1 network failure, -2 local write failure (data still drained).
 */
static int recv_body(session_t *s, int outfd, uint64_t n)
{
    char tmp[8192];
    int werr = 0;
    while (n > 0) {
        const char *src;
        size_t chunk;
        size_t avail = s->rend - s->rstart;
        if (avail > 0) {
            chunk = avail < n ? avail : (size_t)n;
            src = s->rbuf + s->rstart;
            s->rstart += chunk;
        } else {
            size_t want = n < sizeof tmp ? (size_t)n : sizeof tmp;
            ssize_t r = recv(s->fd, tmp, want, 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (r == 0) return -1;
            chunk = (size_t)r;
            src = tmp;
        }
        if (outfd >= 0 && !werr && write_all(outfd, src, chunk) < 0) werr = 1;
        n -= chunk;
    }
    return werr ? -2 : 0;
}

/* ================================================================== */
/*  System information                                                 */
/* ================================================================== */
static void get_sysinfo(double *cpu, long *mem_mb, long *uptime)
{
    *cpu = 0.0; *mem_mb = 0; *uptime = 0;

    FILE *f = fopen("/proc/loadavg", "r");          /* 1-minute load average */
    if (f) {
        if (fscanf(f, "%lf", cpu) != 1) *cpu = 0.0;
        fclose(f);
    }

    f = fopen("/proc/meminfo", "r");                /* used = total - available */
    if (f) {
        char line[256], key[64];
        long val, total = -1, avail = -1;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "%63[^:]: %ld", key, &val) == 2) {
                if (!strcmp(key, "MemTotal")) total = val;
                else if (!strcmp(key, "MemAvailable")) avail = val;
            }
        }
        fclose(f);
        if (total >= 0 && avail >= 0) *mem_mb = (total - avail) / 1024;
    }

    f = fopen("/proc/uptime", "r");
    if (f) {
        double u;
        if (fscanf(f, "%lf", &u) == 1) *uptime = (long)u;
        fclose(f);
    }
}

static int cmd_sysinfo(session_t *s)
{
    double cpu; long mem, up;
    get_sysinfo(&cpu, &mem, &up);
    return reply(s, "OK SYSINFO %.2f %ld %ld", cpu, mem, up);
}

/* ================================================================== */
/*  LISTPROC: snapshot of running processes from /proc                 */
/* ================================================================== */
static int cmd_listproc(session_t *s)
{
    char *out = malloc(MAX_PROC_OUTPUT + 64);
    if (!out) return reply(s, "ERR 009 INTERNAL_ERROR");
    size_t len = 0;
    out[0] = '\0';

    DIR *d = opendir("/proc");
    if (!d) { free(out); return reply(s, "ERR 009 INTERNAL_ERROR"); }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[300], name[128] = "?";
        snprintf(path, sizeof path, "/proc/%s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = '\0';
            fclose(f);
        }
        for (char *p = name; *p; p++)               /* keep it one clean token */
            if (*p == ' ' || *p == ',' || *p == '\t') *p = '_';

        char item[192];
        int il = snprintf(item, sizeof item, "%s%s:%s", len ? "," : "", de->d_name, name);
        if (len + (size_t)il >= MAX_PROC_OUTPUT) {
            strcat(out, ",...");
            break;
        }
        memcpy(out + len, item, (size_t)il + 1);
        len += (size_t)il;
    }
    closedir(d);

    int rc = reply(s, "OK PROCS %s", out);
    free(out);
    return rc;
}

/* ================================================================== */
/*  EXEC: fixed whitelist only. The user's text is never given to the  */
/*  shell -- it is only compared against the table below.              */
/* ================================================================== */
static const struct { const char *name; const char *shell; } WHITELIST[] = {
    { "DATE",     "date" },
    { "UPTIME",   "uptime" },
    { "DISKFREE", "df -h /" },
    { "HOSTNAME", "hostname" },
    { "WHOAMI",   "whoami" },
};

static int cmd_exec(session_t *s, const char *arg)
{
    if (*arg == '\0') return reply(s, "ERR 007 BAD_ARGUMENTS");

    const char *shell = NULL;
    for (size_t i = 0; i < sizeof WHITELIST / sizeof WHITELIST[0]; i++)
        if (strcmp(arg, WHITELIST[i].name) == 0) { shell = WHITELIST[i].shell; break; }
    if (!shell) return reply(s, "ERR 002 COMMAND_NOT_ALLOWED");

    char out[1024];
    size_t len = 0;
    FILE *p = popen(shell, "r");
    if (p) {
        len = fread(out, 1, sizeof out - 1, p);
        pclose(p);
    }
    out[len] = '\0';
    for (size_t i = 0; i < len; i++)                /* response must be one line */
        if (out[i] == '\n' || out[i] == '\r' || out[i] == '\t') out[i] = ' ';
    while (len > 0 && out[len - 1] == ' ') out[--len] = '\0';
    if (len == 0) strcpy(out, "(no output)");

    return reply(s, "OK EXEC_RESULT %s", out);
}

/* ================================================================== */
/*  File transfer                                                      */
/* ================================================================== */
static int valid_filename(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > 200 || n[0] == '.') return 0;   /* no hidden / ".." */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return 0; /* no '/' */
    }
    return 1;
}

/* "<name> <size>" -> name, size. Rest of the line must be digits only. */
static int parse_put_args(char *rest, char **name, uint64_t *size)
{
    char *sp = strchr(rest, ' ');
    if (!sp) return -1;
    *sp++ = '\0';
    while (*sp == ' ') sp++;
    if (*rest == '\0' || *sp == '\0' || strlen(sp) > 18) return -1;
    for (char *p = sp; *p; p++)
        if (!isdigit((unsigned char)*p)) return -1;
    *name = rest;
    *size = strtoull(sp, NULL, 10);
    return 0;
}

/* Discard the body of a refused upload so the TCP stream stays in sync. */
static int refuse_put(session_t *s, uint64_t size, const char *errline)
{
    if (size > DRAIN_LIMIT) {            /* absurd size: reply, then drop the link */
        reply(s, "%s", errline);
        return H_DROP;
    }
    if (recv_body(s, -1, size) == -1) return H_LOST;
    return reply(s, "%s", errline) < 0 ? H_LOST : H_CONTINUE;
}

static int cmd_put(session_t *s, char *rest)
{
    char *name; uint64_t size;
    if (parse_put_args(rest, &name, &size) < 0) {
        return reply(s, "ERR 007 BAD_ARGUMENTS") < 0 ? H_LOST : H_CONTINUE;
    }
    if (size > MAX_FILE_SIZE) {
        log_event(s->peer, "PUT %s rejected: %llu bytes exceeds limit", name, (unsigned long long)size);
        return refuse_put(s, size, "ERR 004 FILE_TOO_LARGE");
    }
    if (!valid_filename(name))
        return refuse_put(s, size, "ERR 008 INVALID_FILENAME");

    char path[512], tmp[560];
    snprintf(path, sizeof path, "%s/%s", STORAGE_DIR, name);
    snprintf(tmp, sizeof tmp, "%s.part.%lu", path, (unsigned long)pthread_self());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return refuse_put(s, size, "ERR 009 INTERNAL_ERROR");

    int rc = recv_body(s, fd, size);        /* exactly <size> raw bytes */
    close(fd);
    if (rc == -1) {                         /* controller vanished mid-upload */
        unlink(tmp);
        log_event(s->peer, "PUT %s aborted: connection lost during transfer", name);
        return H_LOST;
    }
    if (rc == -2 || rename(tmp, path) < 0) {
        unlink(tmp);
        return reply(s, "ERR 009 INTERNAL_ERROR") < 0 ? H_LOST : H_CONTINUE;
    }
    log_event(s->peer, "FILE TRANSFER: PUT %s (%llu bytes) stored at %s",
              name, (unsigned long long)size, path);
    return reply(s, "OK FILE_RECEIVED %s", name) < 0 ? H_LOST : H_CONTINUE;
}

static int cmd_get(session_t *s, const char *name)
{
    if (!valid_filename(name))
        return reply(s, "ERR 008 INVALID_FILENAME") < 0 ? H_LOST : H_CONTINUE;

    char path[512];
    snprintf(path, sizeof path, "%s/%s", STORAGE_DIR, name);
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        return reply(s, "ERR 005 FILE_NOT_FOUND") < 0 ? H_LOST : H_CONTINUE;
    }

    /* header line, then exactly st_size raw bytes */
    if (reply(s, "OK FILE_SEND %s %lld", name, (long long)st.st_size) < 0) {
        close(fd);
        return H_LOST;
    }
    char buf[8192];
    long long remaining = st.st_size;
    while (remaining > 0) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;                  /* file shrank: cannot honour size */
        if (send_all(s->fd, buf, (size_t)r) < 0) { close(fd); return H_LOST; }
        remaining -= r;
    }
    close(fd);
    if (remaining > 0) return H_LOST;       /* stream no longer consistent */
    log_event(s->peer, "FILE TRANSFER: GET %s (%lld bytes) sent", name, (long long)st.st_size);
    return H_CONTINUE;
}

/* ================================================================== */
/*  UDP monitoring                                                     */
/* ================================================================== */
static void *monitor_thread(void *arg)
{
    session_t *s = arg;
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    if (us < 0) {
        log_event(s->peer, "MONITOR: cannot create UDP socket: %s", strerror(errno));
        return NULL;
    }
    struct sockaddr_in dst = s->addr;       /* Controller's IP ... */
    dst.sin_port = htons((uint16_t)s->mon_port);   /* ... on the requested port */

    while (!atomic_load(&s->mon_stop)) {
        double cpu; long mem, up;
        char msg[160];
        get_sysinfo(&cpu, &mem, &up);
        int n = snprintf(msg, sizeof msg, "SYSINFO %.2f %ld %ld %s\n", cpu, mem, up, SID_TAG);
        sendto(us, msg, (size_t)n, 0, (struct sockaddr *)&dst, sizeof dst);

        for (int i = 0; i < MONITOR_INTERVAL_MS / 100 && !atomic_load(&s->mon_stop); i++)
            usleep(100 * 1000);
    }
    close(us);
    return NULL;
}

static void stop_monitor(session_t *s)
{
    if (!s->mon_active) return;
    atomic_store(&s->mon_stop, 1);
    pthread_join(s->mon_thr, NULL);
    s->mon_active = 0;
    log_event(s->peer, "MONITOR stopped");
}

static int cmd_monitor(session_t *s, char *rest)
{
    if (strncmp(rest, "START ", 6) == 0) {
        char *p = rest + 6;
        while (*p == ' ') p++;
        char *end;
        long port = strtol(p, &end, 10);
        if (*p == '\0' || *end != '\0' || port < 1 || port > 65535)
            return reply(s, "ERR 007 BAD_ARGUMENTS");
        stop_monitor(s);                    /* restart cleanly if already running */
        s->mon_port = (int)port;
        atomic_store(&s->mon_stop, 0);
        if (pthread_create(&s->mon_thr, NULL, monitor_thread, s) != 0)
            return reply(s, "ERR 009 INTERNAL_ERROR");
        s->mon_active = 1;
        log_event(s->peer, "MONITOR started: UDP datagrams to port %ld every %d ms", port, MONITOR_INTERVAL_MS);
        return reply(s, "OK MONITOR_STARTED");
    }
    if (strcmp(rest, "STOP") == 0) {
        stop_monitor(s);                    /* idempotent */
        return reply(s, "OK MONITOR_STOPPED");
    }
    return reply(s, "ERR 007 BAD_ARGUMENTS");
}

/* ================================================================== */
/*  Command dispatcher                                                 */
/* ================================================================== */
static int handle_command(session_t *s, char *line)
{
    char *cmd = line, *rest = line;
    while (*rest && *rest != ' ') rest++;
    if (*rest) { *rest++ = '\0'; while (*rest == ' ') rest++; }
    size_t rl = strlen(rest);
    while (rl > 0 && rest[rl - 1] == ' ') rest[--rl] = '\0';

    /* ---- Requirement 2: nothing but AUTH is accepted before authentication ---- */
    if (!s->authed) {
        if (strcmp(cmd, "AUTH") != 0) {
            if (strcmp(cmd, "PUT") == 0) {          /* keep the stream in sync */
                char *n; uint64_t sz;
                if (parse_put_args(rest, &n, &sz) == 0 && sz <= DRAIN_LIMIT)
                    return refuse_put(s, sz, "ERR 006 NOT_AUTHENTICATED");
            }
            return reply(s, "ERR 006 NOT_AUTHENTICATED") < 0 ? H_LOST : H_CONTINUE;
        }
        if (strcmp(rest, AUTH_TOKEN) == 0) {
            s->authed = 1;
            log_event(s->peer, "AUTH success");
            return reply(s, "OK AUTHENTICATED") < 0 ? H_LOST : H_CONTINUE;
        }
        s->auth_failures++;
        log_event(s->peer, "AUTH failed (attempt %d of %d)", s->auth_failures, MAX_AUTH_FAILURES);
        reply(s, "ERR 001 AUTH_FAILED");
        return s->auth_failures >= MAX_AUTH_FAILURES ? H_DROP : H_CONTINUE;
    }

    int rc = 0;
    if      (!strcmp(cmd, "AUTH"))     rc = reply(s, "OK AUTHENTICATED");   /* already in */
    else if (!strcmp(cmd, "SYSINFO"))  rc = cmd_sysinfo(s);
    else if (!strcmp(cmd, "LISTPROC")) rc = cmd_listproc(s);
    else if (!strcmp(cmd, "EXEC"))     rc = cmd_exec(s, rest);
    else if (!strcmp(cmd, "PUT"))      return cmd_put(s, rest);
    else if (!strcmp(cmd, "GET"))      return cmd_get(s, rest);
    else if (!strcmp(cmd, "MONITOR"))  rc = cmd_monitor(s, rest);
    else if (!strcmp(cmd, "QUIT")) {
        stop_monitor(s);                    /* QUIT also ends the UDP stream */
        reply(s, "OK BYE");
        return H_QUIT;
    }
    else rc = reply(s, "ERR 003 UNKNOWN_COMMAND");
    return rc < 0 ? H_LOST : H_CONTINUE;
}

/* ================================================================== */
/*  One thread per Controller                                          */
/* ================================================================== */
static void *client_thread(void *arg)
{
    session_t *s = arg;
    char line[MAX_LINE];
    const char *how = "connection lost (ungraceful disconnect)";

    log_event(s->peer, "Controller connected");

    for (;;) {
        int n = read_line(s, line, sizeof line);
        if (n == -1) break;                         /* EOF or error */
        if (n == -2) {
            if (reply(s, "ERR 011 LINE_TOO_LONG") < 0) break;
            continue;
        }
        if (n == 0) continue;                       /* ignore blank lines */

        if (strncmp(line, "AUTH ", 5) == 0) log_event(s->peer, "RX: AUTH ****");
        else log_event(s->peer, "RX: %.200s", line);

        int rc = handle_command(s, line);
        if (rc == H_QUIT) { how = "QUIT (graceful)"; break; }
        if (rc == H_DROP) { how = "closed by agent"; break; }
        if (rc == H_LOST) break;
    }

    stop_monitor(s);
    close(s->fd);
    log_event(s->peer, "Controller disconnected: %s", how);
    atomic_fetch_sub(&client_count, 1);
    free(s);
    return NULL;
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */
int main(void)
{
    signal(SIGPIPE, SIG_IGN);       /* a dead client must not kill the Agent */

    mkdir(STORAGE_ROOT, 0755);
    if (mkdir(STORAGE_DIR, 0755) < 0 && errno != EEXIST) {
        perror("mkdir " STORAGE_DIR);
        return 1;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(AGENT_PORT);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(srv, 128) < 0) { perror("listen"); return 1; }

    printf("RemoteOps Agent started (registration %s).\n", REG_NO);
    log_event("agent", "Agent started, listening on TCP port %d, storage %s", AGENT_PORT, STORAGE_DIR);

    for (;;) {
        session_t *s = calloc(1, sizeof *s);
        if (!s) { sleep(1); continue; }
        socklen_t len = sizeof s->addr;
        s->fd = accept(srv, (struct sockaddr *)&s->addr, &len);
        if (s->fd < 0) {
            free(s);
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &s->addr.sin_addr, ip, sizeof ip);
        snprintf(s->peer, sizeof s->peer, "%s:%d", ip, ntohs(s->addr.sin_port));
        atomic_init(&s->mon_stop, 0);

        if (atomic_fetch_add(&client_count, 1) >= MAX_CLIENTS) {
            atomic_fetch_sub(&client_count, 1);
            reply(s, "ERR 012 SERVER_BUSY");
            close(s->fd);
            free(s);
            continue;
        }

        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, s) != 0) {
            atomic_fetch_sub(&client_count, 1);
            close(s->fd);
            free(s);
            continue;
        }
        pthread_detach(t);          /* nobody joins client threads */
    }
}
