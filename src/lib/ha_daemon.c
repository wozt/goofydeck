// Home Assistant daemon for GoofyDeck (WebSocket-only).
// - Reads HA_HOST and HA_ACCESS_TOKEN from .env in repo root.
// - Exposes a local unix socket for multi-client sessions.
// - Supports:
//     ping
//     info
//     call <domain> <service> <json>
//     get <entity_id>
//     sub-state <entity_id>
//     unsub <sub_id>
//     subs
// - Pushes:
//     evt state <entity_id> <json_state>
//     evt connected / evt disconnected

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>

#include "../third_party/jsmn.h"

typedef struct {
    char *ha_host;
    char *ha_token;
} Env;

typedef struct {
    int fd;
    char inbuf[8192];
    size_t inlen;
    int next_sub_id;
    struct {
        int id;
        char *entity_id;
    } subs[256];
    int sub_count;
} Client;

typedef enum {
    QMSG_CONNECTED = 1,
    QMSG_DISCONNECTED = 2,
    QMSG_RESULT = 3,
    QMSG_STATE = 4
} QueueMsgType;

typedef struct {
    QueueMsgType type;
    int req_id;          // for RESULT
    int success;         // for RESULT
    char *payload_json;  // for RESULT (optional) or STATE payload
    char *entity_id;     // for STATE
} QueueMsg;

typedef enum {
    REQ_CALL = 1,
    REQ_GET = 2
} PendingType;

typedef struct {
    int in_use;
    int req_id;
    PendingType type;
    int client_fd;
    char *get_entity_id; // for GET
} Pending;

typedef enum {
    HA_REQ_CALL = 1,
    HA_REQ_GET_STATES = 2
} HaReqType;

typedef struct {
    HaReqType type;
    int req_id;
    char *domain;
    char *service;
    char *service_data_json;
    char *get_entity_id;
} HaRequest;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    HaRequest *items;
    size_t len;
    size_t cap;
    int notify_fd; // write end of pipe
} HaQueue;

static volatile sig_atomic_t g_running = 1;

// Set to 0 to silence informational logs from this daemon.
// Errors (prefixed with "[ha] ERROR") are always printed.
static int g_ha_verbose_logs = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void log_msg(const char *fmt, ...) {
    if (!g_ha_verbose_logs) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[ha] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void log_json_preview(const char *prefix, const char *json) {
    if (!prefix) prefix = "json";
    if (!json) json = "";
    char buf[241];
    size_t n = strlen(json);
    size_t cpy = n;
    if (cpy > sizeof(buf) - 1) cpy = sizeof(buf) - 1;
    memcpy(buf, json, cpy);
    buf[cpy] = 0;
    for (size_t i = 0; buf[i]; i++) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') buf[i] = ' ';
    }
    if (n > cpy) log_msg("%s: %s ...(%zu bytes)", prefix, buf, n);
    else log_msg("%s: %s", prefix, buf);
}

static void die_errno(const char *msg) {
    fprintf(stderr, "[ha] ERROR: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    if (!r) die_errno("realloc");
    return r;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) die_errno("malloc");
    memcpy(p, s, n + 1);
    return p;
}

static void trim(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int make_unix_listen(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    if (listen(fd, 32) != 0) { close(fd); return -1; }
    (void)set_nonblocking(fd);
    return fd;
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd fds[1] = {{.fd = fd, .events = POLLOUT}};
                (void)poll(fds, 1, 500);
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static void env_free(Env *e) {
    if (!e) return;
    free(e->ha_host);
    free(e->ha_token);
    e->ha_host = NULL;
    e->ha_token = NULL;
}

static char *strip_quotes_dup(const char *s) {
    if (!s) return xstrdup("");
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        char *out = malloc(n - 1);
        if (!out) die_errno("malloc");
        memcpy(out, s + 1, n - 2);
        out[n - 2] = 0;
        return out;
    }
    char *out = malloc(n + 1);
    if (!out) die_errno("malloc");
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

static int load_env_file(const char *path, Env *out) {
    env_free(out);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == 0 || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);
        char *v = strip_quotes_dup(val);
        if (strcmp(key, "HA_HOST") == 0) {
            out->ha_host = v;
        } else if (strcmp(key, "HA_ACCESS_TOKEN") == 0) {
            out->ha_token = v;
        } else {
            free(v);
        }
    }
    fclose(f);
    return 0;
}

typedef struct {
    int tls;
    char host[256];
    int port;
    char path[256];
    char origin[512];
} HaUrl;

static int parse_ha_url(const char *ha_host, HaUrl *out) {
    memset(out, 0, sizeof(*out));
    if (!ha_host || !ha_host[0]) return -1;

    const char *p = ha_host;
    if (strncmp(p, "ws://", 5) == 0) {
        out->tls = 0;
        p += 5;
    } else if (strncmp(p, "wss://", 6) == 0) {
        out->tls = 1;
        p += 6;
    } else {
        return -1;
    }

    const char *slash = strchr(p, '/');
    char hostport[256];
    if (slash) {
        snprintf(hostport, sizeof(hostport), "%.*s", (int)(slash - p), p);
        snprintf(out->path, sizeof(out->path), "%s", slash);
    } else {
        snprintf(hostport, sizeof(hostport), "%s", p);
        out->path[0] = 0;
    }

    char *colon = strrchr(hostport, ':');
    if (colon) {
        *colon = 0;
        snprintf(out->host, sizeof(out->host), "%s", hostport);
        out->port = atoi(colon + 1);
    } else {
        snprintf(out->host, sizeof(out->host), "%s", hostport);
        out->port = out->tls ? 443 : 80;
    }

    if (out->host[0] == 0 || out->port <= 0 || out->port > 65535) return -1;

    // HA wants /api/websocket by default.
    if (out->path[0] == 0) snprintf(out->path, sizeof(out->path), "/api/websocket");
    else if (strcmp(out->path, "/") == 0) snprintf(out->path, sizeof(out->path), "/api/websocket");

    snprintf(out->origin, sizeof(out->origin), "%s://%s:%d", out->tls ? "https" : "http", out->host, out->port);
    return 0;
}

static int tcp_connect_host(const char *host, int port) {
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port_s, &hints, &res);
    if (rc != 0) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

