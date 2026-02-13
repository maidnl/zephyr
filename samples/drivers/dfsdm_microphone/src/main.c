/*
 * Copyright (c) 2026 Arduino (Daniele Aimo)
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/dfsdm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/cache.h>

/* CONFIGURATION */

/* This example outputs the data obtained from the filter on the UART
 * if SAVE_RAW_DATA is NOT defined the data are printed in a human readable from
 * allowing to "see" the change while noises are recorded by the microphone
 *
 * if SAVE_RAW_DATA is defined the data are printed in order to be actually stored
 * in a wav play and listened */

#define SAVE_RAW_DATA

LOG_MODULE_REGISTER(main);

/* Get the device pointer from Device Tree */
const struct device *pdm_dev = DEVICE_DT_GET(DT_NODELABEL(dfsdm1_flt0));

/* Audio Configuration */
#define AUDIO_FREQ   16000
#define OVERSAMPLING 125
#define CLOCK_RATE   (AUDIO_FREQ * OVERSAMPLING) /* 2.0 MHz */

/* _____________________________________________________________________KSLAB */
#define BLOCK_SIZE_SAMPLES 256 /* ~16ms at 2MHz  */
#define BLOCK_SIZE_BYTES   (BLOCK_SIZE_SAMPLES * sizeof(int32_t))
#define BLOCK_NUM          2
#define DRIVE_BUFFER_SIZE  (BLOCK_SIZE_BYTES * BLOCK_NUM)
/* The driver will use this buffer to store samples from the filter
 * data are 32 aligned to allow cache invalidation */
int32_t __aligned(32) driver_data_buffer[BLOCK_SIZE_SAMPLES * BLOCK_NUM];

/* _________________________________________________________________SEMAPHORE */
/* Used to synchronize the main thread waiting for filtered data */
K_SEM_DEFINE(data_ready_sem, 0, 1);

/* Some quick facts:
 * - audio frequency 16000 Hz means 16000 sample per seconds
 * - a block of 256 sample is filled in 16 ms
 * - this means that the audio callback is called every 16 ms with 256 sample
 *   to be processed
 * - if the main loop is not faster than 16 ms data become corrupted and sample
 *   are lost or overwritten
 * - Setting USB Serial communication to 921600 BAUD means that printing 512 bytes
 *   (because we are squeezing 32 bit sample in 16 bit sample) would take around
 *   5.6 ms to print the buffer -> fast enough respect to the irq firing */

/* _____________________________________________________________DOUBLE_BUFFER */
/* Simply double buffer to "ping pong" data from device to application */
#define DOUBLE_BUFFER_SIZE BLOCK_SIZE_SAMPLES
int32_t double_buffer[2][DOUBLE_BUFFER_SIZE];
uint8_t current_buffer = 0;
#define INCREMENT(x, n)  x = (++x) % n
#define FILLED_BUFFER(n) ((n == 0) ? 1 : 0)

static volatile uint32_t received = 0;

/* ____________________________________________________________AUDIO CALLBACK */
/* It is called when a block of data is available
 * IMPORTANT: the callback is called in a IRQ context, it must perform operation
 * in the quickest possible way (in this case just copy received data for later
 * processing) */
void audio_callback(const struct device *dev, uint8_t filter_id, void *data, size_t size,
		    void *user_data)
{
	int32_t *samples = (int32_t *)data;
	/* copy data on user buffer */
	memcpy(double_buffer[current_buffer], samples, size);
	/* prepare for next chunck of data */
	INCREMENT(current_buffer, 2);
	/* tell the main thread data are ready */
	k_sem_give(&data_ready_sem);
	received += 1;
	// printk("%i\n", received);
}

/* This function is used to print a buffer on USB serial */
void uart_print(const struct device *dev, const char *buf)
{
	while (*buf) {
		uart_poll_out(dev, *buf);
		buf++;
	}
}

#ifdef SAVE_RAW_DATA

static int16_t print_binary_data[DOUBLE_BUFFER_SIZE];
/* This function prints raw data. If correctly saved in a wav file this can be
 * actually listened
 * To produce a wav file we use a simple python script which is expecting 2 bytes
 * per sample, but the filter is producing 32 bit wide data so first we "squeeze"
 * data into 16 bit per sample and then we print the whole new array in "raw"
 * format */
