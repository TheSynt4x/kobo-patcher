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

/* Close the screen opened last and open it again, built afresh. */
void kit_reopen_screen(void);

#endif