typedef struct {
    int fd;
    int tls;
    SSL_CTX *ctx;
    SSL *ssl;
} HaConn;

static void ha_conn_close(HaConn *c) {
    if (!c) return;
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ctx) {
        SSL_CTX_free(c->ctx);
        c->ctx = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

static ssize_t ha_io_read(HaConn *c, void *buf, size_t n) {
    if (!c || c->fd < 0) return -1;
    if (!c->tls) return read(c->fd, buf, n);
    return SSL_read(c->ssl, buf, (int)n);
}

static ssize_t ha_io_write(HaConn *c, const void *buf, size_t n) {
    if (!c || c->fd < 0) return -1;
    if (!c->tls) return write(c->fd, buf, n);
    return SSL_write(c->ssl, buf, (int)n);
}

static int ha_io_write_all(HaConn *c, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = ha_io_write(c, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int recv_http_until(HaConn *c, char *buf, size_t cap) {
    size_t len = 0;
    while (len + 1 < cap) {
        char ch;
        ssize_t r = ha_io_read(c, &ch, 1);
        if (r <= 0) return -1;
        buf[len++] = ch;
        buf[len] = 0;
        if (len >= 4 && memcmp(buf + len - 4, "\r\n\r\n", 4) == 0) return 0;
    }
    return -1;
}

static int b64_encode(const uint8_t *in, size_t inlen, char *out, size_t outcap) {
    int n = EVP_EncodeBlock((unsigned char *)out, in, (int)inlen);
    if (n < 0) return -1;
    if ((size_t)n + 1 > outcap) return -1;
    out[n] = 0;
    return n;
}

static int websocket_handshake(HaConn *c, const HaUrl *u) {
    uint8_t key_raw[16];
    if (RAND_bytes(key_raw, (int)sizeof(key_raw)) != 1) return -1;
    char key_b64[64];
    if (b64_encode(key_raw, sizeof(key_raw), key_b64, sizeof(key_b64)) < 0) return -1;

    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Origin: %s\r\n"
             "\r\n",
             u->path, u->host, u->port, key_b64, u->origin);
    if (ha_io_write_all(c, req, strlen(req)) != 0) return -1;

    char resp[8192];
    if (recv_http_until(c, resp, sizeof(resp)) != 0) return -1;
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0 && strncmp(resp, "HTTP/1.0 101", 12) != 0) return -1;

    // Validate Sec-WebSocket-Accept.
    char *accept_line = NULL;
    for (char *p = resp; *p; ) {
        char *line = p;
        char *nl = strstr(p, "\r\n");
        if (!nl) break;
        *nl = 0;
        p = nl + 2;
        if (strncasecmp(line, "Sec-WebSocket-Accept:", 21) == 0) {
            accept_line = line + 21;
            while (*accept_line == ' ' || *accept_line == '\t') accept_line++;
            break;
        }
    }
    if (!accept_line) return -1;

    const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char keycat[128];
    snprintf(keycat, sizeof(keycat), "%s%s", key_b64, guid);
    uint8_t sha[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)keycat, strlen(keycat), sha);
    char want_b64[64];
    if (b64_encode(sha, sizeof(sha), want_b64, sizeof(want_b64)) < 0) return -1;
    if (strcmp(accept_line, want_b64) != 0) return -1;

    return 0;
}

static int ha_connect(HaConn *c, const HaUrl *u) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->tls = u->tls;
    c->fd = tcp_connect_host(u->host, u->port);
    if (c->fd < 0) return -1;

    if (c->tls) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        c->ctx = SSL_CTX_new(TLS_client_method());
        if (!c->ctx) return -1;
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, NULL); // local trust
        c->ssl = SSL_new(c->ctx);
        if (!c->ssl) return -1;
        SSL_set_fd(c->ssl, c->fd);
        if (SSL_connect(c->ssl) != 1) return -1;
    }

    if (websocket_handshake(c, u) != 0) return -1;
    return 0;
}

static int ws_send_text(HaConn *c, const char *s) {
    size_t len = strlen(s);
    uint8_t hdr[14];
    size_t hlen = 0;
    hdr[0] = 0x81; // FIN + text
    hdr[1] = 0x80; // MASK set
    if (len <= 125) {
        hdr[1] |= (uint8_t)len;
        hlen = 2;
    } else if (len <= 0xFFFF) {
        hdr[1] |= 126;
        hdr[2] = (uint8_t)((len >> 8) & 0xff);
        hdr[3] = (uint8_t)(len & 0xff);
        hlen = 4;
    } else {
        hdr[1] |= 127;
        uint64_t v = (uint64_t)len;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)((v >> (56 - 8 * i)) & 0xff);
        hlen = 10;
    }
    uint8_t mask[4];
    if (RAND_bytes(mask, 4) != 1) return -1;
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;

    uint8_t *payload = malloc(len);
    if (!payload) return -1;
    for (size_t i = 0; i < len; i++) payload[i] = ((uint8_t)s[i]) ^ mask[i % 4];

    int rc = 0;
    if (ha_io_write_all(c, hdr, hlen) != 0) rc = -1;
    else if (ha_io_write_all(c, payload, len) != 0) rc = -1;
    free(payload);
    return rc;
}

static int ws_send_pong(HaConn *c, const uint8_t *data, size_t len) {
    uint8_t hdr[14];
    size_t hlen = 0;
    hdr[0] = 0x8A; // FIN + pong
    hdr[1] = 0x80; // MASK
    if (len <= 125) {
        hdr[1] |= (uint8_t)len;
        hlen = 2;
    } else if (len <= 0xFFFF) {
        hdr[1] |= 126;
        hdr[2] = (uint8_t)((len >> 8) & 0xff);
        hdr[3] = (uint8_t)(len & 0xff);
        hlen = 4;
    } else {
        return -1;
    }
    uint8_t mask[4];
    if (RAND_bytes(mask, 4) != 1) return -1;
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;
    uint8_t *payload = malloc(len);
    if (!payload) return -1;
    for (size_t i = 0; i < len; i++) payload[i] = data[i] ^ mask[i % 4];
    int rc = 0;
    if (ha_io_write_all(c, hdr, hlen) != 0) rc = -1;
    else if (ha_io_write_all(c, payload, len) != 0) rc = -1;
    free(payload);
    return rc;
}

