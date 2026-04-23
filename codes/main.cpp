/*
 * Concurrent Banking Transaction System
 * CS2006 – Operating Systems, Spring 2026
 * FAST-NUCES Karachi
 *
 * Features implemented:
 *  [Mandatory]
 *   1. Thread-safe account ops with pthread_mutex_t per account
 *   2. Semaphore (sem_t) per account limiting MAX_CONCURRENT threads
 *   3. Persistent transaction log (timestamped, thread ID, status)
 *   4. Balance consistency verifier thread
 *  [Additional]
 *   5. Priority-based VIP transactions (priority queue)
 *   6. Undo/rollback for failed/aborted transactions
 *   7. Dynamic account creation at runtime (rwlock-protected registry)
 *   8. Fraud detection (freeze after N withdrawals/min)
 *   9. ncurses real-time GUI dashboard
 */

#include "banking.h"
#include "account_ops.h"
#include "logger.h"
#include "ui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <pthread.h>
#include <queue>
#include <vector>
#include <string>
#include <functional>
#include <random>

// ─── Global definitions ───────────────────────────────────────────────────────
Account          accounts[MAX_ACCOUNTS];
int              num_accounts = 0;
pthread_rwlock_t accounts_rwlock = PTHREAD_RWLOCK_INITIALIZER;

Transaction      tx_log[MAX_TRANSACTIONS];
int              tx_count = 0;
pthread_mutex_t  tx_log_mutex = PTHREAD_MUTEX_INITIALIZER;

std::atomic<bool> g_running(true);
pthread_mutex_t   ui_mutex = PTHREAD_MUTEX_INITIALIZER;

// ─── Priority transaction dispatcher ─────────────────────────────────────────
static std::priority_queue<TxTask, std::vector<TxTask>, std::greater<TxTask>> pq;
static pthread_mutex_t   pq_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    pq_cond  = PTHREAD_COND_INITIALIZER;
static int               pq_seq   = 0;

static void enqueue_task(bool is_vip, std::function<void()> fn) {
    pthread_mutex_lock(&pq_mutex);
    pq.push({is_vip, pq_seq++, fn});
    pthread_cond_signal(&pq_cond);
    pthread_mutex_unlock(&pq_mutex);
}

// Worker thread pool — dequeues and executes tasks
static void* dispatcher_worker(void*) {
    while (g_running || !pq.empty()) {
        pthread_mutex_lock(&pq_mutex);
        while (pq.empty() && g_running)
            pthread_cond_wait(&pq_cond, &pq_mutex);
        if (pq.empty()) { pthread_mutex_unlock(&pq_mutex); break; }
        TxTask task = pq.top(); pq.pop();
        pthread_mutex_unlock(&pq_mutex);
        task.fn();
    }
    return nullptr;
}

// ─── Build a transaction record ───────────────────────────────────────────────
static int g_tx_id = 0;
static pthread_mutex_t tx_id_mutex = PTHREAD_MUTEX_INITIALIZER;

static Transaction make_tx(TxType type, long acc_from, long acc_to,
                            double amount, bool is_vip) {
    Transaction tx{};
    pthread_mutex_lock(&tx_id_mutex);
    tx.id = ++g_tx_id;
    pthread_mutex_unlock(&tx_id_mutex);
    tx.type      = type;
    tx.acc_from  = acc_from;
    tx.acc_to    = acc_to;
    tx.amount    = amount;
    tx.is_vip    = is_vip;
    tx.thread_id = pthread_self();
    tx.timestamp = time(nullptr);
    tx.status    = TX_PENDING;
    return tx;
}

// ─── Execute a single transaction ─────────────────────────────────────────────
static void execute_tx(TxType type, long acc_from, long acc_to,
                        double amount, bool is_vip) {
    Transaction tx = make_tx(type, acc_from, acc_to, amount, is_vip);

    pthread_rwlock_rdlock(&accounts_rwlock);
    int fi = find_account(acc_from);
    int ti = (type == TX_TRANSFER) ? find_account(acc_to) : -1;

    if (fi < 0) {
        tx.status = TX_FAIL_ACCOUNT;
    } else if (type == TX_TRANSFER && ti < 0) {
        tx.status = TX_FAIL_ACCOUNT;
    } else {
        double bb = 0, ba = 0;
        if      (type == TX_DEPOSIT)  tx.status = do_deposit(fi, amount, bb, ba);
        else if (type == TX_WITHDRAW) tx.status = do_withdraw(fi, amount, bb, ba);
        else                          tx.status = do_transfer(fi, ti, amount, bb, ba);

        tx.balance_before = bb;
        tx.balance_after  = ba;

        // Rollback if balance goes negative due to race (extra safety)
        if (tx.status == TX_SUCCESS && ba < 0) {
            do_rollback(tx);
            tx.note = "auto-rollback: balance<0";
        }
    }
    pthread_rwlock_unlock(&accounts_rwlock);

    log_transaction(tx);

    // Update status bar
    char msg[128];
    snprintf(msg, sizeof(msg), "TxID %d | %s Acc=%ld $%.2f -> %s%s",
             tx.id, txTypeStr(type), acc_from, amount,
             txStatusStr(tx.status), is_vip ? " [VIP]" : "");
    ui_set_status(msg);
}

