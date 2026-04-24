#pragma once

void ui_init();
void ui_destroy();
void ui_draw_all();
void ui_set_status(const char* msg);
bool ui_check_input();          // returns true when user pressed Q
void ui_login_screen();
void ui_transfer_popup();
void ui_new_account_popup();
void ui_deposit_popup();
void ui_withdraw_popup();

extern char status_msg[256];
