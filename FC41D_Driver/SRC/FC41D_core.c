/*
 * File     : FC41D_core.c
 * Brief    : Driver core: tick and polling, UART receive path, command engine, feature state machine, recovery (soft / hard reset, BLE priority, heartbeat), run-time jobs, message queue and send path.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-30
 *
 * Revisions
 *   R1  2026-09-30  First complete version
 *   R2  2026-10-05  Readability rewrite, simpler API (settings table, no text copy helpers, fc41d_format)
 *   R3  2026-10-05  Run-time Wi-Fi / TCP changes, fc41d_core_pull_message, per-channel send checks
 *   R4  2026-10-05  Recovery: independent channels, BLE priority, heartbeat, feature retry, soft before hard reset
 *   R5  2026-10-05  Doxygen briefs (parameters, return values) for the critical functions
 *   R6  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  What is in here
 *    1. start, 1 ms tick, polling
 *    2. the UART receive path
 *    3. the command engine (send one AT command, wait for its answer)
 *    4. the state machine that brings the features up in priority order
 *    5. what happens once everything is up (events, reading, sending)
 */

#include "FC41D_Driver/INC/FC41D_core.h"

#include <string.h>

static void           fc41d_communication_type_state_machine(fc41d_core_t *self);
static fc41d_core_state_e fc41d_get_next_setup_state(fc41d_core_t *self);
static void           fc41d_core_runtime(fc41d_core_t *self);
static bool           fc41d_core_process_events(fc41d_core_t *self);
static fc41d_return_e fc41d_core_receive_handler(fc41d_core_t *self);
static fc41d_return_e fc41d_core_transmit_handler(fc41d_core_t *self);
static void           fc41d_core_fault(fc41d_core_t *self, fc41d_core_state_e failed_state);
static void           fc41d_core_rx_pump(fc41d_core_t *self);
static void           fc41d_core_apply_customization(fc41d_core_t *self);
static bool           fc41d_core_run_background(fc41d_core_t *self);
static fc41d_return_e fc41d_core_run_job(fc41d_core_t *self);
static void           fc41d_core_retry_feature(fc41d_core_t *self, fc41d_core_state_e failed_state);
static void           fc41d_core_restart_wifi(fc41d_core_t *self);
static void           fc41d_core_restart_tcp(fc41d_core_t *self);
static fc41d_return_e fc41d_core_ping_handler(fc41d_core_t *self);

static uint16_t fc41d_socket_bit(uint16_t id) {
    return (id <= FC41D_TCP_MAX_SOCKET_ID) ? (uint16_t)(1U << id) : 0U;
}

/* ======================================================================= */
/*  1. Start, tick, polling                                                  */
/* ======================================================================= */
fc41d_return_e fc41d_core_initialize(fc41d_core_t *self) {
    if(self == NULL) {
        return FC41D_NULL_POINTER;
    }

    if(self->callback.fc41d_hardware_transmit_function == NULL ||
       self->callback.fc41d_hardware_receive_function  == NULL) {
        return FC41D_NULL_POINTER;
    }

    self->timer.task        = 0UL;
    self->timer.delay_start = 0UL;
    self->timer.delay_ms    = 0UL;
    memset(&self->diag, 0, sizeof(self->diag));
    self->defaults_written = false;                         // survives a fault recovery, cleared only here

    fc41d_queue_initialize(&self->message_queue);           // not part of reset_runtime: what was received stays readable
    fc41d_core_reset_runtime(self);

    if(self->callback.fc41d_set_IO_pin_LOW_function != NULL) {
        self->callback.fc41d_set_IO_pin_LOW_function();     // module off until the bring-up powers it
    }

    return FC41D_OK;
}

// Everything except callbacks, setup (credentials, ports ...) and diagnostics.
void fc41d_core_reset_runtime(fc41d_core_t *self) {
    self->is_module_ready        = false;
    self->communication_progress = FC41D_IN_PROGRESS;
    self->state                  = FC41D_IDLE;
    self->next_state             = FC41D_IDLE;
    self->job                    = FC41D_JOB_NONE;
    self->last_job               = FC41D_JOB_NONE;
    self->background_pause       = false;
    self->ping_failures          = 0U;
    self->ping_tick              = self->timer.task;

    memset(&self->step,   0, sizeof(self->step));
    memset(&self->net,    0, sizeof(self->net));
    memset(&self->tx_job, 0, sizeof(self->tx_job));
    self->net.ble_mtu = FC41D_BLE_DEFAULT_MTU;

    self->communication_type.wifi_sta   = false;
    self->communication_type.wifi_ap    = false;
    self->communication_type.tcp_client = false;
    self->communication_type.tcp_server = false;
    self->communication_type.ble_gatt   = false;

    self->setup.config.state = FC41D_POWER_ON;

    {
        uint16_t irq = FC41D_CRITICAL_ENTER();
        fc41d_queue_initialize(&self->receive_queue);
        FC41D_CRITICAL_EXIT(irq);
    }
    fc41d_queue_initialize(&self->transmit_queue);

    fc41d_parser_reset(self);
    fc41d_command_handler_reset(self);
}

