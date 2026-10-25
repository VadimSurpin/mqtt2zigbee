#ifndef Z2M_DRIVER_H
#define Z2M_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/* Zigbee2MQTT base topic (its default). */
#define Z2M_BASE_TOPIC    "zigbee2mqtt"
#define Z2M_NAME_LEN      40
#define Z2M_MAX_ENTRIES   128

/* What a discovered property becomes on the Zigbee side. */
typedef enum {
    Z2M_KIND_NONE = 0,
    Z2M_KIND_SWITCH,       /* "state" of a plug/relay */
    Z2M_KIND_LIGHT,        /* "state" of a device that also has "brightness" */
    Z2M_KIND_TEMPERATURE,
    Z2M_KIND_HUMIDITY,
    Z2M_KIND_PRESSURE,
    Z2M_KIND_CONTACT,
    Z2M_KIND_ANALOG,       /* other numeric readings */
} z2m_kind_t;

/* One selectable property of a Zigbee2MQTT device (like a WB control). */
typedef struct {
    char    device[Z2M_NAME_LEN];   /* friendly_name == topic suffix */
    char    prop[20];               /* property in the device's JSON state */
    char    vendor[16];
    char    model[24];
    uint8_t kind;                   /* z2m_kind_t */
    bool    writable;
} z2m_entry_t;

/* Allocate the table (if needed), clear it and re-read <base>/bridge/devices. */
void z2m_driver_refresh(void);
/* Stop listening and free the table. */
void z2m_driver_release(void);

/* Entries found so far, grouped by device. Returns the count; *out may be NULL when 0. */
int  z2m_driver_get_entries(const z2m_entry_t **out);
bool z2m_driver_ready(void);       /* a full bridge/devices message has been parsed */
bool z2m_driver_truncated(void);   /* more entries existed than fit in the table */

/* Subtype string passed to the "z2m" driver's init_endpoint ("switch", "light", ...). */
const char *z2m_kind_name(uint8_t kind);

#endif
