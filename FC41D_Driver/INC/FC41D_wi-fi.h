/*
 * File     : FC41D_wi-fi.h
 * Brief    : Wi-Fi station and SoftAP types and bring-up steps.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-29
 *
 * Revisions
 *   R1  2026-09-29  First complete version
 *   R2  2026-10-05  Readability rewrite, simpler API (settings table, no text copy helpers, fc41d_format)
 *   R3  2026-10-05  AT+QSOFTAPSTOP step for run-time AP changes
 *   R4  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  Wi-Fi station (join a network) and SoftAP (be a network). Types only; the
 *  state machine prototypes are at the end of FC41D_core.h.
 */

#ifndef FC41D_DRIVER_INC_FC41D_WI_FI_H_
#define FC41D_DRIVER_INC_FC41D_WI_FI_H_

#include "FC41D_Driver/INC/FC41D_types.h"

#define FC41D_WIFI_CONNECT_TIMEOUT_MS (20000UL) // no GOT_IP after this long = wrong password / no network
#define FC41D_WIFI_CONNECT_ATTEMPTS   (3U)      // connect attempts before the module is power cycled

// Steps of fc41d_wifi_sta_state_machine, top to bottom.
typedef enum {
    FC41D_QWSCAN        = 0,   // AT+QWSCAN (a scan is not needed to connect: skipped)
    FC41D_QSTADHCP      = 1,   // AT+QSTADHCP=0|1
    FC41D_QSTASTATIC    = 2,   // AT+QSTASTATIC=ip,mask,gw,dns   (only without DHCP)
    FC41D_QSTAAPINFO    = 3,   // AT+QSTAAPINFO=ssid,pwd
    FC41D_QSTAWAIT      = 4,   // wait for +QSTASTAT:GOT_IP (WLAN_CONNECTED with a static IP)
    FC41D_QSTASTOP      = 5,   // AT+QSTASTOP, before a new attempt (also after the credentials changed at run time)
    FC41D_QGETWIFISTATE = 6,   // not used
    FC41D_QGETIP_STA    = 7    // not used (AT+QGETIP never answers OK)
}fc41d_wi_fi_sta_state_e;

// Steps of fc41d_wifi_ap_state_machine, top to bottom.
typedef enum {
    FC41D_QSOFTAP     = 0,     // AT+QSOFTAP=ssid[,key]
    FC41D_QAPSTATIC   = 1,     // AT+QAPSTATIC=ip,mask,gw,dns    (optional)
    FC41D_QAPGETLIST  = 2,     // not used
    FC41D_QSOFTAPSTOP = 3,     // AT+QSOFTAPSTOP, before AT+QSOFTAP when the name / password changed at run time
    FC41D_QGETIP_AP   = 4      // not used
}fc41d_wi_fi_ap_state_e;

typedef struct {
    uint8_t  ip_address[4];
    uint8_t  subnet_mask[4];
    uint8_t  gateway[4];
    uint8_t  dns_server[4];
}fc41d_wi_fi_static_ip_config_t;

// Plain text. An empty password means an open network.
typedef struct {
    char  ssid[32];
    char  password[64];
}fc41d_wi_fi_sta_config_t;

typedef struct {
    char  ssid[32];
    char  password[64];                     // 8..63 characters, empty = open network
    bool  use_static_ip;                    // false = the module's own 192.168.19.1
    fc41d_wi_fi_static_ip_config_t static_ip;
}fc41d_wi_fi_ap_config_t;

typedef struct {
    char    device_name[32];
    uint8_t id;
}fc41d_wi_fi_connected_device_list_t;

#endif /* FC41D_DRIVER_INC_FC41D_WI_FI_H_ */