fc41d_return_e fc41d_core_1ms_timer(fc41d_core_t *self) {
    if(self == NULL) {
        return FC41D_NULL_POINTER;
    }

    self->timer.task++;                 // the only thing the tick does

    return FC41D_OK;
}

fc41d_return_e fc41d_core_polling(fc41d_core_t *self) {
    if(self == NULL) {
        return FC41D_NULL_POINTER;
    }

    // Time only moves when the 1 ms tick is called. Without it every wait lasts
    // forever and the module never gets a single command, so say it loudly.
    self->diag.polls++;
    if(self->diag.polls == FC41D_TICK_CHECK_POLLS && self->timer.task == 0UL) {
        self->diag.tick_missing = true;
    }

#ifndef FC41D_RX_FROM_ISR
    fc41d_core_rx_pump(self);                           // UART FIFO -> receive queue
#endif
    fc41d_parser_service(self);                         // receive queue -> URCs, answers, data
    fc41d_communication_type_state_machine(self);

    return FC41D_OK;
}

bool fc41d_core_is_ready(const fc41d_core_t *self) {
    return (self != NULL) && self->is_module_ready && (self->communication_progress == FC41D_READY);
}

void fc41d_core_start_delay(fc41d_core_t *self, fc41d_core_state_e next_state, uint32_t delay_ms) {
    self->timer.delay_start = self->timer.task;
    self->timer.delay_ms    = delay_ms;
    self->next_state        = next_state;
    self->state             = FC41D_DELAY;
}

/* ======================================================================= */
/*  2. UART receive path                                                     */
/* ======================================================================= */
uint16_t fc41d_core_receive_data(fc41d_core_t *self, const char *data, uint16_t length) {
    uint16_t index;
    uint16_t irq;

    if(self == NULL || data == NULL) {
        return 0U;
    }

    irq = FC41D_CRITICAL_ENTER();
    for(index = 0U; index < length; index++) {
        if(fc41d_queue_space(&self->receive_queue) == 0U) {
            self->diag.rx_overflow++;                   // the oldest byte is lost
        }
        fc41d_queue_push(&self->receive_queue, data[index]);
    }
    FC41D_CRITICAL_EXIT(irq);

    self->diag.rx_bytes += length;

    return length;
}

// The UART FIFO is only 16 deep (1.4 ms at 115200 baud): empty it completely every time.
static void fc41d_core_rx_pump(fc41d_core_t *self) {
    char     chunk[16];
    uint16_t received;
    uint16_t rounds = 0U;

    do {
        received = self->callback.fc41d_hardware_receive_function(chunk, 16U);
        if(received > 16U) {
            received = 16U;
        }
        if(received > 0U) {
            (void)fc41d_core_receive_data(self, chunk, received);
        }
        rounds++;
    } while(received == 16U && rounds < 8U);
}

// With FC41D_RX_FROM_ISR defined this is the ONLY place that reads the UART:
// call it from an interrupt that runs more often than every 1.4 ms.
void fc41d_core_rx_service(fc41d_core_t *self) {
    if(self != NULL) {
        fc41d_core_rx_pump(self);
    }
}

/* ======================================================================= */
/*  3. Command engine                                                        */
/* ======================================================================= */
void fc41d_command_handler_reset(fc41d_core_t *self) {
    fc41d_command_handler_t *ch = &self->command_handler;

    ch->state      = FC41D_COMMAND_TRANSMIT;
    ch->started    = false;
    ch->try_count  = 0U;
    ch->sent       = 0U;
    ch->length     = 0U;
    ch->start_tick = self->timer.task;

    self->parser.expect = NULL;
    self->parser.result = FC41D_RESULT_NONE;
}

// Hands as much of the command to the UART as it takes (its FIFO holds 16 bytes).
static void fc41d_command_send_part(fc41d_core_t *self, const char *command) {
    fc41d_command_handler_t *ch = &self->command_handler;
    uint16_t accepted = self->callback.fc41d_hardware_transmit_function(&command[ch->sent], (uint16_t)(ch->length - ch->sent));
    uint16_t i;

    for(i = 0U; i < accepted; i++) {
        fc41d_log_add(self->diag.tx_log, &self->diag.tx_log_pos, command[ch->sent + i]);
    }

    ch->sent            += accepted;
    self->diag.tx_bytes += accepted;

    if(ch->sent >= ch->length) {
        ch->state      = FC41D_WAITING_FOR_RESPONSE;
        ch->start_tick = self->timer.task;
    }
    else if(accepted == 0U && FC41D_ELAPSED_MS(self, ch->start_tick) > FC41D_TX_STALL_MS) {
        ch->state = FC41D_COMMAND_ERROR;                // the UART does not move
    }
}

