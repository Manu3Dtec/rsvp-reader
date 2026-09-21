#include "transfer_wifi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "transfer_common.h"
#include "rtc_clock.h"
#include "esp_heap_caps.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char *TAG = "RSVP_WIFI";
constexpr const char *kSsid = "RSVP-Reader";
constexpr const char *kPassword = "reader1234";
constexpr const char *kAddress = "http://192.168.4.1";
constexpr size_t kMaxUpload = 64u * 1024u * 1024u;

httpd_handle_t g_server = nullptr;
bool g_wifi_initialized = false;
bool g_event_loop_ready = false;
bool g_running = false;
bool g_stopping = false;
bool g_english = false;
esp_netif_t *g_ap_netif = nullptr;

std::string url_decode(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int a = hex(in[i + 1]);
            const int b = hex(in[i + 2]);
            if (a >= 0 && b >= 0) {
                out.push_back(static_cast<char>((a << 4) | b));
                i += 2;
                continue;
            }
        }
        if (in[i] == '+') out.push_back(' ');
        else out.push_back(in[i]);
    }
    return out;
}

esp_err_t root_handler(httpd_req_t *req) {
    static const char *de = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>RSVP Reader</title><style>body{font-family:system-ui;background:#09090b;color:#f5f5f7;max-width:720px;margin:40px auto;padding:0 18px}main{background:#18181b;border-radius:22px;padding:24px}h1{margin:0 0 8px}p{color:#a1a1aa}.drop{border:2px dashed #52525b;border-radius:18px;padding:28px;text-align:center;margin:24px 0}button{background:#5e5ce6;color:white;border:0;border-radius:12px;padding:12px 18px;font-size:16px}progress{width:100%;height:18px}#status{margin-top:14px}</style></head><body><main><h1>RSVP Reader</h1><p>EPUB oder TXT direkt auf die microSD übertragen.</p><div class="drop"><input id="file" type="file" accept=".epub,.txt"><p><button onclick="upload()">Datei übertragen</button></p></div><progress id="p" max="100" value="0"></progress><div id="status">Bereit</div></main><script>const n=new Date();fetch('/time?y='+n.getFullYear()+'&m='+(n.getMonth()+1)+'&d='+n.getDate()+'&w='+n.getDay()+'&h='+n.getHours()+'&min='+n.getMinutes()+'&s='+n.getSeconds(),{method:'POST'}).catch(()=>{});function upload(){const f=document.getElementById('file').files[0];if(!f){alert('Bitte Datei auswählen');return;}const x=new XMLHttpRequest();x.open('POST','/upload?name='+encodeURIComponent(f.name));x.upload.onprogress=e=>{if(e.lengthComputable){p.value=Math.round(e.loaded*100/e.total);status.textContent='Übertragung: '+p.value+'%';}};x.onload=()=>{status.textContent=x.responseText||('HTTP '+x.status);if(x.status===200)p.value=100;};x.onerror=()=>status.textContent='Übertragung fehlgeschlagen';x.send(f);}</script></body></html>)HTML";
    static const char *en = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>RSVP Reader</title><style>body{font-family:system-ui;background:#09090b;color:#f5f5f7;max-width:720px;margin:40px auto;padding:0 18px}main{background:#18181b;border-radius:22px;padding:24px}h1{margin:0 0 8px}p{color:#a1a1aa}.drop{border:2px dashed #52525b;border-radius:18px;padding:28px;text-align:center;margin:24px 0}button{background:#5e5ce6;color:white;border:0;border-radius:12px;padding:12px 18px;font-size:16px}progress{width:100%;height:18px}#status{margin-top:14px}</style></head><body><main><h1>RSVP Reader</h1><p>Upload EPUB or TXT directly to the microSD card.</p><div class="drop"><input id="file" type="file" accept=".epub,.txt"><p><button onclick="upload()">Upload file</button></p></div><progress id="p" max="100" value="0"></progress><div id="status">Ready</div></main><script>const n=new Date();fetch('/time?y='+n.getFullYear()+'&m='+(n.getMonth()+1)+'&d='+n.getDate()+'&w='+n.getDay()+'&h='+n.getHours()+'&min='+n.getMinutes()+'&s='+n.getSeconds(),{method:'POST'}).catch(()=>{});function upload(){const f=document.getElementById('file').files[0];if(!f){alert('Choose a file first');return;}const x=new XMLHttpRequest();x.open('POST','/upload?name='+encodeURIComponent(f.name));x.upload.onprogress=e=>{if(e.lengthComputable){p.value=Math.round(e.loaded*100/e.total);status.textContent='Uploading: '+p.value+'%';}};x.onload=()=>{status.textContent=x.responseText||('HTTP '+x.status);if(x.status===200)p.value=100;};x.onerror=()=>status.textContent='Upload failed';x.send(f);}</script></body></html>)HTML";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, g_english ? en : de);
}

esp_err_t upload_handler(httpd_req_t *req) {
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > kMaxUpload) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid file size");
    }

    char query[384] = {0};
    char encoded_name[256] = {0};
    if (httpd_req_get_url_query_len(req) <= 0 ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", encoded_name, sizeof(encoded_name)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
    }

    const std::string name = transfer_sanitize_filename(url_decode(encoded_name));
    if (!transfer_supported_filename(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Only EPUB/TXT supported");
    }
    if (!transfer_lock(2000)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage busy");
    }

    const std::string target = transfer_target_path(name);
    const std::string temp = target + ".uploading";
    FILE *out = std::fopen(temp.c_str(), "wb");
    if (!out) {
        transfer_unlock();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
    }

    transfer_set_status(g_english ? "Uploading..." : "Uebertragung...", 0);
    char buffer[4096];
    int remaining = req->content_len;
    int received_total = 0;
    bool ok = true;
    while (remaining > 0) {
        const int want = std::min<int>(remaining, sizeof(buffer));
        int got = httpd_req_recv(req, buffer, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            ok = false;
            break;
        }
        if (std::fwrite(buffer, 1, static_cast<size_t>(got), out) != static_cast<size_t>(got)) {
            ok = false;
            break;
        }
        remaining -= got;
        received_total += got;
        const int pct = static_cast<int>((static_cast<int64_t>(received_total) * 100) / req->content_len);
        transfer_set_status(g_english ? "Uploading..." : "Uebertragung...", pct);
    }

    if (std::fclose(out) != 0) ok = false;
    if (ok && remaining == 0) {
        std::remove(target.c_str());
        if (std::rename(temp.c_str(), target.c_str()) != 0) ok = false;
    }
    if (!ok) std::remove(temp.c_str());
    transfer_unlock();

    if (!ok) {
        transfer_set_status(g_english ? "Upload failed" : "Uebertragung fehlgeschlagen", -1);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    }

    ESP_LOGI(TAG, "Uploaded: %s (%d bytes)", target.c_str(), received_total);
    transfer_set_status(g_english ? "Upload complete" : "Uebertragung abgeschlossen", 100);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, g_english ? "Upload complete. The book is ready." : "Uebertragung abgeschlossen. Das Buch ist bereit.");
}


