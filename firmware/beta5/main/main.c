/*
  File: main.c

  VSCP beta node

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright © 2022-2023 Ake Hedman, the VSCP project <info@vscp.org>

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include "vscp-projdefs.h"
#include "vscp-compiler.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <driver/ledc.h>
#include <driver/gpio.h>

#include <esp_check.h>
#include <esp_crc.h>
#include <esp_https_ota.h>
#include <esp_now.h>
#include <esp_event_base.h>
#include <esp_event.h>
#include <esp_random.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include "esp_crc.h"
#include <nvs_flash.h>
#include <esp_spiffs.h>
#include <esp_event.h>

// #include <esp_crt_bundle.h>

#include <vscp.h>
#include <vscp_class.h>
#include <vscp_type.h>
#include "vscp-droplet.h"

#include "main.h"

#include "button.h"
#include "led_indicator.h"

const char *pop_data   = "VSCP BETA";
static const char *TAG = "BETA";

#define HASH_LEN   32
#define BUTTON_CNT 1

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

// The net interface
esp_netif_t *g_netif = NULL;

// Holds alpha node states
// beta_node_states_t g_state = MAIN_STATE_INIT;

// Event source task related definitions
ESP_EVENT_DEFINE_BASE(BETA_EVENT);

// Handle for status led
led_indicator_handle_t g_led_handle = NULL;

// Handle for nvs storage
nvs_handle_t g_nvsHandle = 0;

static QueueHandle_t s_espnow_queue;

// If init button is held for  > 10 seconds defaults are stored and the
// node is restarted. Wifi credentials need to be restored
uint32_t g_restore_defaults_timer = 0;

// Initiating provisioning is done with a button click
// the provisioning state is active for 30 seconds
uint32_t g_provisioning_state_timer = 0;

// Button defines
static button_handle_t g_btns[BUTTON_CNT] = { 0 };

static void
vscp_heartbeat_task(void *pvParameter);

enum {
  VSCP_SEND_STATE_NONE,
  VSCP_SEND_STATE_SENT,        // Event has been sent, no other events can be sent
  VSCP_SEND_STATE_SEND_CONFIRM // Event send has been confirmed
};

typedef struct __state__ {
  uint8_t state;      // State of send
  uint64_t timestamp; // Time when state was set
} vscp_send_state_t;

static vscp_send_state_t g_send_state = { VSCP_SEND_STATE_NONE, 0 };

//----------------------------------------------------------
typedef enum {
  EXAMPLE_ESPNOW_SEND_CB,
  EXAMPLE_ESPNOW_RECV_CB,
} example_espnow_event_id_t;

typedef struct {
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  esp_now_send_status_t status;
} example_espnow_event_send_cb_t;

typedef struct {
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  uint8_t *data;
  int data_len;
} example_espnow_event_recv_cb_t;

typedef union {
  example_espnow_event_send_cb_t send_cb;
  example_espnow_event_recv_cb_t recv_cb;
} example_espnow_event_info_t;

/* When ESPNOW sending or receiving callback function is called, post event to ESPNOW task. */
typedef struct {
  example_espnow_event_id_t id;
  example_espnow_event_info_t info;
} example_espnow_event_t;

enum {
  EXAMPLE_ESPNOW_DATA_BROADCAST,
  EXAMPLE_ESPNOW_DATA_UNICAST,
  EXAMPLE_ESPNOW_DATA_MAX,
};

const blink_step_t test_blink[] = {
  { LED_BLINK_HOLD, LED_STATE_ON, 50 },   // step1: turn on LED 50 ms
  { LED_BLINK_HOLD, LED_STATE_OFF, 100 }, // step2: turn off LED 100 ms
  { LED_BLINK_HOLD, LED_STATE_ON, 150 },  // step3: turn on LED 150 ms
  { LED_BLINK_HOLD, LED_STATE_OFF, 100 }, // step4: turn off LED 100 ms
  { LED_BLINK_STOP, 0, 0 },               // step5: stop blink (off)
};

///////////////////////////////////////////////////////////
//                   P E R S I S T A N T
///////////////////////////////////////////////////////////

// Set default configuration

node_persistent_config_t g_persistent = {

  // General
  .bProvisioned = false, // Node rhas not been provisioned
  .nodeName     = "Beta Node",
  .lkey         = { 0 }, // Local key for this node only
  .pmk          = { 0 }, // Primary key (common to all nodes)
  .nodeGuid     = { 0 }, // GUID for unit
  .startDelay   = 2,
  .bootCnt      = 0,

  // Droplet
  .dropletLongRange             = false,                  // No long range mode by default
  .dropletChannel               = 1,                      // Use wifi channel
  .dropletTtl                   = 32,                     // Hops for forwarded frames
  .dropletSizeQueue             = 32,                     // Size fo input queue
  .dropletForwardEnable         = true,                   // Forward when packets are received
  .dropletEncryption            = VSCP_ENCRYPTION_AES128, // 0=no encryption, 1=AES-128, 2=AES-192, 3=AES-256
  .dropletFilterAdjacentChannel = true,                   // Don't receive if from other channel
  .dropletForwardSwitchChannel  = false,                  // Allow switchin gchannel on forward
  .dropletFilterWeakSignal      = -100,                   // Filter onm RSSI (zero is no rssi filtering)
};

//----------------------------------------------------------

///////////////////////////////////////////////////////////
//                      V S C P
///////////////////////////////////////////////////////////

// ESP-NOW
SemaphoreHandle_t g_ctrl_task_sem;

