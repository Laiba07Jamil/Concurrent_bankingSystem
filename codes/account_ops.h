#pragma once
#include "banking.h"

int      find_account(long acc_number);
TxStatus do_deposit(int idx, double amount, double& bal_before, double& bal_after);
TxStatus do_withdraw(int idx, double amount, double& bal_before, double& bal_after);
TxStatus do_transfer(int from_idx, int to_idx, double amount,
                     double& from_before, double& from_after);
void     do_rollback(Transaction& tx);
long     create_account(const std::string& name, double initial_balance, bool is_vip);
