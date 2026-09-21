#pragma once

#include <string>

namespace WifiTransfer {

bool start(bool english);
void stop();
bool running();
std::string status();
int progress();
const char *ssid();
const char *password();
const char *address();

}  // namespace WifiTransfer
