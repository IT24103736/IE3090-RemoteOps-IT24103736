/*
 * controller_736.c -- RemoteOps Controller (the administrator's client)
 * IE3090 Network Programming
 *
 * Registration number : IT24103736
 * Agent port          : 9410      SID tag : SID:6373      Token : OPS-3736
 *
 * Build : make -f Makefile_736
 * Run   : ./controller_736 <agent_ip> [port]      (default port 9410)
 *
 * Commands typed at the "remoteops>" prompt:
 *   help                         show this list
 *   auth [token]                 AUTH (default token OPS-3736)
 *   sysinfo                      SYSINFO
 *   listproc                     LISTPROC
 *   exec <name>                  EXEC  (DATE UPTIME DISKFREE HOSTNAME WHOAMI)
 *   put <localfile> [remotename] PUT   (prints throughput)
 *   get <remotename> [localfile] GET   (saved in ./downloads/ by default)
 *   monitor start <udp_port>     MONITOR START + live UDP listener
 *   monitor stop                 MONITOR STOP
 *   raw <text>                   send a line exactly as typed (for error tests)
 *   quit                         QUIT
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
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

#define DEFAULT_PORT  9410
#define DEFAULT_TOKEN "OPS-3736"
#define MAX_LINE      65536

typedef struct {
    int fd;
    char rbuf[8192];
    size_t rstart, rend;
} conn_t;

/* ---------- UDP monitor listener state ---------- */
static int udp_fd = -1;
static pthread_t udp_thr;
static atomic_int udp_run = 0;
static int udp_active = 0;

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

/* Read one '\n' terminated line; extra bytes stay buffered (see agent). */
static int read_line(conn_t *c, char *out, size_t cap)
{
    size_t olen = 0;
    for (;;) {
        while (c->rstart < c->rend) {
            char ch = c->rbuf[c->rstart++];
            if (ch == '\n') {
                out[olen] = '\0';
                if (olen > 0 && out[olen - 1] == '\r') out[--olen] = '\0';
                return (int)olen;
            }
            if (olen + 1 < cap) out[olen++] = ch;
        }
        ssize_t n = recv(c->fd, c->rbuf, sizeof c->rbuf, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        c->rstart = 0;
        c->rend = (size_t)n;
    }
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void print_throughput(const char *what, uint64_t bytes, double secs)
{
    if (secs < 1e-6) secs = 1e-6;
    printf("  [%s] %llu bytes in %.3f s = %.1f KB/s\n", what,
           (unsigned long long)bytes, secs, (double)bytes / 1024.0 / secs);
}

/* Send a one-line command and print the one-line response. */
static int simple_cmd(conn_t *c, const char *cmd)
{
    char line[MAX_LINE], out[MAX_LINE + 8];
    int n = snprintf(out, sizeof out, "%s\n", cmd);
    if (send_all(c->fd, out, (size_t)n) < 0) { printf("send failed\n"); return -1; }
    if (read_line(c, line, sizeof line) < 0) { printf("connection closed by Agent\n"); return -1; }
    printf("%s\n", line);
    return 0;
}

/* ================================================================== */
/*  PUT / GET                                                          */
/* ================================================================== */
static int cmd_put(conn_t *c, const char *local, const char *remote)
{
    struct stat st;
    if (stat(local, &st) < 0 || !S_ISREG(st.st_mode)) {
        printf("cannot read local file '%s'\n", local);
        return 0;
    }
    FILE *f = fopen(local, "rb");
    if (!f) { perror("fopen"); return 0; }

    char hdr[600];
    int hl = snprintf(hdr, sizeof hdr, "PUT %s %lld\n", remote, (long long)st.st_size);
    double t0 = now_sec();
    if (send_all(c->fd, hdr, (size_t)hl) < 0) { fclose(f); return -1; }

    char buf[8192];
    size_t r;
    uint64_t sent = 0;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) {        /* exactly st_size raw bytes */
        if (send_all(c->fd, buf, r) < 0) { fclose(f); printf("send failed\n"); return -1; }
        sent += r;
    }
    fclose(f);

    char line[MAX_LINE];
    if (read_line(c, line, sizeof line) < 0) { printf("connection closed by Agent\n"); return -1; }
    double t1 = now_sec();
    printf("%s\n", line);
    if (strncmp(line, "OK FILE_RECEIVED", 16) == 0) print_throughput("PUT", sent, t1 - t0);
    return 0;
}

static int cmd_get(conn_t *c, const char *remote, const char *local)
{
    char req[600], line[MAX_LINE];
    int n = snprintf(req, sizeof req, "GET %s\n", remote);
    double t0 = now_sec();
    if (send_all(c->fd, req, (size_t)n) < 0) return -1;
    if (read_line(c, line, sizeof line) < 0) { printf("connection closed by Agent\n"); return -1; }
    printf("%s\n", line);

    char name[256];
    unsigned long long size;
    if (sscanf(line, "OK FILE_SEND %255s %llu", name, &size) != 2) return 0;   /* an ERR line */

    mkdir("downloads", 0755);
    char path[600];
    if (local) snprintf(path, sizeof path, "%s", local);
    else snprintf(path, sizeof path, "downloads/%s", remote);
    FILE *out = fopen(path, "wb");
    if (!out) perror("fopen (local save)");

    /* exactly <size> raw bytes follow the header line: buffered bytes first */
    unsigned long long remaining = size;
    char tmp[8192];
    while (remaining > 0) {
        const char *src;
        size_t chunk;
        size_t avail = c->rend - c->rstart;
        if (avail > 0) {
            chunk = avail < remaining ? avail : (size_t)remaining;
            src = c->rbuf + c->rstart;
            c->rstart += chunk;
        } else {
            size_t want = remaining < sizeof tmp ? (size_t)remaining : sizeof tmp;
            ssize_t r = recv(c->fd, tmp, want, 0);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) {
                printf("connection lost during download\n");
                if (out) fclose(out);
                return -1;
            }
            chunk = (size_t)r;
            src = tmp;
        }
        if (out) fwrite(src, 1, chunk, out);
        remaining -= chunk;
    }
    double t1 = now_sec();
    if (out) {
        fclose(out);
        printf("  saved to %s\n", path);
    }
    print_throughput("GET", size, t1 - t0);
    return 0;
}