static int ws_read_frame(HaConn *c, uint8_t **out_payload, size_t *out_len, int *out_opcode) {
    uint8_t hdr[2];
    ssize_t r = ha_io_read(c, hdr, 2);
    if (r <= 0) return -1;
    uint8_t b0 = hdr[0];
    uint8_t b1 = hdr[1];
    int fin = (b0 & 0x80) != 0;
    int opcode = (b0 & 0x0f);
    int masked = (b1 & 0x80) != 0;
    uint64_t len = (uint64_t)(b1 & 0x7f);

    if (!fin) return -1; // keep simple (HA uses FIN)

    if (len == 126) {
        uint8_t ext[2];
        if (ha_io_read(c, ext, 2) != 2) return -1;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (ha_io_read(c, ext, 8) != 8) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (ha_io_read(c, mask, 4) != 4) return -1;
    }

    if (len > (1024u * 1024u * 8u)) return -1;
    uint8_t *p = malloc((size_t)len + 1);
    if (!p) return -1;
    size_t off = 0;
    while (off < (size_t)len) {
        ssize_t n = ha_io_read(c, p + off, (size_t)len - off);
        if (n <= 0) { free(p); return -1; }
        off += (size_t)n;
    }
    if (masked) {
        for (size_t i = 0; i < (size_t)len; i++) p[i] ^= mask[i % 4];
    }
    p[len] = 0;
    *out_payload = p;
    *out_len = (size_t)len;
    *out_opcode = opcode;
    return 0;
}

static int json_validate_one_line(const char *json) {
    if (!json) return -1;
    while (*json == ' ' || *json == '\t') json++;
    if (*json == 0) return -1;
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, strlen(json), NULL, 0);
    if (r < 0) return -1;
    size_t tokcap = (size_t)r + 16;
    jsmntok_t *toks = calloc(tokcap, sizeof(jsmntok_t));
    if (!toks) return -1;
    jsmn_init(&p);
    r = jsmn_parse(&p, json, strlen(json), toks, (unsigned int)tokcap);
    free(toks);
    return (r >= 1) ? 0 : -1;
}

static int jsoneq(const char *js, const jsmntok_t *t, const char *s) {
    size_t n = (size_t)(t->end - t->start);
    return (t->type == JSMN_STRING && strlen(s) == n && strncmp(js + t->start, s, n) == 0);
}

static int json_find_key_linear(const char *js, jsmntok_t *toks, int tokcount, int obj_index, const char *key) {
    // Simpler robust scan for flat root objects.
    jsmntok_t *obj = &toks[obj_index];
    if (obj->type != JSMN_OBJECT) return -1;
    int i = obj_index + 1;
    int pairs = obj->size;
    for (int p = 0; p < pairs; p++) {
        if (!jsoneq(js, &toks[i], key)) {
            // skip key
            i++;
            // skip value token (and its nested children)
            int end = toks[i].end;
            i++;
            while (i < tokcount && toks[i].start < end) i++;
            continue;
        }
        return i + 1;
    }
    return -1;
}

static int json_get_int(const char *js, jsmntok_t *toks, int idx, int *out) {
    if (idx < 0) return -1;
    jsmntok_t *t = &toks[idx];
    if (t->type != JSMN_PRIMITIVE) return -1;
    char tmp[64];
    int n = t->end - t->start;
    if (n <= 0 || n >= (int)sizeof(tmp)) return -1;
    memcpy(tmp, js + t->start, (size_t)n);
    tmp[n] = 0;
    *out = atoi(tmp);
    return 0;
}

static int json_get_bool(const char *js, jsmntok_t *toks, int idx, int *out) {
    if (idx < 0) return -1;
    jsmntok_t *t = &toks[idx];
    if (t->type != JSMN_PRIMITIVE) return -1;
    const char *s = js + t->start;
    size_t n = (size_t)(t->end - t->start);
    if (n == 4 && strncmp(s, "true", 4) == 0) { *out = 1; return 0; }
    if (n == 5 && strncmp(s, "false", 5) == 0) { *out = 0; return 0; }
    return -1;
}

static char *json_token_strdup(const char *js, jsmntok_t *t) {
    if (!t) return NULL;
    int n = t->end - t->start;
    if (n < 0) return NULL;
    char *out = malloc((size_t)n + 1);
    if (!out) return NULL;
    memcpy(out, js + t->start, (size_t)n);
    out[n] = 0;
    return out;
}

static void ha_queue_init(HaQueue *q, int notify_fd) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    q->notify_fd = notify_fd;
}

static void ha_request_free(HaRequest *r) {
    if (!r) return;
    free(r->domain);
    free(r->service);
    free(r->service_data_json);
    free(r->get_entity_id);
    memset(r, 0, sizeof(*r));
}

static void ha_queue_push(HaQueue *q, HaRequest r) {
    pthread_mutex_lock(&q->mu);
    if (q->len >= q->cap) {
        q->cap = q->cap ? q->cap * 2 : 32;
        q->items = xrealloc(q->items, q->cap * sizeof(HaRequest));
    }
    q->items[q->len++] = r;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    (void)write(q->notify_fd, "x", 1);
}

typedef struct {
    pthread_mutex_t mu;
    QueueMsg *items;
    size_t len;
    size_t cap;
    int notify_fd; // write end
} OutQueue;

static void outq_init(OutQueue *q, int notify_fd) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
    q->notify_fd = notify_fd;
}

static void queue_msg_free(QueueMsg *m) {
    if (!m) return;
    free(m->payload_json);
    free(m->entity_id);
    memset(m, 0, sizeof(*m));
}

static void outq_push(OutQueue *q, QueueMsg m) {
    pthread_mutex_lock(&q->mu);
    if (q->len >= q->cap) {
        q->cap = q->cap ? q->cap * 2 : 64;
        q->items = xrealloc(q->items, q->cap * sizeof(QueueMsg));
    }
    q->items[q->len++] = m;
    pthread_mutex_unlock(&q->mu);
    (void)write(q->notify_fd, "o", 1);
}

