/*
 * チート HTTP プロキシ
 *
 * 使い方:
 *   ./cheat <listen-port> <upstream-port> [<upstream-host>] [--bind ADDR]
 *
 * 例:
 *   ./server --port 8081        # 真のサーバー
 *   ./cheat  8080 8081          # ブラウザ ↔ ここ(8080) ↔ サーバー(127.0.0.1:8081)
 *   ./cheat  8080 8081 server   # ↔ Docker 内の別サービス "server:8081" へ転送
 *
 * --bind は待受アドレス（デフォ 127.0.0.1、Docker では 0.0.0.0）
 *
 * 仕事:
 *   POST /api/sync を見つけたら body の "items":[..., X] の末尾 1 個だけを
 *   SSR(id=11) に書換、Content-Length を再計算してサーバーへ転送する。
 *   既存アイテムは触らない（= ロール直後の 1 個だけが SSR にすり替わる）。
 *   それ以外のリクエストは素通し。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <errno.h>

#define MAX_REQ      16384
#define MAX_RESP_BUF 8192
#define SSR_ITEM_ID  11           /* server.c の item_pool[11] = エクスカリバー */

static int         upstream_port;
static const char *upstream_host = "127.0.0.1";

static ssize_t write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return (ssize_t)off;
}

/* fd から HTTP リクエスト 1 本（ヘッダ＋ボディ）を読み切る。
 * 成功なら総バイト数を返し、 *body_off にボディ開始位置、 *clen に Content-Length を入れる。 */
static int read_http(int fd, char *buf, int cap, int *body_off, int *clen) {
    int total = 0;
    int header_end = -1;

    while (total < cap - 1) {
        ssize_t n = read(fd, buf + total, (size_t)(cap - 1 - total));
        if (n <= 0) return -1;
        total += (int)n;
        buf[total] = '\0';
        char *p = strstr(buf, "\r\n\r\n");
        if (p) { header_end = (int)(p - buf); break; }
    }
    if (header_end < 0) return -1;
    *body_off = header_end + 4;

    char *cl = strcasestr(buf, "\r\nContent-Length:");
    *clen = 0;
    if (cl) {
        cl += strlen("\r\nContent-Length:");
        while (*cl == ' ') cl++;
        *clen = atoi(cl);
    }

    int need = *body_off + *clen;
    while (total < need && total < cap - 1) {
        ssize_t n = read(fd, buf + total, (size_t)(need - total));
        if (n <= 0) break;
        total += (int)n;
    }
    return total;
}

/* upstream に接続して fd を返す。ホスト名/IP どちらでも可 */
static int connect_upstream(void) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", upstream_port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(upstream_host, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *r = res; r != NULL; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void forward_unchanged(int client, const char *orig, int orig_len);

/* upstream の応答を全部 client に転送 */
static void splice_upstream_to_client(int up, int client) {
    char buf[MAX_RESP_BUF];
    ssize_t n;
    while ((n = read(up, buf, sizeof(buf))) > 0) {
        if (write_all(client, buf, (size_t)n) < 0) break;
    }
}

/* body 中、items 配列末尾の数値の位置 [*start, *end) を返す。
 * 例: {"items":[1,5,2,8]} → "8" の位置を返す。
 * 数値が一つもなければ 0 を返す。 */
static int find_last_number(const char *body, int blen, int *start, int *end) {
    int rb = -1;
    for (int i = blen - 1; i >= 0; i--) {
        if (body[i] == ']') { rb = i; break; }
    }
    if (rb < 0) return 0;

    int i = rb - 1;
    while (i >= 0 && (body[i] == ' ' || body[i] == '\t' || body[i] == '\n')) i--;
    if (i < 0) return 0;
    if (!(body[i] >= '0' && body[i] <= '9')) return 0;

    *end = i + 1;
    while (i >= 0 && body[i] >= '0' && body[i] <= '9') i--;
    if (i >= 0 && body[i] == '-') i--;
    *start = i + 1;
    return 1;
}

