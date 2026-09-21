#include "transfer_common.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {
SemaphoreHandle_t g_mutex = nullptr;
SemaphoreHandle_t g_status_mutex = nullptr;
std::string g_status = "Bereit";
int g_progress = -1;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}
}

void transfer_common_init() {
    if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
    if (!g_status_mutex) g_status_mutex = xSemaphoreCreateMutex();
}

bool transfer_lock(unsigned timeout_ms) {
    transfer_common_init();
    return g_mutex && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void transfer_unlock() {
    if (g_mutex) xSemaphoreGive(g_mutex);
}

bool transfer_supported_filename(const std::string &name) {
    const std::string n = lower(name);
    return (n.size() >= 5 && n.compare(n.size() - 5, 5, ".epub") == 0) ||
           (n.size() >= 4 && n.compare(n.size() - 4, 4, ".txt") == 0);
}

std::string transfer_sanitize_filename(const std::string &input) {
    std::string name = input;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);

    std::string out;
    out.reserve(std::min<size_t>(name.size(), 180));
    for (unsigned char c : name) {
        if (out.size() >= 180) break;
        if (c < 32 || c == 127 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty() || out == "." || out == "..") out = "book.epub";
    return out;
}

std::string transfer_target_path(const std::string &name) {
    return std::string(BSP_SD_MOUNT_POINT) + "/" + transfer_sanitize_filename(name);
}

void transfer_set_status(const std::string &status, int progress) {
    transfer_common_init();
    if (g_status_mutex && xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_status = status;
        g_progress = progress;
        xSemaphoreGive(g_status_mutex);
    }
}

std::string transfer_get_status() {
    transfer_common_init();
    std::string result = g_status;
    if (g_status_mutex && xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        result = g_status;
        xSemaphoreGive(g_status_mutex);
    }
    return result;
}

int transfer_get_progress() {
    transfer_common_init();
    int result = g_progress;
    if (g_status_mutex && xSemaphoreTake(g_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        result = g_progress;
        xSemaphoreGive(g_status_mutex);
    }
    return result;
}
