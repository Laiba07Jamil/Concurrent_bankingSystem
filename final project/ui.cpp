#include "ui.h"
#include "banking.h"
#include "account_ops.h"
#include "logger.h"
#include <ncurses.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

// ── Status message ────────────────────────────────────────────────────────────
char status_msg[256] = "System ready. Welcome!";

// ── Window handles ────────────────────────────────────────────────────────────
static WINDOW *win_header   = nullptr;
static WINDOW *win_accounts = nullptr;
static WINDOW *win_txlog    = nullptr;
static WINDOW *win_stats    = nullptr;
static WINDOW *win_status   = nullptr;

// ── Colour pairs ──────────────────────────────────────────────────────────────
#define CP_HEADER   1
#define CP_BORDER   2
#define CP_SEM_OK   3
#define CP_SEM_BUSY 4
#define CP_VIP      5
#define CP_SUCCESS  6
#define CP_FAIL     7
#define CP_ROLLBACK 8
#define CP_CHART    9
#define CP_FROZEN  10
#define CP_MGRINF  11

// ─────────────────────────────────────────────────────────────────────────────
void ui_init() {
    initscr();
    start_color();
    use_default_colors();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(120);

    init_pair(CP_HEADER,  COLOR_BLACK,   COLOR_CYAN);
    init_pair(CP_BORDER,  COLOR_CYAN,    COLOR_BLACK);
    init_pair(CP_SEM_OK,  COLOR_GREEN,   COLOR_BLACK);
    init_pair(CP_SEM_BUSY,COLOR_RED,     COLOR_BLACK);
    init_pair(CP_VIP,     COLOR_YELLOW,  COLOR_BLACK);
    init_pair(CP_SUCCESS, COLOR_GREEN,   COLOR_BLACK);
    init_pair(CP_FAIL,    COLOR_RED,     COLOR_BLACK);
    init_pair(CP_ROLLBACK,COLOR_MAGENTA, COLOR_BLACK);
    init_pair(CP_CHART,   COLOR_GREEN,   COLOR_BLACK);
    init_pair(CP_FROZEN,  COLOR_WHITE,   COLOR_RED);
    init_pair(CP_MGRINF,  COLOR_CYAN,    COLOR_BLACK);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    int left_w  = cols / 2;
    int right_w = cols - left_w;
    int mid_h   = rows - 3;
    int log_h   = mid_h * 6 / 10;
    int stat_h  = mid_h - log_h;

    win_header   = newwin(1,      cols,    0,       0);
    win_accounts = newwin(mid_h,  left_w,  1,       0);
    win_txlog    = newwin(log_h,  right_w, 1,       left_w);
    win_stats    = newwin(stat_h, right_w, 1+log_h, left_w);
    win_status   = newwin(2,      cols,    rows-2,  0);

    refresh();
}

void ui_destroy() {
    if (win_header)   delwin(win_header);
    if (win_accounts) delwin(win_accounts);
    if (win_txlog)    delwin(win_txlog);
    if (win_stats)    delwin(win_stats);
    if (win_status)   delwin(win_status);
    endwin();
}

void ui_set_status(const char* msg) {
    pthread_mutex_lock(&ui_mutex);
    snprintf(status_msg, sizeof(status_msg), "%s", msg);
    pthread_mutex_unlock(&ui_mutex);
}

// ─────────────────────────────────────────────────────────────────────────────
static void draw_box_title(WINDOW* w, const char* title) {
    wattron(w, COLOR_PAIR(CP_BORDER) | A_BOLD);
    box(w, 0, 0);
    mvwprintw(w, 0, 2, " %s ", title);
    wattroff(w, COLOR_PAIR(CP_BORDER) | A_BOLD);
}

static void draw_header() {
    int cols = getmaxx(win_header);
    wattron(win_header, COLOR_PAIR(CP_HEADER) | A_BOLD);
    for (int x = 0; x < cols; x++) mvwaddch(win_header, 0, x, ' ');
    const char* title = "Concurrent Banking Transaction System  |  CS2006  Operating Systems";
    int tx = (cols - (int)strlen(title)) / 2;
    if (tx < 0) tx = 0;
    mvwprintw(win_header, 0, tx, "%s", title);
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    char clk[32];
    strftime(clk, sizeof(clk), "[%H:%M:%S]", tm_info);
    mvwprintw(win_header, 0, cols - (int)strlen(clk) - 1, "%s", clk);
    wattroff(win_header, COLOR_PAIR(CP_HEADER) | A_BOLD);
    wrefresh(win_header);
}

