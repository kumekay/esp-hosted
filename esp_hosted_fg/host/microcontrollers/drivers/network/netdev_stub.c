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

#include "string.h"
//#include "app_main.h"
#include "netdev_api.h"
#include "os_wrapper.h"
#include "esp_log.h"

DEFINE_LOG_TAG(netdev);

struct netdev *ndev_db[MAX_INTERFACE];
static uint8_t ndev_index = 0;

/**
  * @brief  initialize detdev
  * @param  None
  * @retval None
  */
void netdev_init(void)
{
	int i;

	for (i = 0; i < MAX_INTERFACE; i++) {
		ndev_db[i] = NULL;
	}
}

/**
  * @brief  open netdev
  * @param  ndev - netdev
  * @retval 0 on success
  */
int netdev_open(netdev_handle_t ndev)
{
	if (!ndev)
		return STM_FAIL;

	if (ndev->rx_q) {
		g_h.funcs->_h_reset_queue(ndev->rx_q);
		return STM_OK;
	}

	ndev->rx_q = g_h.funcs->_h_create_queue(RX_QUEUE_SIZE, sizeof(struct pbuf));

	if (!ndev->rx_q)
		return STM_FAIL;

	ndev->state = NETDEV_STATE_UP;

	return STM_OK;
}

/**
  * @brief  close netdev
  * @param  ndev - netdev
  * @retval None
  */
void netdev_close(netdev_handle_t ndev)
{
	if (!ndev)
		return;

	ndev->state = NETDEV_STATE_DOWN;

	g_h.funcs->_h_msleep(200);

	/* reset queue */
	if (ndev->rx_q)
		g_h.funcs->_h_reset_queue(ndev->rx_q);

	ndev->net_handle = NULL;
}

/**
  * @brief  get netdev handle from interface name
  * @param  if_name - interface name
  * @retval netdev handle
  */
struct netdev * netdev_get(char *if_name)
{
	int i = 0;
	struct netdev *ndev;

	if (!if_name)
		return NULL;

	while (i < MAX_INTERFACE) {
		ndev = ndev_db[i];

		if (ndev) {
			if (strncmp(if_name, ndev->name, MAX_IF_NAME_SIZE) == 0)
				return ndev;
		}

		i++;
	}

	return NULL;
}

/**
  * @brief  allocate netdev handle for interface
  * @param  sizeof_priv - size of priv interface
  *         name - interface name
  * @retval allocated netdev
  */
netdev_handle_t netdev_alloc(uint32_t sizeof_priv, char *name)
{
	struct netdev *ndev = NULL;

	if (!name)
		return NULL;

	ndev = (struct netdev *) g_h.funcs->_h_malloc(sizeof(struct netdev));

	if (ndev) {
		g_h.funcs->_h_memset(ndev, 0, sizeof(struct netdev));
		g_h.funcs->_h_memcpy(ndev->name, name, MAX_IF_NAME_SIZE);

		ndev->priv = g_h.funcs->_h_malloc(sizeof_priv);

		if (!ndev->priv) {
			ESP_LOGE(TAG, "Failed to allocate memory for priv\n");
			g_h.funcs->_h_free(ndev);
			ndev = NULL;
			return NULL;
		}
	} else {
		ESP_LOGE(TAG, "Failed to allocate memory for net dev\n");
		assert(ndev);
	}

	return ndev;
}


/**
  * @brief  free netdev's private handle
  * @param  dev - netdev handle
  * @retval None
  */
void netdev_free(netdev_handle_t dev)
{
	struct netdev *ndev = (struct netdev *) dev;

	if (ndev) {
		if (ndev->priv) {
			g_h.funcs->_h_free(ndev->priv);
			ndev->priv = NULL;
		}

		if (ndev->net_handle) {
			g_h.funcs->_h_free(ndev->net_handle);
			ndev->net_handle = NULL;
		}


		g_h.funcs->_h_free(ndev);
		ndev = NULL;
	}
}


/**
  * @brief  get netdev's private interface
  * @param  dev - private interface
  * @retval private interface handle on success else NULL
  */
void * netdev_get_priv(netdev_handle_t dev)
{
	struct netdev *ndev = (struct netdev *) dev;

	if (ndev) {
		return ndev->priv;
	}

	return NULL;
}


/**
  * @brief  register netdev
  * @param  dev - private interface
  *         ops - network options to register
  * @retval 0 on success, else -1
  */
int netdev_register(netdev_handle_t dev, struct netdev_ops *ops)
{
	struct netdev *ndev = (struct netdev *) dev;

	if (!ndev || !ops) {
		ESP_LOGE(TAG,"Invalid arguments\n");
		return STM_FAIL;
	}

	ndev->net_ops = ops;
	ndev_db[ndev_index % MAX_INTERFACE] = ndev;

	ndev_index++;

	return STM_OK;
}


/**
  * @brief  unregister netdev
  * @param  dev - private interface
  * @retval 0 on success, else -1
  */
int netdev_unregister(netdev_handle_t dev)
{
	struct netdev *ndev = (struct netdev *) dev;

	if (!ndev) {
		ESP_LOGE(TAG,"Invalid arguments\n");
		return STM_FAIL;
	}

	ndev->net_ops = NULL;
	ndev->state = NETDEV_STATE_DOWN;

	return STM_OK;
}

/**
  * @brief  receive on netdev
  * @param  dev - private interface
  *         net_buf - buffer received
  * @retval 0 on success, else -1
  */
int netdev_rx(netdev_handle_t dev, struct pbuf *net_buf)
{
	struct netdev *ndev = (struct netdev *) dev;
	struct network_handle *net_handle;

	if (!ndev || !net_buf) {
		ESP_LOGE(TAG,"Invalid arguments\n");
		g_h.funcs->_h_msleep(50);
		return STM_FAIL;
	}

	if (ndev->state == NETDEV_STATE_UP) {
		if (g_h.funcs->_h_queue_item(ndev->rx_q, net_buf, HOSTED_BLOCK_MAX)) {
			ESP_LOGE(TAG,"Failed to enqueue received packet\n");
			goto done;
		}

		net_handle = (struct network_handle *) ndev->net_handle;

		if (net_handle->net_rx_callback)
			net_handle->net_rx_callback(net_handle);

		g_h.funcs->_h_free(net_buf);
		net_buf = NULL;

	} else {
		goto done;
	}

	return STM_OK;

done:
	if (net_buf) {
		if (net_buf->payload) {
			g_h.funcs->_h_free(net_buf->payload);
			net_buf->payload = NULL;
		}
		g_h.funcs->_h_free(net_buf);
		net_buf = NULL;
	}
	g_h.funcs->_h_msleep(50);
	return STM_FAIL;
}
