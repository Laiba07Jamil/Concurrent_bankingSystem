#pragma once
#include "banking.h"
#include <vector>

void logger_init();
void logger_close();
void log_transaction(const Transaction& tx);

struct VerifyResult { long acc; double expected; double actual; };
std::vector<VerifyResult> verify_balances();