void uart_out(const struct device *dev, int32_t *buf, size_t dim)
{
	if (dim != DOUBLE_BUFFER_SIZE) {
		printk("Something wrong with sizes\n");
		return;
	}
	/* "squeeze data" */
	for (uint32_t i = 0; i < dim; i++) {
		print_binary_data[i] = (int16_t)(*(buf + i));
	}
	/* print raw data */
	unsigned char *b = (unsigned char *)print_binary_data;
	for (int i = 0; i < 256 * sizeof(int16_t); i++) {
		uart_poll_out(dev, *(b + i));
	}
}
#else
static int16_t print_binary_data[DOUBLE_BUFFER_SIZE];
void uart_out(const struct device *dev, int32_t *buf, size_t dim)
{
	char line_buf[16];
	if (dim != DOUBLE_BUFFER_SIZE) {
		printk("Something wrong with sizes\n");
		return;
	}
	/* "squeeze data" */
	char *b = line_buf;
	for (uint32_t i = 0; i < dim; i++) {
		print_binary_data[i] = (int16_t)(*(buf + i));
		snprintf(line_buf, sizeof(line_buf), "%i\n", print_binary_data[i]);
		while (*b) {
			uart_poll_out(dev, *b);
			b++;
		}
	}
}
#endif

/* This function is used to increase baurate of usb serial port */
void configure_usb_baudrate(const struct device *uart_dev)
{
	int ret;

	// Set the baudrate to 921600
	ret = uart_line_ctrl_set(uart_dev, UART_LINE_CTRL_BAUD_RATE, 921600);

	if (ret == -ENOSYS) {
		// This means the specific driver doesn't support setting baudrate manually
		printk("Baudrate setting not supported by this USB driver.\n");
	} else if (ret < 0) {
		printk("Error setting baudrate: %d\n", ret);
	} else {
		// printk("Baudrate set successfully!\n");
	}
}

int main(void)
{
	int err;

	const struct device *uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

	/* Enable the USB Driver */
	if (usb_enable(NULL)) {
		return 0;
	}

	/* Wait for the host (PC) to connect to the Serial Port */
	while (!dtr) {
		uart_line_ctrl_get(uart_dev, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}
	configure_usb_baudrate(uart_dev);

	/* ---- CHANNEL CONFIGURATION ---- */
	struct dfsdm_channel_config ch_cfg = {
		.channel_id = 0,
		.source = DFSDM_SOURCE_PIN,
		.type = DFSDM_TYPE_SPI_RISING,
		.clock_mode = DFSDM_CLOCK_INTERNAL,
		.clock_source = DFSDM_INT_CLOCK_SOURCE_0,
		.output_clock_rate = CLOCK_RATE,
		.offset = 0,
		.right_bit_shift = 0,
	};

	err = dfsdm_configure_channel(pdm_dev, &ch_cfg);

	if (err) {
		LOG_ERR("Failed to configure channel (err %d)", err);
		return err;
	}

	/* ----- STREAM CONFIGURATION (fiter settings) */

	struct dfsdm_stream_config stream_cfg = {
		.filter =
			{
				.filter_id = 0,
				.channel_id = 0,
				.sinc_type = DFSDM_SINC_FAST,
				.oversampling = OVERSAMPLING,
				.integrator = 0,
			},

		.data_buffer = driver_data_buffer,
		.data_buffer_size = DRIVE_BUFFER_SIZE,
		.invalidate_cache = true,
		.discard_first_n_block = 1,
		.callback = audio_callback,
		.user_data = NULL,
	};

	err = dfsdm_configure_stream(pdm_dev, &stream_cfg);

	if (err) {
		LOG_ERR("Failed to configure stream (err %d)", err);
		return err;
	}

	/* ------ START RECORDING ------ */

	err = dfsdm_trigger(pdm_dev, 0, DFSDM_TRIGGER_START);
	if (err) {
		LOG_ERR("Failed to trigger start (err %d)", err);
		return err;
	}
	/* ------ MAIN LOOP -------- */
	while (1) {
		/* Check connection status periodically */
		uart_line_ctrl_get(uart_dev, UART_LINE_CTRL_DTR, &dtr);

		if (dtr) {
			if (k_sem_take(&data_ready_sem, K_FOREVER) == 0) {

				if (received) {
					if (received > 1) {
						// printk("!!!!!!! MISSING DATA !!!!!!!\n\n");
					}
					received = 0;
					/* which buffer is full ? */
					uint8_t bi = FILLED_BUFFER(current_buffer);
					uart_out(uart_dev, (const char *)double_buffer[bi],
						 DOUBLE_BUFFER_SIZE);
				}
			}
		} else {
			k_sem_reset(&data_ready_sem);
			k_sleep(K_MSEC(100));
		}
	}

	return 0;
}
