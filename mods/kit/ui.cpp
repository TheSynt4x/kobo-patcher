/* Kobo's own widgets as blocks for a Lua mod's pages, and a tab of the mod's own in Kobo's bottom
 * bar. Every block is one of Kobo's classes, so Kobo's stylesheets style it as they do their own.
 *
 * - Labels like HeaderMediumLabel only give a base class a name for the stylesheets to select on,
 *   and have no constructor of their own. A label is made as its base, QLabel or ElidedLabel, and
 *   given the class's vtables, which libnickel exports. The class is checked to be a direct
 *   subclass of that base first, so no firmware gets a label that's more than it seems.
 * - Text boxes are Kobo's SearchEditBox and TouchLineEdit, typed in with Kobo's keyboard: the one
 *   a screen's N3Dialog keeps, or on a tab one over the page's lower part, as Kobo's search has.
 * - The tab is a MainNavButton after Kobo's Discover button, with Discover's icons and name, so the
 *   no-discover patch can make room for it. Its page is a MoreController whose loadView makes the
 *   page here and whose navSection is Discover's, so Nickel lights the tab while it's shown.
 * - Pages turn as Kobo's settings do: a PagingContainer, moved a page at a time by dragging.
 * - Nickel's functions are looked up by name when they're first used, not linked, so a firmware
 *   without one loses only what needs it. Mods that use none of this, like Kobo Patcher, still load.
 *
 * As in lua.cpp, nothing raises a Lua error while a Qt value made here is still alive. */
#include <QApplication>
#include <QBoxLayout>
#include <QDesktopWidget>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QGestureEvent>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPixmap>
#include <QPointer>
#include <QSharedPointer>
#include <QTimer>
#include <dlfcn.h>
#include <string.h>
#include "kit.h"
#include "lua_kit.h"

class AbstractController;
class MainWindowController {
public:
    static MainWindowController *sharedInstance();
};

/* Sizes are for the Nia's 758 pixels across, and scaled for larger screens. */
static int px(int value) {
    QDesktopWidget *desktop = QApplication::desktop();
    qreal scale = desktop && desktop->width() > 0 ? qMax<qreal>(1, qMin(desktop->width(), desktop->height()) / 758.0) : 1;
    return qRound(value * scale);
}

/* Zeroed memory from Nickel's operator new. Sizes here have room to spare over what Nickel's own
 * code allocates on 4.38, in case a later firmware's objects are larger. */
static void *alloc(size_t size) {
    void *memory = operator new(size);
    memset(memory, 0, size);
    return memory;
}

static void *nickel(const char *symbol) { return nk_sym(symbol); }

static QByteArray mangled(const char *prefix, const char *name, const char *suffix) {
    return QByteArray(prefix) + QByteArray::number(int(strlen(name))) + name + suffix;
}

/* ---- Kobo's labels ---- */

/* The vtable pointers a widget of the class holds: at +0 for QObject, and at +8 for QPaintDevice,
 * whose part of the vtable group follows an offset-to-top of -8 and the same type info. */
static bool vptrs_of(const char *name, void *out[2]) {
    void **group = static_cast<void **>(dlsym(RTLD_DEFAULT, mangled("_ZTV", name, "").constData()));
    if (!group) return false;
    for (int i = 3; i < 200; i++)
        if (group[i] == reinterpret_cast<void *>(intptr_t(-8)) && group[i + 1] == group[1]) {
            out[0] = group + 2;
            out[1] = group + i + 2;
            return true;
        }
    return false;
}

/* Whether the class is base with a new name and nothing more. */
static bool renames(const char *name, const char *base) {
    const QMetaObject *meta = static_cast<const QMetaObject *>(dlsym(RTLD_DEFAULT, mangled("_ZN", name, "16staticMetaObjectE").constData()));
    return meta && meta->superClass() && !strcmp(meta->superClass()->className(), base);
}

/* The label classes blocks are made of, by the base each renames. */
static const struct {
    const char *name;
    bool elided;
} label_classes[] = {
    {"HeaderMediumLabel", false},     {"RegularLabel", false},      {"MetaDataLabel", false},
    {"HeaderSmallElidedLabel", true}, {"SmallElidedLabel", true},
};

typedef void (*WidgetCtor)(void *self, QWidget *parent);
typedef void (*SetText)(void *self, const QString &text);

void kit_set_text(QLabel *label, const QString &text) {
    static SetText elided_set;
    if (!elided_set) elided_set = reinterpret_cast<SetText>(nickel("_ZN11ElidedLabel7setTextERK7QString"));
    if (label->inherits("ElidedLabel") && elided_set) elided_set(label, text);
    else label->setText(text);
}

