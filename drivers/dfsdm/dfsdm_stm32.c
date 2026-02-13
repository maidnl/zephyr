/*
 * Copyright (c) 2026 Arduino (Daniele Aimo)
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_stm32_dfsdm_filter

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/dfsdm.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <stm32h7xx.h>
#include <zephyr/cache.h>
#include <soc.h>
#include <stdbool.h>

#define DFSDMCh_t DFSDM_Channel_TypeDef

LOG_MODULE_REGISTER(dfsdm_stm32, CONFIG_DFSDM_LOG_LEVEL);

#define DFSDM_CHANNEL_SIZE 0x20
#define DFSDM_FILTER_SIZE  0x80

/* According to reference manual for STMH7:
 * "Bits 31:8 RDATA[23:0]: Regular channel conversion data"
 * So data taken from RDATAR register must be right shifted of 8 positions */
#define DFSDM_DATA_RSHIFT 8

static int dfsdm_configure_and_start_dma(const struct device *dev);

/* ------------------------------------------------ */
/* Driver Structures                                */
/* ------------------------------------------------ */

struct dfsdm_stm32_config {
	uint32_t filter_base_address;
	uint32_t channel_base_address;

	/* DMA Config */
	const struct device *dma_dev;
	uint32_t dma_channel;
	uint32_t dma_slot;

	/* Hardware Resources */
	const struct pinctrl_dev_config *pcfg;
	struct stm32_pclken pclken;
	void (*irq_config_func)(const struct device *dev);
};

struct dfsdm_stm32_data {
	struct dfsdm_stream_config stream_cfg;
	struct dfsdm_watchdog_config watchdog_cfg;

	struct k_msgq sync_msgq;
	char __aligned(4) sync_msgq_buffer[sizeof(void *) * 2];

	/* 20260212 - USING CYCLIC DMA  */
	struct dma_block_config block;
	void *dma_buffers[2];
	void *current_buffer;
	uint8_t current_buffer_id;
	uint32_t current_buffer_pos;
	size_t buffer_size;

	bool dma_active;
};

/* ------------------------------------------------ */
/* Helper Functions                                 */
/* ------------------------------------------------ */

static uint32_t map_sinc_type(enum dfsdm_sinc_type type)
{
	switch (type) {
	case DFSDM_SINC_FAST:
		return 0;
	case DFSDM_SINC_1:
		return 1;
	case DFSDM_SINC_2:
		return 2;
	case DFSDM_SINC_3:
		return 3;
	case DFSDM_SINC_4:
		return 4;
	case DFSDM_SINC_5:
		return 5;
	default:
		return 3;
	}
}

static uint32_t map_channel_type(enum dfsdm_channel_type type)
{
	switch (type) {
	case DFSDM_TYPE_SPI_RISING:
		return 0;
	case DFSDM_TYPE_SPI_FALLING:
		return 1;
	case DFSDM_TYPE_MANCHESTER_RISING:
		return 2;
	case DFSDM_TYPE_MANCHESTER_FALLING:
		return 3;
	default:
		return 0;
	}
}

/**
 * @brief Takes current_buffer from data, prepare it for users applying required
 *        required right shift, then (if defined) calls user's callback.
 *        If the callback is not defined the function tries to unlock the queue
 *        where read function is supposed to wait.
 *        The function also prepare DMA for next transfer "rotating" the data buffer.
 *
 *
 * @param data -> pointer to a dfsm fiter device
 */
