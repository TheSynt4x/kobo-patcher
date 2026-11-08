/* Development tools for a real Kobo, driven from the computer over Wi-Fi with mods/tools/kobo: taps
 * and swipes as real Qt touch events, the visible widgets with their text and places, screenshots,
 * the log of every mod, files, shell commands, and reloading a Lua mod's scripts.
 *
 * It listens on port 8484 only if .adds/devtools/token exists, and answers only requests carrying
 * that token in X-Token. "Developer tools" on Kobo's More page switches it off and on again, with a
 * warning before it's on; off, it leaves .adds/devtools/off, and doesn't listen until it's on
 * again. Everything runs on Nickel's main thread, from Qt's event loop, except the thread that
 * copies stderr into the log. Kept to plain Qt and nk.h, without the Lua kit, so a
 * broken kit can't take the tools down with it.
 *
 * Kobo's Qt isn't stock 5.2.1: its QIODevice has two more virtual functions, so every virtual of a
 * socket, file or process after them is two slots further on than Qt's headers say. Those are
 * called by name (socket->QAbstractSocket::disconnectFromHost()), never through the vtable. */
#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QLabel>
#include <QAbstractButton>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QScreen>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTouchDevice>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>
#include <QWindow>
#include <5.2.1/QtGui/qpa/qwindowsysteminterface.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <time.h>
#include <link.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "nk.h"
#include <icons.inc> /* build.sh: icons/more.png, for the row on More */

#define FOLDER "/mnt/onboard/.adds/devtools"
#define PORT 8484
#define LOG_SIZE (512 * 1024)

/* Run fn after ms, on the event loop. Qt 5.2's QTimer::singleShot can't take a lambda. */
template <typename Fn> static void later(int ms, Fn fn) {
    QTimer *timer = new QTimer;
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, [timer, fn]() {
        timer->deleteLater();
        fn();
    });
    timer->start(ms);
}

/* ---- the log: everything written to stderr, by every mod and by Qt ---- */

static char *log_ring;
static long long log_total; /* bytes ever written; the ring holds the last LOG_SIZE */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int real_stderr = -1;

static void *copy_stderr(void *fd) {
    char buffer[4096];
    for (;;) {
        ssize_t n = read((int)(long)fd, buffer, sizeof buffer);
        if (n <= 0) return 0;
        if (real_stderr >= 0 && write(real_stderr, buffer, n) < 0) {}
        pthread_mutex_lock(&log_lock);
        for (ssize_t i = 0; i < n; i++) log_ring[(log_total + i) % LOG_SIZE] = buffer[i];
        log_total += n;
        pthread_mutex_unlock(&log_lock);
    }
}

static void capture_stderr() {
    int ends[2];
    if (pipe(ends)) return;
    log_ring = static_cast<char *>(malloc(LOG_SIZE));
    real_stderr = dup(2);
    dup2(ends[1], 2);
    close(ends[1]);
    pthread_t thread;
    pthread_create(&thread, 0, copy_stderr, (void *)(long)ends[0]);
    pthread_detach(thread);
}

/* The log from byte `from` on, or from as far back as the ring goes. */
static QByteArray log_since(long long from, long long *next) {
    pthread_mutex_lock(&log_lock);
    long long start = log_total > LOG_SIZE ? log_total - LOG_SIZE : 0;
    if (from < start || from > log_total) from = start;
    QByteArray out;
    out.reserve(log_total - from);
    for (long long i = from; i < log_total; i++) out.append(log_ring[i % LOG_SIZE]);
    *next = log_total;
    pthread_mutex_unlock(&log_lock);
    return out;
}

/* ---- crashes: a report that outlives the restart ----
 * A crash in Nickel restarts the Kobo, and the log above goes with it. So on a fatal signal this
 * appends to crash.log, in the Kobo's storage: the signal, the address, where the program counter
 * and return address are (library and offset), the code addresses on the stack, and the end of
 * the log. Then the signal goes on as it would have. */

static char maps[64 * 1024];

static void read_maps() {
    int fd = open("/proc/self/maps", O_RDONLY);
    int used = 0;
    if (fd >= 0) {
        ssize_t n;
        while (used < (int)sizeof maps - 1 && (n = read(fd, maps + used, sizeof maps - 1 - used)) > 0) used += n;
        close(fd);
    }
    maps[used] = 0;
}

