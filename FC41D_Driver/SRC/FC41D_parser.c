/*
 * File     : FC41D_parser.c
 * Brief    : Byte-by-byte parser: lines, announced payloads, URC table, delivery of received messages, fc41d_format.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-29
 *
 * Revisions
 *   R1  2026-09-29  First complete version
 *   R2  2026-10-05  Readability rewrite, simpler API (settings table, no text copy helpers, fc41d_format)
 *   R3  2026-10-05  Messages go into the message queue; a URC glued to unterminated BLE text is split off
 *   R4  2026-10-05  BLE activity time for the BLE priority rule
 *   R5  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  What is in here
 *    1. text helpers
 *    2. fc41d_format: builds the AT command lines
 *    3. the URC handlers and the table that picks one by the start of a line
 *    4. the byte-by-byte reader (lines and length-announced payloads)
 *
 *  Why byte by byte: the module may cut an answer anywhere (a response was
 *  seen to stop for 150 ms in the middle of a line) and TCP data may contain
 *  CR/LF or the word "OK". So the reader never relies on pauses or on where a
 *  burst of bytes ended:
 *    - a line ends with LF (CR is skipped)
 *    - after "+QIRD: <n>" exactly n bytes are payload, whatever they contain
 *    - a text line that is not a known keyword or URC is BLE data (the module
 *      writes what a phone wrote to the UART without any wrapper)
 */

#include "FC41D_Driver/INC/FC41D_core.h"

#include <stdarg.h>
#include <string.h>

/* ======================================================================= */
/*  1. Text helpers                                                          */
/* ======================================================================= */
uint16_t fc41d_str_length(const char *text) {
    uint16_t length = 0U;

    if(text == NULL) {
        return 0U;
    }
    while(text[length] != '\0') {
        length++;
    }

    return length;
}

bool fc41d_str_equal(const char *a, const char *b) {
    if(a == NULL || b == NULL) {
        return false;
    }
    while(*a != '\0' && *a == *b) {
        a++;
        b++;
    }

    return (*a == *b);
}

bool fc41d_str_starts_with(const char *text, const char *prefix) {
    if(text == NULL || prefix == NULL) {
        return false;
    }
    while(*prefix != '\0') {
        if(*text != *prefix) {
            return false;
        }
        text++;
        prefix++;
    }

    return true;
}

bool fc41d_str_parse_uint(const char **cursor, uint32_t *value) {
    const char *c      = *cursor;
    uint32_t    result = 0UL;
    bool        found  = false;

    while(*c == ' ' || *c == '<') {
        c++;
    }
    while(*c >= '0' && *c <= '9') {
        result = (result * 10UL) + (uint32_t)(*c - '0');
        found  = true;
        c++;
    }

    if(found) {
        *value  = result;
        *cursor = c;
    }

    return found;
}

void fc41d_log_add(char *log, uint16_t *position, char c) {
    log[*position] = c;
    *position = (uint16_t)((*position + 1U) % (FC41D_LOG_SIZE - 1U));
    log[*position] = '\0';                                  // the string always ends at the newest character
}

/* ======================================================================= */
/*  2. fc41d_format                                                          */
/* ======================================================================= */
static bool fc41d_put(char *buffer, uint16_t capacity, uint16_t *length, char c) {
    if((*length + 1U) >= capacity) {                        // one place stays free for the NUL
        return false;
    }
    buffer[*length] = c;
    (*length)++;

    return true;
}

static bool fc41d_put_text(char *buffer, uint16_t capacity, uint16_t *length, const char *text, bool escape) {
    bool fits = true;

    while(*text != '\0') {
        if(escape && (*text == '\\' || *text == ',' || *text == ';' || *text == '=')) {
            fits = fc41d_put(buffer, capacity, length, '\\') && fits;
        }
        fits = fc41d_put(buffer, capacity, length, *text) && fits;
        text++;
    }

    return fits;
}

