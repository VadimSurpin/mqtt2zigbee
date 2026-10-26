#ifndef STATUS_LED_H
#define STATUS_LED_H

typedef enum {
    LED_STATE_NOT_CONFIGURED,  // AP provisioning mode  — blue,  slow pulse 1 Hz
    LED_STATE_WIFI_SEARCHING,  // STA connecting        — yellow, fast blink 5 Hz
    LED_STATE_WIFI_CONNECTED,  // IP obtained, no MQTT  — cyan,   double-pulse / 2 s
    LED_STATE_MQTT_CONNECTED,  // Fully operational     — LED off
    LED_STATE_CONN_ERROR,      // Connection failed     — red,    triple-flash / 2 s
} led_state_t;

void status_led_init(int gpio_num);
void status_led_set(led_state_t state);

#endif
