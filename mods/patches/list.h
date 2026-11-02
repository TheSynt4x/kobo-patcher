/* The patch list for this Kobo's firmware, read from a file written by tools/write_lists.py. */
#ifndef LIST_H
#define LIST_H

#include <stddef.h>

typedef struct {
    const char *file;
    unsigned at;
    const char *find, *replace;
} Edit;

typedef struct {
    const char *id, *title, *category, *group, *warning; /* group and warning can be 0 */
    int restart; /* only takes effect once Nickel restarts */
    const Edit *edits;
    int edit_count;
} Patch;

typedef struct {
    const char *model, *version, *name;
    const Patch *patches;
    int patch_count;
} Firmware;

/* Reads a list, checking every line. Returns 0 and writes why into error if anything's wrong. */
Firmware *list_load(const char *path, char *error, size_t size);
void list_free(Firmware *list);

#endif
