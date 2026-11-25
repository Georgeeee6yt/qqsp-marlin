#pragma once
// QQS-Pro: the Wi-Fi module's web page (file list, start a print, status during heat waits). See qqs_wifi_web.cpp.

extern bool qqs_wifi_idle_ctx;          // mks_wifi_input is being fed from idle() (an SD print's heat wait)
bool qqs_wifi_direct(const char *line); // a G-code line from the module answered here: true = handled
void qqs_wifi_task();                   // call from idle()
bool mks_wifi_in_input();               // mks_wifi.cpp