/* One of Kobo's labels, or a plain one if this firmware's isn't as expected. */
static QLabel *kobo_label(QWidget *parent, const char *name, const QString &text) {
    bool elided = false, known = false;
    for (unsigned i = 0; i < sizeof label_classes / sizeof *label_classes; i++)
        if (!strcmp(label_classes[i].name, name)) known = true, elided = label_classes[i].elided;
    void *vptrs[2];
    bool styled = known && vptrs_of(name, vptrs) && renames(name, elided ? "ElidedLabel" : "QLabel");
    static WidgetCtor elided_ctor;
    if (!elided_ctor) elided_ctor = reinterpret_cast<WidgetCtor>(nickel("_ZN11ElidedLabelC1EP7QWidget"));
    QLabel *label;
    if (styled && elided && elided_ctor) {
        label = static_cast<QLabel *>(alloc(0x200)); /* 0xe8 on 4.38 */
        elided_ctor(label, parent);
    } else {
        styled = styled && !elided;
        label = new (alloc(0x40)) QLabel(parent, 0);
    }
    if (styled) {
        reinterpret_cast<void **>(label)[0] = vptrs[0];
        reinterpret_cast<void **>(label)[2] = vptrs[1];
    } else nk_log("%s isn't as expected on this firmware, using a plain label", name);
    kit_set_text(label, text);
    return label;
}

/* ---- images ---- */

/* An image a mod gave, by its path on the Kobo: under /mnt/onboard, or in the mod's own folder. */
static QString image_path(const char *given) {
    QString path = QString::fromUtf8(given);
    if (!path.startsWith(QLatin1Char('/'))) path = QStringLiteral(NK_FOLDER "/") + path;
    return path.startsWith(QLatin1String("/mnt/onboard/")) && !path.contains(QLatin1String("/../")) ? path : QString();
}

/* Covers decode slowly on a Kobo, so each is kept at the sizes it's been shown at, a few at a time. */
static QHash<QString, QPixmap> *scaled_images;

static void show_image(QLabel *label, const QString &path, int width, int height) {
    if (!scaled_images) scaled_images = new QHash<QString, QPixmap>;
    QString key = path + QStringLiteral("@%1x%2").arg(width).arg(height);
    QPixmap pixmap = scaled_images->value(key);
    /* By what the file holds, not its name: a cover saved as .jpg can be a PNG. */
    QFile file(path);
    if (pixmap.isNull() && !path.isEmpty() && file.open(QIODevice::ReadOnly) && pixmap.loadFromData(file.readAll())) {
        pixmap = pixmap.scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (scaled_images->size() >= 120) scaled_images->clear();
        scaled_images->insert(key, pixmap);
    }
    if (pixmap.isNull()) {
        /* Until there's a cover, an empty one the size of a book. */
        label->clear();
        label->setStyleSheet(QStringLiteral("border: 1px solid black;"));
    } else {
        label->setStyleSheet(QString());
        label->setPixmap(pixmap);
    }
}

/* page:cover(path, height): a book's cover, centred. Its handle's :set(path) changes it. */
static int page_cover(lua_State *co) {
    PagingContainer *paging = kit_check_page(co);
    const char *given = luaL_optstring(co, 2, "");
    int height = (int)luaL_optinteger(co, 3, 354);
    QLabel *cover = new (alloc(0x40)) QLabel(paging, 0);
    cover->setAlignment(Qt::AlignCenter);
    cover->setFixedHeight(px(height));
    cover->setProperty("nkWidth", px(height * 2 / 3));
    show_image(cover, image_path(given), px(height * 2 / 3), px(height));
    paging->addWidget(cover);
    kit_push_handle(co, cover, "nk.image");
    return 1;
}

static int image_set(lua_State *co) {
    QLabel *cover = static_cast<QLabel *>(kit_check_handle(co, 1, "nk.image"));
    show_image(cover, image_path(luaL_optstring(co, 2, "")), cover->property("nkWidth").toInt(), cover->height());
    return 0;
}

/* ---- headings ---- */

/* page:heading(text): a large title, as at the top of Kobo's More page. */
static int page_heading(lua_State *co) {
    PagingContainer *paging = kit_check_page(co);
    const char *text = luaL_checkstring(co, 2);
    QLabel *heading = kobo_label(paging, "HeaderMediumLabel", QString::fromUtf8(text));
    heading->setWordWrap(true);
    paging->addWidget(heading);
    kit_push_handle(co, heading, "nk.label");
    return 1;
}

/* ---- books ---- */

/* A books block's rows, kept to change them. It goes with the page. */
class Books : public QObject {
public:
    explicit Books(QObject *parent) : QObject(parent), hook(0) {}
    struct Row {
        QPointer<QLabel> cover, title, author, status, badge;
    };
    QList<Row> rows;
    KitHook *hook;
};