static void dfsdm_call_user_cb(const struct device *dev)
{
	struct dfsdm_stm32_data *data = dev->data;
	/* invalidate cache */
	if (data->stream_cfg.invalidate_cache && data->dma_active) {
		sys_cache_data_invd_range(data->current_buffer, data->buffer_size);
	}

	/* filter ramp up filter that might cause loud "click" at the beginning
	 * of the recording */
	if (data->stream_cfg.discard_first_n_block > 0) {
		data->stream_cfg.discard_first_n_block--;
		data->current_buffer_id = (++data->current_buffer_id) % 2;
		data->current_buffer = data->dma_buffers[data->current_buffer_id];
		data->current_buffer_pos = 0;
		return;
	}

	/* data are stored in the 24 most high bit so... shift right by 8 */
	int32_t *fb = (int32_t *)data->current_buffer;
	for (int i = 0; i < (data->buffer_size / sizeof(uint32_t)); i++) {
		*(fb + i) = *(fb + i) >> DFSDM_DATA_RSHIFT;
	}

	if (data->stream_cfg.invalidate_cache && data->dma_active) {
		sys_cache_data_flush_range(data->current_buffer, data->buffer_size);
	}

	/* call callback (if defined) */
	if (data->stream_cfg.callback) {
		data->stream_cfg.callback(dev, data->stream_cfg.filter.filter_id,
					  data->current_buffer, data->buffer_size,
					  data->stream_cfg.user_data);
	} else {
		/* here we do not free the slab, the read will do when called */
		if (k_msgq_put(&data->sync_msgq, &data->current_buffer, K_NO_WAIT) != 0) {
			LOG_WRN("Read queue full, dropping block");
		}
	}
	data->current_buffer_id = (++data->current_buffer_id) % 2;
	data->current_buffer = data->dma_buffers[data->current_buffer_id];
	data->current_buffer_pos = 0;
}

/**
 * @brief Callback called when DMA finishes the trasfert from DFSDM filter to
 * kslab current buffer
 *
 * @param dma_dev Pointer to the device structure for DMA driver instance
 * @param pointer to dma user data
 * @param dma channel
 * @status reason for callback
 */
static void dfsdm_dma_callback(const struct device *dma_dev, void *user_data, uint32_t channel,
			       int status)
{
	const struct device *dev = (const struct device *)user_data;
	struct dfsdm_stm32_data *data = dev->data;
	const struct dfsdm_stm32_config *dev_cfg = dev->config;

	/* safety checks */
	if (dev_cfg->dma_channel != channel) {
		return;
	}
	if (status != DMA_STATUS_BLOCK && status != DMA_STATUS_COMPLETE) {
		return;
	}

	if (!data->dma_active) {
		return;
	}
	dfsdm_call_user_cb(dev);
}

/**
 * @brief Configure and start the DMA
 *
 * @param dev Pointer to the device structure for DFSDM driver instance
 *
 * @retval 0 if successful.
 * @retval Negative errno code if failure.
 */
static int dfsdm_configure_and_start_dma(const struct device *dev)
{
	const struct dfsdm_stm32_config *dev_cfg = dev->config;
	DFSDM_Filter_TypeDef *flt_reg = (DFSDM_Filter_TypeDef *)dev_cfg->filter_base_address;
	struct dfsdm_stm32_data *data = dev->data;
	data->dma_active = true;

	data->current_buffer_id = 0;
	data->current_buffer = data->dma_buffers[data->current_buffer_id];
	data->current_buffer_pos = 0;

	/* 20260212 - USING CYCLIC DMA  */
	data->block.block_size = data->buffer_size * 2;
	data->block.dest_address = (uint32_t)data->dma_buffers[0];
	data->block.source_address = (uint32_t)&flt_reg->FLTRDATAR;
	data->block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	data->block.source_reload_en = 1;
	data->block.dest_reload_en = 1;

	struct dma_config dma_cfg = {
		.channel_direction = PERIPHERAL_TO_MEMORY,
		.source_data_size = 4,
		.dest_data_size = 4,
		.source_burst_length = 1,
		.dest_burst_length = 1,
		.dma_slot = dev_cfg->dma_slot,
		.head_block = &data->block,
		.dma_callback = dfsdm_dma_callback,
		.user_data = (void *)dev,
		.cyclic = 1,
	};
	int err = dma_config(dev_cfg->dma_dev, dev_cfg->dma_channel, &dma_cfg);
	if (err < 0) {
		LOG_ERR("ERROR: unable to configure DMA (err = %i)\n", err);
		return err;
	}
	return dma_start(dev_cfg->dma_dev, dev_cfg->dma_channel);
}

