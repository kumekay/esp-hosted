// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2021 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/** Includes **/
#include "sdio_reg.h"
#include "sdio_api.h"
#include "sdio_host.h"
#include "sdio_ll.h"
#include "trace.h"
#include "os_wrapper.h"
#include "esp_log.h"

static const char TAG[] = "SDIO_HOST";

/** Macros/Constants **/
#define CHECK_SDIO_ERR(ErR) {\
	if (ErR) { \
		ESP_LOGE(TAG,"%s: %u err %u\r\n",__func__,__LINE__,ErR); \
		return ErR; \
	} \
}

/** Global Variable **/
/* Counter to hold the amount of buffers already sent to sdio slave */
static uint32_t sdio_esp_tx_bytes = 0;

/* Counter to hold the amount of bytes already received from sdio slave */
static uint32_t sdio_esp_rx_bytes   = 0;

/** Functions Declaration **/

/** SDIO slave initialization  **/
static stm_ret_t esp_slave_init_io(void);
static uint32_t esp_sdio_host_get_buffer_size(void);
static stm_ret_t esp_sdio_slave_get_rx_data_size(uint32_t* rx_size);

/** Functions Defination **/

/**
 * @brief  SDIO slave initialization
 * @param  None
 * @retval STM_OK for success or failure from enum stm_ret_t
 */
static stm_ret_t esp_slave_init_io(void)
{
	uint8_t ioe = 0, ior = 0, ie = 0;
	uint8_t bsl = 0, bsh = 0;
	uint8_t func1_bsl = 0, func1_bsh = 0;
	uint8_t func2_bsl = 0, func2_bsh = 0;

	CHECK_SDIO_ERR(sdio_driver_read_byte(SDIO_FUNC_0, SD_IO_CCCR_FN_ENABLE, &ioe));
	ESP_LOGD(TAG,"IOE: 0x%02x", ioe);

	CHECK_SDIO_ERR(sdio_driver_read_byte(SDIO_FUNC_0, SD_IO_CCCR_FN_READY, &ior));
	ESP_LOGD(TAG,"IOR: 0x%02x", ior);

	// enable function 1
	ioe = 6;
	CHECK_SDIO_ERR(sdio_driver_write_byte(SDIO_FUNC_0, SD_IO_CCCR_FN_ENABLE, ioe, &ioe));
	ESP_LOGD(TAG,"IOE: 0x%02x", ioe);

	ior = 6;
	CHECK_SDIO_ERR(sdio_driver_write_byte(SDIO_FUNC_0, SD_IO_CCCR_FN_READY, ioe, &ior));
	ESP_LOGD(TAG,"IOE: 0x%02x", ior);

	// get interrupt status
	CHECK_SDIO_ERR(sdio_driver_read_byte(SDIO_FUNC_0, SD_IO_CCCR_INT_ENABLE, &ie));
	ESP_LOGD(TAG,"IE: 0x%02x", ie);

	// enable interrupts for function 1&2 and master enable
	ie = 7;
	CHECK_SDIO_ERR(sdio_driver_write_byte(SDIO_FUNC_0, SD_IO_CCCR_INT_ENABLE, ie, &ie));
	ESP_LOGD(TAG,"IE: 0x%02x", ie);

	CHECK_SDIO_ERR(sdio_driver_write_byte(SDIO_FUNC_0, SD_IO_CCCR_BLKSIZEL, bsl, &bsl));
	ESP_LOGD(TAG,"Function 0 BSL: 0x%02x", bsl);

	bsh = 2;
	CHECK_SDIO_ERR(sdio_driver_write_byte(SDIO_FUNC_0, SD_IO_CCCR_BLKSIZEH, bsh, &bsh));
	ESP_LOGD(TAG,"Function 0 BSH: 0x%02x", bsh);

	CHECK_SDIO_ERR(sdio_driver_write_byte(SDIO_FUNC_0, 0x110, func1_bsl, &func1_bsl));
	ESP_LOGD(TAG,"Function 1 BSL: 0x%02x",  func1_bsl);

	func1_bsh = 2;         // Set block size 512 (0x200)
	CHECK_SDIO_ERR(sdio_driver_write_byte(SDIO_FUNC_0, 0x111, func1_bsh, &func1_bsh));
	ESP_LOGD(TAG,"Function 1 BSH: 0x%02x", func1_bsh);

	CHECK_SDIO_ERR(sdio_driver_write_byte(SDIO_FUNC_0, 0x210, func2_bsl, &func2_bsl));
	ESP_LOGD(TAG,"Function 2 BSL: 0x%02x", func2_bsl);

	func2_bsh = 2;
	CHECK_SDIO_ERR(sdio_driver_write_byte(SDIO_FUNC_0, 0x210, func2_bsh, &func2_bsh));
	ESP_LOGD(TAG,"Function 2 BSH: 0x%02x", func2_bsh);
	ESP_LOGI(TAG,"SDIO Slave Initialization completed");
	return STM_OK;
}