static size_t outq_drain(OutQueue *q, QueueMsg **out_items) {
    pthread_mutex_lock(&q->mu);
    size_t n = q->len;
    if (n == 0) {
        pthread_mutex_unlock(&q->mu);
        *out_items = NULL;
        return 0;
    }
    QueueMsg *items = q->items;
    q->items = NULL;
    q->len = 0;
    q->cap = 0;
    pthread_mutex_unlock(&q->mu);
    *out_items = items;
    return n;
}

static int json_escape(const char *in, char *out, size_t cap) {
    size_t w = 0;
    for (size_t i = 0; in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        const char *rep = NULL;
        char tmp[8];
        if (c == '\"') rep = "\\\"";
        else if (c == '\\') rep = "\\\\";
        else if (c == '\b') rep = "\\b";
        else if (c == '\f') rep = "\\f";
        else if (c == '\n') rep = "\\n";
        else if (c == '\r') rep = "\\r";
        else if (c == '\t') rep = "\\t";
        else if (c < 0x20) { snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned int)c); rep = tmp; }
        if (rep) {
            size_t rl = strlen(rep);
            if (w + rl + 1 > cap) return -1;
            memcpy(out + w, rep, rl);
            w += rl;
        } else {
            if (w + 2 > cap) return -1;
            out[w++] = (char)c;
        }
    }
    out[w] = 0;
    return 0;
}

typedef struct {
    Env env;
    HaQueue *in;
    OutQueue *out;
} HaThreadCtx;

static int ws_expect_type(HaConn *c, const char *want_type) {
    uint8_t *pl = NULL;
    size_t n = 0;
    int opcode = 0;
    if (ws_read_frame(c, &pl, &n, &opcode) != 0) return -1;
    if (opcode == 0x9) { (void)ws_send_pong(c, pl, n); free(pl); return ws_expect_type(c, want_type); }
    if (opcode != 0x1) { free(pl); return -1; }

    jsmn_parser p;
    jsmn_init(&p);
    int rc = jsmn_parse(&p, (const char *)pl, n, NULL, 0);
    if (rc < 0) { free(pl); return -1; }
    int tokcap = rc + 16;
    jsmntok_t *toks = calloc((size_t)tokcap, sizeof(jsmntok_t));
    if (!toks) { free(pl); return -1; }
    jsmn_init(&p);
    rc = jsmn_parse(&p, (const char *)pl, n, toks, (unsigned int)tokcap);
    if (rc < 0) { free(toks); free(pl); return -1; }

    int type_idx = json_find_key_linear((const char *)pl, toks, rc, 0, "type");
    if (type_idx < 0) { free(toks); free(pl); return -1; }
    if (jsoneq((const char *)pl, &toks[type_idx], want_type)) {
        free(toks);
        free(pl);
        return 0;
    }
    free(toks);
    free(pl);
    return -1;
}

static void ha_send_connected(OutQueue *out, int connected) {
    QueueMsg m;
    memset(&m, 0, sizeof(m));
    m.type = connected ? QMSG_CONNECTED : QMSG_DISCONNECTED;
    outq_push(out, m);
}

static void ha_send_result(OutQueue *out, int req_id, int success, const char *payload_json) {
    QueueMsg m;
    memset(&m, 0, sizeof(m));
    m.type = QMSG_RESULT;
    m.req_id = req_id;
    m.success = success;
    if (payload_json) m.payload_json = xstrdup(payload_json);
    outq_push(out, m);
}

static void ha_send_state(OutQueue *out, const char *entity_id, const char *state_json) {
    QueueMsg m;
    memset(&m, 0, sizeof(m));
    m.type = QMSG_STATE;
    m.entity_id = xstrdup(entity_id);
    m.payload_json = xstrdup(state_json);
    outq_push(out, m);
}

static int json_extract_state_value(const char *state_obj_json, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = 0;
    if (!state_obj_json || !state_obj_json[0]) return -1;
    jsmn_parser p;
    jsmn_init(&p);
    int rc = jsmn_parse(&p, state_obj_json, strlen(state_obj_json), NULL, 0);
    if (rc < 0) return -1;
    jsmntok_t *toks = calloc((size_t)rc + 16, sizeof(jsmntok_t));
    if (!toks) return -1;
    jsmn_init(&p);
    rc = jsmn_parse(&p, state_obj_json, strlen(state_obj_json), toks, (unsigned int)((size_t)rc + 16));
    if (rc < 0 || toks[0].type != JSMN_OBJECT) {
        free(toks);
        return -1;
    }
    int st_idx = json_find_key_linear(state_obj_json, toks, rc, 0, "state");
    if (st_idx < 0 || toks[st_idx].type != JSMN_STRING) {
        free(toks);
        return -1;
    }
    int n = toks[st_idx].end - toks[st_idx].start;
    if (n <= 0) {
        free(toks);
        return -1;
    }
    size_t cpy = (size_t)n;
    if (cpy + 1 > cap) cpy = cap - 1;
    memcpy(out, state_obj_json + toks[st_idx].start, cpy);
    out[cpy] = 0;
    free(toks);
    return 0;
}

static int ha_ws_send_auth(HaConn *c, const char *token) {
    char esc[4096];
    if (json_escape(token ? token : "", esc, sizeof(esc)) != 0) return -1;
    char msg[4600];
    snprintf(msg, sizeof(msg), "{\"type\":\"auth\",\"access_token\":\"%s\"}", esc);
    return ws_send_text(c, msg);
}

static int ha_ws_send_subscribe_state_changed(HaConn *c, int sub_id) {
    char msg[256];
    snprintf(msg, sizeof(msg), "{\"id\":%d,\"type\":\"subscribe_events\",\"event_type\":\"state_changed\"}", sub_id);
    return ws_send_text(c, msg);
}

static int ha_ws_send_call(HaConn *c, int id, const char *domain, const char *service, const char *service_data_json) {
    char d[256], s[256];
    if (json_escape(domain, d, sizeof(d)) != 0) return -1;
    if (json_escape(service, s, sizeof(s)) != 0) return -1;
    if (!service_data_json || !service_data_json[0]) service_data_json = "{}";
    char msg[8192];
    int n = snprintf(msg, sizeof(msg),
                     "{\"id\":%d,\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\",\"service_data\":%s}",
                     id, d, s, service_data_json);
    if (n <= 0 || (size_t)n >= sizeof(msg)) return -1;
    return ws_send_text(c, msg);
}

