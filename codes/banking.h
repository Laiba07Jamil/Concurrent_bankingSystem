#pragma once
#include <pthread.h>
#include <semaphore.h>
#include <string>
#include <vector>
#include <queue>
#include <functional>
#include <atomic>
#include <ctime>

// ─── Constants ────────────────────────────────────────────────────────────────
#define MAX_ACCOUNTS        50
#define MAX_THREADS         20
#define MAX_TRANSACTIONS    500
#define MAX_CONCURRENT      3        // semaphore limit per account
#define INTEREST_INTERVAL   20       // seconds between interest runs
#define INTEREST_RATE       0.10     // 10%
#define FRAUD_WITHDRAW_LIMIT 5       // withdrawals per minute before freeze
#define FRAUD_WINDOW_SECS   60
#define LOG_FILE            "transactions.log"
#define VERIFIER_INTERVAL   5        // seconds between balance checks

// ─── Transaction types ────────────────────────────────────────────────────────
enum TxType { TX_DEPOSIT, TX_WITHDRAW, TX_TRANSFER };
enum TxStatus { TX_SUCCESS, TX_FAIL_FUNDS, TX_FAIL_ACCOUNT, TX_FAIL_FROZEN,
                TX_FAIL_DEADLOCK, TX_ROLLBACK, TX_PENDING };

static const char* txStatusStr(TxStatus s) {
    switch(s) {
        case TX_SUCCESS:       return "SUCCESS";
        case TX_FAIL_FUNDS:    return "FAIL:INSUFFICIENT_FUNDS";
        case TX_FAIL_ACCOUNT:  return "FAIL:INVALID_ACCOUNT";
        case TX_FAIL_FROZEN:   return "FAIL:ACCOUNT_FROZEN";
        case TX_FAIL_DEADLOCK: return "FAIL:DEADLOCK_ABORTED";
        case TX_ROLLBACK:      return "ROLLBACK";
        case TX_PENDING:       return "PENDING";
    }
    return "UNKNOWN";
}

static const char* txTypeStr(TxType t) {
    switch(t) {
        case TX_DEPOSIT:  return "DEPOSIT";
        case TX_WITHDRAW: return "WITHDRAW";
        case TX_TRANSFER: return "TRANSFER";
    }
    return "UNKNOWN";
}

// ─── Transaction record ───────────────────────────────────────────────────────
struct Transaction {
    int          id;
    TxType       type;
    long         acc_from;
    long         acc_to;      // used for transfer
    double       amount;
    double       balance_before;
    double       balance_after;
    TxStatus     status;
    pthread_t    thread_id;
    time_t       timestamp;
    bool         is_vip;
    std::string  note;        // rollback / fraud note
};

// ─── Bank account ─────────────────────────────────────────────────────────────
struct Account {
    long         acc_number;
    std::string  name;
    double       balance;
    bool         is_vip;
    bool         frozen;
    bool         active;

    pthread_mutex_t  mutex;
    sem_t            semaphore;          // limits concurrent access

    // fraud detection
    std::vector<time_t> recent_withdrawals;
    pthread_mutex_t  fraud_mutex;

    // for dynamic creation
    time_t       created_at;
};

// ─── Priority task for VIP queue ──────────────────────────────────────────────
struct TxTask {
    bool is_vip;
    int  seq;     // arrival order (tie-break)
    std::function<void()> fn;

    bool operator>(const TxTask& o) const {
        if (is_vip != o.is_vip) return !is_vip; // VIP = higher priority
        return seq > o.seq;                       // earlier = higher priority
    }
};

// ─── Shared globals (defined in main.cpp) ────────────────────────────────────
extern Account     accounts[MAX_ACCOUNTS];
extern int         num_accounts;
extern pthread_rwlock_t accounts_rwlock;     // for dynamic account creation

extern Transaction tx_log[MAX_TRANSACTIONS];
extern int         tx_count;
extern pthread_mutex_t tx_log_mutex;

extern std::atomic<bool> g_running;
extern pthread_mutex_t   ui_mutex;          // ncurses is not thread-safe
