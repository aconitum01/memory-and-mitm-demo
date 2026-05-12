/*
 * ガチャゲーム サーバー（脆弱版）
 *
 * エンドポイント:
 *   GET  /                静的ファイル public/index.html
 *   GET  /style.css       静的ファイル
 *   GET  /app.js          静的ファイル
 *   POST /api/roll        ガチャ抽選（サーバー側 rand()）。保管はしない。
 *   POST /api/sync        body {"items":[id,...]} で保管庫を上書き ← 脆弱性
 *   GET  /api/inventory   現在の保管庫を返す
 *   POST /api/reset       保管庫をクリア
 *
 * 保管庫は常駐メモリのみ。シングルユーザー想定。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_REQ      16384
#define MAX_INV      1024
#define ITEM_COUNT   12

typedef struct {
    int         id;
    const char *name;
    int         rarity;   /* 0=N, 1=R, 2=SR, 3=SSR */
} Item;

static const Item item_pool[ITEM_COUNT] = {
    { 0, "スライムのしっぽ",     0 },
    { 1, "こんぼう",             0 },
    { 2, "やくそう",             0 },
    { 3, "ぬののふく",           0 },
    { 4, "ポーション",           1 },
    { 5, "どうのつるぎ",         1 },
    { 6, "てつのたて",           1 },
    { 7, "まほうのスクロール",   1 },
    { 8, "はがねのつるぎ",       2 },
    { 9, "ミスリルのよろい",     2 },
    {10, "けんじゃのつえ",       2 },
    {11, "エクスカリバー",       3 },
};

/* レアリティ別の重み（合計100） */
static int rarity_weights[4] = { 60, 25, 12, 3 };

/* 保管庫: item id を並べる */
static int inventory[MAX_INV];
static int inventory_len = 0;

static int roll_gacha(void) {
    int r = rand() % 100;
    int rarity = 0;
    int acc = 0;
    for (int i = 0; i < 4; i++) {
        acc += rarity_weights[i];
        if (r < acc) { rarity = i; break; }
    }
    int candidates[ITEM_COUNT];
    int n = 0;
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (item_pool[i].rarity == rarity) candidates[n++] = i;
    }
    if (n == 0) return 0;
    return candidates[rand() % n];
}

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

static void send_response(int fd, const char *status,
                          const char *ctype, const char *body, size_t blen) {
    char header[512];
    int hl = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n", status, ctype, blen);
    write_all(fd, header, (size_t)hl);
    if (body && blen > 0) write_all(fd, body, blen);
}

static void send_json(int fd, const char *json) {
    send_response(fd, "200 OK", "application/json; charset=utf-8",
                  json, strlen(json));
}

static void send_404(int fd) {
    send_response(fd, "404 Not Found", "text/plain; charset=utf-8",
                  "Not Found", 9);
}

static void send_400(int fd, const char *msg) {
    send_response(fd, "400 Bad Request", "text/plain; charset=utf-8",
                  msg, strlen(msg));
}

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript; charset=utf-8";
    return "application/octet-stream";
}

static void serve_file(int fd, const char *path) {
    int f = open(path, O_RDONLY);
    if (f < 0) { send_404(fd); return; }
    struct stat st;
    if (fstat(f, &st) < 0) { close(f); send_404(fd); return; }
    char header[512];
    int hl = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n", mime_for(path), (long)st.st_size);
    write_all(fd, header, (size_t)hl);
    char buf[8192];
    ssize_t n;
    while ((n = read(f, buf, sizeof(buf))) > 0) {
        if (write_all(fd, buf, (size_t)n) < 0) break;
    }
    close(f);
}

static void handle_roll(int fd) {
    int id = roll_gacha();
    char json[256];
    snprintf(json, sizeof(json),
        "{\"id\":%d,\"name\":\"%s\",\"rarity\":%d}",
        item_pool[id].id, item_pool[id].name, item_pool[id].rarity);
    fprintf(stderr, "[roll]      → id=%d %s (rarity=%d)\n",
            id, item_pool[id].name, item_pool[id].rarity);
    send_json(fd, json);
}