static int ha_ws_send_get_states(HaConn *c, int id) {
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"id\":%d,\"type\":\"get_states\"}", id);
    return ws_send_text(c, msg);
}

static void *ha_thread_main(void *arg) {
    HaThreadCtx *ctx = (HaThreadCtx *)arg;
    const char *env_path = ".env";

    while (g_running) {
        Env e = {0};
        (void)load_env_file(env_path, &e);
        if (!e.ha_host || !e.ha_token || !e.ha_host[0] || !e.ha_token[0]) {
            env_free(&e);
            ha_send_connected(ctx->out, 0);
            sleep(1);
            continue;
        }

        HaUrl url;
        if (parse_ha_url(e.ha_host, &url) != 0) {
            env_free(&e);
            ha_send_connected(ctx->out, 0);
            sleep(1);
            continue;
        }

        HaConn conn;
        if (ha_connect(&conn, &url) != 0) {
            env_free(&e);
            ha_conn_close(&conn);
            ha_send_connected(ctx->out, 0);
            sleep(1);
            continue;
        }

        // Expect auth_required, then auth, then auth_ok.
        if (ws_expect_type(&conn, "auth_required") != 0) {
            env_free(&e);
            ha_conn_close(&conn);
            ha_send_connected(ctx->out, 0);
            sleep(1);
            continue;
        }
        if (ha_ws_send_auth(&conn, e.ha_token) != 0 || ws_expect_type(&conn, "auth_ok") != 0) {
            env_free(&e);
            ha_conn_close(&conn);
            ha_send_connected(ctx->out, 0);
            sleep(1);
            continue;
        }

        // Subscribe to state_changed once (id=1 reserved).
        const int ha_state_sub_id = 1;
        if (ha_ws_send_subscribe_state_changed(&conn, ha_state_sub_id) != 0) {
            env_free(&e);
            ha_conn_close(&conn);
            ha_send_connected(ctx->out, 0);
            sleep(1);
            continue;
        }

        ha_send_connected(ctx->out, 1);
        log_msg("connected to HA at %s%s", e.ha_host, url.path);

        // Main loop: interleave outgoing requests and incoming frames.
        while (g_running) {
            // send queued requests without blocking too long
            HaRequest req;
            int have_req = 0;
            pthread_mutex_lock(&ctx->in->mu);
            if (ctx->in->len > 0) {
                req = ctx->in->items[0];
                memmove(ctx->in->items, ctx->in->items + 1, (ctx->in->len - 1) * sizeof(HaRequest));
                ctx->in->len--;
                have_req = 1;
            }
            pthread_mutex_unlock(&ctx->in->mu);

            if (have_req) {
                int rc = 0;
                if (req.type == HA_REQ_CALL) {
                    {
                        char tmp[512];
                        snprintf(tmp, sizeof(tmp), "tx call id=%d %s.%s", req.req_id, req.domain ? req.domain : "", req.service ? req.service : "");
                        log_msg("%s", tmp);
                        if (req.service_data_json) log_json_preview("tx service_data", req.service_data_json);
                    }
                    rc = ha_ws_send_call(&conn, req.req_id, req.domain, req.service, req.service_data_json);
                } else if (req.type == HA_REQ_GET_STATES) {
                    log_msg("tx get_states id=%d", req.req_id);
                    rc = ha_ws_send_get_states(&conn, req.req_id);
                }
                if (rc != 0) {
                    // connection likely broken; requeue? for now, fail fast.
                    ha_send_result(ctx->out, req.req_id, 0, NULL);
                    ha_request_free(&req);
                    break;
                }
                ha_request_free(&req);
            }

            struct pollfd pfd = {.fd = conn.fd, .events = POLLIN};
            int pr = poll(&pfd, 1, 50);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) continue;

            uint8_t *pl = NULL;
            size_t n = 0;
            int opcode = 0;
            if (ws_read_frame(&conn, &pl, &n, &opcode) != 0) {
                break;
            }
            if (opcode == 0x9) { (void)ws_send_pong(&conn, pl, n); free(pl); continue; }
            if (opcode != 0x1) { free(pl); continue; }

            const char *js = (const char *)pl;
            size_t jslen = n;
            jsmn_parser p;
            jsmn_init(&p);
            int rc = jsmn_parse(&p, js, jslen, NULL, 0);
            if (rc < 0) { free(pl); continue; }
            int tokcap = rc + 32;
            jsmntok_t *toks = calloc((size_t)tokcap, sizeof(jsmntok_t));
            if (!toks) { free(pl); continue; }
            jsmn_init(&p);
            rc = jsmn_parse(&p, js, jslen, toks, (unsigned int)tokcap);
            if (rc < 0) { free(toks); free(pl); continue; }

            int type_idx = json_find_key_linear(js, toks, rc, 0, "type");
            if (type_idx < 0) { free(toks); free(pl); continue; }

            // result messages (for call_service / get_states / subscribe result)
            if (jsoneq(js, &toks[type_idx], "result")) {
                int id = 0;
                int ok = 0;
                int id_idx = json_find_key_linear(js, toks, rc, 0, "id");
                int suc_idx = json_find_key_linear(js, toks, rc, 0, "success");
                if (json_get_int(js, toks, id_idx, &id) == 0) {
                    if (json_get_bool(js, toks, suc_idx, &ok) != 0) ok = 0;
                    // For get_states, payload lives under "result" token.
                    int res_idx = json_find_key_linear(js, toks, rc, 0, "result");
                    if (res_idx >= 0 && toks[res_idx].start >= 0 && toks[res_idx].end > toks[res_idx].start) {
                        int rlen = toks[res_idx].end - toks[res_idx].start;
                        char *payload = malloc((size_t)rlen + 1);
                        if (payload) {
                            memcpy(payload, js + toks[res_idx].start, (size_t)rlen);
                            payload[rlen] = 0;
                            ha_send_result(ctx->out, id, ok, payload);
                            free(payload);
                        } else {
                            ha_send_result(ctx->out, id, ok, NULL);
                        }
                    } else {
                        ha_send_result(ctx->out, id, ok, NULL);
                    }
                }
            } else if (jsoneq(js, &toks[type_idx], "event")) {
                // state_changed: extract entity_id + new_state JSON
                int id_idx = json_find_key_linear(js, toks, rc, 0, "id");
                int msg_id = 0;
                if (json_get_int(js, toks, id_idx, &msg_id) != 0) msg_id = 0;
                if (msg_id != ha_state_sub_id) {
                    free(toks); free(pl);
                    continue;
                }

                int ev_idx = json_find_key_linear(js, toks, rc, 0, "event");
                if (ev_idx < 0 || toks[ev_idx].type != JSMN_OBJECT) { free(toks); free(pl); continue; }
                int data_idx = json_find_key_linear(js, toks, rc, ev_idx, "data");
                if (data_idx < 0 || toks[data_idx].type != JSMN_OBJECT) { free(toks); free(pl); continue; }
                int ent_idx = json_find_key_linear(js, toks, rc, data_idx, "entity_id");
                if (ent_idx < 0) { free(toks); free(pl); continue; }
                char *entity = json_token_strdup(js, &toks[ent_idx]);
                if (!entity) { free(toks); free(pl); continue; }
                int ns_idx = json_find_key_linear(js, toks, rc, data_idx, "new_state");
                if (ns_idx < 0) { free(entity); free(toks); free(pl); continue; }
                int start = toks[ns_idx].start;
                int end = toks[ns_idx].end;
                if (start < 0 || end <= start) { free(entity); free(toks); free(pl); continue; }
                int slen = end - start;
                char *state = malloc((size_t)slen + 1);
                if (!state) { free(entity); free(toks); free(pl); continue; }
                memcpy(state, js + start, (size_t)slen);
                state[slen] = 0;
                ha_send_state(ctx->out, entity, state);
                free(state);
                free(entity);
            }

            free(toks);
            free(pl);
        }

        ha_send_connected(ctx->out, 0);
        ha_conn_close(&conn);
        env_free(&e);
        sleep(1);
    }

    return NULL;
}

