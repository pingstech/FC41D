/*
 * File     : FC41D_config.h
 * Brief    : Module bring-up types and steps (power, wake-up check, reset, identity, stored defaults).
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
 *  Module bring-up: power, wake-up check, reset, identity, stored defaults.
 *  Types only; the state machine prototype is at the end of FC41D_core.h.
 */

#ifndef FC41D_DRIVER_INC_FC41D_CONFIG_H_
#define FC41D_DRIVER_INC_FC41D_CONFIG_H_

#include "FC41D_Driver/INC/FC41D_types.h"

// The bring-up runs these steps from top to bottom.
typedef enum {
    FC41D_POWER_ON      = 0,   // IO pin HIGH, wait until the module has booted
    FC41D_AT            = 1,   // AT                         is the module alive?
    FC41D_QRST          = 2,   // AT+QRST                    start from a known state ("ready")
    FC41D_QVERSION      = 3,   // AT+QVERSION                read the firmware version
    FC41D_QWLMAC        = 4,   // AT+QWLMAC                  read the MAC address
    FC41D_QECHO         = 5,   // AT+QECHO=0|1
    FC41D_QSTAAPINFODEF = 6,   // AT+QSTAAPINFODEF=ssid,pwd  module reconnects by itself after a reset
    FC41D_QSTADHCPDEF   = 7    // AT+QSTADHCPDEF=0|1
    // AT+QSETBAND is not implemented: it changes the module baud rate at once and
    // the firmware A16M02 behaviour was never tested.
}fc41d_config_state_e;

typedef struct {
    fc41d_config_state_e state;             // current step
    uint32_t             baudrate;          // reserved (the module stays at 115200)
    uint16_t             mac_address[6];    // filled from AT+QWLMAC
    bool                 is_echo_enabled;
    bool                 is_dhcp_enabled;   // station side: true = DHCP, false = static_ip_config
    bool                 auto_connect_to_ap;// store the station credentials in the module's flash
}fc41d_config_t;

#endif /* FC41D_DRIVER_INC_FC41D_CONFIG_H_ */
