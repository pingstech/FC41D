/*
 * File     : FC41D_core.h
 * Brief    : Driver core: driver object, states, queues, command engine, recovery policy constants and the prototypes of every internal module.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-30
 *
 * Revisions
 *   R1  2026-09-30  First complete version
 *   R2  2026-10-05  Readability rewrite, simpler API (settings table, no text copy helpers, fc41d_format)
 *   R3  2026-10-05  Message queue, run-time change events, BLE + Wi-Fi AP + TCP server type
 *   R4  2026-10-05  Recovery: BLE priority window, heartbeat ping job, feature retry, soft / hard reset counters
 *   R5  2026-10-05  Doxygen briefs (parameters, return values) for the critical functions
 *   R6  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  Layers (a header only includes the ones below it):
 *
 *      FC41D_api  ->  FC41D_core  ->  config / wi-fi / tcp / ble / parser  ->  UART port
 *
 *  The leaf headers (config, wi-fi, tcp, ble, parser) hold types only. Every
 *  function that needs the whole driver object is declared at the end of this
 *  file, so no header includes another one in a circle.
 */

#ifndef FC41D_DRIVER_INC_FC41D_CORE_H_
#define FC41D_DRIVER_INC_FC41D_CORE_H_

#include "FC41D_Driver/INC/FC41D_types.h"
#include "FC41D_Driver/INC/FC41D_config.h"
#include "FC41D_Driver/INC/FC41D_wi-fi.h"
#include "FC41D_Driver/INC/FC41D_tcp.h"
#include "FC41D_Driver/INC/FC41D_ble.h"
#include "FC41D_Driver/INC/FC41D_parser.h"
#include "FC41D_Driver/INC/HOL_Queue.h"

// Byte queue (a char is 16 bit on the C28x): UART receive path and application transmit path.
DECLARE_QUEUE(char, 256)

typedef queue_char_256_t fc41d_core_queue_t;

#define fc41d_queue_initialize     queue_initialize_char_256
#define fc41d_queue_push           queue_push_char_256   // when full the oldest byte is overwritten
#define fc41d_queue_pull           queue_pull_char_256
#define fc41d_queue_peek           queue_peek_char_256
#define fc41d_queue_pull_multiple  queue_pull_multiple_char_256
#define fc41d_queue_count          queue_count_char_256
#define fc41d_queue_space          queue_available_space_char_256
#define fc41d_queue_clear          queue_clear_char_256

#define FC41D_TX_CHUNK            (64U)     // application bytes per AT+QISEND / AT+QBLEGATTSNTFY
#define FC41D_COMMAND_MAX         (224U)    // longest AT command line
#define FC41D_TX_STALL_MS         (500U)    // the UART took no byte of a command for this long = error
#define FC41D_POWER_ON_WAIT_MS    (1500U)   // module boot time after the IO pin goes HIGH
#define FC41D_FAULT_OFF_MS        (1000U)   // power off time while recovering from a fault
#define FC41D_FAULT_BACKOFF_MS    (10000U)  // longer pause after FC41D_FAULT_BACKOFF_AFTER faults in a row
#define FC41D_FAULT_BACKOFF_AFTER (3U)
#define FC41D_SOFT_RESET_GAP_MS   (250U)    // pause before a soft reset (AT+QRST) starts the bring-up again
#define FC41D_TICK_CHECK_POLLS    (5000UL)  // this many polls without a single 1 ms tick = diag.tick_missing

// BLE comes first. While a phone is connected AND has talked to us (it wrote, or took a
// notification, or just connected) within this time, the module is never reset: only the
// feature that failed (Wi-Fi, a socket) is tried again on its own.
#define FC41D_BLE_ACTIVE_MS       (60000UL)
#define FC41D_FEATURE_RETRY_MS    (10000U)  // pause before a failed Wi-Fi / TCP feature is set up again (BLE in use)

// A module that says nothing for FC41D_HEARTBEAT_MS gets an "AT". No answer to
// FC41D_PING_FAIL_LIMIT of them in a row = the module hangs: reset it (unless BLE is in use).
#define FC41D_HEARTBEAT_MS        (10000U)
#define FC41D_PING_FAIL_LIMIT     (2U)

#define FC41D_ELAPSED_MS(self, since) ((uint32_t)((self)->timer.task - (uint32_t)(since)))

typedef enum {
    FC41D_ERROR                    = 0,
    FC41D_OK                       = 1,
    FC41D_NULL_POINTER             = 2,
    FC41D_IN_PROGRESS              = 3,
    FC41D_HARD_FAIL                = 4,
    FC41D_READY                    = 5
}fc41d_return_e;

