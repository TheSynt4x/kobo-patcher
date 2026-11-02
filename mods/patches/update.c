/* See update.h. Files come through Qt's QNetworkAccessManager, as Nickel's own downloads do.
 * Nothing downloaded is trusted until it's checked: the manifest has to carry an Ed25519 signature
 * from UPDATE_KEY, and every other file has to match the SHA-512 and size the manifest gives it.
 * That's why a TLS error is ignored, and why plain HTTP is tried when TLS can't be used at all. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../kit/nk.h"
#include "update.h"
#include "vendor/monocypher-ed25519.h"

/* The public half of the key tools/sign_mods.mjs signs with. */
#define UPDATE_KEY "f65e1ff82241944b3e2a8ddb433be5a710835feaca53db3c5fc4afdd01d567b8"

#define FOLDER "/mnt/onboard/.adds/patches"
#define URL_FILE FOLDER "/update-url"
#define DEFAULT_URL "https://kobo.thesynt4x.dev/mods/"
#define MAX_DOWNLOAD (4 << 20)

#define CALL(type, name) ((type)nk_sym(name))

enum { SSL_HANDSHAKE_FAILED = 6 }; /* QNetworkReply::NetworkError */

typedef void (*Got)(const unsigned char *data, int size);

static struct {
    int busy;
    NkLabel *status;
    char model[8], version[32];
    void (*list_changed)(void);
    char base[200];
    char path[64];
    Got done;
    int tried_http;
    unsigned char *manifest;
    int manifest_size;
    char mod_hash[129];
    int mod_size;
} check;

static void *manager;

static void say(const char *text) {
    if (check.status) nk_label_set(check.status, text);
}

static void forget_status(void *unused) { check.status = 0; }

static void finish(const char *text) {
    say(text);
    nk_log("update check: %s", text);
    free(check.manifest);
    check.manifest = 0;
    check.busy = 0;
}

/* ---- downloading ---- */

static void get(const char *path, Got done);

static void finished(void *reply) {
    int error = CALL(int (*)(void *), "_ZNK13QNetworkReply5errorEv")(reply);
    int *bytes;
    CALL(void (*)(int **, void *), "_ZN9QIODevice7readAllEv")(&bytes, reply);
    CALL(void (*)(void *), "_ZN7QObject11deleteLaterEv")(reply);
    if (error == SSL_HANDSHAKE_FAILED && !check.tried_http && !strncmp(check.base, "https://", 8)) {
        /* Everything is checked against the signature, so plain HTTP is as safe, just not private. */
        memmove(check.base + 4, check.base + 5, strlen(check.base + 5) + 1);
        check.tried_http = 1;
        nk_log("TLS didn't work, trying %s", check.base);
        get(check.path, check.done);
    } else if (error || bytes[1] > MAX_DOWNLOAD) {
        nk_log("download of %s failed: %d", check.path, error);
        check.done(0, 0);
    } else {
        check.done((const unsigned char *)bytes + bytes[3], bytes[1]);
    }
    if (*bytes != -1 && !__atomic_sub_fetch(bytes, 1, __ATOMIC_SEQ_CST))
        CALL(void (*)(void *, unsigned, unsigned), "_ZN10QArrayData10deallocateEPS_jj")(bytes, 1, 4);
}

static void get(const char *path, Got done) {
    void (*from_utf8)(void **, const char *, int) = CALL(void (*)(void **, const char *, int), "_ZN7QString15fromUtf8_helperEPKci");
    if (!manager) {
        manager = calloc(1, 64);
        CALL(void (*)(void *, void *), "_ZN21QNetworkAccessManagerC1EP7QObject")(manager, 0);
    }
    snprintf(check.path, sizeof check.path, "%s", path);
    check.done = done;
    char url[300];
    snprintf(url, sizeof url, "%s%s", check.base, path);
    void *text, *qurl, *request, *connection;
    from_utf8(&text, url, strlen(url));
    CALL(void (*)(void **, void **, int), "_ZN4QUrlC1ERK7QStringNS_11ParsingModeE")(&qurl, &text, 0);
    CALL(void (*)(void **, void **), "_ZN15QNetworkRequestC1ERK4QUrl")(&request, &qurl);
    void *reply = CALL(void *(*)(void *, void **), "_ZN21QNetworkAccessManager3getERK15QNetworkRequest")(manager, &request);
    CALL(void (*)(void **), "_ZN15QNetworkRequestD1Ev")(&request);
    CALL(void (*)(void **), "_ZN4QUrlD1Ev")(&qurl);
    /* By name, so that the reply's own ignoreSslErrors runs, not QNetworkReply's empty one. */
    CALL(void (*)(void **, void *, const char *, void *, const char *, int), "_ZN7QObject7connectEPKS_PKcS1_S3_N2Qt14ConnectionTypeE")(
        &connection, reply, "2sslErrors(QList<QSslError>)", reply, "1ignoreSslErrors()", 0);
    nk_connect(reply, "_ZN13QNetworkReply8finishedEv", "_ZN13QNetworkReply16staticMetaObjectE", finished, reply);
}