static void draw_accounts() {
    int rows = getmaxy(win_accounts);
    int cols = getmaxx(win_accounts);
    werase(win_accounts);

    // Title shows role
    char acct_title[64];
    if (logged_in_acc == 0)
        snprintf(acct_title, sizeof(acct_title), "ACCOUNTS  [MANAGER VIEW]");
    else
        snprintf(acct_title, sizeof(acct_title), "ACCOUNTS  [Logged in: %ld]", logged_in_acc);
    draw_box_title(win_accounts, acct_title);

    wattron(win_accounts, A_BOLD | A_UNDERLINE);
    mvwprintw(win_accounts, 1, 2, "%-6s %-10s %10s %-5s %-8s %s",
              "AccNo","Name","Balance","VIP","Status","Sem");
    wattroff(win_accounts, A_BOLD | A_UNDERLINE);

    pthread_rwlock_rdlock(&accounts_rwlock);

    double max_bal = 1.0;
    for (int i = 0; i < num_accounts; i++)
        if (accounts[i].balance > max_bal) max_bal = accounts[i].balance;

    for (int i = 0; i < num_accounts && i < rows - 10; i++) {
        Account& a = accounts[i];
        sem_getvalue(&a.semaphore, &a.current_sem_val);
        bool saturated = (a.current_sem_val <= 0);
        bool is_mine   = (logged_in_acc != 0 && a.acc_number == logged_in_acc);

        // Highlight current user's row
        if (a.frozen) {
            wattron(win_accounts, COLOR_PAIR(CP_FROZEN) | A_BOLD);
        } else if (is_mine) {
            wattron(win_accounts, COLOR_PAIR(CP_VIP) | A_BOLD | A_REVERSE);
        } else if (saturated) {
            wattron(win_accounts, COLOR_PAIR(CP_SEM_BUSY) | A_BOLD);
        } else if (a.is_vip) {
            wattron(win_accounts, COLOR_PAIR(CP_VIP));
        } else {
            wattron(win_accounts, COLOR_PAIR(CP_SEM_OK));
        }

        const char* vip_str    = a.is_vip  ? "VIP" : "-";
        const char* status_str = a.frozen   ? "FROZEN"
                               : saturated  ? "BUSY"
                               : is_mine    ? "<<YOU>>"
                                            : "ACTIVE";
        mvwprintw(win_accounts, 2 + i, 2,
                  "%-6ld %-10s %10.2f %-5s %-8s %d/%d",
                  a.acc_number, a.name.substr(0,10).c_str(),
                  a.balance, vip_str, status_str,
                  a.current_sem_val, MAX_CONCURRENT);

        wattroff(win_accounts,
                 COLOR_PAIR(CP_FROZEN)|COLOR_PAIR(CP_SEM_BUSY)|
                 COLOR_PAIR(CP_VIP)|COLOR_PAIR(CP_SEM_OK)|
                 A_BOLD|A_REVERSE|A_UNDERLINE);
    }

    // Bar chart
    int chart_top = num_accounts + 4;
    if (chart_top < rows - 4) {
        wattron(win_accounts, A_BOLD);
        mvwprintw(win_accounts, chart_top, 2, "Balance Chart:");
        wattroff(win_accounts, A_BOLD);
        int bar_max = cols - 14;
        for (int i = 0; i < num_accounts && (chart_top + 1 + i) < rows - 1; i++) {
            int bar_len = (int)((accounts[i].balance / max_bal) * bar_max);
            if (bar_len < 1 && accounts[i].balance > 0) bar_len = 1;
            if (accounts[i].frozen)
                wattron(win_accounts, COLOR_PAIR(CP_FAIL));
            else if (accounts[i].is_vip)
                wattron(win_accounts, COLOR_PAIR(CP_VIP));
            else
                wattron(win_accounts, COLOR_PAIR(CP_CHART));
            mvwprintw(win_accounts, chart_top + 1 + i, 2, "%-5s ",
                      accounts[i].name.substr(0,5).c_str());
            for (int b = 0; b < bar_len && b < bar_max; b++)
                waddch(win_accounts, ACS_CKBOARD);
            wattroff(win_accounts,
                     COLOR_PAIR(CP_FAIL)|COLOR_PAIR(CP_VIP)|COLOR_PAIR(CP_CHART));
        }
    }

    pthread_rwlock_unlock(&accounts_rwlock);
    wrefresh(win_accounts);
}