static void dfsdm_stm32_isr(const struct device *dev)
{
	const struct dfsdm_stm32_config *dev_cfg = dev->config;
	struct dfsdm_stm32_data *data = dev->data;
	DFSDM_Filter_TypeDef *flt_reg = (DFSDM_Filter_TypeDef *)dev_cfg->filter_base_address;
	DFSDMCh_t *ch0 = (DFSDMCh_t *)(dev_cfg->channel_base_address);
	/* Specific channel address */
	DFSDMCh_t *curr_ch = ch0 + data->watchdog_cfg.channel_id;

	if (!dev_cfg->dma_dev) {
		if ((flt_reg->FLTISR & DFSDM_FLTISR_REOCF) &&
		    (flt_reg->FLTCR2 & DFSDM_FLTCR2_REOCIE)) {
			uint32_t val = flt_reg->FLTRDATAR;
			if (data->current_buffer) {
				uint32_t max_pos = (data->buffer_size / sizeof(uint32_t));
				uint32_t pos = data->current_buffer_pos;
				uint32_t *ptr = (uint32_t *)data->current_buffer;
				if (pos < max_pos) {
					ptr[pos] = val;
				}
				pos++;
				data->current_buffer_pos = pos;
				if (pos >= max_pos) {
					dfsdm_call_user_cb(dev);
				}
			}
		}
	}
	if ((flt_reg->FLTISR & DFSDM_FLTISR_AWDF) && (flt_reg->FLTCR2 & DFSDM_FLTCR2_AWDIE)) {
		uint32_t active_flags = flt_reg->FLTAWSR;
		flt_reg->FLTAWCFR = active_flags;
		uint32_t ch_mask = (1 << data->watchdog_cfg.channel_id);
		{
			if (data->watchdog_cfg.cb) {
				int32_t actual_value = 0;
				if (data->watchdog_cfg.use_low_resolution_watchdog) {
					actual_value = (int32_t)curr_ch->CHWDATAR;
				} else {
					actual_value =
						((int32_t)flt_reg->FLTRDATAR) >> DFSDM_DATA_RSHIFT;
				}
				data->watchdog_cfg.cb(dev, data->watchdog_cfg.channel_id,
						      actual_value, DFSDM_WATCHDOG_THRESHOLD);
			}
		}
	}

	if ((flt_reg->FLTISR & DFSDM_FLTISR_SCDF) && (flt_reg->FLTCR2 & DFSDM_FLTCR2_SCDIE)) {
		flt_reg->FLTICR =
			((1 << data->watchdog_cfg.channel_id) << DFSDM_FLTICR_CLRSCDF_Pos);
		if (data->watchdog_cfg.cb) {
			data->watchdog_cfg.cb(dev, data->watchdog_cfg.channel_id, 0,
					      DFSDM_WATCHDOG_SATURATION);
		}
	}
}