/* ================================================================== */
/*  UDP monitoring listener                                            */
/* ================================================================== */
static void *udp_listener(void *arg)
{
    (void)arg;
    char buf[512];
    while (atomic_load(&udp_run)) {
        ssize_t n = recvfrom(udp_fd, buf, sizeof buf - 1, 0, NULL, NULL);
        if (n > 0) {
            buf[n] = '\0';
            buf[strcspn(buf, "\r\n")] = '\0';
            printf("\n[UDP] %s\n", buf);
            fflush(stdout);
        }
    }
    return NULL;
}

static int udp_start(int port)
{
    if (udp_active) return 0;
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { perror("udp socket"); return -1; }
    int yes = 1;
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct timeval tv = { 1, 0 };                    /* wake up to check udp_run */
    setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(udp_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("udp bind");
        close(udp_fd);
        udp_fd = -1;
        return -1;
    }
    atomic_store(&udp_run, 1);
    pthread_create(&udp_thr, NULL, udp_listener, NULL);
    udp_active = 1;
    return 0;
}

static void udp_stop(void)
{
    if (!udp_active) return;
    atomic_store(&udp_run, 0);
    pthread_join(udp_thr, NULL);
    close(udp_fd);
    udp_fd = -1;
    udp_active = 0;
}

static void cmd_monitor(conn_t *c, const char *sub, const char *portstr)
{
    if (sub && strcasecmp(sub, "start") == 0) {
        char *end;
        long port = portstr ? strtol(portstr, &end, 10) : 0;
        if (!portstr || *end != '\0' || port < 1 || port > 65535) {
            printf("usage: monitor start <udp_port>\n");
            return;
        }
        udp_stop();
        if (udp_start((int)port) < 0) return;
        char cmd[64];
        snprintf(cmd, sizeof cmd, "MONITOR START %ld", port);
        simple_cmd(c, cmd);
    } else if (sub && strcasecmp(sub, "stop") == 0) {
        simple_cmd(c, "MONITOR STOP");
        udp_stop();
    } else {
        printf("usage: monitor start <udp_port> | monitor stop\n");
    }
}