#ifdef CONFIG_IDF_TARGET_ESP32C3
#define BOOT_KEY_GPIIO  GPIO_NUM_9
#define CONFIG_LED_GPIO GPIO_NUM_2
#elif CONFIG_IDF_TARGET_ESP32S3
#define BOOT_KEY_GPIIO  GPIO_NUM_0
#define CONFIG_LED_GPIO GPIO_NUM_2
#else
#define BOOT_KEY_GPIIO  GPIO_NUM_0
#define CONFIG_LED_GPIO GPIO_NUM_2
#endif

// Forward declarations
static void
vscp_espnow_deinit(void *param);

// Signal Wi-Fi events on this event-group
const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t g_wifi_event_group;

#define SEND_CB_OK   BIT0
#define SEND_CB_FAIL BIT1

///////////////////////////////////////////////////////////////////////////////
// droplet_receive_cb
//

void
droplet_receive_cb(const vscpEvent *pev, void *userdata)
{
  int rv;

  if (NULL == pev) {
    ESP_LOGE(TAG, "Invalid pointer for droplet rx cb");
    return;
  }

  // Disable if no broker URL defined
  // if (g_persistent.mqttEnable && (g_persistent.mqttUrl)) {
  //   // Send event to MQTT broker
  //   if (VSCP_ERROR_SUCCESS != (rv = mqtt_send_vscp_event(NULL, pev))) {
  //     ESP_LOGE(TAG, "Failed to send event to MQTT broker rv=%d", rv);
  //   }
  // }

  // If VSCP Link protocol is enabled and a client is connected send event
  // to client
  // if (g_persistent.vscplinkEnable && strlen(g_persistent.vscplinkUrl)) {
  //   // Send event to active VSCP link clients
  //   printf("Sending event to VSCP Link client\n");
  //   if (VSCP_ERROR_SUCCESS != (rv = tcpsrv_sendEventExToAllClients(pev))) {
  //     if (VSCP_ERROR_TRM_FULL == rv) {
  //       ESP_LOGI(TAG, "Failed to send event to tcpipsrv (queue is full for client)");
  //     }
  //     else if (VSCP_ERROR_TIMEOUT == rv) {
  //       ESP_LOGI(TAG, "Failed to send event to tcpipsrv (Unable to get mutex)");
  //     }
  //     else {
  //       ESP_LOGE(TAG, "Failed to send event to tcpipsrv rv=%d", rv);
  //     }
  //   }
  // }
}

///////////////////////////////////////////////////////////////////////////////
// droplet_network_attach_cb
//

void
droplet_network_attach_cb(wifi_pkt_rx_ctrl_t *prxdata, void *userdata)
{
  esp_err_t ret;

  // Set Channel
  g_persistent.dropletChannel = prxdata->channel;
  ret                         = nvs_set_u8(g_nvsHandle, "drop_ch", g_persistent.dropletChannel);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to update droplet channel ret = %X", ret);
  }
}

///////////////////////////////////////////////////////////////////////////////
// setAccessPointParameters
//

esp_err_t
setAccessPointParameters(void)
{
  wifi_config_t wifi_cfg = { .ap = { .channel         = PRJDEF_AP_CHANNEL,
                                     .max_connection  = PRJDEF_AP_MAX_CONNECTIONS,
                                     .beacon_interval = PRJDEF_AP_BEACON_INTERVAL,
                                     .authmode        = WIFI_AUTH_WPA_WPA2_PSK,
                                     /*.ssid_hidden     = 1*/ } };
  memcpy(wifi_cfg.ap.ssid, g_persistent.nodeName, strlen(g_persistent.nodeName));
  memcpy(wifi_cfg.ap.password, PRJDEF_AP_PASSWORD, strlen(PRJDEF_AP_PASSWORD));
  return esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
}

///////////////////////////////////////////////////////////////////////////////
// readPersistentConfigs
//

