/* Gzip here uses "stored" blocks: valid gzip with no compression. It keeps this small and fast on
 * a Kobo's slow CPU, at the cost of a bigger temporary file. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "koboroot.h"

enum { BLOCK = 65535 };

struct KoboRoot {
    FILE *out;
    int failed;
    uint32_t crc, total, table[256];
    unsigned used;
    unsigned char block[BLOCK];
};

static void flush_block(KoboRoot *root, int last) {
    unsigned char header[5] = {last, root->used & 255, root->used >> 8, ~root->used & 255, (~root->used >> 8) & 255};
    if (fwrite(header, 1, 5, root->out) != 5 || fwrite(root->block, 1, root->used, root->out) != root->used) root->failed = 1;
    root->used = 0;
}

static void put(KoboRoot *root, const unsigned char *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        root->crc = root->table[(root->crc ^ data[i]) & 255] ^ (root->crc >> 8);
        root->block[root->used++] = data[i];
        if (root->used == BLOCK) flush_block(root, 0);
    }
    root->total += length;
}

KoboRoot *koboroot_open(const char *path) {
    KoboRoot *root = calloc(1, sizeof *root);
    if (!root) return 0;
    root->out = fopen(path, "wb");
    if (!root->out) {
        free(root);
        return 0;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; bit++) value = value & 1 ? 0xedb88320 ^ (value >> 1) : value >> 1;
        root->table[i] = value;
    }
    root->crc = 0xffffffff;
    static const unsigned char gzip_header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    if (fwrite(gzip_header, 1, 10, root->out) != 10) root->failed = 1;
    return root;
}

static void tar_header(KoboRoot *root, const char *name, unsigned long size, unsigned mode) {
    char header[512] = {0};
    snprintf(header, 100, "%s", name);
    snprintf(header + 100, 8, "%07o", mode);
    snprintf(header + 108, 8, "%07o", 0);
    snprintf(header + 116, 8, "%07o", 0);
    snprintf(header + 124, 12, "%011lo", size);
    snprintf(header + 136, 12, "%011lo", (unsigned long)time(0));
    memset(header + 148, ' ', 8);
    header[156] = '0';
    memcpy(header + 257, "ustar\0" "00", 8);
    strcpy(header + 265, "root");
    strcpy(header + 297, "root");
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (unsigned char)header[i];
    snprintf(header + 148, 8, "%06o", sum);
    header[155] = ' ';
    put(root, (unsigned char *)header, 512);
}

int koboroot_add(KoboRoot *root, const char *file, const Overlay *overlays, int count) {
    int fd = open(file, O_RDONLY);
    struct stat info;
    if (fd < 0 || fstat(fd, &info)) {
        root->failed = 1;
        if (fd >= 0) close(fd);
        return 0;
    }
    char name[256];
    snprintf(name, sizeof name, ".%s", file);
    tar_header(root, name, info.st_size, info.st_mode & 07777);
    enum { CHUNK = 1 << 20 };
    unsigned char *chunk = malloc(CHUNK);
    unsigned long position = 0;
    while (chunk && position < (unsigned long)info.st_size) {
        ssize_t got = read(fd, chunk, CHUNK);
        if (got <= 0) {
            root->failed = 1;
            break;
        }
        for (int i = 0; i < count; i++)
            for (unsigned k = 0; k < overlays[i].length; k++) {
                unsigned long at = overlays[i].at + k;
                if (at < position || at >= position + got) continue;
                unsigned byte;
                sscanf(overlays[i].hex + k * 2, "%2x", &byte);
                chunk[at - position] = byte;
            }
        put(root, chunk, got);
        position += got;
    }
    if (!chunk) root->failed = 1;
    free(chunk);
    close(fd);
    static const unsigned char zeros[512];
    if (info.st_size % 512) put(root, zeros, 512 - info.st_size % 512);
    return !root->failed;
}

int koboroot_close(KoboRoot *root) {
    static const unsigned char zeros[512];
    put(root, zeros, 512);
    put(root, zeros, 512);
    flush_block(root, 1);
    uint32_t crc = ~root->crc, total = root->total;
    unsigned char trailer[8] = {crc, crc >> 8, crc >> 16, crc >> 24, total, total >> 8, total >> 16, total >> 24};
    if (fwrite(trailer, 1, 8, root->out) != 8) root->failed = 1;
    if (fflush(root->out) || fsync(fileno(root->out))) root->failed = 1;
    if (fclose(root->out)) root->failed = 1;
    int ok = !root->failed;
    free(root);
    return ok;
}
