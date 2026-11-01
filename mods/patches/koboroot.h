/* Writes a KoboRoot.tgz, the package Kobo installs when it next starts. Edits overlay the file as
 * it's copied, and a later edit wins where two overlap. */
#ifndef KOBOROOT_H
#define KOBOROOT_H

typedef struct {
    unsigned at, length;
    const char *hex;
} Overlay;

typedef struct KoboRoot KoboRoot;

KoboRoot *koboroot_open(const char *path);
int koboroot_add(KoboRoot *root, const char *file, const Overlay *overlays, int count);
/* Finishes the file. Returns 0 if anything has failed, including earlier adds. */
int koboroot_close(KoboRoot *root);

#endif