typedef enum {
    FC41D_IDLE       = 0,
    FC41D_CONFIG     = 1,
    FC41D_WIFI_STA   = 2,
    FC41D_WIFI_AP    = 3,
    FC41D_TCP_CLIENT = 4,
    FC41D_TCP_SERVER = 5,
    FC41D_BLE        = 6,

    FC41D_DELAY      = 99
}fc41d_core_state_e;

typedef enum {
    FC41D_COMMAND_TRANSMIT     = 0,
    FC41D_WAITING_FOR_RESPONSE = 1,
    FC41D_RESPONSE_RECEIVED    = 2,
    FC41D_COMMAND_ERROR        = 3
}fc41d_command_handler_state_e;

// The feature bits are also the priority: the lowest bit is started first (ble > wifi > tcp).
// A communication type is simply the OR of the features it contains.
#define FEAT_BLE        (1U << 0)
#define FEAT_WIFI_STA   (1U << 1)
#define FEAT_WIFI_AP    (1U << 2)
#define FEAT_TCP_CLIENT (1U << 3)
#define FEAT_TCP_SERVER (1U << 4)

typedef enum {
    FC41D_BLE_GATT_ONLY                         = FEAT_BLE,
    FC41D_WIFI_STA_AND_TCP_CLIENT              = FEAT_WIFI_STA | FEAT_TCP_CLIENT,
    FC41D_WIFI_STA_AND_TCP_SERVER               = FEAT_WIFI_STA | FEAT_TCP_SERVER,
    FC41D_WIFI_AP_AND_TCP_CLIENT                = FEAT_WIFI_AP  | FEAT_TCP_CLIENT,
    FC41D_WIFI_AP_AND_TCP_SERVER                = FEAT_WIFI_AP  | FEAT_TCP_SERVER,
    FC41D_BLE_GATT_WITH_WIFI_STA_AND_TCP_CLIENT = FEAT_BLE | FEAT_WIFI_STA | FEAT_TCP_CLIENT,
    FC41D_BLE_GATT_WITH_WIFI_STA_AND_TCP_SERVER = FEAT_BLE | FEAT_WIFI_STA | FEAT_TCP_SERVER,
    FC41D_BLE_GATT_WITH_WIFI_AP_AND_TCP_CLIENT  = FEAT_BLE | FEAT_WIFI_AP  | FEAT_TCP_CLIENT,
    FC41D_BLE_GATT_WITH_WIFI_AP_AND_TCP_SERVER  = FEAT_BLE | FEAT_WIFI_AP  | FEAT_TCP_SERVER
} fc41d_communication_type_e;

// What the idle state is busy with besides waiting.
typedef enum {
    FC41D_JOB_NONE = 0,
    FC41D_JOB_RX   = 1,    // reading a TCP socket (AT+QIRD)
    FC41D_JOB_TX   = 2,    // sending one application frame
    FC41D_JOB_PING = 3     // "AT": is the module still alive?
}fc41d_job_e;

// How long a command may take and how often it is tried.
typedef struct {
    uint16_t timeout_ms;
    uint16_t tries;
}fc41d_wait_t;

#define FC41D_WAIT_BOOT     ((fc41d_wait_t){  500U, 10U })   // first AT: the module may still be booting
#define FC41D_WAIT_RESET    ((fc41d_wait_t){ 6000U,  3U })   // AT+QRST: OK at once, "ready" about a second later
#define FC41D_WAIT_QUICK    ((fc41d_wait_t){ 2000U,  2U })   // ordinary commands
#define FC41D_WAIT_SLOW     ((fc41d_wait_t){ 3000U,  2U })   // BLE and TCP commands
#define FC41D_WAIT_ONCE     ((fc41d_wait_t){ 3000U,  1U })   // a single attempt
#define FC41D_WAIT_AP_START ((fc41d_wait_t){ 8000U,  2U })   // AT+QSOFTAP takes about 4 s

typedef struct {
    void     (*fc41d_set_IO_pin_HIGH_function)   (void);   // module enable pin (optional)
    void     (*fc41d_set_IO_pin_LOW_function)    (void);   // (optional)
    uint16_t (*fc41d_hardware_transmit_function) (const char *data, uint16_t length);   // returns the bytes taken
    uint16_t (*fc41d_hardware_receive_function)  (char *data, uint16_t max_length);     // returns the bytes copied
}fc41d_hardware_callback_t;