/* ================================================================== */
static void help(void)
{
    printf("Commands:\n"
           "  auth [token]                  authenticate (default OPS-3736)\n"
           "  sysinfo                       CPU load, memory used (MB), uptime (s)\n"
           "  listproc                      list running processes\n"
           "  exec <DATE|UPTIME|DISKFREE|HOSTNAME|WHOAMI>\n"
           "  put <localfile> [remotename]  upload a file\n"
           "  get <remotename> [localfile]  download a file (default ./downloads/)\n"
           "  monitor start <udp_port>      start periodic UDP stats\n"
           "  monitor stop                  stop them\n"
           "  raw <text>                    send a raw line (to test error handling)\n"
           "  quit                          disconnect\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <agent_ip> [port]\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    const char *host = argv[1];
    const char *port = argc > 2 ? argv[2] : "9410";

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        fprintf(stderr, "cannot resolve %s\n", host);
        return 1;
    }
    conn_t c;
    memset(&c, 0, sizeof c);
    c.fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c.fd < 0 || connect(c.fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        return 1;
    }
    freeaddrinfo(res);
    printf("Connected to RemoteOps Agent at %s:%s. Type 'help'.\n", host, port);

    char in[MAX_LINE];
    int tty = isatty(STDIN_FILENO);
    for (;;) {
        if (tty) { printf("remoteops> "); fflush(stdout); }
        if (!fgets(in, sizeof in, stdin)) {                 /* EOF -> leave politely */
            simple_cmd(&c, "QUIT");
            break;
        }
        in[strcspn(in, "\r\n")] = '\0';

        char *argvv[4] = { 0 };
        char copy[MAX_LINE];
        snprintf(copy, sizeof copy, "%s", in);
        int ac = 0;
        for (char *t = strtok(copy, " \t"); t && ac < 4; t = strtok(NULL, " \t")) argvv[ac++] = t;
        if (ac == 0) continue;

        const char *cmd = argvv[0];
        if (!strcasecmp(cmd, "help")) {
            help();
        } else if (!strcasecmp(cmd, "auth")) {
            char line[128];
            snprintf(line, sizeof line, "AUTH %s", argvv[1] ? argvv[1] : DEFAULT_TOKEN);
            if (simple_cmd(&c, line) < 0) break;
        } else if (!strcasecmp(cmd, "sysinfo")) {
            if (simple_cmd(&c, "SYSINFO") < 0) break;
        } else if (!strcasecmp(cmd, "listproc")) {
            if (simple_cmd(&c, "LISTPROC") < 0) break;
        } else if (!strcasecmp(cmd, "exec")) {
            char line[128] = "EXEC", *p = line + 4;
            if (argvv[1]) {
                *p++ = ' ';
                for (const char *q = argvv[1]; *q && p < line + 120; q++) *p++ = (char)toupper((unsigned char)*q);
                *p = '\0';
            }
            if (simple_cmd(&c, line) < 0) break;
        } else if (!strcasecmp(cmd, "put")) {
            if (!argvv[1]) { printf("usage: put <localfile> [remotename]\n"); continue; }
            const char *remote = argvv[2];
            if (!remote) {
                remote = strrchr(argvv[1], '/');
                remote = remote ? remote + 1 : argvv[1];
            }
            if (cmd_put(&c, argvv[1], remote) < 0) break;
        } else if (!strcasecmp(cmd, "get")) {
            if (!argvv[1]) { printf("usage: get <remotename> [localfile]\n"); continue; }
            if (cmd_get(&c, argvv[1], argvv[2]) < 0) break;
        } else if (!strcasecmp(cmd, "monitor")) {
            cmd_monitor(&c, argvv[1], argvv[2]);
        } else if (!strcasecmp(cmd, "raw")) {
            const char *text = in + 3;
            while (*text == ' ') text++;
            if (simple_cmd(&c, text) < 0) break;
        } else if (!strcasecmp(cmd, "quit") || !strcasecmp(cmd, "exit")) {
            simple_cmd(&c, "QUIT");
            break;
        } else {
            printf("unknown command '%s' (type 'help')\n", cmd);
        }
    }
    udp_stop();
    close(c.fd);
    return 0;
}