/* The line of maps holding address, as "library +offset", or 0 if none does. */
static bool where(unsigned long address, char *out, int size) {
    for (char *line = maps; *line;) {
        char *end = strchr(line, '\n');
        unsigned long from, to;
        if (sscanf(line, "%lx-%lx", &from, &to) == 2 && address >= from && address < to) {
            char *name = end ? end : line + strlen(line);
            while (name > line && name[-1] != ' ') name--;
            int length = (end ? end : line + strlen(line)) - name;
            snprintf(out, size, "%.*s +%lx", length, name, address - from);
            return true;
        }
        if (!end) break;
        line = end + 1;
    }
    return false;
}

static void on_crash(int sig, siginfo_t *info, void *context) {
    int fd = open(FOLDER "/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        read_maps();
        mcontext_t *r = &static_cast<ucontext_t *>(context)->uc_mcontext;
        char line[600], place[400];
        int n = snprintf(line, sizeof line, "\n=== %ld: signal %d at %p, in thread %ld\n", (long)time(0), sig, info->si_addr, (long)syscall(SYS_gettid));
        if (write(fd, line, n) < 0) {}
        unsigned long registers[2] = {r->arm_pc, r->arm_lr};
        const char *names[2] = {"pc", "lr"};
        for (int i = 0; i < 2; i++) {
            if (!where(registers[i], place, sizeof place)) snprintf(place, sizeof place, "?");
            n = snprintf(line, sizeof line, "%s %08lx %s\n", names[i], registers[i], place);
            if (write(fd, line, n) < 0) {}
        }
        unsigned long *stack = reinterpret_cast<unsigned long *>(r->arm_sp);
        for (int k = 0, found = 0; k < 512 && found < 24; k++) {
            unsigned long value = stack[k];
            if (value < 0x10000 || !where(value, place, sizeof place) || !strstr(place, ".so")) continue;
            n = snprintf(line, sizeof line, "stack %08lx %s\n", value, place);
            if (write(fd, line, n) < 0) {}
            found++;
        }
        if (log_ring) {
            static const char heading[] = "--- the log's end:\n";
            if (write(fd, heading, sizeof heading - 1) < 0) {}
            long long tail = log_total < 4096 ? log_total : 4096;
            for (long long i = log_total - tail; i < log_total; i++)
                if (write(fd, &log_ring[i % LOG_SIZE], 1) < 0) {}
        }
        fsync(fd);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void catch_crashes() {
    static char stack[64 * 1024];
    stack_t alternate;
    alternate.ss_sp = stack;
    alternate.ss_size = sizeof stack;
    alternate.ss_flags = 0;
    sigaltstack(&alternate, 0);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_crash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    int fatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (unsigned i = 0; i < sizeof fatal / sizeof *fatal; i++) sigaction(fatal[i], &action, 0);
}

/* ---- settling: how long since anything was painted ---- */

static qint64 last_paint;

class PaintWatch : public QObject {
public:
    bool eventFilter(QObject *, QEvent *event) {
        if (event->type() == QEvent::Paint || event->type() == QEvent::UpdateRequest)
            last_paint = QDateTime::currentMSecsSinceEpoch();
        return false;
    }
};

/* ---- touches ---- */

static QTouchDevice *touch_device() {
    QList<const QTouchDevice *> devices = QTouchDevice::devices();
    if (!devices.isEmpty()) return const_cast<QTouchDevice *>(devices.first());
    static QTouchDevice *own;
    if (!own) {
        own = new QTouchDevice;
        own->setType(QTouchDevice::TouchScreen);
        QWindowSystemInterface::registerTouchDevice(own);
    }
    return own;
}

/* The window a finger there would touch: a pop-up, such as a menu, over the rest, then the last
 * one opened. Qt's own topLevelAt doesn't promise which of two overlapping windows it gives. */
static QWindow *window_at(QPointF at) {
    QWindow *window = 0;
    foreach (QWindow *each, QGuiApplication::topLevelWindows()) {
        if (!each->isVisible() || !each->geometry().contains(at.toPoint())) continue;
        bool popup = (each->flags() & Qt::Popup) == Qt::Popup;
        if (popup || !window || (window->flags() & Qt::Popup) != Qt::Popup) window = each;
        if (popup) break;
    }
    if (!window) window = QGuiApplication::topLevelAt(at.toPoint());
    if (!window) window = QGuiApplication::focusWindow();
    if (!window && !QGuiApplication::topLevelWindows().isEmpty()) window = QGuiApplication::topLevelWindows().first();
    return window;
}

static void touch(QPointF at, Qt::TouchPointState state) {
    QWindow *window = window_at(at);
    if (!window) return;
    QSize screen = QGuiApplication::primaryScreen()->size();
    QWindowSystemInterface::TouchPoint point;
    point.id = 1;
    point.state = state;
    point.area = QRectF(at.x() - 1, at.y() - 1, 2, 2);
    point.normalPosition = QPointF(at.x() / screen.width(), at.y() / screen.height());
    point.pressure = state == Qt::TouchPointReleased ? 0 : 1;
    QList<QWindowSystemInterface::TouchPoint> points;
    points.append(point);
    QWindowSystemInterface::handleTouchEvent(window, touch_device(), points);
}

/* A press at `from`, moving to `to` in `steps`, released after `ms`. */
static void gesture(QPointF from, QPointF to, int ms, int steps) {
    touch(from, Qt::TouchPointPressed);
    for (int i = 1; i <= steps; i++) {
        QPointF at = from + (to - from) * i / steps;
        later(ms * i / (steps + 1), [at]() { touch(at, Qt::TouchPointMoved); });
    }
    later(ms, [to]() { touch(to, Qt::TouchPointReleased); });
}

/* ---- the visible widgets ---- */

static void json_string(QByteArray *out, const QString &text) {
    out->append('"');
    QByteArray utf8 = text.toUtf8();
    for (int i = 0; i < utf8.size(); i++) {
        unsigned char c = utf8[i];
        if (c == '"' || c == '\\') { out->append('\\'); out->append(c); }
        else if (c == '\n') out->append("\\n");
        else if (c < 0x20) out->append(QByteArray("\\u00") + QByteArray::number(c, 16).rightJustified(2, '0'));
        else out->append(c);
    }
    out->append('"');
}

static QString widget_text(QWidget *w) {
    if (QLabel *label = qobject_cast<QLabel *>(w)) return label->text();
    if (QAbstractButton *button = qobject_cast<QAbstractButton *>(w)) return button->text();
    if (QLineEdit *edit = qobject_cast<QLineEdit *>(w)) return edit->text().isEmpty() ? edit->placeholderText() : edit->text();
    return QString();
}

/* One JSON object per line, a widget's parents before it: depth, class, name, text, and its place
 * on the screen. */
static void tree(QByteArray *out, QWidget *w, int depth) {
    if (!w->isVisible()) return;
    QRect r(w->mapToGlobal(QPoint(0, 0)), w->size());
    out->append("{\"depth\":" + QByteArray::number(depth) + ",\"class\":");
    json_string(out, QString::fromLatin1(w->metaObject()->className()));
    if (!w->objectName().isEmpty()) { out->append(",\"name\":"); json_string(out, w->objectName()); }
    QString text = widget_text(w);
    if (!text.isEmpty()) { out->append(",\"text\":"); json_string(out, text); }
    if (w->isWindow()) out->append(",\"window\":true");
    QAbstractButton *button = qobject_cast<QAbstractButton *>(w);
    if (button && button->isCheckable()) out->append(button->isChecked() ? ",\"checked\":true" : ",\"checked\":false");
    out->append(",\"x\":" + QByteArray::number(r.x()) + ",\"y\":" + QByteArray::number(r.y()) +
                ",\"w\":" + QByteArray::number(r.width()) + ",\"h\":" + QByteArray::number(r.height()) + "}\n");
    foreach (QObject *child, w->children())
        if (child->isWidgetType()) tree(out, static_cast<QWidget *>(child), depth + 1);
}

/* Top-level widgets, the main window first and what's over it after, as the screen stacks them. */
static QList<QWidget *> windows() {
    QList<QWidget *> shown, over;
    foreach (QWidget *w, QApplication::topLevelWidgets()) {
        if (!w->isVisible()) continue;
        if (w->windowType() == Qt::Window && w->size() == QGuiApplication::primaryScreen()->size()) shown.append(w);
        else over.append(w);
    }
    return shown + over;
}

static QByteArray screenshot() {
    QPixmap screen(QGuiApplication::primaryScreen()->size());
    screen.fill(Qt::white);
    QPainter painter(&screen);
    foreach (QWidget *w, windows()) painter.drawPixmap(w->mapToGlobal(QPoint(0, 0)), w->grab());
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    screen.save(&buffer, "PNG");
    return png;
}

/* ---- a Lua mod's dev hooks, from the kit's lua.cpp ---- */

struct Found {
    QByteArray suffix, path;
};

static int find_library(struct dl_phdr_info *info, size_t, void *data) {
    Found *found = static_cast<Found *>(data);
    if (info->dlpi_name && QByteArray(info->dlpi_name).endsWith(found->suffix)) found->path = info->dlpi_name;
    return 0;
}

typedef int (*DevCall)(const char *code, char *out, int size);

static QByteArray mod_call(const QString &mod, const char *function, const QByteArray &code, bool *ok) {
    *ok = false;
    Found found;
    found.suffix = "/lib" + mod.toUtf8() + ".so";
    dl_iterate_phdr(find_library, &found);
    if (found.path.isEmpty()) return "no mod " + mod.toUtf8() + " loaded\n";
    void *library = dlopen(found.path.constData(), RTLD_LAZY | RTLD_NOLOAD);
    DevCall call = library ? reinterpret_cast<DevCall>(dlsym(library, function)) : 0;
    if (library) dlclose(library);
    if (!call) return mod.toUtf8() + " has no " + function + ": is it a Lua mod with a new kit?\n";
    QByteArray out(64 * 1024, '\0');
    *ok = call(code.constData(), out.data(), out.size()) != 0;
    out.truncate(qstrlen(out.constData()));
    return out;
}

/* ---- files, only in the places a mod lives ---- */

static bool allowed(const QString &path) {
    QString clean = QDir::cleanPath(path);
    return clean.startsWith(QLatin1String("/mnt/onboard/")) || clean.startsWith(QLatin1String("/usr/local/Kobo/")) ||
           clean.startsWith(QLatin1String("/tmp/"));
}

/* Written beside, then renamed over, so a .so Nickel has open stays whole. */
static bool write_file(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QString temporary = path + QStringLiteral(".devtools");
    QFile file(temporary);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) return false;
    file.flush();
    fsync(file.handle());
    file.close();
    if (QFile::exists(path) && !QFile::remove(path)) return false;
    return QFile::rename(temporary, path);
}

static bool remove_path(const QString &path) {
    QFileInfo info(path);
    if (info.isDir()) return QDir(path).removeRecursively();
    return QFile::remove(path);
}

/* ---- the server ---- */

static QByteArray *token;

struct Request {
    QByteArray method, path, body;
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers;
};

static void reply(QTcpSocket *socket, int code, const QByteArray &body, const QByteArray &type = "text/plain",
                  const QByteArray &extra = QByteArray()) {
    QByteArray head = "HTTP/1.0 " + QByteArray::number(code) + (code == 200 ? " OK" : " Error") + "\r\nContent-Type: " + type +
                      "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n" + extra + "Connection: close\r\n\r\n";
    socket->write(head + body);
    socket->QAbstractSocket::disconnectFromHost();
}

static QString arg(const Request &r, const char *name) { return r.query.queryItemValue(QString::fromLatin1(name), QUrl::FullyDecoded); }
static int number(const Request &r, const char *name, int otherwise) {
    bool ok;
    int n = arg(r, name).toInt(&ok);
    return ok ? n : otherwise;
}

/* Answers once nothing has been painted for `quiet` ms, or after `timeout` ms, saying which. */
static void when_settled(QTcpSocket *socket, int quiet, int timeout, qint64 started) {
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool settled = now - last_paint >= quiet && now - started >= quiet;
    if (settled || now - started >= timeout) {
        reply(socket, 200, QByteArray(settled ? "settled" : "still painting") + " after " + QByteArray::number(now - started) + " ms\n");
        return;
    }
    QPointer<QTcpSocket> alive(socket);
    later(50, [alive, quiet, timeout, started]() {
        if (alive) when_settled(alive, quiet, timeout, started);
    });
}

static void run_shell(QTcpSocket *socket, const QByteArray &command, int timeout) {
    QProcess *process = new QProcess;
    process->setProcessChannelMode(QProcess::MergedChannels);
    QPointer<QTcpSocket> alive(socket);
    QObject::connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     [process, alive](int code, QProcess::ExitStatus) {
                         if (alive) reply(alive, 200, process->readAll(), "text/plain", "X-Exit: " + QByteArray::number(code) + "\r\n");
                         process->deleteLater();
                     });
    /* The process is deleted once it finishes, which is usually long before the timeout. */
    QPointer<QProcess> still(process);
    later(timeout, [still]() {
        if (still && still->state() != QProcess::NotRunning) still->kill();
    });
    process->start(QStringLiteral("/bin/sh"), QStringList() << QStringLiteral("-c") << QString::fromUtf8(command));
}

