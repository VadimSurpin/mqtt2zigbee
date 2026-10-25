#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_endpoint.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_attribute.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_analog_input.h"
#include "zigbee_coordinator.h"

static const char *TAG = "ZIGBEE_COORD";

static esp_zb_ep_list_t *s_ep_list = NULL;
static zigbee_attr_report_cb_t  s_attr_report_cb  = NULL;
static zigbee_network_join_cb_t s_network_join_cb = NULL;
static bool s_zb_started = false;
static void (*s_endpoint_setup_cb)(void) = NULL;

/* Per-endpoint Basic cluster identity set by zigbee_coordinator_set_basic_info()
   before each add_*_endpoint call, consumed and cleared by create_endpoint(). */
static char s_pending_mfr[33]   = {0};
static char s_pending_model[33] = {0};

void zigbee_coordinator_set_attr_report_callback(zigbee_attr_report_cb_t cb)
{
    s_attr_report_cb = cb;
}

void zigbee_coordinator_set_network_join_callback(zigbee_network_join_cb_t cb)
{
    s_network_join_cb = cb;
}

void zigbee_coordinator_set_basic_info(const char *mfr_name, const char *model_id)
{
    strncpy(s_pending_mfr,   mfr_name  ? mfr_name  : "", sizeof(s_pending_mfr)   - 1);
    strncpy(s_pending_model, model_id  ? model_id  : "", sizeof(s_pending_model) - 1);
}

/* ZCL action callback — forwards attribute reports/writes via the registered
   callback so the caller (main) can route them to device_manager without
   creating a circular component dependency. */
static esp_err_t zigbee_action_callback(esp_zb_core_action_callback_id_t callback_id,
                                         const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
        const esp_zb_zcl_report_attr_message_t *msg =
            (const esp_zb_zcl_report_attr_message_t *)message;
        if (msg->status != ESP_ZB_ZCL_STATUS_SUCCESS) break;
        ESP_LOGI(TAG, "Attr report ep=%d cluster=0x%04x attr=0x%04x",
                 msg->dst_endpoint, msg->cluster, msg->attribute.id);
        if (s_attr_report_cb) {
            s_attr_report_cb(msg->dst_endpoint, msg->cluster, msg->attribute.id,
                             msg->attribute.data.value, msg->attribute.data.size,
                             (uint8_t)msg->attribute.data.type);
        }
        break;
    }
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
        const esp_zb_zcl_set_attr_value_message_t *msg =
            (const esp_zb_zcl_set_attr_value_message_t *)message;
        if (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) break;
        ESP_LOGI(TAG, "Set attr ep=%d cluster=0x%04x attr=0x%04x",
                 msg->info.dst_endpoint, msg->info.cluster, msg->attribute.id);
        if (s_attr_report_cb) {
            s_attr_report_cb(msg->info.dst_endpoint, msg->info.cluster, msg->attribute.id,
                             msg->attribute.data.value, msg->attribute.data.size,
                             (uint8_t)msg->attribute.data.type);
        }
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p      = signal_struct->p_app_signal;
    esp_err_t  err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialised, scanning for network to join");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        ESP_LOGI(TAG, "First start — scanning for network to join");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            uint8_t  ch   = esp_zb_get_current_channel();
            uint16_t pan  = esp_zb_get_pan_id();
            uint16_t addr = esp_zb_get_short_address();
            ESP_LOGI(TAG, "Rejoined network (channel %d, PAN 0x%04x, addr 0x%04x)", ch, pan, addr);
            if (s_network_join_cb) s_network_join_cb(ch, pan, addr);
        } else {
            ESP_LOGW(TAG, "Failed to rejoin, retrying steering");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_NETWORK_STEERING);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            uint8_t  ch   = esp_zb_get_current_channel();
            uint16_t pan  = esp_zb_get_pan_id();
            uint16_t addr = esp_zb_get_short_address();
            ESP_LOGI(TAG, "Joined network (channel %d, PAN 0x%04x, addr 0x%04x)", ch, pan, addr);
            if (s_network_join_cb) s_network_join_cb(ch, pan, addr);
        } else {
            ESP_LOGW(TAG, "Failed to join network, retrying in 5 s");
            esp_zb_scheduler_alarm(
                (esp_zb_callback_t)esp_zb_bdb_start_top_level_commissioning,
                ESP_ZB_BDB_NETWORK_STEERING, 5000);
        }
        break;

    default:
        ESP_LOGD(TAG, "ZDO signal 0x%x: %s", sig_type, esp_err_to_name(err_status));
        break;
    }
}