/* ---- checking ---- */

static void hex_of(const unsigned char *bytes, int size, char *out) {
    for (int i = 0; i < size; i++) sprintf(out + i * 2, "%02x", bytes[i]);
}

static void sha512_hex(const unsigned char *data, size_t size, char out[129]) {
    unsigned char hash[64];
    crypto_sha512(hash, data, size);
    hex_of(hash, 64, out);
}

static int file_hash(const char *path, char out[129]) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    struct stat info;
    fstat(fileno(file), &info);
    unsigned char *data = malloc(info.st_size + 1);
    int ok = fread(data, 1, info.st_size, file) == (size_t)info.st_size;
    fclose(file);
    if (ok) sha512_hex(data, info.st_size, out);
    free(data);
    return ok;
}

static int unhex(const char *hex, unsigned char *out, int size) {
    if ((int)strlen(hex) != size * 2) return 0;
    for (int i = 0; i < size; i++)
        if (sscanf(hex + i * 2, "%2hhx", &out[i]) != 1) return 0;
    return 1;
}

/* Finds a manifest line by its first two fields ("list" and a name), or its first one alone ("mod"
 * when name is 0). Fills in the hash and size, and the targets for "mod". */
static int manifest_line(const char *kind, const char *name, char hash[129], int *size, char *targets, size_t targets_size) {
    char line[512], *fields[5];
    const char *at = (const char *)check.manifest;
    while (at && *at) {
        const char *end = strchr(at, '\n');
        size_t length = end ? (size_t)(end - at) : strlen(at);
        const char *start = at;
        at = end ? end + 1 : 0;
        if (length >= sizeof line) continue;
        memcpy(line, start, length);
        line[length] = 0;
        int count = 0;
        for (char *field = strtok(line, "\t"); field && count < 5; field = strtok(0, "\t")) fields[count++] = field;
        if (count < 4 || strcmp(fields[0], kind) || (name && strcmp(fields[1], name))) continue;
        const char *found = name ? fields[2] : fields[1];
        if (strlen(found) != 128) continue;
        snprintf(hash, 129, "%s", found);
        *size = atoi(name ? fields[3] : fields[2]);
        if (targets) snprintf(targets, targets_size, "%s", name ? "" : fields[3]);
        return 1;
    }
    return 0;
}

static void list_path(char *path, size_t size) { snprintf(path, size, FOLDER "/lists/%s-%s.txt", check.model, check.version); }

static int write_file(const char *path, const unsigned char *data, int size) {
    char temp[160];
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *file = fopen(temp, "wb");
    if (!file) return 0;
    int ok = fwrite(data, 1, size, file) == (size_t)size;
    ok = !fflush(file) && !fsync(fileno(file)) && ok;
    fclose(file);
    if (!ok || rename(temp, path)) {
        unlink(temp);
        return 0;
    }
    return 1;
}

static void got_mod(const unsigned char *data, int size) {
    char hash[129];
    if (!data) return finish("The new Kobo Patcher couldn't be downloaded. Try again later.");
    sha512_hex(data, size, hash);
    if (size != check.mod_size || strcmp(hash, check.mod_hash)) return finish("The download didn't match what was expected, so it wasn't used.");
    if (!nk_replace_self(data, size)) return finish("The new Kobo Patcher couldn't be installed. Nothing was changed.");
    finish("Restarting to finish the update.");
    nk_reboot();
}

static void install_mod(void *unused) {
    say("Downloading Kobo Patcher...");
    get("libpatches.so", got_mod);
}

static void mod_declined(void *unused) { finish("A new Kobo Patcher is ready whenever you want it."); }

