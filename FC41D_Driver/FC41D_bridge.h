/*
 * File     : FC41D_bridge.h
 * Brief    : BLE <-> TCP test bridge: data type for the CCS watch window and the single call that forwards messages.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-10-05
 *
 * Revisions
 *   R1  2026-10-05  First version
 *   R2  2026-10-05  Doxygen briefs (parameters, return values) for the critical functions
 *   R3  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  Test application: whatever a phone writes over BLE goes to Wi-Fi, and whatever
 *  arrives over Wi-Fi goes to the phone. One call per main loop pass:
 *
 *      fc41d_api_polling(&g_fc41d_api);
 *      fc41d_bridge_run(&g_fc41d_api);
 *
 *  What is forwarded gets a tag in front so the receiver sees where it came from:
 *      BLE  -> TCP   "BLE: <text>"        to every connected client (TCP server)
 *                                         or to the server we are connected to (TCP client)
 *      TCP  -> BLE   "TCP8: <text>"       the number is the socket it came from
 *
 *  Watch g_fc41d_bridge in a CCS expression window: out is the last forwarded line,
 *  the counters tell what happened to the rest.
 */

#ifndef FC41D_DRIVER_FC41D_BRIDGE_H_
#define FC41D_DRIVER_FC41D_BRIDGE_H_

#include "FC41D_Driver/FC41D_api.h"

typedef struct {
    char                   message[FC41D_MESSAGE_MAX];        // scratch: fc41d_get_message writes here (emptied when nothing arrives)
    char                   out[FC41D_MESSAGE_MAX + 12U];      // the last line that was forwarded, with its tag
    fc41d_message_source_e source;                            // where the last message came from (1 BLE, 2 TCP client, 3 TCP server)
    uint16_t               socket;                            // TCP: the socket it came from
    uint32_t               received;                          // messages taken out of the driver
    uint32_t               ble_to_tcp;                        // forwarded BLE -> Wi-Fi
    uint32_t               tcp_to_ble;                        // forwarded Wi-Fi -> BLE
    uint32_t               refused;                           // the other side is not connected / the driver queue is full: lost
    uint32_t               ignored;                           // nothing left after the line end was removed
}fc41d_bridge_t;

extern fc41d_bridge_t g_fc41d_bridge;

/**
 * @brief Takes at most one received message and forwards it to the other side (BLE -> TCP or
 *        TCP -> BLE) with a source tag. Call it on every pass of the main loop.
 * @param api  Driver instance, normally &g_fc41d_api.
 */
void fc41d_bridge_run(fc41d_api_t *api);

#endif /* FC41D_DRIVER_FC41D_BRIDGE_H_ */
