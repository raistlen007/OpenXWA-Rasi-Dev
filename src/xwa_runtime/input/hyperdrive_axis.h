#ifndef XWA_RUNTIME_INPUT_HYPERDRIVE_AXIS_H
#define XWA_RUNTIME_INPUT_HYPERDRIVE_AXIS_H

#include <stdint.h>

/* A lever parked near -1.0 arms; moving it past -0.8 fires exactly once.
 * The 0..65535 mapped axis has no dependency on frame rate or key-repeat.
 * A new engagement requires returning to the lower five percent. */
static inline int XwaHyperdriveAxis_Update(uint32_t value, int* armed) {
	if (!armed) return 0;
	if (value <= 1638u) {
		*armed = 1;
		return 0;
	}
	if (value >= 6554u && *armed) {
		*armed = 0;
		return 1;
	}
	return 0;
}

#endif
