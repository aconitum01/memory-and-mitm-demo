#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <locale.h>
#include <inttypes.h>
#include <sys/types.h>

#define MAX_REGIONS 1024

typedef struct {
    uintptr_t start;
    uintptr_t end;
} Region;

static pid_t      target_pid;
static int        mem_fd = -1;
static Region     regions[MAX_REGIONS];
static int        num_regions;

static uintptr_t *candidates;
static size_t     num_candidates;
static size_t     cap_candidates;

static int read_regions(void) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", target_pid);
    FILE *fp = fopen(path, "r");
    if (!fp) { perror("maps を開けません"); return -1; }

    num_regions = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        uintptr_t start, end;
        char perms[8];
        char rest[512] = "";
        int got = sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]",
                         &start, &end, perms, rest);
        if (got < 3) continue;
        if (perms[1] != 'w') continue;
        if (strstr(rest, "[vvar]") || strstr(rest, "[vdso]") ||
            strstr(rest, "[vsyscall]")) continue;

        if (num_regions < MAX_REGIONS) {
            regions[num_regions].start = start;
            regions[num_regions].end   = end;
            num_regions++;
        }
    }
    fclose(fp);
    return 0;
}

static void ensure_cap(size_t n) {
    if (cap_candidates >= n) return;
    size_t newcap = cap_candidates ? cap_candidates * 2 : 4096;
    while (newcap < n) newcap *= 2;
    uintptr_t *p = realloc(candidates, newcap * sizeof(uintptr_t));
    if (!p) { perror("realloc"); exit(1); }
    candidates     = p;
    cap_candidates = newcap;
}

static int read_int(uintptr_t addr, int *out) {
    return pread(mem_fd, out, sizeof(int), (off_t)addr) == sizeof(int) ? 0 : -1;
}

static int write_int(uintptr_t addr, int val) {
    return pwrite(mem_fd, &val, sizeof(int), (off_t)addr) == sizeof(int) ? 0 : -1;
}

static void cmd_scan(int value) {
    num_candidates = 0;
    if (read_regions() < 0) return;
    printf("%d 個の書き込み可能領域から値 %d を検索中 ...\n",
           num_regions, value);

    size_t bufsz = 1 << 20;
    unsigned char *buf = malloc(bufsz);
    if (!buf) { perror("malloc"); return; }

    for (int r = 0; r < num_regions; r++) {
        uintptr_t addr = regions[r].start;
        uintptr_t end  = regions[r].end;
        while (addr < end) {
            size_t chunk = (end - addr) > bufsz ? bufsz : (size_t)(end - addr);
            ssize_t n = pread(mem_fd, buf, chunk, (off_t)addr);
            if (n <= 0) break;
            for (ssize_t i = 0; i + (ssize_t)sizeof(int) <= n; i += sizeof(int)) {
                int v;
                memcpy(&v, buf + i, sizeof(int));
                if (v == value) {
                    ensure_cap(num_candidates + 1);
                    candidates[num_candidates++] = addr + (uintptr_t)i;
                }
            }
            addr += (uintptr_t)n;
        }
    }
    free(buf);
    printf("ヒット: %zu 件\n", num_candidates);
}

static void cmd_next(int value) {
    if (num_candidates == 0) {
        printf("候補がありません。まず 'scan <値>' を実行してください。\n");
        return;
    }
    size_t keep = 0;
    for (size_t i = 0; i < num_candidates; i++) {
        int v;
        if (read_int(candidates[i], &v) == 0 && v == value) {
            candidates[keep++] = candidates[i];
        }
    }
    num_candidates = keep;
    printf("絞り込み結果: %zu 件\n", num_candidates);
}

static void cmd_list(size_t limit) {
    if (num_candidates == 0) { printf("候補なし。\n"); return; }
    size_t n = num_candidates < limit ? num_candidates : limit;
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        read_int(candidates[i], &v);
        printf("  [%zu] 0x%lx = %d\n", i, candidates[i], v);
    }
    if (n < num_candidates)
        printf("  ... 残り %zu 件 ('list <n>' で更に表示)\n",
               num_candidates - n);
}

static void cmd_set(uintptr_t addr, int value) {
    if (write_int(addr, value) == 0) {
        printf("0x%lx に %d を書き込みました\n", addr, value);
    } else {
        perror("pwrite 失敗");
    }
}