esp_err_t
readPersistentConfigs(void)
{
  esp_err_t rv;
  char buf[80];
  size_t length = sizeof(buf);
  uint8_t val;

  // Provisioned
  rv = nvs_get_u8(g_nvsHandle, "provision", &val);
  if (ESP_OK != rv) {
    val = (uint8_t) g_persistent.bProvisioned;
    rv  = nvs_set_u8(g_nvsHandle, "provision", g_persistent.bProvisioned);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to update provisioned state");
    }
  }
  else {
    g_persistent.bProvisioned = (bool) val;
  }

  // boot counter
  rv = nvs_get_u32(g_nvsHandle, "boot_counter", &g_persistent.bootCnt);
  switch (rv) {

    case ESP_OK:
      ESP_LOGI(TAG, "Boot counter = %d", (int) g_persistent.bootCnt);
      break;

    case ESP_ERR_NVS_NOT_FOUND:
      ESP_LOGE(TAG, "The boot counter is not initialized yet!");
      break;

    default:
      ESP_LOGE(TAG, "Error (%s) reading boot counter!", esp_err_to_name(rv));
      break;
  }

  // Update and write back boot counter
  g_persistent.bootCnt++;
  rv = nvs_set_u32(g_nvsHandle, "boot_counter", g_persistent.bootCnt);
  if (rv != ESP_OK) {
    ESP_LOGE(TAG, "Failed to update boot counter");
  }

  // Node name
  rv = nvs_get_str(g_nvsHandle, "node_name", buf, &length);
  switch (rv) {
    case ESP_OK:
      strncpy(g_persistent.nodeName, buf, sizeof(g_persistent.nodeName));
      ESP_LOGI(TAG, "Node Name = %s", g_persistent.nodeName);
      break;

    case ESP_ERR_NVS_NOT_FOUND:
      rv = nvs_set_str(g_nvsHandle, "node_name", "Alpha Node");
      if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update node name");
      }
      break;

    default:
      ESP_LOGE(TAG, "Error (%s) reading 'node_name'!", esp_err_to_name(rv));
      break;
  }

  // Start Delay (seconds)
  rv = nvs_get_u8(g_nvsHandle, "start_delay", &g_persistent.startDelay);
  switch (rv) {

    case ESP_OK:
      ESP_LOGI(TAG, "Start delay = %d", g_persistent.startDelay);
      break;

    case ESP_ERR_NVS_NOT_FOUND:
      rv = nvs_set_u8(g_nvsHandle, "start_delay", 2);
      if (rv != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update start delay");
      }
      break;

    default:
      ESP_LOGE(TAG, "Error (%s) reading!", esp_err_to_name(rv));
      break;
  }

  // lkey (Local key)
  length = 32;
  rv     = nvs_get_blob(g_nvsHandle, "lkey", g_persistent.lkey, &length);
  if (rv != ESP_OK) {

    // We need to generate a new lkey
    esp_fill_random(g_persistent.lkey, sizeof(g_persistent.lkey));
    ESP_LOGW(TAG, "----------> New lkey generated <----------");

    rv = nvs_set_blob(g_nvsHandle, "lkey", g_persistent.lkey, 32);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to write node lkey to nvs. rv=%d", rv);
    }
  }

  // pmk (Primary key)
  length = 32;
  rv     = nvs_get_blob(g_nvsHandle, "pmk", g_persistent.pmk, &length);
  if (rv != ESP_OK) {

    // We need to generate a new pmk
    esp_fill_random(g_persistent.pmk, sizeof(g_persistent.pmk));
    ESP_LOGW(TAG, "----------> New pmk generated <----------");

    rv = nvs_set_blob(g_nvsHandle, "pmk", g_persistent.pmk, 32);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to write node pmk to nvs. rv=%d", rv);
    }
  }

  // If not provisioned we list dump of key on serial interface
  if (!g_persistent.bProvisioned) {
    ESP_LOGI(TAG, "Unprovisioned pmk");
    ESP_LOG_BUFFER_HEXDUMP(TAG, g_persistent.pmk, sizeof(g_persistent.pmk), ESP_LOG_INFO);
  }

  // GUID
  length = 16;
  rv     = nvs_get_blob(g_nvsHandle, "guid", g_persistent.nodeGuid, &length);

  if (rv != ESP_OK) {
    // FF:FF:FF:FF:FF:FF:FF:FE:MAC1:MAC2:MAC3:MAC4:MAC5:MAC6:NICKNAME1:NICKNAME2
    memset(g_persistent.nodeGuid + 6, 0xff, 7);
    g_persistent.nodeGuid[7] = 0xfe;
    // rv                       = esp_efuse_mac_get_default(g_persistent.nodeGuid + 8);
    //  ESP_MAC_WIFI_STA
    //  ESP_MAC_WIFI_SOFTAP
    rv = esp_read_mac(g_persistent.nodeGuid + 8, ESP_MAC_WIFI_SOFTAP);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "esp_efuse_mac_get_default failed to get GUID. rv=%d", rv);
    }

    rv = nvs_set_blob(g_nvsHandle, "guid", g_persistent.nodeGuid, 16);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to write node GUID to nvs. rv=%d", rv);
    }
  }
  ESP_LOGI(TAG,
           "GUID for node: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
           g_persistent.nodeGuid[0],
           g_persistent.nodeGuid[1],
           g_persistent.nodeGuid[2],
           g_persistent.nodeGuid[3],
           g_persistent.nodeGuid[4],
           g_persistent.nodeGuid[5],
           g_persistent.nodeGuid[6],
           g_persistent.nodeGuid[7],
           g_persistent.nodeGuid[8],
           g_persistent.nodeGuid[9],
           g_persistent.nodeGuid[10],
           g_persistent.nodeGuid[11],
           g_persistent.nodeGuid[12],
           g_persistent.nodeGuid[13],
           g_persistent.nodeGuid[14],
           g_persistent.nodeGuid[15]);

  // Droplet ----------------------------------------------------------------

  // Long Range
  rv = nvs_get_u8(g_nvsHandle, "drop_lr", &val);
  if (ESP_OK != rv) {
    val = (uint8_t) g_persistent.dropletLongRange;
    rv  = nvs_set_u8(g_nvsHandle, "drop_lr", g_persistent.dropletLongRange);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to update droplet long range");
    }
  }
  else {
    g_persistent.dropletLongRange = (bool) val;
  }

  // Channel
  rv = nvs_get_u8(g_nvsHandle, "drop_ch", &g_persistent.dropletChannel);
  if (ESP_OK != rv) {
    rv = nvs_set_u8(g_nvsHandle, "drop_ch", g_persistent.dropletChannel);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to update droplet channel");
    }
  }

  // g_persistent.dropletChannel = 9;    // TODO for test

  // Default queue size
  rv = nvs_get_u8(g_nvsHandle, "drop_qsize", &g_persistent.dropletSizeQueue);
  if (ESP_OK != rv) {
    rv = nvs_set_u8(g_nvsHandle, "drop_qsize", g_persistent.dropletSizeQueue);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to update droplet queue size");
    }
  }

  // Default ttl
  rv = nvs_get_u8(g_nvsHandle, "drop_ttl", &g_persistent.dropletTtl);
  if (ESP_OK != rv) {
    rv = nvs_set_u8(g_nvsHandle, "drop_ttl", g_persistent.dropletTtl);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to update droplet ttl");
    }
  }

  // Forward
  rv = nvs_get_u8(g_nvsHandle, "drop_fw", &val);
  if (ESP_OK != rv) {
    val = (uint8_t) g_persistent.dropletForwardEnable;
    rv  = nvs_set_u8(g_nvsHandle, "drop_fw", g_persistent.dropletForwardEnable);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to update droplet forward");
    }
  }
  else {
    g_persistent.dropletForwardEnable = (bool) val;
  }

  // Encryption
  rv = nvs_get_u8(g_nvsHandle, "drop_enc", &g_persistent.dropletEncryption);
  if (ESP_OK != rv) {
    rv = nvs_set_u8(g_nvsHandle, "drop_enc", g_persistent.dropletEncryption);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to update droplet encryption");
    }
  }

  // Adj filter channel
  rv = nvs_get_u8(g_nvsHandle, "drop_filt", &val);
  if (ESP_OK != rv) {
    val = (uint8_t) g_persistent.dropletFilterAdjacentChannel;
    rv  = nvs_set_u8(g_nvsHandle, "drop_filt", g_persistent.dropletFilterAdjacentChannel);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to update droplet adj channel filter");
    }
  }
  else {
    g_persistent.dropletFilterAdjacentChannel = (bool) val;
  }

  // Allow switching channel on forward
  rv = nvs_get_u8(g_nvsHandle, "drop_swchf", &val);
  if (ESP_OK != rv) {
    val = (uint8_t) g_persistent.dropletForwardSwitchChannel;
    rv  = nvs_set_u8(g_nvsHandle, "drop_swchf", g_persistent.dropletForwardSwitchChannel);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to update droplet shitch channel on forward");
    }
  }
  else {
    g_persistent.dropletFilterAdjacentChannel = (bool) val;
  }

  // RSSI limit
  rv = nvs_get_i8(g_nvsHandle, "drop_rssi", &g_persistent.dropletFilterWeakSignal);
  if (ESP_OK != rv) {
    rv = nvs_set_u8(g_nvsHandle, "drop_rssi", g_persistent.dropletFilterWeakSignal);
    if (rv != ESP_OK) {
      ESP_LOGE(TAG, "Failed to update droplet RSSI");
    }
  }

  rv = nvs_commit(g_nvsHandle);
  if (rv != ESP_OK) {
    ESP_LOGI(TAG, "Failed to commit updates to nvs\n");
  }

  return ESP_OK;
}

