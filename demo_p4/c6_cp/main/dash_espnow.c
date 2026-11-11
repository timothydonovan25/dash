// SPDX-License-Identifier: Apache-2.0
//
// The dash's K-line adapter, heard here. ESP-Hosted has no ESP-NOW RPCs, so
// the C6 runs ESP-NOW itself and hands each of the adapter's frames to the
// P4 as a custom message: the sender's 6-byte address, then the frame as it
// arrived. The P4 decides whose frames count and what they mean
// (components/core/include/core/ecu/ecu.hpp; keep the three numbers below
// in step with it).
//
// The P4 owns the Wi-Fi: it starts it, sets the channel and stops it.
// ESP-NOW is set up again on every interface start, because the host may
// have torn Wi-Fi down and built it again in between.
#include "dash_espnow.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "slave_control.h"

#ifdef CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER

#define DASH_ECU_MAGIC      0x4B          /* core::ecu::kMagic */
#define DASH_ECU_HEADER_LEN 6             /* core::ecu::kHeaderLen */
#define DASH_ECU_MSG_ID     0x454B4C31u   /* core::ecu::kMsgId */

static const char *TAG = "dash_espnow";

/* The Wi-Fi task. Nothing here waits: a frame the link cannot take is
 * dropped, and the adapter sends another in 50 ms. */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
	uint8_t msg[ESP_NOW_ETH_ALEN + ESP_NOW_MAX_DATA_LEN];

	if (!info || !info->src_addr || !data) return;
	if (len < DASH_ECU_HEADER_LEN || len > ESP_NOW_MAX_DATA_LEN) return;
	if (data[0] != DASH_ECU_MAGIC) return;

	memcpy(msg, info->src_addr, ESP_NOW_ETH_ALEN);
	memcpy(msg + ESP_NOW_ETH_ALEN, data, len);
	esp_hosted_send_custom_data(DASH_ECU_MSG_ID, msg, ESP_NOW_ETH_ALEN + len);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
	if (id != WIFI_EVENT_STA_START && id != WIFI_EVENT_AP_START) return;

	esp_now_deinit();   /* an error only if it was not up */
	esp_err_t err = esp_now_init();
	if (err == ESP_OK) err = esp_now_register_recv_cb(on_recv);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "ESP-NOW did not start: %s", esp_err_to_name(err));
	} else {
		ESP_LOGI(TAG, "listening for the K-line adapter");
	}
}

void dash_espnow_init(void)
{
	esp_err_t err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "no Wi-Fi events, so no ESP-NOW: %s", esp_err_to_name(err));
	}
}

#else

void dash_espnow_init(void) {}

#endif
