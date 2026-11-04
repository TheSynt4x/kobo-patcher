/* Kobo's own widgets as blocks for a Lua mod's pages, and a tab of the mod's own in Kobo's bottom
 * bar. Every block is one of Kobo's classes, so Kobo's stylesheets style it as they do their own.
 *
 * - Labels like HeaderMediumLabel only give a base class a name for the stylesheets to select on,
 *   and have no constructor of their own. A label is made as its base, QLabel or ElidedLabel, and
 *   given the class's vtables, which libnickel exports. The class is checked to be a direct
 *   subclass of that base first, so no firmware gets a label that's more than it seems.
 * - Text boxes are Kobo's SearchEditBox and TouchLineEdit, typed in with Kobo's keyboard: the one
 *   a screen's N3Dialog keeps, or on a tab one over the page's lower part, as Kobo's search has.
 * - Tabs are MainNavButtons in Kobo's bottom bar, after or before one of Kobo's own, each with the
 *   mod's name and icons. A tab's page is a MoreController whose loadView makes the page here. Its
 *   navSection is Discover's, and the light moves from Discover's button to the tab's.
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
#include <QImage>
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

/* Blocks are spaced by these: SPACE between parts of one, GAP between blocks. */
enum { SPACE = 8, GAP = 24 };

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
    const char *name, *base;
} label_classes[] = {
    {"HeaderMediumLabel", "FontSizeAdjustingLabel"}, {"RegularLabel", "QLabel"},
    {"MetaDataLabel", "QLabel"},                     {"HeaderSmallElidedLabel", "ElidedLabel"},
    {"SmallElidedLabel", "ElidedLabel"},             {"RegularElidedLabel", "ElidedLabel"},
};

typedef void (*WidgetCtor)(void *self, QWidget *parent);
typedef void (*FlagsCtor)(void *self, QWidget *parent, int flags);
typedef void (*SetText)(void *self, const QString &text);

void kit_set_text(QLabel *label, const QString &text) {
    static SetText elided_set;
    if (!elided_set) elided_set = reinterpret_cast<SetText>(nickel("_ZN11ElidedLabel7setTextERK7QString"));
    if (label->inherits("ElidedLabel") && elided_set) elided_set(label, text);
    else label->setText(text);
}