/**
 * @brief  host use this to initialize the slave as well as SDIO register
 * @param  None
 * @retval STM_OK for success or failure from enum stm_ret_t
 */
stm_ret_t sdio_host_init(void)
{
	CHECK_SDIO_ERR(sdio_driver_init());

	CHECK_SDIO_ERR(esp_slave_init_io());

	return STM_OK;
}

/** receive functions **/
/**
 * @brief  HOST receive data
 * @param  rx_size - read data size
 * @retval STM_OK for success or failure from enum stm_ret_t
 */
static stm_ret_t esp_sdio_slave_get_rx_data_size(uint32_t* rx_size)
{
	uint32_t len = 0, temp = 0;
	stm_ret_t err = sdio_driver_read_bytes(SDIO_FUNC_1,
			SDIO_REG(ESP_SLAVE_PACKET_LEN_REG), &len, 4, 0);
	if (err) {
		ESP_LOGE(TAG,"Err while reading ESP_SLAVE_PACKET_LEN_REG");
		return err;
	}
	len &= ESP_SLAVE_LEN_MASK;
	if (len >= sdio_esp_rx_bytes) {
		len = (len + ESP_RX_BYTE_MAX - sdio_esp_rx_bytes)%ESP_RX_BYTE_MAX;
	} else {
		temp = ESP_RX_BYTE_MAX - sdio_esp_rx_bytes;
		len = temp + len;
		if (len > MAX_SDIO_BUFFER_SIZE) {
			ESP_LOGE(TAG,"Len from slave[%lu] exceeds max [%d]\n",
					len, MAX_SDIO_BUFFER_SIZE);
		}
	}
#if 0
       /* length is expected to be in multiple of ESP_BLOCK_SIZE */
       if(len&(ESP_BLOCK_SIZE-1))
               return STM_FAIL;
#endif

	if (rx_size)
		*rx_size = len;
	return STM_OK;
}

/**
 * @brief  Get a packet from SDIO slave
 * @param  [out] out_data - Data output address
 *         size - The size of the output buffer,
 *                if the buffer is smaller than
 *                the size of data to receive from slave,
 *                the driver returns ESP_ERR_NOT_FINISHED
 *         [out] out_length - Output of length the data received from slave
 *         wait_ms - Time to wait before timeout, in ms
 * @retval STM_OK for success or failure from enum stm_ret_t
 */
