#include "banking.h"
#include <cstdio>
#include <cstring>
#include <ctime>

static FILE* log_fp = nullptr;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

void logger_init() {
    pthread_mutex_lock(&file_mutex);
    log_fp = fopen(LOG_FILE, "a");
    if (log_fp) {
        fprintf(log_fp, "\n=== Session started: %ld ===\n", (long)time(nullptr));
        fflush(log_fp);
    }
    pthread_mutex_unlock(&file_mutex);
}

void logger_close() {
    pthread_mutex_lock(&file_mutex);
    if (log_fp) { fclose(log_fp); log_fp = nullptr; }
    pthread_mutex_unlock(&file_mutex);
}

// ─── Append one transaction to in-memory log and persistent file ──────────────
void log_transaction(const Transaction& tx) {
    // 1. In-memory log (guarded)
    pthread_mutex_lock(&tx_log_mutex);
    if (tx_count < MAX_TRANSACTIONS)
        tx_log[tx_count++] = tx;
    pthread_mutex_unlock(&tx_log_mutex);

    // 2. Persistent file (atomic: single fprintf + fflush)
    char timebuf[32];
    struct tm tm_info;
    localtime_r(&tx.timestamp, &tm_info);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);

    pthread_mutex_lock(&file_mutex);
    if (log_fp) {
        fprintf(log_fp,
            "[%s] TxID=%-4d Thread=%-10lu Type=%-8s Acc=%ld "
            "Amount=%10.2f BalBefore=%10.2f BalAfter=%10.2f VIP=%s Status=%s%s\n",
            timebuf,
            tx.id,
            (unsigned long)tx.thread_id,
            txTypeStr(tx.type),
            tx.acc_from,
            tx.amount,
            tx.balance_before,
            tx.balance_after,
            tx.is_vip ? "Y" : "N",
            txStatusStr(tx.status),
            tx.note.empty() ? "" : (" | " + tx.note).c_str()
        );
        fflush(log_fp);
    }
    pthread_mutex_unlock(&file_mutex);
}

// ─── Balance consistency verifier ─────────────────────────────────────────────
// Sums all successful deposits/withdrawals/transfers from in-memory log and
// checks against current account balances. Flags any mismatch.
struct VerifyResult { long acc; double expected; double actual; };

std::vector<VerifyResult> verify_balances() {
    std::vector<VerifyResult> mismatches;

    pthread_rwlock_rdlock(&accounts_rwlock);
    pthread_mutex_lock(&tx_log_mutex);

    for (int i = 0; i < num_accounts; i++) {
        if (!accounts[i].active) continue;
        long acc = accounts[i].acc_number;

        // find initial balance: balance_before of first tx touching this account
        double init_bal = -1;
        for (int j = 0; j < tx_count; j++) {
            const Transaction& tx = tx_log[j];
            if (tx.status == TX_SUCCESS && tx.acc_from == acc) {
                init_bal = tx.balance_before;
                break;
            }
        }
        if (init_bal < 0) continue;  // no transactions yet

        double computed = init_bal;
        for (int j = 0; j < tx_count; j++) {
            const Transaction& tx = tx_log[j];
            if (tx.status != TX_SUCCESS) continue;
            if (tx.acc_from == acc) {
                if (tx.type == TX_DEPOSIT)  computed += tx.amount;
                if (tx.type == TX_WITHDRAW) computed -= tx.amount;
                if (tx.type == TX_TRANSFER) computed -= tx.amount;
            }
            if (tx.type == TX_TRANSFER && tx.acc_to == acc)
                computed += tx.amount;
        }

        double actual = accounts[i].balance;
        if (std::abs(computed - actual) > 0.001)
            mismatches.push_back({acc, computed, actual});
    }

    pthread_mutex_unlock(&tx_log_mutex);
    pthread_rwlock_unlock(&accounts_rwlock);
    return mismatches;
}
