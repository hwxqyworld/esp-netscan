#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define LED_GPIO 8 // ESP32-C3 SuperMini 板载LED一般为GPIO8，如有不同请修改

typedef enum {
    LED_SCAN,    // 扫描中（慢闪）
    LED_DONE,    // 扫描完成（常亮）
    LED_ERROR    // 错误（快闪）
} led_status_t;

static led_status_t g_led_status = LED_SCAN;
static TaskHandle_t led_task_handle = NULL;
static void led_status_task(void *param);
static void set_led_status(led_status_t status);

#define START_IP  (0xAC100000) // 172.16.0.0
#define END_IP    (0xAC1FFFFF) // 172.31.255.255
#define PORT      3389

#define BATCH_SIZE 100
#define WORKER_COUNT 4 // 并发worker数量，可根据实际情况调整

typedef enum {
    PORT_OPEN,
    PORT_CLOSED,
    HOST_DOWN
} ip_status_t;

typedef struct {
    uint32_t ip;
    ip_status_t status;
} ip_scan_result_t;

static void report_batch(ip_scan_result_t *results, int count);
static void save_batch_to_flash(ip_scan_result_t *results, int count);
static ip_status_t scan_ip(uint32_t ip);

typedef struct {
    uint32_t ip;
} scan_task_t;

static QueueHandle_t task_queue = NULL;
static QueueHandle_t result_queue = NULL;
static void worker_task(void *param);

void app_main(void)
{
    // 初始化 NVS、网络等
    nvs_flash_init();
    esp_netif_init();

    // 初始化LED
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0);

    // 启动LED状态任务
    xTaskCreate(led_status_task, "led_status", 1024, NULL, 2, &led_task_handle);
    set_led_status(LED_SCAN);

    // 初始化 WiFi
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // 关键：关闭省电模式（C3 默认开启会导致连接不稳定）
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 关键：清理上一次连接状态
    esp_wifi_disconnect();
    vTaskDelay(200 / portTICK_PERIOD_MS);

    esp_wifi_set_mode(WIFI_MODE_STA);

    // 配置 WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "FMZ001_Wi-Fi5",
            .password = "1357924680000",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,   // C3 推荐全信道扫描
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI("netscan", "WiFi connecting...");

    while(1) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI("netscan", "Connected to WiFi: %s", ap_info.ssid);
            break;
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    task_queue = xQueueCreate(32, sizeof(scan_task_t));
    result_queue = xQueueCreate(32, sizeof(ip_scan_result_t));

    // 启动worker任务
    for (int i = 0; i < WORKER_COUNT; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "worker_%d", i);
        xTaskCreate(worker_task, name, 4096, NULL, 5, NULL);
    }

    ip_scan_result_t batch[BATCH_SIZE];
    int batch_count = 0;

    uint32_t total_ip = END_IP - START_IP + 1;
    uint32_t dispatched = 0, finished = 0;

    // 先分发部分任务，保持队列有任务
    for (; dispatched < WORKER_COUNT * 8 && dispatched < total_ip; ++dispatched) {
        scan_task_t t = { .ip = START_IP + dispatched };
        xQueueSend(task_queue, &t, portMAX_DELAY);
    }

    bool error_flag = false;

    while (finished < total_ip) {
        ip_scan_result_t result;
        if (xQueueReceive(result_queue, &result, portMAX_DELAY) == pdTRUE) {
            batch[batch_count++] = result;
            finished++;

            // 每收到一个结果就补充一个任务
            if (dispatched < total_ip) {
                scan_task_t t = { .ip = START_IP + dispatched };
                xQueueSend(task_queue, &t, portMAX_DELAY);
                dispatched++;
            }

            if (batch_count == BATCH_SIZE) {
                report_batch(batch, batch_count);
                save_batch_to_flash(batch, batch_count);
                batch_count = 0;
            }
        } else {
            // 队列异常，错误
            error_flag = true;
            break;
        }
    }
    // 处理最后不足100个的批次
    if (!error_flag && batch_count > 0) {
        report_batch(batch, batch_count);
        save_batch_to_flash(batch, batch_count);
    }

    if (error_flag) {
        set_led_status(LED_ERROR);
        ESP_LOGE("netscan", "Queue error, scan aborted!");
    } else {
        set_led_status(LED_DONE);
        ESP_LOGI("netscan", "Scan finished. Total: %u", total_ip);
    }
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}
// LED状态任务
static void led_status_task(void *param)
{
    while (1) {
        switch (g_led_status) {
            case LED_SCAN:
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case LED_DONE:
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case LED_ERROR:
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            default:
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
        }
    }
}

static void set_led_status(led_status_t status)
{
    g_led_status = status;
}

