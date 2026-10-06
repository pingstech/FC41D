/*
 * File     : FC41D_wi-fi.c
 * Brief    : Wi-Fi station and SoftAP state machines.
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
 *  Station (join a network) and SoftAP (be a network). The core calls these
 *  again and again while its state is FC41D_WIFI_STA / FC41D_WIFI_AP; they say
 *  FC41D_OK when the feature is up.
 *
 *  What the station code relies on (see FC41D_wireless_module.md):
 *   - AT+QSTAAPINFO answers OK at once. The real result is the URC
 *     +QSTASTAT:WLAN_CONNECTED and then GOT_IP. A wrong password produces NO
 *     message at all, so the wait has its own timeout.
 *   - With a static IP there is no GOT_IP: WLAN_CONNECTED is the end.
 *   - After a lost link the module reconnects by itself. Sending
 *     AT+QSTAAPINFO again would restart its attempt, so the driver only waits.
 */

#include "FC41D_Driver/INC/FC41D_core.h"

#define FC41D_WIFI_STEP_PAUSE_MS  (100U)
#define FC41D_WIFI_RETRY_PAUSE_MS (3000U)    // between two connect attempts

/* ======================================================================= */
/*  Station                                                                  */
/* ======================================================================= */
static void fc41d_sta_next(fc41d_core_t *self, fc41d_wi_fi_sta_state_e next_step, uint32_t pause_ms) {
    self->step.wifi_sta = next_step;

    if(pause_ms > 0UL) {
        fc41d_core_start_delay(self, FC41D_WIFI_STA, pause_ms);
    }
}

// A command that must work: FC41D_HARD_FAIL if it does not.
static fc41d_return_e fc41d_sta_run(fc41d_core_t *self, const char *command, fc41d_wi_fi_sta_state_e next_step) {
    fc41d_return_e ret = fc41d_core_command(self, command, "OK", FC41D_WAIT_QUICK);

    if(ret == FC41D_OK) {
        fc41d_sta_next(self, next_step, FC41D_WIFI_STEP_PAUSE_MS);
        return FC41D_IN_PROGRESS;
    }

    return ret;
}

// A command that may fail: the sequence goes on either way.
static fc41d_return_e fc41d_sta_try(fc41d_core_t *self, const char *command, fc41d_wi_fi_sta_state_e next_step, uint32_t pause_ms) {
    fc41d_return_e ret = fc41d_core_try_command(self, command, "OK", FC41D_WAIT_ONCE);

    if(ret == FC41D_OK) {
        fc41d_sta_next(self, next_step, pause_ms);
        return FC41D_IN_PROGRESS;
    }

    return ret;
}

// Is the station connected (and, with DHCP, does it have an address)?
static bool fc41d_sta_link_is_up(fc41d_core_t *self) {
    return self->setup.config.is_dhcp_enabled ? self->net.sta_got_ip : self->net.sta_associated;
}

// Waits for the link. After a first success the module retries by itself for
// as long as it takes, so only the very first connect has a time limit.
static fc41d_return_e fc41d_sta_wait_for_link(fc41d_core_t *self) {
    fc41d_net_status_t *net = &self->net;

    if(fc41d_sta_link_is_up(self)) {
        self->communication_type.wifi_sta = true;
        self->step.wifi_sta = FC41D_QWSCAN;
        self->step.attempts = 0U;
        return FC41D_OK;
    }

    if(net->sta_ever_up || FC41D_ELAPSED_MS(self, self->step.wait_start) <= FC41D_WIFI_CONNECT_TIMEOUT_MS) {
        return FC41D_IN_PROGRESS;
    }

    // Silence for too long: wrong password or no such network.
    self->step.attempts++;
    if(self->step.attempts >= FC41D_WIFI_CONNECT_ATTEMPTS) {
        self->step.attempts = 0U;
        self->step.wifi_sta = FC41D_QWSCAN;
        return FC41D_HARD_FAIL;                         // give up: the core power cycles the module
    }

    net->sta_configured = false;
    fc41d_sta_next(self, FC41D_QSTASTOP, 0UL);          // stop, wait a little, try again

    return FC41D_IN_PROGRESS;
}

