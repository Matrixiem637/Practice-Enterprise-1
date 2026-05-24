#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "driver/gpio.h"
#include "esp_sntp.h"

#include "web.h"   // embedded index.html

// ─── Config ───────────────────────────────────────────────────────────────────
#define WIFI_SSID    "Mati201045"
#define WIFI_PASS    "mati2007"
#define BUTTON_GPIO  GPIO_NUM_0
#define TIMEZONE     "CET-1CEST,M3.5.0,M10.5.0/3"  // België / Europa-Centraal

// ─── Globals ──────────────────────────────────────────────────────────────────
static const char *TAG = "WIFI";
static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t       *s_sta_netif = NULL;

#define WIFI_CONNECTED_BIT BIT0

static bool sequence_running  = false;
static int  sequence_count    = 5;
static int  sequence_delay_ms = 2000;

// FIX 1: Guards zodat init_sntp() en start_webserver() maar 1x worden aangeroepen
static bool sntp_started   = false;
static bool server_started = false;

// ─── URL-decode hulpfunctie (%3A → : enz.) ───────────────────────────────────
static void url_decode(char *dst, const char *src, size_t maxlen)
{
    size_t i = 0;
    while (*src && i < maxlen - 1)
    {
        if (*src == '%' && src[1] && src[2])
        {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else if (*src == '+')
        {
            dst[i++] = ' ';
            src++;
        }
        else
        {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

// ─── Scheduler ────────────────────────────────────────────────────────────────
#define MAX_SCHEDULES 8

typedef struct {
    bool active;
    bool enabled;
    int  hour;
    int  minute;
    bool turn_on;
    bool fired_today;
} Schedule;

static Schedule schedules[MAX_SCHEDULES] = {0};
static bool time_synced = false;

// ─── Prototypes ───────────────────────────────────────────────────────────────
static void wifi_status_task(void *pvParameters);
static void shelly_sequence_task(void *pvParameters);
static void scheduler_task(void *pvParameters);
static void button_task(void *pvParameters);
static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data);
static void wifi_init_sta(void);
static void shelly_set(bool on);
static void start_webserver(void);
static void init_sntp(void);

// ─── HTTP: hoofdpagina ────────────────────────────────────────────────────────
static esp_err_t root_handler(httpd_req_t *req)
{
    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
    return ESP_OK;
}

// ─── HTTP: directe schakelaar ─────────────────────────────────────────────────
static esp_err_t on_handler(httpd_req_t *req)
{
    shelly_set(true);
    httpd_resp_send(req, "ON", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t off_handler(httpd_req_t *req)
{
    shelly_set(false);
    httpd_resp_send(req, "OFF", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ─── HTTP: sequentie ──────────────────────────────────────────────────────────
static esp_err_t start_handler(httpd_req_t *req)
{
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK)
    {
        char param[16];
        if (httpd_query_key_value(buf, "count", param, sizeof(param)) == ESP_OK)
            sequence_count = atoi(param);
        if (httpd_query_key_value(buf, "delay", param, sizeof(param)) == ESP_OK)
        {
            int sec = atoi(param);
            if (sec >= 1) sequence_delay_ms = sec * 1000;
        }
    }
    sequence_running = true;
    ESP_LOGI("SEQ", "Start sequentie: %d x %d ms", sequence_count, sequence_delay_ms);
    httpd_resp_send(req, "STARTED", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    sequence_running = false;
    ESP_LOGI("SEQ", "Sequentie gestopt");
    httpd_resp_send(req, "STOPPED", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ─── HTTP: status JSON ────────────────────────────────────────────────────────
static esp_err_t status_handler(httpd_req_t *req)
{
    char resp[1024];
    int  pos = 0;

    time_t    now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    char time_str[16], date_str[32];
    if (time_synced)
    {
        strftime(time_str, sizeof(time_str), "%H:%M:%S",    &ti);
        strftime(date_str, sizeof(date_str), "%A %d %B %Y", &ti);
    }
    else
    {
        strcpy(time_str, "--:--:--");
        strcpy(date_str, "Tijdssync bezig...");
    }

    pos += snprintf(resp + pos, sizeof(resp) - pos,
                    "{\"time\":\"%s\",\"date\":\"%s\",\"synced\":%s,"
                    "\"schedules\":[",
                    time_str, date_str, time_synced ? "true" : "false");

    bool first = true;
    for (int i = 0; i < MAX_SCHEDULES; i++)
    {
        if (!schedules[i].active) continue;
        pos += snprintf(resp + pos, sizeof(resp) - pos,
                        "%s{\"idx\":%d,\"time\":\"%02d:%02d\","
                        "\"on\":%s,\"enabled\":%s}",
                        first ? "" : ",",
                        i,
                        schedules[i].hour, schedules[i].minute,
                        schedules[i].turn_on ? "true" : "false",
                        schedules[i].enabled ? "true" : "false");
        first = false;
    }

    pos += snprintf(resp + pos, sizeof(resp) - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, pos);
    return ESP_OK;
}

// ─── HTTP: schema toevoegen (/sched/add?time=HH:MM&on=1|0) ───────────────────
static esp_err_t sched_add_handler(httpd_req_t *req)
{
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Geen parameters");
        return ESP_FAIL;
    }

    char tparam_raw[16] = {0}, tparam[16] = {0}, onparam[4] = {0};
    httpd_query_key_value(buf, "time", tparam_raw, sizeof(tparam_raw));
    httpd_query_key_value(buf, "on",   onparam,    sizeof(onparam));

    url_decode(tparam, tparam_raw, sizeof(tparam));

    int hh = 0, mm = 0;
    sscanf(tparam, "%d:%d", &hh, &mm);

    if (hh < 0 || hh > 23 || mm < 0 || mm > 59)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Ongeldige tijd");
        return ESP_FAIL;
    }

    int slot = -1;
    for (int i = 0; i < MAX_SCHEDULES; i++)
        if (!schedules[i].active) { slot = i; break; }

    if (slot < 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Maximum bereikt");
        return ESP_FAIL;
    }

    schedules[slot] = (Schedule){
        .active      = true,
        .enabled     = true,
        .hour        = hh,
        .minute      = mm,
        .turn_on     = (onparam[0] == '1'),
        .fired_today = false,
    };

    ESP_LOGI("SCHED", "Nieuw schema [%d]: %02d:%02d -> %s",
             slot, hh, mm, schedules[slot].turn_on ? "AAN" : "UIT");

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ─── HTTP: schema verwijderen (/sched/del?idx=N) ──────────────────────────────
static esp_err_t sched_del_handler(httpd_req_t *req)
{
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK)
    {
        char param[8];
        if (httpd_query_key_value(buf, "idx", param, sizeof(param)) == ESP_OK)
        {
            int idx = atoi(param);
            if (idx >= 0 && idx < MAX_SCHEDULES)
            {
                memset(&schedules[idx], 0, sizeof(Schedule));
                ESP_LOGI("SCHED", "Schema %d verwijderd", idx);
            }
        }
    }
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ─── HTTP: schema aan/uitzetten (/sched/toggle?idx=N) ────────────────────────
static esp_err_t sched_toggle_handler(httpd_req_t *req)
{
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK)
    {
        char param[8];
        if (httpd_query_key_value(buf, "idx", param, sizeof(param)) == ESP_OK)
        {
            int idx = atoi(param);
            if (idx >= 0 && idx < MAX_SCHEDULES && schedules[idx].active)
            {
                schedules[idx].enabled = !schedules[idx].enabled;
                ESP_LOGI("SCHED", "Schema %d: %s",
                         idx, schedules[idx].enabled ? "ingeschakeld" : "uitgeschakeld");
            }
        }
    }
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ─── Shelly HTTP call ──────────────────────────────────────────────────────────
static void shelly_set(bool on)
{
    char url[128];
    snprintf(url, sizeof(url),
             "http://10.165.226.20/rpc/Switch.Set?id=0&on=%s",
             on ? "true" : "false");

    esp_http_client_config_t config = { .url = url, .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE("SHELLY", "HTTP init mislukt"); return; }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
        ESP_LOGI("SHELLY", "Shelly = %s", on ? "AAN" : "UIT");
    else
        ESP_LOGE("SHELLY", "Fout: %s", esp_err_to_name(err));

    esp_http_client_cleanup(client);
}

// ─── Webserver ─────────────────────────────────────────────────────────────────
static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) return;

    const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET, .handler = root_handler         },
        { .uri = "/on",           .method = HTTP_GET, .handler = on_handler           },
        { .uri = "/off",          .method = HTTP_GET, .handler = off_handler          },
        { .uri = "/start",        .method = HTTP_GET, .handler = start_handler        },
        { .uri = "/stop",         .method = HTTP_GET, .handler = stop_handler         },
        { .uri = "/status",       .method = HTTP_GET, .handler = status_handler       },
        { .uri = "/sched/add",    .method = HTTP_GET, .handler = sched_add_handler    },
        { .uri = "/sched/del",    .method = HTTP_GET, .handler = sched_del_handler    },
        { .uri = "/sched/toggle", .method = HTTP_GET, .handler = sched_toggle_handler },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI("WEB", "Webserver gestart");
}

// ─── SNTP ──────────────────────────────────────────────────────────────────────
// FIX 4: Log de gesynchroniseerde tijd in de callback
static void sntp_sync_cb(struct timeval *tv)
{
    time_synced = true;
    time_t now = tv->tv_sec;
    struct tm ti;
    localtime_r(&now, &ti);
    ESP_LOGI("SNTP", "Gesynchroniseerd: %02d:%02d:%02d %02d/%02d/%04d",
             ti.tm_hour, ti.tm_min, ti.tm_sec,
             ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
}

static void init_sntp(void)
{
    setenv("TZ", TIMEZONE, 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "216.239.35.0");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();

    ESP_LOGI("SNTP", "SNTP gestart, wachten op sync...");

    // FIX 4: Wacht max 10s op eerste sync bij opstart
    int retry = 0;
    while (!time_synced && retry++ < 20)
        vTaskDelay(pdMS_TO_TICKS(500));

    if (time_synced)
        ESP_LOGI("SNTP", "Tijdssync OK bij opstart");
    else
        ESP_LOGW("SNTP", "Tijdssync nog bezig op achtergrond...");
}

// ─── Tasks ─────────────────────────────────────────────────────────────────────
static void wifi_status_task(void *pvParameters)
{
    while (1)
    {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
            ESP_LOGI("STATUS", "Verbonden: %s | RSSI=%d dBm | Kanaal=%d",
                     (char *)ap_info.ssid, ap_info.rssi, ap_info.primary);
        else
            ESP_LOGW("STATUS", "Niet verbonden");

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void shelly_sequence_task(void *pvParameters)
{
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    while (1)
    {
        if (!sequence_running)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        shelly_set(true);
        vTaskDelay(pdMS_TO_TICKS(sequence_delay_ms));
        if (!sequence_running) continue;

        shelly_set(false);
        vTaskDelay(pdMS_TO_TICKS(sequence_delay_ms));

        if (--sequence_count <= 0)
            sequence_running = false;
    }
}

static void scheduler_task(void *pvParameters)
{
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);

    int last_day = -1;

    while (1)
    {
        if (!time_synced)
        {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        time_t    now;
        struct tm ti;
        time(&now);
        localtime_r(&now, &ti);

        if (ti.tm_yday != last_day)
        {
            last_day = ti.tm_yday;
            for (int i = 0; i < MAX_SCHEDULES; i++)
                schedules[i].fired_today = false;
            ESP_LOGI("SCHED", "Nieuwe dag — schema's gereset (%02d/%02d)",
                     ti.tm_mday, ti.tm_mon + 1);
        }

        for (int i = 0; i < MAX_SCHEDULES; i++)
        {
            if (!schedules[i].active)     continue;
            if (!schedules[i].enabled)    continue;
            if (schedules[i].fired_today) continue;

            if (ti.tm_hour == schedules[i].hour &&
                ti.tm_min  == schedules[i].minute)
            {
                ESP_LOGI("SCHED", "Schema %d: %02d:%02d -> %s",
                         i, schedules[i].hour, schedules[i].minute,
                         schedules[i].turn_on ? "AAN" : "UIT");
                shelly_set(schedules[i].turn_on);
                schedules[i].fired_today = true;
            }
        }

        // FIX 3: 10s interval zodat schema's niet gemist worden
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ─── WiFi event handler ────────────────────────────────────────────────────────
static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "WiFi gestart, verbinden...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Verbinding verloren. Reason=%d. Opnieuw...", ev->reason);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // FIX 2: time_synced NIET resetten — systeemklok loopt gewoon door
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP gekregen: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // FIX 1: Guards — initialiseer maar 1x
        if (!sntp_started)   { init_sntp();       sntp_started   = true; }
        if (!server_started) { start_webserver();  server_started = true; }
    }
}

// ─── WiFi init ─────────────────────────────────────────────────────────────────
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_ip_info_t ip_info = {0};
    ip_info.ip.addr      = ESP_IP4TOADDR(10, 165, 226, 50);
    ip_info.gw.addr      = ESP_IP4TOADDR(10, 165, 226, 29);
    ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(s_sta_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_sta_netif, &ip_info));

    esp_netif_dns_info_t dns = {0};
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns));

    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns));

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.sta.ssid,     WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta klaar");
}

// ─── Button task ───────────────────────────────────────────────────────────────
static void button_task(void *pvParameters)
{
    int last_state = 1;
    while (1)
    {
        int state = gpio_get_level(BUTTON_GPIO);
        if (last_state == 1 && state == 0)
        {
            sequence_running = !sequence_running;
            ESP_LOGI("BUTTON", "Sequence %s",
                     sequence_running ? "GESTART" : "GESTOPT");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        last_state = state;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─── app_main ──────────────────────────────────────────────────────────────────
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI("APP", "ESP32 gestart");

    wifi_init_sta();

    xTaskCreate(wifi_status_task,     "wifi_status", 4096, NULL, 5, NULL);
    xTaskCreate(shelly_sequence_task, "shelly_seq",  8192, NULL, 5, NULL);
    xTaskCreate(scheduler_task,       "scheduler",   4096, NULL, 5, NULL);

    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << BUTTON_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
}