// worker任务体
static void worker_task(void *param)
{
    scan_task_t task;
    while (1) {
        if (xQueueReceive(task_queue, &task, portMAX_DELAY) == pdTRUE) {
            ip_scan_result_t result;
            result.ip = task.ip;
            result.status = scan_ip(task.ip);
            xQueueSend(result_queue, &result, portMAX_DELAY);
        }
    }
}

// 扫描单个IP的3389端口，返回状态
static ip_status_t scan_ip(uint32_t ip)
{
    struct sockaddr_in addr;
    int sock, ret, err;
    fd_set fdset;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 }; // 50ms
    socklen_t lon;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(ip);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return HOST_DOWN;
    }

    // 设置为非阻塞
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) {
        // 立即连接成功
        close(sock);
        return PORT_OPEN;
    } else if (errno == EINPROGRESS) {
        // 连接正在进行，使用select等待
        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);
        ret = select(sock + 1, NULL, &fdset, NULL, &tv);
        if (ret > 0) {
            lon = sizeof(int);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (void*)(&err), &lon);
            if (err == 0) {
                close(sock);
                return PORT_OPEN;
            } else if (err == ECONNREFUSED) {
                close(sock);
                return PORT_CLOSED;
            } else {
                close(sock);
                return HOST_DOWN;
            }
        } else {
            // select超时或出错
            close(sock);
            return HOST_DOWN;
        }
    } else {
        // 立即失败
        if (errno == ECONNREFUSED) {
            close(sock);
            return PORT_CLOSED;
        } else {
            close(sock);
            return HOST_DOWN;
        }
    }
}

// 批量上报（需实现webhook HTTP POST）
static void report_batch(ip_scan_result_t *results, int count)
{
    ESP_LOGI("netscan", "Reporting %d results", count);

    // 分配足够大的缓冲区（你100条记录大概需要5~8KB）
    size_t json_size = 8192;
    char *json_buffer = malloc(json_size);
    if (!json_buffer) {
        ESP_LOGE("netscan", "malloc json_buffer failed");
        return;
    }

    int offset = 0;
    offset += snprintf(json_buffer + offset, json_size - offset, "{\"results\":[");

    for (int i = 0; i < count; i++) {
        const char *status_str = "unknown";
        if (results[i].status == PORT_OPEN) status_str = "open";
        else if (results[i].status == PORT_CLOSED) status_str = "closed";
        else if (results[i].status == HOST_DOWN) status_str = "down";

        uint8_t *ip_bytes = (uint8_t *)&results[i].ip;

        offset += snprintf(json_buffer + offset, json_size - offset,
            "{\"ip\":\"%u.%u.%u.%u\",\"status\":\"%s\"}%s",
            ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3],
            status_str, i < count - 1 ? "," : "");

        if (offset >= json_size - 128) {
            ESP_LOGW("netscan", "json_buffer nearly full, truncating");
            break;
        }
    }

    offset += snprintf(json_buffer + offset, json_size - offset, "]}");

    // HTTP 请求缓冲区
    size_t req_size = 12288;
    char *http_request = malloc(req_size);
    if (!http_request) {
        ESP_LOGE("netscan", "malloc http_request failed");
        free(json_buffer);
        return;
    }

    snprintf(http_request, req_size,
        "POST /webhook HTTP/1.1\r\n"
        "Host: 110.42.45.169\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n\r\n%s",
        offset, json_buffer);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock >= 0) {
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(52099),
            .sin_addr.s_addr = inet_addr("110.42.45.169"),
        };

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            send(sock, http_request, strlen(http_request), 0);
        } else {
            int err = errno;
            ESP_LOGE("netscan", "connect failed, errno=%d (%s)", err, strerror(err));
            // 设置LED为错误状态并停止任务
            set_led_status(LED_ERROR);
            ESP_LOGE("netscan", "Stopping all tasks due to webhook connect failure");
            vTaskDelay(pdMS_TO_TICKS(100)); // 确保日志输出
            vTaskSuspend(NULL); // 停止当前任务（或可用esp_restart()重启设备）
        }
        close(sock);
    }

    free(http_request);
    free(json_buffer);

    ESP_LOGI("netscan", "Webhook sent for %d results", count);
}

// 批量保存到flash（可用NVS或SPIFFS）
static void save_batch_to_flash(ip_scan_result_t *results, int count)
{
    // TODO: 实现保存到flash
    // ESP_LOGI("netscan", "Saving %d results to flash", count);
    // 暂时打印到串口
    for (int i = 0; i < count; i++) {
        const char *status_str = "unknown";
        if (results[i].status == PORT_OPEN) status_str = "open";
        else if (results[i].status == PORT_CLOSED) status_str = "closed";
        else if (results[i].status == HOST_DOWN) status_str = "down";

        uint8_t *ip_bytes = (uint8_t *)&results[i].ip;

        ESP_LOGI("netscan", "Saved: %u.%u.%u.%u - %s",
            ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3],
            status_str);
    }
}