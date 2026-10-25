#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "mqtt_client_wrapper.h"
#include "app_config.h"
#include "device_manager.h"

static const char *TAG = "MQTT_CLIENT";
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static void (*s_message_handler)(const char *topic, const char *data, int len) = NULL;
static void (*s_connect_callback)(bool connected) = NULL;

#define MAX_SUBSCRIPTIONS 32
static char s_subscriptions[MAX_SUBSCRIPTIONS][128];
static int s_subscription_count = 0;

#define MAX_TOPICS 128
static char s_discovered_topics[MAX_TOPICS][256];
static int s_topic_count = 0;
static bool s_scanning = false;

/* Single-topic payload capture (for bridge/devices retained message). */
#define CAPTURE_BUF_SIZE (24 * 1024)
/* Streaming consumer for one (possibly fragmented) topic — see mqtt_client_stream_topic(). */
static void (*s_stream_cb)(const char *data, int len, int offset, int total) = NULL;
static char s_stream_topic[128] = {0};
static bool s_streaming = false;

static char s_capture_topic[256] = {0};
static char s_capture_buf[CAPTURE_BUF_SIZE];
static int  s_capture_total_len  = 0;
static int  s_capture_written    = 0;
static bool s_capture_ready      = false;
static bool s_capturing          = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "MQTT connected");
            for (int i = 0; i < s_subscription_count; i++) {
                esp_mqtt_client_subscribe(s_client, s_subscriptions[i], 1);
            }
            if (s_connect_callback) s_connect_callback(true);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGI(TAG, "MQTT disconnected");
            if (s_connect_callback) s_connect_callback(false);
            break;
        case MQTT_EVENT_DATA: {
            char topic_str[256] = {0};
            int tlen = event->topic_len < 255 ? event->topic_len : 255;
            if (tlen > 0) strncpy(topic_str, event->topic, tlen);

            /* Passively collect topics for the device-discovery UI. */
            if (s_topic_count < MAX_TOPICS && tlen > 0) {
                bool found = false;
                for (int i = 0; i < s_topic_count; i++) {
                    if (strcmp(s_discovered_topics[i], topic_str) == 0) { found = true; break; }
                }
                if (!found)
                    strncpy(s_discovered_topics[s_topic_count++], topic_str, 255);
            }

            /* Single-topic payload capture with fragmentation reassembly. */
            if (tlen > 0 && s_capture_topic[0] && strcmp(topic_str, s_capture_topic) == 0) {
                s_capturing = true;
                s_capture_total_len = event->total_data_len > 0 ? event->total_data_len : event->data_len;
                s_capture_written = 0;
                s_capture_ready = false;
            }
            if (s_capturing) {
                int off = event->current_data_offset;
                int len = event->data_len;
                int avail = CAPTURE_BUF_SIZE - 1 - off;
                if (avail > 0) {
                    int copy = len < avail ? len : avail;
                    memcpy(s_capture_buf + off, event->data, copy);
                    s_capture_written = off + copy;
                }
                if (off + len >= s_capture_total_len) {
                    s_capture_buf[s_capture_written] = '\0';
                    s_capture_ready = true;
                    s_capturing = false;
                    ESP_LOGI(TAG, "Capture complete: %d bytes", s_capture_written);
                }
            }

            /* Streaming consumer: sees every fragment in order. Continuation fragments
               carry no topic, so the match made on the first fragment is remembered. */
            if (s_stream_cb && s_stream_topic[0]) {
                if (tlen > 0) s_streaming = (strcmp(topic_str, s_stream_topic) == 0);
                if (s_streaming) {
                    int total = event->total_data_len > 0 ? event->total_data_len : event->data_len;
                    s_stream_cb(event->data, event->data_len, event->current_data_offset, total);
                    if (event->current_data_offset + event->data_len >= total) s_streaming = false;
                }
            }

            if (s_message_handler && tlen > 0)
                s_message_handler(topic_str, event->data, event->data_len);
            break;
        }
        default:
            break;
    }
}