static int dfsdm_stm32_configure_channel(const struct device *dev,
					 const struct dfsdm_channel_config *cfg)
{

	static uint8_t configuration_num = 0;
	/* Configuration */
	const struct dfsdm_stm32_config *dev_cfg = dev->config;
	/* Base channel address - for global configuration and enable */
	DFSDMCh_t *ch0 = (DFSDMCh_t *)(dev_cfg->channel_base_address);
	/* Specific channel address */
	DFSDMCh_t *curr_ch = ch0 + cfg->channel_id;

	if (configuration_num == 0) {
		ch0->CHCFGR1 &= ~DFSDM_CHCFGR1_DFSDMEN;
		/* Internal clock setting configuration */
		if (cfg->clock_mode != DFSDM_CLOCK_EXTERNAL) {
			uint32_t clk;
			if (cfg->clock_source == DFSDM_INT_CLOCK_SOURCE_0) { // SYS CLK
				clock_control_get_rate(DEVICE_DT_GET(DT_NODELABEL(rcc)),
						       (clock_control_subsys_t)&dev_cfg->pclken,
						       &clk);
			} else if (cfg->clock_source == DFSDM_INT_CLOCK_SOURCE_1) { // AUDIO CLK
				// TODO : how to get the audio ck frequency???
				// ---> calculate the correct clock
				ch0->CHCFGR1 |= DFSDM_CHCFGR1_CKOUTSRC;
			} else {
				return -ENOTSUP;
			}
			uint32_t div = clk / cfg->output_clock_rate;
			if (div < 1) {
				div = 1;
			} else if (div > 256) {
				div = 256;
			}
			uint32_t reg_val = ch0->CHCFGR1 & ~(DFSDM_CHCFGR1_CKOUTDIV_Msk);
			reg_val |= ((div - 1) << DFSDM_CHCFGR1_CKOUTDIV_Pos);
			ch0->CHCFGR1 = reg_val;
		}
		/* Enable DFSDM */
		ch0->CHCFGR1 |= DFSDM_CHCFGR1_DFSDMEN;
		configuration_num = 0xFF; /* never configure again global properties */
	}

	/* DISABLE CURRENT CHANNEL CHANNEL */
	curr_ch->CHCFGR1 &= ~DFSDM_CHCFGR1_CHEN;

	uint32_t datmpx = (cfg->source == DFSDM_SOURCE_INTERNAL_ADC) ? 1
			  : (cfg->source == DFSDM_SOURCE_PARALLEL)   ? 2
								     : 0;
	uint32_t sitp = map_channel_type(cfg->type);
	uint32_t spicksel = (cfg->clock_mode == DFSDM_CLOCK_INTERNAL)        ? 1
			    : (cfg->clock_mode == DFSDM_CLOCK_INTERNAL_DIV2) ? 2
									     : 0;
	uint32_t reg = curr_ch->CHCFGR1;

	reg &= ~(DFSDM_CHCFGR1_DATMPX | DFSDM_CHCFGR1_SITP | DFSDM_CHCFGR1_SPICKSEL);
	reg |= (datmpx << DFSDM_CHCFGR1_DATMPX_Pos) | (sitp << DFSDM_CHCFGR1_SITP_Pos) |
	       (spicksel << DFSDM_CHCFGR1_SPICKSEL_Pos);

	curr_ch->CHCFGR1 = reg;

	curr_ch->CHCFGR2 &= ~(DFSDM_CHCFGR2_OFFSET | DFSDM_CHCFGR2_DTRBS);
	curr_ch->CHCFGR2 |= ((cfg->offset << DFSDM_CHCFGR2_OFFSET_Pos) & DFSDM_CHCFGR2_OFFSET);
	curr_ch->CHCFGR2 |=
		((cfg->right_bit_shift << DFSDM_CHCFGR2_DTRBS_Pos) & DFSDM_CHCFGR2_DTRBS);

	/* RE-ENABLE CHANNEL */
	curr_ch->CHCFGR1 |= DFSDM_CHCFGR1_CHEN;

	return 0;
}

static int dfsdm_stm32_configure_stream(const struct device *dev,
					const struct dfsdm_stream_config *cfg)
{
	struct dfsdm_stm32_data *data = dev->data;
	const struct dfsdm_stm32_config *dev_cfg = dev->config;
	DFSDM_Filter_TypeDef *flt_reg = (DFSDM_Filter_TypeDef *)dev_cfg->filter_base_address;

	data->stream_cfg = *cfg;
	flt_reg->FLTCR1 &= ~DFSDM_FLTCR1_DFEN;

	uint32_t fcr = flt_reg->FLTFCR;
	fcr &= ~(DFSDM_FLTFCR_FORD | DFSDM_FLTFCR_FOSR | DFSDM_FLTFCR_IOSR);
	fcr |= (map_sinc_type(cfg->filter.sinc_type) << DFSDM_FLTFCR_FORD_Pos);
	fcr |= ((cfg->filter.oversampling - 1) << DFSDM_FLTFCR_FOSR_Pos);
	fcr |= (cfg->filter.integrator << DFSDM_FLTFCR_IOSR_Pos);
	flt_reg->FLTFCR = fcr;

	flt_reg->FLTCR1 &= ~DFSDM_FLTCR1_RCH;
	flt_reg->FLTCR1 |= (cfg->filter.channel_id << DFSDM_FLTCR1_RCH_Pos) | DFSDM_FLTCR1_RCONT |
			   DFSDM_FLTCR1_FAST;

	if (dev_cfg->dma_dev) {
		flt_reg->FLTCR1 |= DFSDM_FLTCR1_RDMAEN;
	} else {
		flt_reg->FLTCR1 &= ~DFSDM_FLTCR1_RDMAEN;
	}

	return 0;
}

