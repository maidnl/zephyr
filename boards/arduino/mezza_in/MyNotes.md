# To use coprocessor

## Device tree

```
/ {
    zephyr,user {
        /* Application controls these pins directly */
        copro-reset-gpios = <&gpioa 4 GPIO_ACTIVE_LOW>;
        copro-boot-gpios = <&gpiob 7 GPIO_ACTIVE_HIGH>;
    };
};

&spi1 {
    status = "okay";

    /* The array of Chip Select pins for devices on this bus */
    cs-gpios = <&gpioc 1 GPIO_ACTIVE_LOW>;

    /* Our Slave MCU */
    io_copro: mcu2@0 {
        compatible = "vnd,io-copro";
        reg = <0>; /* Matches index 0 in the cs-gpios array above */
        spi-max-frequency = <4000000>; /* 4 MHz */
    };
};
```

## Bindings

Create a file named vnd,io-copro.yaml inside your project's dts/bindings/ folder (or your board
folder) with exactly these three lines:

```
compatible: "vnd,io-copro"
description: Custom IO Coprocessor accessed via generic SPI
include: spi-device.yaml
```

## C source interaction

```
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

/* 1. Grab the generic SPI configuration from the devicetree */
/* We set the SPI mode (e.g., 8-bit words, Mode 0) right here */
static const struct spi_dt_spec copro_spi =
    SPI_DT_SPEC_GET(DT_NODELABEL(io_copro), SPI_WORD_SET(8) | SPI_MODE_CPOL | SPI_MODE_CPHA, 0);

/* 2. Grab the control pins from zephyr,user */
static const struct gpio_dt_spec copro_reset =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), copro_reset_gpios);

void main(void) {
    uint8_t tx_data[] = {0x01, 0x02, 0x03};
    uint8_t rx_data[3] = {0};

    /* Setup SPI Tx/Rx buffers */
    struct spi_buf tx_buf = { .buf = tx_data, .len = sizeof(tx_data) };
    struct spi_buf rx_buf = { .buf = rx_data, .len = sizeof(rx_data) };
    struct spi_buf_set tx_bufs = { .buffers = &tx_buf, .count = 1 };
    struct spi_buf_set rx_bufs = { .buffers = &rx_buf, .count = 1 };

    /* 3. Check if the SPI bus is ready */
    if (!spi_is_ready_dt(&copro_spi)) {
        printk("SPI bus is not ready!\n");
        return;
    }

    /* (Optional) Toggle your reset pin using the gpio_dt_spec here */

    /* 4. Perform the generic SPI transaction */
    /* This automatically asserts the CS pin, sends the data, and de-asserts CS */
    int err = spi_transceive_dt(&copro_spi, &tx_bufs, &rx_bufs);

    if (err == 0) {
        printk("Received: 0x%02x 0x%02x 0x%02x\n", rx_data[0], rx_data[1], rx_data[2]);
    }
}
```