static bool fc41d_put_number(char *buffer, uint16_t capacity, uint16_t *length, unsigned long value) {
    char     digits[10];
    uint16_t count = 0U;
    bool     fits  = true;

    do {
        digits[count] = (char)('0' + (value % 10UL));
        count++;
        value /= 10UL;
    } while(value > 0UL && count < 10U);

    while(count > 0U) {
        count--;
        fits = fc41d_put(buffer, capacity, length, digits[count]) && fits;
    }

    return fits;
}

const char *fc41d_format(char *buffer, uint16_t capacity, const char *format, ...) {
    va_list  args;
    uint16_t length = 0U;
    bool     fits   = true;

    if(capacity == 0U) {
        return NULL;
    }

    va_start(args, format);

    while(*format != '\0') {
        if(*format != '%') {
            fits = fc41d_put(buffer, capacity, &length, *format) && fits;
            format++;
            continue;
        }

        format++;                                           // the letter after the percent sign
        switch(*format) {
            case 's': {
                fits = fc41d_put_text(buffer, capacity, &length, va_arg(args, const char *), false) && fits;
            }
            break;

            case 'e': {
                fits = fc41d_put_text(buffer, capacity, &length, va_arg(args, const char *), true) && fits;
            }
            break;

            case 'u': {
                fits = fc41d_put_number(buffer, capacity, &length, (unsigned long)va_arg(args, unsigned int)) && fits;
            }
            break;

            case 'l': {
                fits = fc41d_put_number(buffer, capacity, &length, va_arg(args, unsigned long)) && fits;
            }
            break;

            case 'i': {
                const uint8_t *ip = va_arg(args, const uint8_t *);
                uint16_t       i;

                for(i = 0U; i < 4U; i++) {
                    fits = fc41d_put_number(buffer, capacity, &length, (unsigned long)(ip[i] & 0xFFU)) && fits;
                    if(i < 3U) {
                        fits = fc41d_put(buffer, capacity, &length, '.') && fits;
                    }
                }
            }
            break;

            case '%': {
                fits = fc41d_put(buffer, capacity, &length, '%') && fits;
            }
            break;

            default: {
                fits = false;                               // a format letter we do not know
            }
            break;
        }

        if(*format != '\0') {
            format++;
        }
    }

    va_end(args);
    buffer[length] = '\0';

    return fits ? buffer : NULL;
}

/* ======================================================================= */
/*  3. URC handlers                                                          */
/*  Each one gets the text that follows its prefix in the table below.       */
/* ======================================================================= */
static uint16_t fc41d_socket_bit(uint32_t id) {
    return (id <= FC41D_TCP_MAX_SOCKET_ID) ? (uint16_t)(1U << id) : 0U;
}

// Application text for the user: one frame [source][id][length][text ...] in the
// message queue (fc41d_core_pull_message takes it out). A frame is stored whole
// or not at all, so a full queue loses the NEW message and never damages an old one.
static void fc41d_parser_deliver(fc41d_core_t *self, fc41d_data_source_e source, uint16_t id, const char *data, uint16_t length) {
    fc41d_core_queue_t *queue = &self->message_queue;
    uint16_t            i;

    if(length == 0U) {
        return;
    }

    if(source == FC41D_DATA_BLE) {
        self->net.ble_activity_tick = self->timer.task;     // the phone wrote: BLE is in use (also when the queue is full)
    }

    if(fc41d_queue_space(queue) < (size_t)(length + 3U)) {
        self->diag.rx_dropped++;
        return;
    }

    fc41d_queue_push(queue, (char)source);
    fc41d_queue_push(queue, (char)id);
    fc41d_queue_push(queue, (char)length);
    for(i = 0U; i < length; i++) {
        fc41d_queue_push(queue, data[i]);
    }

    self->diag.rx_data_bytes += length;
}

