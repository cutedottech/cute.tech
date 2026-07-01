#pragma once

void display_init(void);
void display_show_no_config(void);
void display_show_connecting(const char *ssid);
void display_show_connected(const char *ip);
void display_show_disconnected(int reason);
