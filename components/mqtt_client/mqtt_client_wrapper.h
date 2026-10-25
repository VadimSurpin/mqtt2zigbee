#ifndef MQTT_CLIENT_WRAPPER_H
#define MQTT_CLIENT_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

void mqtt_client_start(void);
void mqtt_client_stop(void);
esp_err_t mqtt_client_publish(const char *topic, const char *data, int qos, int retain);
esp_err_t mqtt_client_subscribe(const char *topic, int qos);
void mqtt_client_set_message_handler(void (*handler)(const char *topic, const char *data, int len));
void mqtt_client_set_connect_callback(void (*cb)(bool connected));
bool mqtt_client_is_connected(void);

bool mqtt_client_test_connection(const char *host, int port, bool use_tls, 
    const char *client_id, const char *username, const char *password);

int mqtt_client_get_child_topics(const char* path, char children[][256], int max_count);
int mqtt_client_get_all_topics(char topics[][256], int max_count);
int mqtt_client_get_all_topics_json(char* json_buf, int buf_size);
void        mqtt_client_clear_topics(void);
void        mqtt_client_rescan_topics(void);
void        mqtt_client_capture_topic(const char *topic);
bool        mqtt_client_is_capture_ready(void);
const char *mqtt_client_get_capture_buf(void);

/* Deliver every fragment of one topic, in order, to cb(data, len, offset, total) instead of
 * buffering it. For large retained payloads (e.g. zigbee2mqtt/bridge/devices) that must not
 * be held in RAM. Re-subscribes so the retained message is sent again; cb=NULL stops it. */
void        mqtt_client_stream_topic(const char *topic,
                                     void (*cb)(const char *data, int len, int offset, int total));

#endif