static void fc41d_parser_flush_raw(fc41d_core_t *self) {
    fc41d_parser_t *p = &self->parser;

    if(p->raw_fill > 0U) {
        fc41d_parser_deliver(self, FC41D_DATA_TCP, p->raw_socket, p->raw_buffer, p->raw_fill);
        p->raw_fill = 0U;
    }
}

// The next `length` bytes are payload of `socket`.
static void fc41d_parser_start_raw(fc41d_core_t *self, uint32_t socket, uint32_t length) {
    fc41d_parser_t *p = &self->parser;

    if(length == 0UL || length > FC41D_RAW_MAX_LENGTH) {
        return;                                             // nothing to collect, or not believable
    }

    p->mode          = FC41D_PARSER_RAW;
    p->raw_remaining = (uint16_t)length;
    p->raw_socket    = (uint16_t)socket;
    p->raw_fill      = 0U;
}

// +QSTASTAT:<event>
static void fc41d_parser_wifi_status(fc41d_core_t *self, const char *event) {
    fc41d_net_status_t *net = &self->net;

    if(fc41d_str_equal(event, "WLAN_CONNECTED")) {
        net->sta_associated = true;
        if(!self->setup.config.is_dhcp_enabled) {
            net->sta_ever_up = true;                        // a static IP never produces GOT_IP
        }
    }
    else if(fc41d_str_equal(event, "GOT_IP")) {
        net->sta_got_ip     = true;
        net->sta_associated = true;
        net->sta_ever_up    = true;
    }
    else if(fc41d_str_equal(event, "WLAN_DISCONNECTED") || fc41d_str_equal(event, "SCAN_NO_AP")) {
        // While the module is still trying to connect it repeats these every few
        // seconds. Only the first one after a working link means "link lost".
        if(net->sta_associated || net->sta_got_ip) {
            net->sta_associated = false;
            net->sta_got_ip     = false;
            net->sta_link_lost  = true;
        }
    }
}

// +QBLESTAT:<state>
static void fc41d_parser_ble_status(fc41d_core_t *self, const char *state) {
    fc41d_net_status_t *net = &self->net;

    if(fc41d_str_equal(state, "CONNECTED")) {
        net->ble_connected     = true;
        net->ble_activity_tick = self->timer.task;          // a phone just arrived: BLE is in use
        return;
    }

    net->ble_connected = false;                             // DISCONNECTED, ADVERTISING, INIT or NOINIT
    if(fc41d_str_equal(state, "NOINIT") && self->communication_type.ble_gatt) {
        net->ble_reinit = true;                             // advertising is gone: build it again
    }
}

// +QBLEMTU:<mtu>
static void fc41d_parser_ble_mtu(fc41d_core_t *self, const char *args) {
    uint32_t mtu;

    if(fc41d_str_parse_uint(&args, &mtu) && mtu >= 23UL && mtu <= 517UL) {
        self->net.ble_mtu = (uint16_t)mtu;
    }
}

// +QIOPEN: <id>,<error>   (the result of AT+QIOPEN)
static void fc41d_parser_tcp_open(fc41d_core_t *self, const char *args) {
    fc41d_parser_t *p = &self->parser;
    uint32_t        id;
    uint32_t        error;

    if(!fc41d_str_parse_uint(&args, &id)) {
        return;
    }
    if(*args == ',') {
        args++;
    }
    if(!fc41d_str_parse_uint(&args, &error)) {
        return;
    }

    p->qiopen_seen  = true;
    p->qiopen_id    = (uint16_t)id;
    p->qiopen_error = (uint16_t)error;

    if(error == 0UL && id != FC41D_TCP_LISTENER_ID) {
        self->net.tcp_open_mask |= fc41d_socket_bit(id);
    }
}

