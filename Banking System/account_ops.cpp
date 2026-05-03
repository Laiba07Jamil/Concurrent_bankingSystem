#include "account_ops.h"
#include <cstring>
#include<unistd.h>

void acquire_priority_lock(int idx) {
    Account& a = accounts[idx];
    bool is_vip_thread = a.is_vip; 
    pthread_mutex_lock(&a.mutex);
    if (is_vip_thread) a.waiting_vips++;
    while (a.is_busy || (!is_vip_thread && a.waiting_vips > 0)) {
        pthread_cond_wait(&a.priority_cond, &a.mutex);
    }
    if (is_vip_thread) a.waiting_vips--;
    
    a.is_busy = true; 
    usleep(1000000); 
    pthread_mutex_unlock(&a.mutex);
}
void release_priority_lock(int idx) {
    Account& a = accounts[idx];
    pthread_mutex_lock(&a.mutex);
    a.is_busy = false;   
    pthread_cond_broadcast(&a.priority_cond); 
    pthread_mutex_unlock(&a.mutex);
}

int find_account(long acc_number) {
    for (int i = 0; i < num_accounts; i++)
        if (accounts[i].active && accounts[i].acc_number == acc_number)
            return i;
    return -1;
}

TxStatus do_deposit(int idx, double amount, double& bal_before, double& bal_after) {
    Account& a = accounts[idx];
    
    acquire_priority_lock(idx); 
    
    pthread_mutex_lock(&a.mutex);
    if (a.frozen) {
        pthread_mutex_unlock(&a.mutex);
        release_priority_lock(idx);
        return TX_FAIL_FROZEN;
    }
    bal_before  = a.balance;
    a.balance  += amount;
    bal_after   = a.balance;
    pthread_mutex_unlock(&a.mutex);
    
    release_priority_lock(idx); 
    return TX_SUCCESS;
}

TxStatus do_withdraw(int idx, double amount, double& bal_before, double& bal_after) {
    Account& a = accounts[idx];   
    acquire_priority_lock(idx);    
    pthread_mutex_lock(&a.mutex);
    if (a.frozen) {
        pthread_mutex_unlock(&a.mutex);
        release_priority_lock(idx);
        return TX_FAIL_FROZEN;
    }
    if (a.balance < amount) {
        bal_before = a.balance;
        bal_after  = a.balance;
        pthread_mutex_unlock(&a.mutex);
        release_priority_lock(idx);
        return TX_FAIL_FUNDS;
    }
    bal_before  = a.balance;
    a.balance  -= amount;
    bal_after   = a.balance;
    pthread_mutex_unlock(&a.mutex);
    
    release_priority_lock(idx);
    return TX_SUCCESS;
}

TxStatus do_transfer(int from_idx, int to_idx, double amount,
                     double& from_before, double& from_after) {
    if (from_idx == to_idx) return TX_FAIL_ACCOUNT;

    int first  = (from_idx < to_idx) ? from_idx : to_idx;
    int second = (from_idx < to_idx) ? to_idx   : from_idx;

    acquire_priority_lock(first);
    acquire_priority_lock(second);

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
    
    release_priority_lock(second);
    release_priority_lock(first);
    
    return result;
}

// ── do_rollback ──────────────────────────────────────────────────────────────
void do_rollback(Transaction& tx) {
    pthread_rwlock_rdlock(&accounts_rwlock);
    int idx = find_account(tx.acc_from);
    if (idx >= 0) {
        acquire_priority_lock(idx);
        pthread_mutex_lock(&accounts[idx].mutex);
        accounts[idx].balance = tx.balance_before;
        pthread_mutex_unlock(&accounts[idx].mutex);
        release_priority_lock(idx);
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
    
    // Initialize Priority Fields
    a.waiting_vips = 0;
    a.is_busy = false;
    pthread_mutex_init(&a.mutex, nullptr);
    pthread_cond_init(&a.priority_cond, nullptr);
    
    pthread_rwlock_unlock(&accounts_rwlock);
    return a.acc_number;
}