static void handle_inventory(int fd) {
    char json[16384];
    int off = snprintf(json, sizeof(json), "{\"items\":[");
    for (int i = 0; i < inventory_len; i++) {
        int id = inventory[i];
        if (id < 0 || id >= ITEM_COUNT) continue;
        off += snprintf(json + off, sizeof(json) - off,
            "%s{\"id\":%d,\"name\":\"%s\",\"rarity\":%d}",
            i == 0 ? "" : ",",
            item_pool[id].id, item_pool[id].name, item_pool[id].rarity);
    }
    off += snprintf(json + off, sizeof(json) - off, "]}");
    send_json(fd, json);
}

/* body 例: {"items":[3,1,5,2,3]}  数字だけ抜き出して inventory を上書き */
static void handle_sync(int fd, const char *body, int blen) {
    const char *p = body;
    const char *end = body + blen;

    /* "[" まで進める */
    while (p < end && *p != '[') p++;
    if (p >= end) { send_400(fd, "no items array"); return; }
    p++;

    int new_inv[MAX_INV];
    int new_len = 0;

    while (p < end && *p != ']' && new_len < MAX_INV) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n')) p++;
        if (p >= end || *p == ']') break;
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            int id = (int)strtol(p, (char**)&p, 10);
            if (id >= 0 && id < ITEM_COUNT) {
                new_inv[new_len++] = id;
            }
        } else {
            p++;
        }
    }

    memcpy(inventory, new_inv, sizeof(int) * (size_t)new_len);
    inventory_len = new_len;

    fprintf(stderr, "[sync]      ← %d items 受信して保管庫を上書き\n", new_len);
    send_json(fd, "{\"ok\":true}");
}

static void handle_reset(int fd) {
    inventory_len = 0;
    fprintf(stderr, "[reset]     保管庫をクリア\n");
    send_json(fd, "{\"ok\":true}");
}

static int read_request(int fd, char *buf, int *body_off, int *clen) {
    int total = 0;
    int header_end = -1;

    while (total < MAX_REQ - 1) {
        ssize_t n = read(fd, buf + total, (size_t)(MAX_REQ - 1 - total));
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
    while (total < need && total < MAX_REQ - 1) {
        ssize_t n = read(fd, buf + total, (size_t)(need - total));
        if (n <= 0) break;
        total += (int)n;
    }
    return total;
}

static void handle_request(int fd, const char *buf, int body_off, int clen) {
    char method[16] = {0}, path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);

    if (!strcmp(method, "GET") && !strcmp(path, "/")) {
        serve_file(fd, "public/index.html");
    } else if (!strcmp(method, "GET") && !strcmp(path, "/style.css")) {
        serve_file(fd, "public/style.css");
    } else if (!strcmp(method, "GET") && !strcmp(path, "/app.js")) {
        serve_file(fd, "public/app.js");
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/roll")) {
        handle_roll(fd);
    } else if (!strcmp(method, "GET") && !strcmp(path, "/api/inventory")) {
        handle_inventory(fd);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/sync")) {
        handle_sync(fd, buf + body_off, clen);
    } else if (!strcmp(method, "POST") && !strcmp(path, "/api/reset")) {
        handle_reset(fd);
    } else {
        send_404(fd);
    }
}

int main(int argc, char **argv) {
    int         port = 8080;
    const char *bind_addr = "127.0.0.1";   /* デフォはローカル限定 */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--bind") && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fprintf(stderr,
                "Usage: %s [--port N] [--bind ADDR]\n"
                "  --port N      待受ポート（デフォルト 8080）\n"
                "  --bind ADDR   待受アドレス（デフォルト 127.0.0.1、Docker では 0.0.0.0）\n",
                argv[0]);
            return 0;
        }
    }

    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    signal(SIGPIPE, SIG_IGN);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid bind address: %s\n", bind_addr);
        return 1;
    }
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server, 16) < 0) { perror("listen"); return 1; }

    fprintf(stderr, "[gacha-server] http://%s:%d/\n", bind_addr, port);
    fprintf(stderr, "[gacha-server] PID: %d\n", getpid());

    while (1) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept"); continue;
        }
        char *buf = malloc(MAX_REQ);
        if (!buf) { close(client); continue; }
        int body_off = 0, clen = 0;
        int n = read_request(client, buf, &body_off, &clen);
        if (n > 0) {
            handle_request(client, buf, body_off, clen);
        }
        free(buf);
        close(client);
    }
    return 0;
}
