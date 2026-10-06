/*
 * File     : FC41D_api.h
 * Brief    : Public interface of the FC41D driver: settings table, initialize, 1 ms tick, polling, sending, receiving and run-time Wi-Fi / TCP changes.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-29
 *
 * Revisions
 *   R1  2026-09-29  First complete version
 *   R2  2026-10-05  Readability rewrite, simpler API (settings table, no text copy helpers, fc41d_format)
 *   R3  2026-10-05  fc41d_get_message, fc41d_customize_wifi_settings, fc41d_customize_tcp_settings; on_data callback removed
 *   R4  2026-10-05  Documented the recovery policy (independent channels, BLE priority, soft / hard reset)
 *   R5  2026-10-05  Doxygen briefs (parameters, return values) for the critical functions
 *   R6  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  The only header the rest of the firmware needs.
 *
 *  Settings: one constant table (it lives in flash, nothing to call to fill it)
 *  ------------------------------------------------------------------------
 *      static const fc41d_api_settings_t fc41d_settings = {
 *          .type = FC41D_BLE_GATT_WITH_WIFI_AP_AND_TCP_SERVER,
 *          .setup = {
 *              .ble_gatt_config   = { .device_name = "FC41D_Dev", .service_uuid = "FFF0", .characteristic_uuid = "FFF1" },
 *              .wi_fi_ap_config   = { .ssid = "FC41D_AP", .password = "12345678" },
 *              .tcp_server_config = { .port = 8080 },
 *              // .wi_fi_sta_config  = { .ssid = "my_network", .password = "my_password" },   (FC41D_..._WIFI_STA_... types)
 *              // .tcp_client_config = { .ip_address = {192, 168, 1, 10}, .port = 5000 },     (FC41D_..._TCP_CLIENT types)
 *              // .config            = { .is_dhcp_enabled = true },
 *          },
 *          // .enable_pin_high = my_en_high, .enable_pin_low = my_en_low,   // module enable pin: needed for the hard reset
 *      };
 *
 *  Use (after the SCI pins and baud rate are set up)
 *  -------------------------------------------------
 *      fc41d_api_initialize(&g_fc41d_api, SCIB_BASE, &fc41d_settings);   // once
 *      fc41d_api_1ms_timer(&g_fc41d_api);        // EVERY millisecond - without it nothing ever happens
 *      fc41d_api_polling(&g_fc41d_api);          // every pass of the main loop (at least once per ms)
 *
 *      // Everything that arrives, from BLE or Wi-Fi, comes out of one function:
 *      char text[FC41D_MESSAGE_MAX];
 *      uint16_t length;
 *      switch(fc41d_get_message(&g_fc41d_api, text, &length)) {
 *          case FC41D_MESSAGE_BLE:        ...  break;     // text[0 .. length-1] (and a NUL after it)
 *          case FC41D_MESSAGE_TCP_CLIENT: ...  break;
 *          case FC41D_MESSAGE_TCP_SERVER: ...  break;     // g_fc41d_api.message_socket_id = which client
 *          default:                            break;     // FC41D_MESSAGE_NONE: nothing arrived
 *      }
 *
 *      if(fc41d_api_is_ready(&g_fc41d_api)) { fc41d_api_tcp_send(...); fc41d_api_ble_send(...); }
 *
 *  Changing the Wi-Fi / TCP values while the program runs
 *  ------------------------------------------------------
 *      fc41d_customize_wifi_settings(&g_fc41d_api, "new_network", "new_password");
 *      fc41d_customize_tcp_settings (&g_fc41d_api, ip, 5000);      // ip = uint8_t[4], ignored by a TCP server
 *  Only the Wi-Fi / the socket is set up again; a connected BLE phone stays connected.
 *  fc41d_api_is_ready is false until the new link is up. Call them from the main loop.
 *
 *  Connection loss and recovery
 *  ----------------------------
 *  Every channel stands on its own: while the Wi-Fi / TCP side is down BLE keeps sending
 *  and receiving, and the other way round (fc41d_api_ble_send / fc41d_api_tcp_send look at
 *  their own channel only; fc41d_api_is_ready means "everything is up").
 *  A lost Wi-Fi link, TCP socket or BLE advertising is set up again by itself, one at a time.
 *  When a feature gives up for good, or the module stops answering (a heartbeat "AT" is
 *  sent after 10 s of silence), the module is reset: AT+QRST first (soft), plus the enable
 *  pin (hard) when the module did not answer or the soft reset did not help.
 *  BLE has priority: while a phone is connected and has talked to us in the last
 *  FC41D_BLE_ACTIVE_MS (it wrote, took a notification, or just connected) the module is
 *  NEVER reset. Only the failed Wi-Fi / TCP feature starts again on its own, every
 *  FC41D_FEATURE_RETRY_MS. g_fc41d_api.core.diag counts resets_blocked and feature_retries.
 *
 *  Polling speed: the UART receive FIFO holds 16 bytes = 1.4 ms at 115200 baud and
 *  a byte that arrives into a full FIFO is lost for good. If the main loop cannot
 *  promise one pass per millisecond, define FC41D_RX_FROM_ISR in the project and
 *  call fc41d_api_rx_service() from a 1 ms (or faster) interrupt; polling can then
 *  be as slow as it likes.
 *
 *  Look at g_fc41d_api.core.diag in a CCS watch window: tx_log / rx_log are the
 *  last text sent to and received from the module, tick_missing says that
 *  fc41d_api_1ms_timer is never called, last_fault_state / faults say which part
 *  of the bring-up keeps failing.
 *
 *  The instance (queues, buffers, about 1.8 k words) is placed in the
 *  "debug_data" section (RAMLS0D) like g_ui, because RAMM1D has no room for it.
 *
 *  Data is text. BLE: no CR/LF. TCP: no CR/LF and no double quote (the text is
 *  written between quotes into the AT+QISEND command).
 */