static void draw_txlog() {
    int rows = getmaxy(win_txlog);
    werase(win_txlog);
    draw_box_title(win_txlog, "TRANSACTION LOG");

    wattron(win_txlog, A_BOLD | A_UNDERLINE);
    mvwprintw(win_txlog, 1, 2, "%-4s %-9s %-6s %9s %-11s TID",
              "ID","Type","Acc","Amount","Status");
    wattroff(win_txlog, A_BOLD | A_UNDERLINE);

    pthread_mutex_lock(&tx_log_mutex);
    int visible = rows - 4;
    int start   = (tx_count > visible) ? tx_count - visible : 0;
    for (int i = start; i < tx_count; i++) {
        const Transaction& tx = tx_log[i];
        int row = 2 + (i - start);

        if (tx.status == TX_SUCCESS)
            wattron(win_txlog, COLOR_PAIR(CP_SUCCESS));
        else if (tx.status == TX_ROLLBACK)
            wattron(win_txlog, COLOR_PAIR(CP_ROLLBACK));
        else if (tx.status == TX_FAIL_FROZEN)
            wattron(win_txlog, COLOR_PAIR(CP_FROZEN));
        else
            wattron(win_txlog, COLOR_PAIR(CP_FAIL));

        // Find account name for display
        const char* acc_name = "?";
        for (int j = 0; j < num_accounts; j++)
            if (accounts[j].acc_number == tx.acc_from) { acc_name = accounts[j].name.c_str(); break; }

        mvwprintw(win_txlog, row, 2, "%-4d %-9s %-6s %9.2f %-11s %06lu",
                  tx.id, txTypeStr(tx.type),
                  acc_name,
                  tx.amount,
                  txStatusStr(tx.status),
                  tx.thread_id % 1000000UL);

        wattroff(win_txlog,
                 COLOR_PAIR(CP_SUCCESS)|COLOR_PAIR(CP_ROLLBACK)|
                 COLOR_PAIR(CP_FAIL)|COLOR_PAIR(CP_FROZEN));
    }
    pthread_mutex_unlock(&tx_log_mutex);
    wrefresh(win_txlog);
}

static void draw_stats() {
    werase(win_stats);
    draw_box_title(win_stats, "STATISTICS");

    int frozen_now = 0;
    pthread_rwlock_rdlock(&accounts_rwlock);
    for (int i = 0; i < num_accounts; i++)
        if (accounts[i].frozen) frozen_now++;
    pthread_rwlock_unlock(&accounts_rwlock);

    mvwprintw(win_stats, 1, 2, "Total Transactions : %d",   tx_count);
    wattron(win_stats, COLOR_PAIR(CP_SUCCESS));
    mvwprintw(win_stats, 2, 2, "Successful         : %d",   stat_success.load());
    wattroff(win_stats, COLOR_PAIR(CP_SUCCESS));
    wattron(win_stats, COLOR_PAIR(CP_FAIL));
    mvwprintw(win_stats, 3, 2, "Failed             : %d",   stat_fail.load());
    wattroff(win_stats, COLOR_PAIR(CP_FAIL));
    wattron(win_stats, COLOR_PAIR(CP_ROLLBACK));
    mvwprintw(win_stats, 4, 2, "Rollbacks          : %d",   stat_rollback.load());
    wattroff(win_stats, COLOR_PAIR(CP_ROLLBACK));
    wattron(win_stats, COLOR_PAIR(CP_FAIL));
    mvwprintw(win_stats, 5, 2, "Frozen Accounts    : %d / %d", frozen_now, num_accounts);
    wattroff(win_stats, COLOR_PAIR(CP_FAIL));
    wattron(win_stats, COLOR_PAIR(CP_VIP));
    mvwprintw(win_stats, 6, 2, "VIP Transactions   : %d",   stat_vip_tx.load());
    wattroff(win_stats, COLOR_PAIR(CP_VIP));
    mvwprintw(win_stats, 7, 2, "Total Volume       : $%.2f", stat_total_volume.load());

    // Role indicator
    wattron(win_stats, COLOR_PAIR(CP_MGRINF) | A_BOLD);
    if (logged_in_acc == 0)
        mvwprintw(win_stats, 9, 2, "Role: MANAGER  [Full Access]");
    else {
        int idx = find_account(logged_in_acc);
        const char* uname = (idx >= 0) ? accounts[idx].name.c_str() : "?";
        mvwprintw(win_stats, 9, 2, "Role: USER  [%s / Acc %ld]", uname, logged_in_acc);
    }
    wattroff(win_stats, COLOR_PAIR(CP_MGRINF) | A_BOLD);

    wrefresh(win_stats);
}

static void draw_status() {
    werase(win_status);
    wattron(win_status, COLOR_PAIR(CP_BORDER));
    box(win_status, 0, 0);
    wattroff(win_status, COLOR_PAIR(CP_BORDER));

    mvwprintw(win_status, 1, 2, "Status: %.85s", status_msg);

    int cols = getmaxx(win_status);
    char hint[128];
    if (logged_in_acc == 0)
        snprintf(hint, sizeof(hint), "[N]ew Acc  [T]ransfer  [W]ithdraw  [D]eposit  [Q]uit  | MANAGER");
    else
        snprintf(hint, sizeof(hint), "[T]ransfer  [W]ithdraw  [D]eposit  [Q]uit  | USER");
    int hlen = (int)strlen(hint);
    int hx   = cols - hlen - 2;
    if (hx < 1) hx = 1;
    mvwprintw(win_status, 1, hx, "%s", hint);
    wrefresh(win_status);
}