//-----------------------------------------------------------------------------
//                              Button handlers
//-----------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////////
// get_btn_index
//

static int
get_btn_index(button_handle_t btn)
{
  for (size_t i = 0; i < BUTTON_CNT; i++) {
    if (btn == g_btns[i]) {
      return i;
    }
  }
  return -1;
}

///////////////////////////////////////////////////////////////////////////////
// button_single_click_cb
//

static void
button_single_click_cb(void *arg, void *data)
{
  // Initiate provisioning
  ESP_LOGI(TAG, "BTN%d: BUTTON_SINGLE_CLICK", get_btn_index((button_handle_t) arg));

  droplet_startClientProvisioning();

  if (led_indicator_start(g_led_handle, BLINK_PROVISIONING) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start indicator light");
  }
}

///////////////////////////////////////////////////////////////////////////////
// button_double_click_cb
//

static void
button_double_click_cb(void *arg, void *data)
{
  // Restart
  ESP_LOGI(TAG, "Will reboot device in two seconds");
  vTaskDelay(2000 / portTICK_PERIOD_MS);
  esp_restart();
}

///////////////////////////////////////////////////////////////////////////////
// button_long_press_start_cb
//

static void
button_long_press_start_cb(void *arg, void *data)
{
  // > 10 seconds Restore defaults
  ESP_LOGI(TAG, "BTN%d: BUTTON_LONG_PRESS_START", get_btn_index((button_handle_t) arg));
  g_restore_defaults_timer = getMilliSeconds();
}

///////////////////////////////////////////////////////////////////////////////
// button_long_press_hold_cb
//

static void
button_long_press_hold_cb(void *arg, void *data)
{
  ESP_LOGI(TAG,
           "Will restore defaults in %u seconds",
           (int) (10 - ((getMilliSeconds() - g_restore_defaults_timer) / 1000)));
  if ((getMilliSeconds() - g_restore_defaults_timer) > 10000) {
    // vprintf_like_t logFunc = esp_log_set_vprintf(g_stdLogFunc);
    // wifi_prov_mgr_reset_provisioning();
    esp_restart();
  }
}

//-----------------------------------------------------------------------------
//                                 espnow OTA
//-----------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////////
// startOTA
//

void
startOTA(void)
{
  // xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);
}

///////////////////////////////////////////////////////////////////////////////
// firmware_download
//

