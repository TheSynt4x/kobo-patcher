/* See nk.h. Addresses and layouts here come from Qt 5.2's ABI and were checked on Nia firmware
 * 4.38.23852. Anything firmware-specific is verified at runtime before it's used.
 *
 * The kit is built against Qt 5.2.1's headers, with no C++ library of its own: it uses Nickel's Qt,
 * and operator new and delete from Nickel's. Calls into Qt and Nickel are direct. build.sh lists
 * every symbol the mod leaves undefined, and they're all checked before the mod starts, so a
 * firmware missing one gets nothing at all rather than a crash when it's first called. */
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopWidget>
#include <QDialog>
#include <QGridLayout>
#include <QHash>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QSharedPointer>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <new>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "kit.h"

#ifndef NK_NAME
#define NK_NAME "mod"
#endif
#define INSTALLED_AT "/usr/local/Kobo/imageformats/lib" NK_NAME ".so"
#define UNINSTALL_FLAG "/mnt/onboard/.adds/" NK_NAME "/uninstall"
/* Outside imageformats, because Qt would still load libpatches.so.anything from in there. */
#define DISABLED_AT "/usr/local/Kobo/" NK_NAME ".disabled"
#define FAILSAFE_SECONDS 20

/* ---- Nickel's own classes, declared with only what's used ----
 * Their objects are made with the size Nickel's own code allocates, never sizeof, since these
 * declarations leave fields out. */

class IconLeftButton : public QWidget {
public:
    explicit IconLeftButton(QWidget *parent);
};
class TouchLabel : public QLabel {
public:
    TouchLabel(QWidget *parent, Qt::WindowFlags flags);
};
class N3ButtonLabel : public TouchLabel {
public:
    explicit N3ButtonLabel(QWidget *parent);
};
class TouchCheckBox : public QCheckBox {
public:
    explicit TouchCheckBox(QWidget *parent);
};
class SettingContainer : public QWidget {
public:
    explicit SettingContainer(QWidget *parent);
};
class N3SettingsView : public QWidget {
public:
    PagingContainer *pagingContainer() const;
};
class N3SettingsExtrasView : public QWidget {
public:
    explicit N3SettingsExtrasView(QWidget *parent);
};
class N3SettingsLoggingCategoryView : public QWidget {
public:
    explicit N3SettingsLoggingCategoryView(QWidget *parent);
    PagingContainer *pagingContainer() const;
};
class ConfirmationDialog : public QDialog {
public:
    void setTitle(const QString &text);
    void setText(const QString &text);
    void setAcceptButtonText(const QString &text);
    void setRejectButtonText(const QString &text);
};
class ConfirmationDialogFactory {
public:
    static ConfirmationDialog *getConfirmationDialog(QWidget *parent);
    static void *showErrorDialog(const QString &title, const QString &text);
};
class N3PowerWorkflowManager {
public:
    static N3PowerWorkflowManager *sharedInstance();
    void reboot();
};
class AbstractController;
class MainWindowController {
public:
    static MainWindowController *sharedInstance();
    void push(AbstractController *controller, bool animate);
    void pop(AbstractController *controller);
};

/* ---- plumbing ---- */

/* Written whole, in one write, so a line from another thread, such as the crash guard's, isn't cut in
 * two by Nickel's own output. */
void nk_log(const char *fmt, ...) {
    char line[1024];
    int prefix = snprintf(line, sizeof line, "[" NK_NAME "] ");
    va_list args;
    va_start(args, fmt);
    int length = prefix + vsnprintf(line + prefix, sizeof line - prefix - 1, fmt, args);
    va_end(args);
    if (length > (int)sizeof line - 2) length = sizeof line - 2;
    line[length] = '\n';
    if (write(2, line, length + 1) < 0) return;
}

static int all_found = 1;

void *nk_sym(const char *name) {
    void *found = dlsym(RTLD_DEFAULT, name);
    if (!found) {
        nk_log("missing %s", name);
        all_found = 0;
    }
    return found;
}

int nk_ok(void) { return all_found; }

/* Zeroed memory from Nickel's operator new, for an object of Nickel's own size. */
static void *alloc(size_t size) {
    void *memory = operator new(size);
    memset(memory, 0, size);
    return memory;
}

static QString qstr(const char *text) { return QString::fromUtf8(text); }

static const char *class_name(QObject *object) { return object->metaObject()->className(); }

static QObject *find_named(QObject *parent, const char *name) {
    const QObjectList &kids = parent->children();
    for (int i = 0; i < kids.size(); i++)
        if (kids[i]->objectName() == QLatin1String(name)) return kids[i];
    return 0;
}

static QObject *find_class(QObject *parent, const char *name) {
    const QObjectList &kids = parent->children();
    for (int i = 0; i < kids.size(); i++)
        if (!strcmp(class_name(kids[i]), name)) return kids[i];
    return 0;
}

/* Kobo's label classes have no exported constructors, so a QLabel is made and given the vtable
 * pointers of one of Kobo's: the QObject one at +0 and the QPaintDevice one at +8. */
static void take_vptrs(void *vptrs[2], QObject *from) {
    vptrs[0] = reinterpret_cast<void **>(from)[0];
    vptrs[1] = reinterpret_cast<void **>(from)[2];
}

/* Connect a Qt signal to a C function, the way Qt's functor connect does: a QSlotObjectBase is a
 * refcount and an impl function, which gets 0 to destroy, 1 to call, 2 to compare. */
struct Slot : QtPrivate::QSlotObjectBase {
    NkFn fn;
    void *arg;
    Slot(NkFn fn, void *arg) : QSlotObjectBase(impl), fn(fn), arg(arg) {}
    static void impl(int which, QSlotObjectBase *base, QObject *, void **, bool *ret) {
        Slot *slot = static_cast<Slot *>(base);
        if (which == 1) slot->fn(slot->arg);
        else if (which == 2 && ret) *ret = false;
    }
};

