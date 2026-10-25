#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>

void web_server_start(uint16_t port);
void web_server_stop(void);
void web_server_trigger_scan(void);
void web_server_set_zigbee_info(const char *ieee, uint16_t pan_id, uint8_t channel, uint16_t short_addr);
void web_server_notify_paired(const char *name, const char *ieee, const char *model);

#endif
