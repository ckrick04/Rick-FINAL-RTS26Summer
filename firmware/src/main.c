
/*
* ============================================================
*  RUN MODE  (serial monitor vs. web monitor)
* ============================================================
*
* USE_WEBSERVER selects the Core-0 observability plane. The Core-1 pipeline is
* identical in both modes.
*
*   USE_WEBSERVER = 0  -> Serial monitor (provided, working). Prints queue depth,
*                         event bits, and heartbeats once a second. No Wi-Fi, so
*                         the pipeline runs in Wokwi out of the box.
*   USE_WEBSERVER = 1  -> Web monitor. Runs webmonitor_task instead — the stub you
*                         fill in with App 1's HTTP server (deliverable #3). Needs
*                         the Wi-Fi REQUIRES already in this folder's CMakeLists.
*
* Start on USE_WEBSERVER=0 to get the pipeline moving in the simulator, then flip
* to 1 once you have implemented the web monitor.
*
* ============================================================
* Theme: Industrial / Theme Park
* ============================================================
*/

#ifndef USE_WEBSERVER
#define USE_WEBSERVER 1
#endif

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#if USE_WEBSERVER
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

#define HTTP_PORT         80

#define WIFI_SSID         "Wokwi-GUEST"
#define WIFI_PASS         ""             /* Wokwi virtual AP is open */
#endif

#define ESTOP_GPIO GPIO_NUM_4
#define BUTTON_GPIO GPIO_NUM_18

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5

static const char *TAG = "FINAL";

// Create enum & instance for displaying over WiFi or serial
static EventGroupHandle_t display_group;
#define SERIAL_DISP_BIT (1 << 0)
#define WIFI_DISP_BIT (1 << 1)

#define MEASURE_WCET(_max_var, _body) do {                       \
    int64_t _t0 = esp_timer_get_time();                          \
    _body;                                                        \
    int64_t _dt = esp_timer_get_time() - _t0;                    \
    if ((uint64_t)_dt > (_max_var)) (_max_var) = (uint64_t)_dt;  \
} while (0)

/* ---------- IPC objects (created in app_main, used everywhere) ---------- */
static QueueHandle_t      data_q;        /* TODO: choose depth + item size for YOUR pipeline */
static EventGroupHandle_t evt_group;
static TaskHandle_t       responder_handle;
static TaskHandle_t       prod_handle; // Suspend producer on e-stop
static SemaphoreHandle_t  estop_sem;

/* Event-group bit definitions */
#define EV_BIT_DATA_PRODUCED  (1 << 0)
#define EV_BIT_DATA_PROCESSED (1 << 1)

/* Per-task heartbeats — proof of life for the monitor. Single 32-bit reads are
 * atomic on Xtensa, so the monitor can read these without a lock (App 6's topic). */
static volatile uint32_t hb_prod, hb_cons, hb_coord, hb_resp;
// Variables for wcet measurements
static volatile uint64_t wcet_prod_max_us, wcet_cons_max_us, wcet_coord_max_us, wcet_resp_max_us, wcet_estop_max_us;
// Variables accessible to web server
static volatile uint8_t r_completed, r_progress;

// Emergency Stop Flag
static volatile bool estop_active;

// Struct definition for producer/consumer
// Gives the percent of the ride that has been completed thus far
typedef struct ride_state {
  uint32_t timestamp_ms;
  int pct_value;
  int ride_count;
} ride_state_t;

/* ---------- Producer task (Core 1) ---------- */
static void producer_task(void *arg)
{
    int tick = 0;
    int rides_completed = 0;
    for (;;) {
        MEASURE_WCET(wcet_prod_max_us, ({
        ride_state_t cur_state = {esp_timer_get_time() / 1000, tick, rides_completed};

        /* TODO: push into queue with a timeout. Decide back-pressure policy. */
        if (xQueueSend(data_q, &cur_state, pdMS_TO_TICKS(10)) != pdTRUE) {
          // queue full — drop the latest value
          ESP_LOGW(TAG, "Queue full - ride unable to move to %d%%", cur_state.pct_value);
        }

        xEventGroupSetBits(evt_group, EV_BIT_DATA_PRODUCED);

        tick++;
        hb_prod++;
        // Use tick as a percentage, reset after 100%
        if (tick > 100) {
          tick = 0;
          rides_completed++;
        }
        }));
        vTaskDelay(pdMS_TO_TICKS(100));   /* 10 Hz producer */
    }
}

