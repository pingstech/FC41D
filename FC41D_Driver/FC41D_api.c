/*
 * File     : FC41D_api.c
 * Brief    : Implementation of the public API. Owns the driver instance g_fc41d_api (placed in the debug_data section).
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-29
 *
 * Revisions
 *   R1  2026-09-29  First complete version
 *   R2  2026-10-02  TI C28x fixes: 16 bit char types, project-root-relative includes
 *   R3  2026-10-05  Readability rewrite, simpler API (settings table, no text copy helpers, fc41d_format)
 *   R4  2026-10-05  Message receiving and run-time Wi-Fi / TCP customisation with value checks
 *   R5  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

#include "FC41D_Driver/FC41D_api.h"
#include "FC41D_Driver/INC/F2800157_port.h"

#include <string.h>

// About 1.4 k words of queues and buffers: RAMM1D (0x240..0x7F8) is full, RAMLS0D is not.
// That section is not cleared at start-up; fc41d_api_initialize clears the instance.
#if defined(__TI_COMPILER_VERSION__)
#pragma DATA_SECTION(g_fc41d_api, "debug_data");
#endif
fc41d_api_t g_fc41d_api;

fc41d_api_return_e fc41d_api_initialize(fc41d_api_t *self, uint32_t sci_base, const fc41d_api_settings_t *settings)
{
    if(self == NULL || settings == NULL) {
        return FC41D_API_NULL_POINTER;
    }

    if(sci_base == 0U) {
        return FC41D_API_ERROR;
    }

    memset(self, 0, sizeof(*self));
    fc41d_port_init(sci_base);

    self->core.callback.fc41d_hardware_transmit_function = fc41d_port_transmit;
    self->core.callback.fc41d_hardware_receive_function  = fc41d_port_receive;
    self->core.callback.fc41d_set_IO_pin_HIGH_function   = settings->enable_pin_high;
    self->core.callback.fc41d_set_IO_pin_LOW_function    = settings->enable_pin_low;

    self->core.setup                   = settings->setup;
    self->core.communication_type.type = settings->type;

    if(fc41d_core_initialize(&self->core) != FC41D_OK) {
        return FC41D_API_INVALID_CALLBACK_POINTER;
    }

    return FC41D_API_OK;
}

fc41d_api_return_e fc41d_api_1ms_timer(fc41d_api_t *self)
{
    if(self == NULL) {
        return FC41D_API_NULL_POINTER;
    }

    (void)fc41d_core_1ms_timer(&self->core);

    return FC41D_API_OK;
}

fc41d_api_return_e fc41d_api_polling(fc41d_api_t *self)
{
    if(self == NULL) {
        return FC41D_API_NULL_POINTER;
    }

    (void)fc41d_core_polling(&self->core);

    return FC41D_API_OK;
}

bool fc41d_api_is_ready(const fc41d_api_t *self)
{
    return (self != NULL) && fc41d_core_is_ready(&self->core);
}

uint16_t fc41d_api_ble_send(fc41d_api_t *self, const char *data, uint16_t length)
{
    if(self == NULL) {
        return 0U;
    }

    return fc41d_core_send(&self->core, FC41D_DATA_BLE, 0U, data, length);
}

uint16_t fc41d_api_tcp_send(fc41d_api_t *self, uint16_t socket_id, const char *data, uint16_t length)
{
    if(self == NULL) {
        return 0U;
    }

    return fc41d_core_send(&self->core, FC41D_DATA_TCP, socket_id, data, length);
}

fc41d_message_source_e fc41d_get_message(fc41d_api_t *self, char *buffer, uint16_t *length)
{
    fc41d_data_source_e source;
    uint16_t            id;
    uint16_t            count;

    if(self == NULL || buffer == NULL || length == NULL) {
        return FC41D_MESSAGE_NONE;
    }

    *length = 0U;
    count = fc41d_core_pull_message(&self->core, &source, &id, buffer, FC41D_MESSAGE_MAX);
    if(count == 0U) {
        return FC41D_MESSAGE_NONE;
    }

    *length = count;
    if(source == FC41D_DATA_BLE) {
        return FC41D_MESSAGE_BLE;
    }

    self->message_socket_id = id;

    return (self->core.communication_type.type & FEAT_TCP_SERVER) ? FC41D_MESSAGE_TCP_SERVER : FC41D_MESSAGE_TCP_CLIENT;
}

// Letters, digits, spaces and the like; a control character would end the AT command line.
static bool fc41d_api_text_is_plain(const char *text, uint16_t length)
{
    uint16_t i;

    for(i = 0U; i < length; i++) {
        if((uint16_t)text[i] < 0x20U || (uint16_t)text[i] > 0x7EU) {
            return false;
        }
    }

    return true;
}

fc41d_api_return_e fc41d_customize_wifi_settings(fc41d_api_t *self, const char *ssid, const char *password)
{
    fc41d_core_t *core;
    char         *ssid_field;
    char         *password_field;
    uint16_t      ssid_size;
    uint16_t      password_size;
    uint16_t      ssid_length;
    uint16_t      password_length;
    bool          is_access_point;

    if(self == NULL || ssid == NULL) {
        return FC41D_API_NULL_POINTER;
    }
    if(password == NULL) {
        password = "";
    }

    core = &self->core;
    if(core->communication_type.type & FEAT_WIFI_STA) {
        ssid_field      = core->setup.wi_fi_sta_config.ssid;
        password_field  = core->setup.wi_fi_sta_config.password;
        ssid_size       = (uint16_t)sizeof(core->setup.wi_fi_sta_config.ssid);
        password_size   = (uint16_t)sizeof(core->setup.wi_fi_sta_config.password);
        is_access_point = false;
    }
    else if(core->communication_type.type & FEAT_WIFI_AP) {
        ssid_field      = core->setup.wi_fi_ap_config.ssid;
        password_field  = core->setup.wi_fi_ap_config.password;
        ssid_size       = (uint16_t)sizeof(core->setup.wi_fi_ap_config.ssid);
        password_size   = (uint16_t)sizeof(core->setup.wi_fi_ap_config.password);
        is_access_point = true;
    }
    else {
        return FC41D_API_NOT_SUPPORTED;
    }

    ssid_length     = fc41d_str_length(ssid);
    password_length = fc41d_str_length(password);

    if(ssid_length == 0U || ssid_length >= ssid_size || password_length >= password_size ||
       !fc41d_api_text_is_plain(ssid, ssid_length) || !fc41d_api_text_is_plain(password, password_length)) {
        return FC41D_API_INVALID_VALUE;
    }
    if(is_access_point && password_length != 0U && password_length < 8U) {
        return FC41D_API_INVALID_VALUE;                 // WPA needs at least 8 characters
    }

    if(fc41d_str_equal(ssid, ssid_field) && fc41d_str_equal(password, password_field)) {
        return FC41D_API_OK;                            // nothing changes: do not disturb the link
    }

    memcpy(ssid_field, ssid, ssid_length + 1U);
    memcpy(password_field, password, password_length + 1U);

    if(!is_access_point) {
        core->defaults_written = false;                 // the copy in the module's flash is renewed at the next bring-up
    }
    core->net.wifi_changed = true;

    return FC41D_API_OK;
}

fc41d_api_return_e fc41d_customize_tcp_settings(fc41d_api_t *self, const uint8_t *ip_address, uint16_t port)
{
    fc41d_core_t *core;
    uint16_t      i;

    if(self == NULL) {
        return FC41D_API_NULL_POINTER;
    }

    core = &self->core;
    if(core->communication_type.type & FEAT_TCP_CLIENT) {
        fc41d_tcp_client_config_t *client = &core->setup.tcp_client_config;
        uint16_t                   sum    = 0U;

        if(ip_address == NULL) {
            return FC41D_API_NULL_POINTER;
        }
        for(i = 0U; i < 4U; i++) {
            if(ip_address[i] > 255U) {
                return FC41D_API_INVALID_VALUE;
            }
            sum = (uint16_t)(sum + ip_address[i]);
        }
        if(sum == 0U || port == 0U) {                   // 0.0.0.0 and port 0 are not a server
            return FC41D_API_INVALID_VALUE;
        }

        if(client->port == port && memcmp(client->ip_address, ip_address, sizeof(client->ip_address)) == 0) {
            return FC41D_API_OK;
        }
        for(i = 0U; i < 4U; i++) {
            client->ip_address[i] = ip_address[i];
        }
        client->port = port;
    }
    else if(core->communication_type.type & FEAT_TCP_SERVER) {
        if(port == 0U) {
            return FC41D_API_INVALID_VALUE;
        }
        if(core->setup.tcp_server_config.port == port) {
            return FC41D_API_OK;
        }
        core->setup.tcp_server_config.port = port;
    }
    else {
        return FC41D_API_NOT_SUPPORTED;
    }

    core->net.tcp_changed = true;

    return FC41D_API_OK;
}

#ifdef FC41D_RX_FROM_ISR
fc41d_api_return_e fc41d_api_rx_service(fc41d_api_t *self)
{
    if(self == NULL) {
        return FC41D_API_NULL_POINTER;
    }

    fc41d_core_rx_service(&self->core);

    return FC41D_API_OK;
}
#endif
