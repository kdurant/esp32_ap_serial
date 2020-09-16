#ifndef UART_PROCESS
#define UART_PROCESS

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "string.h"
#include "driver/gpio.h"

#define RX_BUF_SIZE 1024

// #define TXD_PIN (GPIO_NUM_13)
// #define RXD_PIN (GPIO_NUM_12)

#define TXD_PIN (GPIO_NUM_14)
#define RXD_PIN (GPIO_NUM_15)

typedef struct _uartBuf_
{
    uint8_t  frameEnd;
    uint16_t len;
    uint8_t  data[RX_BUF_SIZE];
} UartBuf;

extern UartBuf UartTcpBuf;

void uartInitNormal(void);

void uartInitInterrupt(void);

int uart_write_frame(uart_port_t uart_num, const char* src, size_t size);

#endif