#ifndef FC41D_DRIVER_FC41D_API_H_
#define FC41D_DRIVER_FC41D_API_H_

#include "FC41D_Driver/INC/FC41D_core.h"

#define FC41D_API_TCP_ALL_CLIENTS (0xFFFFU)     // socket id for fc41d_api_tcp_send: every accepted client
#define FC41D_MESSAGE_MAX         (FC41D_LINE_MAX)   // size of the buffer fc41d_get_message needs (text + NUL)

typedef enum {
    FC41D_API_OK                       = 0,
    FC41D_API_ERROR                    = 1,
    FC41D_API_NULL_POINTER             = 2,
    FC41D_API_INVALID_CALLBACK_POINTER = 3,
    FC41D_API_INVALID_VALUE            = 4,     // empty / too long name, bad address, port 0 ...
    FC41D_API_NOT_SUPPORTED            = 5      // the selected communication type has no Wi-Fi / TCP part to change
}fc41d_api_return_e;

// Where a received message came from.
typedef enum {
    FC41D_MESSAGE_NONE       = 0,   // nothing was received
    FC41D_MESSAGE_BLE        = 1,   // written by the phone to the GATT characteristic
    FC41D_MESSAGE_TCP_CLIENT = 2,   // sent by the server we are connected to
    FC41D_MESSAGE_TCP_SERVER = 3    // sent by one of the clients of our server (see message_socket_id)
}fc41d_message_source_e;

typedef struct {
    fc41d_core_t core;
    uint16_t     message_socket_id;     // TCP: socket the last message came from (the id for fc41d_api_tcp_send)
}fc41d_api_t;

typedef struct {
    fc41d_communication_type_e     type;        // which features to bring up
    fc41d_core_connection_config_t setup;       // credentials, ports, uuids, options
    void (*enable_pin_high)(void);              // module enable pin (optional, NULL = always powered)
    void (*enable_pin_low)(void);               // (optional)
}fc41d_api_settings_t;

/** The driver instance (defined in FC41D_api.c). */
extern fc41d_api_t g_fc41d_api;

/**
 * @brief  Starts the driver: clears the instance, copies the settings and begins the module
 *         bring-up. Nothing blocks, the state machine does the rest inside polling.
 * @param  self      Driver instance, normally &g_fc41d_api.
 * @param  sci_base  Base address of the SCI (e.g. SCIB_BASE), already set to 115200 8N1.
 * @param  settings  Settings table. It is copied, so it may live in flash.
 * @return FC41D_API_OK, FC41D_API_NULL_POINTER, FC41D_API_ERROR (sci_base is 0) or
 *         FC41D_API_INVALID_CALLBACK_POINTER.
 */
fc41d_api_return_e fc41d_api_initialize(fc41d_api_t *self, uint32_t sci_base, const fc41d_api_settings_t *settings);

/**
 * @brief  Millisecond tick of the driver. Call it every 1 ms; it only counts, so a timer
 *         interrupt is fine. Without it nothing ever happens.
 * @param  self  Driver instance.
 * @return FC41D_API_OK or FC41D_API_NULL_POINTER.
 */
