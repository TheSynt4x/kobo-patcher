/* Turning patches on and off in Nickel's memory while it runs. The files on disk aren't changed. */
#ifndef LIVE_H
#define LIVE_H

#include "list.h"

enum { OFF, ON, UNKNOWN };

/* Call once from nk_mod_init with the firmware's version. Returns 0 if this firmware lacks
 * something it needs, and then nothing else here may be used. */
int live_init(const char *version);

/* Whether the patch's bytes in memory are Kobo's, the patch's, or neither. */
int live_state(const Patch *patch);

/* Writes the patch's bytes, after checking every edit's current bytes are the other side's.
 * Returns 0 if they weren't, and then nothing was changed. Sets *restart when Nickel has already
 * used something the patch changes and won't look again until it restarts. */
int live_set(const Patch *patch, int on, int *restart);

/* Asks the screens Nickel has already built to update, so changes show without a restart. */
void live_refresh(void);

#endif