static void handle(QTcpSocket *socket, const Request &r) {
    if (!token || r.headers.value("x-token") != *token) return reply(socket, 403, "wrong or missing X-Token\n");
    const QByteArray &p = r.path;
    if (p == "/ping") {
        QSize s = QGuiApplication::primaryScreen()->size();
        return reply(socket, 200, "devtools " + QByteArray::number(s.width()) + "x" + QByteArray::number(s.height()) + "\n");
    }
    if (p == "/tap" || p == "/press" || p == "/swipe") {
        QPointF from(number(r, "x", -1), number(r, "y", -1));
        QPointF to = p == "/swipe" ? QPointF(number(r, "x2", -1), number(r, "y2", -1)) : from;
        if (from.x() < 0 || from.y() < 0 || to.x() < 0 || to.y() < 0) return reply(socket, 400, "needs x and y\n");
        int ms = number(r, "ms", p == "/tap" ? 60 : p == "/press" ? 1200 : 300);
        gesture(from, to, ms, p == "/swipe" ? 8 : 0);
        QPointer<QTcpSocket> alive(socket);
        later(ms + 10, [alive]() { if (alive) reply(alive, 200, "done\n"); });
        return;
    }
    if (p == "/settle") {
        return when_settled(socket, number(r, "quiet", 400), number(r, "timeout", 8000), QDateTime::currentMSecsSinceEpoch());
    }
    if (p == "/tree") {
        QByteArray out;
        foreach (QWidget *w, windows()) tree(&out, w, 0);
        return reply(socket, 200, out, "application/x-ndjson");
    }
    if (p == "/shot") return reply(socket, 200, screenshot(), "image/png");
    if (p == "/log") {
        long long next;
        QByteArray text = log_since(arg(r, "from").toLongLong(), &next);
        return reply(socket, 200, text, "text/plain", "X-Next: " + QByteArray::number(next) + "\r\n");
    }
    if (p == "/file" || p == "/ls") {
        QString path = arg(r, "path");
        if (!allowed(path)) return reply(socket, 403, "only under /mnt/onboard, /usr/local/Kobo or /tmp\n");
        if (p == "/ls") {
            QByteArray out;
            foreach (const QFileInfo &f, QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name))
                out += (f.isDir() ? "d " : QByteArray::number(f.size()) + " ") + f.fileName().toUtf8() + "\n";
            return reply(socket, 200, out);
        }
        if (r.method == "GET") {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return reply(socket, 404, "can't read " + path.toUtf8() + "\n");
            return reply(socket, 200, file.readAll(), "application/octet-stream");
        }
        if (r.method == "PUT") return write_file(path, r.body) ? reply(socket, 200, "written\n") : reply(socket, 500, "couldn't write\n");
        if (r.method == "DELETE") return remove_path(path) ? reply(socket, 200, "removed\n") : reply(socket, 404, "couldn't remove\n");
    }
    if (p == "/sh") return run_shell(socket, r.body, number(r, "timeout", 20000));
    if (p == "/reload" || p == "/eval") {
        bool ok;
        QByteArray out = mod_call(arg(r, "mod"), p == "/reload" ? "nk_dev_reload" : "nk_dev_eval", r.body, &ok);
        return reply(socket, ok ? 200 : 500, out);
    }
    if (p == "/reboot") {
        reply(socket, 200, "rebooting\n");
        later(300, []() { nk_reboot(); });
        return;
    }
    reply(socket, 404, "no " + p + "\n");
}