static void cmd_setall(int value) {
    size_t ok = 0;
    for (size_t i = 0; i < num_candidates; i++) {
        if (write_int(candidates[i], value) == 0) ok++;
    }
    printf("%zu/%zu 件に %d を書き込みました\n", ok, num_candidates, value);
}

static void cmd_read(uintptr_t addr) {
    int v;
    if (read_int(addr, &v) == 0) printf("0x%lx = %d (0x%x)\n", addr, v, v);
    else perror("pread 失敗");
}

static void cmd_regions(void) {
    read_regions();
    for (int i = 0; i < num_regions; i++) {
        printf("  0x%012lx - 0x%012lx (%lu bytes)\n",
               regions[i].start, regions[i].end,
               regions[i].end - regions[i].start);
    }
    printf("書き込み可能領域: %d 個\n", num_regions);
}

static void print_help(void) {
    puts("操作方法:");
    puts("  scan <値>            書き込み可能領域から int 値を全検索");
    puts("  next <値>            現在の値が一致する候補だけに絞り込む");
    puts("  list [n]             候補を最大 n 件表示 (デフォルト 16)");
    puts("  read <16進アドレス>  指定アドレスの int 値を読む");
    puts("  set  <16進アドレス> <値>   指定アドレスに int 値を書く");
    puts("  setall <値>          現在の全候補に同じ値を書き込む");
    puts("  regions              ターゲットの書き込み可能領域を一覧");
    puts("  reset                候補リストをクリア");
    puts("  help                 このヘルプを表示");
    puts("  quit                 終了");
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (argc < 2) {
        fprintf(stderr, "使い方: %s <pid>\n", argv[0]);
        return 1;
    }
    target_pid = (pid_t)atoi(argv[1]);
    if (target_pid <= 0) { fprintf(stderr, "不正な PID です\n"); return 1; }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", target_pid);
    mem_fd = open(path, O_RDWR);
    if (mem_fd < 0) {
        perror("/proc/<pid>/mem を開けません");
        fprintf(stderr,
                "ヒント: ターゲット側で prctl(PR_SET_PTRACER) を呼ぶか、\n"
                "        /proc/sys/kernel/yama/ptrace_scope を 0 にしてください。\n");
        return 1;
    }

    printf("PID %d にアタッチしました。\n", target_pid);
    print_help();

    char line[256];
    while (1) {
        printf("cheat> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) { putchar('\n'); break; }

        char cmd[32];
        if (sscanf(line, "%31s", cmd) != 1) continue;

        if (!strcmp(cmd, "quit") || !strcmp(cmd, "q") || !strcmp(cmd, "exit")) {
            break;
        } else if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
            print_help();
        } else if (!strcmp(cmd, "scan")) {
            int v;
            if (sscanf(line, "%*s %d", &v) == 1) cmd_scan(v);
            else printf("使い方: scan <値>\n");
        } else if (!strcmp(cmd, "next")) {
            int v;
            if (sscanf(line, "%*s %d", &v) == 1) cmd_next(v);
            else printf("使い方: next <値>\n");
        } else if (!strcmp(cmd, "list")) {
            size_t n = 16;
            sscanf(line, "%*s %zu", &n);
            cmd_list(n);
        } else if (!strcmp(cmd, "read")) {
            uintptr_t a;
            if (sscanf(line, "%*s %lx", &a) == 1) cmd_read(a);
            else printf("使い方: read <16進アドレス>\n");
        } else if (!strcmp(cmd, "set")) {
            uintptr_t a; int v;
            if (sscanf(line, "%*s %lx %d", &a, &v) == 2) cmd_set(a, v);
            else printf("使い方: set <16進アドレス> <値>\n");
        } else if (!strcmp(cmd, "setall")) {
            int v;
            if (sscanf(line, "%*s %d", &v) == 1) cmd_setall(v);
            else printf("使い方: setall <値>\n");
        } else if (!strcmp(cmd, "regions")) {
            cmd_regions();
        } else if (!strcmp(cmd, "reset")) {
            num_candidates = 0;
            printf("候補リストをクリアしました。\n");
        } else {
            printf("不明なコマンド: %s ('help' で一覧)\n", cmd);
        }
    }

    if (mem_fd >= 0) close(mem_fd);
    free(candidates);
    return 0;
}