/* QObject::connectImpl is private, so it's declared again here under its own name. */
QMetaObject::Connection connect_impl(const QObject *sender, void **signal, const QObject *receiver, void **slot,
                                     QtPrivate::QSlotObjectBase *slot_object, Qt::ConnectionType type, const int *types,
                                     const QMetaObject *sender_meta)
    __asm__("_ZN7QObject11connectImplEPKS_PPvS1_S3_PN9QtPrivate15QSlotObjectBaseEN2Qt14ConnectionTypeEPKiPK11QMetaObject");

/* The signal and its class's staticMetaObject are looked up by name, so a firmware without them
 * still loads the mod, and the check before it starts finds them missing. */
static void connect_to(QObject *sender, const char *signal, const char *meta, NkFn fn, void *arg) {
    void *signal_ptr[2] = {nk_sym(signal), 0}, *no_slot[2] = {0, 0};
    void *static_meta = nk_sym(meta);
    if (!signal_ptr[0] || !static_meta) return;
    connect_impl(sender, signal_ptr, sender, no_slot, new Slot(fn, arg), Qt::AutoConnection, 0,
                 static_cast<const QMetaObject *>(static_meta));
}

void kit_connect(QObject *sender, void **signal, const QMetaObject *meta, NkFn fn, void *arg) {
    void *no_slot[2] = {0, 0};
    connect_impl(sender, signal, sender, no_slot, new Slot(fn, arg), Qt::AutoConnection, 0, meta);
}

void nk_connect(void *sender, const char *signal, const char *meta, NkFn fn, void *arg) {
    connect_to(static_cast<QObject *>(sender), signal, meta, fn, arg);
}

/* ---- hooking ---- */

/* libnickel calls its own exported functions through its GOT, so changing an entry there
 * redirects the call without touching any code. */
struct Hook {
    const char *symbol;
    void *replacement, *original; /* no replacement: only read what's there */
};

static int hook_library(struct dl_phdr_info *info, size_t, void *data) {
    Hook *hook = static_cast<Hook *>(data);
    if (!strstr(info->dlpi_name, "libnickel.so")) return 0;
    const ElfW(Phdr) *ph = info->dlpi_phdr;
    const ElfW(Dyn) *dyn = 0;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) dyn = reinterpret_cast<const ElfW(Dyn) *>(info->dlpi_addr + ph[i].p_vaddr);
    if (!dyn) return 1;
    const ElfW(Sym) *symbols = 0;
    const char *strings = 0;
    const ElfW(Rel) *rel = 0;
    size_t rel_size = 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        if (dyn->d_tag == DT_SYMTAB) symbols = reinterpret_cast<const ElfW(Sym) *>(dyn->d_un.d_ptr);
        else if (dyn->d_tag == DT_STRTAB) strings = reinterpret_cast<const char *>(dyn->d_un.d_ptr);
        else if (dyn->d_tag == DT_JMPREL) rel = reinterpret_cast<const ElfW(Rel) *>(dyn->d_un.d_ptr);
        else if (dyn->d_tag == DT_PLTRELSZ) rel_size = dyn->d_un.d_val;
    }
    if (!symbols || !strings || !rel) return 1;
    for (size_t i = 0; i < rel_size / sizeof *rel; i++) {
        const ElfW(Sym) *sym = &symbols[ELF32_R_SYM(rel[i].r_info)];
        if (strcmp(strings + sym->st_name, hook->symbol)) continue;
        void **slot = reinterpret_cast<void **>(info->dlpi_addr + rel[i].r_offset);
        uintptr_t page = reinterpret_cast<uintptr_t>(slot) & ~uintptr_t(0xfff);
        if (hook->replacement && mprotect(reinterpret_cast<void *>(page), 0x1000, PROT_READ | PROT_WRITE)) return 1;
        /* Another mod may have hooked it already, so its replacement is called next and both run.
         * Otherwise the slot holds Nickel's own address, or the stub that looks it up. */
        void *current = *slot;
        Dl_info where;
        bool nickels = dladdr(current, &where) && where.dli_fname && strstr(where.dli_fname, "libnickel.so");
        hook->original = nickels || !current ? dlsym(RTLD_DEFAULT, hook->symbol) : current;
        if (hook->replacement) *slot = hook->replacement;
        return 1;
    }
    return 1;
}

/* What libnickel's own calls to symbol reach now: another mod's hook, if one has taken it, or
 * Nickel's function. For calling it the way Nickel does, so every mod's hook on it runs. */
void *kit_got(const char *symbol) {
    Hook hook = {symbol, 0, 0};
    dl_iterate_phdr(hook_library, &hook);
    return hook.original ? hook.original : dlsym(RTLD_DEFAULT, symbol);
}

void *nk_hook(const char *symbol, void *replacement) {
    Hook hook = {symbol, replacement, 0};
    dl_iterate_phdr(hook_library, &hook);
    if (!hook.original) {
        nk_log("can't hook %s", symbol);
        all_found = 0;
    }
    return hook.original;
}

static void guard_screen(void);
static void unguard_screen(void);
static void failsafe_stop(void);

/* ---- when Kobo builds a screen ---- */

/* Functions run after the constructor of one of Kobo's widget classes, for the classes made with
 * a parent widget, Class::Class(QWidget*). Each constructor hooked needs a function of its own that
 * knows the original, so there's a pool of them. Another mod's kit hooking the same constructor
 * chains with this one. Allocated when first used, since a mod starts before this file's own
 * statics are made. */
struct Built {
    QByteArray symbol;
    void *(*real)(void *, void *);
    QList<QPair<NkBuiltFn, void *> > fns;
};
enum { BUILT_SLOTS = 16 };
static Built *built;
static int built_count;