fc41d_return_e fc41d_command_handler(fc41d_core_t *self, const char *command, const char *expected_response, uint16_t timeout_ms, uint16_t max_try_count) {
    fc41d_command_handler_t *ch;

    if(self == NULL || command == NULL) {
        return FC41D_NULL_POINTER;
    }

    ch = &self->command_handler;
    if(max_try_count == 0U) {
        max_try_count = 1U;
    }

    switch(ch->state) {
        case FC41D_COMMAND_TRANSMIT: {
            if(!ch->started) {                          // first call of this attempt
                ch->length = fc41d_str_length(command);
                if(ch->length == 0U) {
                    fc41d_command_handler_reset(self);
                    return FC41D_HARD_FAIL;
                }
                ch->started    = true;
                ch->sent       = 0U;
                ch->start_tick = self->timer.task;
                ch->try_count++;
                fc41d_parser_expect(self, (expected_response != NULL) ? expected_response : "OK");
            }
            fc41d_command_send_part(self, command);
        }
        break;

        case FC41D_WAITING_FOR_RESPONSE: {
            if(self->parser.result == FC41D_RESULT_MATCH) {
                self->diag.cmd_ok++;
                fc41d_command_handler_reset(self);      // ready for the next command
                return FC41D_OK;
            }
            else if(self->parser.result == FC41D_RESULT_ERROR) {
                self->diag.cmd_errors++;
                ch->state = FC41D_COMMAND_ERROR;
            }
            else if(FC41D_ELAPSED_MS(self, ch->start_tick) > timeout_ms) {
                self->diag.cmd_timeouts++;
                ch->state = FC41D_COMMAND_ERROR;
            }
        }
        break;

        case FC41D_COMMAND_ERROR: {
            if(ch->try_count >= max_try_count) {
                self->diag.cmd_hard_fails++;
                fc41d_command_handler_reset(self);      // the engine is usable again
                return FC41D_HARD_FAIL;
            }
            ch->started = false;                        // the next call sends the command again
            ch->sent    = 0U;
            ch->state   = FC41D_COMMAND_TRANSMIT;
        }
        break;

        default: {
            fc41d_command_handler_reset(self);
        }
        break;
    }

    return FC41D_IN_PROGRESS;
}

fc41d_return_e fc41d_core_command(fc41d_core_t *self, const char *command, const char *expected_response, fc41d_wait_t wait) {
    return fc41d_command_handler(self, command, expected_response, wait.timeout_ms, wait.tries);
}

fc41d_return_e fc41d_core_try_command(fc41d_core_t *self, const char *command, const char *expected_response, fc41d_wait_t wait) {
    fc41d_return_e ret = fc41d_core_command(self, command, expected_response, wait);

    return (ret == FC41D_HARD_FAIL) ? FC41D_OK : ret;
}

const char *fc41d_core_credentials_command(fc41d_core_t *self, const char *head, const char *ssid, const char *password) {
    if(password[0] == '\0') {
        return fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "%s%e\r\n", head, ssid);
    }

    return fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "%s%e,%e\r\n", head, ssid, password);
}

/* ======================================================================= */
/*  4. State machine                                                         */
/* ======================================================================= */
// Runs a sub-module state machine and reacts to how it ended.
static void fc41d_core_run_feature(fc41d_core_t *self, fc41d_return_e ret, fc41d_core_state_e feature) {
    if(ret == FC41D_OK) {
        fc41d_core_start_delay(self, FC41D_IDLE, 250U);
    }
    else if(ret == FC41D_HARD_FAIL) {
        fc41d_core_fault(self, feature);
    }
}

static void fc41d_communication_type_state_machine(fc41d_core_t *self) {
    if(self->net.wifi_changed || self->net.tcp_changed) {
        fc41d_core_apply_customization(self);
    }

    // The channels that are up keep working while another one is being set up.
    if(fc41d_core_run_background(self)) {
        return;
    }

    switch (self->state) {
        case FC41D_IDLE: {
            if(!self->is_module_ready) {
                fc41d_core_start_delay(self, FC41D_CONFIG, 250U);
            }
            else if(self->communication_progress != FC41D_READY) {
                if(self->job == FC41D_JOB_NONE && fc41d_core_process_events(self)) {
                    break;                                          // a link / BLE loss is handled even while something else is being set up
                }
                fc41d_core_state_e next_state = fc41d_get_next_setup_state(self);

                if(next_state == FC41D_IDLE) {
                    self->communication_progress  = FC41D_READY;    // every wanted feature is up
                    self->diag.consecutive_faults = 0U;
                }
                else {
                    fc41d_core_start_delay(self, next_state, 250U);
                }
            }
            else {
                fc41d_core_runtime(self);
            }
        }
        break;

        case FC41D_CONFIG: {
            fc41d_return_e ret = fc41d_config_state_machine(self);

            if(ret == FC41D_OK) {
                self->is_module_ready = true;
            }
            fc41d_core_run_feature(self, ret, FC41D_CONFIG);
        }
        break;

        case FC41D_WIFI_STA:   fc41d_core_run_feature(self, fc41d_wifi_sta_state_machine(self),   FC41D_WIFI_STA);   break;
        case FC41D_WIFI_AP:    fc41d_core_run_feature(self, fc41d_wifi_ap_state_machine(self),    FC41D_WIFI_AP);    break;
        case FC41D_TCP_CLIENT: fc41d_core_run_feature(self, fc41d_tcp_client_state_machine(self), FC41D_TCP_CLIENT); break;
        case FC41D_TCP_SERVER: fc41d_core_run_feature(self, fc41d_tcp_server_state_machine(self), FC41D_TCP_SERVER); break;
        case FC41D_BLE:        fc41d_core_run_feature(self, fc41d_ble_gatt_state_machine(self),   FC41D_BLE);        break;

        case FC41D_DELAY: {
            if(FC41D_ELAPSED_MS(self, self->timer.delay_start) >= self->timer.delay_ms) {
                self->state = self->next_state;
            }
        }
        break;

        default: {
            self->state = FC41D_IDLE;
        }
        break;
    }
}