fc41d_return_e fc41d_wifi_sta_state_machine(fc41d_core_t *self) {
    const fc41d_wi_fi_sta_config_t *sta  = &self->setup.wi_fi_sta_config;
    bool                            dhcp = self->setup.config.is_dhcp_enabled;
    fc41d_return_e                  ret  = FC41D_IN_PROGRESS;
    const char                     *command;

    switch(self->step.wifi_sta) {
        case FC41D_QWSCAN: {
            fc41d_sta_next(self, FC41D_QSTADHCP, 0UL);                      // a scan is not needed to connect
        }
        break;

        case FC41D_QSTADHCP: {
            ret = fc41d_sta_try(self, dhcp ? "AT+QSTADHCP=1\r\n" : "AT+QSTADHCP=0\r\n",
                                dhcp ? FC41D_QSTAAPINFO : FC41D_QSTASTATIC, FC41D_WIFI_STEP_PAUSE_MS);
        }
        break;

        case FC41D_QSTASTATIC: {
            const fc41d_wi_fi_static_ip_config_t *ip = &self->setup.static_ip_config;

            command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QSTASTATIC=%i,%i,%i,%i\r\n",
                                   ip->ip_address, ip->subnet_mask, ip->gateway, ip->dns_server);
            if(command == NULL) {
                return FC41D_HARD_FAIL;
            }
            ret = fc41d_sta_run(self, command, FC41D_QSTAAPINFO);
        }
        break;

        case FC41D_QSTAAPINFO: {
            if(self->net.sta_configured) {                                  // reconnecting: the module does it
                fc41d_sta_next(self, FC41D_QSTAWAIT, 0UL);
                break;
            }

            command = fc41d_core_credentials_command(self, "AT+QSTAAPINFO=", sta->ssid, sta->password);
            if(sta->ssid[0] == '\0' || command == NULL) {
                return FC41D_HARD_FAIL;                                     // nothing (valid) to connect to
            }

            ret = fc41d_core_command(self, command, "OK", FC41D_WAIT_QUICK);
            if(ret == FC41D_OK) {                                           // OK only means "accepted"
                self->net.sta_configured = true;
                self->step.wait_start    = self->timer.task;
                fc41d_sta_next(self, FC41D_QSTAWAIT, 0UL);
                ret = FC41D_IN_PROGRESS;
            }
        }
        break;

        case FC41D_QSTAWAIT: {
            ret = fc41d_sta_wait_for_link(self);
        }
        break;

        case FC41D_QSTASTOP: {
            ret = fc41d_sta_try(self, "AT+QSTASTOP\r\n", FC41D_QSTAAPINFO, FC41D_WIFI_RETRY_PAUSE_MS);
        }
        break;

        default: {
            self->step.wifi_sta = FC41D_QWSCAN;
        }
        break;
    }

    return ret;
}

/* ======================================================================= */
/*  SoftAP                                                                   */
/* ======================================================================= */
static fc41d_return_e fc41d_ap_finished(fc41d_core_t *self) {
    self->net.ap_up = true;
    self->communication_type.wifi_ap = true;
    self->step.wifi_ap = FC41D_QSOFTAP;

    return FC41D_OK;
}

fc41d_return_e fc41d_wifi_ap_state_machine(fc41d_core_t *self) {
    const fc41d_wi_fi_ap_config_t *ap  = &self->setup.wi_fi_ap_config;
    fc41d_return_e                 ret = FC41D_IN_PROGRESS;
    const char                    *command;

    switch(self->step.wifi_ap) {
        case FC41D_QSOFTAP: {
            command = fc41d_core_credentials_command(self, "AT+QSOFTAP=", ap->ssid, ap->password);
            if(ap->ssid[0] == '\0' || command == NULL) {
                return FC41D_HARD_FAIL;                                     // no network name configured
            }

            ret = fc41d_core_command(self, command, "OK", FC41D_WAIT_AP_START);
            if(ret != FC41D_OK) {
                break;
            }

            if(!ap->use_static_ip) {
                return fc41d_ap_finished(self);
            }
            self->step.wifi_ap = FC41D_QAPSTATIC;
            fc41d_core_start_delay(self, FC41D_WIFI_AP, FC41D_WIFI_STEP_PAUSE_MS);
            ret = FC41D_IN_PROGRESS;
        }
        break;

        case FC41D_QAPSTATIC: {
            command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QAPSTATIC=%i,%i,%i,%i\r\n",
                                   ap->static_ip.ip_address, ap->static_ip.subnet_mask, ap->static_ip.gateway, ap->static_ip.dns_server);
            if(command == NULL) {
                return FC41D_HARD_FAIL;
            }

            ret = fc41d_core_command(self, command, "OK", FC41D_WAIT_QUICK);
            if(ret == FC41D_OK) {
                return fc41d_ap_finished(self);
            }
        }
        break;

        case FC41D_QSOFTAPSTOP: {                                           // only after the name / password changed at run time
            if(fc41d_core_try_command(self, "AT+QSOFTAPSTOP\r\n", "OK", FC41D_WAIT_ONCE) == FC41D_OK) {
                self->step.wifi_ap = FC41D_QSOFTAP;
                fc41d_core_start_delay(self, FC41D_WIFI_AP, FC41D_WIFI_STEP_PAUSE_MS);
            }
        }
        break;

        default: {
            self->step.wifi_ap = FC41D_QSOFTAP;
        }
        break;
    }

    return ret;
}
