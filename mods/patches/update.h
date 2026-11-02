/* "Check for updates": a newer patch list for this firmware, and a newer mod, over Wi-Fi. */
#ifndef UPDATE_H
#define UPDATE_H

#include "../kit/nk.h"

/* Starts a check, reporting progress in status. list_changed runs after a new list is saved.
 * Does nothing while a check is already running. */
void update_check(NkLabel *status, const char *model, const char *version, void (*list_changed)(void));

#endif