// The priority is the bit order of the FEAT_ flags: ble > wifi > tcp.
// A feature is wanted when its bit is in the selected type and it is not up yet.
static fc41d_core_state_e fc41d_get_next_setup_state(fc41d_core_t *self) {
    const fc41d_communication_type_t *comm_type;

    if(self == NULL) {
        return FC41D_IDLE;
    }

    comm_type = &self->communication_type;

    if     ((comm_type->type & FEAT_BLE)        && !comm_type->ble_gatt)   { return FC41D_BLE; }
    else if((comm_type->type & FEAT_WIFI_STA)   && !comm_type->wifi_sta)   { return FC41D_WIFI_STA; }
    else if((comm_type->type & FEAT_WIFI_AP)    && !comm_type->wifi_ap)    { return FC41D_WIFI_AP; }
    else if((comm_type->type & FEAT_TCP_CLIENT) && !comm_type->tcp_client) { return FC41D_TCP_CLIENT; }
    else if((comm_type->type & FEAT_TCP_SERVER) && !comm_type->tcp_server) { return FC41D_TCP_SERVER; }
    else                                                                   { return FC41D_IDLE; }
}

/**
 * @brief  Is a phone connected and talking to us (it connected, wrote or took a notification
 *         within FC41D_BLE_ACTIVE_MS)? Then nothing may reset the module.
 * @return true when BLE is in use.
 */
static bool fc41d_core_ble_in_use(const fc41d_core_t *self) {
    return self->net.ble_connected && (FC41D_ELAPSED_MS(self, self->net.ble_activity_tick) < FC41D_BLE_ACTIVE_MS);
}

/**
 * @brief  A feature gave up or the module hangs. BLE has priority: while a phone uses BLE
 *         only the failed Wi-Fi / TCP feature starts again. Otherwise the module is reset
 *         and everything is set up from the start: AT+QRST first (soft), plus the enable
 *         pin (hard) when the module did not answer or a soft reset did not help.
 * @param  failed_state  The state that gave up (FC41D_CONFIG for a silent module).
 */
static void fc41d_core_fault(fc41d_core_t *self, fc41d_core_state_e failed_state) {
    uint16_t consecutive;
    bool     hard;

    if(fc41d_core_ble_in_use(self)) {
        self->diag.resets_blocked++;
        self->diag.last_fault_state = (uint16_t)failed_state;
        fc41d_core_retry_feature(self, failed_state);
        return;
    }

    consecutive = (uint16_t)(self->diag.consecutive_faults + 1U);
    hard        = (failed_state == FC41D_CONFIG) || (consecutive >= 2U);

    fc41d_core_reset_runtime(self);

    self->diag.faults++;
    self->diag.consecutive_faults = consecutive;
    self->diag.last_fault_state   = (uint16_t)failed_state;

    if(hard) {
        self->diag.hard_resets++;
        if(self->callback.fc41d_set_IO_pin_LOW_function != NULL) {
            self->callback.fc41d_set_IO_pin_LOW_function();
        }
        fc41d_core_start_delay(self, FC41D_IDLE, (consecutive >= FC41D_FAULT_BACKOFF_AFTER) ? FC41D_FAULT_BACKOFF_MS : FC41D_FAULT_OFF_MS);
    }
    else {
        self->diag.soft_resets++;
        fc41d_core_start_delay(self, FC41D_IDLE, FC41D_SOFT_RESET_GAP_MS);
    }
}

/* ======================================================================= */
/*  5. Everything is up                                                      */
/* ======================================================================= */
// The module reset by itself: configure it again from the start.
static void fc41d_core_handle_reboot(fc41d_core_t *self) {
    self->diag.reboots++;
    fc41d_core_reset_runtime(self);
}

// The Wi-Fi link dropped. Open sockets die silently, so forget them (and close
// them later); the module reconnects by itself, we only wait for it.
static void fc41d_core_handle_wifi_loss(fc41d_core_t *self) {
    fc41d_net_status_t         *net = &self->net;
    fc41d_communication_type_t *ct  = &self->communication_type;

    self->diag.link_losses++;

    if(!(ct->type & FEAT_WIFI_STA)) {
        return;
    }

    net->tcp_stale_mask |= net->tcp_open_mask | fc41d_socket_bit(FC41D_TCP_LISTENER_ID);
    net->tcp_open_mask   = 0U;
    net->tcp_rx_mask     = 0U;
    net->listener_open   = false;
    memset(net->clients, 0, sizeof(net->clients));

    ct->wifi_sta   = false;
    ct->tcp_client = false;
    ct->tcp_server = false;
    self->step.wifi_sta   = FC41D_QSTAAPINFO;           // no new connect command: just wait
    self->step.tcp_client = FC41D_CLIENT_QICLOSE_STALE;
    self->step.tcp_server = FC41D_QICFG;
    self->communication_progress = FC41D_IN_PROGRESS;
}

