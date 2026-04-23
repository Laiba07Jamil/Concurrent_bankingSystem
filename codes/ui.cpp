#include "banking.h"
#include <ncurses.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>

// ─── Windows ──────────────────────────────────────────────────────────────────
static WINDOW* win_title   = nullptr;
static WINDOW* win_accounts= nullptr;
static WINDOW* win_txlog   = nullptr;
static WINDOW* win_stats   = nullptr;
static WINDOW* win_status  = nullptr;

static int SCR_ROWS, SCR_COLS;

// ─── Colour pairs ─────────────────────────────────────────────────────────────
#define CP_TITLE    1
#define CP_HEADER   2
#define CP_SUCCESS  3
#define CP_FAIL     4
#define CP_FROZEN   5
#define CP_VIP      6
#define CP_WARN     7
#define CP_NORMAL   8
#define CP_BORDER   9

void ui_init() {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    getmaxyx(stdscr, SCR_ROWS, SCR_COLS);

    start_color();
    use_default_colors();
    init_pair(CP_TITLE,   COLOR_BLACK,  COLOR_CYAN);
    init_pair(CP_HEADER,  COLOR_BLACK,  COLOR_BLUE);
    init_pair(CP_SUCCESS, COLOR_GREEN,  -1);
    init_pair(CP_FAIL,    COLOR_RED,    -1);
    init_pair(CP_FROZEN,  COLOR_WHITE,  COLOR_RED);
    init_pair(CP_VIP,     COLOR_YELLOW, -1);
    init_pair(CP_WARN,    COLOR_YELLOW, -1);
    init_pair(CP_NORMAL,  -1,           -1);
    init_pair(CP_BORDER,  COLOR_CYAN,   -1);

    // ── Layout ─────────────────────────────────────────────────────────────
    // Title bar: 3 rows
    // Left panel (accounts): full height - title - status
    // Right top (tx log): ~60% height
    // Right bottom (stats): ~40% height
    // Status bar: 3 rows at bottom

    int title_h  = 3;
    int status_h = 3;
    int body_h   = SCR_ROWS - title_h - status_h;

    int left_w   = SCR_COLS / 2;
    int right_w  = SCR_COLS - left_w;

    int rtop_h   = body_h * 6 / 10;
    int rbot_h   = body_h - rtop_h;

    win_title    = newwin(title_h,  SCR_COLS,    0,       0);
    win_accounts = newwin(body_h,   left_w,  title_h, 0);
    win_txlog    = newwin(rtop_h,   right_w, title_h, left_w);
    win_stats    = newwin(rbot_h,   right_w, title_h + rtop_h, left_w);
    win_status   = newwin(status_h, SCR_COLS,    SCR_ROWS - status_h, 0);

    refresh();
}

void ui_destroy() {
    delwin(win_title);
    delwin(win_accounts);
    delwin(win_txlog);
    delwin(win_stats);
    delwin(win_status);
    endwin();
}

// ─── Draw title bar ───────────────────────────────────────────────────────────
static void draw_title() {
    werase(win_title);
    wbkgd(win_title, COLOR_PAIR(CP_TITLE) | A_BOLD);
    int w = getmaxx(win_title);

    char timebuf[32];
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_info);

    const char* title = "  Concurrent Banking Transaction System  |  CS2006 – Operating Systems";
    mvwprintw(win_title, 1, 2, "%s", title);
    mvwprintw(win_title, 1, w - 12, "[%s]", timebuf);
    wrefresh(win_title);
}