fc41d_api_return_e fc41d_api_1ms_timer(fc41d_api_t *self);

/**
 * @brief  Runs the driver: UART receive, parser and state machines. Call it on every pass
 *         of the main loop, at least once per millisecond.
 * @param  self  Driver instance.
 * @return FC41D_API_OK or FC41D_API_NULL_POINTER.
 */
fc41d_api_return_e fc41d_api_polling(fc41d_api_t *self);

/**
 * @brief  Is every channel of the selected communication type up (BLE, Wi-Fi, TCP)?
 *         The send functions look at their own channel only.
 * @param  self  Driver instance.
 * @return true when everything is up.
 */
bool fc41d_api_is_ready(const fc41d_api_t *self);

/**
 * @brief  Queues text for the BLE central (sent as notifications, one frame per AT command).
 * @param  self    Driver instance.
 * @param  data    Text to send, no CR / LF.
 * @param  length  Number of characters.
 * @return Bytes queued. 0 = refused (BLE is down, bad characters or the queue is full);
 *         less than length = the queue (about 190 bytes) had no room for the rest.
 */
uint16_t fc41d_api_ble_send(fc41d_api_t *self, const char *data, uint16_t length);

/**
 * @brief  Queues text for a TCP socket.
 * @param  self       Driver instance.
 * @param  socket_id  Socket to send to (g_fc41d_api.message_socket_id answers the last sender)
 *                    or FC41D_API_TCP_ALL_CLIENTS for every connected client.
 * @param  data       Text to send, no CR / LF and no double quote.
 * @param  length     Number of characters.
 * @return Bytes queued. 0 = refused (TCP is down, bad characters or the queue is full).
 */
uint16_t fc41d_api_tcp_send(fc41d_api_t *self, uint16_t socket_id, const char *data, uint16_t length);

/**
 * @brief  Takes the oldest received message (BLE or Wi-Fi). TCP is a byte stream: pieces of
 *         one sender that wait behind each other are joined, so end your messages with a
 *         character of your own if you need the borders.
 * @param  self    Driver instance.
 * @param  buffer  Receives the text, NUL terminated. Must hold FC41D_MESSAGE_MAX characters.
 * @param  length  Receives the number of characters (0 when there is no message).
 * @return Where the message came from, FC41D_MESSAGE_NONE when nothing was received.
 *         For TCP, g_fc41d_api.message_socket_id is the socket it came from.
 */
fc41d_message_source_e fc41d_get_message(fc41d_api_t *self, char *buffer, uint16_t *length);

/**
 * @brief  Changes the Wi-Fi name and password while the program runs. The station joins the
 *         new network (FC41D_WIFI_STA types) or the access point is renamed (FC41D_WIFI_AP
 *         types). Only the Wi-Fi is set up again, a connected BLE phone stays connected.
 * @param  self      Driver instance.
 * @param  ssid      New network name, not empty.
 * @param  password  New password, "" or NULL = open network. 8..63 characters for an access point.
 * @return FC41D_API_OK (also when nothing changes), FC41D_API_NULL_POINTER,
 *         FC41D_API_INVALID_VALUE or FC41D_API_NOT_SUPPORTED (the type has no Wi-Fi).
 */
fc41d_api_return_e fc41d_customize_wifi_settings(fc41d_api_t *self, const char *ssid, const char *password);

/**
 * @brief  Changes the TCP target (client) or the listening port (server) while the program
 *         runs. Clients that already connected to our server stay connected.
 * @param  self        Driver instance.
 * @param  ip_address  Client: server address, uint8_t[4]. Server: ignored, may be NULL.
 * @param  port        Server port (client) or port to listen on (server), not 0.
 * @return FC41D_API_OK (also when nothing changes), FC41D_API_NULL_POINTER,
 *         FC41D_API_INVALID_VALUE or FC41D_API_NOT_SUPPORTED (the type has no TCP).
 */
fc41d_api_return_e fc41d_customize_tcp_settings(fc41d_api_t *self, const uint8_t *ip_address, uint16_t port);

#ifdef FC41D_RX_FROM_ISR
/**
 * @brief  Moves the UART FIFO into the driver. Only with FC41D_RX_FROM_ISR: call it from an
 *         interrupt every 1 ms or faster (see "Polling speed" above).
 * @param  self  Driver instance.
 * @return FC41D_API_OK or FC41D_API_NULL_POINTER.
 */
fc41d_api_return_e fc41d_api_rx_service(fc41d_api_t *self);
#endif

#endif /* FC41D_DRIVER_FC41D_API_H_ */