template <int N> static void *built_hook(void *self, void *parent) {
    void *result = built[N].real(self, parent);
    QList<QPair<NkBuiltFn, void *> > fns = built[N].fns; /* a copy, in case one adds another */
    for (int i = 0; i < fns.size(); i++) fns[i].first(self, fns[i].second);
    return result;
}
static void *const built_hooks[BUILT_SLOTS] = {
    (void *)built_hook<0>,  (void *)built_hook<1>,  (void *)built_hook<2>,  (void *)built_hook<3>,
    (void *)built_hook<4>,  (void *)built_hook<5>,  (void *)built_hook<6>,  (void *)built_hook<7>,
    (void *)built_hook<8>,  (void *)built_hook<9>,  (void *)built_hook<10>, (void *)built_hook<11>,
    (void *)built_hook<12>, (void *)built_hook<13>, (void *)built_hook<14>, (void *)built_hook<15>,
};

int nk_on_screen(const char *cls, NkBuiltFn fn, void *arg) {
    /* Split, so build.sh doesn't take the prefix for a symbol the mod needs: a class this firmware
     * lacks only loses what watches it. */
    QByteArray symbol = QByteArray("_" "ZN") + QByteArray::number(int(strlen(cls))) + cls + "C1EP7QWidget";
    if (!built) built = new Built[BUILT_SLOTS];
    for (int i = 0; i < built_count; i++) {
        if (built[i].symbol != symbol) continue;
        built[i].fns << qMakePair(fn, arg);
        return 1;
    }
    /* Looked up first, so a class this firmware doesn't have only costs what needs it. */
    if (!dlsym(RTLD_DEFAULT, symbol.constData())) {
        nk_log("this firmware has no %s made with a parent widget", cls);
        return 0;
    }
    if (built_count == BUILT_SLOTS) {
        nk_log("too many kinds of screen to watch, not %s", cls);
        return 0;
    }
    Built &b = built[built_count];
    b.real = reinterpret_cast<void *(*)(void *, void *)>(nk_hook(symbol.constData(), built_hooks[built_count]));
    if (!b.real) return 0;
    b.symbol = symbol;
    b.fns << qMakePair(fn, arg);
    built_count++;
    return 1;
}

/* ---- rows in Kobo's More and Settings ---- */

/* A row a mod adds beside one of Kobo's own, by its object name, or in the default place. Rows go
 * in when the screen is built, so changed rows show the next time it opens. */
struct AddedRow {
    QByteArray title, beside;
    bool after, first;
    const unsigned char *icon;
    unsigned icon_size;
    NkFn open;
    void *arg;
};
static QList<AddedRow> *more_rows, *settings_rows;

void kit_forget_rows(void) {
    if (more_rows) more_rows->clear();
    if (settings_rows) settings_rows->clear();
}

static void *italic_label_vptr[2];

static int more_index(void *layout, const char *name) {
    QBoxLayout *box = static_cast<QBoxLayout *>(layout);
    for (int i = 0; i < box->count(); i++) {
        QWidget *w = box->itemAt(i)->widget();
        if (w && w->objectName() == QLatin1String(name)) return i;
    }
    return -1;
}

/* A row's icon, scaled as Kobo's own are on this Kobo, which are larger on larger screens. Kobo's
 * pictures are 56 pixels on the Nia, with room around the drawing, and a mod's icon, drawn nearer its
 * edges, matches them at 40: so it's shown at 40/56 of Kobo's size. Without Kobo's, as it's drawn. */
