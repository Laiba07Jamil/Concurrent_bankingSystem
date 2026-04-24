#include "logger.h"
#include <cstdio>

static FILE*           log_fp     = nullptr;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

void logger_init() {
    pthread_mutex_lock(&file_mutex);
    log_fp = fopen(LOG_FILE, "a");
    if (log_fp) {
        fprintf(log_fp, "=== Banking Session Started ===\n");
        fflush(log_fp);
    }
    pthread_mutex_unlock(&file_mutex);
}

void logger_close() {
    pthread_mutex_lock(&file_mutex);
    if (log_fp) { fclose(log_fp); log_fp = nullptr; }
    pthread_mutex_unlock(&file_mutex);
}

void log_transaction(const Transaction& tx) {
    pthread_mutex_lock(&tx_log_mutex);
    if (tx_count < MAX_TRANSACTIONS)
        tx_log[tx_count++] = tx;
    pthread_mutex_unlock(&tx_log_mutex);

    pthread_mutex_lock(&file_mutex);
    if (log_fp) {
        fprintf(log_fp, "[ID:%d] Acc:%ld Type:%-8s Amt:%8.2f Status:%s\n",
                tx.id, tx.acc_from,
                txTypeStr(tx.type), tx.amount, txStatusStr(tx.status));
        fflush(log_fp);
    }
    pthread_mutex_unlock(&file_mutex);
}

std::vector<VerifyResult> verify_balances() {
    // Placeholder — structural extension point
    return {};
}

