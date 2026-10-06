/*
 * File     : FC41D_types.h
 * Brief    : Basic types shared by every FC41D header: stdint / stdbool, C28x uint8_t, critical-section macros.
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-10-02
 *
 * Revisions
 *   R1  2026-10-02  First version (uint8_t for the C28x through the driverlib hw_types.h)
 *   R2  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  Basic types for every FC41D header.
 *
 *  The C28x has no 8 bit type: TI's <stdint.h> does not define uint8_t, the
 *  project's driverlib "inc/hw_types.h" does (as a 16 bit type) and
 *  HOL_Queue.h needs it. Including it here makes each FC41D header usable on
 *  its own. On a PC build (unit tests) <stdint.h> already has uint8_t.
 */

#ifndef FC41D_DRIVER_INC_FC41D_TYPES_H_
#define FC41D_DRIVER_INC_FC41D_TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(__TMS320C28XX__)
#include "inc/hw_types.h"
#endif

// Optional: feed the receive queue from an interrupt (see FC41D_api.h,
// fc41d_api_rx_service). Define FC41D_RX_FROM_ISR in the project to turn it on;
// the main loop then touches the queue only inside a short critical section.
#if defined(__TMS320C28XX__) && defined(FC41D_RX_FROM_ISR)
// __disable_interrupts() / __enable_interrupts() are TI compiler intrinsics.
#define FC41D_CRITICAL_ENTER()            (__disable_interrupts() & 1U)    // 1 = were already disabled
#define FC41D_CRITICAL_EXIT(was_disabled) do { if((was_disabled) == 0U) { (void)__enable_interrupts(); } } while(0)
#else
#define FC41D_CRITICAL_ENTER()            (0U)
#define FC41D_CRITICAL_EXIT(was_disabled) ((void)(was_disabled))
#endif

#endif /* FC41D_DRIVER_INC_FC41D_TYPES_H_ */