struct Tap {
    QPointer<Books> books;
    int index;
};

static void book_tapped(void *arg) {
    Tap *tap = static_cast<Tap *>(arg);
    lua_State *co = tap->books ? kit_hook_task(tap->books->hook) : 0;
    if (!co) return;
    lua_pushinteger(co, tap->index + 1);
    kit_resume(co, 1);
}

/* A string field of the table at `index`, if it has one. */
static bool field_of(lua_State *co, int index, const char *name, QString *out) {
    bool found = lua_getfield(co, index, name) == LUA_TSTRING;
    if (found) *out = QString::fromUtf8(lua_tostring(co, -1));
    lua_pop(co, 1);
    return found;
}

static void fill_row(lua_State *co, int book, Books::Row &row) {
    QString text;
    if (field_of(co, book, "title", &text) && row.title) kit_set_text(row.title, text);
    if (field_of(co, book, "author", &text) && row.author) kit_set_text(row.author, text);
    if (field_of(co, book, "status", &text) && row.status) row.status->setText(text);
    if (field_of(co, book, "badge", &text) && row.badge) row.badge->setText(text);
    if (field_of(co, book, "cover", &text) && row.cover) show_image(row.cover, image_path(text.toUtf8().constData()), px(70), px(110));
}

typedef void (*TouchCtor)(void *self, QWidget *parent, int flags);

/* A row as My Books lists a book: its cover, title and author, and on the right its state and
 * format. The row is a TouchLabel, Kobo's tappable label, holding the rest. */
static Books::Row add_book_row(PagingContainer *paging, Books *books, int index) {
    static TouchCtor touch_ctor;
    if (!touch_ctor) touch_ctor = reinterpret_cast<TouchCtor>(nickel("_ZN10TouchLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE"));
    QLabel *row;
    if (touch_ctor) {
        row = static_cast<QLabel *>(alloc(0x100)); /* 0x84 on 4.38 */
        touch_ctor(row, paging, 0);
    } else row = new (alloc(0x40)) QLabel(paging, 0);
    row->setObjectName(QStringLiteral("nkBook"));
    row->setFixedHeight(px(140));
    QVBoxLayout *outer = new QVBoxLayout(row);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    QHBoxLayout *line = new QHBoxLayout();
    line->setContentsMargins(px(15), px(15), px(15), px(15));
    line->setSpacing(px(15));
    outer->addLayout(line, 1);
    QFrame *rule = new QFrame(row);
    rule->setFixedHeight(1);
    rule->setStyleSheet(QStringLiteral("background-color: black;"));
    outer->addWidget(rule);

    Books::Row parts;
    QLabel *cover = new (alloc(0x40)) QLabel(row, 0);
    cover->setFixedSize(px(70), px(110));
    cover->setAlignment(Qt::AlignCenter);
    parts.cover = cover;
    line->addWidget(cover, 0, 0);

    QVBoxLayout *middle = new QVBoxLayout();
    middle->addStretch(1);
    parts.title = kobo_label(row, "HeaderSmallElidedLabel", QString());
    parts.author = kobo_label(row, "SmallElidedLabel", QString());
    middle->addWidget(parts.title, 0, 0);
    middle->addWidget(parts.author, 0, 0);
    middle->addStretch(1);
    line->addLayout(middle, 1);

    QVBoxLayout *right = new QVBoxLayout();
    parts.status = kobo_label(row, "RegularLabel", QString());
    parts.badge = kobo_label(row, "MetaDataLabel", QString());
    parts.status->setAlignment(Qt::AlignRight);
    parts.badge->setAlignment(Qt::AlignRight);
    right->addWidget(parts.status, 0, 0);
    right->addWidget(parts.badge, 0, 0);
    right->addStretch(1);
    line->addLayout(right, 0);

    Tap *tap = new Tap();
    tap->books = books;
    tap->index = index;
    QObject::connect(row, &QObject::destroyed, [tap]() { delete tap; });
    if (touch_ctor) nk_connect(row, "_ZN10TouchLabel6tappedEb", "_ZN10TouchLabel16staticMetaObjectE", book_tapped, tap);
    /* Kobo's elided labels can take a tap themselves. */
    QLabel *texts[] = {parts.title, parts.author};
    for (int i = 0; i < 2; i++)
        if (texts[i]->inherits("ElidedLabel")) nk_connect(texts[i], "_ZN11ElidedLabel6tappedEv", "_ZN11ElidedLabel16staticMetaObjectE", book_tapped, tap);
    paging->addWidget(row);
    return parts;
}

/* page:books(list, tapped): a row for each book, as My Books lists them. A book is a table of
 * title, author, cover (a path), status and badge. tapped(i) runs when the i-th is tapped. The
 * handle's :set(i, book) changes the fields given. */