static size_t
firmware_download(const char *url)
{
#define OTA_DATA_PAYLOAD_LEN 1024

  esp_err_t ret               = ESP_OK;
  esp_ota_handle_t ota_handle = 0;
  uint8_t *data               = malloc(OTA_DATA_PAYLOAD_LEN); // TODO ESP MALLOC
  size_t total_size           = 0;
  uint32_t start_time         = xTaskGetTickCount();

  esp_http_client_config_t config = {
    .url            = url,
    .transport_type = HTTP_TRANSPORT_UNKNOWN,
  };

  /**
   * @brief 1. Connect to the server
   */
  esp_http_client_handle_t client = esp_http_client_init(&config);
  ESP_GOTO_ON_ERROR(!client, EXIT, TAG, "Initialise HTTP connection");

  ESP_LOGI(TAG, "Open HTTP connection: %s", url);

  /**
   * @brief First, the firmware is obtained from the http server and stored
   */
  do {
    ret = esp_http_client_open(client, 0);

    if (ret != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      ESP_LOGW(TAG, "<%s> Connection service failed", esp_err_to_name(ret));
    }
  } while (ret != ESP_OK);

  total_size = esp_http_client_fetch_headers(client);

  if (total_size <= 0) {
    ESP_LOGW(TAG, "Please check the address of the server");
    ret = esp_http_client_read(client, (char *) data, OTA_DATA_PAYLOAD_LEN);
    ESP_GOTO_ON_ERROR(ret < 0, EXIT, TAG, "<%s> Read data from http stream", esp_err_to_name(ret));

    ESP_LOGW(TAG, "Recv data: %.*s", ret, data);
    goto EXIT;
  }

  /**
   * @brief 2. Read firmware from the server and write it to the flash of the root node
   */

  const esp_partition_t *updata_partition = esp_ota_get_next_update_partition(NULL);
  /**< Commence an OTA update writing to the specified partition. */
  ret = esp_ota_begin(updata_partition, total_size, &ota_handle);
  ESP_GOTO_ON_ERROR(ret != ESP_OK, EXIT, TAG, "<%s> esp_ota_begin failed, total_size", esp_err_to_name(ret));

  for (ssize_t size = 0, recv_size = 0; recv_size < total_size; recv_size += size) {
    size = esp_http_client_read(client, (char *) data, OTA_DATA_PAYLOAD_LEN);
    ESP_GOTO_ON_ERROR(size < 0, EXIT, TAG, "<%s> Read data from http stream", esp_err_to_name(ret));

    if (size > 0) {
      /**< Write OTA update data to partition */
      ret = esp_ota_write(ota_handle, data, OTA_DATA_PAYLOAD_LEN);
      ESP_GOTO_ON_ERROR(ret != ESP_OK,
                        EXIT,
                        TAG,
                        "<%s> Write firmware to flash, size: %d, data: %.*s",
                        esp_err_to_name(ret),
                        size,
                        size,
                        data);
    }
    else {
      ESP_LOGW(TAG, "<%s> esp_http_client_read", esp_err_to_name((int) ret));
      goto EXIT;
    }
  }

  ESP_LOGI(TAG,
           "The service download firmware is complete, Spend time: %ds",
           (int) ((xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS / 1000));

  ret = esp_ota_end(ota_handle);
  ESP_GOTO_ON_ERROR(ret != ESP_OK, EXIT, TAG, "<%s> esp_ota_end", esp_err_to_name(ret));

EXIT:
  free(data);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  return total_size;
}

///////////////////////////////////////////////////////////////////////////////
// ota_initator_data_cb
//

esp_err_t
ota_initator_data_cb(size_t src_offset, void *dst, size_t size)
{
  static const esp_partition_t *data_partition = NULL;

  if (!data_partition) {
    data_partition = esp_ota_get_next_update_partition(NULL);
  }

  return esp_partition_read(data_partition, src_offset, dst, size);
}

///////////////////////////////////////////////////////////////////////////////
// firmware_send
//

// static void
// firmware_send(size_t firmware_size, uint8_t sha[ESPNOW_OTA_HASH_LEN])
// {
//   esp_err_t ret                         = ESP_OK;
//   uint32_t start_time                   = xTaskGetTickCount();
//   espnow_ota_result_t espnow_ota_result = { 0 };
//   espnow_ota_responder_t *info_list     = NULL;
//   size_t num                            = 0;

//   espnow_ota_initator_scan(&info_list, &num, pdMS_TO_TICKS(3000));
//   ESP_LOGW(TAG, "espnow wait ota num: %d", num);

//   espnow_addr_t *dest_addr_list = ESP_MALLOC(num * ESPNOW_ADDR_LEN);

//   for (size_t i = 0; i < num; i++) {
//     memcpy(dest_addr_list[i], info_list[i].mac, ESPNOW_ADDR_LEN);
//   }

//   ESP_FREE(info_list);

//   ret = espnow_ota_initator_send(dest_addr_list, num, sha, firmware_size, ota_initator_data_cb, &espnow_ota_result);
//   ESP_GOTO_ON_ERROR(ret != ESP_OK, EXIT, TAG, "<%s> espnow_ota_initator_send", esp_err_to_name(ret));

//   if (espnow_ota_result.successed_num == 0) {
//     ESP_LOGW(TAG, "Devices upgrade failed, unfinished_num: %d", espnow_ota_result.unfinished_num);
//     goto EXIT;
//   }

//   ESP_LOGI(TAG,
//            "Firmware is sent to the device to complete, Spend time: %ds",
//            (xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS / 1000);
//   ESP_LOGI(TAG,
//            "Devices upgrade completed, successed_num: %d, unfinished_num: %d",
//            espnow_ota_result.successed_num,
//            espnow_ota_result.unfinished_num);

// EXIT:
//   espnow_ota_initator_result_free(&espnow_ota_result);
// }

// ///////////////////////////////////////////////////////////////////////////////
// // initiateFirmwareUpload
// //

// int
// initiateFirmwareUpload(void)
// {
//   uint8_t sha_256[32]                   = { 0 };
//   const esp_partition_t *data_partition = esp_ota_get_next_update_partition(NULL);

//   size_t firmware_size = firmware_download(CONFIG_FIRMWARE_UPGRADE_URL);
//   esp_partition_get_sha256(data_partition, sha_256);

//   // Send new firmware to clients
//   firmware_send(firmware_size, sha_256);

//   return VSCP_ERROR_SUCCESS;
// }

// ///////////////////////////////////////////////////////////////////////////////
// // respondToFirmwareUpload
// //

// int
// respondToFirmwareUpload(void)
// {
//   espnow_ota_config_t ota_config = {
//     .skip_version_check       = true,
//     .progress_report_interval = 10,
//   };

//   // Take care of firmware update of out node
//   espnow_ota_responder_start(&ota_config);

//   return VSCP_ERROR_SUCCESS;
// }

// ----------------------------------------------------------------------------
//                              espnow key exchange
//-----------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////////
// security
//

// void
// Initiate_security_key_transfer(void)
// {
//   uint32_t start_time1                  = xTaskGetTickCount();
//   espnow_sec_result_t espnow_sec_result = { 0 };
//   espnow_sec_responder_t *info_list     = NULL;
//   size_t num                            = 0;
//   espnow_sec_initiator_scan(&info_list, &num, pdMS_TO_TICKS(3000));
//   ESP_LOGW(TAG, "espnow wait security num: %d", num);

//   if (num == 0) {
//     ESP_FREE(info_list);
//     return;
//   }

//   espnow_addr_t *dest_addr_list = ESP_MALLOC(num * ESPNOW_ADDR_LEN);

//   for (size_t i = 0; i < num; i++) {
//     memcpy(dest_addr_list[i], info_list[i].mac, ESPNOW_ADDR_LEN);
//   }

//   ESP_FREE(info_list);
//   uint32_t start_time2 = xTaskGetTickCount();
//   esp_err_t ret        = espnow_sec_initiator_start(g_sec, pop_data, dest_addr_list, num, &espnow_sec_result);
//   ESP_GOTO_ON_ERROR(ret != ESP_OK, EXIT, TAG, "<%s> espnow_sec_initator_start", esp_err_to_name(ret));

//   ESP_LOGI(TAG,
//            "App key is sent to the device to complete, Spend time: %dms, Scan time: %dms",
//            (xTaskGetTickCount() - start_time1) * portTICK_PERIOD_MS,
//            (start_time2 - start_time1) * portTICK_PERIOD_MS);
//   ESP_LOGI(TAG,
//            "Devices security completed, successed_num: %d, unfinished_num: %d",
//            espnow_sec_result.successed_num,
//            espnow_sec_result.unfinished_num);

// EXIT:
//   ESP_FREE(dest_addr_list);
//   espnow_sec_initator_result_free(&espnow_sec_result);
// }

// void
// respond_to_security_key_transfer(void)
// {
//   espnow_sec_responder_start(g_sec, pop_data);
//   ESP_LOGI(TAG, "<===============================>");
// }

// ----------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////////
// read_onboard_temperature
//

float
read_onboard_temperature(void)
{
  // TODO
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
// getMilliSeconds
//

uint32_t
getMilliSeconds(void)
{
  return (esp_timer_get_time() / 1000);
};

///////////////////////////////////////////////////////////////////////////////
// get_device_guid
//

bool
get_device_guid(uint8_t *pguid)
{
  esp_err_t rv;
  size_t length = 16;

  // Check pointer
  if (NULL == pguid) {
    return false;
  }

  rv = nvs_get_blob(g_nvsHandle, "guid", pguid, &length);
  switch (rv) {

    case ESP_OK:
      break;

    case ESP_ERR_NVS_NOT_FOUND:
      printf("GUID not found in nvs\n");
      return false;

    default:
      printf("Error (%s) reading GUID from nvs!\n", esp_err_to_name(rv));
      return false;
  }

  return true;
}

///////////////////////////////////////////////////////////////////////////////
// beta_event_handler
//
// Event handler for catching system events
//

static void
beta_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_base == BETA_EVENT) {
    if (event_id == BETA_START_CLIENT_PROVISIONING) {
      ESP_LOGI(TAG, "Start client provisioning");
    }
    else if (event_id == BETA_STOP_CLIENT_PROVISIONING) {
      ESP_LOGI(TAG, "Stop client provisioning");
    }
    else if (event_id == BETA_GET_IP_ADDRESS_START) {
      ESP_LOGI(TAG, "Waiting for IP-address");
    }
    else if (event_id == BETA_GET_IP_ADDRESS_STOP) {
      ESP_LOGI(TAG, "IP-address received");
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// wifi_event_handler
//

static void
wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
    ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
  }
  else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
    ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac), event->aid);
  }
}