/* After the list, the mod itself: only on the Kobos the manifest names, where it has been tried. */
static void check_mod(const char *list_result) {
    char targets[256], here[48], own[129];
    snprintf(here, sizeof here, "%s-%s", check.model, check.version);
    int listed = manifest_line("mod", 0, check.mod_hash, &check.mod_size, targets, sizeof targets);
    int ours = 0;
    for (char *target = strtok(targets, ","); target; target = strtok(0, ",")) ours = ours || !strcmp(target, here);
    if (!listed || !ours || !file_hash(nk_self_path(), own) || !strcmp(own, check.mod_hash)) return finish(list_result);
    say(list_result);
    nk_confirm("Update Kobo Patcher?", "A new version of Kobo Patcher is ready. Your Kobo will restart to install it, and your choices are kept.",
               "Update", "Later", install_mod, mod_declined, 0);
}

static void got_list(const unsigned char *data, int size) {
    char hash[129], path[128], expected[129];
    int expected_size;
    char name[48];
    snprintf(name, sizeof name, "%s-%s", check.model, check.version);
    manifest_line("list", name, expected, &expected_size, 0, 0);
    if (!data) return finish("The patch list couldn't be downloaded. Try again later.");
    sha512_hex(data, size, hash);
    if (size != expected_size || strcmp(hash, expected)) return finish("The download didn't match what was expected, so it wasn't used.");
    mkdir("/mnt/onboard/.adds", 0755);
    mkdir(FOLDER, 0755);
    mkdir(FOLDER "/lists", 0755);
    list_path(path, sizeof path);
    if (!write_file(path, data, size)) return finish("The patch list couldn't be saved. Check there's free space.");
    check.list_changed();
    check_mod("New patches are ready. Close Kobo Patcher and open it again to see them.");
}

static void got_signature(const unsigned char *data, int size) {
    unsigned char key[32], signature[64];
    char hex[130] = "";
    if (!data) return finish("Couldn't check for updates. Make sure Wi-Fi is on, then try again.");
    snprintf(hex, sizeof hex, "%.*s", size < 128 ? size : 128, (const char *)data);
    if (!unhex(UPDATE_KEY, key, 32) || !unhex(hex, signature, 64) ||
        crypto_ed25519_check(signature, key, check.manifest, check.manifest_size))
        return finish("The update didn't check out, so it wasn't used.");
    char name[48], expected[129], own[129], path[128];
    int expected_size;
    snprintf(name, sizeof name, "%s-%s", check.model, check.version);
    list_path(path, sizeof path);
    if (!manifest_line("list", name, expected, &expected_size, 0, 0)) return check_mod("There are no patches for this firmware yet.");
    if (file_hash(path, own) && !strcmp(own, expected)) return check_mod("Everything is up to date.");
    say("Downloading patches...");
    snprintf(path, sizeof path, "lists/%s.txt", name);
    get(path, got_list);
}

static void got_manifest(const unsigned char *data, int size) {
    if (!data) return finish("Couldn't check for updates. Make sure Wi-Fi is on, then try again.");
    check.manifest = malloc(size + 1);
    memcpy(check.manifest, data, size);
    check.manifest[size] = 0;
    check.manifest_size = size;
    get("manifest.sig", got_signature);
}

void update_check(NkLabel *status, const char *model, const char *version, void (*list_changed)(void)) {
    if (check.busy) return;
    check.busy = 1;
    check.status = status;
    nk_connect(status, "_ZN7QObject9destroyedEPS_", "_ZN7QObject16staticMetaObjectE", forget_status, 0);
    snprintf(check.model, sizeof check.model, "%s", model);
    snprintf(check.version, sizeof check.version, "%s", version);
    check.list_changed = list_changed;
    check.tried_http = 0;
    snprintf(check.base, sizeof check.base, "%s", DEFAULT_URL);
    /* For testing: a file with another address, like http://192.168.1.10:8765/mods/. What's there
     * still has to be signed. */
    FILE *file = fopen(URL_FILE, "r");
    if (file) {
        char line[200] = "";
        if (fgets(line, sizeof line, file)) {
            line[strcspn(line, "\r\n")] = 0;
            size_t length = strlen(line);
            if (length > 8 && length < sizeof check.base - 1)
                snprintf(check.base, sizeof check.base, "%s%s", line, line[length - 1] == '/' ? "" : "/");
        }
        fclose(file);
    }
    say("Checking for updates...");
    nk_log("checking %s", check.base);
    get("manifest.txt", got_manifest);
}