// The module is not advertising any more: set BLE up again.
static void fc41d_core_handle_ble_loss(fc41d_core_t *self) {
    self->communication_type.ble_gatt = false;
    self->step.ble = FC41D_QBLEDEINIT;
    self->communication_progress = FC41D_IN_PROGRESS;
}

// A socket was closed by the other side.
static void fc41d_core_handle_socket_closed(fc41d_core_t *self) {
    fc41d_communication_type_t *ct   = &self->communication_type;
    uint16_t                    mask = self->net.tcp_closed_mask;

    self->net.tcp_closed_mask = 0U;

    if((mask & fc41d_socket_bit(FC41D_TCP_CLIENT_SOCKET_ID)) && ct->tcp_client) {
        ct->tcp_client        = false;                  // connect again
        self->step.tcp_client = FC41D_CLIENT_QICLOSE_STALE;
        self->communication_progress = FC41D_IN_PROGRESS;
    }
    if((mask & fc41d_socket_bit(FC41D_TCP_LISTENER_ID)) && ct->tcp_server) {
        ct->tcp_server        = false;
        self->step.tcp_server = FC41D_QICFG;
        self->communication_progress = FC41D_IN_PROGRESS;
    }
}

// The Wi-Fi is started from the beginning (stop, then connect / start again). Every
// socket dies with it, so they are forgotten and closed later. BLE is not touched.
static void fc41d_core_restart_wifi(fc41d_core_t *self) {
    fc41d_net_status_t         *net = &self->net;
    fc41d_communication_type_t *ct  = &self->communication_type;

    net->tcp_stale_mask |= net->tcp_open_mask;
    if(ct->type & FEAT_TCP_SERVER) {
        net->tcp_stale_mask |= fc41d_socket_bit(FC41D_TCP_LISTENER_ID);
    }
    net->tcp_open_mask = 0U;
    net->tcp_rx_mask   = 0U;
    net->listener_open = false;
    memset(net->clients, 0, sizeof(net->clients));
    ct->tcp_client = false;
    ct->tcp_server = false;
    self->step.tcp_client = FC41D_CLIENT_QICLOSE_STALE;
    self->step.tcp_server = FC41D_QICFG;

    if(ct->type & FEAT_WIFI_STA) {
        net->sta_associated = false;                    // so the disconnect we cause is not reported as a lost link
        net->sta_got_ip     = false;
        net->sta_configured = false;
        net->sta_ever_up    = false;                    // the connect time limit applies to the new attempt
        ct->wifi_sta        = false;
        self->step.wifi_sta = FC41D_QSTASTOP;
        self->step.attempts = 0U;
    }
    else {
        net->ap_up          = false;
        ct->wifi_ap         = false;
        self->step.wifi_ap  = FC41D_QSOFTAPSTOP;
    }
}

// The TCP client socket / the listener is closed and opened again. Clients that
// already connected to our listener stay connected.
static void fc41d_core_restart_tcp(fc41d_core_t *self) {
    fc41d_net_status_t         *net      = &self->net;
    fc41d_communication_type_t *ct       = &self->communication_type;
    uint16_t                    client   = fc41d_socket_bit(FC41D_TCP_CLIENT_SOCKET_ID);
    uint16_t                    listener = fc41d_socket_bit(FC41D_TCP_LISTENER_ID);

    if(ct->type & FEAT_TCP_CLIENT) {
        net->tcp_stale_mask |= client;
        net->tcp_open_mask  &= (uint16_t)~client;
        net->tcp_rx_mask    &= (uint16_t)~client;
        ct->tcp_client        = false;
        self->step.tcp_client = FC41D_CLIENT_QICLOSE_STALE;
    }
    if(ct->type & FEAT_TCP_SERVER) {
        net->tcp_stale_mask |= listener;
        net->listener_open    = false;
        ct->tcp_server        = false;
        self->step.tcp_server = FC41D_QICFG;
    }
}

/**
 * @brief  The customize functions already wrote the new values into the setup. This makes
 *         the running features use them: the Wi-Fi or socket is closed and set up again,
 *         everything else (BLE above all) keeps running.
 */
static void fc41d_core_apply_customization(fc41d_core_t *self) {
    fc41d_net_status_t *net = &self->net;

    if(!self->is_module_ready) {                        // the bring-up has not used the values yet: it will read the new ones
        net->wifi_changed = false;
        net->tcp_changed  = false;
        return;
    }
    if(self->state == FC41D_BLE || self->job != FC41D_JOB_NONE) {
        return;                                         // not in the middle of something: try again on the next poll
    }

    fc41d_command_handler_reset(self);                  // the answer to a command we leave behind is ignored

    if(net->wifi_changed) {
        net->wifi_changed = false;
        fc41d_core_restart_wifi(self);
    }
    if(net->tcp_changed) {
        net->tcp_changed = false;
        fc41d_core_restart_tcp(self);
    }

    self->communication_progress = FC41D_IN_PROGRESS;
    fc41d_core_start_delay(self, FC41D_IDLE, 100U);     // the idle state starts the features again, in priority order
}