static int page_books(lua_State *co) {
    PagingContainer *paging = kit_check_page(co);
    luaL_checktype(co, 2, LUA_TTABLE);
    luaL_checktype(co, 3, LUA_TFUNCTION);
    Books *books = new Books(paging);
    books->hook = kit_hook(co, 3, books);
    lua_Integer count = luaL_len(co, 2);
    for (lua_Integer i = 1; i <= count && i <= 500; i++) {
        lua_geti(co, 2, i);
        if (lua_istable(co, -1)) {
            books->rows << add_book_row(paging, books, books->rows.size());
            fill_row(co, lua_gettop(co), books->rows.last());
        }
        lua_pop(co, 1);
    }
    kit_push_handle(co, books, "nk.books");
    return 1;
}

static int books_set(lua_State *co) {
    Books *books = static_cast<Books *>(kit_check_handle(co, 1, "nk.books"));
    lua_Integer i = luaL_checkinteger(co, 2);
    luaL_checktype(co, 3, LUA_TTABLE);
    if (i >= 1 && i <= books->rows.size()) fill_row(co, 3, books->rows[int(i - 1)]);
    return 0;
}

/* ---- text boxes and the keyboard ---- */

struct Field {
    QPointer<QLineEdit> edit;
    QObject *receiver;
    KitHook *go;
};
static QHash<QObject *, Field *> *fields; /* by text box */

/* Kobo's keyboard where a text box is: its N3Dialog's, or one of our own on a tab. */
struct Keys {
    QPointer<QWidget> host, frame;
    QObject *controller;
    bool own;
    QPointer<QLineEdit> typing;
};
static QHash<QObject *, Keys *> *keyboards; /* by host */

static QWidget *host_of(QWidget *widget) {
    QWidget *top = widget;
    for (QWidget *w = widget; w; w = w->parentWidget()) {
        if (w->inherits("N3Dialog") || w->objectName() == QLatin1String("nkTabPage")) return w;
        top = w;
    }
    return top;
}

static void show_keys(Keys *keys, bool show) {
    if (!keys->frame) return;
    if (!keys->own) {
        QMetaObject::invokeMethod(keys->host, show ? "showKeyboard" : "hideKeyboard");
        return;
    }
    if (!show) {
        keys->frame->hide();
        return;
    }
    int height = keys->frame->sizeHint().height();
    keys->frame->setGeometry(0, keys->host->height() - height, keys->host->width(), height);
    keys->frame->raise();
    keys->frame->show();
}

/* Go on the keyboard: it goes, and the text box's go(text) runs. */
static void committed(void *arg) {
    Keys *keys = static_cast<Keys *>(arg);
    QLineEdit *edit = keys->typing;
    show_keys(keys, false);
    Field *field = edit && fields ? fields->value(edit) : 0;
    if (!field) return;
    edit->clearFocus();
    lua_State *co = kit_hook_task(field->go);
    if (!co) return;
    QByteArray text = edit->text().toUtf8();
    lua_pushlstring(co, text.constData(), text.size());
    kit_resume(co, 1);
}

typedef QObject *(*CreateKeyboard)(void *frame, int script, const QLocale &locale);
typedef QWidget *(*KeyboardFrameOf)(void *dialog);
enum { LATIN_KEYBOARD = 1 };

static Keys *keys_for(QWidget *host) {
    if (!keyboards) keyboards = new QHash<QObject *, Keys *>;
    if (keyboards->contains(host)) return keyboards->value(host);
    CreateKeyboard create = reinterpret_cast<CreateKeyboard>(nickel("_ZN13KeyboardFrame14createKeyboardE14KeyboardScriptRK7QLocale"));
    WidgetCtor frame_ctor = reinterpret_cast<WidgetCtor>(nickel("_ZN13KeyboardFrameC1EP7QWidget"));
    KeyboardFrameOf dialog_frame = reinterpret_cast<KeyboardFrameOf>(nickel("_ZN8N3Dialog13keyboardFrameEv"));
    if (!create || !frame_ctor) return 0;
    Keys *keys = new Keys();
    keys->host = host;
    if (host->inherits("N3Dialog") && dialog_frame) keys->frame = dialog_frame(host);
    if (!keys->frame) {
        /* Over the page's lower part rather than in its layout, so what's above isn't squeezed. */
        QWidget *frame = static_cast<QWidget *>(alloc(0x100)); /* 0x1c on 4.38 */
        frame_ctor(frame, host);
        frame->hide();
        keys->frame = frame;
        keys->own = true;
    }
    keys->controller = create(keys->frame, LATIN_KEYBOARD, QLocale());
    if (keys->controller)
        nk_connect(keys->controller, "_ZN18KeyboardController15commitRequestedEv", "_ZN18KeyboardController16staticMetaObjectE", committed, keys);
    keyboards->insert(host, keys);
    QObject::connect(host, &QObject::destroyed, [host, keys]() {
        keyboards->remove(host);
        delete keys;
    });
    return keys;
}