// ─── Client thread args ───────────────────────────────────────────────────────
struct ClientArgs {
    int    client_id;
    bool   is_vip;
    int    num_ops;
};

static void* client_thread(void* arg) {
    ClientArgs* a = (ClientArgs*)arg;

    std::mt19937 rng(pthread_self() ^ time(nullptr));
    std::uniform_int_distribution<int>    acc_dist(0, num_accounts - 1);
    std::uniform_real_distribution<double> amt_dist(10.0, 500.0);
    std::uniform_int_distribution<int>    op_dist(0, 2);

    for (int i = 0; i < a->num_ops && g_running; i++) {
        pthread_rwlock_rdlock(&accounts_rwlock);
        if (num_accounts == 0) { pthread_rwlock_unlock(&accounts_rwlock); break; }
        long from_acc = accounts[acc_dist(rng) % num_accounts].acc_number;
        long to_acc   = accounts[acc_dist(rng) % num_accounts].acc_number;
        pthread_rwlock_unlock(&accounts_rwlock);

        double   amt  = (int)(amt_dist(rng) * 100) / 100.0;
        TxType   type = (TxType)op_dist(rng);
        bool     vip  = a->is_vip;

        // Capture by value for lambda
        enqueue_task(vip, [from_acc, to_acc, amt, type, vip]() {
            execute_tx(type, from_acc, to_acc, amt, vip);
        });

        // small sleep between ops to simulate timing
        usleep(100000 + (rng() % 200000));  // 100-300ms
    }
    delete a;
    return nullptr;
}

// ─── Background: periodic interest ────────────────────────────────────────────
static void* interest_thread(void*) {
    while (g_running) {
        sleep(INTEREST_INTERVAL);
        if (!g_running) break;

        pthread_rwlock_rdlock(&accounts_rwlock);
        for (int i = 0; i < num_accounts; i++) {
            if (!accounts[i].active || accounts[i].frozen) continue;
            if (accounts[i].balance >= 100.0) {
                sem_wait(&accounts[i].semaphore);
                pthread_mutex_lock(&accounts[i].mutex);
                double old_bal = accounts[i].balance;
                accounts[i].balance *= (1.0 + INTEREST_RATE);
                pthread_mutex_unlock(&accounts[i].mutex);
                sem_post(&accounts[i].semaphore);

                // log as a special deposit
                Transaction tx = make_tx(TX_DEPOSIT, accounts[i].acc_number, 0,
                                          accounts[i].balance - old_bal, false);
                tx.balance_before = old_bal;
                tx.balance_after  = accounts[i].balance;
                tx.status         = TX_SUCCESS;
                tx.note           = "interest";
                log_transaction(tx);
            }
        }
        pthread_rwlock_unlock(&accounts_rwlock);
        ui_set_status("Interest applied to eligible accounts");
    }
    return nullptr;
}

// ─── Background: balance verifier ─────────────────────────────────────────────
static void* verifier_thread(void*) {
    while (g_running) {
        sleep(VERIFIER_INTERVAL);
        if (!g_running) break;
        auto mismatches = verify_balances();
        if (!mismatches.empty()) {
            char msg[256];
            snprintf(msg, sizeof(msg), "⚠ CONSISTENCY ERROR: Acc %ld expected=%.2f actual=%.2f",
                     mismatches[0].acc, mismatches[0].expected, mismatches[0].actual);
            ui_set_status(msg);
        } else {
            ui_set_status("Balance verifier: all accounts consistent ✓");
        }
    }
    return nullptr;
}

// ─── Background: fraud monitor ────────────────────────────────────────────────
static void* fraud_monitor_thread(void*) {
    while (g_running) {
        sleep(3);
        if (!g_running) break;
        pthread_rwlock_rdlock(&accounts_rwlock);
        for (int i = 0; i < num_accounts; i++) {
            if (!accounts[i].active) continue;
            // unfreeze accounts after 30s if fraud flag was set
            if (accounts[i].frozen) {
                time_t now = time(nullptr);
                pthread_mutex_lock(&accounts[i].fraud_mutex);
                bool can_unfreeze = accounts[i].recent_withdrawals.empty() ||
                    (now - accounts[i].recent_withdrawals.back() > 30);
                pthread_mutex_unlock(&accounts[i].fraud_mutex);
                if (can_unfreeze) {
                    pthread_mutex_lock(&accounts[i].mutex);
                    accounts[i].frozen = false;
                    pthread_mutex_unlock(&accounts[i].mutex);
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Account %ld unfrozen after fraud cooldown",
                             accounts[i].acc_number);
                    ui_set_status(msg);
                }
            }
        }
        pthread_rwlock_unlock(&accounts_rwlock);
    }
    return nullptr;
}