static int send_line(Client *c, const char *line) {
    if (!c || c->fd < 0 || !line) return -1;
    return write_all(c->fd, line, strlen(line));
}

static void client_free(Client *c) {
    if (!c) return;
    for (int i = 0; i < c->sub_count; i++) free(c->subs[i].entity_id);
    c->sub_count = 0;
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->inlen = 0;
    c->next_sub_id = 1;
}

static Client *clients_add(Client **arr, size_t *len, size_t *cap, int fd) {
    if (*len >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = xrealloc(*arr, *cap * sizeof(Client));
    }
    Client *c = &(*arr)[(*len)++];
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->next_sub_id = 1;
    (void)set_nonblocking(fd);
    return c;
}

static Client *clients_find(Client *arr, size_t len, int fd) {
    for (size_t i = 0; i < len; i++) if (arr[i].fd == fd) return &arr[i];
    return NULL;
}

static void clients_remove_at(Client *arr, size_t *len, size_t idx) {
    client_free(&arr[idx]);
    if (idx + 1 < *len) memmove(&arr[idx], &arr[idx + 1], (*len - idx - 1) * sizeof(Client));
    (*len)--;
}

static Pending *pending_find(Pending *p, size_t n, int req_id) {
    for (size_t i = 0; i < n; i++) if (p[i].in_use && p[i].req_id == req_id) return &p[i];
    return NULL;
}

static Pending *pending_alloc(Pending *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (!p[i].in_use) return &p[i];
    return NULL;
}

static int cmd_parse_call(char *line, char **out_domain, char **out_service, char **out_json) {
    // line: "call <domain> <service> <json...>"
    trim(line);
    if (strncmp(line, "call ", 5) != 0) return -1;
    char *p = line + 5;
    while (*p == ' ') p++;
    char *domain = p;
    while (*p && *p != ' ') p++;
    if (!*p) return -1;
    *p++ = 0;
    while (*p == ' ') p++;
    char *service = p;
    while (*p && *p != ' ') p++;
    if (!*p) return -1;
    *p++ = 0;
    while (*p == ' ') p++;
    char *json = p;
    if (!*json) return -1;
    *out_domain = domain;
    *out_service = service;
    *out_json = json;
    return 0;
}