/**
 * @brief  A Wi-Fi / TCP feature gave up while BLE is in use: it starts again after a pause.
 *         The module and BLE are not touched.
 * @param  failed_state  The feature that gave up.
 */
static void fc41d_core_retry_feature(fc41d_core_t *self, fc41d_core_state_e failed_state) {
    if(failed_state != FC41D_WIFI_STA && failed_state != FC41D_WIFI_AP &&
       failed_state != FC41D_TCP_CLIENT && failed_state != FC41D_TCP_SERVER) {
        return;                                         // the module itself (or BLE): nothing to restart on its own
    }

    fc41d_command_handler_reset(self);
    self->diag.feature_retries++;

    if(failed_state == FC41D_WIFI_STA || failed_state == FC41D_WIFI_AP) {
        fc41d_core_restart_wifi(self);
    }
    else {
        fc41d_core_restart_tcp(self);
    }

    self->communication_progress = FC41D_IN_PROGRESS;
    fc41d_core_start_delay(self, FC41D_IDLE, FC41D_FEATURE_RETRY_MS);
}

// URC driven changes that send the driver back to a setup step.
static bool fc41d_core_process_events(fc41d_core_t *self) {
    fc41d_net_status_t *net = &self->net;

    if(net->module_rebooted) {
        fc41d_core_handle_reboot(self);
        return true;
    }
    if(net->sta_link_lost) {
        net->sta_link_lost = false;
        fc41d_core_handle_wifi_loss(self);
        return true;
    }
    if(net->ble_reinit) {
        net->ble_reinit = false;
        fc41d_core_handle_ble_loss(self);
        return true;
    }
    if(net->tcp_closed_mask != 0U) {
        fc41d_core_handle_socket_closed(self);
        return true;
    }

    return false;
}

// Picks the next job when none is running: read a socket or send a frame.
static fc41d_job_e fc41d_core_pick_job(fc41d_core_t *self) {
    bool rx_wanted;
    bool tx_wanted;

    // Silence for a while: is the module still there? This comes first. An open socket is
    // read every second, and those reads (which fail on a hung module) must not keep it waiting.
    if(FC41D_ELAPSED_MS(self, self->parser.last_byte_tick) >= FC41D_HEARTBEAT_MS &&
       FC41D_ELAPSED_MS(self, self->ping_tick) >= FC41D_HEARTBEAT_MS) {
        return FC41D_JOB_PING;
    }

    // A recv URC is sent once and never repeated: look at the open sockets now and then.
    if(self->net.tcp_open_mask != 0U && FC41D_ELAPSED_MS(self, self->net.tcp_poll_tick) >= FC41D_TCP_POLL_MS) {
        self->net.tcp_rx_mask  |= self->net.tcp_open_mask;
        self->net.tcp_poll_tick = self->timer.task;
    }

    rx_wanted = (self->net.tcp_rx_mask != 0U);
    tx_wanted = self->tx_job.active || (fc41d_queue_count(&self->transmit_queue) > 0U);

    if(rx_wanted && tx_wanted) {                        // take turns
        return (self->last_job == FC41D_JOB_RX) ? FC41D_JOB_TX : FC41D_JOB_RX;
    }
    if(rx_wanted) {
        return FC41D_JOB_RX;
    }
    if(tx_wanted) {
        return FC41D_JOB_TX;
    }

    return FC41D_JOB_NONE;
}

static fc41d_return_e fc41d_core_run_job(fc41d_core_t *self) {
    switch(self->job) {
        case FC41D_JOB_RX:   return fc41d_core_receive_handler(self);
        case FC41D_JOB_TX:   return fc41d_core_transmit_handler(self);
        case FC41D_JOB_PING: return fc41d_core_ping_handler(self);
        default:             return FC41D_OK;
    }
}

// The AT command engine handles one command at a time. It is free when no command is
// running and none is waiting for another attempt.
static bool fc41d_core_engine_is_idle(const fc41d_core_t *self) {
    const fc41d_command_handler_t *ch = &self->command_handler;

    return (ch->state == FC41D_COMMAND_TRANSMIT) && !ch->started && (ch->try_count == 0U);
}

/**
 * @brief  While a feature is set up (Wi-Fi connecting, a socket opening, ...) most of the
 *         time is spent waiting for a pause or an URC. The jobs of the channels that are up
 *         (BLE notifications, TCP read / send, heartbeat) run in those gaps, so a connection
 *         that is down does not stop the other.
 * @return true when it used this pass.
 */