void ui_draw_all() {
    draw_header();
    draw_accounts();
    draw_txlog();
    draw_stats();
    draw_status();
}

// ═══════════════════════════════════════════════════════════════════════════
//  POPUP HELPERS
// ═══════════════════════════════════════════════════════════════════════════
static void popup_begin() { timeout(-1); echo(); curs_set(1); }
static void popup_end()   { noecho(); curs_set(0); timeout(120); clear(); refresh(); }

// Helper: log a manual transaction and update stats
static void record_manual_tx(TxType type, long from, long to,
                              double amount, double bb, double ba, TxStatus s) {
    Transaction tx{};
    pthread_mutex_lock(&tx_log_mutex);
    tx.id = tx_count + 1;
    pthread_mutex_unlock(&tx_log_mutex);
    tx.type           = type;
    tx.acc_from       = from;
    tx.acc_to         = to;
    tx.amount         = amount;
    tx.balance_before = bb;
    tx.balance_after  = ba;
    tx.status         = s;
    tx.thread_id      = (unsigned long)pthread_self();
    tx.timestamp      = time(nullptr);
    log_transaction(tx);

    if (s == TX_SUCCESS) {
        stat_success++;
        double c = stat_total_volume.load();
        while (!stat_total_volume.compare_exchange_weak(c, c + amount)) {}
    } else if (s == TX_FAIL_FUNDS) {
        stat_fail++; stat_rollback++;
    } else {
        stat_fail++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shared result display helper
static void show_result(WINDOW* pw, int result_row, TxStatus s,
                        const char* ok_msg, const char* fail_msg,
                        double bb=0, double ba=0) {
    werase(pw);
    int ww = getmaxx(pw), wh = getmaxy(pw);
    wattron(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    box(pw, 0, 0);
    mvwprintw(pw, 0, 2, " Result ");
    wattroff(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);

    if (s == TX_SUCCESS) {
        wattron(pw, COLOR_PAIR(CP_SUCCESS)|A_BOLD);
        mvwprintw(pw, result_row, 2, "SUCCESS: %s", ok_msg);
        wattroff(pw, COLOR_PAIR(CP_SUCCESS)|A_BOLD);
        mvwprintw(pw, result_row+1, 2, "Before: $%.2f   After: $%.2f", bb, ba);
    } else if (s == TX_FAIL_FUNDS) {
        wattron(pw, COLOR_PAIR(CP_ROLLBACK)|A_BOLD);
        mvwprintw(pw, result_row, 2, "ROLLBACK: Insufficient funds.");
        mvwprintw(pw, result_row+1, 2, "Balance unchanged: $%.2f", bb);
        wattroff(pw, COLOR_PAIR(CP_ROLLBACK)|A_BOLD);
    } else if (s == TX_FAIL_FROZEN) {
        wattron(pw, COLOR_PAIR(CP_FROZEN)|A_BOLD);
        mvwprintw(pw, result_row, 2, "FAIL: Account is FROZEN (fraud flag).");
        wattroff(pw, COLOR_PAIR(CP_FROZEN)|A_BOLD);
    } else {
        wattron(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, result_row, 2, "FAIL: %s", fail_msg);
        wattroff(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
    }
    mvwprintw(pw, wh-2, 2, "Press any key to continue...");
    (void)ww;
    wrefresh(pw);
    timeout(-1); wgetch(pw); timeout(120);
}

// ─────────────────────────────────────────────────────────────────────────────
//  LOGIN SCREEN
// ─────────────────────────────────────────────────────────────────────────────
void ui_login_screen() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    clear(); refresh();

    int wh=12, ww=56, wy=(rows-wh)/2, wx=(cols-ww)/2;
    WINDOW* lw = newwin(wh, ww, wy, wx);
    keypad(lw, TRUE);

    wattron(lw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    box(lw, 0, 0);
    wattroff(lw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    wattron(lw, COLOR_PAIR(CP_HEADER)|A_BOLD);
    mvwprintw(lw, 1, (ww-27)/2, "  BANK LOGIN SYSTEM  ");
    wattroff(lw, COLOR_PAIR(CP_HEADER)|A_BOLD);

    mvwprintw(lw, 3, 2, "Enter your Account Number to log in.");
    mvwprintw(lw, 4, 2, "Enter 0 for Manager / Observer view.");
    mvwprintw(lw, 5, 2, " ");
    wattron(lw, COLOR_PAIR(CP_MGRINF));
    mvwprintw(lw, 6, 2, "  Manager (0)  : N, T, W, D, Q  [full access]");
    mvwprintw(lw, 7, 2, "  User (AccNo) : T, W, D, Q     [own acc only]");
    wattroff(lw, COLOR_PAIR(CP_MGRINF));
    mvwprintw(lw, 9, 2, "Account No: ");
    wrefresh(lw);

    popup_begin();
    char input[24] = {};
    mvwgetnstr(lw, 9, 14, input, 20);
    popup_end();

    logged_in_acc = atol(input);
    delwin(lw);

    // Set initial status
    if (logged_in_acc == 0) {
        ui_set_status("Manager logged in — full access: N, T, W, D, Q");
    } else {
        char msg[128];
        int idx = find_account(logged_in_acc);
        if (idx >= 0)
            snprintf(msg, sizeof(msg), "Logged in as %s (Acc %ld) — T, W, D, Q on your account",
                     accounts[idx].name.c_str(), logged_in_acc);
        else
            snprintf(msg, sizeof(msg), "Acc %ld not found — defaulting to manager view", logged_in_acc);
        ui_set_status(msg);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  NEW ACCOUNT POPUP  (Manager only — key N)
// ─────────────────────────────────────────────────────────────────────────────
void ui_new_account_popup() {
    if (logged_in_acc != 0) {
        ui_set_status("ACCESS DENIED: Only Manager can create accounts. Log in with 0.");
        return;
    }
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int wh=12, ww=52, wy=(rows-wh)/2, wx=(cols-ww)/2;

    WINDOW* pw = newwin(wh, ww, wy, wx);
    keypad(pw, TRUE);
    wattron(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    box(pw, 0, 0);
    wattroff(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    wattron(pw, A_BOLD);
    mvwprintw(pw, 1, (ww-21)/2, "--- NEW ACCOUNT ---");
    wattroff(pw, A_BOLD);
    mvwprintw(pw, 3, 2, "Name            : ");
    mvwprintw(pw, 4, 2, "Initial Balance : ");
    mvwprintw(pw, 5, 2, "VIP? (y/n)      : ");
    wrefresh(pw);

    popup_begin();
    char name_buf[64]={}, bal_buf[32]={}, vip_buf[4]={};
    mvwgetnstr(pw, 3, 20, name_buf, 60);
    mvwgetnstr(pw, 4, 20, bal_buf,  30);
    mvwgetnstr(pw, 5, 20, vip_buf,   3);
    popup_end();

    double init_bal = (strlen(bal_buf) > 0) ? atof(bal_buf) : 0.0;
    bool   is_vip   = (vip_buf[0]=='y' || vip_buf[0]=='Y');
    long   new_acc  = -1;
    if (strlen(name_buf) > 0)
        new_acc = create_account(std::string(name_buf), init_bal, is_vip);

    werase(pw);
    wattron(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    box(pw, 0, 0);
    mvwprintw(pw, 0, 2, " Result ");
    wattroff(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);

    if (new_acc > 0) {
        wattron(pw, COLOR_PAIR(CP_SUCCESS)|A_BOLD);
        mvwprintw(pw, 2, 2, "Account Created Successfully!");
        wattroff(pw, COLOR_PAIR(CP_SUCCESS)|A_BOLD);
        mvwprintw(pw, 3, 2, "Name    : %s", name_buf);
        mvwprintw(pw, 4, 2, "Acc No  : %ld", new_acc);
        mvwprintw(pw, 5, 2, "Balance : $%.2f", init_bal);
        mvwprintw(pw, 6, 2, "VIP     : %s", is_vip ? "YES" : "NO");
        char msg[128];
        snprintf(msg, sizeof(msg), "Account '%s' created — Acc No: %ld", name_buf, new_acc);
        ui_set_status(msg);
    } else {
        wattron(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, 4, 2, "ERROR: Could not create account.");
        mvwprintw(pw, 5, 2, "(Limit reached or empty name?)");
        wattroff(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        ui_set_status("ERROR: Account creation failed.");
    }
    mvwprintw(pw, 10, 2, "Press any key...");
    wrefresh(pw);
    timeout(-1); wgetch(pw); timeout(120);
    delwin(pw);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DEPOSIT POPUP  (key D)
//  Manager: choose any account.  User: deposits into own account only.
// ─────────────────────────────────────────────────────────────────────────────
void ui_deposit_popup() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int wh=11, ww=52, wy=(rows-wh)/2, wx=(cols-ww)/2;

    WINDOW* pw = newwin(wh, ww, wy, wx);
    keypad(pw, TRUE);
    wattron(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    box(pw, 0, 0);
    wattroff(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    wattron(pw, COLOR_PAIR(CP_SUCCESS)|A_BOLD);
    mvwprintw(pw, 1, (ww-17)/2, "--- DEPOSIT ---");
    wattroff(pw, COLOR_PAIR(CP_SUCCESS)|A_BOLD);

    long target_acc = logged_in_acc;
    char acc_buf[24]={}, amt_buf[32]={};

    if (logged_in_acc == 0) {
        // Manager: ask which account
        mvwprintw(pw, 3, 2, "Account ID  : ");
        mvwprintw(pw, 4, 2, "Amount ($)  : ");
        wrefresh(pw);
        popup_begin();
        mvwgetnstr(pw, 3, 16, acc_buf, 20);
        mvwgetnstr(pw, 4, 16, amt_buf, 30);
        popup_end();
        target_acc = atol(acc_buf);
    } else {
        // User: show their account, only ask amount
        int idx = find_account(logged_in_acc);
        if (idx >= 0) {
            wattron(pw, COLOR_PAIR(CP_VIP));
            mvwprintw(pw, 3, 2, "Account     : %ld (%s)",
                      logged_in_acc, accounts[idx].name.c_str());
            mvwprintw(pw, 4, 2, "Balance     : $%.2f", accounts[idx].balance);
            wattroff(pw, COLOR_PAIR(CP_VIP));
        }
        mvwprintw(pw, 5, 2, "Amount ($)  : ");
        wrefresh(pw);
        popup_begin();
        mvwgetnstr(pw, 5, 16, amt_buf, 30);
        popup_end();
    }

    double amount = atof(amt_buf);
    int idx = find_account(target_acc);

    if (idx < 0) {
        werase(pw); box(pw,0,0);
        wattron(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, 3, 2, "ERROR: Account %ld not found.", target_acc);
        wattroff(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, wh-2, 2, "Press any key...");
        wrefresh(pw);
        timeout(-1); wgetch(pw); timeout(120);
        delwin(pw); return;
    }
    if (amount <= 0) {
        werase(pw); box(pw,0,0);
        wattron(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, 3, 2, "ERROR: Amount must be positive.");
        wattroff(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, wh-2, 2, "Press any key...");
        wrefresh(pw);
        timeout(-1); wgetch(pw); timeout(120);
        delwin(pw); return;
    }

    double bb=0, ba=0;
    TxStatus s = do_deposit(idx, amount, bb, ba);
    record_manual_tx(TX_DEPOSIT, target_acc, 0, amount, bb, ba, s);

    char ok_msg[128], fail_msg[64];
    snprintf(ok_msg,   sizeof(ok_msg),  "Deposited $%.2f to Acc %ld", amount, target_acc);
    snprintf(fail_msg, sizeof(fail_msg), "%s", txStatusStr(s));
    if (s == TX_SUCCESS) ui_set_status(ok_msg);
    else                 ui_set_status(fail_msg);

    show_result(pw, 3, s, ok_msg, fail_msg, bb, ba);
    delwin(pw);
}

// ─────────────────────────────────────────────────────────────────────────────
//  WITHDRAW POPUP  (key W)
// ─────────────────────────────────────────────────────────────────────────────
void ui_withdraw_popup() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int wh=11, ww=52, wy=(rows-wh)/2, wx=(cols-ww)/2;

    WINDOW* pw = newwin(wh, ww, wy, wx);
    keypad(pw, TRUE);
    wattron(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    box(pw, 0, 0);
    wattroff(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    wattron(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
    mvwprintw(pw, 1, (ww-19)/2, "--- WITHDRAW ---");
    wattroff(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);

    long target_acc = logged_in_acc;
    char acc_buf[24]={}, amt_buf[32]={};

    if (logged_in_acc == 0) {
        mvwprintw(pw, 3, 2, "Account ID  : ");
        mvwprintw(pw, 4, 2, "Amount ($)  : ");
        wrefresh(pw);
        popup_begin();
        mvwgetnstr(pw, 3, 16, acc_buf, 20);
        mvwgetnstr(pw, 4, 16, amt_buf, 30);
        popup_end();
        target_acc = atol(acc_buf);
    } else {
        int idx = find_account(logged_in_acc);
        if (idx >= 0) {
            wattron(pw, COLOR_PAIR(CP_VIP));
            mvwprintw(pw, 3, 2, "Account     : %ld (%s)",
                      logged_in_acc, accounts[idx].name.c_str());
            mvwprintw(pw, 4, 2, "Balance     : $%.2f", accounts[idx].balance);
            wattroff(pw, COLOR_PAIR(CP_VIP));
        }
        mvwprintw(pw, 5, 2, "Amount ($)  : ");
        wrefresh(pw);
        popup_begin();
        mvwgetnstr(pw, 5, 16, amt_buf, 30);
        popup_end();
    }

    double amount = atof(amt_buf);
    int idx = find_account(target_acc);

    if (idx < 0) {
        werase(pw); box(pw,0,0);
        wattron(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, 3, 2, "ERROR: Account %ld not found.", target_acc);
        wattroff(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, wh-2, 2, "Press any key...");
        wrefresh(pw);
        timeout(-1); wgetch(pw); timeout(120);
        delwin(pw); return;
    }
    if (amount <= 0) {
        werase(pw); box(pw,0,0);
        wattron(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, 3, 2, "ERROR: Amount must be positive.");
        wattroff(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, wh-2, 2, "Press any key...");
        wrefresh(pw);
        timeout(-1); wgetch(pw); timeout(120);
        delwin(pw); return;
    }

    double bb=0, ba=0;
    TxStatus s = do_withdraw(idx, amount, bb, ba);
    record_manual_tx(TX_WITHDRAW, target_acc, 0, amount, bb, ba, s);

    char ok_msg[128], fail_msg[64];
    snprintf(ok_msg,   sizeof(ok_msg),  "Withdrew $%.2f from Acc %ld", amount, target_acc);
    snprintf(fail_msg, sizeof(fail_msg), "%s", txStatusStr(s));
    if (s == TX_SUCCESS) ui_set_status(ok_msg);
    else                 ui_set_status(fail_msg);

    show_result(pw, 3, s, ok_msg, fail_msg, bb, ba);
    delwin(pw);
}

// ─────────────────────────────────────────────────────────────────────────────
//  TRANSFER POPUP  (key T)
//  Manager: choose from/to freely.  User: from = own account only.
// ─────────────────────────────────────────────────────────────────────────────
void ui_transfer_popup() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int wh=15, ww=70, wy=(rows-wh)/2, wx=(cols-ww)/2;

    WINDOW* pw = newwin(wh, ww, wy, wx);
    keypad(pw, TRUE);
    wattron(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    box(pw, 0, 0);
    wattroff(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    wattron(pw, A_BOLD);
    mvwprintw(pw, 1, (ww-23)/2, "--- MANUAL TRANSFER ---");
    wattroff(pw, A_BOLD);

    char from_buf[24]={}, to_buf[24]={}, amt_buf[32]={};
    long from_id, to_id;

    if (logged_in_acc == 0) {
        // Manager: choose both accounts
        mvwprintw(pw, 3, 2, "From Account ID : ");
        mvwprintw(pw, 4, 2, "To   Account ID : ");
        mvwprintw(pw, 5, 2, "Amount ($)      : ");
        wrefresh(pw);
        popup_begin();
        mvwgetnstr(pw, 3, 20, from_buf, 20);
        mvwgetnstr(pw, 4, 20, to_buf,   20);
        mvwgetnstr(pw, 5, 20, amt_buf,  30);
        popup_end();
        from_id = atol(from_buf);
        to_id   = atol(to_buf);
    } else {
        // User: from = own account
        from_id = logged_in_acc;
        int myidx = find_account(from_id);
        if (myidx >= 0) {
            wattron(pw, COLOR_PAIR(CP_VIP));
            mvwprintw(pw, 3, 2, "From            : %ld (%s)",
                      from_id, accounts[myidx].name.c_str());
            mvwprintw(pw, 4, 2, "Balance         : $%.2f", accounts[myidx].balance);
            wattroff(pw, COLOR_PAIR(CP_VIP));
        }
        mvwprintw(pw, 5, 2, "To Account ID   : ");
        mvwprintw(pw, 6, 2, "Amount ($)      : ");
        wrefresh(pw);
        popup_begin();
        mvwgetnstr(pw, 5, 20, to_buf,  20);
        mvwgetnstr(pw, 6, 20, amt_buf, 30);
        popup_end();
        to_id = atol(to_buf);
    }

    double amount   = atof(amt_buf);
    int from_idx    = find_account(from_id);
    int to_idx      = find_account(to_id);

    werase(pw);
    wattron(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);
    box(pw, 0, 0);
    mvwprintw(pw, 0, 2, " Transfer Result ");
    wattroff(pw, COLOR_PAIR(CP_BORDER)|A_BOLD);

    if (from_idx < 0 || to_idx < 0) {
        wattron(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, 3, 2, "ERROR: Account ID not found.");
        if (num_accounts > 0)
            mvwprintw(pw, 4, 2, "Valid IDs: %ld to %ld",
                      accounts[0].acc_number, accounts[num_accounts-1].acc_number);
        wattroff(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        ui_set_status("TRANSFER ERROR: Invalid account ID.");
        mvwprintw(pw, wh-2, 2, "Press any key...");
        wrefresh(pw);
        timeout(-1); wgetch(pw); timeout(120);
        delwin(pw); return;
    }
    if (amount <= 0) {
        wattron(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, 3, 2, "ERROR: Amount must be positive.");
        wattroff(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        ui_set_status("TRANSFER ERROR: Invalid amount.");
        mvwprintw(pw, wh-2, 2, "Press any key...");
        wrefresh(pw);
        timeout(-1); wgetch(pw); timeout(120);
        delwin(pw); return;
    }

    double bb=0, ba=0;
    TxStatus s = do_transfer(from_idx, to_idx, amount, bb, ba);
    record_manual_tx(TX_TRANSFER, from_id, to_id, amount, bb, ba, s);
    if (accounts[from_idx].is_vip || accounts[to_idx].is_vip) stat_vip_tx++;

    char ok_msg[128], fail_msg[64];
    snprintf(ok_msg,   sizeof(ok_msg),  "$%.2f moved: %ld -> %ld", amount, from_id, to_id);
    snprintf(fail_msg, sizeof(fail_msg), "%s", txStatusStr(s));

    if (s == TX_SUCCESS) {
        wattron(pw, COLOR_PAIR(CP_SUCCESS)|A_BOLD);
        mvwprintw(pw, 2, 2, "SUCCESS");
        wattroff(pw, COLOR_PAIR(CP_SUCCESS)|A_BOLD);
        mvwprintw(pw, 3, 2, "Transferred : $%.2f", amount);
        mvwprintw(pw, 4, 2, "From Acc    : %ld (%s)",
          from_id, accounts[from_idx].name.c_str());
	mvwprintw(pw, 5, 2, "              was $%.2f  ->  now $%.2f", bb, ba);
        mvwprintw(pw, 6, 2, "To Acc      : %ld (%s)",
                  to_id, accounts[to_idx].name.c_str());
        ui_set_status(ok_msg);
    } else if (s == TX_FAIL_FUNDS) {
        wattron(pw, COLOR_PAIR(CP_ROLLBACK)|A_BOLD);
        mvwprintw(pw, 3, 2, "ROLLBACK: Insufficient funds.");
        mvwprintw(pw, 4, 2, "Balance unchanged: $%.2f", bb);
        wattroff(pw, COLOR_PAIR(CP_ROLLBACK)|A_BOLD);
        ui_set_status("ROLLBACK: Insufficient funds — no change made.");
    } else if (s == TX_FAIL_FROZEN) {
        wattron(pw, COLOR_PAIR(CP_FROZEN)|A_BOLD);
        mvwprintw(pw, 3, 2, "FAIL: One or both accounts are FROZEN.");
        mvwprintw(pw, 4, 2, "Fraud flag active — contact manager.");
        wattroff(pw, COLOR_PAIR(CP_FROZEN)|A_BOLD);
        ui_set_status("TRANSFER FAIL: Account frozen.");
    } else {
        wattron(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        mvwprintw(pw, 3, 2, "FAIL: %s", txStatusStr(s));
        wattroff(pw, COLOR_PAIR(CP_FAIL)|A_BOLD);
    }

    mvwprintw(pw, wh-2, 2, "Press any key to continue...");
    wrefresh(pw);
    timeout(-1); wgetch(pw); timeout(120);
    delwin(pw);
}

// ─────────────────────────────────────────────────────────────────────────────
//  INPUT HANDLER
// ─────────────────────────────────────────────────────────────────────────────
bool ui_check_input() {
    int ch = getch();
    if (ch == ERR) return false;

    if (ch == 'q' || ch == 'Q') {
        int rows2, cols2;
        getmaxyx(stdscr, rows2, cols2);
        int wh=6, ww=48, wy=(rows2-wh)/2, wx=(cols2-ww)/2;
        WINDOW* bw = newwin(wh, ww, wy, wx);
        wattron(bw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        box(bw, 0, 0);
        mvwprintw(bw, 1, 2, "LOGGING OFF...");
        mvwprintw(bw, 2, 2, "TERMINATING PROCESS");
        mvwprintw(bw, 3, 2, "Saving logs & joining threads...");
        wattroff(bw, COLOR_PAIR(CP_FAIL)|A_BOLD);
        wrefresh(bw);
        usleep(1200000);
        delwin(bw);
        return true;
    }

    // N — new account (manager only)
    if (ch == 'n' || ch == 'N') {
        ui_new_account_popup();
        touchwin(stdscr); refresh();
    }

    // T — transfer
    if (ch == 't' || ch == 'T') {
        ui_transfer_popup();
        touchwin(stdscr); refresh();
    }

    // D — deposit
    if (ch == 'd' || ch == 'D') {
        ui_deposit_popup();
        touchwin(stdscr); refresh();
    }

    // W — withdraw
    if (ch == 'w' || ch == 'W') {
        ui_withdraw_popup();
        touchwin(stdscr); refresh();
    }

    return false;
}
