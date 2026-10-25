#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <stdint.h>
#include "esp_err.h"

#define RECONFIG_TRIGGER 0   /* long press (3 s) — factory reset */

void button_handler_init(void);
void button_handler_set_callback(uint8_t button_id, void (*callback)(void));
void button_handler_set_short_callback(void (*callback)(void));
void button_handler_deinit(void);

#endif