typedef struct {
    fc41d_config_t                 config;
    fc41d_wi_fi_static_ip_config_t static_ip_config;
    fc41d_wi_fi_sta_config_t       wi_fi_sta_config;
    fc41d_wi_fi_ap_config_t        wi_fi_ap_config;
    fc41d_tcp_client_config_t      tcp_client_config;
    fc41d_tcp_server_config_t      tcp_server_config;
    fc41d_ble_gatt_config_t        ble_gatt_config;
}fc41d_core_connection_config_t;

typedef struct {
    fc41d_command_handler_state_e state;
    bool                          started;      // the current attempt was begun
    uint16_t                      try_count;
    uint16_t                      sent;         // command bytes the UART already took
    uint16_t                      length;
    uint32_t                      start_tick;   // reference for the stall / response timeout
}fc41d_command_handler_t;

typedef struct {
    volatile uint32_t task;     // millisecond counter, written ONLY by fc41d_core_1ms_timer
    uint32_t          delay_start;
    uint32_t          delay_ms;
}fc41d_core_timer_t;

// Which features are up. Each sub-module sets its own flag when it is finished;
// the core clears them again when the link is lost.
typedef struct {
    bool wifi_sta;
    bool wifi_ap;
    bool tcp_client;
    bool tcp_server;
    bool ble_gatt;
    fc41d_communication_type_e type;
}fc41d_communication_type_t;

// Everything the parser learns from URCs. Members marked "event" are raised by
// the parser, handled once by the core and cleared.
typedef struct {
    bool     sta_associated;     // +QSTASTAT:WLAN_CONNECTED
    bool     sta_got_ip;         // +QSTASTAT:GOT_IP
    bool     sta_configured;     // AT+QSTAAPINFO was accepted (the module reconnects by itself)
    bool     sta_ever_up;        // the link has worked at least once
    bool     sta_link_lost;      // event
    bool     ap_up;
    bool     ble_connected;
    uint32_t ble_activity_tick;  // last time the phone connected, wrote or took a notification
    bool     ble_reinit;         // event: the module fell back to NOINIT
    bool     module_rebooted;    // event: "ready" although the module was configured
    bool     wifi_changed;       // event: fc41d_customize_wifi_settings stored new values
    bool     tcp_changed;        // event: fc41d_customize_tcp_settings stored new values
    bool     listener_open;
    uint16_t ble_mtu;
    uint16_t tcp_open_mask;      // sockets that can carry data (bit = socket id)
    uint16_t tcp_rx_mask;        // sockets with data waiting
    uint16_t tcp_closed_mask;    // event: +QIURC: "closed"
    uint16_t tcp_stale_mask;     // sockets that may be zombies after a Wi-Fi loss
    uint32_t tcp_poll_tick;
    fc41d_tcp_connected_device_list_t clients[FC41D_TCP_MAX_CLIENTS];
}fc41d_net_status_t;

// The current step of every sub-module.
typedef struct {
    fc41d_wi_fi_sta_state_e  wifi_sta;
    fc41d_wi_fi_ap_state_e   wifi_ap;
    fc41d_tcp_client_state_e tcp_client;
    fc41d_tcp_server_state_e tcp_server;
    fc41d_ble_gatt_state_e   ble;
    uint32_t                 wait_start;   // when the wait-step that is running began
    uint16_t                 attempts;
}fc41d_steps_t;

// One application frame taken from the transmit queue.
typedef struct {
    bool     active;
    uint16_t destination;       // fc41d_data_source_e
    uint16_t id;
    uint16_t length;
    char     data[FC41D_TX_CHUNK + 1U];     // text, always NUL terminated
}fc41d_tx_job_t;

