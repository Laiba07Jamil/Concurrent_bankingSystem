#pragma once
#include <pthread.h>
#include <semaphore.h>
#include <string>
#include <atomic>
#include <ctime>

#define MAX_ACCOUNTS     50
#define MAX_TRANSACTIONS 2000
#define MAX_CONCURRENT   3
#define LOG_FILE         "transactions.log"

#define FRAUD_WITHDRAW_LIMIT   10      
#define FRAUD_WINDOW_SECS      60     
#define FRAUD_UNFREEZE_SECS    15     

enum TxType   { TX_DEPOSIT, TX_WITHDRAW, TX_TRANSFER };
enum TxStatus { TX_SUCCESS, TX_FAIL_FUNDS, TX_FAIL_ACCOUNT,
                TX_FAIL_FROZEN, TX_FAIL_DEADLOCK, TX_ROLLBACK, TX_PENDING };

static inline const char* txStatusStr(TxStatus s) {
    switch(s) {
        case TX_SUCCESS:       return "SUCCESS";
        case TX_FAIL_FUNDS:    return "FAIL:FUNDS";
        case TX_FAIL_FROZEN:   return "FROZEN";
        case TX_FAIL_DEADLOCK: return "DEADLOCK";
        case TX_ROLLBACK:      return "ROLLBACK";
        case TX_FAIL_ACCOUNT:  return "NO ACCOUNT";
        default:               return "FAIL";
    }
}
static inline const char* txTypeStr(TxType t) {
    return (t==TX_DEPOSIT)?"DEPOSIT":(t==TX_WITHDRAW)?"WITHDRAW":"TRANSFER";
}

struct Account {
    long            acc_number;
    std::string     name;
    double          balance;
    bool            is_vip;
    bool            frozen;
    bool            active;
    time_t          frozen_at;       
    pthread_mutex_t mutex;
    sem_t           semaphore;
    int             current_sem_val;
    int  waiting_vips; 
    bool is_busy;      
    pthread_cond_t priority_cond; 
};

struct Transaction {
    int      id;
    TxType   type;
    long     acc_from, acc_to;
    double   amount, balance_before, balance_after;
    TxStatus status;
    unsigned long thread_id;
    time_t   timestamp;
};

// ── Globals (defined in main.cpp) ────────────────────────────────────────────
extern Account          accounts[MAX_ACCOUNTS];
extern int              num_accounts;
extern pthread_rwlock_t accounts_rwlock;

extern Transaction      tx_log[MAX_TRANSACTIONS];
extern int              tx_count;
extern pthread_mutex_t  tx_log_mutex;

extern std::atomic<bool>   g_running;
extern pthread_mutex_t     ui_mutex;
extern long                logged_in_acc;  

// Statistics
extern std::atomic<int>    stat_success;
extern std::atomic<int>    stat_fail;
extern std::atomic<int>    stat_rollback;
extern std::atomic<int>    stat_frozen_count;
extern std::atomic<int>    stat_vip_tx;
extern std::atomic<double> stat_total_volume;
