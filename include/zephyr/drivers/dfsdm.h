/*
 * Copyright (c) 2026 Arduino (Daniele Aimo)
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_DFSDM_H_
#define ZEPHYR_INCLUDE_DRIVERS_DFSDM_H_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief DFSDM driver interface definition */

/**
 * Some general information about the driver:
 *
 * - As general rule the use of DMA is optional
 * - If the DMA is defined in the DT then the driver should use it
 * - If the DMA is not define in the DT the driver can use something like the
 *   irq of the filter itself
 * - For audio application the use of DMA is almost mandatory due to high rate
 *   sampling
 *
 * - The user get the data from the filter in 2 ways:
 * 1. by attaching a callback (see struct dfsdm_stream_config) which will be
 *    called any time the buffer of data is full (or half full). In this case
 *    probably the callback will be called in an irq context (with all well known
 *    consequences of that)
 * 2. if the user do no attach a callback then data can be retrieved via the read
 *    function
 *
 * In any case the user must get data from the filter in the faster possible way
 * or data might be lost or overwritten or corrupted
 *
 * The use of stream configuration and watchdog configuration function might
 * require to use channel configuration and / or stream configuration 
 * before (this could be hw dependent)
 * */

/** @brief The commands that can be sent to the driver */
enum dfsdm_trigger_cmd {
	/** Stop the filter */
	DFSDM_TRIGGER_STOP,
	/** Start the filter */
	DFSDM_TRIGGER_START,
	/** Pause the filter (might be not available, depends on specific
	 * hardware implementation) */
	DFSDM_TRIGGER_PAUSE,
	/** Resume the filter (might be not available, depends on specific
	 * hardware implementation) */
	DFSDM_TRIGGER_RESUME,
};

/** @brief Source used by the filter */
enum dfsdm_channel_source {
	/** The source of the filter come from an external PIN */
	DFSDM_SOURCE_PIN,
	/** The source of the filter come from Internal ADC */
	DFSDM_SOURCE_INTERNAL_ADC,
	/** The source of the filter come from parallel input */
	DFSDM_SOURCE_PARALLEL,
};

/** @brief Waveform input type and sample position */
enum dfsdm_channel_type {
	/** SPI with sampling on rising edge */
	DFSDM_TYPE_SPI_RISING,
	/** SPI with sampling on falling edge */
	DFSDM_TYPE_SPI_FALLING,
	/** Manchester with sampling on rising edge */
	DFSDM_TYPE_MANCHESTER_RISING,
	/** Manchester with sampling on falling edge */
	DFSDM_TYPE_MANCHESTER_FALLING,
};

/** @brief Select different clock mode */
enum dfsdm_channel_clock_mode {
	DFSDM_CLOCK_EXTERNAL,
	DFSDM_CLOCK_INTERNAL,
	DFSDM_CLOCK_INTERNAL_DIV2,
};

/** @brief Internal ck source selection */
enum dfsdm_internal_ck_source {
	DFSDM_INT_CLOCK_SOURCE_0,
	DFSDM_INT_CLOCK_SOURCE_1,
	DFSDM_INT_CLOCK_SOURCE_2
};

/** @brief Digital Filter Type (Sinc Order) */
enum dfsdm_sinc_type {
	DFSDM_SINC_FAST,
	DFSDM_SINC_1,
	DFSDM_SINC_2,
	DFSDM_SINC_3,
	DFSDM_SINC_4,
	DFSDM_SINC_5,
};

/** @brief define watchdog event to be monitored */
enum dfsdm_watchdog_event {
	DFSDM_WATCHDOG_THRESHOLD,
	DFSDM_WATCHDOG_SATURATION,
};

/* ------------------------------------------------ */
/* Configuration Structures                         */
/* ------------------------------------------------ */

/** @brief Channel Configuration (Physical Layer) */
struct dfsdm_channel_config {
	uint8_t channel_id;
	enum dfsdm_channel_source source;
	enum dfsdm_channel_type type;
	enum dfsdm_channel_clock_mode clock_mode;
	enum dfsdm_internal_ck_source clock_source;
	uint32_t output_clock_rate;
	uint32_t offset;
	uint8_t right_bit_shift;
};

/** * @brief Filter Configuration */
struct dfsdm_filter_config {
	uint8_t filter_id;  /* Hardware Filter ID */
	uint8_t channel_id; /* The Channel ID to listen to */

	enum dfsdm_sinc_type sinc_type; /* Sinc Filter Order */
	uint32_t oversampling;          /* FOSR (1..1024) */
	uint32_t integrator;            /* IOSR (1..256), usually 1 for audio */
};

/** @brief Callback for Stream Data */
typedef void (*dfsdm_stream_callback_t)(const struct device *dev, uint8_t filter_id, void *data,
					size_t size, void *user_data);

