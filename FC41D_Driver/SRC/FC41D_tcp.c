/*
 * File     : FC41D_tcp.c
 * Brief    : TCP client and server state machines, socket read and send jobs.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-29
 *
 * Revisions
 *   R1  2026-09-29  First complete version
 *   R2  2026-10-05  Readability rewrite, simpler API (settings table, no text copy helpers, fc41d_format)
 *   R3  2026-10-05  Stale sockets closed before a run-time change, own close events ignored
 *   R4  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  TCP client and server in the module's "buffer" access mode.
 *
 *  What the code relies on (see FC41D_wireless_module.md, TCP part):
 *   - AT+QIOPEN answers OK at once. "Connected" is the URC +QIOPEN: <id>,0
 *     (a refused or unreachable server gives +QIOPEN: <id>,<error> after ~20 s).
 *   - Buffer mode: +QIURC: "recv",<id> says that data is waiting. The parser
 *     marks the socket and the core reads it with AT+QIRD. The URC is sent
 *     once and never repeated, so open sockets are also read now and then.
 *   - The listener accepts clients by itself; each one shows up as
 *     +QIURC: "incoming",<id>,<listener>,"<ip>",<port>.
 *   - AT+QISEND takes the payload as text between quotes.
 *   - A refused connection is not a module problem: the client waits and tries
 *     again, it never power cycles the module.
 */

#include "FC41D_Driver/INC/FC41D_core.h"

#define FC41D_TCP_STEP_PAUSE_MS (100U)

static uint16_t fc41d_tcp_bit(uint16_t id) {
    return (id <= FC41D_TCP_MAX_SOCKET_ID) ? (uint16_t)(1U << id) : 0U;
}

// The lowest socket id in the mask (0xFFFF = none).
static uint16_t fc41d_tcp_lowest(uint16_t mask) {
    uint16_t id;

    for(id = 0U; id <= FC41D_TCP_MAX_SOCKET_ID; id++) {
        if(mask & (uint16_t)(1U << id)) {
            return id;
        }
    }

    return 0xFFFFU;
}

// AT+QICLOSE for every socket of the mask that may have died with the Wi-Fi.
// A socket that is already gone only answers ERROR, which does not matter.
static fc41d_return_e fc41d_tcp_close_stale(fc41d_core_t *self, uint16_t allowed_mask) {
    uint16_t       pending = (uint16_t)(self->net.tcp_stale_mask & allowed_mask);
    uint16_t       id;
    const char    *command;

    if(pending == 0U) {
        return FC41D_OK;
    }

    id      = fc41d_tcp_lowest(pending);
    command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QICLOSE=%u\r\n", id);

    if(fc41d_core_try_command(self, command, "OK", FC41D_WAIT_ONCE) == FC41D_OK) {
        self->net.tcp_stale_mask &= (uint16_t)~fc41d_tcp_bit(id);
        pending                  &= (uint16_t)~fc41d_tcp_bit(id);
        return (pending == 0U) ? FC41D_OK : FC41D_IN_PROGRESS;
    }

    return FC41D_IN_PROGRESS;
}

/* ======================================================================= */
/*  Client                                                                   */
/* ======================================================================= */
static void fc41d_tcp_client_next(fc41d_core_t *self, fc41d_tcp_client_state_e next_step, uint32_t pause_ms) {
    self->step.tcp_client = next_step;

    if(next_step == FC41D_CLIENT_QIOPEN) {
        self->parser.qiopen_seen = false;               // the result must come AFTER the open command
        self->net.tcp_closed_mask &= (uint16_t)~fc41d_tcp_bit(FC41D_TCP_CLIENT_SOCKET_ID);   // "closed" of the socket we just closed ourselves
    }
    if(pause_ms > 0UL) {
        fc41d_core_start_delay(self, FC41D_TCP_CLIENT, pause_ms);
    }
}

// The connection did not work out (server refused, unreachable, busy socket):
// forget it, close the socket and try again later.
static void fc41d_tcp_client_retry_later(fc41d_core_t *self) {
    self->diag.tcp_open_failures++;
    self->net.tcp_stale_mask |= fc41d_tcp_bit(FC41D_TCP_CLIENT_SOCKET_ID);
    fc41d_tcp_client_next(self, FC41D_CLIENT_QICLOSE_STALE, FC41D_TCP_RETRY_MS);
}