stm_ret_t sdio_host_get_packet(void* out_data, size_t size,
		size_t* out_length, uint32_t wait_ms)
{
	stm_ret_t err = STM_OK;
	uint32_t len = 0, wait_time = 0, len_remain = 0;
	uint8_t* start_ptr = NULL;
	int len_to_send = 0, block_n = 0;

	if (size <= 0) {
		ESP_LOGE(TAG,"Invalid size:%d", size);
		return STM_FAIL_INVALID_ARG;
	}

	for (;;) {
		err = esp_sdio_slave_get_rx_data_size(&len);

		if (err == STM_OK && len > 0) {
			ESP_LOGW(TAG,"Expected length to be read %lu",len);
			break;
		}

		/* If no error and no data, retry */
		wait_time++;

		if (wait_time >= wait_ms) {
			return STM_FAIL_TIMEOUT;
		}

		hard_delay(1);
	}

	if (len > size) {
		ESP_LOGE(TAG,"Pkt size to be read[%lu] > max sdio size supported[%u]",len, size);
		return STM_OK;
	}

	len_remain = len;
	start_ptr = (uint8_t*)out_data;

	do {
		/* currently driver supports only block size of 512 */

		block_n = len_remain / ESP_BLOCK_SIZE;

		if (block_n != 0) {
			len_to_send = ESP_BLOCK_SIZE;
			ESP_LOGV(TAG,"block_n %u, len-to_send %lu",block_n,len_to_send);

			err = sdio_driver_read_blocks(SDIO_FUNC_1,
					ESP_SLAVE_CMD53_END_ADDR - len_remain,
					start_ptr, len_to_send, block_n);
		} else {
			len_to_send = len_remain;
			/* though the driver supports to split packet of unaligned size into length
			 * of 4x and 1~3, we still get aligned size of data to get higher
			 * efficiency. The length is determined by the SDIO address, and the
			 * remaining will be ignored by the slave hardware
			 */
			err = sdio_driver_read_bytes(SDIO_FUNC_1,
					ESP_SLAVE_CMD53_END_ADDR - len_remain, start_ptr,
					(len_to_send + 3) & (~3), block_n);
		}

		if (err) {
			ESP_LOGE(TAG,"Err from read bytes %x",err);
			return err;
		}

		start_ptr += len_to_send;
		len_remain -= len_to_send;
	} while (len_remain != 0);

	*out_length = len;
	sdio_esp_rx_bytes += len;
	if (sdio_esp_rx_bytes >= ESP_RX_BYTE_MAX) {
		sdio_esp_rx_bytes -= ESP_RX_BYTE_MAX;
	}

	return STM_OK;
}

/**
 * @brief  Clear interrupt bits of SDIO slave
 * @param  intr_mask - Mask of interrupt bits to clear
 * @retval STM_OK for success or failure from enum stm_ret_t
 */
stm_ret_t sdio_host_clear_intr(uint32_t intr_mask)
{
	return sdio_driver_write_bytes(SDIO_FUNC_1,
			SDIO_REG(ESP_SLAVE_INT_CLR_REG), (uint8_t*)&intr_mask, 4);
}

/**
 * @brief  Get interrupt bits of SDIO slave
 *
 * @param  intr_st - Output of the masked interrupt bits
 *                   set to NULL if only raw bits are read
 *
 * @retval STM_OK for success or failure from enum stm_ret_t
 */
stm_ret_t sdio_host_get_intr(uint32_t* intr_st)
{
	stm_ret_t ret = STM_OK;

	if (intr_st == NULL) {
		return STM_FAIL_INVALID_ARG;
	}

	if (intr_st != NULL) {
		ret = sdio_driver_read_bytes(SDIO_FUNC_1,
				SDIO_REG(ESP_SLAVE_INT_ST_REG), (uint8_t*)intr_st, 4, 0);
		if (ret) {
			return ret;
		}
	}

	return STM_OK;
}

/** send functions **/

/**
 * @brief  Get available buffer to write to slave before transmit
 * @param  None
 * @retval
 *         Number of buffers available at slave
 */