static int dfsdm_stm32_configure_watchdog(const struct device *dev,
					  const struct dfsdm_watchdog_config *cfg)
{
	if (cfg->filter_id >= 4) {
		return -EINVAL;
	}

	if (cfg->channel_id >= 8) {
		return -EINVAL;
	}

	struct dfsdm_stm32_data *data = dev->data;
	const struct dfsdm_stm32_config *dev_cfg = dev->config;
	DFSDM_Filter_TypeDef *flt_reg = (DFSDM_Filter_TypeDef *)dev_cfg->filter_base_address;
	/* Base channel address - for global configuration and enable */
	DFSDMCh_t *ch0 = (DFSDMCh_t *)(dev_cfg->channel_base_address);
	/* Specific channel address */
	DFSDMCh_t *curr_ch = ch0 + cfg->channel_id;

	if (!cfg->use_low_resolution_watchdog && data->stream_cfg.filter.oversampling == 0) {
		LOG_ERR("Regular watchdog requires a configured stream. Configure stream first or "
			"use low-res watchdog.");
		return -EINVAL;
	}

	/* DISABLE CURRENT CHANNEL CHANNEL */
	curr_ch->CHCFGR1 &= ~DFSDM_CHCFGR1_CHEN;
	uint32_t flt_value = flt_reg->FLTCR1;
	uint32_t ch_value = curr_ch->CHAWSCDR;

	if (cfg->use_low_resolution_watchdog) {
		flt_value |= DFSDM_FLTCR1_AWFSEL;
		uint32_t sync = map_sinc_type(cfg->low_res_config.sync);
		if (sync > 3) {
			sync = 3;
		}
		ch_value |= (sync << DFSDM_CHAWSCDR_AWFORD_Pos);
		uint32_t oversampling = cfg->low_res_config.oversampling;

		if (oversampling > 31) {
			oversampling = 31;
		}
		ch_value |= (oversampling << DFSDM_CHAWSCDR_AWFOSR_Pos);
	} else {
		flt_value &= ~DFSDM_FLTCR1_AWFSEL;
	}
	uint32_t high_val = (uint32_t)cfg->high_threshold & 0x00FFFFFF;
	uint32_t low_val = (uint32_t)cfg->low_threshold & 0x00FFFFFF;
	flt_reg->FLTAWHTR = (high_val << DFSDM_FLTAWHTR_AWHT_Pos) & DFSDM_FLTAWHTR_AWHT;
	flt_reg->FLTAWLTR = (low_val << DFSDM_FLTAWLTR_AWLT_Pos) & DFSDM_FLTAWLTR_AWLT;
	flt_reg->FLTCR1 = flt_value;
	flt_reg->FLTCR2 |= ((1 << cfg->channel_id) << DFSDM_FLTCR2_AWDCH_Pos);
	ch_value |= (cfg->short_circuit_counter & 0xFF);
	curr_ch->CHAWSCDR = ch_value;

	if (cfg->enable_analog_watchdog) {
		flt_reg->FLTCR2 |= DFSDM_FLTCR2_AWDIE;
	} else {
		flt_reg->FLTCR2 &= ~DFSDM_FLTCR2_AWDIE;
	}

	if (cfg->enable_short_circuit_detect) {
		curr_ch->CHCFGR1 |= DFSDM_CHCFGR1_SCDEN;
		flt_reg->FLTCR2 |= DFSDM_FLTCR2_SCDIE;
	} else {
		curr_ch->CHCFGR1 &= ~DFSDM_CHCFGR1_SCDEN;
		flt_reg->FLTCR2 &= ~DFSDM_FLTCR2_SCDIE;
	}
	/* RE-ENABLE CHANNEL */
	curr_ch->CHCFGR1 |= DFSDM_CHCFGR1_CHEN;
	data->watchdog_cfg = *cfg;

	return 0;
}