static void fc41d_net_client_add(fc41d_net_status_t *net, uint32_t id, const uint16_t *ip, uint32_t port) {
    uint16_t i;
    uint16_t slot = FC41D_TCP_MAX_CLIENTS;

    for(i = 0U; i < FC41D_TCP_MAX_CLIENTS; i++) {
        if(net->clients[i].in_use && net->clients[i].id == (uint8_t)id) {
            slot = i;                                       // the same id again: refresh
            break;
        }
        if(!net->clients[i].in_use && slot == FC41D_TCP_MAX_CLIENTS) {
            slot = i;
        }
    }

    if(slot < FC41D_TCP_MAX_CLIENTS) {
        for(i = 0U; i < 4U; i++) {
            net->clients[slot].ip[i] = ip[i];
        }
        net->clients[slot].port   = (uint16_t)port;
        net->clients[slot].id     = (uint8_t)id;
        net->clients[slot].in_use = true;
    }
}

static void fc41d_net_client_remove(fc41d_net_status_t *net, uint32_t id) {
    uint16_t i;

    for(i = 0U; i < FC41D_TCP_MAX_CLIENTS; i++) {
        if(net->clients[i].in_use && net->clients[i].id == (uint8_t)id) {
            net->clients[i].in_use = false;
        }
    }
}

// +QIURC: "incoming",<id>,<listener>,"<ip>",<port>
static void fc41d_parser_tcp_incoming(fc41d_core_t *self, const char *args) {
    uint16_t ip[4] = {0U, 0U, 0U, 0U};
    uint32_t id;
    uint32_t listener;
    uint32_t value;
    uint32_t port = 0UL;
    uint16_t i;

    if(!fc41d_str_parse_uint(&args, &id)) {
        return;
    }
    if(*args == ',') { args++; }
    if(!fc41d_str_parse_uint(&args, &listener)) {
        return;
    }
    if(*args == ',') { args++; }
    if(*args == '"') { args++; }
    for(i = 0U; i < 4U; i++) {
        if(fc41d_str_parse_uint(&args, &value)) {
            ip[i] = (uint16_t)value;
        }
        if(i < 3U && *args == '.') { args++; }
    }
    if(*args == '"') { args++; }
    if(*args == ',') { args++; }
    (void)fc41d_str_parse_uint(&args, &port);

    fc41d_net_client_add(&self->net, id, ip, port);
    self->net.tcp_open_mask |= fc41d_socket_bit(id);
}

// +QIURC: "recv",<id>[,<len>]   "closed",<id>   "incoming",...
static void fc41d_parser_tcp_event(fc41d_core_t *self, const char *args) {
    char     word[16];
    uint16_t w = 0U;
    uint32_t id;
    uint32_t length;

    while(*args == ' ') {
        args++;
    }
    if(*args != '"') {
        return;
    }
    args++;

    while(*args != '\0' && *args != '"' && w < 15U) {      // the quoted word
        word[w] = *args;
        w++;
        args++;
    }
    word[w] = '\0';
    if(*args == '"') {
        args++;
    }
    if(*args == ',') {
        args++;
    }

    if(fc41d_str_equal(word, "recv")) {
        if(!fc41d_str_parse_uint(&args, &id)) {
            return;
        }
        if(*args == ',') {                                  // direct push: the data follows
            args++;
            if(fc41d_str_parse_uint(&args, &length)) {
                fc41d_parser_start_raw(self, id, length);
            }
        }
        else {                                              // buffer mode: data is waiting
            self->net.tcp_rx_mask |= fc41d_socket_bit(id);
        }
    }
    else if(fc41d_str_equal(word, "closed")) {
        if(!fc41d_str_parse_uint(&args, &id)) {
            return;
        }
        self->net.tcp_open_mask   &= (uint16_t)~fc41d_socket_bit(id);
        self->net.tcp_rx_mask     &= (uint16_t)~fc41d_socket_bit(id);
        self->net.tcp_closed_mask |= fc41d_socket_bit(id);
        fc41d_net_client_remove(&self->net, id);
        if(id == FC41D_TCP_LISTENER_ID) {
            self->net.listener_open = false;
        }
    }
    else if(fc41d_str_equal(word, "incoming")) {
        fc41d_parser_tcp_incoming(self, args);
    }
}