/* ---------- Consumer task (Core 1) ---------- */
static void consumer_task(void *arg)
{
    for (;;) {
        // Process ride progress
        ride_state_t cur_state;
        if (xQueueReceive(data_q, &cur_state, portMAX_DELAY) == pdTRUE){
          MEASURE_WCET(wcet_cons_max_us, ({
          r_completed = (uint8_t)cur_state.ride_count;
          r_progress = (uint8_t)cur_state.pct_value;
          ESP_LOGI(TAG, "[consumer] Ride #%d, Completion: %d%%", 
          cur_state.ride_count, cur_state.pct_value);
          }));
        }
        hb_cons++;
        xEventGroupSetBits(evt_group, EV_BIT_DATA_PROCESSED);
    }
}

/* ---------- Coordinator task (Core 1) ----------
 * Waits for BOTH event bits to be set, then signals the responder via direct
 * task notification.
 */
static void coordinator_task(void *arg)
{
    const EventBits_t wait_mask = EV_BIT_DATA_PRODUCED | EV_BIT_DATA_PROCESSED;
    for (;;) {
        EventBits_t got = xEventGroupWaitBits(evt_group, wait_mask,
                                              pdTRUE,   /* clear on exit */
                                              pdTRUE,   /* wait for ALL */
                                              portMAX_DELAY);
        MEASURE_WCET(wcet_coord_max_us, ({
        if ((got & wait_mask) == wait_mask) {
            xTaskNotifyGive(responder_handle);
            hb_coord++;
        }
        }));
    }
}

/* ---------- Responder task (Core 1) ----------
 * Wakes via direct task notification from coordinator OR from button ISR.
 */
static void responder_task(void *arg)
{
    for (;;) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (n == 0) continue;
        MEASURE_WCET(wcet_resp_max_us, ({
        // Check if the ride is halted or not. This can be queried by pressing the Port 18 button
        if (estop_active){
          ESP_LOGI(TAG, "[responder] Ride Status = HALTED");
        }
        else{
          ESP_LOGI(TAG, "[responder] Ride Status = IN_PROGRESS");
        }
        hb_resp++;
        }));
    }
}

/* ---------- E-stop task (Core 1) ----------
 * Stops progression of the ride by halting the producer via vTaskSuspend.
 */
static void estop_task(void *arg)
{
    for (;;){
        if (xSemaphoreTake(estop_sem, portMAX_DELAY) == pdTRUE){
            MEASURE_WCET(wcet_estop_max_us, ({
            estop_active = !estop_active;
            if (estop_active){
              vTaskSuspend(prod_handle);
              ESP_LOGI(TAG, "[EMERGENCY STOP] - Safe State Activated");
            }
            else{
              vTaskResume(prod_handle);
              ESP_LOGI(TAG, "[EMERGENCY STOP] - Ride Resuming...");
            }
            }));
        }
    }
}