/**
 * @brief Start and stop the filter
 *
 * It is supposed that configuration function are called before to start the
 * filter
 *
 * In case of START request the function grab a slab and then
 * - if DMA is define in DT -> starts the DMA
 * - if DMA is not defined in DT -> enables the filter IRQ
 * - actually start the filter
 */
static int dfsdm_stm32_trigger(const struct device *dev, uint8_t filter_id,
			       enum dfsdm_trigger_cmd cmd)
{
	const struct dfsdm_stm32_config *dev_cfg = dev->config;
	struct dfsdm_stm32_data *data = dev->data;
	DFSDM_Filter_TypeDef *flt_reg = (DFSDM_Filter_TypeDef *)dev_cfg->filter_base_address;

	switch (cmd) {
	case DFSDM_TRIGGER_START:
		/* safety check -> this could happen if we use only watchdog and not regular
		 * streaming*/
		if (data->stream_cfg.data_buffer != NULL && data->stream_cfg.data_buffer_size > 0) {
			/* ---- GRAB BUFFER ---- */
			uint32_t *ptr = data->stream_cfg.data_buffer;
			/* we use just half buffer per time */
			data->buffer_size = (data->stream_cfg.data_buffer_size / 2);

			data->dma_buffers[0] = (void *)ptr;
			ptr = ptr + (data->buffer_size / sizeof(uint32_t));
			data->dma_buffers[1] = (void *)ptr;
			/* with DMA */
			if (dev_cfg->dma_dev) {
				/* ---- START DMA --- */
				int err = dfsdm_configure_and_start_dma(dev);
				if (err < 0) {
					return err;
				}
			} else { /* without DMA */
				data->current_buffer = data->dma_buffers[0];
				data->current_buffer_pos = 0;
				data->current_buffer_id = 0;
				/* ---- ENABLE FILTER IRQ --- */
				flt_reg->FLTCR2 |= DFSDM_FLTCR2_REOCIE;
			}
		}
		/* ---- ENABLE THE FILTER ---- */
		flt_reg->FLTCR1 |= DFSDM_FLTCR1_DFEN | DFSDM_FLTCR1_RSWSTART;
		break;
	case DFSDM_TRIGGER_STOP:
		/* ----- STOP THE FILTER ---- */
		flt_reg->FLTCR1 &= ~(DFSDM_FLTCR1_RSWSTART | DFSDM_FLTCR1_DFEN);
		if (data->stream_cfg.data_buffer != NULL) {
			/* STOP either the DMA or the FILTER IRQ */
			if (dev_cfg->dma_dev) {
				data->dma_active = false;
				dma_stop(dev_cfg->dma_dev, dev_cfg->dma_channel);
			} else {
				flt_reg->FLTCR2 &= ~DFSDM_FLTCR2_REOCIE;
			}
			data->dma_buffers[0] = NULL;
			data->dma_buffers[1] = NULL;
			data->current_buffer = NULL;
		}
		break;

	default:
		return -ENOTSUP;
	}

	return 0;
}

static int dfsdm_stm32_read(const struct device *dev, uint8_t filter_id, void *user_buf,
			    size_t buf_size, size_t *bytes_read, k_timeout_t timeout)
{
	struct dfsdm_stm32_data *data = dev->data;
	if (data->stream_cfg.callback != NULL) {
		return -EINVAL;
	}
	void *data_ptr = NULL;
	int ret = k_msgq_get(&data->sync_msgq, &data_ptr, timeout);

	size_t cpy_size = (buf_size <= data->buffer_size) ? buf_size : data->buffer_size;

	memcpy(user_buf, data_ptr, cpy_size);
	*bytes_read = cpy_size;

	if (ret == 0) {
		return 0;
	}
	return -ETIMEDOUT;
}

