/*
 * File     : FC41D_ble.c
 * Brief    : BLE GATT bring-up (service, characteristic, advertising) and notification sending.
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
 *  BLE GATT server: one service with one characteristic. A phone writes to the
 *  characteristic (the text arrives on the UART, the parser puts it into the
 *  message queue) and the module notifies the phone with AT+QBLEGATTSNTFY.
 *
 *  What the code relies on (see FC41D_wireless_module.md):
 *   - The service can be defined once per AT+QBLEINIT=2. A new one needs
 *     AT+QBLEDEINIT first (it answers ERROR on a fresh module, that is fine).
 *   - Firmware A16 reports +QBLESTAT:CONNECTED / DISCONNECTED and
 *     +QBLEMTU:<mtu> by itself and starts advertising again after a disconnect.
 *   - A notification carries at most MTU - 3 bytes, more is cut silently: the
 *     core splits the data before it gets here.
 */

#include "FC41D_Driver/INC/FC41D_core.h"

#define FC41D_BLE_STEP_PAUSE_MS (300U)

static void fc41d_ble_next(fc41d_core_t *self, fc41d_ble_gatt_state_e next_step) {
    self->step.ble = next_step;
    fc41d_core_start_delay(self, FC41D_BLE, FC41D_BLE_STEP_PAUSE_MS);
}

// A command that must work: FC41D_HARD_FAIL if it does not.
static fc41d_return_e fc41d_ble_run(fc41d_core_t *self, const char *command, fc41d_ble_gatt_state_e next_step) {
    fc41d_return_e ret = fc41d_core_command(self, command, "OK", FC41D_WAIT_SLOW);

    if(ret == FC41D_OK) {
        fc41d_ble_next(self, next_step);
        return FC41D_IN_PROGRESS;
    }

    return ret;
}

// A command that may fail: the sequence goes on either way.
static fc41d_return_e fc41d_ble_try(fc41d_core_t *self, const char *command, fc41d_ble_gatt_state_e next_step) {
    fc41d_return_e ret = fc41d_core_try_command(self, command, "OK", FC41D_WAIT_ONCE);

    if(ret == FC41D_OK) {
        fc41d_ble_next(self, next_step);
        return FC41D_IN_PROGRESS;
    }

    return ret;
}

fc41d_return_e fc41d_ble_gatt_state_machine(fc41d_core_t *self) {
    const fc41d_ble_gatt_config_t *ble = &self->setup.ble_gatt_config;
    fc41d_return_e                 ret = FC41D_IN_PROGRESS;
    const char                    *command;

    switch(self->step.ble) {
        case FC41D_QBLEDEINIT: {
            ret = fc41d_ble_try(self, "AT+QBLEDEINIT\r\n", FC41D_QBLEINIT);
        }
        break;

        case FC41D_QBLEINIT: {
            ret = fc41d_ble_run(self, "AT+QBLEINIT=2\r\n", FC41D_QBLENAME);
        }
        break;

        case FC41D_QBLENAME: {
            // The name is cosmetic: a refusal must not stop the bring-up.
            command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QBLENAME=%s\r\n", ble->device_name);

            if(ble->device_name[0] == '\0' || command == NULL) {
                self->step.ble = FC41D_QBLEGATTSSRV;                        // keep the default name
            }
            else {
                ret = fc41d_ble_try(self, command, FC41D_QBLEGATTSSRV);
            }
        }
        break;

        case FC41D_QBLEGATTSSRV: {
            command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QBLEGATTSSRV=%s\r\n", ble->service_uuid);

            if(ble->service_uuid[0] == '\0' || command == NULL) {
                return FC41D_HARD_FAIL;                                     // no service uuid configured
            }
            ret = fc41d_ble_run(self, command, FC41D_QBLEGATTSCHAR);
        }
        break;

        case FC41D_QBLEGATTSCHAR: {
            // FF is the property field that gave "write + notify" on the real module.
            command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QBLEGATTSCHAR=%s,FF\r\n", ble->characteristic_uuid);

            if(ble->characteristic_uuid[0] == '\0' || command == NULL) {
                return FC41D_HARD_FAIL;                                     // no characteristic uuid configured
            }
            ret = fc41d_ble_run(self, command, FC41D_QBLEADVSTART);
        }
        break;

        case FC41D_QBLEADVSTART: {
            ret = fc41d_core_command(self, "AT+QBLEADVSTART\r\n", "OK", FC41D_WAIT_SLOW);

            if(ret == FC41D_OK) {                                           // now a phone can see the module
                self->communication_type.ble_gatt = true;
                self->step.ble = FC41D_QBLEDEINIT;
                return FC41D_OK;
            }
        }
        break;

        default: {
            self->step.ble = FC41D_QBLEDEINIT;
        }
        break;
    }

    return ret;
}

// AT+QBLEGATTSNTFY=<characteristic uuid>,<text>
fc41d_return_e fc41d_ble_send_state_machine(fc41d_core_t *self) {
    const char *command;

    if(!self->net.ble_connected) {
        return FC41D_HARD_FAIL;                         // nobody to notify
    }

    command = fc41d_format(self->command_buffer, FC41D_COMMAND_MAX, "AT+QBLEGATTSNTFY=%s,%s\r\n",
                           self->setup.ble_gatt_config.characteristic_uuid, self->tx_job.data);
    if(command == NULL) {
        return FC41D_HARD_FAIL;
    }

    return fc41d_core_command(self, command, "OK", FC41D_WAIT_SLOW);
}