static bool fc41d_core_run_background(fc41d_core_t *self) {
    if(!self->is_module_ready || self->state == FC41D_IDLE || self->state == FC41D_CONFIG) {
        return false;                                   // idle has its own runtime, the bring-up owns the engine
    }

    if(self->job == FC41D_JOB_NONE) {
        if(!fc41d_core_engine_is_idle(self) || self->background_pause) {
            self->background_pause = false;             // the feature gets its turn
            return false;
        }
        self->job = fc41d_core_pick_job(self);
        if(self->job == FC41D_JOB_NONE) {
            return false;
        }
        self->last_job = self->job;
    }

    if(fc41d_core_run_job(self) != FC41D_IN_PROGRESS) {
        self->job = FC41D_JOB_NONE;
        self->background_pause = true;
    }

    return true;
}

// "AT" to a module that was silent. Two unanswered ones in a row = it hangs.
static fc41d_return_e fc41d_core_ping_handler(fc41d_core_t *self) {
    fc41d_return_e ret = fc41d_core_command(self, "AT\r\n", "OK", FC41D_WAIT_QUICK);

    if(ret == FC41D_IN_PROGRESS) {
        return FC41D_IN_PROGRESS;
    }

    self->ping_tick = self->timer.task;
    self->diag.pings++;

    if(ret == FC41D_OK) {
        self->ping_failures = 0U;
        return FC41D_OK;
    }

    self->ping_failures++;
    if(self->ping_failures >= FC41D_PING_FAIL_LIMIT) {
        self->ping_failures = 0U;
        fc41d_core_fault(self, FC41D_CONFIG);           // BLE in use: refused there, then the next ping tries again
    }

    return FC41D_OK;
}

static void fc41d_core_runtime(fc41d_core_t *self) {
    if(self->job == FC41D_JOB_NONE) {
        if(fc41d_core_process_events(self)) {
            return;
        }
        self->job = fc41d_core_pick_job(self);
        if(self->job != FC41D_JOB_NONE) {
            self->last_job = self->job;
        }
    }

    if(fc41d_core_run_job(self) != FC41D_IN_PROGRESS) {
        self->job = FC41D_JOB_NONE;
    }
}

// Fetches the data of a socket the module told us about.
static fc41d_return_e fc41d_core_receive_handler(fc41d_core_t *self) {
    return fc41d_tcp_read_state_machine(self);
}

/* ----------------------------------------------------------------------- */
/*  Application data from the module                                         */
/* ----------------------------------------------------------------------- */
// The queue holds [source][id][length][text ...] frames; offset counts from the oldest byte.
static uint16_t fc41d_message_peek(const fc41d_core_queue_t *queue, uint16_t offset) {
    return (uint16_t)queue->buffer[(queue->read_index + offset) % (sizeof(queue->buffer) / sizeof(queue->buffer[0]))];
}

uint16_t fc41d_core_pull_message(fc41d_core_t *self, fc41d_data_source_e *source, uint16_t *id, char *buffer, uint16_t capacity) {
    fc41d_core_queue_t *queue = &self->message_queue;
    uint16_t            total = 0U;

    if(self == NULL || source == NULL || id == NULL || buffer == NULL || capacity < 2U) {
        return 0U;
    }

    buffer[0] = '\0';
    if(fc41d_queue_count(queue) < 3U) {
        return 0U;
    }

    *source = (fc41d_data_source_e)fc41d_message_peek(queue, 0U);
    *id     = fc41d_message_peek(queue, 1U);

    // One frame. TCP is a byte stream cut into pieces on the way, so the pieces of
    // the same socket right behind it are joined as long as they fit the buffer.
    while(fc41d_queue_count(queue) >= 3U) {
        uint16_t length = fc41d_message_peek(queue, 2U);
        uint16_t take   = length;
        char     skip;

        if(total > 0U) {
            bool same_stream = (*source == FC41D_DATA_TCP) && (fc41d_message_peek(queue, 0U) == (uint16_t)*source) && (fc41d_message_peek(queue, 1U) == *id);

            if(!same_stream || (total + length) > (uint16_t)(capacity - 1U)) {
                break;
            }
        }
        if(take > (uint16_t)(capacity - 1U - total)) {
            take = (uint16_t)(capacity - 1U - total);   // a frame longer than the buffer: the rest is dropped
        }

        (void)fc41d_queue_pull(queue, &skip);           // the three header characters
        (void)fc41d_queue_pull(queue, &skip);
        (void)fc41d_queue_pull(queue, &skip);

        if(take > 0U) {
            size_t got = 0U;
            (void)fc41d_queue_pull_multiple(queue, &buffer[total], take, &got);
            total = (uint16_t)(total + got);
        }
        for(; take < length; take++) {
            (void)fc41d_queue_pull(queue, &skip);
        }
    }

    buffer[total] = '\0';

    return total;
}

