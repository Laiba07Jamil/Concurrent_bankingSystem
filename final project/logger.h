#pragma once
#include "banking.h"
#include <vector>

struct VerifyResult { long acc; double expected; double actual; };

void logger_init();
void logger_close();
void log_transaction(const Transaction& tx);
std::vector<VerifyResult> verify_balances();

