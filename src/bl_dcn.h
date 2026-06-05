#ifndef BL_DCN_H
#define BL_DCN_H

#include <stdint.h>
#include <stdbool.h>

/* Frame receive + dispatch for the bootloader.
 *
 * The single entry point bl_dcn_service() polls the UART for one byte's
 * worth of progress and returns. Call it in a tight main loop. When a
 * complete, well-formed frame addressed to BL_DCN_ADDRESS arrives, it
 * is dispatched synchronously and a response is emitted before this
 * function returns.
 *
 * Returns true if a frame was processed this call (caller can use this
 * to drive an activity indicator, kick the watchdog, etc.).
 */
bool bl_dcn_service(void);

/* Out-of-line notification used by BLRESET. Returns true if a BLRESET
 * frame was acknowledged on the most recent call to bl_dcn_service(),
 * which signals bl_main that it should trigger a watchdog reset. */
bool bl_dcn_reset_requested(void);

#endif /* BL_DCN_H */
