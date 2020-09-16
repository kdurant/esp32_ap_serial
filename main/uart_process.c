#include "uart_process.h"

UartBuf UartTcpBuf;

void uartInitNormal(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};

    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // We won't use a buffer for sending data.
    uart_driver_install(UART_NUM_1, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
}

void uartRxTask()
{
    static const char* RX_TASK_TAG = "RX_TASK";
    esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);
    uint8_t* data = (uint8_t*)malloc(RX_BUF_SIZE + 1);
    while(1)
    {
        const int rxBytes = uart_read_bytes(UART_NUM_1, data, RX_BUF_SIZE, 1000 / portTICK_RATE_MS);
        if(rxBytes > 0)
        {
            data[rxBytes] = 0;
            ESP_LOGI(RX_TASK_TAG, "Read %d bytes: '%s'", rxBytes, data);
            ESP_LOG_BUFFER_HEXDUMP(RX_TASK_TAG, data, rxBytes, ESP_LOG_INFO);
        }
    }
    free(data);
}

static void IRAM_ATTR uart1_intr_handle(void* arg)
{
    volatile uart_dev_t* uart = &UART1;
    // uart->int_clr.frm_err     = 1;
    // uart_write_bytes(UART_NUM_1, (const char*)"uart1_intr_handle\r\n", strlen("uart1_intr_handle\r\n"));
    if(uart->int_st.rxfifo_full)
    {
        // uart_write_bytes(UART_NUM_1, (const char*)"rxfifo_full\r\n", strlen("rxfifo_full\r\n"));
        uart->int_clr.rxfifo_full = 1;
        while(uart->status.rxfifo_cnt)
        {
            UartTcpBuf.data[UartTcpBuf.len] = uart->fifo.rw_byte;
            UartTcpBuf.len++;
        }
    }

    if(uart->int_st.rxfifo_tout)  //检查是否产生超时中断
    {
        uart->int_clr.rxfifo_tout = 1;

        while(uart->status.rxfifo_cnt)
        {
            UartTcpBuf.data[UartTcpBuf.len] = uart->fifo.rw_byte;
            UartTcpBuf.len++;
        }
        UartTcpBuf.frameEnd = 1;
    }

    if(UartTcpBuf.frameEnd)
    {
        // uart_write_frame(UART_NUM_1, (char*)(UartTcpBuf.data), UartTcpBuf.len);
        while(UartTcpBuf.len > 0)
        {
            uart_write_bytes(UART_NUM_1, (char*)(UartTcpBuf.data), UartTcpBuf.len);
        }

        UartTcpBuf.frameEnd = 0;
        UartTcpBuf.len      = 0;
    }

    // uart_clear_intr_status(UART_NUM_1, UART_RXFIFO_FULL_INT_CLR | UART_RXFIFO_TOUT_INT_CLR);
    // uart_disable_rx_intr(UART_NUM_1);
}

void uartInitInterrupt(void)
{
    // release the pre registered UART handler/subroutine
    uart_isr_free(UART_NUM_1);
    uart_isr_handle_t handle;
    uart_isr_register(UART_NUM_1, uart1_intr_handle, NULL, ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_IRAM, &handle);
    // uart_set_rx_timeout(UART_NUM_1, 20);
    uart_enable_rx_intr(UART_NUM_1);  // enable RX interrupt (RX_FULL & RX_TIMEOUT INTERRUPT)
}

int uart_write_frame(uart_port_t uart_num, const char* src, size_t size)
{
    int offset = 0;
    while(size > 64)
    {
        uart_write_bytes(uart_num, src + offset, 64);
        uart_wait_tx_done(uart_num, portMAX_DELAY);
        size -= 64;
        offset += 64;
    }
    uart_write_bytes(uart_num, src + offset, size);

    return 0;
}