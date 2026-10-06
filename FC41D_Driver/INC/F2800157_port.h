/*
 * File     : F2800157_port.h
 * Brief    : UART port of the FC41D driver on the TMS320F2800157 SCI: init, transmit, receive.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-29
 *
 * Revisions
 *   R1  2026-09-29  First complete version
 *   R2  2026-10-05  Doxygen briefs (parameters, return values) for the critical functions
 *   R3  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

#ifndef FC41D_DRIVER_F2800157_PORT_H_
#define FC41D_DRIVER_F2800157_PORT_H_

#include <stdint.h>

/**
 * @brief Binds the port to a SCI base address. Call once, after HAL_setParams().
 * @param sci_base  Base address of the SCI (e.g. SCIB_BASE).
 */
void fc41d_port_init(uint32_t sci_base);

/**
 * @brief  Hands characters to the UART transmit FIFO. Never blocks.
 * @param  data    Characters to send.
 * @param  length  Number of characters offered.
 * @return Characters ACTUALLY taken, 0..length. Fewer than length is normal (the FIFO is
 *         16 deep): the caller advances by this count and continues on its next call.
 */
uint16_t fc41d_port_transmit(const char *data, uint16_t length);

/**
 * @brief  Copies what has arrived from the UART receive FIFO. Never blocks.
 * @param  data        Receives the characters.
 * @param  max_length  Size of data.
 * @return Characters copied. 0 means nothing was waiting, which is not an error.
 */
uint16_t fc41d_port_receive(char *data, uint16_t max_length);

/** Diagnostics for a CCS watch window. */
extern volatile uint32_t fc41dPortTxChars;
extern volatile uint32_t fc41dPortRxChars;
extern volatile uint32_t fc41dPortRxErrors;

#endif /* FC41D_DRIVER_F2800157_PORT_H_ */
