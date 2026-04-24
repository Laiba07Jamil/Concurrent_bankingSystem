 #include "account_ops.h"
#include <cstring>

// ── find_account ─────────────────────────────────────────────────────────────
int find_account(long acc_number) {
    for (int i = 0; i < num_accounts; i++)
        if (accounts[i].active && accounts[i].acc_number == acc_number)
            return i;
    return -1;
}


// ── do_deposit ───────────────────────────────────────────────────────────────

TxStatus do_deposit(int idx, double amount, double& bal_before, double& bal_after) {
    Account& a = accounts[idx];
    sem_wait(&a.semaphore);
    pthread_mutex_lock(&a.mutex);
    if (a.frozen) {
        pthread_mutex_unlock(&a.mutex);
        sem_post(&a.semaphore);
        return TX_FAIL_FROZEN;
    }
    bal_before  = a.balance;
    a.balance  += amount;
    bal_after   = a.balance;
    pthread_mutex_unlock(&a.mutex);
    sem_post(&a.semaphore);
    return TX_SUCCESS;
}


// ── do_withdraw ──────────────────────────────────────────────────────────────

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
        bal_before = a.balance;
        bal_after  = a.balance;
        pthread_mutex_unlock(&a.mutex);
        sem_post(&a.semaphore);
        return TX_FAIL_FUNDS;
    }
    bal_before  = a.balance;
    a.balance  -= amount;
    bal_after   = a.balance;
    pthread_mutex_unlock(&a.mutex);
    sem_post(&a.semaphore);
    return TX_SUCCESS;

}
// ── do_transfer ──────────────────────────────────────────────────────────────

// Deadlock prevention: always lock lower-index account first.

TxStatus do_transfer(int from_idx, int to_idx, double amount,
                     double& from_before, double& from_after) {
    if (from_idx == to_idx) return TX_FAIL_ACCOUNT;
    int first  = (from_idx < to_idx) ? from_idx : to_idx;
    int second = (from_idx < to_idx) ? to_idx   : from_idx;
    sem_wait(&accounts[first].semaphore);
    sem_wait(&accounts[second].semaphore);
    pthread_mutex_lock(&accounts[first].mutex);
    pthread_mutex_lock(&accounts[second].mutex);
    Account& src = accounts[from_idx];
    Account& dst = accounts[to_idx];
    TxStatus result;
    if (src.frozen || dst.frozen) {
        result = TX_FAIL_FROZEN;
    } else if (src.balance < amount) {
        from_before = src.balance;
        from_after  = src.balance;
        result = TX_FAIL_FUNDS;
    } else {
        from_before  = src.balance;
        src.balance -= amount;
        dst.balance += amount;
        from_after   = src.balance;
        result = TX_SUCCESS;
    }
    pthread_mutex_unlock(&accounts[second].mutex);
    pthread_mutex_unlock(&accounts[first].mutex);
    sem_post(&accounts[second].semaphore);
    sem_post(&accounts[first].semaphore);
    return result;
}


// ── do_rollback ──────────────────────────────────────────────────────────────

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
    pthread_rwlock_unlock(&accounts_rwlock);
    tx.status = TX_ROLLBACK;
}


// ── create_account ───────────────────────────────────────────────────────────

long create_account(const std::string& name, double initial_balance, bool is_vip) {
    pthread_rwlock_wrlock(&accounts_rwlock);
    if (num_accounts >= MAX_ACCOUNTS) {
        pthread_rwlock_unlock(&accounts_rwlock);
        return -1;
    }
    int i      = num_accounts++;
    Account& a = accounts[i];
    a.acc_number      = 1000 + i;
    a.name            = name;
    a.balance         = initial_balance;
    a.is_vip          = is_vip;
    a.frozen          = false;
    a.active          = true;
    a.frozen_at       = 0;
    a.current_sem_val = MAX_CONCURRENT;
    pthread_mutex_init(&a.mutex, nullptr);
    sem_init(&a.semaphore, 0, MAX_CONCURRENT);
    pthread_rwlock_unlock(&accounts_rwlock);
    return a.acc_number;

} 