void mqtt_client_start(void)
{
    if (s_client) return;

    const char *host = app_config_get_mqtt_host();
    if (!host || host[0] == '\0') {
        ESP_LOGW(TAG, "MQTT broker not configured, skipping");
        return;
    }

    uint16_t port = app_config_get_mqtt_port();
    const char *client_id = app_config_get_mqtt_client_id();
    const char *username = app_config_get_mqtt_username();
    const char *password = app_config_get_mqtt_password();

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt%s://%s:%d", 
             app_config_get_mqtt_use_tls() ? "s" : "", host, port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = client_id,
        .credentials.username = username,
        .credentials.authentication.password = password,
        .network.timeout_ms = 5000,
        .network.reconnect_timeout_ms = 30000,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT client started: %s (free heap: %lu)", uri, esp_get_free_heap_size());
}

void mqtt_client_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }
}

esp_err_t mqtt_client_publish(const char *topic, const char *data, int qos, int retain)
{
    if (!s_client || !s_connected) return ESP_FAIL;
    return esp_mqtt_client_publish(s_client, topic, data, 0, qos, retain);
}

esp_err_t mqtt_client_subscribe(const char *topic, int qos)
{
    // Always buffer so subscriptions survive reconnects and pre-connection calls.
    bool found = false;
    for (int i = 0; i < s_subscription_count; i++) {
        if (strcmp(s_subscriptions[i], topic) == 0) { found = true; break; }
    }
    if (!found && s_subscription_count < MAX_SUBSCRIPTIONS) {
        strncpy(s_subscriptions[s_subscription_count++], topic, 127);
    }
    if (!s_client || !s_connected) return ESP_OK;  // will be applied on MQTT_EVENT_CONNECTED
    int msg_id = esp_mqtt_client_subscribe(s_client, topic, qos);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

void mqtt_client_set_message_handler(void (*handler)(const char *topic, const char *data, int len))
{
    s_message_handler = handler;
}

void mqtt_client_set_connect_callback(void (*cb)(bool connected))
{
    s_connect_callback = cb;
}

bool mqtt_client_is_connected(void)
{
    return s_connected;
}

static void scan_mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            s_scanning = true;
            ESP_LOGI(TAG, "Test connection connected, scanning for topics...");
            esp_mqtt_client_subscribe(s_client, "#", 0);
            break;
        case MQTT_EVENT_DATA:
            if (s_topic_count < MAX_TOPICS) {
                char topic_str[256] = {0};
                int len = event->topic_len < 255 ? event->topic_len : 255;
                strncpy(topic_str, event->topic, len);
                bool found = false;
                for (int i = 0; i < s_topic_count; i++) {
                    if (strcmp(s_discovered_topics[i], topic_str) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found && strlen(topic_str) > 0) {
                    strncpy(s_discovered_topics[s_topic_count++], topic_str, 255);
                }
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            s_scanning = false;
            break;
        default:
            break;
    }
}

bool mqtt_client_test_connection(const char *host, int port, bool use_tls, 
    const char *client_id, const char *username, const char *password)
{
    if (s_client) {
        mqtt_client_stop();
    }

    s_topic_count = 0;

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt%s://%s:%d", 
             use_tls ? "s" : "", host, port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = client_id,
        .credentials.username = username,
        .credentials.authentication.password = password,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, scan_mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    for (int i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_connected && s_scanning) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            mqtt_client_stop();
            s_client = NULL;
            s_connected = false;
            return true;
        }
    }

    mqtt_client_stop();
    s_client = NULL;
    s_connected = false;
    return false;
}

int mqtt_client_get_discovered_topics(char topics[][256], int max_count)
{
    int count = s_topic_count < max_count ? s_topic_count : max_count;
    for (int i = 0; i < count; i++) {
        strncpy(topics[i], s_discovered_topics[i], 255);
    }
    return count;
}

int mqtt_client_get_all_topics(char topics[][256], int max_count)
{
    return mqtt_client_get_discovered_topics(topics, max_count);
}

int mqtt_client_get_all_topics_json(char* json_buf, int buf_size)
{
    int count = s_topic_count < 500 ? s_topic_count : 500;
    int offset = 0;
    offset += snprintf(json_buf + offset, buf_size - offset, "{\"topics\":[");
    for (int i = 0; i < count; i++) {
        if (i > 0) offset += snprintf(json_buf + offset, buf_size - offset, ",");
        offset += snprintf(json_buf + offset, buf_size - offset, "\"%s\"", s_discovered_topics[i]);
    }
    offset += snprintf(json_buf + offset, buf_size - offset, "]}");
    return offset;
}

void mqtt_client_clear_topics(void)
{
    s_topic_count = 0;
}

