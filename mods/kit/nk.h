/* The kit's own C layer, for adding to Kobo's UI with Kobo's own widgets, so new things look like
 * they belong. The kit is C++ built against Qt 5.2.1's headers, and these C functions are a thin
 * layer over it. Every symbol it needs is checked when Nickel loads the mod, and a mod that can't
 * find one on this firmware stays out of the way.
 *
 * This header is internal. Mods are written in Lua, whose API is the one that's documented and kept
 * working. C++ mods may use this, but it can change in any release, and they keep up with it. */
#ifndef NK_H
#define NK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NkScreen NkScreen;
typedef struct NkLabel NkLabel;
typedef void (*NkFn)(void *arg);

/* Each mod defines this. It runs once, when Nickel loads the mod, before any UI exists. */
void nk_mod_init(void);

/* What this build is: "release" for the site's, or "dev", its commit (+ if it had changes not
 * committed) and when, for one built on a computer. A dev build's More rows say DEV BUILD. */
const char *nk_build(void);

/* Log to stderr, prefixed with the mod's name. */
void nk_log(const char *fmt, ...);

/* Look up a symbol. Failing lookups are logged and return 0. nk_ok() is false once any has failed. */
void *nk_sym(const char *name);
int nk_ok(void);

/* Connect a Qt signal, given by its mangled name and its class's staticMetaObject, to fn(arg). */
void nk_connect(void *sender, const char *signal, const char *meta, NkFn fn, void *arg);

/* Disable the mod if Nickel crashes in the next few seconds. Startup is covered already; call this
 * before changing anything Nickel will run. Calling it again extends the time. */
void nk_failsafe(int seconds);

/* Replace a function that libnickel calls. Returns the original, or 0 if it can't. */
void *nk_hook(const char *symbol, void *replacement);

/* Run built(view, arg) each time Nickel makes a widget of one of Kobo's classes, such as
 * "MoreView", once its constructor is done. Only for classes made with a parent widget,
 * Class::Class(QWidget*); view is the QWidget. Several can watch the same class. Call from
 * nk_mod_init. Returns 0 if this firmware has no such class. */
typedef void (*NkBuiltFn)(void *view, void *arg);
int nk_on_screen(const char *cls, NkBuiltFn built, void *arg);

/* Add a row to the More page after Help, with the other mods' rows, with a PNG icon built into the
 * mod, scaled as Kobo's own icons there are. Tapping it runs open(arg). Call from nk_mod_init; a
 * mod can add several. If More isn't as expected, there's no row. */
void nk_more_row(const char *title, const unsigned char *icon_png, unsigned icon_size, NkFn open, void *arg);

/* Add a row at the top of Kobo's Settings list, styled like Kobo's own. Tapping it runs open(arg).
 * Call from nk_mod_init; a mod can add several. If Settings isn't laid out as expected, there's no row. */
void nk_settings_row(const char *title, NkFn open, void *arg);

/* Open a settings-style screen. build(screen, arg) fills it in when it opens; pages and the close
 * button come with the screen. */
void nk_open_screen(const char *title, const char *small_title, void (*build)(NkScreen *, void *), void *arg);

/* The grey section heading, as in Beta Features. Each of these returns its label, for nk_label_set. */
NkLabel *nk_section(NkScreen *s, const char *title);

/* A line of text. */
NkLabel *nk_text(NkScreen *s, const char *text);

/* A row with a title on the left and italic text on the right. Returns the italic text. */
NkLabel *nk_value_row(NkScreen *s, const char *title, const char *value);

/* A row with a title and Kobo's "On" tick box. changed(arg) runs whenever it's ticked or unticked,
 * including by nk_toggle_set. */
typedef struct NkToggle NkToggle;
NkToggle *nk_toggle_row(NkScreen *s, const char *title, int on, NkFn changed, void *arg);
int nk_toggle_on(NkToggle *toggle);
void nk_toggle_set(NkToggle *toggle, int on);

/* A row with a title and an italic button, like Beta Features' "Start". Returns the title, so it
 * can be changed with nk_label_set. */
NkLabel *nk_button_row(NkScreen *s, const char *title, const char *button, NkFn tapped, void *arg);
void nk_label_set(NkLabel *label, const char *text);

/* Kobo's dialog with two buttons. One of the functions runs when it's answered; either can be 0. */
void nk_confirm(const char *title, const char *text, const char *accept, const char *reject, NkFn accepted, NkFn rejected, void *arg);

/* Kobo's error dialog. */
void nk_error(const char *title, const char *text);

/* Where the mod's file is now: moved aside while the crash guard is on, and back afterwards. */
const char *nk_self_path(void);

/* Replace the mod's file with a new build, taking effect when Nickel next starts. The crash guard
 * is stopped first so it can't put the old file back. A mod with a version keeps the build that's
 * running, and puts the new one, of that version, on probation: if the guard catches it crashing
 * or freezing in its first few starts, the one before it comes back. Returns 0 if nothing was
 * changed. */
int nk_replace_self(const void *data, unsigned size, int version);

/* Restart the Kobo the way Kobo does. */
void nk_reboot(void);

/* Run a shell command on its own, outside Nickel and from /, the way NickelMenu's cmd_spawn does. The crash
 * guard is stopped first, since the command may well stop Nickel. */
void nk_spawn(const char *command);

#ifdef __cplusplus
}
#endif

#endif