// +QIRD: <len>   the answer of AT+QIRD, <len> bytes of data follow
// +QIRD: <a>,<b>,<c>   the statistics form, no data
static void fc41d_parser_tcp_read(fc41d_core_t *self, const char *args) {
    uint32_t length;

    if(!fc41d_str_parse_uint(&args, &length)) {
        return;
    }
    if(*args == ',') {
        return;
    }

    self->parser.last_qird_len = (uint16_t)length;
    fc41d_parser_start_raw(self, self->parser.qird_socket, length);
}

static void fc41d_parser_version(fc41d_core_t *self, const char *text) {
    uint16_t i = 0U;

    while(*text == ' ') {
        text++;
    }
    while(*text != '\0' && i < (FC41D_VERSION_MAX - 1U)) {
        self->parser.version[i] = *text;
        i++;
        text++;
    }
    self->parser.version[i] = '\0';
}

static uint16_t fc41d_hex_value(char c) {
    if(c >= '0' && c <= '9') { return (uint16_t)(c - '0'); }
    if(c >= 'a' && c <= 'f') { return (uint16_t)(c - 'a' + 10); }
    if(c >= 'A' && c <= 'F') { return (uint16_t)(c - 'A' + 10); }
    return 0xFFFFU;
}

// xx:xx:xx:xx:xx:xx
static void fc41d_parser_mac(fc41d_core_t *self, const char *text) {
    uint16_t mac[6];
    uint16_t i;

    while(*text == ' ') {
        text++;
    }

    for(i = 0U; i < 6U; i++) {
        uint16_t high = fc41d_hex_value(text[0]);
        uint16_t low  = fc41d_hex_value(text[1]);

        if(high == 0xFFFFU || low == 0xFFFFU) {
            return;                                         // not a MAC address
        }
        mac[i] = (uint16_t)((high << 4) | low);
        text += 2;
        if(i < 5U) {
            if(*text != ':' && *text != '-') {
                return;
            }
            text++;
        }
    }

    for(i = 0U; i < 6U; i++) {
        self->setup.config.mac_address[i] = mac[i];
    }
}

typedef void (*fc41d_urc_handler_t)(fc41d_core_t *self, const char *args);

typedef struct {
    const char          *prefix;
    fc41d_urc_handler_t  handler;        // NULL = a known line that needs no action
}fc41d_urc_t;

// The leading '+' is optional: the old firmware sent "QSTASTAT:..." without it.
static const fc41d_urc_t fc41d_urc_table[] = {
    { "QSTASTAT:",      fc41d_parser_wifi_status },
    { "QBLESTAT:",      fc41d_parser_ble_status  },
    { "QBLEMTU:",       fc41d_parser_ble_mtu     },
    { "QIOPEN:",        fc41d_parser_tcp_open    },
    { "QIURC:",         fc41d_parser_tcp_event   },
    { "QIRD:",          fc41d_parser_tcp_read    },
    { "QVERSION:",      fc41d_parser_version     },
    { "FC41D_VERSION:", fc41d_parser_version     },
    { "QWLMAC:",        fc41d_parser_mac         },
    { "WMAC:",          fc41d_parser_mac         },
    { "QISEND:",        NULL },                  // answers to commands we send, nothing in them we need
    { "QISTATE:",       NULL },
    { "QIGETERROR:",    NULL },
    { "QICFG:",         NULL },
    { "QIACCEPT:",      NULL },
    { "QBLEADDR:",      NULL },
    { "QBLENAME:",      NULL },
    { "QGETIP:",        NULL },
    { "QGETWIFISTATE:", NULL },
    { "STATION_",       NULL },
    { "SOFTAP_",        NULL },
    { "WSCAN:",         NULL }
};