/* ---------- Button ISR — notify responder directly ---------- */
static volatile int64_t last_edge_us;
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - last_edge_us < 10000) return;
    last_edge_us = now;

    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(responder_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

static volatile int64_t last_estop_edge_us;
static void IRAM_ATTR estop_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - last_estop_edge_us < 10000) return;
    last_estop_edge_us = now;

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(estop_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

// WCET Monitor
static void task_monitor(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(10000);

    for (;;) {
        printf("\n=== Task Monitor ===\n");
        printf("%-12s %-8s %-9s %-12s %-10s\n",
               "Task", "Period", "Priority", "Heartbeats", "WCET(us)");
        printf("%-12s %-8s %-9d %-12lu %-10llu\n",
               "Producer", "100 ms",  8, (unsigned long)hb_prod, (unsigned long long)wcet_prod_max_us);
        printf("%-12s %-8s %-9d %-12lu %-10llu\n",
               "Consumer", "100 ms",  8, (unsigned long)hb_cons, (unsigned long long)wcet_cons_max_us);
        printf("%-12s %-8s %-9d %-12lu %-10llu\n",
               "Coordinator", "100 ms",   9, (unsigned long)hb_coord, (unsigned long long)wcet_coord_max_us);
        printf("%-12s %-8s %-9d %-12lu %-10llu\n",
               "Responder", "20 ms",  12, (unsigned long)hb_resp, (unsigned long long)wcet_resp_max_us);
        printf("%-12s %-8s %-9d %-12s %-10llu\n",
               "E-STOP", "10 ms",  15, "", (unsigned long long)wcet_estop_max_us);

        vTaskDelayUntil(&last, period);
    }
}

/* ---------- Serial monitor task (Core 0)  [USE_WEBSERVER = 0] ----------
 * Provided and working. Prints the same state the web monitor will show, so the
 * pipeline is observable in Wokwi with no Wi-Fi. This is your baseline; the web
 * monitor (USE_WEBSERVER=1) renders the identical fields over HTTP.
 */
static void serial_monitor_task(void *arg)
{
    for (;;) {
        // Block if serial display bit not set
        xEventGroupWaitBits(display_group, SERIAL_DISP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        UBaseType_t depth = uxQueueMessagesWaiting(data_q);
        EventBits_t bits  = xEventGroupGetBits(evt_group);
        ESP_LOGI(TAG,
                 "[serial] status=%s ride count=%u completion=%u%%",
                 estop_active ? "HALTED" : "IN PROGRESS",
                 (unsigned)r_completed, (unsigned)r_progress);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#if USE_WEBSERVER
/* ---------- HTTP handler: live JSON state ---------- */
static esp_err_t handle_state(httpd_req_t *req)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf),
        "{\"on\":%s,\"count\":%u,\"progress\":%u}",
        estop_active ? "false" : "true", 
        (unsigned)r_completed, (unsigned)r_progress);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* ---------- HTTP handler: root page (HTML shell only) ---------- *
 * The HTML is served once. JavaScript polls /state at 4 Hz and updates the
 * DOM in place &mdash; no full reload, no flicker, no Nyquist aliasing against
 * the 1 Hz blink. */
static esp_err_t handle_root(httpd_req_t *req)
{
    /* TODO: customize this HTML for your theme.
     * Examples:
     *   - Avionics: "UAV-01 Beacon · status: ON"
     *   - Medical:  "Heart-rate indicator · pulse: detected"
     *   - Space:    "SAT-1 health beacon · t+00:42:17"
     *   - Industrial: "Ride-X dispatch · READY"
     *   - Security:   "Enclave-A integrity · OK"
     */
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang=\"en\"><head>"
        "<meta charset=\"utf-8\">"
        "<title>Ride Monitor</title>"
        "<style>"
        "  body { font-family: -apple-system, sans-serif; background: #FAFAF5; "
        "         color: #1A1A1A; padding: 2rem; }"
        "  h1 { color: #6B4F09; border-bottom: 3px solid #FFC904; "
        "       display: inline-block; padding-bottom: 4px; }"
        "  .state { font-size: 3em; font-weight: 700; margin: 1rem 0; "
        "           transition: color 120ms ease; }"
        "  .state.on  { color: #1B6A2E; }"
        "  .state.off { color: #B81829; }"
        "  .meta { color: #6B4F09; font-variant-numeric: tabular-nums; }"
        "  .dot { display:inline-block; width: 0.6em; height: 0.6em; "
        "         border-radius: 50%; margin-right: 0.4em; "
        "         vertical-align: middle; transition: background 120ms ease; }"
        "  .dot.on  { background: #1B6A2E; }"
        "  .dot.off { background: #B81829; }"
        "</style></head>"
        "<body>"
        "<h1>Ride Monitor</h1>"
        "<p>Ride Status:</p>"
        "<div id=\"state\" class=\"state off\">"
        "  <span id=\"dot\" class=\"dot off\"></span><span id=\"label\">--</span>"
        "</div>"
        "<p class=\"meta\">Rides completed: <span id=\"count\">0</span></p>"
        "<p class=\"meta\">Current ride progress: <span id=\"progress\">0</span></p>"
        "<p class=\"meta\">Polling at 1 Hz via <code>/state</code> JSON endpoint.</p>"
        "<script>"
        "async function poll(){"
        "  try{"
        "    const r = await fetch('/state',{cache:'no-store'});"
        "    const s = await r.json();"
        "    const cls = s.on ? 'on' : 'off';"
        "    document.getElementById('state').className = 'state ' + cls;"
        "    document.getElementById('dot').className = 'dot ' + cls;"
        "    document.getElementById('label').textContent = s.on ? 'IN PROGRESS' : 'HALTED';"
        "    document.getElementById('count').textContent = s.count;"
        "    document.getElementById('progress').textContent = s.progress + '%';"
        "  }catch(e){/* ignore transient network blips */}"
        "}"
        "setInterval(poll, 500);"
        "poll();"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.core_id = 0;                    /* networking on Core 0 */
    cfg.task_priority = 5;
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = handle_root,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t state = {
            .uri = "/state",
            .method = HTTP_GET,
            .handler = handle_state,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &state);

        ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_PORT);
    } else {
        ESP_LOGE(TAG, "HTTP server failed to start");
    }
    return server;
}

