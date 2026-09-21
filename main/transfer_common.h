#pragma once

#include <cstddef>
#include <string>

void transfer_common_init();
bool transfer_lock(unsigned timeout_ms = 1000);
void transfer_unlock();

bool transfer_supported_filename(const std::string &name);
std::string transfer_sanitize_filename(const std::string &name);
std::string transfer_target_path(const std::string &name);

void transfer_set_status(const std::string &status, int progress = -1);
std::string transfer_get_status();
int transfer_get_progress();