static bool fc41d_parser_handle_urc(fc41d_core_t *self, const char *text) {
    uint16_t i;

    for(i = 0U; i < (sizeof(fc41d_urc_table) / sizeof(fc41d_urc_table[0])); i++) {
        if(fc41d_str_starts_with(text, fc41d_urc_table[i].prefix)) {
            if(fc41d_urc_table[i].handler != NULL) {
                fc41d_urc_table[i].handler(self, text + fc41d_str_length(fc41d_urc_table[i].prefix));
            }
            return true;
        }
    }

    return false;
}

/* ======================================================================= */
/*  4. The reader                                                            */
/* ======================================================================= */
static bool fc41d_parser_matches(const char *line, const char *expected) {
    if(fc41d_str_equal(expected, "OK") || fc41d_str_equal(expected, "ERROR")) {
        return fc41d_str_equal(line, expected);             // the whole line: an SSID may contain "OK"
    }

    return fc41d_str_starts_with(line, expected);
}

// Is this line the answer the command in flight is waiting for?
static void fc41d_parser_check_response(fc41d_parser_t *p, const char *line) {
    if(p->expect == NULL || p->result != FC41D_RESULT_NONE) {
        return;
    }

    if(fc41d_parser_matches(line, p->expect)) {
        p->result = FC41D_RESULT_MATCH;
    }
    else if(fc41d_str_equal(line, "ERROR")) {
        p->result = FC41D_RESULT_ERROR;
    }
}

// A line that no table entry knew.
// A phone's text has no line end, so an URC that follows it closely lands in the
// same line: "hello+QIURC: "recv",8". Returns where such an URC begins (0 = none).
static uint16_t fc41d_parser_urc_start(const char *line) {
    static const char *const known[] = { "+QIURC:", "+QSTASTAT:", "+QBLESTAT:", "+QBLEMTU:" };
    uint16_t                 i;
    uint16_t                 k;

    for(i = 1U; line[i] != '\0'; i++) {
        if(line[i] != '+') {
            continue;
        }
        for(k = 0U; k < (sizeof(known) / sizeof(known[0])); k++) {
            if(fc41d_str_starts_with(&line[i], known[k])) {
                return i;
            }
        }
    }

    return 0U;
}

static void fc41d_parser_handle_other(fc41d_core_t *self, const char *line) {
    uint16_t urc_at;

    if(line[0] == '+' || !self->net.ble_connected) {
        self->diag.unknown_lines++;                         // an URC we do not use / noise
        return;
    }

    // Without a "+" it is the text a phone wrote to our characteristic.
    urc_at = fc41d_parser_urc_start(line);
    if(urc_at > 0U) {
        fc41d_parser_deliver(self, FC41D_DATA_BLE, 0U, line, urc_at);
        (void)fc41d_parser_handle_urc(self, &line[urc_at + 1U]);
        return;
    }

    fc41d_parser_deliver(self, FC41D_DATA_BLE, 0U, line, fc41d_str_length(line));
}

static void fc41d_parser_handle_line(fc41d_core_t *self) {
    fc41d_parser_t *p    = &self->parser;
    const char     *line = p->line;

    p->line[p->line_length] = '\0';
    p->line_length = 0U;

    if(fc41d_str_starts_with(line, "CMDRSP:")) {            // wrapper of the 2021 firmware
        line += 7;
    }
    if(line[0] == '\0') {
        return;
    }
    self->diag.lines++;

    fc41d_parser_check_response(p, line);

    if(fc41d_str_equal(line, "OK") || fc41d_str_equal(line, "ERROR")) {
        return;
    }
    if(fc41d_str_equal(line, "ready")) {
        if(self->is_module_ready) {
            self->net.module_rebooted = true;               // a reset we did not ask for
        }
        return;
    }
    if(self->setup.config.is_echo_enabled && fc41d_str_starts_with(line, "AT")) {
        return;                                             // our own command coming back
    }

    if(!fc41d_parser_handle_urc(self, (line[0] == '+') ? (line + 1) : line)) {
        fc41d_parser_handle_other(self, line);
    }
}