/* ---------- Wi-Fi event handler ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, displaying on serial & reconnecting...");
        xEventGroupSetBits(display_group, SERIAL_DISP_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_webserver();
        xEventGroupSetBits(display_group, WIFI_DISP_BIT);
        xEventGroupClearBits(display_group, SERIAL_DISP_BIT);
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}
/* ---------- Web monitor (Core 0)  [USE_WEBSERVER = 1] ---------- */
#else
#endif /* USE_WEBSERVER */

// Monitors network while on WiFi, switch back to serial in the event of failure
void task_network_monitor(void *pvParameters) {
    for (;;) {
        // Block if WiFi display bit not set, only run while wifi is enabled
        xEventGroupWaitBits(display_group, WIFI_DISP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock >= 0) {
            struct sockaddr_in dest_addr;
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(80);
            
            // Reliable external IP
            inet_pton(AF_INET, "1.1.1.1", &dest_addr.sin_addr); 
            
            // Set a short timeout so the task doesn't block forever
            struct timeval timeout;
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

            int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            
            if (err == 0) {
                // Connection successful!.
                // Ensure set to wifi
                xEventGroupClearBits(display_group, SERIAL_DISP_BIT);
                xEventGroupSetBits(display_group, WIFI_DISP_BIT);
                close(sock);
            } else {
                // Connection failed!.
                ESP_LOGE(TAG, "WiFi unreachable, falling back to serial.");
                
                // Turn on serial
                xEventGroupClearBits(display_group, WIFI_DISP_BIT);
                xEventGroupSetBits(display_group, SERIAL_DISP_BIT);
                close(sock);
            }
        }
        
        // Wait 10 seconds before checking again
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* ---------- app_main ---------- */
void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    display_group = xEventGroupCreate(); // for serial or wifi display
    ESP_LOGI(TAG, "==== Final Capstone - Ride Progress & Safety Demo ====");

#if USE_WEBSERVER
    ESP_LOGI(TAG, "Monitor: WEB (USE_WEBSERVER=1) — (Core 0)");
    wifi_init_sta(); // Event group bits are set by the event handler
#else
    ESP_LOGI(TAG, "Monitor: SERIAL (USE_WEBSERVER=0) — Core-0 summary once/sec, no Wi-Fi");
    xEventGroupClearBits(display_group, WIFI_DISP_BIT);
    xEventGroupSetBits(display_group, SERIAL_DISP_BIT);
#endif

    /* TODO: pick queue length + item size. Defend in README.
     * Hint: producer at 20 Hz, consumer at unknown rate — what burst size? */
    data_q = xQueueCreate(5, sizeof(ride_state_t));
    evt_group = xEventGroupCreate();
    estop_sem = xSemaphoreCreateBinary();

    /* Tasks on Core 1 (real-time plane). 4096-byte stacks: any task that calls
     * ESP_LOGI needs headroom for the vprintf formatting (2048 overflows). */
    xTaskCreatePinnedToCore(producer_task,    "prod",   4096, NULL,  8, &prod_handle, APP_CPU_NUM);
    xTaskCreatePinnedToCore(consumer_task,    "cons",   4096, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(coordinator_task, "coord",  4096, NULL,  9, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(responder_task,   "resp",   4096, NULL, 12, &responder_handle, APP_CPU_NUM);
    xTaskCreatePinnedToCore(estop_task,    "estop",   4096, NULL, 15, NULL, APP_CPU_NUM);
    // xTaskCreatePinnedToCore(task_monitor,    "tmonitor",   4096, NULL, 3, NULL, APP_CPU_NUM);

    /* Observability plane on Core 0 (networking plane) */
    // Serial monitor task (will halt if wifi enabled)
    xTaskCreatePinnedToCore(serial_monitor_task, "smonitor", 4096, NULL, 4, NULL, PRO_CPU_NUM);
    // WiFi monitoring task
    xTaskCreatePinnedToCore(task_network_monitor,    "tmonitor",   4096, NULL, 3, NULL, PRO_CPU_NUM);

    /* Button ISR */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };
    /* E-STOP ISR */
    gpio_config_t estop_cfg = {
        .pin_bit_mask = 1ULL << ESTOP_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };

    gpio_config(&cfg);
    gpio_config(&estop_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
    gpio_isr_handler_add(ESTOP_GPIO, estop_isr, NULL);
}
