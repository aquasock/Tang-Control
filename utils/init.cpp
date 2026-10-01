extern "C" {
#include "bflb_gpio.h"
#include "bflb_irq.h"
#include "board.h"
}

#include "utils.h"
#include "init.h"

extern "C" void bflb_uart_set_console(struct bflb_device_s *dev);

namespace {
uint32_t fpga_uart_baud = 2000000;
bool fpga_uart_initialized = false;

// The BL616 UART RX FIFO is only 32 bytes; at 2 Mbaud a burst of FPGA traffic
// can overflow it between 1 ms polls, dropping bytes and corrupting frames.
// Drain the FIFO in the RX interrupt (threshold 7) into a larger ring buffer
// instead, and let the RX task parse from the ring buffer at its leisure.
//
// Minimal single-producer (ISR) / single-consumer (RX task) ring buffer:
// head is written only by the ISR, tail only by the RX task, so no lock is
// needed. Size 2048 comfortably covers the 1024-byte stream credit window.
#define RX_RING_SIZE 2048
static uint8_t rx_ring_buf[RX_RING_SIZE];
static volatile uint32_t rx_ring_head = 0;
static volatile uint32_t rx_ring_tail = 0;

static void uart1_rx_isr(int irq, void *arg)
{
    uint32_t intstatus = bflb_uart_get_intstatus(uart1_dev);
    if (intstatus & (UART_INTSTS_RX_FIFO | UART_INTSTS_RTO)) {
        while (bflb_uart_rxavailable(uart1_dev)) {
            const uint8_t ch = (uint8_t)bflb_uart_getchar(uart1_dev);
            const uint32_t next = (rx_ring_head + 1) & (RX_RING_SIZE - 1);
            if (next != rx_ring_tail) {  // drop the byte if the ring is full
                rx_ring_buf[rx_ring_head] = ch;
                rx_ring_head = next;
            }
        }
        if (intstatus & UART_INTSTS_RTO) {
            bflb_uart_int_clear(uart1_dev, UART_INTCLR_RTO);
        }
    }
}

bool configure_fpga_uart(uint32_t baudrate)
{
    struct bflb_uart_config_s uart1_cfg = {
        .baudrate = baudrate,
        .direction = UART_DIRECTION_TXRX,
        .data_bits = UART_DATA_BITS_8,
        .stop_bits = UART_STOP_BITS_1,
        .parity    = UART_PARITY_NONE,
        .bit_order = UART_LSB_FIRST,
        .flow_ctrl = 0,
        .tx_fifo_threshold = 7,
        .rx_fifo_threshold = 7,
    };
    if (fpga_uart_initialized) {
        bflb_uart_deinit(uart1_dev);
    }
    bflb_uart_init(uart1_dev, &uart1_cfg);
    bflb_uart_set_console(uart1_dev);
    fpga_uart_initialized = true;
    fpga_uart_baud = baudrate;

    rx_ring_head = 0;
    rx_ring_tail = 0;
    bflb_uart_rxint_mask(uart1_dev, false);
    bflb_irq_attach(uart1_dev->irq_num, uart1_rx_isr, NULL);
    bflb_irq_enable(uart1_dev->irq_num);
    return true;
}
}

uint32_t fpga_uart_rx_ring_len(void)
{
    return (rx_ring_head - rx_ring_tail) & (RX_RING_SIZE - 1);
}

uint32_t fpga_uart_rx_ring_read(uint8_t *ch)
{
    if (rx_ring_head == rx_ring_tail) {
        return 0;  // empty
    }
    *ch = rx_ring_buf[rx_ring_tail];
    rx_ring_tail = (rx_ring_tail + 1) & (RX_RING_SIZE - 1);
    return 1;
}

bool fpga_uart_set_baud(uint32_t baudrate)
{
    if (baudrate != 2000000 && baudrate != 5000000) {
        return false;
    }
    taskENTER_CRITICAL();
    const bool result = configure_fpga_uart(baudrate);
    taskEXIT_CRITICAL();
    return result;
}