/* Reads a request: its line, its headers, then Content-Length bytes of body. */
static void read_request(QTcpSocket *socket, QByteArray *buffer) {
    buffer->append(socket->readAll());
    int end = buffer->indexOf("\r\n\r\n");
    if (end < 0) return;
    Request r;
    QList<QByteArray> lines = buffer->left(end).split('\n');
    QList<QByteArray> first = lines.value(0).trimmed().split(' ');
    r.method = first.value(0);
    QUrl url(QStringLiteral("http://kobo") + QString::fromLatin1(first.value(1)));
    r.path = url.path().toUtf8();
    r.query = QUrlQuery(url);
    for (int i = 1; i < lines.size(); i++) {
        int colon = lines[i].indexOf(':');
        if (colon > 0) r.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
    }
    int length = r.headers.value("content-length").toInt();
    if (buffer->size() < end + 4 + length) return;
    r.body = buffer->mid(end + 4, length);
    buffer->clear();
    handle(socket, r);
}

static QTcpServer *server;

static void start_server() {
    if (server) return;
    QFile file(QStringLiteral(FOLDER "/token"));
    if (!file.open(QIODevice::ReadOnly)) {
        nk_log("no " FOLDER "/token, not listening");
        return;
    }
    token = new QByteArray(file.readAll().trimmed());
    if (token->size() < 16) {
        nk_log("token is too short, not listening");
        token = 0;
        return;
    }
    static bool watching;
    if (!watching) qApp->installEventFilter(new PaintWatch);
    watching = true;
    server = new QTcpServer;
    if (!server->listen(QHostAddress::Any, PORT)) {
        nk_log("can't listen on %d", PORT);
        delete server;
        server = 0;
        return;
    }
    QObject::connect(server, &QTcpServer::newConnection, []() {
        while (QTcpSocket *socket = server->QTcpServer::nextPendingConnection()) {
            QByteArray *buffer = new QByteArray;
            QObject::connect(socket, &QTcpSocket::readyRead, [socket, buffer]() { read_request(socket, buffer); });
            QObject::connect(socket, &QTcpSocket::disconnected, [socket, buffer]() {
                delete buffer;
                socket->deleteLater();
            });
        }
    });
    nk_log("listening on %d", PORT);
}

