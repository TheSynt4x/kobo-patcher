/* "KOReader" on the More page, above Settings, when KOReader is in .adds. Tapping it runs
 * KOReader's own launcher, which stops Nickel and starts it again when KOReader quits. */
#include <unistd.h>
#include "../kit/nk.h"
#include "koreader.h"

#define LAUNCHER "/mnt/onboard/.adds/koreader/koreader.sh"

static const unsigned char icon[] = {
#include "koreader_icon.inc"
};

static void open_koreader(void *unused) {
    if (access(LAUNCHER, R_OK)) {
        nk_error("KOReader isn't installed", "Add it again from kobo.thesynt4x.dev, then restart your Kobo.");
        return;
    }
    nk_log("starting KOReader");
    nk_spawn("exec " LAUNCHER);
}

void koreader_init(void) {
    if (access(LAUNCHER, F_OK)) return;
    nk_more_row("KOReader", icon, sizeof icon, open_koreader, 0);
}