static fc41d_return_e fc41d_tcp_client_wait_for_connection(fc41d_core_t *self) {
    const fc41d_parser_t *p = &self->parser;

    if(p->qiopen_seen && p->qiopen_id == FC41D_TCP_CLIENT_SOCKET_ID) {
        if(p->qiopen_error == 0U) {
            self->communication_type.tcp_client = true;
            self->step.tcp_client = FC41D_CLIENT_QICLOSE_STALE;
            return FC41D_OK;
        }
        fc41d_tcp_client_retry_later(self);             // the server said no
    }
    else if(FC41D_ELAPSED_MS(self, self->step.wait_start) > FC41D_TCP_OPEN_TIMEOUT_MS) {
        fc41d_tcp_client_retry_later(self);             // no answer at all
    }

    return FC41D_IN_PROGRESS;
}

fc41d_return_e fc41d_tcp_client_state_machine(fc41d_core_t *self) {
    fc41d_return_e ret = FC41D_IN_PROGRESS;
    const char    *command;

    switch(self->step.tcp_client) {
        case FC41D_CLIENT_QICLOSE_STALE: {
            ret = fc41d_tcp_close_stale(self, (uint16_t)~fc41d_tcp_bit(FC41D_TCP_LISTENER_ID));
            if(ret == FC41D_OK) {
                fc41d_tcp_client_next(self, FC41D_CLIENT_QICFG, FC41D_TCP_STEP_PAUSE_MS);
            }
            ret = FC41D_IN_PROGRESS;
        }
        break;

        case FC41D_CLIENT_QICFG: {
            // The payload is text. The setting exists on firmware 2025 and later; where it does not,
            // text is the default anyway, so a refusal is harmless.
            if(fc41d_core_try_command(self, "AT+QICFG=\"datatype\",0\r\n", "OK", FC41D_WAIT_ONCE) == FC41D_OK) {
                fc41d_tcp_client_next(self, FC41D_CLIENT_QIOPEN, FC41D_TCP_STEP_PAUSE_MS);
            }
        }
        break;

        case FC41D_CLIENT_QIOPEN: {
            // AT+QIOPEN=<socket>,"TCP","<server ip>",<server port>,<local port 0>,<buffer mode 0>
            command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QIOPEN=%u,\"TCP\",\"%i\",%u,0,0\r\n",
                                   FC41D_TCP_CLIENT_SOCKET_ID, self->setup.tcp_client_config.ip_address, self->setup.tcp_client_config.port);
            if(command == NULL) {
                return FC41D_HARD_FAIL;
            }

            ret = fc41d_core_command(self, command, "OK", FC41D_WAIT_SLOW);
            if(ret == FC41D_HARD_FAIL) {
                fc41d_tcp_client_retry_later(self);
            }
            else if(ret == FC41D_OK) {                                      // accepted: the result is a URC
                self->step.wait_start = self->timer.task;
                fc41d_tcp_client_next(self, FC41D_CLIENT_QIOPENWAIT, 0UL);
            }
            ret = FC41D_IN_PROGRESS;
        }
        break;

        case FC41D_CLIENT_QIOPENWAIT: {
            ret = fc41d_tcp_client_wait_for_connection(self);
        }
        break;

        default: {
            self->step.tcp_client = FC41D_CLIENT_QICLOSE_STALE;
        }
        break;
    }

    return ret;
}

/* ======================================================================= */
/*  Server                                                                   */
/* ======================================================================= */
static void fc41d_tcp_server_next(fc41d_core_t *self, fc41d_tcp_server_state_e next_step, uint32_t pause_ms) {
    self->step.tcp_server = next_step;

    if(next_step == FC41D_SERVER_QIOPEN) {
        self->parser.qiopen_seen = false;
        self->net.tcp_closed_mask &= (uint16_t)~fc41d_tcp_bit(FC41D_TCP_LISTENER_ID);
    }
    if(pause_ms > 0UL) {
        fc41d_core_start_delay(self, FC41D_TCP_SERVER, pause_ms);
    }
}

static fc41d_return_e fc41d_tcp_server_wait_for_listener(fc41d_core_t *self) {
    const fc41d_parser_t *p = &self->parser;

    if(p->qiopen_seen && p->qiopen_id == FC41D_TCP_LISTENER_ID) {
        self->step.tcp_server = FC41D_QICFG;
        if(p->qiopen_error != 0U) {
            return FC41D_HARD_FAIL;                     // port in use / no network: start over
        }
        self->net.listener_open = true;
        self->communication_type.tcp_server = true;
        return FC41D_OK;
    }

    if(FC41D_ELAPSED_MS(self, self->step.wait_start) > FC41D_TCP_LISTEN_TIMEOUT_MS) {
        self->step.tcp_server = FC41D_QICFG;
        return FC41D_HARD_FAIL;                         // no answer
    }

    return FC41D_IN_PROGRESS;
}