static void fc41d_parser_feed(fc41d_core_t *self, char c) {
    fc41d_parser_t *p = &self->parser;

    fc41d_log_add(self->diag.rx_log, &self->diag.rx_log_pos, c);
    p->last_byte_tick = self->timer.task;

    if(p->mode == FC41D_PARSER_RAW) {
        p->raw_buffer[p->raw_fill] = c;
        p->raw_fill++;
        p->raw_remaining--;

        if(p->raw_fill >= FC41D_RAW_CHUNK || p->raw_remaining == 0U) {
            fc41d_parser_flush_raw(self);
        }
        if(p->raw_remaining == 0U) {
            p->mode = FC41D_PARSER_LINE;
        }
        return;
    }

    if(c == '\n') {
        if(p->line_length > 0U) {
            fc41d_parser_handle_line(self);
        }
    }
    else if(c == '\r') {
        /* skipped */
    }
    else if(p->line_length < (FC41D_LINE_MAX - 1U)) {
        p->line[p->line_length] = c;
        p->line_length++;
    }
    else if(p->line[0] != '+') {
        // A long text line: pass on what we have and go on collecting.
        p->line[p->line_length] = '\0';
        if(self->net.ble_connected) {
            fc41d_parser_deliver(self, FC41D_DATA_BLE, 0U, p->line, p->line_length);
        }
        p->line[0]     = c;
        p->line_length = 1U;
    }
    /* a too long "+" line keeps its first FC41D_LINE_MAX - 1 characters */
}

void fc41d_parser_reset(fc41d_core_t *self) {
    memset(&self->parser, 0, sizeof(self->parser));
    self->parser.mode   = FC41D_PARSER_LINE;
    self->parser.result = FC41D_RESULT_NONE;
    self->parser.last_byte_tick = self->timer.task;
}

void fc41d_parser_expect(fc41d_core_t *self, const char *expected_response) {
    self->parser.expect = expected_response;
    self->parser.result = FC41D_RESULT_NONE;
}

// Runs everything in the receive queue through the reader, then cleans up a
// line or payload that stopped in the middle.
void fc41d_parser_service(fc41d_core_t *self) {
    fc41d_parser_t *p = &self->parser;
    char            chunk[32];
    size_t          got;
    size_t          i;
    uint32_t        idle;

    while(fc41d_queue_count(&self->receive_queue) > 0U) {
        uint16_t irq = FC41D_CRITICAL_ENTER();
        got = 0U;
        fc41d_queue_pull_multiple(&self->receive_queue, chunk, sizeof(chunk), &got);
        FC41D_CRITICAL_EXIT(irq);

        for(i = 0U; i < got; i++) {
            fc41d_parser_feed(self, chunk[i]);
        }
    }

    idle = FC41D_ELAPSED_MS(self, p->last_byte_tick);

    if(p->mode == FC41D_PARSER_RAW) {
        if(idle >= FC41D_PARSER_STUCK_MS) {                 // the module never sent the rest
            fc41d_parser_flush_raw(self);
            p->mode          = FC41D_PARSER_LINE;
            p->raw_remaining = 0U;
        }
        return;
    }

    if(p->line_length == 0U) {
        return;
    }

    // Text from a BLE central has no line end. When it stopped coming it is
    // complete, unless it is only the beginning of OK / ERROR / ready.
    p->line[p->line_length] = '\0';
    if(p->line[0] != '+' && idle >= FC41D_PARSER_IDLE_FLUSH_MS &&
       !fc41d_str_starts_with("OK", p->line) &&
       !fc41d_str_starts_with("ERROR", p->line) &&
       !fc41d_str_starts_with("ready", p->line) &&
       !fc41d_str_starts_with("CMDRSP:", p->line)) {
        fc41d_parser_handle_line(self);
    }
    else if(idle >= FC41D_PARSER_STUCK_MS) {
        p->line_length = 0U;                                // give up on a stuck line
    }
}