typedef void (*SetReceiver)(QObject *controller, QObject *receiver, bool unknown);

/* A text box was tapped: the keyboard types in it. */
static void start_typing(void *arg) {
    Field *field = static_cast<Field *>(arg);
    if (!field->edit) return;
    static SetReceiver set_receiver;
    if (!set_receiver) set_receiver = reinterpret_cast<SetReceiver>(nickel("_ZN24SearchKeyboardController11setReceiverEP16KeyboardReceiverb"));
    Keys *keys = keys_for(host_of(field->edit));
    if (!keys || !keys->controller || !set_receiver || !field->receiver) return;
    set_receiver(keys->controller, field->receiver, true);
    keys->typing = field->edit;
    field->edit->setFocus();
    show_keys(keys, true);
}

typedef void (*ReceiverCtor)(void *self, QLineEdit *edit, bool unknown);
typedef void (*SetBool)(void *self, bool on);

/* page:search(placeholder, go, text) and page:field(placeholder, go, text): Kobo's search box, or
 * its plain text box. Tapping it brings up Kobo's keyboard, and go(text) runs on its Go. The
 * handle has :text() and :set(text). */
static int text_box(lua_State *co, bool search) {
    PagingContainer *paging = kit_check_page(co);
    const char *placeholder = luaL_checkstring(co, 2);
    luaL_checktype(co, 3, LUA_TFUNCTION);
    const char *text = luaL_optstring(co, 4, "");
    WidgetCtor ctor = reinterpret_cast<WidgetCtor>(nickel(search ? "_ZN13SearchEditBoxC1EP7QWidget" : "_ZN13TouchLineEditC1EP7QWidget"));
    SetText set_placeholder = reinterpret_cast<SetText>(nickel("_ZN13TouchLineEdit24setCustomPlaceholderTextERK7QString"));
    ReceiverCtor receiver_ctor = reinterpret_cast<ReceiverCtor>(nickel("_ZN16KeyboardReceiverC1EP9QLineEditb"));
    QLineEdit *edit;
    if (ctor) {
        edit = static_cast<QLineEdit *>(alloc(0x200)); /* 0x94 and 0x40 on 4.38 */
        ctor(edit, paging);
    } else edit = new (alloc(0x80)) QLineEdit(paging);
    if (search && edit->inherits("SearchEditBox")) {
        SetBool show_type = reinterpret_cast<SetBool>(nickel("_ZN13SearchEditBox17setShowSearchTypeEb"));
        if (show_type) show_type(edit, false);
    }
    if (edit->inherits("TouchLineEdit") && set_placeholder) set_placeholder(edit, QString::fromUtf8(placeholder));
    else edit->setPlaceholderText(QString::fromUtf8(placeholder));
    edit->setText(QString::fromUtf8(text));
    Field *field = new Field();
    field->edit = edit;
    field->go = kit_hook(co, 3, edit);
    if (receiver_ctor) {
        field->receiver = static_cast<QObject *>(alloc(0x100)); /* 0x24 on 4.38 */
        receiver_ctor(field->receiver, edit, false);
        field->receiver->setParent(edit);
    }
    if (!fields) fields = new QHash<QObject *, Field *>;
    fields->insert(edit, field);
    QObject::connect(edit, &QObject::destroyed, [edit, field]() {
        fields->remove(edit);
        delete field;
    });
    if (edit->inherits("TouchLineEdit")) nk_connect(edit, "_ZN13TouchLineEdit6tappedEv", "_ZN13TouchLineEdit16staticMetaObjectE", start_typing, field);
    paging->addWidget(edit);
    kit_push_handle(co, edit, "nk.field");
    return 1;
}

static int page_search(lua_State *co) { return text_box(co, true); }
static int page_field(lua_State *co) { return text_box(co, false); }

static int field_text(lua_State *co) {
    QByteArray text = static_cast<QLineEdit *>(kit_check_handle(co, 1, "nk.field"))->text().toUtf8();
    lua_pushlstring(co, text.constData(), text.size());
    return 1;
}

static int field_set(lua_State *co) {
    QLineEdit *edit = static_cast<QLineEdit *>(kit_check_handle(co, 1, "nk.field"));
    edit->setText(QString::fromUtf8(luaL_checkstring(co, 2)));
    return 0;
}

/* ---- the tab ---- */

