/*
 * File     : FC41D_bridge.c
 * Brief    : BLE <-> TCP test bridge implementation: forwards every message to the other side with a source tag.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-10-05
 *
 * Revisions
 *   R1  2026-10-05  First version
 *   R2  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  BLE <-> TCP test bridge, see FC41D_bridge.h.
 */

#include "FC41D_Driver/FC41D_bridge.h"

// About 200 words: RAMM1D is full, RAMLS0D is not (same trick as g_fc41d_api and g_ui).
#if defined(__TI_COMPILER_VERSION__)
#pragma DATA_SECTION(g_fc41d_bridge, "debug_data");
#endif
fc41d_bridge_t g_fc41d_bridge;

// The driver refuses CR / LF in the data, and a double quote on TCP (the text is written
// between quotes into the AT command). So: the line end that netcat, Hercules or telnet
// add is dropped, a CR / LF inside the text becomes a space and, for TCP, a double quote
// becomes an apostrophe. Returns the new length.
static uint16_t fc41d_bridge_clean(char *text, uint16_t length, bool for_tcp)
{
    uint16_t i;

    while(length > 0U && (text[length - 1U] == '\r' || text[length - 1U] == '\n')) {
        length--;
    }

    for(i = 0U; i < length; i++) {
        if(text[i] == '\r' || text[i] == '\n') {
            text[i] = ' ';
        }
        else if(for_tcp && text[i] == '"') {
            text[i] = '\'';
        }
    }
    text[length] = '\0';

    return length;
}

void fc41d_bridge_run(fc41d_api_t *api)
{
    fc41d_bridge_t        *bridge = &g_fc41d_bridge;
    fc41d_message_source_e source;
    const char            *line;
    uint16_t               length;
    uint16_t               sent;
    bool                   from_ble;

    source = fc41d_get_message(api, bridge->message, &length);
    if(source == FC41D_MESSAGE_NONE) {
        return;
    }

    from_ble = (source == FC41D_MESSAGE_BLE);

    bridge->received++;
    bridge->source = source;
    bridge->socket = api->message_socket_id;

    length = fc41d_bridge_clean(bridge->message, length, from_ble);     // BLE text is going to TCP
    if(length == 0U) {
        bridge->ignored++;
        return;
    }

    if(from_ble) {
        line = fc41d_format(bridge->out, (uint16_t)sizeof(bridge->out), "BLE: %s", bridge->message);
    }
    else {
        line = fc41d_format(bridge->out, (uint16_t)sizeof(bridge->out), "TCP%u: %s", bridge->socket, bridge->message);
    }
    if(line == NULL) {
        bridge->refused++;                                              // does not fit with its tag (cannot happen with FC41D_MESSAGE_MAX)
        return;
    }
    length = fc41d_str_length(line);

    if(!from_ble) {
        sent = fc41d_api_ble_send(api, line, length);
    }
    else if(api->core.communication_type.type & FEAT_TCP_SERVER) {
        sent = fc41d_api_tcp_send(api, FC41D_API_TCP_ALL_CLIENTS, line, length);   // every connected client
    }
    else {
        sent = fc41d_api_tcp_send(api, FC41D_TCP_CLIENT_SOCKET_ID, line, length);  // the server we connected to
    }

    if(sent != length) {
        bridge->refused++;                                              // 0 = the other side is down, less = the driver queue was full: the text is cut
    }
    else if(from_ble) {
        bridge->ble_to_tcp++;
    }
    else {
        bridge->tcp_to_ble++;
    }
}
