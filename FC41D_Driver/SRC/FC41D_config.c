/*
 * File     : FC41D_config.c
 * Brief    : Module bring-up state machine: power, AT check, reset, identity, stored defaults.
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
 *  Brings the module up: power, wake-up check, reset, identity, stored
 *  defaults. The core calls it again and again while its state is
 *  FC41D_CONFIG; it says FC41D_OK when the last step is done.
 *
 *  Every step is one line: send a command, and when it is answered go to the
 *  next step after a short pause. "run" steps must succeed, "try" steps may
 *  fail (the information commands, for example).
 */

#include "FC41D_Driver/INC/FC41D_core.h"

#define FC41D_AFTER_RESET_MS (500U)    // let the module settle after "ready"
#define FC41D_STEP_PAUSE_MS  (100U)

static void fc41d_config_next(fc41d_core_t *self, fc41d_config_state_e next_state, uint32_t pause_ms) {
    self->setup.config.state = next_state;

    if(pause_ms > 0UL) {
        fc41d_core_start_delay(self, FC41D_CONFIG, pause_ms);
    }
}

// A command that must work: FC41D_HARD_FAIL if it does not.
static fc41d_return_e fc41d_config_run(fc41d_core_t *self, const char *command, const char *expected, fc41d_wait_t wait, fc41d_config_state_e next_state, uint32_t pause_ms) {
    fc41d_return_e ret = fc41d_core_command(self, command, expected, wait);

    if(ret == FC41D_OK) {
        fc41d_config_next(self, next_state, pause_ms);
        return FC41D_IN_PROGRESS;
    }

    return ret;
}

// A command that may fail: the sequence goes on either way.
static fc41d_return_e fc41d_config_try(fc41d_core_t *self, const char *command, const char *expected, fc41d_wait_t wait, fc41d_config_state_e next_state, uint32_t pause_ms) {
    fc41d_return_e ret = fc41d_core_try_command(self, command, expected, wait);

    if(ret == FC41D_OK) {
        fc41d_config_next(self, next_state, pause_ms);
        return FC41D_IN_PROGRESS;
    }

    return ret;
}

// AT+QSTAAPINFODEF=<ssid>,<password>, or NULL when it must not be stored.
// The module keeps it in flash, so it is written once per MCU start only.
static const char *fc41d_config_credentials_command(fc41d_core_t *self) {
    const fc41d_wi_fi_sta_config_t *sta = &self->setup.wi_fi_sta_config;

    if(self->defaults_written || !self->setup.config.auto_connect_to_ap) {
        return NULL;
    }
    if(!(self->communication_type.type & FEAT_WIFI_STA) || sta->ssid[0] == '\0') {
        return NULL;
    }

    return fc41d_core_credentials_command(self, "AT+QSTAAPINFODEF=", sta->ssid, sta->password);
}

fc41d_return_e fc41d_config_state_machine(fc41d_core_t *self) {
    fc41d_config_t *cfg = &self->setup.config;
    fc41d_return_e  ret = FC41D_IN_PROGRESS;

    switch(cfg->state) {
        case FC41D_POWER_ON: {
            if(self->callback.fc41d_set_IO_pin_HIGH_function != NULL) {
                self->callback.fc41d_set_IO_pin_HIGH_function();
            }
            fc41d_config_next(self, FC41D_AT, FC41D_POWER_ON_WAIT_MS);      // let the module boot
        }
        break;

        case FC41D_AT: {
            ret = fc41d_config_run(self, "AT\r\n", "OK", FC41D_WAIT_BOOT, FC41D_QRST, FC41D_STEP_PAUSE_MS);
        }
        break;

        case FC41D_QRST: {
            // OK comes at once, "ready" about a second later: only "ready" ends this step.
            ret = fc41d_config_run(self, "AT+QRST\r\n", "ready", FC41D_WAIT_RESET, FC41D_QVERSION, FC41D_AFTER_RESET_MS);
        }
        break;

        case FC41D_QVERSION: {
            ret = fc41d_config_try(self, "AT+QVERSION\r\n", "OK", FC41D_WAIT_QUICK, FC41D_QWLMAC, FC41D_STEP_PAUSE_MS);
        }
        break;

        case FC41D_QWLMAC: {
            ret = fc41d_config_try(self, "AT+QWLMAC\r\n", "OK", FC41D_WAIT_QUICK, FC41D_QECHO, FC41D_STEP_PAUSE_MS);
        }
        break;

        case FC41D_QECHO: {
            const char *command = cfg->is_echo_enabled ? "AT+QECHO=1\r\n" : "AT+QECHO=0\r\n";

            ret = fc41d_config_try(self, command, "OK", FC41D_WAIT_QUICK, FC41D_QSTAAPINFODEF, FC41D_STEP_PAUSE_MS);
        }
        break;

        case FC41D_QSTAAPINFODEF: {
            const char *command = fc41d_config_credentials_command(self);

            if(command == NULL) {
                fc41d_config_next(self, FC41D_QSTADHCPDEF, 0UL);            // nothing to store
            }
            else {
                ret = fc41d_config_try(self, command, "OK", FC41D_WAIT_QUICK, FC41D_QSTADHCPDEF, FC41D_STEP_PAUSE_MS);
            }
        }
        break;

        case FC41D_QSTADHCPDEF: {
            // Only useful together with the stored credentials.
            bool wanted = !self->defaults_written && cfg->auto_connect_to_ap && (self->communication_type.type & FEAT_WIFI_STA);

            if(!wanted) {
                cfg->state = FC41D_POWER_ON;                                // the bring-up is complete
                return FC41D_OK;
            }

            ret = fc41d_core_try_command(self, cfg->is_dhcp_enabled ? "AT+QSTADHCPDEF=1\r\n" : "AT+QSTADHCPDEF=0\r\n", "OK", FC41D_WAIT_QUICK);
            if(ret == FC41D_OK) {
                self->defaults_written = true;
                cfg->state = FC41D_POWER_ON;
                return FC41D_OK;
            }
        }
        break;

        default: {
            cfg->state = FC41D_POWER_ON;
        }
        break;
    }

    return ret;
}