/* 改ざんしたリクエストを upstream に送る。client にはレスポンスをそのまま返す */
static void tamper_and_forward(int client,
                               const char *orig, int orig_len,
                               int body_off, int orig_clen) {
    const char *orig_body = orig + body_off;
    int s = 0, e = 0;
    if (!find_last_number(orig_body, orig_clen, &s, &e)) {
        /* 空配列なら触れない */
        forward_unchanged(client, orig, orig_len);
        return;
    }

    char ssr_str[16];
    int ssr_len = snprintf(ssr_str, sizeof(ssr_str), "%d", SSR_ITEM_ID);

    /* 新しい body = orig_body[0..s) + "11" + orig_body[e..clen) */
    int bo = s + ssr_len + (orig_clen - e);
    char *new_body = malloc((size_t)bo + 1);
    if (!new_body) return;
    memcpy(new_body, orig_body, (size_t)s);
    memcpy(new_body + s, ssr_str, (size_t)ssr_len);
    memcpy(new_body + s + ssr_len, orig_body + e, (size_t)(orig_clen - e));

    /* 新しいヘッダ: 元のヘッダから Content-Length 行を除去して、末尾に新 CL を追加 */
    char *new_req = malloc((size_t)(body_off + bo + 256));
    if (!new_req) { free(new_body); return; }
    int wo = 0;

    /* 1 行目（リクエストライン）コピー */
    const char *eol = strstr(orig, "\r\n");
    int rl = (int)(eol - orig);
    memcpy(new_req + wo, orig, (size_t)rl);
    wo += rl;
    memcpy(new_req + wo, "\r\n", 2);
    wo += 2;

    /* 後続ヘッダを Content-Length 行だけ抜きながらコピー */
    const char *p = eol + 2;
    const char *headers_end = orig + body_off - 4; /* "\r\n\r\n" の手前 */
    while (p < headers_end) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end || line_end > headers_end) break;
        int line_len = (int)(line_end - p);
        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            /* skip */
        } else {
            memcpy(new_req + wo, p, (size_t)line_len);
            wo += line_len;
            memcpy(new_req + wo, "\r\n", 2);
            wo += 2;
        }
        p = line_end + 2;
    }

    wo += sprintf(new_req + wo, "Content-Length: %d\r\n\r\n", bo);
    memcpy(new_req + wo, new_body, (size_t)bo);
    wo += bo;

    /* 書換前の末尾アイテム ID を切り出す */
    char before_buf[16] = {0};
    int blen = e - s;
    if (blen > 0 && blen < (int)sizeof(before_buf)) {
        memcpy(before_buf, orig_body + s, (size_t)blen);
    }
    fprintf(stderr,
        "[cheat] ───── /api/sync intercepted ─────\n"
        "        ← from client: %.*s\n"
        "        → to   server: %.*s\n"
        "        書換: 末尾 id=%s → id=%d (SSR エクスカリバー)\n",
        orig_clen, orig_body,
        bo,       new_body,
        before_buf, SSR_ITEM_ID);

    int up = connect_upstream();
    if (up < 0) {
        free(new_body); free(new_req);
        return;
    }
    write_all(up, new_req, (size_t)wo);
    splice_upstream_to_client(up, client);
    close(up);
    free(new_body);
    free(new_req);
}

static void forward_unchanged(int client, const char *orig, int orig_len) {
    int up = connect_upstream();
    if (up < 0) return;
    write_all(up, orig, (size_t)orig_len);
    splice_upstream_to_client(up, client);
    close(up);
}

static void handle_connection(int client) {
    char *buf = malloc(MAX_REQ);
    if (!buf) return;

    int body_off = 0, clen = 0;
    int total = read_http(client, buf, MAX_REQ, &body_off, &clen);
    if (total <= 0) { free(buf); return; }

    int is_sync = (strncmp(buf, "POST /api/sync", 14) == 0);
    if (is_sync) {
        tamper_and_forward(client, buf, total, body_off, clen);
    } else {
        forward_unchanged(client, buf, total);
    }
    free(buf);
}

int main(int argc, char **argv) {
    int         listen_port = 0;
    int         positional  = 0;
    const char *bind_addr   = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bind") && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (positional == 0) {
            listen_port = atoi(argv[i]); positional++;
        } else if (positional == 1) {
            upstream_port = atoi(argv[i]); positional++;
        } else if (positional == 2) {
            upstream_host = argv[i]; positional++;
        } else {
            fprintf(stderr, "Usage: %s <listen-port> <upstream-port> "
                            "[<upstream-host>] [--bind ADDR]\n", argv[0]);
            return 1;
        }
    }
    if (listen_port <= 0 || upstream_port <= 0) {
        fprintf(stderr, "Usage: %s <listen-port> <upstream-port> "
                        "[<upstream-host>] [--bind ADDR]\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(listen_port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid bind address: %s\n", bind_addr);
        return 1;
    }
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server, 16) < 0) { perror("listen"); return 1; }

    fprintf(stderr,
        "[cheat-proxy] ══════════════════════════════════════════════════\n"
        "[cheat-proxy]   MITM proxy listening on %s:%d\n"
        "[cheat-proxy]   upstream: %s:%d\n"
        "[cheat-proxy]   /api/sync の body 末尾アイテムを id=%d (SSR エクスカリバー) に書換\n"
        "[cheat-proxy] ══════════════════════════════════════════════════\n",
        bind_addr, listen_port, upstream_host, upstream_port, SSR_ITEM_ID);

    while (1) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept"); continue;
        }
        handle_connection(client);
        close(client);
    }
    return 0;
}