static void set_more_icon(QObject *row, const QSize &kobo, const unsigned char *png, unsigned size) {
    QPixmap pixmap;
    pixmap.loadFromData(png, size, "PNG", 0);
    QSize wanted = kobo * 40 / 56;
    if (!pixmap.isNull() && wanted.isValid() && !wanted.isEmpty() && wanted != pixmap.size())
        pixmap = pixmap.scaled(wanted, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    row->setProperty("pixmap", pixmap);
}

/* More: Kobo's rows are IconLeftButtons in a GenericContainerWidget's layout. A row that names none
 * of Kobo's goes after Help, the last of them, so the mods' rows are together below Kobo's own. A
 * row that asks to be first goes straight after Help. */
static void more_built(void *view, void *) {
    if (!more_rows || more_rows->isEmpty()) return;
    QObject *container = find_class(static_cast<QObject *>(view), "GenericContainerWidget");
    QBoxLayout *layout = container ? static_cast<QBoxLayout *>(static_cast<QWidget *>(container)->layout()) : 0;
    if (!layout || more_index(layout, "settingsButton") < 0) {
        nk_log("More page isn't what's expected, leaving it alone");
        return;
    }
    QWidget *box = static_cast<QWidget *>(container);
    QList<AddedRow> rows = *more_rows;
    QWidget *settings = layout->itemAt(more_index(layout, "settingsButton"))->widget();
    QSize kobo_icon = settings ? settings->property("pixmap").value<QPixmap>().size() : QSize();
    int firsts = 0;
    for (int i = 0; i < rows.size(); i++) {
        bool last = rows[i].beside.isEmpty();
        QByteArray beside = last ? QByteArray("helpButton") : rows[i].beside;
        int at = more_index(layout, beside.constData());
        if (at < 0) {
            nk_log("no %s on More for the row %s, so it goes above Settings", beside.constData(), rows[i].title.constData());
            at = more_index(layout, "settingsButton");
        } else if (last && rows[i].first) {
            /* Straight after Help, after this mod's own first rows, so they keep their order. */
            at += 1 + firsts++;
        } else if (last || rows[i].after) {
            /* After any of ours already there, so rows keep their order. */
            for (at++; at < layout->count(); at++) {
                QWidget *next = layout->itemAt(at)->widget();
                if (!next || next->objectName() != QLatin1String("nkMoreRow")) break;
            }
        }
        IconLeftButton *row = new (alloc(0x58)) IconLeftButton(box);
        row->setObjectName(QStringLiteral("nkMoreRow"));
        set_more_icon(row, kobo_icon, rows[i].icon, rows[i].icon_size);
        row->setProperty("text", QString::fromUtf8(rows[i].title));
        layout->insertWidget(at, row, 0, 0);
        connect_to(row, "_ZN14IconButtonBase6tappedEv", "_ZN14IconButtonBase16staticMetaObjectE", rows[i].open, rows[i].arg);
        /* The italic label, for values, is learned from the row. */
        QObject *inner = find_class(row, "GenericContainerWidget");
        QObject *italic = inner ? find_class(inner, "RegularItalicLabel") : 0;
        if (italic && !italic_label_vptr[0]) take_vptrs(italic_label_vptr, italic);
    }
    nk_log("%d More rows added", rows.size());
}

void kit_more_row(const char *title, const unsigned char *icon_png, unsigned icon_size, const char *beside, bool after, bool first, NkFn open, void *arg) {
    if (!more_rows) {
        more_rows = new QList<AddedRow>;
        nk_on_screen("MoreView", more_built, 0);
    }
    AddedRow row = {title, beside ? beside : "", after, first, icon_png, icon_size, open, arg};
    *more_rows << row;
}

void nk_more_row(const char *title, const unsigned char *icon_png, unsigned icon_size, NkFn open, void *arg) {
    kit_more_row(title, icon_png, icon_size, 0, false, false, open, arg);
}

/* Every symbol the mod uses, listed by build.sh: what it leaves undefined, and the names this file
 * looks up. They're all checked before the mod starts. */
static int all_symbols_present(void) {
    static const char *const symbols[] = {
#include <symbols.inc>
    };
    int found = 1;
    for (size_t i = 0; i < sizeof symbols / sizeof *symbols; i++)
        if (!nk_sym(symbols[i])) found = 0;
    return found;
}

static QObject *find_named_deep(QObject *parent, const char *name) {
    const QObjectList &kids = parent->children();
    for (int i = 0; i < kids.size(); i++) {
        if (kids[i]->objectName() == QLatin1String(name)) return kids[i];
        QObject *deeper = find_named_deep(kids[i], name);
        if (deeper) return deeper;
    }
    return 0;
}

/* Where a widget is in a PagingContainer's QList<QPointer<QWidget>>, at +0x2c. Each item is a
 * QPointer: its refcount data, then the widget. -1 if it's not there or the list looks wrong. */
static int paging_index(void *paging, void *widget) {
    int *list = *reinterpret_cast<int **>(static_cast<char *>(paging) + 0x2c);
    if (!list || list[0] < 1 || list[2] < 0 || list[3] < list[2] || list[3] > list[1] || list[3] - list[2] > 64) return -1;
    for (int i = list[2]; i < list[3]; i++) {
        void **pointer = reinterpret_cast<void ***>(list + 4)[i];
        if (pointer && pointer[1] == widget) return i - list[2];
    }
    return -1;
}

/* The widget at an index of a PagingContainer's list, or 0, checked as paging_index checks it. */
static QWidget *paging_at(void *paging, int index) {
    int *list = *reinterpret_cast<int **>(static_cast<char *>(paging) + 0x2c);
    if (!list || list[0] < 1 || list[2] < 0 || list[3] < list[2] || list[3] > list[1] || list[3] - list[2] > 64) return 0;
    if (index < 0 || index >= list[3] - list[2]) return 0;
    void **pointer = reinterpret_cast<void ***>(list + 4)[list[2] + index];
    return pointer ? static_cast<QWidget *>(pointer[1]) : 0;
}

/* Kobo's Settings rows are a TouchLabel subclass with no constructor of its own: Ui_N3SettingsView
 * makes a TouchLabel and sets these vtable pointers. They're copied from Device information, which
 * every firmware tried has, and its place checks the list. Rows go at the top unless they say. */
static const int settings_row_vptrs[] = {0, 2, 8, 0x13, 0x16};

static void settings_built(void *view, void *) {
    if (!settings_rows || settings_rows->isEmpty()) return;
    PagingContainer *paging = static_cast<N3SettingsView *>(view)->pagingContainer();
    QObject *model = paging ? find_named_deep(paging, "deviceInformation") : 0;
    if (!model || !model->inherits("TouchLabel") || paging_index(paging, model) < 0) {
        nk_log("Settings isn't what's expected, leaving it alone");
        return;
    }
    QList<AddedRow> rows = *settings_rows;
    int top = 0;
    for (int i = 0; i < rows.size(); i++) {
        int at = top;
        if (!rows[i].beside.isEmpty()) {
            QObject *kobo = find_named_deep(paging, rows[i].beside.constData());
            int found = kobo ? paging_index(paging, kobo) : -1;
            if (found < 0) {
                nk_log("no %s in Settings for the row %s, so it goes at the top", rows[i].beside.constData(), rows[i].title.constData());
            } else if (rows[i].after) {
                /* After any of ours already there, so rows keep their order. */
                for (at = found + 1; paging_at(paging, at) && paging_at(paging, at)->objectName() == QLatin1String("nkSettingsRow"); at++) {}
            } else {
                at = found;
            }
        }
        TouchLabel *row = new (alloc(0x84)) TouchLabel(paging, 0);
        for (size_t v = 0; v < sizeof settings_row_vptrs / sizeof *settings_row_vptrs; v++)
            reinterpret_cast<void **>(row)[settings_row_vptrs[v]] = reinterpret_cast<void **>(model)[settings_row_vptrs[v]];
        row->setObjectName(QStringLiteral("nkSettingsRow"));
        row->setText(QString::fromUtf8(rows[i].title));
        paging->insertWidget(at, row);
        if (rows[i].beside.isEmpty()) top++;
        connect_to(row, "_ZN10TouchLabel6tappedEb", "_ZN10TouchLabel16staticMetaObjectE", rows[i].open, rows[i].arg);
    }
    nk_log("%d Settings rows added", rows.size());
}

void kit_settings_row(const char *title, const char *beside, bool after, NkFn open, void *arg) {
    if (!settings_rows) {
        settings_rows = new QList<AddedRow>;
        nk_on_screen("N3SettingsView", settings_built, 0);
    }
    AddedRow row = {title, beside ? beside : "", after, false, 0, 0, open, arg};
    *settings_rows << row;
}

void nk_settings_row(const char *title, NkFn open, void *arg) { kit_settings_row(title, 0, false, open, arg); }

/* ---- screens ---- */

/* Screens reuse N3SettingsLoggingCategoryController and its view, a bare paged settings page
 * with nothing else on it. A copy of the controller's vtables replaces what fills the page. */
enum { PRIMARY_SLOTS = 16, GET_TITLE = 13, GET_SMALL_TITLE = 14, LOAD_VIEW = 15, ABSTRACT_SLOTS = 9, ABSTRACT_LOAD_VIEW = 8 };

/* What's learned from Kobo's own labels, for every screen. */
static struct {
    void *vptrs[4][2];
    int learned;
} screen;

/* Each open screen's own title and build, by its controller, so screens over screens keep theirs.
 * Kept until the screen's view goes, unless it's reopened as another, which keeps the arg. */
struct ScreenInfo {
    QByteArray title, small_title;
    void (*build)(NkScreen *, void *);
    void *arg;
    NkFn done; /* when the screen has gone for good, for the arg; or 0 */
    bool handed;
};
static QHash<void *, ScreenInfo *> *screens; /* by controller */
static QList<void *> *screen_stack;          /* controllers in the order they were pushed */

static ScreenInfo *info_of(void *controller) { return screens ? screens->value(controller) : 0; }

static void screen_gone(void *controller) {
    ScreenInfo *info = screens ? screens->take(controller) : 0;
    if (screen_stack) screen_stack->removeAll(controller);
    if (!info) return;
    if (!info->handed && info->done) info->done(info->arg);
    delete info;
}

static void learn_label(QObject *parent, const char *name, int which) {
    const QObjectList &kids = parent->children();
    for (int i = 0; i < kids.size() && !screen.vptrs[which][0]; i++) {
        if (!strcmp(class_name(kids[i]), name)) take_vptrs(screen.vptrs[which], kids[i]);
        learn_label(kids[i], name, which);
    }
}

enum { LABEL_SECTION, LABEL_TEXT, LABEL_ROW };

/* Beta Features holds the heading and text labels, so one is built and thrown away to learn them. */
static void learn_labels(void) {
    if (screen.learned) return;
    screen.learned = 1;
    N3SettingsExtrasView *view = new (alloc(0x400)) N3SettingsExtrasView(0);
    learn_label(view, "MetaDataHeaderLabel", LABEL_SECTION);
    learn_label(view, "RegularLabel", LABEL_TEXT);
    learn_label(view, "RegularLabelVPadded", LABEL_ROW);
    view->deleteLater();
    /* The italic label, for values, is in Kobo's icon rows. */
    if (!italic_label_vptr[0]) {
        IconLeftButton *button = new (alloc(0x58)) IconLeftButton(0);
        QObject *inner = find_class(button, "GenericContainerWidget");
        QObject *italic = inner ? find_class(inner, "RegularItalicLabel") : 0;
        if (italic) take_vptrs(italic_label_vptr, italic);
        button->deleteLater();
    }
}

static QLabel *label(QWidget *parent, void *vptrs[2], const char *text) {
    static_assert(sizeof(QLabel) == 0x18, "QLabel isn't Qt 5.2's");
    QLabel *object = new (alloc(0x18)) QLabel(parent, 0);
    if (vptrs[0]) {
        reinterpret_cast<void **>(object)[0] = vptrs[0];
        reinterpret_cast<void **>(object)[2] = vptrs[1];
    }
    object->setText(qstr(text));
    return object;
}

static void add(NkScreen *s, QWidget *block) { s->paging->addWidget(block); }

NkLabel *nk_section(NkScreen *s, const char *title) {
    QLabel *heading = label(s->paging, screen.vptrs[LABEL_SECTION], title);
    add(s, heading);
    return reinterpret_cast<NkLabel *>(heading);
}

NkLabel *nk_text(NkScreen *s, const char *text) {
    QLabel *line = label(s->paging, screen.vptrs[LABEL_TEXT], text);
    line->setWordWrap(true);
    /* Space above and below, as Kobo leaves around its own notes: less above, since a row above has
     * padding of its own, so the text sits evenly between it and what follows. As padding in the
     * label's own style, because Kobo's stylesheet sets a label's margins again when it styles it,
     * and scaled from the Nia's 758 pixels across, as Kobo's fonts are. Kobo's own padding at the
     * sides stays, so the text lines up with the rows. */
    QDesktopWidget *desktop = QApplication::desktop();
    qreal scale = desktop && desktop->width() > 0 ? qMax<qreal>(1, qMin(desktop->width(), desktop->height()) / 758.0) : 1;
    line->setStyleSheet(QStringLiteral("padding-top: %1px; padding-bottom: %2px;").arg(qRound(8 * scale)).arg(qRound(22 * scale)));
    add(s, line);
    return reinterpret_cast<NkLabel *>(line);
}

/* A settings row: Kobo's container, with the title on the left filling the row. */
struct Row {
    SettingContainer *row;
    QHBoxLayout *box;
    QLabel *name;
};

static Row row_with_title(NkScreen *s, const char *title) {
    Row r;
    r.row = new (alloc(0x6c)) SettingContainer(s->paging);
    r.box = new (alloc(0x10)) QHBoxLayout(r.row);
    r.name = label(0, screen.vptrs[LABEL_ROW], title);
    r.name->setWordWrap(true);
    r.box->addWidget(r.name, 1, 0);
    return r;
}

NkLabel *nk_value_row(NkScreen *s, const char *title, const char *value) {
    Row r = row_with_title(s, title);
    QLabel *right = label(0, italic_label_vptr, value);
    /* Lines the value up with the "On" and "Start" of Kobo's own settings rows. */
    right->setStyleSheet(QStringLiteral("padding-right: 17px;"));
    r.box->addWidget(right, 0, 0);
    add(s, r.row);
    return reinterpret_cast<NkLabel *>(right);
}

NkToggle *nk_toggle_row(NkScreen *s, const char *title, int on, NkFn changed, void *arg) {
    Row r = row_with_title(s, title);
    TouchCheckBox *check = new (alloc(0x58)) TouchCheckBox(0);
    check->setText(QStringLiteral("On"));
    check->setChecked(on);
    connect_to(check, "_ZN9QCheckBox12stateChangedEi", "_ZN9QCheckBox16staticMetaObjectE", changed, arg);
    r.box->addWidget(check, 0, 0);
    add(s, r.row);
    return reinterpret_cast<NkToggle *>(check);
}

int nk_toggle_on(NkToggle *toggle) { return reinterpret_cast<QAbstractButton *>(toggle)->isChecked(); }

void nk_toggle_set(NkToggle *toggle, int on) { reinterpret_cast<QAbstractButton *>(toggle)->setChecked(on); }

NkLabel *nk_button_row(NkScreen *s, const char *title, const char *button, NkFn tapped, void *arg) {
    Row r = row_with_title(s, title);
    N3ButtonLabel *action = new (alloc(0x98)) N3ButtonLabel(0);
    action->setText(qstr(button));
    connect_to(action, "_ZN10TouchLabel6tappedEb", "_ZN10TouchLabel16staticMetaObjectE", tapped, arg);
    r.box->addWidget(action, 0, 0);
    add(s, r.row);
    return reinterpret_cast<NkLabel *>(r.name);
}


/* ---- blocks on Kobo's own screens, for nk.on_screen ---- */

/* At the end of a Kobo widget's layout: a new row of a grid, across it, or before a box's closing
 * stretch, so it sits under what's there. False if it has neither. */
static bool append_to(QWidget *into, QWidget *block) {
    QLayout *layout = into->layout();
    if (QGridLayout *grid = qobject_cast<QGridLayout *>(layout)) {
        grid->addWidget(block, grid->rowCount(), 0, 1, qMax(1, grid->columnCount()));
    } else if (QBoxLayout *box = qobject_cast<QBoxLayout *>(layout)) {
        int at = box->count();
        if (at > 0 && box->itemAt(at - 1)->spacerItem()) at--;
        box->insertWidget(at, block);
    } else {
        return false;
    }
    block->show();
    return true;
}

QLabel *kit_append_section(QWidget *into, const char *title) {
    learn_labels();
    QLabel *heading = label(into, screen.vptrs[LABEL_SECTION], title);
    if (append_to(into, heading)) return heading;
    delete heading;
    return 0;
}

QLabel *kit_append_button_row(QWidget *into, const char *title, const char *button, NkFn tapped, void *arg) {
    learn_labels();
    SettingContainer *row = new (alloc(0x6c)) SettingContainer(into);
    QHBoxLayout *box = new (alloc(0x10)) QHBoxLayout(row);
    QLabel *name = label(0, screen.vptrs[LABEL_ROW], title);
    name->setWordWrap(true);
    box->addWidget(name, 1, 0);
    N3ButtonLabel *action = new (alloc(0x98)) N3ButtonLabel(0);
    action->setText(qstr(button));
    connect_to(action, "_ZN10TouchLabel6tappedEb", "_ZN10TouchLabel16staticMetaObjectE", tapped, arg);
    box->addWidget(action, 0, 0);
    if (append_to(into, row)) return name;
    delete row;
    return 0;
}

void nk_label_set(NkLabel *line, const char *text) { reinterpret_cast<QLabel *>(line)->setText(qstr(text)); }

void nk_confirm(const char *title, const char *text, const char *accept, const char *reject, NkFn accepted, NkFn rejected, void *arg) {
    ConfirmationDialog *dialog = ConfirmationDialogFactory::getConfirmationDialog(0);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setTitle(qstr(title));
    dialog->setText(qstr(text));
    dialog->setAcceptButtonText(qstr(accept));
    dialog->setRejectButtonText(qstr(reject));
    if (accepted) connect_to(dialog, "_ZN7QDialog8acceptedEv", "_ZN7QDialog16staticMetaObjectE", accepted, arg);
    if (rejected) connect_to(dialog, "_ZN7QDialog8rejectedEv", "_ZN7QDialog16staticMetaObjectE", rejected, arg);
    dialog->QDialog::open();
}

void nk_error(const char *title, const char *text) { ConfirmationDialogFactory::showErrorDialog(qstr(title), qstr(text)); }

const char *nk_self_path(void) { return access(INSTALLED_AT, F_OK) == 0 ? INSTALLED_AT : DISABLED_AT; }

/* Written beside the old file and renamed over it, so a lost write leaves the old one. Nickel keeps
 * running the old copy it already loaded. */
int nk_replace_self(const void *data, unsigned size) {
    failsafe_stop();
    int fd = open(INSTALLED_AT ".new", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) return 0;
    int ok = write(fd, data, size) == (ssize_t)size;
    ok = !fsync(fd) && ok;
    close(fd);
    if (!ok || rename(INSTALLED_AT ".new", INSTALLED_AT)) {
        unlink(INSTALLED_AT ".new");
        return 0;
    }
    unlink(DISABLED_AT);
    sync();
    nk_log("replaced with a new build");
    return 1;
}

void nk_reboot(void) {
    failsafe_stop();
    N3PowerWorkflowManager::sharedInstance()->reboot();
}

/* Forked twice, so init takes the command and Nickel is left no child to wait for. Between fork
 * and exec only async-signal-safe calls are made, since Nickel has other threads. */
void nk_spawn(const char *command) {
    failsafe_stop();
    pid_t child = fork();
    if (child == 0) {
        if (fork() != 0) _exit(0);
        setsid();
        if (chdir("/")) _exit(127);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execl("/bin/sh", "sh", "-c", command, (char *)0);
        _exit(127);
    }
    if (child > 0) waitpid(child, 0, 0);
    else nk_log("couldn't start %s", command);
}

/* These replace virtual functions that return a QString, so the address to build it in comes
 * first, before `this`. */
static void get_title(QString *out, void *self) {
    ScreenInfo *info = info_of(self);
    new (out) QString(info ? QString::fromUtf8(info->title) : QString());
}
static void get_small_title(QString *out, void *self) {
    ScreenInfo *info = info_of(self);
    new (out) QString(info ? QString::fromUtf8(info->small_title) : QString());
}

/* Mirrors what every settings controller's loadView does: make the view and keep a QPointer to it. */
static void load_view(void *self) {
    void **controller = static_cast<void **>(self);
    N3SettingsLoggingCategoryView *view = new (alloc(0x20)) N3SettingsLoggingCategoryView(0);
    controller[3] = QtSharedPointer::ExternalRefCountData::getAndRef(view);
    controller[4] = view;
    QObject::connect(view, &QObject::destroyed, [self]() { screen_gone(self); });
    ScreenInfo *info = info_of(self);
    if (!info) return;
    NkScreen built = {view->pagingContainer()};
    learn_labels();
    info->build(&built, info->arg);
}

/* The abstract-controller vtable's entry is a thunk: `this` points 8 bytes into the object. */
static void load_view_thunk(void *self) { load_view(static_cast<char *>(self) - 8); }

/* Not "slots": Qt defines that as nothing. */
static void **copy_vtable(void **vtable, int count) {
    void **copy = static_cast<void **>(malloc((count + 2) * sizeof(void *)));
    memcpy(copy, vtable - 2, (count + 2) * sizeof(void *));
    return copy + 2;
}

void kit_open_screen(const char *title, const char *small_title, void (*build)(NkScreen *, void *), void *arg, NkFn done) {
    void *load_real = nk_sym("_ZN35N3SettingsLoggingCategoryController8loadViewEv");
    void *title_real = nk_sym("_ZN35N3SettingsLoggingCategoryController8getTitleEv");
    void *controller_ctor = nk_sym("_ZN35N3SettingsLoggingCategoryControllerC1EP7QObject");
    if (!load_real || !title_real || !controller_ctor) {
        if (done) done(arg);
        return;
    }
    void **controller = static_cast<void **>(alloc(0x1c));
    reinterpret_cast<void (*)(void *, void *)>(controller_ctor)(controller, 0);
    void **primary = static_cast<void **>(controller[0]), **abstract = static_cast<void **>(controller[2]);
    if (primary[LOAD_VIEW] != load_real || primary[GET_TITLE] != title_real) {
        nk_log("controller layout has changed on this firmware, not opening");
        if (done) done(arg);
        return;
    }
    ScreenInfo *info = new ScreenInfo;
    info->title = title;
    info->small_title = small_title;
    info->build = build;
    info->arg = arg;
    info->done = done;
    info->handed = false;
    if (!screens) screens = new QHash<void *, ScreenInfo *>;
    if (!screen_stack) screen_stack = new QList<void *>;
    screens->insert(controller, info);
    *screen_stack << controller;
    static void **new_primary, **new_abstract;
    if (!new_primary) {
        new_primary = copy_vtable(primary, PRIMARY_SLOTS);
        new_abstract = copy_vtable(abstract, ABSTRACT_SLOTS);
        new_primary[GET_TITLE] = reinterpret_cast<void *>(get_title);
        new_primary[GET_SMALL_TITLE] = reinterpret_cast<void *>(get_small_title);
        new_primary[LOAD_VIEW] = reinterpret_cast<void *>(load_view);
        new_abstract[ABSTRACT_LOAD_VIEW] = reinterpret_cast<void *>(load_view_thunk);
    }
    controller[0] = new_primary;
    controller[2] = new_abstract;
    MainWindowController *window = MainWindowController::sharedInstance();
    /* Opening a screen is the riskiest thing a mod does, so a crash here disables it too. */
    guard_screen();
    window->push(reinterpret_cast<AbstractController *>(controller + 2), false);
    unguard_screen();
}

void nk_open_screen(const char *title, const char *small_title, void (*build)(NkScreen *, void *), void *arg) {
    kit_open_screen(title, small_title, build, arg, 0);
}

const char *kit_top_screen_title(void) {
    ScreenInfo *info = screen_stack && !screen_stack->isEmpty() ? info_of(screen_stack->last()) : 0;
    return info ? info->title.constData() : 0;
}

/* The screen opened last is closed and opened again, built afresh, with the same arg. */
void kit_reopen_screen(void) {
    void *controller = screen_stack && !screen_stack->isEmpty() ? screen_stack->last() : 0;
    ScreenInfo *info = controller ? info_of(controller) : 0;
    if (!info) return;
    ScreenInfo again = *info;
    info->handed = true;
    screen_stack->removeAll(controller);
    MainWindowController::sharedInstance()->pop(reinterpret_cast<AbstractController *>(static_cast<void **>(controller) + 2));
    kit_open_screen(again.title.constData(), again.small_title.constData(), again.build, again.arg, again.done);
}

/* ---- loading, as an image-format plugin ---- */

/* Qt reads this block out of the file without loading it, then loads every image-format plugin
 * it finds in /usr/local/Kobo/imageformats. It's the header from Kobo's own libkimg.so with the
 * key and names changed, so the format never matches a real image. build.sh puts the mod's own
 * names in it. */
__attribute__((used, section(".qtmetadata"))) static const unsigned char plugin_metadata[] = {
#include <metadata.inc>
};

extern "C" __attribute__((visibility("default"))) const char *qt_plugin_query_metadata(void) {
    return reinterpret_cast<const char *>(plugin_metadata);
}

extern "C" __attribute__((visibility("default"))) void *qt_plugin_instance(void) { return 0; }

/* A mod that crashes or freezes Nickel mustn't stop the Kobo starting. The file is moved away while
 * it's being tried, so a crash, or a freeze that ends with the power held off, means it isn't loaded
 * next time. It's put back once Nickel's main thread has carried on for a quiet while: the guard's
 * thread only puts it back after a heartbeat from the main thread's event loop, so time passing
 * while Nickel is frozen doesn't count. If the main thread hasn't run for FREEZE_SECONDS while the
 * guard is on, Nickel is frozen, and the guard's thread stops it, as a crash would, so the Kobo starts
 * again without the mod and nobody has to hold the power off. Each move is synced, because a frozen
 * Kobo gets held off, and an unsynced move can be lost with the power. */
static void failsafe_arm(void) { rename(INSTALLED_AT, DISABLED_AT); sync(); }
static void failsafe_clear(void) { rename(DISABLED_AT, INSTALLED_AT); sync(); }

static pthread_mutex_t failsafe_lock = PTHREAD_MUTEX_INITIALIZER;
static long failsafe_until; /* while this is set, a thread clears the guard once it's passed */
static long alive_at;       /* when the main thread's event loop last ran, from its heartbeat */
static long armed_at;       /* when the guard last went on */
static bool beating;        /* the heartbeat is running, so a quiet main thread means a freeze */
/* Far longer than Nickel takes to start, or ever keeps its main thread busy. */
enum { FREEZE_SECONDS = 180 };

static long now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

static void *failsafe_wait(void *) {
    for (;;) {
        pthread_mutex_lock(&failsafe_lock);
        long left = failsafe_until - now();
        if (left <= 0 && __atomic_load_n(&alive_at, __ATOMIC_ACQUIRE) >= failsafe_until) {
            failsafe_until = 0;
            failsafe_clear();
            pthread_mutex_unlock(&failsafe_lock);
            nk_log("failsafe cleared");
            return 0;
        }
        long alive = __atomic_load_n(&alive_at, __ATOMIC_ACQUIRE);
        long quiet = now() - (alive > armed_at ? alive : armed_at);
        pthread_mutex_unlock(&failsafe_lock);
        if (beating && quiet >= FREEZE_SECONDS) {
            nk_log("Nickel hasn't run for %ld seconds while the guard was on, so it's stopped, with the mod set aside", quiet);
            sync();
            abort();
        }
        sleep(left > 0 ? left : 1);
    }
}

/* The main thread's heartbeat: a timer on its event loop, which only fires while Nickel is running.
 * Without Qt's application yet there's no event loop to prove, so the guard stays set, and the mod
 * isn't loaded next time, which the log says. */
static void start_heartbeat(void) {
    if (!QCoreApplication::instance()) {
        nk_log("no Qt application yet, so the guard can't tell Nickel is running and stays set");
        return;
    }
    QTimer *beat = new QTimer();
    QObject::connect(beat, &QTimer::timeout, [] { __atomic_store_n(&alive_at, now(), __ATOMIC_RELEASE); });
    beat->start(1000);
    beating = true;
}

void nk_failsafe(int seconds) {
    pthread_mutex_lock(&failsafe_lock);
    if (!failsafe_until) {
        failsafe_arm();
        armed_at = now();
        pthread_t thread;
        pthread_create(&thread, 0, failsafe_wait, 0);
        pthread_detach(thread);
    }
    if (now() + seconds > failsafe_until) failsafe_until = now() + seconds;
    pthread_mutex_unlock(&failsafe_lock);
}

/* A restart Nickel chooses isn't a crash, so it mustn't disable the mod. A crash or abort never
 * runs atexit handlers, so those are still caught. */
static void failsafe_stop(void) {
    pthread_mutex_lock(&failsafe_lock);
    if (failsafe_until) {
        failsafe_until = 0;
        failsafe_clear();
    }
    pthread_mutex_unlock(&failsafe_lock);
}

/* A screen opening while the guard is already timed leaves the guard to its thread. */
static void guard_screen(void) {
    pthread_mutex_lock(&failsafe_lock);
    if (!failsafe_until) failsafe_arm();
    pthread_mutex_unlock(&failsafe_lock);
}

static void unguard_screen(void) {
    pthread_mutex_lock(&failsafe_lock);
    if (!failsafe_until) failsafe_clear();
    pthread_mutex_unlock(&failsafe_lock);
}

__attribute__((constructor)) static void load(void) {
    /* Other Qt programs on the Kobo load image plugins too. */
    if (!strstr(program_invocation_short_name, "nickel")) return;
    if (access(UNINSTALL_FLAG, F_OK) == 0) {
        unlink(INSTALLED_AT);
        unlink(DISABLED_AT);
        unlink(UNINSTALL_FLAG);
        nk_log("uninstalled");
        return;
    }
    if (!all_symbols_present()) {
        nk_log("this firmware is missing something, so the mod does nothing");
        return;
    }
    nk_failsafe(FAILSAFE_SECONDS);
    atexit(failsafe_stop);
    start_heartbeat();
    nk_log("loaded");
    nk_mod_init();
}
