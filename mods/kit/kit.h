/* What the kit's own C++ files share. Mods use nk.h, or Lua. */
#ifndef NK_KIT_H
#define NK_KIT_H

#include <QWidget>
#include "nk.h"

class PagingContainer : public QWidget {
public:
    void addWidget(QWidget *widget);
    void insertWidget(int index, QWidget *widget);
};

struct NkScreen {
    PagingContainer *paging;
};

/* Connect a signal, given as a member function pointer, to fn(arg). */
void kit_connect(QObject *sender, void **signal, const QMetaObject *meta, NkFn fn, void *arg);
template <typename Signal> void kit_connect(QObject *sender, Signal signal, const QMetaObject *meta, NkFn fn, void *arg) {
    kit_connect(sender, reinterpret_cast<void **>(&signal), meta, fn, arg);
}

/* Open a settings-style screen, as nk_open_screen does, with done(arg) run once it has gone for
 * good, so arg can be freed. Titles are copied. */
void kit_open_screen(const char *title, const char *small_title, void (*build)(NkScreen *, void *), void *arg, NkFn done);
/* The title of the screen opened last that's still open, or 0. */
const char *kit_top_screen_title(void);
/* Close the screen opened last and open it again, built afresh. */
void kit_reopen_screen(void);

/* A row beside one of Kobo's own on More or in Settings, by that row's object name, after it or
 * before it; with no name, in the usual place. Titles are copied. */
void kit_more_row(const char *title, const unsigned char *icon_png, unsigned icon_size, const char *beside, bool after, NkFn open, void *arg);
void kit_settings_row(const char *title, const char *beside, bool after, NkFn open, void *arg);
/* Forgets every row added, for main.lua to add them again when it runs again. */
void kit_forget_rows(void);

#endif