fc41d_return_e fc41d_tcp_server_state_machine(fc41d_core_t *self) {
    fc41d_return_e ret = FC41D_IN_PROGRESS;
    const char    *command;

    switch(self->step.tcp_server) {
        case FC41D_QICFG: {
            if(self->net.tcp_stale_mask != 0U) {                            // the old listener / clients from before a Wi-Fi loss or a change
                (void)fc41d_tcp_close_stale(self, 0xFFFFU);
                break;
            }
            // Clients are accepted automatically (the default); a refusal is harmless.
            if(fc41d_core_try_command(self, "AT+QICFG=\"accept/mode\",0\r\n", "OK", FC41D_WAIT_ONCE) == FC41D_OK) {
                fc41d_tcp_server_next(self, FC41D_QICFG_DATATYPE, FC41D_TCP_STEP_PAUSE_MS);
            }
        }
        break;

        case FC41D_QICFG_DATATYPE: {
            if(fc41d_core_try_command(self, "AT+QICFG=\"datatype\",0\r\n", "OK", FC41D_WAIT_ONCE) == FC41D_OK) {
                fc41d_tcp_server_next(self, FC41D_SERVER_QIOPEN, FC41D_TCP_STEP_PAUSE_MS);
            }
        }
        break;

        case FC41D_SERVER_QIOPEN: {
            // AT+QIOPEN=<socket>,"TCP LISTENER","",0,<port to listen on>,<buffer mode 0>
            command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QIOPEN=%u,\"TCP LISTENER\",\"\",0,%u,0\r\n",
                                   FC41D_TCP_LISTENER_ID, self->setup.tcp_server_config.port);
            if(command == NULL) {
                return FC41D_HARD_FAIL;
            }

            ret = fc41d_core_command(self, command, "OK", FC41D_WAIT_SLOW);
            if(ret == FC41D_OK) {
                self->step.wait_start = self->timer.task;
                fc41d_tcp_server_next(self, FC41D_SERVER_QIOPENWAIT, 0UL);
                ret = FC41D_IN_PROGRESS;
            }
        }
        break;

        case FC41D_SERVER_QIOPENWAIT: {
            ret = fc41d_tcp_server_wait_for_listener(self);
        }
        break;

        default: {
            self->step.tcp_server = FC41D_QICFG;
        }
        break;
    }

    return ret;
}

/* ======================================================================= */
/*  Runtime: read and send                                                   */
/* ======================================================================= */
// AT+QIRD=<socket>,<chunk>  ->  +QIRD: <n>, n bytes of data (the parser passes them on), OK.
// A full chunk means there may be more, so the socket stays marked.
fc41d_return_e fc41d_tcp_read_state_machine(fc41d_core_t *self) {
    fc41d_net_status_t *net = &self->net;
    const char         *command;
    fc41d_return_e      ret;
    uint16_t            id;

    if(!self->command_handler.started) {                // choose the socket now and keep it until the answer is in
        if(net->tcp_rx_mask == 0U) {
            return FC41D_OK;
        }
        self->parser.qird_socket   = fc41d_tcp_lowest(net->tcp_rx_mask);
        self->parser.last_qird_len = 0U;
    }
    id = self->parser.qird_socket;

    command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QIRD=%u,%u\r\n", id, FC41D_TCP_READ_CHUNK);
    ret     = fc41d_core_command(self, command, "OK", FC41D_WAIT_ONCE);

    if(ret == FC41D_IN_PROGRESS) {
        return FC41D_IN_PROGRESS;
    }

    if(ret == FC41D_OK && self->parser.last_qird_len >= FC41D_TCP_READ_CHUNK) {
        return FC41D_OK;                                // a full chunk: stay marked and read again
    }

    net->tcp_rx_mask &= (uint16_t)~fc41d_tcp_bit(id);   // drained, or this socket cannot be read
    return FC41D_OK;
}

// AT+QISEND=<socket>,<length>,"<text>"  ->  +QISEND: <n>, OK
fc41d_return_e fc41d_tcp_send_state_machine(fc41d_core_t *self) {
    const fc41d_tx_job_t *job = &self->tx_job;
    const char           *command;

    if((self->net.tcp_open_mask & fc41d_tcp_bit(job->id)) == 0U) {
        return FC41D_HARD_FAIL;                         // that socket is not connected
    }

    command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QISEND=%u,%u,\"%s\"\r\n", job->id, job->length, job->data);
    if(command == NULL) {
        return FC41D_HARD_FAIL;
    }

    return fc41d_core_command(self, command, "OK", FC41D_WAIT_SLOW);
}
