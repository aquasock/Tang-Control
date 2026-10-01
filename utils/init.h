#pragma once

extern void init_gpio_and_uart();
extern bool fpga_uart_set_baud(uint32_t baudrate);
extern uint32_t fpga_uart_get_baud();
extern uint32_t fpga_uart_rx_ring_len(void);
extern uint32_t fpga_uart_rx_ring_read(uint8_t *ch);