///////////////////////////////////////////////////////////////////////////////
// system_event_handler
//
// Event handler for catching system events
//

static void
system_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  static bool s_ap_staconnected_flag = false;
  static bool s_sta_connected_flag   = false;
  static int retries;

  if (event_base == WIFI_EVENT) {

    //     switch (event_id) {

    //       case WIFI_EVENT_WIFI_READY: {
    //         // Set channel
    //         ESP_ERROR_CHECK(esp_wifi_set_channel(DROPLET_CHANNEL, WIFI_SECOND_CHAN_NONE));
    //       } break;

    //       case WIFI_EVENT_STA_START: {
    //         ESP_LOGI(TAG, "Connecting........");
    //         //esp_wifi_connect();
    //       }

    //       case WIFI_EVENT_AP_STACONNECTED: {
    //         wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
    //         // ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), (int)event->aid);
    //         s_ap_staconnected_flag = true;
    //         break;
    //       }

    //       case WIFI_EVENT_AP_STADISCONNECTED: {
    //         wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
    //         // ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac), ((int)event->aid);
    //         s_ap_staconnected_flag = false;
    //         break;
    //       }

    //       case WIFI_EVENT_STA_CONNECTED: {
    //         wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *) event_data;
    //         ESP_LOGI(TAG,
    //                  "Connected to %s (BSSID: " MACSTR ", Channel: %d)",
    //                  event->ssid,
    //                  MAC2STR(event->bssid),
    //                  event->channel);
    //         s_sta_connected_flag = true;
    //         break;
    //       }

    //       case WIFI_EVENT_STA_DISCONNECTED: {
    //         ESP_LOGI(TAG, "sta disconnect");
    //         s_sta_connected_flag = false;
    //         ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
    //         g_state = MAIN_STATE_INIT;
    //         esp_wifi_connect();
    //         break;
    //       }
    // #ifdef CONFIG_EXAMPLE_PROV_TRANSPORT_SOFTAP
    //       case WIFI_EVENT_AP_STACONNECTED:
    //         ESP_LOGI(TAG, "SoftAP transport: Connected!");
    //         break;
    //       case WIFI_EVENT_AP_STADISCONNECTED:
    //         ESP_LOGI(TAG, "SoftAP transport: Disconnected!");
    //         break;
    // #endif
    //       default:
    //         break;
    //     }
    //   }
    // Post 5.0 stable
    // ---------------
    // else if (event_base == ESP_HTTPS_OTA_EVENT) {
    //   switch (event_id) {

    //     case ESP_HTTPS_OTA_START: {
    //       ;
    //     } break;

    //     case ESP_HTTPS_OTA_CONNECTED: {
    //       ;
    //     } break;

    //     case ESP_HTTPS_OTA_GET_IMG_DESC: {
    //       ;
    //     } break;

    //     case ESP_HTTPS_OTA_VERIFY_CHIP_ID: {
    //       ;
    //     } break;

    //     case ESP_HTTPS_OTA_DECRYPT_CB: {
    //       ;
    //     } break;

    //     case ESP_HTTPS_OTA_WRITE_FLASH: {
    //       ;
    //     } break;

    //     case ESP_HTTPS_OTA_UPDATE_BOOT_PARTITION: {
    //       ;
    //     } break;

    //     case ESP_HTTPS_OTA_FINISH: {
    //       ;
    //     } break;

    //   case ESP_HTTPS_OTA_ABORT: {
    //       ;
    //     } break;
  }

  else if (event_base == BETA_EVENT) {
    ESP_LOGI(TAG, "Beta event -----------------------------------------------------------> id=%ld", event_id);
  }
}