typedef int (*IntOf)(const void *self);
typedef void (*Turn)(void *self);
typedef void (*PointOf)(QPoint *out, const void *self);

/* A tab's page: a PagingContainer that turns a page for each drag up or down, or across, with
 * "Page 2 of 5" under it while there's more than one. */
class TabPage : public QWidget {
public:
    TabPage() : QWidget(0) {}
    QPointer<QWidget> paging;
    QPointer<QLabel> pages;

    void count_pages() {
        static IntOf page_of, count_of;
        if (!page_of) page_of = reinterpret_cast<IntOf>(nickel("_ZNK15PagingContainer7getPageEv"));
        if (!count_of) count_of = reinterpret_cast<IntOf>(nickel("_ZNK15PagingContainer8numPagesEv"));
        if (!paging || !pages || !page_of || !count_of) return;
        int page = page_of(paging), count = count_of(paging);
        if (page < count) page++; /* from 0 */
        pages->setText(QStringLiteral("Page %1 of %2").arg(page).arg(count));
        pages->setVisible(count > 1);
    }

protected:
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        /* The pages are counted once they're laid out, which is after they're shown. */
        QPointer<TabPage> self(this);
        QTimer *soon = new QTimer();
        soon->setSingleShot(true);
        QObject::connect(soon, &QTimer::timeout, [self, soon]() {
            soon->deleteLater();
            if (self) self->count_pages();
        });
        soon->start(0);
    }

    bool event(QEvent *event) override {
        static int *drag_type;
        static PointOf begin, end;
        if (!drag_type) drag_type = static_cast<int *>(nickel("_ZN21DragGestureRecognizer12_gestureTypeE"));
        if (!begin) begin = reinterpret_cast<PointOf>(nickel("_ZNK11DragGesture5beginEv"));
        if (!end) end = reinterpret_cast<PointOf>(nickel("_ZNK11DragGesture3endEv"));
        if (event->type() != QEvent::Gesture || !drag_type || !begin || !end) return QWidget::event(event);
        QGestureEvent *gestures = static_cast<QGestureEvent *>(event);
        QGesture *drag = gestures->gesture(Qt::GestureType(*drag_type));
        if (!drag) return QWidget::event(event);
        gestures->accept(drag);
        if (drag->state() == Qt::GestureFinished && paging) {
            QPoint from, to;
            begin(&from, drag);
            end(&to, drag);
            QPoint moved = from - to;
            int along = qAbs(moved.y()) >= qAbs(moved.x()) ? moved.y() : moved.x();
            static Turn next, prev;
            if (!next) next = reinterpret_cast<Turn>(nickel("_ZN15PagingContainer8nextPageEv"));
            if (!prev) prev = reinterpret_cast<Turn>(nickel("_ZN15PagingContainer8prevPageEv"));
            if (qAbs(along) > px(40) && next && prev) {
                (along > 0 ? next : prev)(paging);
                count_pages();
            }
        }
        return true;
    }
};

static struct {
    char *title; /* or 0 for Discover's own */
    int build;
    QPointer<QWidget> button;
    bool discover_active, open;
} tab;

static void light_buttons();

static TabPage *make_tab_page() {
    TabPage *page = new TabPage();
    page->setObjectName(QStringLiteral("nkTabPage"));
    page->setAttribute(Qt::WA_StyledBackground);
    page->setStyleSheet(QStringLiteral("#nkTabPage { background-color: white; }"));
    /* What Kobo reads off a view to show its status bar: the clock, Wi-Fi and battery. */
    page->setProperty("showTitle", true);
    page->setProperty("showIndicators", true);
    page->setProperty("showStatusBottomLine", false);
    static int *drag_type;
    if (!drag_type) drag_type = static_cast<int *>(nickel("_ZN21DragGestureRecognizer12_gestureTypeE"));
    if (drag_type) page->grabGesture(Qt::GestureType(*drag_type));
    QVBoxLayout *column = new QVBoxLayout(page);
    /* Below the status bar, with the margins of Kobo's More page. */
    column->setContentsMargins(px(40), px(84), px(40), px(8));
    column->setSpacing(0);
    WidgetCtor paging_ctor = reinterpret_cast<WidgetCtor>(nickel("_ZN15PagingContainerC1EP7QWidget"));
    if (!paging_ctor) return page;
    QWidget *paging = static_cast<QWidget *>(alloc(0x100)); /* 0x30 on 4.38 */
    paging_ctor(paging, page);
    page->paging = paging;
    column->addWidget(paging, 1);
    QLabel *pages = kobo_label(page, "MetaDataLabel", QString());
    pages->setAlignment(Qt::AlignRight);
    pages->hide();
    page->pages = pages;
    column->addWidget(pages, 0);
    kit_build_page(static_cast<PagingContainer *>(paging), tab.build, true);
    /* Both are Discover to Kobo, so only the one showing is lit. */
    tab.open = true;
    light_buttons();
    QObject::connect(page, &QObject::destroyed, [] {
        tab.open = false;
        light_buttons();
    });
    return page;
}