uint32_t fpga_uart_get_baud()
{
    return fpga_uart_baud;
}

void init_gpio_and_uart() {
    // turn of UART0
    // uart0_dev = bflb_device_get_by_name("uart0");
    // bflb_uart_deinit(uart0_dev);

    gpio_dev = bflb_device_get_by_name("gpio");
    // deinit all GPIOs
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_0);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_1);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_2);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_3);

    bflb_gpio_deinit(gpio_dev, GPIO_PIN_10);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_11);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_12);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_13);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_14);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_15);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_16);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_17);

    bflb_gpio_deinit(gpio_dev, GPIO_PIN_20);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_21);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_22);

    bflb_gpio_deinit(gpio_dev, GPIO_PIN_27);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_28);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_29);
    bflb_gpio_deinit(gpio_dev, GPIO_PIN_30);

    /* Core control UART 1 */
#ifdef TANG_PRIMER25K
    bflb_gpio_uart_init(gpio_dev, GPIO_PIN_11, GPIO_UART_FUNC_UART1_TX);    // JTAG connector pin 6
    bflb_gpio_uart_init(gpio_dev, GPIO_PIN_10, GPIO_UART_FUNC_UART1_RX);    // JTAG connector pin 7 (pin8 is GND, pin1 is VCC)
#elif defined(TANG_NANO20K)
    bflb_gpio_uart_init(gpio_dev, GPIO_PIN_11, GPIO_UART_FUNC_UART1_TX);    // JTAG connector pin 6
    bflb_gpio_uart_init(gpio_dev, GPIO_PIN_13, GPIO_UART_FUNC_UART1_RX);    // JTAG connector pin 7 (pin8 is GND, pin1 is VCC)
#else
    bflb_gpio_uart_init(gpio_dev, GPIO_PIN_28, GPIO_UART_FUNC_UART1_TX);    // JTAG connector pin 6
    bflb_gpio_uart_init(gpio_dev, GPIO_PIN_27, GPIO_UART_FUNC_UART1_RX);    // JTAG connector pin 7 (pin8 is GND, pin1 is VCC)
#endif

    /* Get handle to UART1 */
    uart1_dev = bflb_device_get_by_name("uart1");
    /* Initialize UART1 at the protocol's safe rate. */
#if defined(TANG_CONSOLE60K) || defined(TANG_CONSOLE138K)
    configure_fpga_uart(2000000);
#else
    // all other boards have 26MHz XTAL
    configure_fpga_uart(2000000 * 40 / 26);
#endif

    // set JTAG pins to high-Z
    // interrupts masked, SWGPIO mode, output off, input off, schmitt ON
    const uint32_t GPIO_HIGH_Z = (1 << 22) | (0xB << 8) | (1 << 1);
    *reg_gpio_tms = GPIO_HIGH_Z;
    *reg_gpio_tck = GPIO_HIGH_Z;
    *reg_gpio_tdo = GPIO_HIGH_Z;
    *reg_gpio_tdi = GPIO_HIGH_Z;

    // Initialize SD pins
    board_sdh_gpio_init();
    // bflb_gpio_init(gpio, GPIO_PIN_10, GPIO_FUNC_SDH | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_2);  // D1
    // bflb_gpio_init(gpio, GPIO_PIN_11, GPIO_FUNC_SDH | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_2);  // D0
    // bflb_gpio_init(gpio, GPIO_PIN_12, GPIO_FUNC_SDH | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_2);  // CLK
    // bflb_gpio_init(gpio, GPIO_PIN_13, GPIO_FUNC_SDH | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_2);  // CMD
    // bflb_gpio_init(gpio, GPIO_PIN_14, GPIO_FUNC_SDH | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_2);  // D3
    // bflb_gpio_init(gpio, GPIO_PIN_15, GPIO_FUNC_SDH | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_2);  // D2

    // Set GPIO 1 (physical pin 15) to high to enable SDMMC
    bflb_gpio_init(gpio_dev, GPIO_PIN_16, GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_3);
    bflb_gpio_set(gpio_dev, GPIO_PIN_16);
}
