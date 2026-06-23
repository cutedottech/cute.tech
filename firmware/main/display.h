#pragma once

void display_init(void);
void display_show_splash(void);
void display_show_needs_setup(void);
void display_show_connecting(const char *ssid);
void display_show_connected(const char *ip);
void display_show_disconnected(int reason);
void display_show_online(const char *device_name);
void display_show_reconnecting(void);