void mqtt_client_rescan_topics(void)
{
    s_topic_count = 0;
    /* Remove '#' from the persistent buffer so mqtt_client_subscribe will
       send a fresh SUBSCRIBE packet — the broker then re-delivers all retained
       messages, which is how passive topic discovery works. */
    for (int i = 0; i < s_subscription_count; i++) {
        if (strcmp(s_subscriptions[i], "#") == 0) {
            for (int j = i; j < s_subscription_count - 1; j++)
                memcpy(s_subscriptions[j], s_subscriptions[j + 1], sizeof(s_subscriptions[0]));
            s_subscription_count--;
            break;
        }
    }
    if (s_client && s_connected)
        esp_mqtt_client_unsubscribe(s_client, "#");
    mqtt_client_subscribe("#", 0);
}

int mqtt_client_get_child_topics(const char* path, char children[][256], int max_count)
{
    int count = 0;
    char normalized_path[256] = {0};
    int path_len = 0;
    
    if (path && strlen(path) > 0) {
        path_len = strlen(path);
        strncpy(normalized_path, path, sizeof(normalized_path) - 1);
        if (normalized_path[path_len - 1] == '/') {
            normalized_path[path_len - 1] = '\0';
            path_len--;
        }
    }
    
    path_len = strlen(normalized_path);
    
    for (int i = 0; i < s_topic_count && count < max_count; i++) {
        if (path_len > 0 && strncmp(s_discovered_topics[i], normalized_path, path_len) != 0) {
            continue;
        }
        
        const char* rest = s_discovered_topics[i] + path_len;
        if (*rest == '\0') {
            continue;
        }
        if (*rest == '/') rest++;
        if (*rest == '\0') {
            continue;
        }
        
        char prefix[256];
        const char* slash = strchr(rest, '/');
        if (slash) {
            strncpy(prefix, rest, slash - rest);
            prefix[slash - rest] = '\0';
        } else {
            strncpy(prefix, rest, 255);
            prefix[254] = '\0';
        }
        
        bool already_added = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(children[j], prefix) == 0) {
                already_added = true;
                break;
            }
        }
        
        if (!already_added && strlen(prefix) > 0) {
            strncpy(children[count++], prefix, 255);
            children[count-1][255] = '\0';
        }
    }
    
    ESP_LOGI(TAG, "get_child_topics: returning %d children", count);
    return count;
}

void mqtt_client_capture_topic(const char *topic)
{
    strncpy(s_capture_topic, topic, sizeof(s_capture_topic) - 1);
    s_capture_topic[sizeof(s_capture_topic) - 1] = '\0';
    s_capture_ready = false;
    s_capturing = false;
    s_capture_written = 0;
    /* Force fresh SUBSCRIBE so the broker re-delivers the retained message. */
    if (s_client && s_connected)
        esp_mqtt_client_unsubscribe(s_client, topic);
    for (int i = 0; i < s_subscription_count; i++) {
        if (strcmp(s_subscriptions[i], topic) == 0) {
            for (int j = i; j < s_subscription_count - 1; j++)
                memcpy(s_subscriptions[j], s_subscriptions[j + 1], sizeof(s_subscriptions[0]));
            s_subscription_count--;
            break;
        }
    }
    mqtt_client_subscribe(topic, 0);
    ESP_LOGI(TAG, "Capture started for: %s", topic);
}

bool mqtt_client_is_capture_ready(void) { return s_capture_ready; }
const char *mqtt_client_get_capture_buf(void) { return s_capture_buf; }

void mqtt_client_stream_topic(const char *topic, void (*cb)(const char *data, int len, int offset, int total))
{
    s_streaming = false;
    s_stream_cb = cb;
    s_stream_topic[0] = '\0';
    if (!topic || !cb) return;
    strncpy(s_stream_topic, topic, sizeof(s_stream_topic) - 1);
    /* Force a fresh SUBSCRIBE so the broker re-delivers the retained message. */
    if (s_client && s_connected)
        esp_mqtt_client_unsubscribe(s_client, topic);
    for (int i = 0; i < s_subscription_count; i++) {
        if (strcmp(s_subscriptions[i], topic) == 0) {
            for (int j = i; j < s_subscription_count - 1; j++)
                memcpy(s_subscriptions[j], s_subscriptions[j + 1], sizeof(s_subscriptions[0]));
            s_subscription_count--;
            break;
        }
    }
    mqtt_client_subscribe(topic, 0);
}
