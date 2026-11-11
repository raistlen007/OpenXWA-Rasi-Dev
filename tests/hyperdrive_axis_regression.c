#include "xwa_runtime/input/hyperdrive_axis.h"

#include <assert.h>
#include <stddef.h>

int main(void) {
	int armed = 0;
	assert(!XwaHyperdriveAxis_Update(65535, &armed)); /* already forward at startup */
	assert(!XwaHyperdriveAxis_Update(1638, &armed));  /* near lower stop arms */
	assert(armed);
	assert(!XwaHyperdriveAxis_Update(5000, &armed));  /* movement below threshold */
	assert(armed);
	assert(!XwaHyperdriveAxis_Update(6553, &armed));  /* still below -0.8 */
	assert(XwaHyperdriveAxis_Update(6554, &armed));   /* crosses -0.8 */
	assert(!armed);
	assert(!XwaHyperdriveAxis_Update(65535, &armed)); /* no repeated activation */
	assert(!XwaHyperdriveAxis_Update(2000, &armed));  /* not returned far enough */
	assert(!XwaHyperdriveAxis_Update(1638, &armed));
	assert(XwaHyperdriveAxis_Update(6554, &armed));   /* rearmed */
	assert(!XwaHyperdriveAxis_Update(6554, &armed));
	armed = 0; /* simulated disconnect or focus loss */
	assert(!XwaHyperdriveAxis_Update(65535, &armed));
	assert(!XwaHyperdriveAxis_Update(0, &armed));
	assert(XwaHyperdriveAxis_Update(6554, &armed));
	assert(!XwaHyperdriveAxis_Update(6554, NULL));
	return 0;
}