static uint32_t esp_sdio_host_get_buffer_size(void)
{
	stm_ret_t ret = STM_OK;
	uint32_t len = 0;

	ret = sdio_driver_read_bytes(SDIO_FUNC_1,
			SDIO_REG(ESP_SLAVE_TOKEN_RDATA), &len, 4, 0);
	if (ret) {
		ESP_LOGE(TAG,"Read length error, ret=%d", ret);
		return 0;
	}

	len = (len >> ESP_SDIO_SEND_OFFSET) & ESP_TX_BUFFER_MASK;
	len = (len + ESP_TX_BUFFER_MAX - sdio_esp_tx_bytes) % ESP_TX_BUFFER_MAX;
	ESP_LOGV(TAG,"Host get buff size: len %lu ", len);
	return len;
}

/**
 * @brief  Send a interrupt signal to the SDIO slave
 * @param  intr_no - interrupt number, now only support 0
 * @retval STM_OK for success or failure from enum stm_ret_t
 */
stm_ret_t sdio_host_send_intr(uint8_t intr_no)
{
	uint32_t intr_mask = 0;
	if (intr_no >= MAX_SDIO_SCRATCH_REG_SUPPORTED) {
		ESP_LOGE(TAG," Error interrupt number");
		return STM_FAIL_INVALID_ARG;
	}

	intr_mask = 0x1 << (intr_no + ESP_SDIO_CONF_OFFSET);
	return STM32WriteReg(SDIO_FUNC_1, SDIO_REG(ESP_SLAVE_SCRATCH_REG_7), intr_mask);
}

/**
 * @brief Send a packet to the SDIO slave
 * @param start - Start address of the packet to send
 *        length - Length of data to send, if the packet is over-size,
 *                 the it will be divided into blocks and hold into different
 *                 buffers automatically
 * @retval STM_OK for success or failure from enum stm_ret_t
 */
stm_ret_t sdio_host_send_packet(const void* start, uint32_t length)
{
	stm_ret_t err;
	uint8_t* start_ptr = (uint8_t*)start;
	uint32_t len_remain = length, num = 0, cnt = 300;
	ESP_LOGD(TAG,"length received %d %lu ", length, len_remain);

	int buffer_used, block_n = 0,len_to_send = 0;

	buffer_used = (length + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE;

#if 0
	while (1) {
		num = esp_sdio_host_get_buffer_size();
		ESP_LOGD(TAG,"Buffer size %lu can be send, input len: %u, len_remain: %lu", num, length, len_remain);

		if (num * ESP_BLOCK_SIZE < length) {
			if (!--cnt) {
				ESP_LOGE(TAG,"buff not enough: curr[%lu], exp[%d]", num, buffer_used);
				return STM_FAIL_TIMEOUT;
			} else {
				ESP_LOGE(TAG,"buff not enough: curr[%lu], exp[%d], retry..", num, buffer_used);
			}

			hard_delay(1);
		}  else {
			break;
		}
	}

#endif
	do {
		/* Though the driver supports to split packet of unaligned size into
		 * length of 4x and 1~3, we still send aligned size of data to get
		 * higher effeciency. The length is determined by the SDIO address, and
		 * the remainning will be discard by the slave hardware
		 */
		block_n = len_remain / ESP_BLOCK_SIZE;

		if (block_n) {
			len_to_send = block_n * ESP_BLOCK_SIZE;
			err = sdio_driver_write_blocks(SDIO_FUNC_1,
					ESP_SLAVE_CMD53_END_ADDR - len_remain,
					start_ptr, len_to_send);
		} else {
			len_to_send = len_remain;
			err = sdio_driver_write_bytes(SDIO_FUNC_1,
					ESP_SLAVE_CMD53_END_ADDR - len_remain,
					start_ptr, (len_to_send + 3) & (~3));
		}

		if (err) {
			return err;
		}

		start_ptr += len_to_send;
		len_remain -= len_to_send;
	} while (len_remain);

	if (sdio_esp_tx_bytes >= ESP_TX_BUFFER_MAX) {
		sdio_esp_tx_bytes -= ESP_TX_BUFFER_MAX;
	}

	sdio_esp_tx_bytes += buffer_used;
	return STM_OK;
}