static const struct dfsdm_driver_api dfsdm_stm32_api = {
	.configure_channel = dfsdm_stm32_configure_channel,
	.configure_stream = dfsdm_stm32_configure_stream,
	.configure_watchdog = dfsdm_stm32_configure_watchdog,
	.trigger = dfsdm_stm32_trigger,
	.read = dfsdm_stm32_read,
};

static int dfsdm_stm32_init(const struct device *dev)
{
	const struct dfsdm_stm32_config *cfg = dev->config;
	/* Enable Clocks */
	if (clock_control_on(DEVICE_DT_GET(DT_NODELABEL(rcc)),
			     (clock_control_subsys_t)&cfg->pclken) < 0) {
		printk("[ERROR]: failed to enable clock\n");
		return -EIO;
	}

	/* Configure Pins (Using Parent's Config) */
	/* This will return -2 if Parent's Pinctrl struct is empty or not found */
	int err = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (err < 0) {
		printk("[ERROR]: failed to apply pinctrl state -> %i\n", err);
		return -EIO;
	}

	/* Enable IRQ (Used for Watchdog OR ISR mode) */
	cfg->irq_config_func(dev);
	return 0;
}

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT st_stm32_dfsdm

#define DEFINE_DFSDM_PARENT_PINCTRL(n) PINCTRL_DT_INST_DEFINE(n);
DT_INST_FOREACH_STATUS_OKAY(DEFINE_DFSDM_PARENT_PINCTRL)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT st_stm32_dfsdm_filter
/* ------------------------------------------------ */
/* Macro Initialization                             */
/* ------------------------------------------------ */

#define DFSDM_STM32_INIT(n)                                                                        \
	/* DEFINE ISR */                                                                           \
	static void dfsdm_config_func_##n(const struct device *dev)                                \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), dfsdm_stm32_isr,            \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	/* DEFINE DATA */                                                                          \
	static struct dfsdm_stm32_data dfsdm_data_##n = {                                          \
		.sync_msgq =                                                                       \
			Z_MSGQ_INITIALIZER(dfsdm_data_##n.sync_msgq,                               \
					   dfsdm_data_##n.sync_msgq_buffer, sizeof(void *), 2),    \
	};                                                                                         \
                                                                                                   \
	/* DEFINE CONFIG */                                                                        \
	static const struct dfsdm_stm32_config dfsdm_config_##n = {                                \
		.filter_base_address =                                                             \
			DT_REG_ADDR(DT_INST_PARENT(n)) + 0x100 + DT_INST_REG_ADDR(n),              \
		.channel_base_address = DT_REG_ADDR(DT_INST_PARENT(n)),                            \
                                                                                                   \
		/* DMA Macros */                                                                   \
		.dma_dev = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, dmas),                                     \
                              (DEVICE_DT_GET(DT_INST_DMAS_CTLR(n))), (NULL)),                             \
			 .dma_channel = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, dmas),                                 \
                                  (DT_INST_DMAS_CELL_BY_NAME(n, rx, channel)), (0)),                   \
				  .dma_slot = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, dmas),                                    \
                               (DT_INST_DMAS_CELL_BY_NAME(n, rx, slot)), (0)),             \
                                                                                                   \
					   /* PINCTRL FIX: Use DEV getter with Parent Node ID */   \
					   .pcfg = PINCTRL_DT_DEV_CONFIG_GET(DT_INST_PARENT(n)),   \
                                                                                                   \
					   /* Clocks from Parent */                                \
					   .pclken = {.bus = DT_CLOCKS_CELL(DT_INST_PARENT(n),     \
									    bus),                  \
						      .enr = DT_CLOCKS_CELL(DT_INST_PARENT(n),     \
									    bits)},                \
                                                                                                   \
					   .irq_config_func = dfsdm_config_func_##n,               \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, dfsdm_stm32_init, NULL, &dfsdm_data_##n, &dfsdm_config_##n,       \
			      POST_KERNEL, CONFIG_DFSDM_INIT_PRIORITY, &dfsdm_stm32_api);

DT_INST_FOREACH_STATUS_OKAY(DFSDM_STM32_INIT)