// ─── Dynamic account creator thread ───────────────────────────────────────────
// Creates a new account every 8 seconds to demonstrate dynamic creation
static void* account_creator_thread(void*) {
    const char* names[] = {"Zaid", "Hina", "Omar", "Sara", "Ali",
                            "Noor", "Bilal", "Ayesha", "Tariq", "Maryam"};
    int idx = 0;
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> bal_dist(500.0, 5000.0);

    while (g_running && num_accounts < MAX_ACCOUNTS - 1) {
        sleep(8);
        if (!g_running) break;
        double init_bal = (int)(bal_dist(rng) * 100) / 100.0;
        bool vip = (idx % 4 == 0);
        long acc = create_account(names[idx % 10], init_bal, vip);
        if (acc > 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "New account created: %ld (%s) balance=$%.2f%s",
                     acc, names[idx % 10], init_bal, vip ? " [VIP]" : "");
            ui_set_status(msg);
        }
        idx++;
        if (idx >= 5) break; // create 5 extra accounts then stop
    }
    return nullptr;
}

// ─── UI refresh thread ────────────────────────────────────────────────────────
static void* ui_thread(void*) {
    while (g_running) {
        pthread_mutex_lock(&ui_mutex);
        ui_draw_all();
        pthread_mutex_unlock(&ui_mutex);
        if (ui_check_quit()) g_running = false;
        usleep(200000); // 5 fps
    }
    return nullptr;
}

// ─── Seed initial accounts ────────────────────────────────────────────────────
static void seed_accounts() {
    struct { const char* name; double bal; bool vip; } init[] = {
        {"Ahmed",   5000.00, false},
        {"Simal",   3200.50, true },
        {"Laiba",   1500.00, false},
        {"Usman",   8750.25, true },
        {"Fatima",  2100.00, false},
        {"Hassan",  6300.75, false},
        {"Zainab",  4500.00, true },
        {"Kamran",   750.00, false},
    };
    for (auto& r : init)
        create_account(r.name, r.bal, r.vip);
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main() {
    srand((unsigned)time(nullptr));

    // Initialise subsystems
    logger_init();
    seed_accounts();
    ui_init();

    // Spawn background daemons
    pthread_t t_interest, t_verifier, t_fraud, t_creator, t_ui;
    pthread_create(&t_interest, nullptr, interest_thread,       nullptr);
    pthread_create(&t_verifier, nullptr, verifier_thread,       nullptr);
    pthread_create(&t_fraud,    nullptr, fraud_monitor_thread,  nullptr);
    pthread_create(&t_creator,  nullptr, account_creator_thread,nullptr);
    pthread_create(&t_ui,       nullptr, ui_thread,             nullptr);

    // Spawn dispatcher worker pool
    pthread_t workers[MAX_THREADS];
    for (int i = 0; i < MAX_THREADS; i++)
        pthread_create(&workers[i], nullptr, dispatcher_worker, nullptr);

    // Spawn client threads (mix of VIP and regular)
    const int NUM_CLIENTS = 6;
    pthread_t clients[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; i++) {
        ClientArgs* args = new ClientArgs();
        args->client_id = i;
        args->is_vip    = (i % 3 == 0);   // every 3rd client is VIP
        args->num_ops   = 15 + (i * 3);   // varied workloads
        pthread_create(&clients[i], nullptr, client_thread, args);
    }

    // Wait for client threads to finish
    for (int i = 0; i < NUM_CLIENTS; i++)
        pthread_join(clients[i], nullptr);

    // Let transactions drain then signal stop
    sleep(2);
    g_running = false;
    pthread_cond_broadcast(&pq_cond);

    // Join worker pool
    for (int i = 0; i < MAX_THREADS; i++)
        pthread_join(workers[i], nullptr);

    // Join daemons
    pthread_join(t_interest, nullptr);
    pthread_join(t_verifier, nullptr);
    pthread_join(t_fraud,    nullptr);
    pthread_join(t_creator,  nullptr);
    pthread_join(t_ui,       nullptr);

    // Cleanup
    ui_destroy();

    // Print final summary to stdout
    printf("\n=== FINAL ACCOUNT BALANCES ===\n");
    for (int i = 0; i < num_accounts; i++) {
        printf("  Acc %-4ld %-12s  $%10.2f  %s%s\n",
               accounts[i].acc_number,
               accounts[i].name.c_str(),
               accounts[i].balance,
               accounts[i].is_vip ? "[VIP] " : "",
               accounts[i].frozen ? "[FROZEN]" : "");
        pthread_mutex_destroy(&accounts[i].mutex);
        pthread_mutex_destroy(&accounts[i].fraud_mutex);
        sem_destroy(&accounts[i].semaphore);
    }

    printf("Total transactions logged: %d\n", tx_count);
    printf("Log file: %s\n", LOG_FILE);

    logger_close();
    pthread_rwlock_destroy(&accounts_rwlock);
    return 0;
}
