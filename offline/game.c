#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <locale.h>
#include <wchar.h>
#include <sys/prctl.h>

#define LOG_LINES 4
#define LOG_BYTES 240

typedef struct {
    char name[32];
    int  hp;
    int  max_hp;
    int  atk_min;
    int  atk_max;
} Character;

static Character hero;
static Character boss;
static char      log_buf[LOG_LINES][LOG_BYTES + 1];

static int display_width(const char *s) {
    size_t n = mbstowcs(NULL, s, 0);
    if (n == (size_t)-1) return (int)strlen(s);
    wchar_t *wcs = malloc((n + 1) * sizeof(wchar_t));
    if (!wcs) return (int)strlen(s);
    mbstowcs(wcs, s, n + 1);
    int w = wcswidth(wcs, n);
    free(wcs);
    return w < 0 ? (int)strlen(s) : w;
}

static void log_push(const char *fmt, ...) {
    for (int i = 0; i < LOG_LINES - 1; i++) {
        memcpy(log_buf[i], log_buf[i + 1], LOG_BYTES + 1);
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_buf[LOG_LINES - 1], LOG_BYTES, fmt, ap);
    va_end(ap);
}

static void draw_hp_bar(int y, int x, int hp, int max_hp, int width) {
    if (hp < 0) hp = 0;
    int filled = (max_hp > 0) ? (hp * width) / max_hp : 0;
    if (filled > width) filled = width;
    mvaddch(y, x, '[');
    for (int i = 0; i < width; i++) {
        addch(i < filled ? '#' : '-');
    }
    printw("] %d/%d   ", hp, max_hp);
}

static void draw_screen(void) {
    erase();

    attron(A_BOLD);
    mvprintw(0, 2, "=== 勇者 vs 魔王 ===   (ゲーム PID: %d)", getpid());
    attroff(A_BOLD);
    mvhline(1, 0, '-', COLS);

    /* 勇者パネル */
    mvprintw(3, 2, "%s", hero.name);
    mvprintw(4, 2, "HP: ");
    draw_hp_bar(4, 6, hero.hp, hero.max_hp, 20);
    mvprintw(6, 4, " \\o/ ");
    mvprintw(7, 4, "  |  ");
    mvprintw(8, 4, " / \\ ");

    /* 魔王パネル */
    int bx = COLS / 2 + 2;
    if (bx < 42) bx = 42;
    mvprintw(3, bx, "%s", boss.name);
    mvprintw(4, bx, "HP: ");
    draw_hp_bar(4, bx + 4, boss.hp, boss.max_hp, 20);
    mvprintw(6, bx + 2, "/\\___/\\");
    mvprintw(7, bx + 2, "( >.< )");
    mvprintw(8, bx + 2, " >VVV< ");

    mvhline(10, 0, '-', COLS);
    mvprintw(10, 2, "[ ログ ]");
    for (int i = 0; i < LOG_LINES; i++) {
        mvprintw(11 + i, 2, "%s", log_buf[i]);
    }
    mvhline(11 + LOG_LINES, 0, '-', COLS);

    attron(A_BOLD);
    mvprintw(12 + LOG_LINES, 2, "[A] こうげき    [Q] しゅうりょう");
    attroff(A_BOLD);

    refresh();
}

static int randrange(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + rand() % (hi - lo + 1);
}

static void end_screen(const char *msg, int win) {
    draw_screen();
    int y = LINES / 2;
    int x = (COLS - display_width(msg)) / 2;
    if (win) attron(A_BOLD | A_BLINK);
    else     attron(A_BOLD);
    mvprintw(y, x, "%s", msg);
    if (win) attroff(A_BOLD | A_BLINK);
    else     attroff(A_BOLD);
    const char *hint = "なにかキーをおして しゅうりょう";
    mvprintw(y + 2, (COLS - display_width(hint)) / 2, "%s", hint);
    refresh();
    nodelay(stdscr, FALSE);
    getch();
}

int main(void) {
    setlocale(LC_ALL, "");
    /* 同じ UID の別プロセスから /proc/<pid>/mem を開けるようにする */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    srand((unsigned)time(NULL));

    strncpy(hero.name, "勇者", sizeof(hero.name) - 1);
    hero.hp = hero.max_hp = 100;
    hero.atk_min = 8;
    hero.atk_max = 15;

    strncpy(boss.name, "魔王", sizeof(boss.name) - 1);
    boss.hp = boss.max_hp = 9999;
    boss.atk_min = 25;
    boss.atk_max = 35;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    for (int i = 0; i < LOG_LINES; i++) log_buf[i][0] = '\0';
    log_push("%s が あらわれた！", boss.name);
    log_push("[A] キーで こうげき。");

    while (1) {
        draw_screen();
        int ch = getch();

        if (ch == 'q' || ch == 'Q') break;

        if (ch == 'a' || ch == 'A') {
            int dmg = randrange(hero.atk_min, hero.atk_max);
            boss.hp -= dmg;
            log_push("%s の こうげき！ %s に %d の ダメージ！",
                     hero.name, boss.name, dmg);

            if (boss.hp <= 0) {
                boss.hp = 0;
                end_screen("*** WIN！ ***", 1);
                break;
            }

            int bdmg = randrange(boss.atk_min, boss.atk_max);
            hero.hp -= bdmg;
            log_push("%s の こうげき！ %s に %d の ダメージ！",
                     boss.name, hero.name, bdmg);

            if (hero.hp <= 0) {
                hero.hp = 0;
                end_screen("GameOver", 0);
                break;
            }
        }
    }

    endwin();
    return 0;
}
