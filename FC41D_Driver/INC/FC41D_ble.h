/*
 * File     : FC41D_ble.h
 * Brief    : BLE GATT server types and bring-up steps.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-29
 *
 * Revisions
 *   R1  2026-09-29  First complete version
 *   R2  2026-10-05  Readability rewrite, simpler API (settings table, no text copy helpers, fc41d_format)
 *   R3  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  BLE GATT server (the module is the peripheral, a phone is the central).
 *  Types only; the state machine prototypes are at the end of FC41D_core.h.
 */

#ifndef FC41D_DRIVER_INC_FC41D_BLE_H_
#define FC41D_DRIVER_INC_FC41D_BLE_H_

#include "FC41D_Driver/INC/FC41D_types.h"

#define FC41D_BLE_DEFAULT_MTU  (23U)   // until the module reports +QBLEMTU:<n>
#define FC41D_BLE_ATT_OVERHEAD (3U)    // one notification carries at most MTU - 3 bytes

// Steps of fc41d_ble_gatt_state_machine, top to bottom.
typedef enum {
    FC41D_QBLEDEINIT    = 0,   // AT+QBLEDEINIT             clean start (ERROR on a fresh module is fine)
    FC41D_QBLEINIT      = 1,   // AT+QBLEINIT=2             peripheral role
    FC41D_QBLENAME      = 2,   // AT+QBLENAME=<name>        the name a phone shows
    FC41D_QBLEGATTSSRV  = 3,   // AT+QBLEGATTSSRV=<uuid>    the service
    FC41D_QBLEGATTSCHAR = 4,   // AT+QBLEGATTSCHAR=<uuid>,FF   the characteristic (write + notify)
    FC41D_QBLEADVSTART  = 5,   // AT+QBLEADVSTART           become visible
    FC41D_QBLEGATTSNTFY = 6,   // runtime: send data to the phone (fc41d_ble_send_state_machine)
    FC41D_QBLEADVSTOP   = 7    // not used
}fc41d_ble_gatt_state_e;

// Plain text. The UUIDs are the 16 bit short form as hex text: "FFF0", "FFF1".
typedef struct {
    char  device_name[32];
    char  service_uuid[16];
    char  characteristic_uuid[16];
}fc41d_ble_gatt_config_t;

typedef struct {
    char    device_name[32];
    uint8_t id;
}fc41d_ble_connected_device_list_t;

#endif /* FC41D_DRIVER_INC_FC41D_BLE_H_ */