/* ----------------------------------------------------------------------- */
/*  Application data to the module                                           */
/*  A frame in the transmit queue is [destination][id][length][payload ...]  */
/* ----------------------------------------------------------------------- */
static uint16_t fc41d_core_queue_frames(fc41d_core_t *self, fc41d_data_source_e destination, uint16_t id, const char *data, uint16_t length, uint16_t chunk_max) {
    uint16_t queued = 0U;
    uint16_t chunk;
    uint16_t i;

    while(queued < length) {
        chunk = (uint16_t)(length - queued);
        if(chunk > chunk_max) {
            chunk = chunk_max;
        }

        if(fc41d_queue_space(&self->transmit_queue) < (size_t)(chunk + 3U)) {
            break;                                      // no room: the caller sees a short count
        }

        fc41d_queue_push(&self->transmit_queue, (char)destination);
        fc41d_queue_push(&self->transmit_queue, (char)id);
        fc41d_queue_push(&self->transmit_queue, (char)chunk);
        for(i = 0U; i < chunk; i++) {
            fc41d_queue_push(&self->transmit_queue, data[queued + i]);
        }
        queued += chunk;
    }

    return queued;
}

// The payload sits inside the AT command line, so these characters would end or break it.
static bool fc41d_core_payload_is_safe(const char *data, uint16_t length, fc41d_data_source_e destination) {
    uint16_t i;

    for(i = 0U; i < length; i++) {
        if(data[i] == '\r' || data[i] == '\n') {
            return false;
        }
        if(destination == FC41D_DATA_TCP && data[i] == '"') {
            return false;
        }
    }

    return true;
}

// Every channel is judged on its own: while the Wi-Fi is down BLE still sends, and the
// other way round.
static bool fc41d_core_destination_is_up(const fc41d_core_t *self, fc41d_data_source_e destination) {
    const fc41d_communication_type_t *ct = &self->communication_type;

    if(!self->is_module_ready) {
        return false;
    }

    return (destination == FC41D_DATA_BLE) ? ct->ble_gatt : (ct->tcp_client || ct->tcp_server);
}

uint16_t fc41d_core_send(fc41d_core_t *self, fc41d_data_source_e destination, uint16_t id, const char *data, uint16_t length) {
    uint16_t chunk_max = FC41D_TX_CHUNK;
    uint16_t i;

    if(self == NULL || data == NULL || length == 0U) {
        return 0U;
    }

    if(!fc41d_core_destination_is_up(self, destination) || !fc41d_core_payload_is_safe(data, length, destination)) {
        self->diag.tx_rejected++;
        return 0U;
    }

    if(destination == FC41D_DATA_BLE) {
        uint16_t mtu_payload = (uint16_t)(self->net.ble_mtu - FC41D_BLE_ATT_OVERHEAD);   // a longer notification is cut
        if(mtu_payload < chunk_max) {
            chunk_max = mtu_payload;
        }
        return fc41d_core_queue_frames(self, destination, 0U, data, length, chunk_max);
    }

    if(id != 0xFFFFU) {
        return fc41d_core_queue_frames(self, destination, id, data, length, chunk_max);
    }

    // every accepted client: the answer is what fitted for ALL of them
    {
        uint16_t sent_to = 0U;
        uint16_t result  = length;

        for(i = 0U; i < FC41D_TCP_MAX_CLIENTS; i++) {
            if(self->net.clients[i].in_use) {
                uint16_t queued = fc41d_core_queue_frames(self, destination, self->net.clients[i].id, data, length, chunk_max);
                sent_to++;
                if(queued < result) {
                    result = queued;
                }
            }
        }

        return (sent_to > 0U) ? result : 0U;
    }
}

static fc41d_return_e fc41d_core_transmit_handler(fc41d_core_t *self) {
    fc41d_tx_job_t *job = &self->tx_job;
    fc41d_return_e  ret;

    if(!job->active) {
        char     header[3];
        size_t   got = 0U;
        uint16_t length;

        if(fc41d_queue_count(&self->transmit_queue) < 3U) {
            return FC41D_OK;
        }

        fc41d_queue_pull_multiple(&self->transmit_queue, header, 3U, &got);
        length = (uint16_t)header[2];

        if(length > FC41D_TX_CHUNK) {
            fc41d_queue_initialize(&self->transmit_queue);      // a damaged frame: start the queue over
            self->diag.tx_dropped++;
            return FC41D_OK;
        }

        got = 0U;
        fc41d_queue_pull_multiple(&self->transmit_queue, job->data, length, &got);

        job->data[got]   = '\0';                                // so it can be printed with %s

        if(!fc41d_core_destination_is_up(self, (fc41d_data_source_e)header[0])) {
            self->diag.tx_dropped++;                            // that channel went down after the data was queued
            return FC41D_OK;
        }
        job->destination = (uint16_t)header[0];
        job->id          = (uint16_t)header[1];
        job->length      = (uint16_t)got;
        job->active      = true;
    }

    if(job->destination == FC41D_DATA_BLE) {
        ret = fc41d_ble_send_state_machine(self);
    }
    else {
        ret = fc41d_tcp_send_state_machine(self);
    }

    if(ret == FC41D_IN_PROGRESS) {
        return FC41D_IN_PROGRESS;
    }

    if(ret == FC41D_OK) {
        self->diag.tx_data_bytes += job->length;
        if(job->destination == FC41D_DATA_BLE) {
            self->net.ble_activity_tick = self->timer.task;     // the phone took a notification: it is there
        }
    }
    else {
        self->diag.tx_dropped++;
    }
    job->active = false;

    return FC41D_OK;
}