// Everything worth watching in a CCS watch window.
typedef struct {
    char     tx_log[FC41D_LOG_SIZE];     // the last text sent to the module (wraps around)
    char     rx_log[FC41D_LOG_SIZE];     // the last text received from the module
    uint16_t tx_log_pos;
    uint16_t rx_log_pos;
    uint32_t polls;                      // calls of fc41d_api_polling
    bool     tick_missing;               // true = fc41d_api_1ms_timer is never called
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_data_bytes;              // application payload put into the message queue
    uint32_t tx_data_bytes;              // application payload sent
    uint16_t rx_dropped;                 // messages lost because the message queue was full (nobody read it)
    uint16_t rx_overflow;
    uint16_t lines;
    uint16_t unknown_lines;
    uint16_t cmd_ok;
    uint16_t cmd_timeouts;
    uint16_t cmd_errors;
    uint16_t cmd_hard_fails;
    uint16_t tx_dropped;
    uint16_t tx_rejected;
    uint16_t link_losses;
    uint16_t reboots;
    uint16_t tcp_open_failures;
    uint16_t faults;                     // times the module was reset because something gave up / hung
    uint16_t soft_resets;                // ... by AT+QRST at the start of the bring-up only
    uint16_t hard_resets;                // ... with the enable pin as well
    uint16_t resets_blocked;             // resets that BLE traffic prevented
    uint16_t feature_retries;            // Wi-Fi / TCP set up again on its own (BLE in use)
    uint16_t pings;                      // heartbeat "AT" commands sent
    uint16_t consecutive_faults;
    uint16_t last_fault_state;           // the core state that gave up last
}fc41d_diag_t;

typedef struct {
    bool                           is_module_ready;         // the bring-up sequence is finished
    fc41d_return_e                 communication_progress;  // FC41D_READY once every wanted feature is up
    fc41d_core_state_e             state;
    fc41d_core_state_e             next_state;
    fc41d_core_timer_t             timer;
    fc41d_core_connection_config_t setup;
    fc41d_command_handler_t        command_handler;
    fc41d_hardware_callback_t      callback;
    fc41d_core_queue_t             receive_queue;
    fc41d_core_queue_t             transmit_queue;
    fc41d_core_queue_t             message_queue;           // received BLE / TCP text, frames of [source][id][length][text ...]
    fc41d_communication_type_t     communication_type;
    fc41d_parser_t                 parser;
    fc41d_net_status_t             net;
    fc41d_steps_t                  step;
    fc41d_tx_job_t                 tx_job;
    fc41d_job_e                    job;
    fc41d_job_e                    last_job;
    bool                           background_pause;        // a job just ended: give the feature that is being set up one turn
    uint16_t                       ping_failures;
    uint32_t                       ping_tick;
    bool                           defaults_written;       // the DEF commands (module flash) ran since the MCU started
    char                           command_buffer[FC41D_COMMAND_MAX];
    fc41d_diag_t                   diag;
}fc41d_core_t;

/* ----------------------------------------------------------------------- */
/*  Core                                                                     */
/* ----------------------------------------------------------------------- */
/**
 * @brief  Prepares the core. The callbacks and the setup must be filled in before.
 * @param  self  Driver core.
 * @return FC41D_OK, or FC41D_NULL_POINTER when a UART callback is missing.
 */
fc41d_return_e fc41d_core_initialize(fc41d_core_t *self);

/** @brief Every 1 ms. It only counts, so a timer interrupt is fine. */
fc41d_return_e fc41d_core_1ms_timer(fc41d_core_t *self);

/**
 * @brief  UART receive, parser and state machines. From the main loop, at least once per ms.
 * @param  self  Driver core.
 * @return FC41D_OK or FC41D_NULL_POINTER.
 */
fc41d_return_e fc41d_core_polling(fc41d_core_t *self);

// Moves the UART FIFO into the receive queue. Polling does it by itself unless
// FC41D_RX_FROM_ISR is defined; then call this from an interrupt instead.
void           fc41d_core_rx_service(fc41d_core_t *self);

// Puts received bytes into the receive queue (public for tests). Returns the bytes taken.
uint16_t       fc41d_core_receive_data(fc41d_core_t *self, const char *data, uint16_t length);

/**
 * @brief  Queues application text for the BLE central or a TCP socket.
 * @param  self         Driver core.
 * @param  destination  FC41D_DATA_BLE or FC41D_DATA_TCP.
 * @param  id           TCP socket id, 0xFFFF = every accepted client (ignored for BLE).
 * @param  data         Text, no CR / LF (and no double quote for TCP).
 * @param  length       Number of characters.
 * @return Bytes queued, 0 = refused (channel down, bad characters, queue full).
 */
uint16_t       fc41d_core_send(fc41d_core_t *self, fc41d_data_source_e destination, uint16_t id, const char *data, uint16_t length);

/**
 * @brief  Takes the oldest received message out of the message queue. Pieces of TCP data
 *         from the same socket that wait behind each other are joined as far as they fit.
 * @param  self      Driver core.
 * @param  source    Receives where the message came from (BLE or TCP).
 * @param  id        Receives the socket id for TCP.
 * @param  buffer    Receives the text, NUL terminated.
 * @param  capacity  Size of buffer including the NUL.
 * @return Number of characters, 0 = nothing received.
 */