int main(int argc, char **argv) {
    const char *sock_path = "/tmp/goofydeck_ha.sock";
    const char *env_path = ".env";
    (void)env_path;
    int opt;
    (void)opt;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
            printf("Usage: %s [--sock /tmp/goofydeck_ha.sock]\n", argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = make_unix_listen(sock_path);
    if (listen_fd < 0) die_errno("listen socket");
    log_msg("listening on %s", sock_path);

    int ha_in_pipe[2];
    int ha_out_pipe[2];
    if (pipe(ha_in_pipe) != 0) die_errno("pipe");
    if (pipe(ha_out_pipe) != 0) die_errno("pipe");
    (void)set_nonblocking(ha_out_pipe[0]);
    (void)set_nonblocking(ha_out_pipe[1]);

    HaQueue inq;
    ha_queue_init(&inq, ha_in_pipe[1]);
    OutQueue outq;
    outq_init(&outq, ha_out_pipe[1]);

    HaThreadCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.in = &inq;
    ctx.out = &outq;
    pthread_t thr;
    pthread_create(&thr, NULL, ha_thread_main, &ctx);

    Client *clients = NULL;
    size_t client_len = 0, client_cap = 0;

    Pending pending[1024];
    memset(pending, 0, sizeof(pending));
    int next_req_id = 100;
    int ha_connected = 0;

    while (g_running) {
        // poll: listen fd + outq notify + clients
        size_t nfds = 2 + client_len;
        struct pollfd *fds = calloc(nfds, sizeof(struct pollfd));
        if (!fds) die_errno("calloc");
        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;
        fds[1].fd = ha_out_pipe[0];
        fds[1].events = POLLIN;
        for (size_t i = 0; i < client_len; i++) {
            fds[2 + i].fd = clients[i].fd;
            fds[2 + i].events = POLLIN;
        }

        int pr = poll(fds, (nfds_t)nfds, 200);
        if (pr < 0) {
            free(fds);
            if (errno == EINTR) continue;
            die_errno("poll");
        }

        // Accept new clients
        if (fds[0].revents & POLLIN) {
            for (;;) {
                int cfd = accept(listen_fd, NULL, NULL);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }
                Client *c = clients_add(&clients, &client_len, &client_cap, cfd);
                (void)c;
                log_msg("client connected fd=%d", cfd);
                if (ha_connected) (void)write_all(cfd, "evt connected\n", 14);
                else (void)write_all(cfd, "evt disconnected\n", 17);
            }
        }

        // Drain HA out queue notifications
        if (fds[1].revents & POLLIN) {
            char tmp[256];
            while (read(ha_out_pipe[0], tmp, sizeof(tmp)) > 0) {}
            QueueMsg *items = NULL;
            size_t n = outq_drain(&outq, &items);
            for (size_t i = 0; i < n; i++) {
                QueueMsg *m = &items[i];
                if (m->type == QMSG_CONNECTED) {
                    int was = ha_connected;
                    ha_connected = 1;
                    if (!was) log_msg("ws connected");
                    for (size_t ci = 0; ci < client_len; ci++) (void)send_line(&clients[ci], "evt connected\n");
                } else if (m->type == QMSG_DISCONNECTED) {
                    int was = ha_connected;
                    ha_connected = 0;
                    if (was) log_msg("ws disconnected");
                    for (size_t ci = 0; ci < client_len; ci++) (void)send_line(&clients[ci], "evt disconnected\n");
                } else if (m->type == QMSG_RESULT) {
                    Pending *p = pending_find(pending, sizeof(pending)/sizeof(pending[0]), m->req_id);
                    if (p && p->in_use) {
                        Client *c = clients_find(clients, client_len, p->client_fd);
                        if (c) {
                            if (!ha_connected) {
                                log_msg("req id=%d result: err ha_disconnected", m->req_id);
                                (void)send_line(c, "err ha_disconnected\n");
                            } else if (!m->success) {
                                log_msg("req id=%d result: err ha_error", m->req_id);
                                (void)send_line(c, "err ha_error\n");
                            } else if (p->type == REQ_GET) {
                                log_msg("req id=%d result: ok (get entity_id=%s)", m->req_id, p->get_entity_id ? p->get_entity_id : "");
                                // payload_json is an array for get_states; extract entity here.
                                const char *payload = m->payload_json ? m->payload_json : "[]";
                                const char *want = p->get_entity_id ? p->get_entity_id : "";
                                // Parse array of state objects and find entity_id.
                                jsmn_parser jp;
                                jsmn_init(&jp);
                                int rc = jsmn_parse(&jp, payload, strlen(payload), NULL, 0);
                                if (rc < 0) {
                                    (void)send_line(c, "err bad_json\n");
                                } else {
                                    jsmntok_t *toks = calloc((size_t)rc + 32, sizeof(jsmntok_t));
                                    if (!toks) {
                                        (void)send_line(c, "err oom\n");
                                    } else {
                                        jsmn_init(&jp);
                                        rc = jsmn_parse(&jp, payload, strlen(payload), toks, (unsigned int)((size_t)rc + 32));
                                        if (rc < 0 || toks[0].type != JSMN_ARRAY) {
                                            (void)send_line(c, "err bad_json\n");
                                        } else {
                                            int found = 0;
                                            // tokens: array elements are objects; scan for entity_id.
                                            for (int ti = 1; ti < rc; ti++) {
                                                if (toks[ti].type != JSMN_OBJECT) continue;
                                                int ent_idx = json_find_key_linear(payload, toks, rc, ti, "entity_id");
                                                if (ent_idx < 0 || toks[ent_idx].type != JSMN_STRING) continue;
                                                int nlen = toks[ent_idx].end - toks[ent_idx].start;
                                                if (nlen <= 0) continue;
                                                if ((int)strlen(want) == nlen && strncmp(payload + toks[ent_idx].start, want, (size_t)nlen) == 0) {
                                                    int start = toks[ti].start;
                                                    int end = toks[ti].end;
                                                    if (start >= 0 && end > start) {
                                                        int slen = end - start;
                                                        char *resp = malloc((size_t)slen + 32);
                                                        if (resp) {
                                                            memcpy(resp, "ok ", 3);
                                                            memcpy(resp + 3, payload + start, (size_t)slen);
                                                            resp[3 + slen] = '\n';
                                                            resp[3 + slen + 1] = 0;
                                                            (void)send_line(c, resp);
                                                            free(resp);
                                                            found = 1;
                                                            break;
                                                        }
                                                    }
                                                }
                                            }
                                            if (!found) (void)send_line(c, "err not_found\n");
                                        }
                                        free(toks);
                                    }
                                }
                            } else {
                                log_msg("req id=%d result: ok (call)", m->req_id);
                                (void)send_line(c, "ok\n");
                            }
                        }
                        free(p->get_entity_id);
                        memset(p, 0, sizeof(*p));
                    }
                } else if (m->type == QMSG_STATE) {
                    if (!m->entity_id || !m->payload_json) continue;
                    {
                        char st[96] = {0};
                        if (json_extract_state_value(m->payload_json, st, sizeof(st)) == 0 && st[0]) {
                            log_msg("rx state entity=%s state=%s", m->entity_id, st);
                        } else {
                            log_msg("rx state entity=%s", m->entity_id);
                        }
                    }
                    for (size_t ci = 0; ci < client_len; ci++) {
                        Client *c = &clients[ci];
                        for (int si = 0; si < c->sub_count; si++) {
                            if (strcmp(c->subs[si].entity_id, m->entity_id) == 0) {
                                char line[8192];
                                int nn = snprintf(line, sizeof(line), "evt state %s %s\n", m->entity_id, m->payload_json);
                                if (nn > 0 && (size_t)nn < sizeof(line)) (void)send_line(c, line);
                            }
                        }
                    }
                }
                queue_msg_free(m);
            }
            free(items);
        }

        // Read client commands
        for (size_t i = 0; i < client_len; ) {
            Client *c = &clients[i];
            short re = fds[2 + i].revents;
            if (re & (POLLHUP | POLLERR)) {
                clients_remove_at(clients, &client_len, i);
                continue;
            }
            if (!(re & POLLIN)) { i++; continue; }

            char buf[1024];
            ssize_t n = read(c->fd, buf, sizeof(buf));
            if (n <= 0) {
                clients_remove_at(clients, &client_len, i);
                continue;
            }
            if (c->inlen + (size_t)n > sizeof(c->inbuf)) c->inlen = 0;
            memcpy(c->inbuf + c->inlen, buf, (size_t)n);
            c->inlen += (size_t)n;

            size_t start = 0;
            for (;;) {
                void *nlp = memchr(c->inbuf + start, '\n', c->inlen - start);
                if (!nlp) break;
                size_t nl = (size_t)((char *)nlp - (c->inbuf + start));
                char line[2048];
                size_t cpy = nl;
                if (cpy >= sizeof(line)) cpy = sizeof(line) - 1;
                memcpy(line, c->inbuf + start, cpy);
                line[cpy] = 0;
                start += nl + 1;
                trim(line);
                if (line[0] == 0) continue;

                if (strcmp(line, "ping") == 0) {
                    (void)send_line(c, "ok\n");
                    continue;
                }
                if (strcmp(line, "info") == 0) {
                    char out[256];
                    snprintf(out, sizeof(out), "ok {\"ws\":\"%s\"}\n", ha_connected ? "connected" : "disconnected");
                    (void)send_line(c, out);
                    continue;
                }
                if (strcmp(line, "subs") == 0) {
                    // minimal JSON list
                    char out[8192];
                    size_t w = 0;
                    w += (size_t)snprintf(out + w, sizeof(out) - w, "ok [");
                    for (int si = 0; si < c->sub_count; si++) {
                        w += (size_t)snprintf(out + w, sizeof(out) - w, "%s{\"id\":%d,\"entity_id\":\"%s\"}",
                                              (si ? "," : ""), c->subs[si].id, c->subs[si].entity_id);
                    }
                    w += (size_t)snprintf(out + w, sizeof(out) - w, "]\n");
                    (void)send_line(c, out);
                    continue;
                }
                if (strncmp(line, "sub-state ", 10) == 0) {
                    char *entity = line + 10;
                    trim(entity);
                    if (entity[0] == 0) { (void)send_line(c, "err bad_args\n"); continue; }
                    if (c->sub_count >= (int)(sizeof(c->subs)/sizeof(c->subs[0]))) { (void)send_line(c, "err too_many\n"); continue; }
                    int id = c->next_sub_id++;
                    c->subs[c->sub_count].id = id;
                    c->subs[c->sub_count].entity_id = xstrdup(entity);
                    c->sub_count++;
                    log_msg("subscribe fd=%d sub_id=%d entity=%s", c->fd, id, entity);
                    char out[64];
                    snprintf(out, sizeof(out), "ok sub_id=%d\n", id);
                    (void)send_line(c, out);
                    continue;
                }
                if (strncmp(line, "unsub ", 6) == 0) {
                    int id = atoi(line + 6);
                    int found = 0;
                    for (int si = 0; si < c->sub_count; si++) {
                        if (c->subs[si].id == id) {
                            log_msg("unsubscribe fd=%d sub_id=%d entity=%s", c->fd, id, c->subs[si].entity_id ? c->subs[si].entity_id : "");
                            free(c->subs[si].entity_id);
                            memmove(&c->subs[si], &c->subs[si + 1], (size_t)(c->sub_count - si - 1) * sizeof(c->subs[0]));
                            c->sub_count--;
                            found = 1;
                            break;
                        }
                    }
                    if (!found) log_msg("unsubscribe fd=%d sub_id=%d (not_found)", c->fd, id);
                    (void)send_line(c, found ? "ok\n" : "err not_found\n");
                    continue;
                }
                if (strncmp(line, "get ", 4) == 0) {
                    if (!ha_connected) { (void)send_line(c, "err ha_disconnected\n"); continue; }
                    char *entity = line + 4;
                    trim(entity);
                    if (!entity[0]) { (void)send_line(c, "err bad_args\n"); continue; }
                    log_msg("cmd get fd=%d entity=%s", c->fd, entity);
                    Pending *p = pending_alloc(pending, sizeof(pending)/sizeof(pending[0]));
                    if (!p) { (void)send_line(c, "err busy\n"); continue; }
                    int id = next_req_id++;
                    p->in_use = 1;
                    p->req_id = id;
                    p->type = REQ_GET;
                    p->client_fd = c->fd;
                    p->get_entity_id = xstrdup(entity);
                    HaRequest r;
                    memset(&r, 0, sizeof(r));
                    r.type = HA_REQ_GET_STATES;
                    r.req_id = id;
                    r.get_entity_id = xstrdup(entity);
                    ha_queue_push(&inq, r);
                    continue;
                }
                if (strncmp(line, "call ", 5) == 0) {
                    if (!ha_connected) { (void)send_line(c, "err ha_disconnected\n"); continue; }
                    char *domain = NULL, *service = NULL, *json = NULL;
                    if (cmd_parse_call(line, &domain, &service, &json) != 0) { (void)send_line(c, "err bad_args\n"); continue; }
                    if (json_validate_one_line(json) != 0) { (void)send_line(c, "err bad_json\n"); continue; }
                    log_msg("cmd call fd=%d %s.%s", c->fd, domain, service);
                    log_json_preview("cmd service_data", json);
                    Pending *p = pending_alloc(pending, sizeof(pending)/sizeof(pending[0]));
                    if (!p) { (void)send_line(c, "err busy\n"); continue; }
                    int id = next_req_id++;
                    p->in_use = 1;
                    p->req_id = id;
                    p->type = REQ_CALL;
                    p->client_fd = c->fd;
                    p->get_entity_id = NULL;
                    HaRequest r;
                    memset(&r, 0, sizeof(r));
                    r.type = HA_REQ_CALL;
                    r.req_id = id;
                    r.domain = xstrdup(domain);
                    r.service = xstrdup(service);
                    r.service_data_json = xstrdup(json);
                    ha_queue_push(&inq, r);
                    continue;
                }

                (void)send_line(c, "err unknown\n");
            }

            if (start > 0 && start < c->inlen) {
                memmove(c->inbuf, c->inbuf + start, c->inlen - start);
                c->inlen -= start;
            } else if (start >= c->inlen) {
                c->inlen = 0;
            }

            i++;
        }

        free(fds);
    }

    // shutdown
    for (size_t i = 0; i < client_len; i++) client_free(&clients[i]);
    free(clients);
    close(listen_fd);
    unlink(sock_path);
    pthread_join(thr, NULL);
    return 0;
}