// ─── Draw accounts panel ──────────────────────────────────────────────────────
static void draw_accounts() {
    werase(win_accounts);
    int h = getmaxy(win_accounts);
    int w = getmaxx(win_accounts);

    wattron(win_accounts, COLOR_PAIR(CP_BORDER));
    box(win_accounts, 0, 0);
    wattroff(win_accounts, COLOR_PAIR(CP_BORDER));

    wattron(win_accounts, COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvwprintw(win_accounts, 0, 2, " ACCOUNTS ");
    wattroff(win_accounts, COLOR_PAIR(CP_HEADER) | A_BOLD);

    // column header
    wattron(win_accounts, A_BOLD | A_UNDERLINE);
    mvwprintw(win_accounts, 1, 2, "%-6s %-12s %10s %-6s %-7s",
              "AccNo", "Name", "Balance", "VIP", "Status");
    wattroff(win_accounts, A_BOLD | A_UNDERLINE);

    pthread_rwlock_rdlock(&accounts_rwlock);
    int row = 2;
    for (int i = 0; i < num_accounts && row < h - 1; i++) {
        Account& a = accounts[i];
        if (!a.active) continue;

        if (a.frozen) {
            wattron(win_accounts, COLOR_PAIR(CP_FROZEN) | A_BOLD);
        } else if (a.is_vip) {
            wattron(win_accounts, COLOR_PAIR(CP_VIP));
        } else {
            wattron(win_accounts, COLOR_PAIR(CP_NORMAL));
        }

        mvwprintw(win_accounts, row, 2, "%-6ld %-12.12s %10.2f %-6s %-7s",
                  a.acc_number,
                  a.name.c_str(),
                  a.balance,
                  a.is_vip ? "★ VIP" : "  -  ",
                  a.frozen ? "FROZEN" : "ACTIVE");

        wattroff(win_accounts, A_BOLD | COLOR_PAIR(CP_FROZEN) | COLOR_PAIR(CP_VIP) | COLOR_PAIR(CP_NORMAL));
        row++;
    }
    pthread_rwlock_unlock(&accounts_rwlock);

    // bar chart of balances (bottom portion)
    int chart_start = row + 1;
    if (chart_start < h - 4) {
        wattron(win_accounts, A_DIM);
        mvwprintw(win_accounts, chart_start, 2, "Balance Chart:");
        wattroff(win_accounts, A_DIM);
        chart_start++;

        double max_bal = 1.0;
        pthread_rwlock_rdlock(&accounts_rwlock);
        for (int i = 0; i < num_accounts; i++)
            if (accounts[i].active && accounts[i].balance > max_bal)
                max_bal = accounts[i].balance;

        int chart_rows = h - chart_start - 1;
        int bar_w      = (w - 4) / (num_accounts > 0 ? num_accounts : 1);
        if (bar_w < 1) bar_w = 1;

        for (int i = 0; i < num_accounts && i * bar_w < w - 4; i++) {
            if (!accounts[i].active) continue;
            int bar_h = (int)((accounts[i].balance / max_bal) * chart_rows);
            for (int r = 0; r < chart_rows; r++) {
                int screen_row = chart_start + chart_rows - 1 - r;
                if (screen_row >= h - 1) continue;
                if (r < bar_h) {
                    wattron(win_accounts, COLOR_PAIR(CP_SUCCESS) | A_REVERSE);
                    mvwprintw(win_accounts, screen_row, 2 + i * bar_w, "%*s", bar_w > 1 ? bar_w - 1 : 1, "");
                    wattroff(win_accounts, COLOR_PAIR(CP_SUCCESS) | A_REVERSE);
                } 
            }
        }
        pthread_rwlock_unlock(&accounts_rwlock);
    }

    wrefresh(win_accounts);
}

// ─── Draw transaction log panel ───────────────────────────────────────────────
static void draw_txlog() {
    werase(win_txlog);
    int h = getmaxy(win_txlog);
    int w = getmaxx(win_txlog);

    wattron(win_txlog, COLOR_PAIR(CP_BORDER));
    box(win_txlog, 0, 0);
    wattroff(win_txlog, COLOR_PAIR(CP_BORDER));

    wattron(win_txlog, COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvwprintw(win_txlog, 0, 2, " TRANSACTION LOG ");
    wattroff(win_txlog, COLOR_PAIR(CP_HEADER) | A_BOLD);

    wattron(win_txlog, A_BOLD | A_UNDERLINE);
    mvwprintw(win_txlog, 1, 2, "%-4s %-8s %6s %10s %-22s",
              "ID", "Type", "Acc", "Amount", "Status");
    wattroff(win_txlog, A_BOLD | A_UNDERLINE);

    pthread_mutex_lock(&tx_log_mutex);
    int display_rows = h - 3;
    int start        = (tx_count > display_rows) ? tx_count - display_rows : 0;
    int row          = 2;
    for (int i = start; i < tx_count && row < h - 1; i++, row++) {
        const Transaction& tx = tx_log[i];

        bool ok = (tx.status == TX_SUCCESS);
        if (ok)          wattron(win_txlog, COLOR_PAIR(CP_SUCCESS));
        else if (tx.status == TX_ROLLBACK) wattron(win_txlog, COLOR_PAIR(CP_WARN));
        else             wattron(win_txlog, COLOR_PAIR(CP_FAIL));

        if (tx.is_vip) wattron(win_txlog, A_BOLD);

        char note_short[20] = "";
        if (!tx.note.empty()) snprintf(note_short, sizeof(note_short), " [%s]", tx.note.c_str());

        mvwprintw(win_txlog, row, 2, "%-4d %-8s %6ld %10.2f %-22s%s",
                  tx.id,
                  txTypeStr(tx.type),
                  tx.acc_from,
                  tx.amount,
                  txStatusStr(tx.status),
                  tx.is_vip ? " ★" : "");

        wattroff(win_txlog, A_BOLD | COLOR_PAIR(CP_SUCCESS) | COLOR_PAIR(CP_FAIL) | COLOR_PAIR(CP_WARN));
    }
    pthread_mutex_unlock(&tx_log_mutex);

    wrefresh(win_txlog);
}

// ─── Draw stats panel ─────────────────────────────────────────────────────────
static void draw_stats() {
    werase(win_stats);
    int w = getmaxx(win_stats);

    wattron(win_stats, COLOR_PAIR(CP_BORDER));
    box(win_stats, 0, 0);
    wattroff(win_stats, COLOR_PAIR(CP_BORDER));

    wattron(win_stats, COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvwprintw(win_stats, 0, 2, " STATISTICS ");
    wattroff(win_stats, COLOR_PAIR(CP_HEADER) | A_BOLD);

    // Tally stats
    int total = 0, success = 0, failed = 0, rollbacks = 0, vip_tx = 0;
    int frozen_acc = 0;
    double total_vol = 0;

    pthread_mutex_lock(&tx_log_mutex);
    for (int i = 0; i < tx_count; i++) {
        total++;
        if (tx_log[i].status == TX_SUCCESS) { success++; total_vol += tx_log[i].amount; }
        else if (tx_log[i].status == TX_ROLLBACK) rollbacks++;
        else failed++;
        if (tx_log[i].is_vip) vip_tx++;
    }
    pthread_mutex_unlock(&tx_log_mutex);

    pthread_rwlock_rdlock(&accounts_rwlock);
    for (int i = 0; i < num_accounts; i++)
        if (accounts[i].active && accounts[i].frozen) frozen_acc++;
    pthread_rwlock_unlock(&accounts_rwlock);

    int r = 1;
    mvwprintw(win_stats, r++, 2, "Total Transactions : %d", total);
    wattron(win_stats, COLOR_PAIR(CP_SUCCESS));
    mvwprintw(win_stats, r++, 2, "Successful         : %d", success);
    wattroff(win_stats, COLOR_PAIR(CP_SUCCESS));
    wattron(win_stats, COLOR_PAIR(CP_FAIL));
    mvwprintw(win_stats, r++, 2, "Failed             : %d", failed);
    wattroff(win_stats, COLOR_PAIR(CP_FAIL));
    wattron(win_stats, COLOR_PAIR(CP_WARN));
    mvwprintw(win_stats, r++, 2, "Rollbacks          : %d", rollbacks);
    mvwprintw(win_stats, r++, 2, "Frozen Accounts    : %d", frozen_acc);
    wattroff(win_stats, COLOR_PAIR(CP_WARN));
    wattron(win_stats, COLOR_PAIR(CP_VIP));
    mvwprintw(win_stats, r++, 2, "VIP Transactions   : %d", vip_tx);
    wattroff(win_stats, COLOR_PAIR(CP_VIP));
    mvwprintw(win_stats, r++, 2, "Total Volume       : $%.2f", total_vol);
    mvwprintw(win_stats, r++, 2, "Active Accounts    : %d", num_accounts);

    wrefresh(win_stats);
}

// ─── Status bar ───────────────────────────────────────────────────────────────
static char status_msg[256] = "System running...";

void ui_set_status(const char* msg) {
    pthread_mutex_lock(&ui_mutex);
    strncpy(status_msg, msg, sizeof(status_msg) - 1);
    pthread_mutex_unlock(&ui_mutex);
}

static void draw_status() {
    werase(win_status);
    wbkgd(win_status, COLOR_PAIR(CP_HEADER));
    pthread_mutex_lock(&ui_mutex);
    mvwprintw(win_status, 1, 2, "STATUS: %s", status_msg);
    pthread_mutex_unlock(&ui_mutex);
    mvwprintw(win_status, 1, getmaxx(win_status) - 20, "Press 'q' to quit");
    wrefresh(win_status);
}

// ─── Full redraw (call from UI thread only) ───────────────────────────────────
void ui_draw_all() {
    draw_title();
    draw_accounts();
    draw_txlog();
    draw_stats();
    draw_status();
}

// ─── Check for quit keypress ──────────────────────────────────────────────────
bool ui_check_quit() {
    int ch = getch();
    return (ch == 'q' || ch == 'Q');
}
