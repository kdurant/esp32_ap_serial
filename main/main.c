/*  WiFi softAP Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "uart_process.h"

/* The examples use WiFi configuration that you can set via project
   configuration menu.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_MAX_STA_CONN CONFIG_ESP_MAX_STA_CONN

#define TCP_SERVER_TASK_STK_SIZE 4096
#define TCP_SERVER_TASK_PRIO 7
#define PROCESS_CLIENT_TASK_STK_SIZE 2048
#define PROCESS_CLIENT_TASK_PRIO 7

#define TCP_SERVER_PORT 6666
#define TCP_SERVER_LISTEN_CLIENT_NUM 3

int client_sockfd;

/* Private macro -------------------------------------------------------------*/
#define TAG_XLI __func__

/* Private typedef -----------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
static TaskHandle_t app_tcp_server_single_conn_task_handler;

/* Private function ----------------------------------------------------------*/
static void app_tcp_server_single_conn_task(void* arg);

static const char* TAG = "wifi softAP";

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if(event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t* event =
            (wifi_event_ap_staconnected_t*)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac),
                 event->aid);
    }
    else if(event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t* event =
            (wifi_event_ap_stadisconnected_t*)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac),
                 event->aid);
    }
}

void wifi_init_softap()
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .ap = {.ssid           = EXAMPLE_ESP_WIFI_SSID,
               .ssid_len       = strlen(EXAMPLE_ESP_WIFI_SSID),
               .password       = EXAMPLE_ESP_WIFI_PASS,
               .max_connection = EXAMPLE_MAX_STA_CONN,
               .authmode       = WIFI_AUTH_WPA_WPA2_PSK},
    };
    if(strlen(EXAMPLE_ESP_WIFI_PASS) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
}

static void echo_task(void* arg)
{
    while(1)
    {
        if(UartTcpBuf.frameEnd)
        {
            uart_write_bytes(UART_NUM_1, (const char*)UartTcpBuf.data, UartTcpBuf.len);
            UartTcpBuf.frameEnd = 0x00;
        }
        // taskYIELD();
        esp_task_wdt_reset();
        vTaskDelay(1 / portTICK_RATE_MS);
    }
}

void uartRxTask()
{
    while(1)
    {
        UartZynqBuf.len = uart_read_bytes(UART_NUM_1, UartZynqBuf.data, RX_BUF_SIZE, 20 / portTICK_RATE_MS);
        if(UartZynqBuf.len > 0)
        {
            if(client_sockfd > 0)
                send(client_sockfd, (const char*)UartZynqBuf.data, UartZynqBuf.len, 0);
            UartZynqBuf.frameEnd = 0x01;
        }
    }
}

void app_main()
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES ||
       ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uartInitNormal();
    // uartInitInterrupt();

    ESP_LOGI(TAG, "ESP_WIFI_MODE_AP");
    wifi_init_softap();

    xTaskCreate((TaskFunction_t)app_tcp_server_single_conn_task,
                (const char*)"app_tcp_server_task",
                (uint16_t)TCP_SERVER_TASK_STK_SIZE,
                (void*)NULL,
                (UBaseType_t)TCP_SERVER_TASK_PRIO,
                (TaskHandle_t*)&app_tcp_server_single_conn_task_handler);

    xTaskCreate(echo_task,
                "uart_echo_task",
                (uint16_t)TCP_SERVER_TASK_STK_SIZE,
                NULL,
                10,
                NULL);
    xTaskCreate(uartRxTask,
                "uart_receive_task",
                (uint16_t)TCP_SERVER_TASK_STK_SIZE,
                NULL,
                10,
                NULL);
}

/**=============================================================================
 * @brief           Tcp服务端任务
 *
 * @param[in]       arg: 任务参数指针
 *
 * @return          none
 *============================================================================*/
static void app_tcp_server_single_conn_task(void* arg)
{
    struct sockaddr_in serv_addr, client_addr;
    socklen_t          client_addr_len = sizeof(client_addr);

    int serv_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(serv_sockfd < 0)
    {
        ESP_LOGE(TAG_XLI, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG_XLI, "Socket created");

    bzero(&serv_addr, sizeof(struct sockaddr_in));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port        = htons(TCP_SERVER_PORT);

    if(bind(serv_sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        ESP_LOGE(TAG_XLI, "Socket unable to bind: errno %d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG_XLI, "Socket binded");

    if(listen(serv_sockfd, 1) < 0)
    {
        ESP_LOGE(TAG_XLI, "Error occured during listen: errno %d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG_XLI, "Socket listening");

    char rx_buffer[512] = {0};
    while(1)
    {
        // int
        client_sockfd = accept(serv_sockfd, (struct sockaddr*)&client_addr, &client_addr_len);
        if(client_sockfd < 0)
        {
            ESP_LOGE(TAG_XLI, "Unable to accept connection: errno %d", errno);
        }
        ESP_LOGI(TAG_XLI, "A new client is connected, socket_fd=%d, addr=%s", client_sockfd, inet_ntoa(client_addr.sin_addr));

        while(1)
        {
            int len = recv(client_sockfd, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if(len < 0)
            {
                ESP_LOGE(TAG_XLI, "Recv failed: errno %d", errno);
                shutdown(client_sockfd, 0);
                close(client_sockfd);
                break;
            }
            else if(len == 0)
            {
                ESP_LOGI(TAG_XLI, "Connection closed");
                shutdown(client_sockfd, 0);
                close(client_sockfd);
                break;
            }
            else
            {
                ESP_LOGI(TAG_XLI, "Received %d bytes from socket_fd %d:", len, client_sockfd);
                ESP_LOGI(TAG_XLI, "%s", rx_buffer);

                memcpy(UartTcpBuf.data, rx_buffer, len);
                UartTcpBuf.len      = len;
                UartTcpBuf.frameEnd = 0x01;
                // send(client_sockfd, rx_buffer, sizeof(rx_buffer) / 2 - 1, 0);
            }
        }
        // if(UartZynqBuf.frameEnd)
        // {
        //     send(client_sockfd, UartZynqBuf.data, UartZynqBuf.len, 0);
        //     UartZynqBuf.frameEnd = 0x00;
        // }
    }

    vTaskDelete(NULL);
}