static void add_cluster_to_list(esp_zb_cluster_list_t *cluster_list, uint16_t cluster_id)
{
    if (cluster_id == 0x0006) {
        esp_zb_attribute_list_t *attr = esp_zb_on_off_cluster_create(NULL);
        esp_zb_cluster_list_add_on_off_cluster(cluster_list, attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    } else if (cluster_id == 0x0008) {
        esp_zb_attribute_list_t *attr = esp_zb_level_cluster_create(NULL);
        esp_zb_cluster_list_add_level_cluster(cluster_list, attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    } else if (cluster_id == 0x0300) {
        esp_zb_attribute_list_t *attr = esp_zb_color_control_cluster_create(NULL);
        esp_zb_cluster_list_add_color_control_cluster(cluster_list, attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    } else if (cluster_id == 0x0402) {
        esp_zb_attribute_list_t *attr = esp_zb_temperature_meas_cluster_create(NULL);
        esp_zb_cluster_list_add_temperature_meas_cluster(cluster_list, attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    } else if (cluster_id == 0x0405) {
        esp_zb_attribute_list_t *attr = esp_zb_humidity_meas_cluster_create(NULL);
        esp_zb_cluster_list_add_humidity_meas_cluster(cluster_list, attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    } else if (cluster_id == 0x0403) {
        esp_zb_attribute_list_t *attr = esp_zb_pressure_meas_cluster_create(NULL);
        esp_zb_cluster_list_add_pressure_meas_cluster(cluster_list, attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    } else if (cluster_id == 0x0500) {
        esp_zb_attribute_list_t *attr = esp_zb_ias_zone_cluster_create(NULL);
        esp_zb_cluster_list_add_ias_zone_cluster(cluster_list, attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    } else if (cluster_id == 0x000C) {
        esp_zb_analog_input_cluster_cfg_t ai_cfg = {
            .out_of_service = false,
            .present_value  = 0.0f,
            .status_flags   = 0,
        };
        esp_zb_attribute_list_t *attr = esp_zb_analog_input_cluster_create(&ai_cfg);
        esp_zb_cluster_list_add_analog_input_cluster(cluster_list, attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    }
}

/* ZCL character strings: first byte is length, no null terminator. */
static void make_zcl_str(const char *s, uint8_t *buf, uint8_t buf_max)
{
    uint8_t len = (uint8_t)strnlen(s, buf_max - 1);
    buf[0] = len;
    memcpy(buf + 1, s, len);
}

static esp_err_t create_endpoint(uint8_t endpoint_id, uint16_t device_id,
                                  const uint16_t *clusters, size_t cluster_count)
{
    if (!s_ep_list) {
        s_ep_list = esp_zb_ep_list_create();
        if (!s_ep_list) return ESP_FAIL;
    }

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    if (!cluster_list) return ESP_FAIL;

    /* Basic cluster — always present; carry manufacturer name / model if set. */
    {
        esp_zb_basic_cluster_cfg_t bcfg = {
            .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
            .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
        };
        esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&bcfg);

        /* esp_zb_basic_cluster_create may pre-populate ManufacturerName and
           ModelIdentifier as empty strings. Try add_attr first; if the SDK
           returns an error (attribute already exists), fall back to update.
           Heap-allocate ZCL string buffers so each endpoint has its own copy
           that persists for the lifetime of the Zigbee stack. */
        if (s_pending_mfr[0]) {
            uint8_t *mfr_zcl = calloc(1, 34);
            if (mfr_zcl) {
                make_zcl_str(s_pending_mfr, mfr_zcl, 34);
                if (esp_zb_basic_cluster_add_attr(basic,
                        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, mfr_zcl) != ESP_OK) {
                    esp_zb_cluster_update_attr(basic,
                        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, mfr_zcl);
                }
            }
            ESP_LOGI(TAG, "ep%d manufacturer='%s'", endpoint_id, s_pending_mfr);
        }
        if (s_pending_model[0]) {
            uint8_t *model_zcl = calloc(1, 34);
            if (model_zcl) {
                make_zcl_str(s_pending_model, model_zcl, 34);
                if (esp_zb_basic_cluster_add_attr(basic,
                        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model_zcl) != ESP_OK) {
                    esp_zb_cluster_update_attr(basic,
                        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model_zcl);
                }
            }
            ESP_LOGI(TAG, "ep%d model='%s'", endpoint_id, s_pending_model);
        }
        esp_zb_cluster_list_add_basic_cluster(cluster_list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        /* Consumed — clear for the next call. */
        s_pending_mfr[0]   = '\0';
        s_pending_model[0] = '\0';
    }

    for (size_t i = 0; i < cluster_count; i++) {
        add_cluster_to_list(cluster_list, clusters[i]);
    }

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint           = endpoint_id,
        .app_device_id      = device_id,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_version = 1,
    };

    esp_err_t ret = esp_zb_ep_list_add_ep(s_ep_list, cluster_list, ep_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Endpoint %d registered (%zu clusters)", endpoint_id, cluster_count);
    } else {
        ESP_LOGE(TAG, "Failed to register endpoint %d: %s", endpoint_id, esp_err_to_name(ret));
    }
    return ret;
}

static void zigbee_main_task(void *arg)
{
    ESP_LOGI(TAG, "Free heap before Zigbee init: %lu", esp_get_free_heap_size());

    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role         = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg    = { .max_children = 10 },
    };

    esp_zb_init(&zb_cfg);
    ESP_LOGI(TAG, "Free heap after Zigbee init: %lu", esp_get_free_heap_size());

    /* Build endpoint list now that esp_zb_init() has run — safe to call esp_zb_* here. */
    if (s_endpoint_setup_cb) s_endpoint_setup_cb();

    esp_zb_core_action_handler_register(zigbee_action_callback);

    /* Scan all 2.4 GHz Zigbee channels (11–26) so we can find the hub's channel. */
    esp_err_t err = esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set channel mask: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* Fallback EP1 — only when no device endpoints were configured.
       esp_zb_device_register needs at least one endpoint.
       When devices ARE configured (s_ep_list != NULL), EP1 is skipped so
       Alice discovers exactly N devices instead of N+1. */
    if (!s_ep_list) {
        uint16_t onoff[] = {0x0006};
        zigbee_coordinator_set_basic_info("Espressif", "MQTT-GW");
        create_endpoint(1, ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID, onoff, 1);
    }

    err = esp_zb_device_register(s_ep_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register device: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = esp_zb_start(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Zigbee: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    s_zb_started = true;
    esp_zb_stack_main_loop();   /* blocks here — runs the Zigbee host loop */
    vTaskDelete(NULL);
}

void zigbee_coordinator_start(void (*endpoint_setup_cb)(void))
{
    static bool s_started = false;
    if (s_started) return;
    s_started = true;
    s_endpoint_setup_cb = endpoint_setup_cb;
    ESP_LOGI(TAG, "Starting Zigbee Coordinator task");
    xTaskCreate(zigbee_main_task, "zigbee_main", 8192, NULL, 4, NULL);
}

void zigbee_coordinator_stop(void)
{
    ESP_LOGI(TAG, "Stopping Zigbee Coordinator");
    esp_zb_factory_reset();
}

static void do_factory_reset(uint8_t param)
{
    (void)param;
    ESP_LOGI(TAG, "Erasing Zigbee NVS and rebooting");
    esp_zb_factory_reset();
}

void zigbee_coordinator_leave_network(void)
{
    ESP_LOGI(TAG, "Leaving Zigbee network");
    if (s_zb_started) {
        /* Run inside the Zigbee task — esp_zb_factory_reset needs stack context. */
        esp_zb_scheduler_alarm(do_factory_reset, 0, 100);
    } else {
        esp_zb_factory_reset();
    }
}

esp_err_t zigbee_coordinator_add_onoff_endpoint(uint8_t endpoint_id)
{
    uint16_t clusters[] = {0x0006};
    return create_endpoint(endpoint_id, ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID, clusters, 1);
}

esp_err_t zigbee_coordinator_add_switch_endpoint(uint8_t endpoint_id)
{
    uint16_t clusters[] = {0x0006};
    return create_endpoint(endpoint_id, ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID, clusters, 1);
}

esp_err_t zigbee_coordinator_add_temperature_endpoint(uint8_t endpoint_id)
{
    uint16_t clusters[] = {0x0402};
    return create_endpoint(endpoint_id, ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID, clusters, 1);
}

esp_err_t zigbee_coordinator_add_humidity_endpoint(uint8_t endpoint_id)
{
    uint16_t clusters[] = {0x0405};
    return create_endpoint(endpoint_id, ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID, clusters, 1);
}

esp_err_t zigbee_coordinator_add_pressure_endpoint(uint8_t endpoint_id)
{
    uint16_t clusters[] = {0x0403};
    return create_endpoint(endpoint_id, ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID, clusters, 1);
}

esp_err_t zigbee_coordinator_add_contact_endpoint(uint8_t endpoint_id)
{
    uint16_t clusters[] = {0x0500};
    return create_endpoint(endpoint_id, ESP_ZB_HA_IAS_ZONE_ID, clusters, 1);
}

esp_err_t zigbee_coordinator_add_rgb_light_endpoint(uint8_t endpoint_id)
{
    uint16_t clusters[] = {0x0006, 0x0008, 0x0300};
    return create_endpoint(endpoint_id, ESP_ZB_HA_COLOR_DIMMABLE_LIGHT_DEVICE_ID, clusters, 3);
}

static esp_err_t set_attr(uint8_t ep, uint16_t cluster, uint16_t attr, void *val)
{
    if (!s_zb_started) return ESP_OK;  /* stack not running yet */
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(50))) {
        ESP_LOGW(TAG, "set_attr: could not acquire Zigbee lock");
        return ESP_FAIL;
    }
    /* check=false: allow local app writes to ZCL read-only attributes (e.g. OnOff) */
    esp_zb_zcl_status_t st = esp_zb_zcl_set_attribute_val(
        ep, cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, val, false);
    esp_zb_lock_release();
    if (st != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "set_attr ep=%d cluster=0x%04x attr=0x%04x failed: %d",
                 ep, cluster, attr, st);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t zigbee_coordinator_set_onoff(uint8_t endpoint_id, bool on)
{
    uint8_t value = on ? 1 : 0;
    ESP_LOGI(TAG, "Set On/Off ep=%d: %s", endpoint_id, on ? "ON" : "OFF");
    return set_attr(endpoint_id, 0x0006, 0x0000, &value);
}

esp_err_t zigbee_coordinator_set_rgb_light(uint8_t endpoint_id, bool on, uint8_t brightness,
                                             uint8_t r, uint8_t g, uint8_t b)
{
    ESP_LOGI(TAG, "Set RGB ep=%d on=%d bri=%d rgb=(%d,%d,%d)",
             endpoint_id, on, brightness, r, g, b);
    uint8_t onoff_val = on ? 1 : 0;
    set_attr(endpoint_id, 0x0006, 0x0000, &onoff_val);
    set_attr(endpoint_id, 0x0008, 0x0000, &brightness);
    return ESP_OK;
}

esp_err_t zigbee_coordinator_set_temperature(uint8_t endpoint_id, float celsius)
{
    int16_t value = (int16_t)(celsius * 100.0f);
    ESP_LOGI(TAG, "Set temperature ep=%d: %.2f°C", endpoint_id, celsius);
    return set_attr(endpoint_id, 0x0402, 0x0000, &value);
}

esp_err_t zigbee_coordinator_set_humidity(uint8_t endpoint_id, float percent)
{
    uint16_t value = (uint16_t)(percent * 100.0f);
    ESP_LOGI(TAG, "Set humidity ep=%d: %.2f%%", endpoint_id, percent);
    return set_attr(endpoint_id, 0x0405, 0x0000, &value);
}

esp_err_t zigbee_coordinator_set_pressure(uint8_t endpoint_id, float hpa)
{
    int16_t value = (int16_t)hpa;
    ESP_LOGI(TAG, "Set pressure ep=%d: %.1f hPa", endpoint_id, hpa);
    return set_attr(endpoint_id, 0x0403, 0x0000, &value);
}

esp_err_t zigbee_coordinator_set_contact(uint8_t endpoint_id, bool open)
{
    uint16_t value = open ? 0x0001 : 0x0000;
    ESP_LOGI(TAG, "Set contact ep=%d: %s", endpoint_id, open ? "open" : "closed");
    return set_attr(endpoint_id, 0x0500, 0x0002, &value);
}

esp_err_t zigbee_coordinator_add_analog_input_endpoint(uint8_t endpoint_id)
{
    uint16_t clusters[] = {0x000C};
    return create_endpoint(endpoint_id, ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID, clusters, 1);
}

esp_err_t zigbee_coordinator_add_level_endpoint(uint8_t endpoint_id)
{
    uint16_t clusters[] = {0x0006, 0x0008};
    return create_endpoint(endpoint_id, ESP_ZB_HA_LEVEL_CONTROLLABLE_OUTPUT_DEVICE_ID, clusters, 2);
}

esp_err_t zigbee_coordinator_set_analog_input(uint8_t endpoint_id, float value)
{
    ESP_LOGI(TAG, "Set analog input ep=%d: %.4f", endpoint_id, value);
    return set_attr(endpoint_id, 0x000C,
                    ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, &value);
}

esp_err_t zigbee_coordinator_set_level(uint8_t endpoint_id, uint8_t level)
{
    ESP_LOGI(TAG, "Set level ep=%d: %u", endpoint_id, level);
    return set_attr(endpoint_id, 0x0008, 0x0000, &level);
}