/* One of Kobo's labels, or a plain one if this firmware's isn't as expected. */
static QLabel *kobo_label(QWidget *parent, const char *name, const QString &text) {
    const char *base = 0;
    for (unsigned i = 0; i < sizeof label_classes / sizeof *label_classes; i++)
        if (!strcmp(label_classes[i].name, name)) base = label_classes[i].base;
    void *vptrs[2];
    bool styled = base && vptrs_of(name, vptrs) && renames(name, base);
    static WidgetCtor elided_ctor;
    static FlagsCtor adjusting_ctor;
    if (!elided_ctor) elided_ctor = reinterpret_cast<WidgetCtor>(nickel("_ZN11ElidedLabelC1EP7QWidget"));
    if (!adjusting_ctor) adjusting_ctor = reinterpret_cast<FlagsCtor>(nickel("_ZN22FontSizeAdjustingLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE"));
    QLabel *label;
    if (styled && !strcmp(base, "ElidedLabel") && elided_ctor) {
        label = static_cast<QLabel *>(alloc(0x200)); /* 0xe8 on 4.38 */
        elided_ctor(label, parent);
    } else if (styled && !strcmp(base, "FontSizeAdjustingLabel") && adjusting_ctor) {
        label = static_cast<QLabel *>(alloc(0x100)); /* 0x20 on 4.38 */
        adjusting_ctor(label, parent, 0);
    } else {
        styled = styled && !strcmp(base, "QLabel");
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

static QPixmap scaled_pixmap(const QString &path, int width, int height) {
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
    return pixmap;
}

/* A cover as Kobo's Home shows one, in a box of width by height: the image in a thin frame at the
 * box's bottom or top left, or a dashed outline of the whole box while there's none. */
static void show_cover(QLabel *cover, const QString &path, int width, int height, bool bottom) {
    QPixmap pixmap = scaled_pixmap(path, width - 2, height - 2);
    if (pixmap.isNull()) {
        cover->clear();
        cover->setFixedSize(width, height);
        cover->setStyleSheet(QStringLiteral("border: 1px dashed black;"));
    } else {
        cover->setFixedSize(pixmap.width() + 2, pixmap.height() + 2);
        cover->setStyleSheet(QStringLiteral("border: 1px solid #999999;"));
        cover->setPixmap(pixmap);
    }
    cover->move(0, bottom ? height - cover->height() : 0);
    /* At the top, what follows comes right after the cover, whatever its shape. */
    if (!bottom && cover->parentWidget()) cover->parentWidget()->setFixedHeight(cover->height());
}

static void show_image(QLabel *label, const QString &path, int width, int height) {
    QPixmap pixmap = scaled_pixmap(path, width, height);
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

/* Something tapped that runs a script's function, and goes with its widget. */
class Action : public QObject {
public:
    explicit Action(QObject *parent) : QObject(parent), hook(0) {}
    KitHook *hook;
};

static void action_tapped(void *arg) {
    QPointer<Action> *action = static_cast<QPointer<Action> *>(arg);
    lua_State *co = *action ? kit_hook_task((*action)->hook) : 0;
    if (co) kit_resume(co, 0);
}

/* Kobo's icons a block can show, by the name a script gives. */
static const struct {
    const char *name, *path;
} icons[] = {
    {"settings", ":/images/reading/settings_gear.png"},
};

/* One of Kobo's icons by name, or else the mod's own icons/<name>.png, which is scaled to fit
 * size. Null if there's neither. Only when a page is built: never while the mod loads. */
static QPixmap icon_pixmap(const char *name, int size) {
    for (unsigned i = 0; i < sizeof icons / sizeof *icons; i++)
        if (!strcmp(icons[i].name, name)) return QPixmap(QString::fromLatin1(icons[i].path));
    const unsigned char *data;
    unsigned bytes;
    QPixmap image;
    QByteArray file = QByteArray("icons/") + name + ".png";
    if (!kit_embedded(file.constData(), &data, &bytes) || !image.loadFromData(data, bytes, "PNG")) return QPixmap();
    return image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

typedef void (*TouchCtor)(void *self, QWidget *parent, int flags);

/* Kobo's tappable label, to hold a block's parts. */
static QLabel *touch_label(QWidget *parent) {
    static TouchCtor touch_ctor;
    if (!touch_ctor) touch_ctor = reinterpret_cast<TouchCtor>(nickel("_ZN10TouchLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE"));
    if (!touch_ctor) return new (alloc(0x40)) QLabel(parent, 0);
    QLabel *label = static_cast<QLabel *>(alloc(0x100)); /* 0x84 on 4.38 */
    touch_ctor(label, parent, 0);
    return label;
}

/* A square to tap, with the image centred in it, running tapped when tapped. */
static QLabel *tap_square(QWidget *parent, const QPixmap &image, int size, void (*tapped)(void *), void *arg) {
    QLabel *square = touch_label(parent);
    square->setPixmap(image);
    square->setFixedSize(px(size), px(size));
    square->setAlignment(Qt::AlignCenter);
    if (square->inherits("TouchLabel")) nk_connect(square, "_ZN10TouchLabel6tappedEb", "_ZN10TouchLabel16staticMetaObjectE", tapped, arg);
    return square;
}

/* page:heading(text, icon, tapped): a large title, as at the top of Kobo's More page. With an
 * icon it's on the right, and tapped() runs when it's tapped: one of Kobo's, such as "settings",
 * or the mod's own icons/<icon>.png, scaled to Kobo's size for it. */
static int page_heading(lua_State *co) {
    PagingContainer *paging = kit_check_page(co);
    const char *text = luaL_checkstring(co, 2);
    const char *icon = luaL_optstring(co, 3, 0);
    if (icon) luaL_checktype(co, 4, LUA_TFUNCTION);
    QWidget *holder = new QWidget(paging);
    QHBoxLayout *line = new QHBoxLayout(holder);
    line->setContentsMargins(0, px(GAP), 0, px(2 * SPACE));
    QLabel *heading = kobo_label(holder, "HeaderMediumLabel", QString::fromUtf8(text));
    heading->setWordWrap(true);
    line->addWidget(heading, 1, Qt::AlignVCenter);
    QPixmap image = icon ? icon_pixmap(icon, px(32)) : QPixmap();
    if (icon && image.isNull()) return luaL_error(co, "no icon called %s, of Kobo's or in icons/", icon);
    if (!image.isNull()) {
        Action *action = new Action(holder);
        action->hook = kit_hook(co, 4, action);
        QPointer<Action> *arg = new QPointer<Action>(action);
        QLabel *square = tap_square(holder, image, 56, action_tapped, arg);
        QObject::connect(square, &QObject::destroyed, [arg]() { delete arg; });
        line->addWidget(square, 0, Qt::AlignVCenter);
    }
    paging->addWidget(holder);
    kit_push_handle(co, heading, "nk.label");
    return 1;
}

/* ---- books ---- */

/* A shelf is SHELF_WIDTH across, on the Nia, whatever its number of columns: three books of
 * TILE_WIDTH by default. Covers keep TILE_WIDTH to TILE_COVER. */
enum { SHELF_COLUMNS = 3, SHELF_WIDTH = 570, TILE_WIDTH = 190, TILE_COVER = 220, BOOK_COVER_WIDTH = 230, BOOK_COVER = 345 };

/* A shelf's books, kept to change them. It goes with the page. */
class Books : public QObject {
public:
    explicit Books(QObject *parent) : QObject(parent), hook(0) {}
    struct Tile {
        QPointer<QLabel> tile, cover, title, author, status;
        int width, height; /* the cover's, in pixels */
    };
    QList<Tile> tiles;
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

/* Kobo's grey caption, in capitals as its style shows it, cut to the label's width. */
static void set_caption(QLabel *label, const QString &text) {
    label->ensurePolished();
    label->setText(label->fontMetrics().elidedText(text.toUpper(), Qt::ElideRight, label->width()));
}

/* A title on two lines at most, the second cut short if it must be. */
static void set_title(QLabel *label, const QString &text) {
    QFontMetrics metrics = label->fontMetrics();
    int width = label->width();
    QStringList words = text.simplified().split(QLatin1Char(' '));
    QString first;
    while (!words.isEmpty()) {
        QString longer = first.isEmpty() ? words.first() : first + QLatin1Char(' ') + words.first();
        if (metrics.width(longer) > width) break;
        first = longer;
        words.removeFirst();
    }
    if (first.isEmpty()) label->setText(metrics.elidedText(text, Qt::ElideRight, width));
    else if (words.isEmpty()) label->setText(first);
    else label->setText(first + QLatin1Char('\n') + metrics.elidedText(words.join(QLatin1Char(' ')), Qt::ElideRight, width));
}

/* What's happening to a book, on the bottom left of its cover. */
static void set_badge(QLabel *badge, const QString &text) {
    badge->setText(text);
    badge->setHidden(text.isEmpty());
    badge->adjustSize();
    badge->move(0, badge->parentWidget()->height() - badge->height());
    badge->raise();
}

static void fill_tile(lua_State *co, int book, Books::Tile &tile) {
    QString text;
    if (field_of(co, book, "title", &text) && tile.title) set_title(tile.title, text);
    if (field_of(co, book, "author", &text) && tile.author) set_caption(tile.author, text);
    if (field_of(co, book, "status", &text) && tile.status) set_badge(tile.status, text);
    if (field_of(co, book, "cover", &text) && tile.cover)
        show_cover(tile.cover, image_path(text.toUtf8().constData()), tile.width, tile.height, true);
}

/* A book as Kobo's Home shows one: its cover, its title under it, and its author in grey capitals. */
static Books::Tile add_tile(QWidget *row, Books *books, int index, int width, int height) {
    Books::Tile parts;
    parts.width = width;
    parts.height = height;
    QLabel *tile = touch_label(row);
    tile->setObjectName(QStringLiteral("nkBook"));
    tile->setFixedWidth(width);
    parts.tile = tile;
    QVBoxLayout *column = new QVBoxLayout(tile);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    QWidget *box = new QWidget(tile);
    box->setFixedSize(width, height);
    parts.cover = new (alloc(0x40)) QLabel(box, 0);
    show_cover(parts.cover, QString(), width, height, true);
    parts.status = kobo_label(box, "MetaDataLabel", QString());
    parts.status->setStyleSheet(QStringLiteral("background-color: black; color: white; padding: %1px %2px;").arg(px(4)).arg(px(8)));
    parts.status->hide();
    column->addWidget(box, 0, 0);
    column->addSpacing(px(SPACE));
    parts.title = kobo_label(tile, "RegularLabel", QString());
    parts.author = kobo_label(tile, "MetaDataLabel", QString());
    parts.title->setFixedWidth(width);
    parts.author->setFixedWidth(width);
    column->addWidget(parts.title, 0, 0);
    column->addWidget(parts.author, 0, 0);
    column->addStretch(1);
    /* Room for two lines of title, with the author right under however many there are; and a
     * TouchLabel is as tall as its text, not its layout, so it's told. */
    parts.title->ensurePolished();
    parts.author->ensurePolished();
    tile->setFixedHeight(height + px(SPACE) + parts.title->fontMetrics().lineSpacing() * 2 +
                         parts.author->fontMetrics().height());

    Tap *tap = new Tap();
    tap->books = books;
    tap->index = index;
    QObject::connect(tile, &QObject::destroyed, [tap]() { delete tap; });
    if (tile->inherits("TouchLabel")) nk_connect(tile, "_ZN10TouchLabel6tappedEb", "_ZN10TouchLabel16staticMetaObjectE", book_tapped, tap);
    return parts;
}

/* page:books(list, tapped, {columns = }): the books three across, or columns across (1 to 6), as
 * Kobo's Home shows them, in the same width whatever the columns. A book is a table of title,
 * author, cover (a path) and status, shown on its cover. tapped(i) runs when the i-th is tapped.
 * The handle's :set(i, book) changes the fields given. */
static int page_books(lua_State *co) {
    PagingContainer *paging = kit_check_page(co);
    luaL_checktype(co, 2, LUA_TTABLE);
    luaL_checktype(co, 3, LUA_TFUNCTION);
    int columns = SHELF_COLUMNS;
    if (lua_istable(co, 4)) {
        lua_getfield(co, 4, "columns");
        if (!lua_isnil(co, -1)) columns = int(luaL_checkinteger(co, -1));
        lua_pop(co, 1);
        if (columns < 1 || columns > 6) return luaL_error(co, "page:books takes 1 to 6 columns");
    }
    int width = px(SHELF_WIDTH) / columns, height = width * TILE_COVER / TILE_WIDTH;
    Books *books = new Books(paging);
    books->hook = kit_hook(co, 3, books);
    lua_Integer count = luaL_len(co, 2);
    if (count > 500) count = 500;
    /* A row a block, so pages turn between rows. */
    for (lua_Integer first = 1; first <= count; first += columns) {
        QWidget *row = new QWidget(paging);
        QHBoxLayout *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, px(GAP));
        line->setSpacing(0);
        for (int j = 0; j < columns; j++) {
            if (j) line->addStretch(1);
            lua_Integer i = first + j;
            QWidget *slot = 0;
            if (i <= count) {
                Books::Tile tile;
                if (lua_geti(co, 2, i) == LUA_TTABLE) {
                    tile = add_tile(row, books, int(i - 1), width, height);
                    fill_tile(co, lua_gettop(co), tile);
                    slot = tile.tile;
                }
                lua_pop(co, 1);
                books->tiles << tile;
            }
            if (!slot) {
                slot = new QWidget(row);
                slot->setFixedWidth(width);
            }
            line->addWidget(slot, 0, Qt::AlignTop);
        }
        paging->addWidget(row);
    }
    kit_push_handle(co, books, "nk.books");
    return 1;
}

static int books_set(lua_State *co) {
    Books *books = static_cast<Books *>(kit_check_handle(co, 1, "nk.books"));
    lua_Integer i = luaL_checkinteger(co, 2);
    luaL_checktype(co, 3, LUA_TTABLE);
    if (i >= 1 && i <= books->tiles.size()) fill_tile(co, 3, books->tiles[int(i - 1)]);
    return 0;
}

/* ---- a book ---- */

typedef void (*SetBool)(void *self, bool on);

/* Kobo's black button, as on its store's pages, or 0 if this firmware has none. */
static QLabel *kobo_button(QWidget *parent) {
    WidgetCtor ctor = reinterpret_cast<WidgetCtor>(nickel("_ZN13N3ButtonLabelC1EP7QWidget"));
    SetBool primary = reinterpret_cast<SetBool>(nickel("_ZN13N3ButtonLabel16setPrimaryButtonEb"));
    if (!ctor) return 0;
    QLabel *button = static_cast<QLabel *>(alloc(0x200)); /* 0x98 on 4.38 */
    ctor(button, parent);
    if (primary) primary(button, true);
    /* Kobo paints its buttons itself, and makes the primary one black only in some places. */
    typedef void (*SetColor)(void *self, QColor color);
    SetColor background = reinterpret_cast<SetColor>(nickel("_ZN13N3ButtonLabel18setBackgroundColorE6QColor"));
    if (background) background(button, QColor(Qt::black));
    button->setStyleSheet(QStringLiteral("color: white;"));
    button->setAlignment(Qt::AlignCenter);
    return button;
}

class Book : public QObject {
public:
    explicit Book(QObject *parent) : QObject(parent), hook(0) {}
    QPointer<QLabel> cover, title, author, action, status;
    KitHook *hook;
};

static void book_action(void *arg) {
    QPointer<Book> *book = static_cast<QPointer<Book> *>(arg);
    lua_State *co = *book ? kit_hook_task((*book)->hook) : 0;
    if (co) kit_resume(co, 0);
}

static void fill_book(lua_State *co, int index, Book *book) {
    QString text;
    if (field_of(co, index, "title", &text) && book->title) book->title->setText(text);
    if (field_of(co, index, "author", &text) && book->author) book->author->setText(text);
    if (field_of(co, index, "status", &text) && book->status) {
        book->status->setText(text);
        book->status->setHidden(text.isEmpty());
    }
    if (field_of(co, index, "action", &text) && book->action) {
        book->action->setText(text);
        book->action->setHidden(text.isEmpty());
    }
    if (field_of(co, index, "cover", &text) && book->cover)
        show_cover(book->cover, image_path(text.toUtf8().constData()), px(BOOK_COVER_WIDTH), px(BOOK_COVER), false);
}

/* page:book(fields, tapped): a book as Kobo's book details show one: its cover on the left, and on
 * the right its title, author, Kobo's black button and a line of what's happening. The fields are
 * cover (a path), title, author, action (the button's text, or "" for none) and status. tapped()
 * runs when the button is tapped. The handle's :set(fields) changes the fields given. */
static int page_book(lua_State *co) {
    PagingContainer *paging = kit_check_page(co);
    luaL_checktype(co, 2, LUA_TTABLE);
    luaL_checktype(co, 3, LUA_TFUNCTION);
    QWidget *block = new QWidget(paging);
    Book *book = new Book(block);
    book->hook = kit_hook(co, 3, book);
    QHBoxLayout *line = new QHBoxLayout(block);
    line->setContentsMargins(0, px(SPACE), 0, px(GAP));
    line->setSpacing(px(GAP));
    QWidget *box = new QWidget(block);
    box->setFixedSize(px(BOOK_COVER_WIDTH), px(BOOK_COVER));
    book->cover = new (alloc(0x40)) QLabel(box, 0);
    show_cover(book->cover, QString(), px(BOOK_COVER_WIDTH), px(BOOK_COVER), false);
    line->addWidget(box, 0, Qt::AlignTop);

    QVBoxLayout *right = new QVBoxLayout();
    right->setSpacing(px(SPACE));
    book->title = kobo_label(block, "HeaderMediumLabel", QString());
    book->author = kobo_label(block, "RegularLabel", QString());
    book->status = kobo_label(block, "RegularLabel", QString());
    book->title->setWordWrap(true);
    book->author->setWordWrap(true);
    book->status->setWordWrap(true);
    right->addWidget(book->title, 0, 0);
    right->addWidget(book->author, 0, 0);
    right->addSpacing(px(2 * SPACE));
    book->action = kobo_button(block);
    if (book->action) {
        book->action->setMinimumHeight(px(64));
        right->addWidget(book->action, 0, 0);
        QPointer<Book> *arg = new QPointer<Book>(book);
        QObject::connect(book->action, &QObject::destroyed, [arg]() { delete arg; });
        nk_connect(book->action, "_ZN10TouchLabel6tappedEb", "_ZN10TouchLabel16staticMetaObjectE", book_action, arg);
    }
    right->addWidget(book->status, 0, 0);
    right->addStretch(1);
    line->addLayout(right, 1);
    fill_book(co, 2, book);
    paging->addWidget(block);
    kit_push_handle(co, book, "nk.book");
    return 1;
}

/* page:paragraph(text): text in Kobo's regular style, wrapped, as a book's description. */
static int page_paragraph(lua_State *co) {
    PagingContainer *paging = kit_check_page(co);
    QLabel *text = kobo_label(paging, "RegularLabel", QString::fromUtf8(luaL_checkstring(co, 2)));
    text->setWordWrap(true);
    text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    text->setContentsMargins(0, 0, 0, px(2 * SPACE));
    paging->addWidget(text);
    kit_push_handle(co, text, "nk.label");
    return 1;
}

static int book_set(lua_State *co) {
    Book *book = static_cast<Book *>(kit_check_handle(co, 1, "nk.book"));
    luaL_checktype(co, 2, LUA_TTABLE);
    fill_book(co, 2, book);
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

typedef void (*Turn)(void *self);
typedef void (*PointOf)(QPoint *out, const void *self);

/* A tab's page: a PagingContainer that turns a page for each drag up or down, or across. Kobo's
 * paging bar stays, at the page's right edge as in My Books. */
class TabPage : public QWidget {
public:
    TabPage() : QWidget(0) {}
    QPointer<QWidget> paging;

protected:
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
            if (qAbs(along) > px(40) && next && prev) (along > 0 ? next : prev)(paging);
        }
        return true;
    }
};

/* A tab in Kobo's bottom bar, and the pages opened over its first, by build function, the last on
 * top. Each mod's kit keeps its own tabs, and its pages say whose they are, so one mod's tabs never
 * touch another's. */
struct Tab {
    QString name, icon; /* icons/<icon>.png, and icons/<icon>-active.png if there is one */
    QString after, before;           /* Kobo's sections, like "discover"; neither means before More */
    int build;
    QList<int> stack;
    QPointer<QWidget> button;
};
static QList<Tab *> *tabs;
static Tab *shown;        /* whose page is showing, or 0 */
static QWidget *shown_page;
static Tab *opening;      /* whose page the next loadView makes */
static bool discover_lit; /* Kobo's own light for Discover, whose section a tab borrows */

static void light_buttons();
static QString icon_file(const QString &name);
static void open_tab(Tab *t);

static lua_State *ui_lua;

static void close_pages(Tab *t, int keep) {
    while (t->stack.size() > keep) luaL_unref(ui_lua, LUA_REGISTRYINDEX, t->stack.takeLast());
}

/* Once the tap that asked is done, as the widget it came from goes with the page. */
static void reopen_soon(Tab *t) {
    QTimer *soon = new QTimer();
    soon->setSingleShot(true);
    QObject::connect(soon, &QTimer::timeout, [soon, t]() {
        soon->deleteLater();
        open_tab(t);
    });
    soon->start(0);
}

static void back_tapped(void *arg) {
    Tab *t = static_cast<Tab *>(arg);
    if (!t->stack.isEmpty()) close_pages(t, t->stack.size() - 1);
    reopen_soon(t);
}

/* Tapping the tab in the bar goes back to its first page, as Kobo's own tabs do. */
static void tab_tapped(void *arg) {
    Tab *t = static_cast<Tab *>(arg);
    close_pages(t, 0);
    open_tab(t);
}

/* The tab of this kit whose page the widget is on, or 0. Another mod's tab pages have the same name,
 * for the stylesheet, but a different kit. */
static Tab *tab_of(QWidget *widget) {
    for (QWidget *w = widget; w; w = w->parentWidget())
        if (w->objectName() == QLatin1String("nkTabPage"))
            return w->property("nkKit").value<quintptr>() == quintptr(&tabs) ? reinterpret_cast<Tab *>(w->property("nkTab").value<quintptr>()) : 0;
    return 0;
}

/* page:open(build, title): a page over this one, built by build(page), with Kobo's back arrow to
 * return. On a tab it's the tab's own page. On a screen it's a screen over it, with the title given,
 * or this screen's, and this screen's title as its small title. */
static int page_open(lua_State *co) {
    PagingContainer *paging = kit_check_page(co);
    luaL_checktype(co, 2, LUA_TFUNCTION);
    Tab *t = tab_of(reinterpret_cast<QWidget *>(paging));
    if (!t) {
        const char *over = kit_top_screen_title();
        QByteArray small_title(over ? over : "Settings");
        const char *title = luaL_optstring(co, 3, small_title.constData());
        kit_lua_open_screen(co, title, small_title.constData(), 2);
        return 0;
    }
    lua_pushvalue(co, 2);
    t->stack << luaL_ref(co, LUA_REGISTRYINDEX);
    reopen_soon(t);
    return 0;
}

static TabPage *make_tab_page(Tab *t) {
    TabPage *page = new TabPage();
    page->setObjectName(QStringLiteral("nkTabPage"));
    page->setProperty("nkKit", quintptr(&tabs));
    page->setProperty("nkTab", quintptr(t));
    page->setAttribute(Qt::WA_StyledBackground);
    /* Kobo's style pads some labels to fit its own layouts; a tab's blocks line up on their own. */
    page->setStyleSheet(QStringLiteral("#nkTabPage { background-color: white; } #nkTabPage QLabel { padding: 0px; }"));
    /* What Kobo reads off a view to show its status bar: the clock, Wi-Fi and battery. */
    page->setProperty("showTitle", true);
    page->setProperty("showIndicators", true);
    page->setProperty("showStatusBottomLine", false);
    static int *drag_type;
    if (!drag_type) drag_type = static_cast<int *>(nickel("_ZN21DragGestureRecognizer12_gestureTypeE"));
    if (drag_type) page->grabGesture(Qt::GestureType(*drag_type));
    QVBoxLayout *column = new QVBoxLayout(page);
    bool over = !t->stack.isEmpty();
    /* Below the status bar, with the left margin of Kobo's More page. On the right, Kobo's paging
     * bar is the margin. */
    column->setContentsMargins(px(40), over ? px(76) : px(84), 0, px(8));
    column->setSpacing(0);
    if (over) {
        /* Kobo's back arrow, in a square to tap centred on it, the size of its dialogs' own. The
         * arrow keeps to the page's left edge, so the square sits over the margin. */
        QPixmap arrow(QStringLiteral(":/images/reading/back_arrow.png"));
        QLabel *back = tap_square(page, arrow, 78, back_tapped, t);
        if (arrow.isNull()) back->setText(QStringLiteral("‹"));
        int arrow_width = arrow.isNull() ? px(32) : arrow.width();
        back->move(px(40) + arrow_width / 2 - back->width() / 2, px(76) + px(32) - back->height() / 2);
        back->raise();
        column->addSpacing(px(64));
    }
    WidgetCtor paging_ctor = reinterpret_cast<WidgetCtor>(nickel("_ZN15PagingContainerC1EP7QWidget"));
    if (!paging_ctor) return page;
    QWidget *paging = static_cast<QWidget *>(alloc(0x100)); /* 0x30 on 4.38 */
    paging_ctor(paging, page);
    page->paging = paging;
    column->addWidget(paging, 1);
    kit_build_page(static_cast<PagingContainer *>(paging), over ? t->stack.last() : t->build, true);
    shown = t;
    shown_page = page;
    light_buttons();
    QObject::connect(page, &QObject::destroyed, [page] {
        /* Not when this tab's own page is opened again, which is made before the old one goes. */
        if (shown_page != page) return;
        shown = 0;
        shown_page = 0;
        light_buttons();
    });
    return page;
}

/* The controller's loadView, as MoreController's: make the view and keep a QPointer to it. */
static void tab_load_view(void *self) {
    void **controller = static_cast<void **>(self);
    QWidget *page = make_tab_page(opening);
    controller[3] = QtSharedPointer::ExternalRefCountData::getAndRef(page);
    controller[4] = page;
}
static void tab_load_view_thunk(void *self) { tab_load_view(static_cast<char *>(self) - 8); }
/* A tab borrows Discover's section, and light_buttons moves the light from Discover's button to
 * the tab's. */
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
static void open_tab(Tab *t) {
    if (!t || !kit_ready_to_open()) return;
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
    opening = t;
    nk_failsafe(30);
    clear(MainWindowController::sharedInstance(), reinterpret_cast<AbstractController *>(controller + 2), false);
}

void kit_reopen_tab() { open_tab(shown); }

bool kit_reload_tab() {
    foreach (QWidget *w, QApplication::allWidgets()) {
        if (!w->isVisible()) continue;
        Tab *t = w->objectName() == QLatin1String("nkTabPage") ? tab_of(w) : 0;
        if (!t) continue;
        close_pages(t, 0);
        open_tab(t);
        return true;
    }
    return false;
}

typedef void (*ViewCtor)(void *self, QWidget *parent);
typedef void (*SetActive)(void *self, bool active);
static ViewCtor real_nav_view;
static SetActive real_discover_active;

static const char *const SECTIONS[] = {"home", "books", "discover", "notebooks", "activity", "more", 0};

static bool is_section(const QString &name) {
    for (int i = 0; SECTIONS[i]; i++)
        if (name == QLatin1String(SECTIONS[i])) return true;
    return false;
}

/* Where a tab's button goes in the bar's row: after or before Kobo's button for a section, and after
 * any tabs already there, so tabs keep the order they were made in. -1 if that button isn't there. */
static int place_in(QBoxLayout *row, QWidget *view, Tab *t, QWidget **beside) {
    QString section = !t->after.isEmpty() ? t->after : !t->before.isEmpty() ? t->before : QStringLiteral("more");
    QWidget *kobo = view->findChild<QWidget *>(section + QStringLiteral("Button"));
    int at = kobo ? row->indexOf(kobo) : -1;
    if (at < 0) return -1;
    *beside = kobo;
    if (t->after.isEmpty()) return at;
    for (at++; at < row->count(); at++) {
        QWidget *next = row->itemAt(at)->widget();
        if (!next || next->objectName() != QLatin1String("nkTabButton")) break;
    }
    return at;
}

/* Kobo's bottom bar: each tab's button goes in where the tab asked, with the share of the bar of
 * the Kobo button beside it. */
static void hooked_nav_view(void *self, QWidget *parent) {
    real_nav_view(self, parent);
    QWidget *view = static_cast<QWidget *>(self);
    QBoxLayout *row = view->findChild<QBoxLayout *>(QStringLiteral("mainNavLayout"));
    WidgetCtor button_ctor = reinterpret_cast<WidgetCtor>(nickel("_ZN13MainNavButtonC1EP7QWidget"));
    SetText set_pixmap = reinterpret_cast<SetText>(nickel("_ZN13MainNavButton9setPixmapERK7QString"));
    SetText set_active_pixmap = reinterpret_cast<SetText>(nickel("_ZN13MainNavButton15setActivePixmapERK7QString"));
    SetText set_text = reinterpret_cast<SetText>(nickel("_ZN13MainNavButton7setTextERK7QString"));
    if (!row || !button_ctor || !set_pixmap || !set_active_pixmap || !set_text) {
        nk_log("the bottom bar isn't as expected, no tabs");
        return;
    }
    foreach (Tab *t, *tabs) {
        QWidget *beside = 0;
        int at = place_in(row, view, t, &beside);
        if (at < 0) {
            nk_log("no place in the bottom bar for the tab %s", t->name.toUtf8().constData());
            continue;
        }
        QWidget *button = static_cast<QWidget *>(alloc(0x100)); /* 0x7c on 4.38 */
        button_ctor(button, view);
        button->setObjectName(QStringLiteral("nkTabButton"));
        QString icon = icon_file(t->icon), active = icon_file(t->icon + QStringLiteral("-active"));
        set_pixmap(button, icon);
        set_active_pixmap(button, active.isEmpty() ? icon : active);
        set_text(button, t->name);
        button->setSizePolicy(beside->sizePolicy());
        button->setMinimumSize(beside->minimumSize());
        button->setMaximumSize(beside->maximumSize());
        row->insertWidget(at, button, row->stretch(row->indexOf(beside)));
        nk_connect(button, "_ZN13MainNavButton6tappedEv", "_ZN13MainNavButton16staticMetaObjectE", tab_tapped, t);
        t->button = button;
        nk_log("tab %s added", t->name.toUtf8().constData());
    }
}

static void *nav_view;

/* Kobo lights Discover while a tab's page shows, as the tab borrows its section. The light goes to
 * the tab's button instead. With another mod's tabs, its kit hooked this too, and passes on
 * Discover's light only when none of its own tabs is showing. */
static void light_buttons() {
    static SetActive set_active;
    if (!set_active) set_active = reinterpret_cast<SetActive>(nickel("_ZN13MainNavButton9setActiveEb"));
    if (nav_view) real_discover_active(nav_view, discover_lit && !shown);
    if (!set_active) return;
    foreach (Tab *t, *tabs)
        if (t->button) set_active(t->button, discover_lit && t == shown);
}

static void hooked_discover_active(void *self, bool active) {
    nav_view = self;
    discover_lit = active;
    light_buttons();
}

/* A bar icon from the mod's icons/<name>.png, as a file for Kobo's button to load, scaled to the
 * height of Kobo's own: 28 pixels on the Nia. Drawn larger, it stays sharp on denser screens. Only
 * once Kobo builds the bar: decoding a PNG while Qt is still loading this plugin waits on Qt's own
 * plugin loader, for ever. */
static QString icon_file(const QString &name) {
    const unsigned char *data;
    unsigned size;
    QByteArray embedded = "icons/" + name.toUtf8() + ".png";
    if (!kit_embedded(embedded.constData(), &data, &size)) return QString();
    QImage image;
    if (!image.loadFromData(data, int(size), "PNG")) return QString();
    QString path = QStringLiteral("/tmp/nk-%1-%2.png").arg(quintptr(&tabs), 0, 16).arg(name);
    image = image.scaled(px(28), px(28), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image.save(path, "PNG") ? path : QString();
}

static QString field(lua_State *co, const char *name) {
    lua_getfield(co, 1, name);
    QString value = lua_isnil(co, -1) ? QString() : QString::fromUtf8(luaL_checkstring(co, -1));
    lua_pop(co, 1);
    return value;
}

/* nk.tab{name = , icon = , after = or before = , build = }: a tab in Kobo's bottom bar, opening a
 * page built by build(page). The icon is icons/<icon>.png, and icons/<icon>-active.png while the
 * tab is open if there is one. after or before name one of Kobo's sections (home, books, discover,
 * notebooks, activity, more), and without either it goes before More. A mod can have several tabs.
 * Call it from main.lua: running it again with the same name gives that tab the new build. */
static int l_tab(lua_State *co) {
    luaL_checktype(co, 1, LUA_TTABLE);
    QString name = field(co, "name"), icon = field(co, "icon"), after = field(co, "after"), before = field(co, "before");
    if (name.isEmpty()) return luaL_error(co, "nk.tab needs a name");
    if (icon.isEmpty()) return luaL_error(co, "nk.tab needs an icon");
    if (!after.isEmpty() && !before.isEmpty()) return luaL_error(co, "nk.tab takes after or before, not both");
    if (!after.isEmpty() && !is_section(after)) return luaL_error(co, "nk.tab: no section %s", after.toUtf8().constData());
    if (!before.isEmpty() && !is_section(before)) return luaL_error(co, "nk.tab: no section %s", before.toUtf8().constData());
    lua_getfield(co, 1, "build");
    luaL_checktype(co, -1, LUA_TFUNCTION);
    int build = luaL_ref(co, LUA_REGISTRYINDEX);
    if (!tabs) tabs = new QList<Tab *>;
    foreach (Tab *t, *tabs) {
        if (t->name != name) continue;
        luaL_unref(co, LUA_REGISTRYINDEX, t->build);
        t->build = build;
        return 0;
    }
    const unsigned char *png;
    unsigned png_size;
    if (!kit_embedded(("icons/" + icon.toUtf8() + ".png").constData(), &png, &png_size)) {
        luaL_unref(co, LUA_REGISTRYINDEX, build);
        return luaL_error(co, "no icons/%s.png", icon.toUtf8().constData());
    }
    Tab *t = new Tab;
    t->name = name;
    t->icon = icon;
    t->after = after;
    t->before = before;
    t->build = build;
    *tabs << t;
    if (real_nav_view) return 0;
    real_nav_view = reinterpret_cast<ViewCtor>(nk_hook("_ZN11MainNavViewC1EP7QWidget", reinterpret_cast<void *>(hooked_nav_view)));
    real_discover_active = reinterpret_cast<SetActive>(nk_hook("_ZN11MainNavView17setDiscoverActiveEb", reinterpret_cast<void *>(hooked_discover_active)));
    if (!real_nav_view || !real_discover_active) nk_log("couldn't hook the bottom bar, no tabs");
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
    static const luaL_Reg page_methods[] = {{"heading", page_heading}, {"books", page_books}, {"book", page_book},
                                           {"cover", page_cover}, {"search", page_search}, {"field", page_field},
                                           {"open", page_open}, {"paragraph", page_paragraph}, {0, 0}};
    static const luaL_Reg books_methods[] = {{"set", books_set}, {0, 0}};
    static const luaL_Reg book_methods[] = {{"set", book_set}, {0, 0}};
    static const luaL_Reg field_methods[] = {{"text", field_text}, {"set", field_set}, {0, 0}};
    static const luaL_Reg image_methods[] = {{"set", image_set}, {0, 0}};
    kit_add_page_methods(page_methods);
    ui_lua = L;
    kit_add_type("nk.books", books_methods);
    kit_add_type("nk.book", book_methods);
    kit_add_type("nk.field", field_methods);
    kit_add_type("nk.image", image_methods);
    lua_pushcfunction(L, l_tab);
    lua_setfield(L, -2, "tab");
    lua_pushcfunction(L, l_toast);
    lua_setfield(L, -2, "toast");
}
