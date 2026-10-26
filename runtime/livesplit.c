// LiveSplit Server client (Gameplay tab "LiveSplit"): the run timer (runtime/hle/run_timer.c) drives LiveSplit through
// its Server component's text protocol - one command per line, "\r\n" ended: starttimer, initgametime, setgametime
// <h:mm:ss.fff>, pausegametime / unpausegametime, split. Nothing is read back.
//
// Its own thread: connects to the host:port of the setting (default localhost:16834), sends what the game thread
// queued, and while LiveSplit isn't there tries again every few seconds without a word (one log line per change).
// The game thread only appends to a small queue under a mutex; commands queued while not connected are dropped, and
// a new connection is brought up to date from the run's state instead (run_timer_livesplit_sync).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hle.h"
#include "livesplit.h"
#include "settings.h"

void run_timer_livesplit_sync(void (*send)(const char *line, void *ctx), void *ctx);  // runtime/hle/run_timer.c

enum { QMAX = 64, LINE = 96, RETRY_S = 3 };
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static char g_q[QMAX][LINE];
static int g_qhead, g_qn;
static volatile int g_connected;
static int g_started;

bool livesplit_connected(void) { return g_connected != 0; }

static void ensure_thread(void);

void livesplit_cmd(const char *line) {
    if (!g_settings.livesplit) return;
    ensure_thread();
    pthread_mutex_lock(&g_lock);
    if (g_connected && g_qn < QMAX) {
        snprintf(g_q[(g_qhead + g_qn) % QMAX], LINE, "%s", line);
        g_qn++;
        pthread_cond_signal(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
}

// host:port of the setting ("localhost", "127.0.0.1:16834", "[::1]:16834")
static void parse_host(const char *s, char *host, size_t hn, char *port, size_t pn) {
    snprintf(port, pn, "16834");
    while (*s == ' ') s++;
    if (*s == '[') {  // [IPv6]:port
        const char *e = strchr(s, ']');
        snprintf(host, hn, "%.*s", e ? (int)(e - s - 1) : (int)strlen(s + 1), s + 1);
        if (e && e[1] == ':' && e[2]) snprintf(port, pn, "%s", e + 2);
    } else {
        const char *c = strrchr(s, ':');
        if (c && strchr(s, ':') == c) {  // (one colon: host:port; more: a bare IPv6 address)
            snprintf(host, hn, "%.*s", (int)(c - s), s);
            if (c[1]) snprintf(port, pn, "%s", c + 1);
        } else snprintf(host, hn, "%s", s);
    }
    size_t n = strlen(host);
    while (n && host[n - 1] == ' ') host[--n] = 0;
    if (!host[0]) snprintf(host, hn, "localhost");
}

static void set_nonblocking(sock_t s, bool on) {
#ifdef _WIN32
    u_long v = on;
    ioctlsocket(s, FIONBIO, &v);
#else
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
#endif
}

// a connection within ~2 s, or BAD_SOCK
static sock_t try_connect(const char *setting) {
    char host[128], port[16];
    parse_host(setting, host, sizeof host, port, sizeof port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) || !res) return BAD_SOCK;
    sock_t fd = BAD_SOCK;
    for (struct addrinfo *a = res; a && fd == BAD_SOCK; a = a->ai_next) {
        sock_t s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == BAD_SOCK) continue;
        set_nonblocking(s, true);
        int r = connect(s, a->ai_addr, (int)a->ai_addrlen);
        bool ok = r == 0;
        if (!ok) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(s, &w);
            struct timeval tv = {2, 0};
            if (select((int)s + 1, NULL, &w, NULL, &tv) == 1) {
                int err = 0;
                socklen_t len = sizeof err;
                ok = !getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) && !err;
            }
        }
        if (!ok) { sock_close(s); continue; }
        set_nonblocking(s, false);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#endif
#ifdef _WIN32
        DWORD to = 2000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
#else
        struct timeval to = {2, 0};
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
#endif
        fd = s;
    }
    freeaddrinfo(res);
    return fd;
}

typedef struct { sock_t fd; bool ok; } Conn;

static void send_line(const char *line, void *ctx) {
    Conn *c = ctx;
    if (!c->ok) return;
    char b[LINE + 4];
    int n = snprintf(b, sizeof b, "%s\r\n", line);
#ifdef MSG_NOSIGNAL
    int flags = MSG_NOSIGNAL;
#else
    int flags = 0;
#endif
    for (int done = 0; done < n;) {
        int w = (int)send(c->fd, b + done, n - done, flags);
        if (w <= 0) { c->ok = false; return; }
        done += w;
    }
    if (ENV("LARECOMP_LIVESPLIT_TRACE")) LOG("[livesplit] > %s\n", line);
}

// the peer closed the connection (LiveSplit quit): a read that would block is fine
static bool peer_closed(sock_t fd) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    struct timeval tv = {0, 0};
    if (select((int)fd + 1, &r, NULL, NULL, &tv) != 1) return false;
    char b[256];
    int n = (int)recv(fd, b, sizeof b, 0);  // (LiveSplit answers get* commands only: anything else is discarded)
    return n <= 0;
}

static void wait_s(int secs) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += secs;
    pthread_cond_timedwait(&g_cond, &g_lock, &ts);
}

static void *thread_main(void *arg) {
    (void)arg;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    Conn c = {BAD_SOCK, false};
    char host[sizeof g_settings.livesplit_host] = "";
    bool told_down = false;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        bool want = g_settings.livesplit;
        if (c.fd != BAD_SOCK && (!want || !c.ok || strcmp(host, g_settings.livesplit_host) || peer_closed(c.fd))) {
            sock_close(c.fd);
            c.fd = BAD_SOCK;
            g_connected = 0;
            g_qn = 0;
            LOG("[livesplit] disconnected from %s\n", host);
            told_down = true;
        }
        if (!want) { wait_s(1); continue; }
        if (c.fd == BAD_SOCK) {
            snprintf(host, sizeof host, "%s", g_settings.livesplit_host);
            pthread_mutex_unlock(&g_lock);
            sock_t fd = try_connect(host);
            pthread_mutex_lock(&g_lock);
            if (fd == BAD_SOCK) {
                if (!told_down) LOG("[livesplit] no LiveSplit Server at %s: trying again every %d s\n", host, RETRY_S);
                told_down = true;
                wait_s(RETRY_S);
                continue;
            }
            c.fd = fd, c.ok = true;
            g_connected = 1;
            g_qn = 0;
            told_down = false;
            LOG("[livesplit] connected to %s\n", host);
            pthread_mutex_unlock(&g_lock);
            run_timer_livesplit_sync(send_line, &c);
            pthread_mutex_lock(&g_lock);
            continue;
        }
        if (!g_qn) { wait_s(1); continue; }
        char line[LINE];
        snprintf(line, sizeof line, "%s", g_q[g_qhead]);
        g_qhead = (g_qhead + 1) % QMAX, g_qn--;
        pthread_mutex_unlock(&g_lock);
        send_line(line, &c);
        pthread_mutex_lock(&g_lock);
    }
    return NULL;
}

static void ensure_thread(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_started) {
        g_started = 1;
        pthread_t t;
        if (pthread_create(&t, NULL, thread_main, NULL) == 0) pthread_detach(t);
        else LOG("[livesplit] cannot start its thread\n");
    }
    pthread_mutex_unlock(&g_lock);
}

void livesplit_poke(void) {
    if (!g_settings.livesplit) return;
    if (!g_started) ensure_thread();
    pthread_mutex_lock(&g_lock);
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}
