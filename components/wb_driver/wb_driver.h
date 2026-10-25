#ifndef WB_DRIVER_H
#define WB_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#define WB_MAX_DEVICES    128
/* 38 bytes/entry → 1500 × 38 = ~57 KB heap during discovery (same as original 350 × 160) */
#define WB_MAX_CONTROLS   300

/* Flags bits in wb_control_t.flags */
#define WB_FLAG_VALID     0x01
#define WB_FLAG_READONLY  0x02

/* WB control type IDs — used for display only.
 * Writability is NOT inferred from the type: e.g. "switch" is used for both
 * writable relay outputs and read-only physical inputs (wb-mr).
 * Readonly flag is set only by retained /on message or explicit /meta/readonly. */
#define WB_TYPE_UNKNOWN              0
#define WB_TYPE_SWITCH               1
#define WB_TYPE_RANGE                2
#define WB_TYPE_PUSHBUTTON           3
#define WB_TYPE_RGB                  4
#define WB_TYPE_TEXT                 5
#define WB_TYPE_ALARM                6
#define WB_TYPE_FIRST_SENSOR         7  /* sensor types start here (informational) */
#define WB_TYPE_TEMPERATURE          7
#define WB_TYPE_REL_HUMIDITY         8
#define WB_TYPE_ATMOSPHERIC_PRESSURE 9
#define WB_TYPE_LUX                  10
#define WB_TYPE_POWER                11
#define WB_TYPE_POWER_CONSUMPTION    12
#define WB_TYPE_VOLTAGE              13
#define WB_TYPE_CURRENT              14
#define WB_TYPE_VALUE                15
#define WB_TYPE_ENERGY               16
#define WB_TYPE_RESISTANCE           17
#define WB_TYPE_CONCENTRATION        18
#define WB_TYPE_HEAT_ENERGY          19
#define WB_TYPE_WATER_FLOW           20
#define WB_TYPE_WATER_CONSUMPTION    21
#define WB_TYPE_WIND_SPEED           22
#define WB_TYPE_WIND_DIRECTION       23
#define WB_TYPE_NOISE                24
#define WB_TYPE_REACTIVE_POWER       25
#define WB_TYPE_PM25                 26
#define WB_TYPE_PM10                 27
#define WB_TYPE_PRECIPITATION_RATE   28

typedef struct {
    char id[32];
    char name[32];
    bool valid;
} wb_device_t;

/* 38 bytes: dev_idx(1) + name(28) + type_id(1) + units(6) + order(1) + flags(1) */
typedef struct {
    uint8_t dev_idx;   /* index into internal s_devices[] table */
    char    name[28];
    uint8_t type_id;   /* WB_TYPE_* */
    char    units[6];
    uint8_t order;
    uint8_t flags;     /* WB_FLAG_VALID | WB_FLAG_READONLY */
} wb_control_t;

void wb_driver_init(void);
void wb_driver_process(const char *topic, const char *payload, int len);
void wb_driver_subscribe(void);
void wb_driver_set_control(const char *device_id, const char *control_name, const char *value);

/* Discovery lifecycle */
void wb_driver_enable_discovery(void);
void wb_driver_disable_discovery(void);
void wb_driver_refresh(void);

/* true for Wiren Board built-in virtual devices (system, network, wb-gpio, ...) */
bool wb_driver_is_builtin(const char *dev_id);

/* Which WB devices discovery keeps. Matches the DISC_MODE_* values in app_config.h:
 *   0 = WB without built-ins (default), 1 = WB with built-ins, 2 = none (Zigbee2MQTT mode).
 * Persisted by app_config; applied here at boot and when the user changes it. */
void wb_driver_set_mode(uint8_t mode);
uint8_t wb_driver_get_mode(void);

/* true once any /devices/... topic was received (independent of the filter), so the UI
 * can tell whether the broker has Wiren Board topics at all. */
bool wb_driver_wb_present(void);

int                wb_driver_get_device_count(void);
const wb_device_t *wb_driver_get_device(int idx);
int                wb_driver_get_controls(const char *device_id, const wb_control_t **out, int max);
bool               wb_driver_has_controls(void);

const char *wb_control_type_name(const wb_control_t *ctrl);
bool        wb_control_is_writable(const wb_control_t *ctrl);

#endif /* WB_DRIVER_H */