/** * @brief Stream Configuration */
struct dfsdm_stream_config {
	/** Filter settings */
	struct dfsdm_filter_config filter;

	/** The user must provider a buffer where are stores data received
	 * Alignment required might be HW dependent */
	void *data_buffer;
	/** The size of data buffer in bytes */
	size_t data_buffer_size;
	/** Set to true if data_buffer need to be invalidate after DMA filling */
	bool invalidate_cache;

	/** The filter need to warm up when it is started because it passes from
	 * 0 to a different mean DC value (this can be heard in case of a
	 * microphone as a loud "click" at the very beginning of the recording),
	 * this configuration paramenter allow to discard
	 * 1 or more block of data so that this loud "click" is not present
	 * anymore */
	uint8_t discard_first_n_block;

	/** async handler: the use of the callback is optional
	 * in case of undefined callback the user must poll for data received using
	 * read function */
	dfsdm_stream_callback_t callback;
	void *user_data;
};

struct dfsdm_watchdog_low_res_config {
	enum dfsdm_sinc_type sync;
	uint32_t oversampling;
};

/** @brief Watchdog Configuration */
struct dfsdm_watchdog_config {
	uint8_t filter_id;
	uint8_t channel_id;
	bool enable_analog_watchdog;
	bool use_low_resolution_watchdog;
	struct dfsdm_watchdog_low_res_config low_res_config;
	bool enable_short_circuit_detect;
	uint32_t short_circuit_counter;
	int32_t high_threshold;
	int32_t low_threshold;
	void (*cb)(const struct device *dev, uint8_t channel_id, uint32_t value,
		   enum dfsdm_watchdog_event event);
};

/* ------------------------------------------------ */
/* API Function Definitions                         */
/* ------------------------------------------------ */

typedef int (*dfsdm_api_configure_channel_t)(const struct device *dev,
					     const struct dfsdm_channel_config *cfg);

typedef int (*dfsdm_api_configure_stream_t)(const struct device *dev,
					    const struct dfsdm_stream_config *cfg);

typedef int (*dfsdm_api_configure_watchdog_t)(const struct device *dev,
					      const struct dfsdm_watchdog_config *cfg);

typedef int (*dfsdm_api_trigger_t)(const struct device *dev, uint8_t filter_id,
				   enum dfsdm_trigger_cmd cmd);

typedef int (*dfsdm_api_read_t)(const struct device *dev, uint8_t filter_id,
				void *buf,          /* Destination buffer (User owned) */
				size_t size,        /* Size of destination buffer */
				size_t *bytes_read, /* Actual bytes copied */
				k_timeout_t timeout);

__subsystem struct dfsdm_driver_api {
	dfsdm_api_configure_channel_t configure_channel;
	dfsdm_api_configure_stream_t configure_stream;
	dfsdm_api_configure_watchdog_t configure_watchdog;
	dfsdm_api_trigger_t trigger;
	dfsdm_api_read_t read;
};

/* ------------------------------------------------ */
/* Public Helper Functions                          */
/* ------------------------------------------------ */

static inline int dfsdm_configure_channel(const struct device *dev,
					  const struct dfsdm_channel_config *cfg)
{
	const struct dfsdm_driver_api *api = (const struct dfsdm_driver_api *)dev->api;
	return api->configure_channel(dev, cfg);
}

static inline int dfsdm_configure_stream(const struct device *dev,
					 const struct dfsdm_stream_config *cfg)
{
	const struct dfsdm_driver_api *api = (const struct dfsdm_driver_api *)dev->api;
	return api->configure_stream(dev, cfg);
}

static inline int dfsdm_configure_watchdog(const struct device *dev,
					   const struct dfsdm_watchdog_config *cfg)
{
	const struct dfsdm_driver_api *api = (const struct dfsdm_driver_api *)dev->api;
	if (api->configure_watchdog == NULL) {
		return -ENOTSUP;
	}
	return api->configure_watchdog(dev, cfg);
}

static inline int dfsdm_trigger(const struct device *dev, uint8_t filter_id,
				enum dfsdm_trigger_cmd cmd)
{
	const struct dfsdm_driver_api *api = (const struct dfsdm_driver_api *)dev->api;
	return api->trigger(dev, filter_id, cmd);
}

static inline int dfsdm_read(const struct device *dev, uint8_t filter_id, void *buffer, size_t size,
			     size_t *bytes_read, k_timeout_t timeout)
{
	const struct dfsdm_driver_api *api = (const struct dfsdm_driver_api *)dev->api;
	return api->read(dev, filter_id, buffer, size, bytes_read, timeout);
}

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_DFSDM_H_ */
