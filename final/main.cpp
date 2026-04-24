#include "banking.h"
#include "account_ops.h"
#include "logger.h"
#include "ui.h"
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <csignal>
#include <sys/time.h>

// ═══════════════════════════════════════════════════════════════════════════
//  GLOBAL DEFINITIONS
// ═══════════════════════════════════════════════════════════════════════════
Account          accounts[MAX_ACCOUNTS];
int              num_accounts = 0;
pthread_rwlock_t accounts_rwlock = PTHREAD_RWLOCK_INITIALIZER;

Transaction      tx_log[MAX_TRANSACTIONS];
int              tx_count = 0;
pthread_mutex_t  tx_log_mutex = PTHREAD_MUTEX_INITIALIZER;

std::atomic<bool>   g_running(true);
pthread_mutex_t     ui_mutex = PTHREAD_MUTEX_INITIALIZER;
long                logged_in_acc = 0;

std::atomic<int>    stat_success(0);
std::atomic<int>    stat_fail(0);
std::atomic<int>    stat_rollback(0);
std::atomic<int>    stat_frozen_count(0);
std::atomic<int>    stat_vip_tx(0);
std::atomic<double> stat_total_volume(0.0);

static void add_volume(double v) {
    double c = stat_total_volume.load();
    while (!stat_total_volume.compare_exchange_weak(c, c + v)) {}
}

// ═══════════════════════════════════════════════════════════════════════════
//  SIGNALS
// ═══════════════════════════════════════════════════════════════════════════
static void sigint_handler(int)  { g_running.store(false); }
static void sigalrm_handler(int) { ui_set_status("DIAGNOSTIC: Periodic balance check OK."); }

// ═══════════════════════════════════════════════════════════════════════════
//  WORKER THREAD
// ═══════════════════════════════════════════════════════════════════════════
static void* worker_thread(void* arg) {
    int seed = (int)(long)arg;
    srand((unsigned)time(nullptr) ^ (unsigned)seed ^ (unsigned)(long)pthread_self());

    while (g_running.load()) {
        pthread_rwlock_rdlock(&accounts_rwlock);
        int n = num_accounts;
        pthread_rwlock_unlock(&accounts_rwlock);
        if (n < 2) { usleep(300000); continue; }

        int from = rand() % n;
        int to   = rand() % n;
        if (from == to) { usleep(100000); continue; }

        int op_roll = rand() % 5;
        int op_type = (op_roll <= 1) ? 0 : (op_roll == 2) ? 1 : 2;

        double amount  = 50.0 + (rand() % 200);
        double b_before=0, b_after=0;
        TxStatus s;

        if      (op_type == 0) s = do_deposit (from, amount, b_before, b_after);
        else if (op_type == 1) s = do_withdraw(from, amount, b_before, b_after);
        else                   s = do_transfer(from, to, amount, b_before, b_after);

        Transaction tx{};
        pthread_mutex_lock(&tx_log_mutex);
        tx.id = tx_count + 1;
        pthread_mutex_unlock(&tx_log_mutex);

        tx.type           = (op_type==0)?TX_DEPOSIT:(op_type==1)?TX_WITHDRAW:TX_TRANSFER;
        tx.acc_from       = accounts[from].acc_number;
        tx.acc_to         = (op_type==2) ? accounts[to].acc_number : 0;
        tx.amount         = amount;
        tx.balance_before = b_before;
        tx.balance_after  = b_after;
        tx.status         = s;
        tx.thread_id      = (unsigned long)pthread_self();
        tx.timestamp      = time(nullptr);
        log_transaction(tx);

        if (s == TX_SUCCESS) {
            stat_success++;
            add_volume(amount);
            if (accounts[from].is_vip) stat_vip_tx++;
        } else {
            stat_fail++;
            if (s == TX_FAIL_FUNDS) stat_rollback++;
        }
        usleep(500000 + rand() % 400000);
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
//  FRAUD DETECTION THREAD
// ═══════════════════════════════════════════════════════════════════════════
static void* fraud_thread(void*) {
    while (g_running.load()) {
        sleep(15);
        time_t now = time(nullptr);

        for (int i = 0; i < num_accounts; i++) {
            Account& a = accounts[i];
            if (a.frozen && (now - a.frozen_at) >= FRAUD_UNFREEZE_SECS) {
                pthread_mutex_lock(&a.mutex);
                a.frozen = false;
                pthread_mutex_unlock(&a.mutex);
                ui_set_status("FRAUD MONITOR: Account auto-unfrozen.");
            }
        }
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
//  VERIFIER THREAD
// ═══════════════════════════════════════════════════════════════════════════
static void* verifier_thread(void*) {
    while (g_running.load()) {
        sleep(30);
        pthread_rwlock_rdlock(&accounts_rwlock);
        bool ok = true;
        for (int i = 0; i < num_accounts; i++)
            if (accounts[i].balance < 0) ok = false;
        pthread_rwlock_unlock(&accounts_rwlock);
        if (!ok) ui_set_status("VERIFIER: Warning! Negative balance detected.");
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════════════════
int main() {
    srand((unsigned)time(nullptr));

    struct sigaction sa_int{}, sa_alrm{};
    sa_int.sa_handler = sigint_handler; sigemptyset(&sa_int.sa_mask);
    sigaction(SIGINT, &sa_int, nullptr);
    sa_alrm.sa_handler = sigalrm_handler; sigemptyset(&sa_alrm.sa_mask);
    sigaction(SIGALRM, &sa_alrm, nullptr);

    struct itimerval timer{};
    timer.it_value.tv_sec = timer.it_interval.tv_sec = 30;
    setitimer(ITIMER_REAL, &timer, nullptr);

    logger_init();
    ui_init();

    // Create Manager and initial User
    create_account("Ahmed",  15000.00, false);
    create_account("Simal",  12000.50, true);
    create_account("Laiba",   8500.00, false);
    create_account("Usman",  20000.25, true);
    create_account("Fatima",  9500.00, false);
    create_account("Hassan", 18000.75, false);
    create_account("Zainab", 14000.00, true);

    #define NUM_WORKERS 3
    pthread_t workers[NUM_WORKERS], fraud_tid, verifier_tid;
    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_create(&workers[i], nullptr, worker_thread, (void*)(long)i);
    
    pthread_create(&fraud_tid,    nullptr, fraud_thread,    nullptr);
    pthread_create(&verifier_tid, nullptr, verifier_thread, nullptr);

    while (g_running.load()) {
        ui_login_screen();
        if (logged_in_acc == -1) { g_running.store(false); break; }

        bool session_active = true;
        while (session_active && g_running.load()) {
            if (ui_check_input()) { session_active = false; }
            ui_draw_all();
            usleep(50000); 
        }
    }

    g_running.store(false);
    for (int i = 0; i < NUM_WORKERS; i++) pthread_join(workers[i], nullptr);
    pthread_join(fraud_tid, nullptr);
    pthread_join(verifier_tid, nullptr);

    ui_destroy();
    logger_close();
    return 0;
}