int query_int(const char *query, const char *key, int fallback = -1) {
    char value[16] = {};
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) return fallback;
    return std::atoi(value);
}

esp_err_t time_handler(httpd_req_t *req) {
    char query[192] = {};
    if (httpd_req_get_url_query_len(req) <= 0 ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing time");
    }

    const int year = query_int(query, "y");
    const int month = query_int(query, "m");
    const int day = query_int(query, "d");
    const int weekday = query_int(query, "w");
    const int hour = query_int(query, "h");
    const int minute = query_int(query, "min");
    const int second = query_int(query, "s");

    if (!RtcClock::set_local(year, month, day, weekday, hour, minute, second)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "RTC set failed");
    }
    return httpd_resp_sendstr(req, "OK");
}

bool start_http() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    if (httpd_start(&g_server, &config) != ESP_OK) return false;

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = root_handler;
    if (httpd_register_uri_handler(g_server, &root) != ESP_OK) return false;

    httpd_uri_t upload = {};
    upload.uri = "/upload";
    upload.method = HTTP_POST;
    upload.handler = upload_handler;
    if (httpd_register_uri_handler(g_server, &upload) != ESP_OK) return false;

    httpd_uri_t set_time = {};
    set_time.uri = "/time";
    set_time.method = HTTP_POST;
    set_time.handler = time_handler;
    if (httpd_register_uri_handler(g_server, &set_time) != ESP_OK) return false;
    return true;
}
}