/* The controller's loadView, as MoreController's: make the view and keep a QPointer to it. */
static void tab_load_view(void *self) {
    void **controller = static_cast<void **>(self);
    QWidget *page = make_tab_page();
    controller[3] = QtSharedPointer::ExternalRefCountData::getAndRef(page);
    controller[4] = page;
}
static void tab_load_view_thunk(void *self) { tab_load_view(static_cast<char *>(self) - 8); }
enum { DISCOVER_SECTION = 5, VTABLE_SLOTS = 64 };
static int tab_section(const void *) { return DISCOVER_SECTION; }

static void **copy_vtable(void **vtable) {
    void **copy = static_cast<void **>(malloc((VTABLE_SLOTS + 2) * sizeof(void *)));
    memcpy(copy, vtable - 2, (VTABLE_SLOTS + 2) * sizeof(void *));
    return copy + 2;
}

/* Points every slot holding `from` at `to`, and says how many there were. */
static int replace_slot(void **vtable, void *from, void *to) {
    int found = 0;
    for (int i = 0; i < VTABLE_SLOTS; i++)
        if (from && vtable[i] == from) vtable[i] = to, found++;
    return found;
}

typedef void (*Clear)(MainWindowController *window, AbstractController *controller, bool animate);

/* What Kobo's More tab does: a new controller replaces what's above the bar. */
static void open_tab(void *) {
    if (!kit_ready_to_open()) return;
    void *ctor = nickel("_ZN14MoreControllerC1Ev"), *load = nickel("_ZN14MoreController8loadViewEv");
    void *load_thunk = nickel("_ZThn8_N14MoreController8loadViewEv"), *section = nickel("_ZNK14MoreController10navSectionEv");
    void *section_thunk = nickel("_ZThn8_NK14MoreController10navSectionEv");
    Clear clear = reinterpret_cast<Clear>(nickel("_ZN20MainWindowController5clearEP18AbstractControllerb"));
    if (!ctor || !load || !load_thunk || !section || !section_thunk || !clear) return;
    void **controller = static_cast<void **>(alloc(0x100)); /* 0x38 on 4.38 */
    reinterpret_cast<void (*)(void *)>(ctor)(controller);
    static void **primary, **abstract;
    if (!primary) {
        void **new_primary = copy_vtable(static_cast<void **>(controller[0]));
        void **new_abstract = copy_vtable(static_cast<void **>(controller[2]));
        int found = replace_slot(new_primary, load, reinterpret_cast<void *>(tab_load_view)) +
                    replace_slot(new_abstract, load_thunk, reinterpret_cast<void *>(tab_load_view_thunk));
        replace_slot(new_primary, section, reinterpret_cast<void *>(tab_section));
        replace_slot(new_abstract, section_thunk, reinterpret_cast<void *>(tab_section));
        if (found != 2) {
            nk_log("MoreController isn't as expected, not opening the tab");
            return;
        }
        primary = new_primary;
        abstract = new_abstract;
    }
    controller[0] = primary;
    controller[2] = abstract;
    nk_failsafe(30);
    clear(MainWindowController::sharedInstance(), reinterpret_cast<AbstractController *>(controller + 2), false);
}

void kit_reopen_tab() { open_tab(0); }

typedef void (*ViewCtor)(void *self, QWidget *parent);
typedef void (*SetActive)(void *self, bool active);
static ViewCtor real_nav_view;
static SetActive real_discover_active;