uint16_t       fc41d_core_pull_message(fc41d_core_t *self, fc41d_data_source_e *source, uint16_t *id, char *buffer, uint16_t capacity);

bool           fc41d_core_is_ready(const fc41d_core_t *self);

// Clears flags, queues and parser; keeps callbacks, setup and diagnostics.
void           fc41d_core_reset_runtime(fc41d_core_t *self);

// Leaves the current state and comes back to next_state after delay_ms.
void           fc41d_core_start_delay(fc41d_core_t *self, fc41d_core_state_e next_state, uint32_t delay_ms);

/* ----------------------------------------------------------------------- */
/*  Command engine: one AT command at a time                                 */
/* ----------------------------------------------------------------------- */
/**
 * @brief  Sends one AT command and waits for its answer. It never blocks: call it again and
 *         again with the SAME arguments until it stops returning FC41D_IN_PROGRESS.
 * @param  self               Driver core.
 * @param  command            Complete command line, ends with CR LF.
 * @param  expected_response  Line that ends the command. "OK" and "ERROR" must match the
 *                            whole line, other text only the beginning. NULL = "OK".
 * @param  timeout_ms         Time allowed for the answer of one attempt.
 * @param  max_try_count      Attempts before giving up.
 * @return FC41D_IN_PROGRESS while working, FC41D_OK when the expected line arrived,
 *         FC41D_HARD_FAIL when every attempt got ERROR or no answer.
 */
fc41d_return_e fc41d_command_handler(fc41d_core_t *self, const char *command, const char *expected_response, uint16_t timeout_ms, uint16_t max_try_count);
void           fc41d_command_handler_reset(fc41d_core_t *self);

/** @brief The same as fc41d_command_handler with a readable wait profile (FC41D_WAIT_*). */
fc41d_return_e fc41d_core_command(fc41d_core_t *self, const char *command, const char *expected_response, fc41d_wait_t wait);
// Like fc41d_core_command, but a failure is not an error: it reports FC41D_OK.
fc41d_return_e fc41d_core_try_command(fc41d_core_t *self, const char *command, const char *expected_response, fc41d_wait_t wait);

/**
 * @brief  Builds "<head><ssid>[,<password>]" plus CR LF with the characters the module needs
 *         escaped (AT+QSTAAPINFO, AT+QSTAAPINFODEF, AT+QSOFTAP).
 * @param  head      Command name including the "=".
 * @return The command (in the core's command buffer), NULL when it does not fit.
 */
const char    *fc41d_core_credentials_command(fc41d_core_t *self, const char *head, const char *ssid, const char *password);

/* ----------------------------------------------------------------------- */
/*  Parser (FC41D_parser.c)                                                  */
/* ----------------------------------------------------------------------- */
void           fc41d_parser_reset(fc41d_core_t *self);
/** @brief Names the line that ends the command in flight (used by the command engine). */
void           fc41d_parser_expect(fc41d_core_t *self, const char *expected_response);
/** @brief Reads everything in the receive queue: answers, URC events and application messages. */
void           fc41d_parser_service(fc41d_core_t *self);

/* ----------------------------------------------------------------------- */
/*  Sub-module state machines. Each returns FC41D_IN_PROGRESS while working, */
/*  FC41D_OK when its feature is up, FC41D_HARD_FAIL when it gave up.        */
/* ----------------------------------------------------------------------- */
fc41d_return_e fc41d_config_state_machine(fc41d_core_t *self);
fc41d_return_e fc41d_wifi_sta_state_machine(fc41d_core_t *self);
fc41d_return_e fc41d_wifi_ap_state_machine(fc41d_core_t *self);
fc41d_return_e fc41d_tcp_client_state_machine(fc41d_core_t *self);
fc41d_return_e fc41d_tcp_server_state_machine(fc41d_core_t *self);
fc41d_return_e fc41d_ble_gatt_state_machine(fc41d_core_t *self);

// Runtime jobs, one AT command each, started by the idle state.
fc41d_return_e fc41d_tcp_read_state_machine(fc41d_core_t *self);   // read the lowest socket with data
fc41d_return_e fc41d_tcp_send_state_machine(fc41d_core_t *self);   // send tx_job over TCP
fc41d_return_e fc41d_ble_send_state_machine(fc41d_core_t *self);   // send tx_job to the BLE central

#endif /* FC41D_DRIVER_INC_FC41D_CORE_H_ */