///////////////////////////////////////////////////////////////////////////////
// LED status task
//

void
led_task(void *pvParameter)
{
  // GPIO_NUM_16 is G16 on board
  gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
  printf("Blinking LED on GPIO 16\n");
  int cnt = 0;
  while (1) {
    gpio_set_level(GPIO_NUM_2, cnt % 2);
    cnt++;
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

///////////////////////////////////////////////////////////////////////////////
// app_main
//

void
app_main(void)
{
  esp_err_t ret;
  uint8_t dest_addr[ESP_NOW_ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
  uint8_t buf[DROPLET_MIN_FRAME + 3]; // Three byte data
  size_t size = sizeof(buf);

  // Initialize NVS partition
  esp_err_t rv = nvs_flash_init();
  if (rv == ESP_ERR_NVS_NO_FREE_PAGES || rv == ESP_ERR_NVS_NEW_VERSION_FOUND) {

    // NVS partition was truncated
    // and needs to be erased
    ESP_ERROR_CHECK(nvs_flash_erase());

    // Retry nvs_flash_init
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  // Initialize LED indicator
  led_indicator_config_t indicator_config = {
    .off_level = 0, // if zero, attach led positive side to esp32 gpio pin
    .mode      = LED_GPIO_MODE,
  };

  ESP_LOGI(TAG, "Init. indicator subsystem");
  g_led_handle = led_indicator_create(PRJDEF_INDICATOR_LED_PIN, &indicator_config);
  if (NULL == g_led_handle) {
    ESP_LOGE(TAG, "Failed to create LED indicator");
  }

  // Initialize Buttons

  button_config_t btncfg = {
        .type = BUTTON_TYPE_GPIO,
        //.long_press_time = CONFIG_BUTTON_LONG_PRESS_TIME_MS,
        //.short_press_time = CONFIG_BUTTON_SHORT_PRESS_TIME_MS,
        .gpio_button_config = {
            .gpio_num = 0,
            .active_level = 0,
        },
    };

  ESP_LOGI(TAG, "Init. button subsystem");
  g_btns[0] = iot_button_create(&btncfg);
  iot_button_register_cb(g_btns[0], BUTTON_SINGLE_CLICK, button_single_click_cb, NULL);
  iot_button_register_cb(g_btns[0], BUTTON_DOUBLE_CLICK, button_double_click_cb, NULL);
  iot_button_register_cb(g_btns[0], BUTTON_LONG_PRESS_START, button_long_press_start_cb, NULL);
  iot_button_register_cb(g_btns[0], BUTTON_LONG_PRESS_HOLD, button_long_press_hold_cb, NULL);

  if (led_indicator_start(g_led_handle, BLINK_CONNECTING) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start indicator light");
  }

  // ----------------------------------------------------------------------------
  //                        NVS - Persistent storage
  // ----------------------------------------------------------------------------

  // Init persistent storage
  ESP_LOGI(TAG, "Persistent storage ... ");

  rv = nvs_open("config", NVS_READWRITE, &g_nvsHandle);
  if (rv != ESP_OK) {
    ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(rv));
  }
  else {
    // Read (or set to defaults) persistent values
    readPersistentConfigs();
  }

  g_ctrl_task_sem = xSemaphoreCreateBinary();

  /*
    g_wifi_event_group = xEventGroupCreate();

     esp_event_loop_args_t beta_loop_config = {
      .queue_size = 10,
      .task_name = "alpha loop",
      .task_priority = uxTaskPriorityGet(NULL),
      .task_stack_size = 2048,
      .task_core_id = tskNO_AFFINITY
    };

    esp_event_loop_handle_t beta_loop_handle;
    ESP_ERROR_CHECK(esp_event_loop_create(&beta_loop_config, &beta_loop_handle));

    ESP_ERROR_CHECK(esp_event_handler_register_with(beta_loop_handle,
                                                             BETA_EVENT,
                                                             ESP_EVENT_ANY_ID,
                                                             beta_event_handler,
                                                             NULL));
    */
  

  if (led_indicator_start(g_led_handle, BLINK_PROVISIONING) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start indicator light");
  }

  // ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &system_event_handler, NULL));
  // ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &system_event_handler, NULL));
  // ESP_ERROR_CHECK(esp_event_handler_register(BETA_EVENT, ESP_EVENT_ANY_ID, &system_event_handler, NULL));

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  g_netif = esp_netif_create_default_wifi_ap();  
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  //ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  //ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  
  // Configure AP paramters
  if (ESP_OK != (ret = setAccessPointParameters())) {
    ESP_LOGE(TAG, "Unable top set AP parameters. rv =%X", ret);
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_start());

  if (g_persistent.dropletChannel) {
    ret = esp_wifi_set_channel(g_persistent.dropletChannel, WIFI_SECOND_CHAN_NONE);
    if (ESP_OK != ret) {
      ESP_LOGE(TAG, "Failed to set channel %d ret=%X", g_persistent.dropletChannel, ret);
    }
  }

  if (g_persistent.dropletLongRange) {
    ESP_ERROR_CHECK(
      esp_wifi_set_protocol(ESP_IF_WIFI_STA,
                            WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));
  }

  if (led_indicator_start(g_led_handle, BLINK_CONNECTING) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start indicator light");
  }

  // ----------------------------------------------------------------------------
  //                                   Spiffs
  // ----------------------------------------------------------------------------

  // Initialize Spiffs for web pages
  ESP_LOGI(TAG, "Initializing SPIFFS");

  esp_vfs_spiffs_conf_t spiffsconf = { .base_path              = "/spiffs",
                                       .partition_label        = "web",
                                       .max_files              = 50,
                                       .format_if_mount_failed = true };

  // Initialize and mount SPIFFS filesystem.
  ret = esp_vfs_spiffs_register(&spiffsconf);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount or format web filesystem");
    }
    else if (ret == ESP_ERR_NOT_FOUND) {
      ESP_LOGE(TAG, "Failed to find SPIFFS partition for web ");
    }
    else {
      ESP_LOGE(TAG, "Failed to initialize SPIFFS for web (%s)", esp_err_to_name(ret));
    }
    return;
  }

  ESP_LOGI(TAG, "Performing SPIFFS_check().");
  ret = esp_spiffs_check(spiffsconf.partition_label);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS_check() failed (%s)", esp_err_to_name(ret));
    return;
  }
  else {
    ESP_LOGI(TAG, "SPIFFS_check() successful");
  }

  ESP_LOGI(TAG, "SPIFFS for web initialized");

  size_t total = 0, used = 0;
  ret = esp_spiffs_info(spiffsconf.partition_label, &total, &used);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s). Formatting...", esp_err_to_name(ret));
    esp_spiffs_format(spiffsconf.partition_label);
    return;
  }
  else {
    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
  }

  DIR *dir = opendir("/spiffs");
  if (dir == NULL) {
    return;
  }

  while (true) {

    struct dirent *de = readdir(dir);
    if (!de) {
      break;
    }

    printf("Found file: %s\n", de->d_name);
  }

  closedir(dir);

  // Start LED controlling tast
  // xTaskCreate(&led_task, "led_task", 1024, NULL, 5, NULL);

  // ----------------------------------------------------------------------------
  //                              Droplet
  // ----------------------------------------------------------------------------

  // Initialize droplet
  droplet_config_t droplet_config = { .nodeType               = VSCP_DROPLET_BETA,
                                      .channel                = g_persistent.dropletChannel,
                                      .ttl                    = g_persistent.dropletTtl,
                                      .bForwardEnable         = g_persistent.dropletForwardEnable,
                                      .sizeQueue              = g_persistent.dropletSizeQueue,
                                      .bFilterAdjacentChannel = g_persistent.dropletFilterAdjacentChannel,
                                      .bForwardSwitchChannel  = g_persistent.dropletForwardSwitchChannel,
                                      .filterWeakSignal       = g_persistent.dropletFilterWeakSignal };

  // Set local key
  droplet_config.lkey = g_persistent.lkey;

  // Set primary key
  droplet_config.pmk = g_persistent.pmk;

  // Set GUID
  droplet_config.nodeGuid = g_persistent.nodeGuid;

  // Set callback for droplet receive events
  droplet_set_vscp_user_handler_cb(droplet_receive_cb);

  // Set callback for droplet node network attach
  droplet_set_attach_network_handler_cb(droplet_network_attach_cb);

  if (ESP_OK != droplet_init(&droplet_config)) {
    ESP_LOGE(TAG, "Failed to initialize espnow");
  }

  ESP_LOGI(TAG, "espnow initializated");

  // startOTA();

  ESP_LOGI(TAG, "Going to work now!");

  /*
    Start main application loop now
  */

  /* const char *obj = "{"
     "\"vscpHead\": 2,"
     "\"vscpObId\": 123,"
     "\"vscpDateTime\": \"2017-01-13T10:16:02\","
     "\"vscpTimeStamp\":50817,"
     "\"vscpClass\": 10,"
     "\"vscpType\": 8,"
     "\"vscpGuid\": \"00:00:00:00:00:00:00:00:00:00:00:00:00:01:00:02\","
     "\"vscpData\": [1,2,3,4,5,6,7],"
     "\"note\": \"This is some text\""
  "}";

vscpEventEx ex;
droplet_parse_vscp_json(obj, &ex);
char str[512];
droplet_create_vscp_json(str, &ex); */

  // test();

  while (1) {
    // esp_task_wdt_reset();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }

  // Unmount web spiffs partition and disable SPIFFS
  esp_vfs_spiffs_unregister(spiffsconf.partition_label);
  ESP_LOGI(TAG, "web SPIFFS unmounted");

  // Clean up
  iot_button_delete(g_btns[0]);

  // Close
  nvs_close(g_nvsHandle);
}