/* Kobo's bottom bar: the tab goes after Kobo's Discover button, with its share of the bar. */
static void hooked_nav_view(void *self, QWidget *parent) {
    real_nav_view(self, parent);
    QWidget *view = static_cast<QWidget *>(self);
    QWidget *discover = view->findChild<QWidget *>(QStringLiteral("discoverButton"));
    QBoxLayout *row = view->findChild<QBoxLayout *>(QStringLiteral("mainNavLayout"));
    WidgetCtor button_ctor = reinterpret_cast<WidgetCtor>(nickel("_ZN13MainNavButtonC1EP7QWidget"));
    SetText set_pixmap = reinterpret_cast<SetText>(nickel("_ZN13MainNavButton9setPixmapERK7QString"));
    SetText set_active_pixmap = reinterpret_cast<SetText>(nickel("_ZN13MainNavButton15setActivePixmapERK7QString"));
    SetText set_text = reinterpret_cast<SetText>(nickel("_ZN13MainNavButton7setTextERK7QString"));
    if (!discover || !row || !button_ctor || !set_pixmap || !set_active_pixmap || !set_text) {
        nk_log("the bottom bar isn't as expected, no tab");
        return;
    }
    QString text = tab.title ? QString::fromUtf8(tab.title) : QStringLiteral("Discover");
    QList<QLabel *> labels = discover->findChildren<QLabel *>();
    if (!tab.title && !labels.isEmpty() && !labels.last()->text().isEmpty()) text = labels.last()->text();
    QWidget *button = static_cast<QWidget *>(alloc(0x100)); /* 0x7c on 4.38 */
    button_ctor(button, view);
    button->setObjectName(QStringLiteral("nkTabButton"));
    set_pixmap(button, QStringLiteral(":/images/home/main_nav_discover.png"));
    set_active_pixmap(button, QStringLiteral(":/images/home/main_nav_discover_active.png"));
    set_text(button, text);
    int at = row->indexOf(discover);
    button->setSizePolicy(discover->sizePolicy());
    button->setMinimumSize(discover->minimumSize());
    button->setMaximumSize(discover->maximumSize());
    row->insertWidget(at + 1, button, row->stretch(at));
    nk_connect(button, "_ZN13MainNavButton6tappedEv", "_ZN13MainNavButton16staticMetaObjectE", open_tab, 0);
    tab.button = button;
    nk_log("tab added");
}

static void *nav_view;

static void light_buttons() {
    static SetActive set_active;
    if (!set_active) set_active = reinterpret_cast<SetActive>(nickel("_ZN13MainNavButton9setActiveEb"));
    if (nav_view) real_discover_active(nav_view, tab.discover_active && !tab.open);
    if (tab.button && set_active) set_active(tab.button, tab.discover_active && tab.open);
}

static void hooked_discover_active(void *self, bool active) {
    nav_view = self;
    tab.discover_active = active;
    light_buttons();
}

/* nk.tab(title, build): a tab in the bottom bar, after Discover, opening a page built by
 * build(page). With no title it takes Discover's name, in Kobo's language. Call it from main.lua. */
static int l_tab(lua_State *co) {
    const char *title = lua_isnoneornil(co, 1) ? 0 : luaL_checkstring(co, 1);
    luaL_checktype(co, 2, LUA_TFUNCTION);
    lua_pushvalue(co, 2);
    int build = luaL_ref(co, LUA_REGISTRYINDEX);
    if (tab.build) luaL_unref(co, LUA_REGISTRYINDEX, tab.build);
    tab.build = build;
    if (real_nav_view) return 0;
    if (title) tab.title = strdup(title);
    real_nav_view = reinterpret_cast<ViewCtor>(nk_hook("_ZN11MainNavViewC1EP7QWidget", reinterpret_cast<void *>(hooked_nav_view)));
    real_discover_active = reinterpret_cast<SetActive>(nk_hook("_ZN11MainNavView17setDiscoverActiveEb", reinterpret_cast<void *>(hooked_discover_active)));
    if (!real_nav_view || !real_discover_active) nk_log("couldn't hook the bottom bar, no tab");
    return 0;
}

/* ---- the rest ---- */

typedef void (*Toast)(MainWindowController *window, const QString &title, const QString &text, int ms);

/* nk.toast(title, text, ms): Kobo's message that comes and goes by itself. */
static int l_toast(lua_State *co) {
    const char *title = luaL_checkstring(co, 1), *text = luaL_optstring(co, 2, "");
    int ms = (int)luaL_optinteger(co, 3, 3000);
    Toast toast = reinterpret_cast<Toast>(nickel("_ZN20MainWindowController5toastERK7QStringS2_i"));
    if (toast) toast(MainWindowController::sharedInstance(), QString::fromUtf8(title), QString::fromUtf8(text), ms);
    return 0;
}

void kit_open_ui(lua_State *L) {
    static const luaL_Reg page_methods[] = {{"heading", page_heading}, {"books", page_books}, {"cover", page_cover},
                                           {"search", page_search}, {"field", page_field}, {0, 0}};
    static const luaL_Reg books_methods[] = {{"set", books_set}, {0, 0}};
    static const luaL_Reg field_methods[] = {{"text", field_text}, {"set", field_set}, {0, 0}};
    static const luaL_Reg image_methods[] = {{"set", image_set}, {0, 0}};
    kit_add_page_methods(page_methods);
    kit_add_type("nk.books", books_methods);
    kit_add_type("nk.field", field_methods);
    kit_add_type("nk.image", image_methods);
    lua_pushcfunction(L, l_tab);
    lua_setfield(L, -2, "tab");
    lua_pushcfunction(L, l_toast);
    lua_setfield(L, -2, "toast");
}
