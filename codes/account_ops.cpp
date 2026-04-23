#include "banking.h"
#include <cstring>
#include <ctime>
#include <algorithm>

// ─── Find account index (call with at least rwlock read held) ─────────────────
int find_account(long acc_number) {
    for (int i = 0; i < num_accounts; i++)
        if (accounts[i].active && accounts[i].acc_number == acc_number)
            return i;
    return -1;
}

// ─── Fraud detection: record withdrawal and check threshold ───────────────────
static bool check_fraud(int idx) {
    Account& a = accounts[idx];
    pthread_mutex_lock(&a.fraud_mutex);

    time_t now = time(nullptr);
    // purge old entries outside the window
    a.recent_withdrawals.erase(
        std::remove_if(a.recent_withdrawals.begin(), a.recent_withdrawals.end(),
            [&](time_t t){ return (now - t) > FRAUD_WINDOW_SECS; }),
        a.recent_withdrawals.end());

    a.recent_withdrawals.push_back(now);
    int count = (int)a.recent_withdrawals.size();
    pthread_mutex_unlock(&a.fraud_mutex);

    if (count > FRAUD_WITHDRAW_LIMIT) {
        pthread_mutex_lock(&a.mutex);
        a.frozen = true;
        pthread_mutex_unlock(&a.mutex);
        return true;   // fraud triggered
    }
    return false;
}

// ─── Deposit ──────────────────────────────────────────────────────────────────
TxStatus do_deposit(int idx, double amount, double& bal_before, double& bal_after) {
    Account& a = accounts[idx];
    sem_wait(&a.semaphore);
    pthread_mutex_lock(&a.mutex);

    if (a.frozen) {
        pthread_mutex_unlock(&a.mutex);
        sem_post(&a.semaphore);
        return TX_FAIL_FROZEN;
    }
    bal_before   = a.balance;
    a.balance   += amount;
    bal_after    = a.balance;

    pthread_mutex_unlock(&a.mutex);
    sem_post(&a.semaphore);
    return TX_SUCCESS;
}

// ─── Withdraw ─────────────────────────────────────────────────────────────────
TxStatus do_withdraw(int idx, double amount, double& bal_before, double& bal_after) {
    Account& a = accounts[idx];
    sem_wait(&a.semaphore);
    pthread_mutex_lock(&a.mutex);

    if (a.frozen) {
        pthread_mutex_unlock(&a.mutex);
        sem_post(&a.semaphore);
        return TX_FAIL_FROZEN;
    }
    if (a.balance < amount) {
        pthread_mutex_unlock(&a.mutex);
        sem_post(&a.semaphore);
        return TX_FAIL_FUNDS;
    }
    bal_before   = a.balance;
    a.balance   -= amount;
    bal_after    = a.balance;

    pthread_mutex_unlock(&a.mutex);
    sem_post(&a.semaphore);

    // check fraud after releasing locks
    bool fraud = check_fraud(idx);
    if (fraud) {
        // rollback the withdrawal
        sem_wait(&a.semaphore);
        pthread_mutex_lock(&a.mutex);
        a.balance = bal_before;
        pthread_mutex_unlock(&a.mutex);
        sem_post(&a.semaphore);
        bal_after = bal_before;
        return TX_FAIL_FROZEN;
    }
    return TX_SUCCESS;
}

// ─── Transfer (acquire two locks in account-number order to avoid deadlock) ───
TxStatus do_transfer(int from_idx, int to_idx, double amount,
                     double& from_before, double& from_after) {
    // Always lock lower index first to prevent deadlock
    int first  = (from_idx < to_idx) ? from_idx : to_idx;
    int second = (from_idx < to_idx) ? to_idx   : from_idx;

    sem_wait(&accounts[from_idx].semaphore);
    sem_wait(&accounts[to_idx].semaphore);
    pthread_mutex_lock(&accounts[first].mutex);
    pthread_mutex_lock(&accounts[second].mutex);

    Account& src  = accounts[from_idx];
    Account& dst  = accounts[to_idx];

    TxStatus result;
    if (src.frozen || dst.frozen) {
        result = TX_FAIL_FROZEN;
    } else if (src.balance < amount) {
        result = TX_FAIL_FUNDS;
    } else {
        from_before  = src.balance;
        src.balance -= amount;
        dst.balance += amount;
        from_after   = src.balance;
        result       = TX_SUCCESS;
    }

    pthread_mutex_unlock(&accounts[second].mutex);
    pthread_mutex_unlock(&accounts[first].mutex);
    sem_post(&accounts[to_idx].semaphore);
    sem_post(&accounts[from_idx].semaphore);
    return result;
}

// ─── Rollback a previously committed transaction ──────────────────────────────
void do_rollback(Transaction& tx) {
    pthread_rwlock_rdlock(&accounts_rwlock);
    int idx = find_account(tx.acc_from);
    if (idx >= 0) {
        sem_wait(&accounts[idx].semaphore);
        pthread_mutex_lock(&accounts[idx].mutex);
        accounts[idx].balance = tx.balance_before;
        pthread_mutex_unlock(&accounts[idx].mutex);
        sem_post(&accounts[idx].semaphore);
    }
    // for transfer, restore destination too
    if (tx.type == TX_TRANSFER && tx.acc_to > 0) {
        int tidx = find_account(tx.acc_to);
        if (tidx >= 0) {
            sem_wait(&accounts[tidx].semaphore);
            pthread_mutex_lock(&accounts[tidx].mutex);
            // destination had +amount added, subtract it back
            accounts[tidx].balance -= tx.amount;
            pthread_mutex_unlock(&accounts[tidx].mutex);
            sem_post(&accounts[tidx].semaphore);
        }
    }
    pthread_rwlock_unlock(&accounts_rwlock);
    tx.status = TX_ROLLBACK;
}

// ─── Dynamic account creation ─────────────────────────────────────────────────
// Returns new account number, or -1 on failure
long create_account(const std::string& name, double initial_balance, bool is_vip) {
    pthread_rwlock_wrlock(&accounts_rwlock);
    if (num_accounts >= MAX_ACCOUNTS) {
        pthread_rwlock_unlock(&accounts_rwlock);
        return -1;
    }
    int i = num_accounts++;
    Account& a      = accounts[i];
    a.acc_number    = 1000 + i;
    a.name          = name;
    a.balance       = initial_balance;
    a.is_vip        = is_vip;
    a.frozen        = false;
    a.active        = true;
    a.created_at    = time(nullptr);
    pthread_mutex_init(&a.mutex, nullptr);
    pthread_mutex_init(&a.fraud_mutex, nullptr);
    sem_init(&a.semaphore, 0, MAX_CONCURRENT);
    pthread_rwlock_unlock(&accounts_rwlock);
    return a.acc_number;
}