static void stop_server() {
    if (!server) return;
    server->QTcpServer::close();
    server->deleteLater();
    server = 0;
    nk_log("stopped listening");
}

/* ---- the switch in Settings ---- */

static const char WARNING[] = "While these are on, anyone on your Wi-Fi who has this Kobo's token can control it: tap and "
                              "type on it, read and change its files, and run commands on it. Turn them off when you "
                              "aren't using them.";
static NkToggle *toggle;
static bool setting; /* the toggle being set from here, not tapped */

static bool is_on() { return access(FOLDER "/off", F_OK) != 0; }

static void set_toggle(bool on) {
    setting = true;
    if (toggle) nk_toggle_set(toggle, on);
    setting = false;
}

static void turned_on(void *) {
    unlink(FOLDER "/off");
    start_server();
}

static void kept_off(void *) { set_toggle(false); }

static void switched(void *) {
    if (setting || !toggle) return;
    if (!nk_toggle_on(toggle)) {
        int fd = open(FOLDER "/off", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        stop_server();
        return;
    }
    nk_confirm("Turn on developer tools?", WARNING, "Turn on", "Cancel", turned_on, kept_off, 0);
}

static void build_screen(NkScreen *screen, void *) {
    nk_section(screen, "WI-FI ACCESS");
    nk_text(screen, WARNING);
    toggle = nk_toggle_row(screen, "Developer tools", is_on(), switched, 0);
}

static void open_screen(void *) { nk_open_screen("Developer tools", "More", build_screen, 0); }

void nk_mod_init(void) {
    if (access(FOLDER "/token", F_OK)) {
        nk_log("no " FOLDER "/token, doing nothing");
        return;
    }
    capture_stderr();
    catch_crashes();
    nk_more_row("Developer tools", icon_more, icon_more_size, open_screen, 0);
    if (!is_on()) {
        nk_log("switched off on More, not listening");
        return;
    }
    /* Once Qt's event loop runs, so the server lives on Nickel's main thread. */
    later(0, []() { start_server(); });
}