namespace WifiTransfer {

bool start(bool english) {
    g_english = english;
    if (g_stopping) {
        transfer_set_status(english ? "Wi-Fi is stopping" : "WLAN wird beendet", -1);
        return false;
    }
    if (g_running) return true;
    transfer_common_init();

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }
    if (!g_event_loop_ready) {
        err = esp_event_loop_create_default();
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) g_event_loop_ready = true;
        else {
            ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
            return false;
        }
    }

    if (!g_wifi_initialized) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
        if (!g_ap_netif) {
            ESP_LOGE(TAG, "Could not create WiFi AP netif");
            return false;
        }
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&init);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
            return false;
        }
        g_wifi_initialized = true;
    }

    wifi_config_t ap = {};
    std::snprintf(reinterpret_cast<char *>(ap.ap.ssid), sizeof(ap.ap.ssid), "%s", kSsid);
    std::snprintf(reinterpret_cast<char *>(ap.ap.password), sizeof(ap.ap.password), "%s", kPassword);
    ap.ap.ssid_len = std::strlen(kSsid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_AP, &ap)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi AP start failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!start_http()) {
        esp_wifi_stop();
        esp_wifi_deinit();
        g_wifi_initialized = false;
        if (g_ap_netif) {
            esp_netif_destroy_default_wifi(g_ap_netif);
            g_ap_netif = nullptr;
        }
        ESP_LOGE(TAG, "HTTP server start failed");
        return false;
    }

    g_running = true;
    transfer_set_status(english ? "Wi-Fi upload ready" : "WLAN-Upload bereit", -1);
    ESP_LOGI(TAG, "WiFi upload ready: SSID=%s address=%s", kSsid, kAddress);
    return true;
}

void stop() {
    if (g_stopping) return;
    g_stopping = true;
    const bool was_running = g_running;
    g_running = false;

    ESP_LOGI(TAG, "Stopping WiFi upload backend");
    ESP_LOGI(TAG, "DMA/internal heap before WiFi teardown: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));

    if (g_server) {
        httpd_stop(g_server);
        g_server = nullptr;
    }

    if (was_running) {
        const esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
        }
    }

    // esp_wifi_stop() alone keeps the Wi-Fi driver and its internal DMA-capable
    // buffers allocated. The AMOLED SPI driver also needs internal/DMA memory.
    // Fully tear Wi-Fi down before allowing a new LVGL screen to be rendered.
    if (g_wifi_initialized) {
        const esp_err_t err = esp_wifi_deinit();
        if (err != ESP_OK) ESP_LOGW(TAG, "esp_wifi_deinit: %s", esp_err_to_name(err));
        g_wifi_initialized = false;
    }

    if (g_ap_netif) {
        esp_netif_destroy_default_wifi(g_ap_netif);
        g_ap_netif = nullptr;
    }

    // Give the system a short scheduling point so freed Wi-Fi blocks are
    // available to the SPI/LCD path before the next full-screen redraw.
    vTaskDelay(pdMS_TO_TICKS(80));

    ESP_LOGI(TAG, "DMA/internal heap after WiFi teardown: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));

    g_stopping = false;
    ESP_LOGI(TAG, "WiFi upload backend stopped");
}

bool running() { return g_running; }
std::string status() { return transfer_get_status(); }
int progress() { return transfer_get_progress(); }
const char *ssid() { return kSsid; }
const char *password() { return kPassword; }
const char *address() { return kAddress; }

}  // namespace WifiTransfer
