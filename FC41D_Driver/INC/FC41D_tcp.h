/*
 * File     : FC41D_tcp.h
 * Brief    : TCP client / server types, socket constants and bring-up steps.
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
 *  TCP client / server. The module runs in "buffer" access mode: a
 *  +QIURC: "recv",<id> says that data is waiting and AT+QIRD fetches it.
 *  Types only; the state machine prototypes are at the end of FC41D_core.h.
 */

#ifndef FC41D_DRIVER_INC_FC41D_TCP_H_
#define FC41D_DRIVER_INC_FC41D_TCP_H_

#include "FC41D_Driver/INC/FC41D_types.h"

#define FC41D_TCP_MAX_CLIENTS        (4U)      // accepted clients tracked at the same time
#define FC41D_TCP_CLIENT_SOCKET_ID   (0U)      // socket used for the client connection
#define FC41D_TCP_LISTENER_ID        (5U)      // socket used for the listener
#define FC41D_TCP_MAX_SOCKET_ID      (11U)     // the module has sockets 0..11
#define FC41D_TCP_READ_CHUNK         (128U)    // bytes asked from AT+QIRD at once
#define FC41D_TCP_POLL_MS            (1000U)   // open sockets are read now and then: a recv URC is never repeated
#define FC41D_TCP_RETRY_MS           (5000U)   // wait before the client connects again
#define FC41D_TCP_OPEN_TIMEOUT_MS    (30000UL) // +QIOPEN URC: ~1-5 s if it works, ~20 s if the server is unreachable
#define FC41D_TCP_LISTEN_TIMEOUT_MS  (6000UL)  // the listener answers within a moment

// Steps of fc41d_tcp_client_state_machine, top to bottom.
typedef enum {
    FC41D_CLIENT_QICLOSE_STALE = 0,  // AT+QICLOSE of sockets that may have died with the Wi-Fi
    FC41D_CLIENT_QICFG         = 1,  // AT+QICFG="datatype",0   text payload
    FC41D_CLIENT_QIOPEN        = 2,  // AT+QIOPEN=<id>,"TCP",<ip>,<port>,0,0
    FC41D_CLIENT_QIOPENWAIT    = 3,  // wait for +QIOPEN: <id>,0
    FC41D_QISWTMD              = 4,  // not used (transparent mode)
    FC41D_ATO                  = 5,  // not used
    FC41D_TRIPLE_PLUS          = 6,  // not used
    FC41D_CLIENT_QISEND        = 7,  // runtime: send data  (fc41d_tcp_send_state_machine)
    FC41D_CLIENT_QIRD          = 8,  // runtime: read data  (fc41d_tcp_read_state_machine)
    FC41D_CLIENT_QICLOSE       = 9   // not used
}fc41d_tcp_client_state_e;

// Steps of fc41d_tcp_server_state_machine, top to bottom.
typedef enum {
    FC41D_QICFG             = 0,  // AT+QICFG="accept/mode",0   accept clients automatically
    FC41D_QICFG_DATATYPE    = 1,  // AT+QICFG="datatype",0      text payload
    FC41D_SERVER_QIOPEN     = 2,  // AT+QIOPEN=<id>,"TCP LISTENER","",0,<port>,0
    FC41D_SERVER_QIOPENWAIT = 3,  // wait for +QIOPEN: <id>,0
    FC41D_QISTATE           = 4,  // not used
    FC41D_SERVER_QISEND     = 5,  // runtime: send data
    FC41D_SERVER_QIRD       = 6,  // runtime: read data
    FC41D_SERVER_QICLOSE    = 7   // not used
}fc41d_tcp_server_state_e;

typedef struct {
    uint8_t  ip_address[4];     // the server to connect to
    uint16_t port;
}fc41d_tcp_client_config_t;

typedef struct {
    uint16_t port;              // port to listen on
    uint8_t  max_connections;   // informational, the module decides
}fc41d_tcp_server_config_t;

// A client the listener accepted (+QIURC: "incoming",...).
typedef struct {
    uint16_t ip[4];
    uint16_t port;
    uint8_t  id;                // socket id, use it with the send function
    bool     in_use;
}fc41d_tcp_connected_device_list_t;

#endif /* FC41D_DRIVER_INC_FC41D_TCP_H_ */
