/*
 * File     : F2800157_port.c
 * Brief    : SCI FIFO based transmit / receive functions with counters for the CCS watch window.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-29
 *
 * Revisions
 *   R1  2026-09-29  First complete version
 *   R2  2026-10-02  TI C28x fixes: 16 bit char types, project-root-relative includes
 *   R3  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

#include <stddef.h>

#include "FC41D_Driver/INC/F2800157_port.h"
#include "sci.h"

static uint32_t g_base = 0U;

volatile uint32_t fc41dPortTxChars  = 0;
volatile uint32_t fc41dPortRxChars  = 0;
volatile uint32_t fc41dPortRxErrors = 0;

void fc41d_port_init(uint32_t sci_base)
{
    g_base            = sci_base;
    fc41dPortTxChars  = 0U;
    fc41dPortRxChars  = 0U;
    fc41dPortRxErrors = 0U;
}

uint16_t fc41d_port_transmit(const char *data, uint16_t length)
{
    uint16_t n = 0U;

    if((g_base == 0U) || (data == NULL))
    {
        return 0U;
    }

    while((n < length) && (SCI_getTxFIFOStatus(g_base) != SCI_FIFO_TX16))
    {
        SCI_writeCharNonBlocking(g_base, ((uint16_t)data[n]) & 0x00FFU);
        n++;
    }

    fc41dPortTxChars += n;

    return n;
}

uint16_t fc41d_port_receive(char *data, uint16_t max_length)
{
    uint16_t n = 0U;

    if((g_base == 0U) || (data == NULL))
    {
        return 0U;
    }

    // A framing, overrun or break error latches RXERROR and the receiver stops
    // handing over bytes until it is cleared. The only way to clear it is a
    // module-wide software reset, which would cut the byte in the transmit
    // shift register in half - so do it only while nothing is going out. The
    // reset leaves the baud rate and frame format untouched.
    if(((SCI_getRxStatus(g_base) & SCI_RXSTATUS_ERROR) != 0U) &&
       (SCI_getTxFIFOStatus(g_base) == SCI_FIFO_TX0) &&
       (SCI_isTransmitterBusy(g_base) == false))
    {
        fc41dPortRxErrors++;
        SCI_performSoftwareReset(g_base);
        SCI_resetRxFIFO(g_base);
    }

    while((n < max_length) && (SCI_getRxFIFOStatus(g_base) != SCI_FIFO_RX0))
    {
        data[n] = (char)(SCI_readCharNonBlocking(g_base) & 0x00FFU);
        n++;
    }

    fc41dPortRxChars += n;

    return n;
}
