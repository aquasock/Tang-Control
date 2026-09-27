#pragma once

extern void init_gpio_and_uart();
extern bool fpga_uart_set_baud(uint32_t baudrate);
extern uint32_t fpga_uart_get_baud();
