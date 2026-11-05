/* The kit's tools for Lua mods: the network, downloads, Kobo's import, JSON, regular expressions,
 * text in every alphabet, files, hashes and signatures, the device, and the mod's own file. The safety rules are in the
 * tools, not in the mods:
 *
 * - nk.http follows redirects (Qt 5.2 doesn't), gives up after a timeout, cuts off an answer over
 *   its limit, and only sends authorization to the host it was meant for.
 * - nk.download writes to a hidden file beside the target, checks its size and hash, and only then
 *   renames it into place, so a book is never half there.
 * - nk.file only reaches /mnt/onboard, and writes go to a new file that's renamed over the old.
 * - nk.replace_self only installs a build whose SHA-512 the mod already trusts.
 *
 * As in lua.cpp, nothing raises a Lua error while a Qt value made here is still alive. */
#include <QLocale>
#include <QCoreApplication>
#include <QEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include "kit.h"
#include "lua_kit.h"
#include "vendor/monocypher/monocypher-ed25519.h"

#define USER_AGENT "Mozilla/5.0 (Linux; Kobo) " NK_NAME
#define ONBOARD "/mnt/onboard/"

/* Nickel's import after a USB connection. */
class PlugWorkflowManager {
public:
    static PlugWorkflowManager *sharedInstance();
    void sync();
};

static QNetworkAccessManager *network() {
    static QNetworkAccessManager *manager;
    if (!manager) manager = new QNetworkAccessManager();
    return manager;
}

typedef QList<QPair<QByteArray, QByteArray> > Headers;

/* The `headers` field of an options table, as name, value pairs. */
static void read_headers(lua_State *co, int options, Headers *out) {
    if (lua_getfield(co, options, "headers") == LUA_TTABLE) {
        lua_pushnil(co);
        while (lua_next(co, -2)) {
            if (lua_type(co, -2) == LUA_TSTRING && lua_type(co, -1) == LUA_TSTRING)
                *out << qMakePair(QByteArray(lua_tostring(co, -2)), QByteArray(lua_tostring(co, -1)));
            lua_pop(co, 1);
        }
    }
    lua_pop(co, 1);
}

static QByteArray opt_string(lua_State *co, int options, const char *name) {
    QByteArray value;
    if (lua_getfield(co, options, name) == LUA_TSTRING) value = lua_tostring(co, -1);
    lua_pop(co, 1);
    return value;
}

static qint64 opt_number(lua_State *co, int options, const char *name, qint64 otherwise) {
    qint64 value = otherwise;
    if (lua_getfield(co, options, name) == LUA_TNUMBER) value = (qint64)lua_tonumber(co, -1);
    lua_pop(co, 1);
    return value;
}

static QNetworkRequest request_for(const QUrl &url, const Headers &headers, const QByteArray &auth, const QString &auth_host) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", USER_AGENT);
    for (int i = 0; i < headers.size(); i++) request.setRawHeader(headers[i].first, headers[i].second);
    if (!auth.isEmpty() && url.host() == auth_host) request.setRawHeader("Authorization", auth);
    return request;
}

/* Kobo's Qt isn't stock 5.2.1: its QIODevice has two more virtual functions, so every virtual of a
 * reply after them is two slots further on than Qt's headers say, and reply->abort() would run
 * writeData instead. abort is a slot, so it's called by name, through the reply's meta-object. */
static void abort_reply(QNetworkReply *reply) { QMetaObject::invokeMethod(reply, "abort", Qt::DirectConnection); }

static int status_of(QNetworkReply *reply) { return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); }

static QUrl redirect_of(QNetworkReply *reply) {
    int status = status_of(reply);
    QUrl target = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (status < 300 || status >= 400 || !target.isValid()) return QUrl();
    return reply->url().resolved(target);
}

/* Runs fn once whatever signal is being handled now is done with, so a reply isn't deleted while
 * it's still sending one. */
template <typename Fn> static void later(Fn fn) {
    QTimer *timer = new QTimer();
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, [timer, fn]() {
        timer->deleteLater();
        fn();
    });
    timer->start(0);
}

/* ---- nk.http ---- */

struct Request {
    lua_State *co;
    QUrl url;
    QByteArray body, type, auth;
    QString auth_host;
    Headers headers;
    bool post;
    qint64 limit;
    int timeout, redirects;
    bool too_large;
};

static void send(Request *r);

static void answer(Request *r, int status, const QByteArray &body, const QString &why) {
    lua_State *co = r->co;
    delete r;
    lua_pushinteger(co, status);
    if (status) lua_pushlstring(co, body.constData(), body.size());
    else lua_pushnil(co);
    if (why.isEmpty()) lua_pushnil(co);
    else lua_pushstring(co, why.toUtf8().constData());
    kit_resume(co, 3);
}

static void send(Request *r) {
    QNetworkRequest request = request_for(r->url, r->headers, r->auth, r->auth_host);
    QNetworkReply *reply;
    if (r->post) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, r->type);
        reply = network()->post(request, r->body);
    } else reply = network()->get(request);
    QTimer *timer = new QTimer(reply);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, [reply]() { abort_reply(reply); });
    timer->start(r->timeout);
    QObject::connect(reply, &QNetworkReply::downloadProgress, [r, reply](qint64 received, qint64) {
        if (received > r->limit && !r->too_large) {
            r->too_large = true;
            abort_reply(reply);
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, [r, reply, timer]() {
        QObject::disconnect(reply, 0, 0, 0);
        timer->stop();
        reply->deleteLater();
        int status = status_of(reply);
        QUrl next = redirect_of(reply);
        if (!r->too_large && next.isValid() && r->redirects < 5) {
            r->redirects++;
            r->url = next;
            later([r]() { send(r); });
            return;
        }
        QByteArray body = reply->readAll();
        QString why;
        if (r->too_large) {
            status = 0;
            why = QStringLiteral("sent more than %1 bytes").arg(r->limit);
        } else if (!status) why = reply->errorString();
        else if (status < 200 || status >= 300) why = QStringLiteral("answered %1").arg(status);
        later([r, status, body, why]() { answer(r, status, body, why); });
    });
}

/* nk.http.get(url, options) and nk.http.post(url, options): wait for the answer, and return its
 * status (0 when nothing answered), its body, and what went wrong or nil. Options: headers, body,
 * type (for a post, "application/json" unless said), auth (an Authorization header, only sent to
 * the URL's own host), limit (bytes, 4 MB unless said), timeout (ms, 30 s unless said). */
static int http(lua_State *co, bool post) {
    kit_must_wait(co);
    const char *url = luaL_checkstring(co, 1);
    bool options = lua_istable(co, 2);
    Request *r = new Request();
    r->co = co;
    r->post = post;
    r->url = QUrl(QString::fromUtf8(url));
    r->auth_host = r->url.host();
    r->limit = 4 * 1024 * 1024;
    r->timeout = 30000;
    r->type = "application/json";
    if (options) {
        read_headers(co, 2, &r->headers);
        r->body = opt_string(co, 2, "body");
        r->auth = opt_string(co, 2, "auth");
        QByteArray type = opt_string(co, 2, "type");
        if (!type.isEmpty()) r->type = type;
        r->limit = opt_number(co, 2, "limit", r->limit);
        r->timeout = (int)opt_number(co, 2, "timeout", r->timeout);
    }
    bool valid = r->url.isValid() && (r->url.scheme() == QLatin1String("http") || r->url.scheme() == QLatin1String("https"));
    if (!valid) {
        delete r;
        return luaL_error(co, "not an http or https address: %s", url);
    }
    send(r);
    return lua_yield(co, 0);
}

static int l_get(lua_State *co) { return http(co, false); }
static int l_post(lua_State *co) { return http(co, true); }

/* ---- paths ---- */

/* A path a mod may use: absolute under /mnt/onboard, or relative to the mod's own .adds folder. */
static bool allowed_path(const char *given, QString *out) {
    QString path = QString::fromUtf8(given);
    if (!path.startsWith(QLatin1Char('/'))) path = QStringLiteral(NK_FOLDER "/") + path;
    path = QDir::cleanPath(path);
    /* Cleaning takes the onboard folder's own slash off, and it can be listed too. */
    bool onboard = path.startsWith(QLatin1String(ONBOARD)) || path + QLatin1Char('/') == QLatin1String(ONBOARD);
    if (!onboard || path.contains(QLatin1String("/../"))) return false;
    *out = path;
    return true;
}

static QString check_path(lua_State *co, int index) {
    const char *given = luaL_checkstring(co, index);
    QString path;
    if (!allowed_path(given, &path)) luaL_error(co, "only paths under " ONBOARD " can be used: %s", given);
    return path;
}

/* Written beside the old file and renamed over it, so a lost write leaves the old one. */
static bool write_whole(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).path());
    QString part = path + QStringLiteral(".new");
    QFile file(part);
    if (!file.open(QIODevice::WriteOnly)) return false;
    bool ok = file.write(data) == data.size() && file.flush() && fsync(file.handle()) == 0;
    file.close();
    if (!ok || rename(QFile::encodeName(part).constData(), QFile::encodeName(path).constData())) {
        QFile::remove(part);
        return false;
    }
    return true;
}

/* ---- nk.download ---- */

struct Download {
    lua_State *co;
    QUrl url;
    QString path, part;
    Headers headers;
    QByteArray md5, sha512, referer;
    qint64 size, limit, bytes, total;
    int fd, redirects, progress, shown;
    QCryptographicHash *hash_md5, *hash_sha512;
    QTimer *stalled;
    QString why;
};

static void fetch_file(Download *d);

static void downloaded(Download *d, int status, QString why) {
    d->stalled->deleteLater();
    if (d->fd >= 0) {
        if (fsync(d->fd)) why = QStringLiteral("couldn't be saved");
        close(d->fd);
    }
    if (why.isEmpty() && d->size >= 0 && d->bytes != d->size) why = QStringLiteral("is %1 bytes, not %2").arg(d->bytes).arg(d->size);
    if (why.isEmpty() && !d->md5.isEmpty() && d->hash_md5->result().toHex() != d->md5) why = QStringLiteral("isn't the file asked for (its MD5 differs)");
    if (why.isEmpty() && !d->sha512.isEmpty() && d->hash_sha512->result().toHex() != d->sha512) why = QStringLiteral("isn't the file asked for (its SHA-512 differs)");
    if (why.isEmpty() && rename(QFile::encodeName(d->part).constData(), QFile::encodeName(d->path).constData())) why = QStringLiteral("couldn't be put in place");
    if (!why.isEmpty()) QFile::remove(d->part);
    else sync();
    lua_State *co = d->co;
    if (d->progress) luaL_unref(kit_lua(), LUA_REGISTRYINDEX, d->progress);
    delete d->hash_md5;
    delete d->hash_sha512;
    delete d;
    lua_pushboolean(co, why.isEmpty());
    if (why.isEmpty()) lua_pushnil(co);
    else lua_pushstring(co, why.toUtf8().constData());
    lua_pushinteger(co, status);
    kit_resume(co, 3);
}

static void show_progress(Download *d) {
    if (!d->progress) return;
    int step = d->total > 0 ? int(d->bytes * 100 / d->total) : int(d->bytes >> 20);
    if (step == d->shown) return;
    d->shown = step;
    lua_State *co = kit_task(d->progress);
    lua_pushinteger(co, d->bytes);
    lua_pushinteger(co, d->total);
    kit_resume(co, 2);
}

static void fetch_file(Download *d) {
    QNetworkRequest request = request_for(d->url, d->headers, QByteArray(), QString());
    if (!d->referer.isEmpty()) request.setRawHeader("Referer", d->referer);
    QNetworkReply *reply = network()->get(request);
    QObject::disconnect(d->stalled, 0, 0, 0);
    QObject::connect(d->stalled, &QTimer::timeout, [d, reply]() {
        d->why = QStringLiteral("sent nothing for a minute");
        abort_reply(reply);
    });
    d->stalled->start(60000);
    QObject::connect(reply, &QNetworkReply::readyRead, [d, reply]() {
        int status = status_of(reply);
        if (status < 200 || status >= 300 || !d->why.isEmpty()) return; /* a redirect's or a refusal's body */
        if (!d->total) d->total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        QByteArray data = reply->readAll();
        d->bytes += data.size();
        d->stalled->start(60000);
        if (d->bytes > d->limit || d->total > d->limit) {
            d->why = QStringLiteral("is larger than %1 bytes").arg(d->limit);
            abort_reply(reply);
            return;
        }
        d->hash_md5->addData(data);
        d->hash_sha512->addData(data);
        for (int done = 0; done < data.size();) {
            ssize_t n = write(d->fd, data.constData() + done, data.size() - done);
            if (n <= 0) {
                d->why = QStringLiteral("couldn't be saved: is the Kobo full?");
                abort_reply(reply);
                return;
            }
            done += n;
        }
        show_progress(d);
    });
    QObject::connect(reply, &QNetworkReply::finished, [d, reply]() {
        QObject::disconnect(reply, 0, 0, 0);
        d->stalled->stop();
        reply->deleteLater();
        int status = status_of(reply);
        QUrl next = redirect_of(reply);
        if (d->why.isEmpty() && next.isValid() && d->redirects < 5) {
            d->redirects++;
            d->url = next;
            later([d]() { fetch_file(d); });
            return;
        }
        QString why = d->why;
        if (why.isEmpty() && (status < 200 || status >= 300)) why = status ? QStringLiteral("answered %1").arg(status) : reply->errorString();
        if (why.isEmpty()) {
            QByteArray rest = reply->readAll();
            if (!rest.isEmpty()) {
                d->bytes += rest.size();
                d->hash_md5->addData(rest);
                d->hash_sha512->addData(rest);
                if (write(d->fd, rest.constData(), rest.size()) != rest.size()) why = QStringLiteral("couldn't be saved: is the Kobo full?");
            }
        }
        later([d, status, why]() { downloaded(d, status, why); });
    });
}

/* nk.download(url, path, options): saves url at path, and returns true, or false and why, then the
 * status. Options: md5 or sha512 (hex) and size, which the file must match; limit (bytes, 512 MB
 * unless said); referer; headers; progress(bytes, total), run as it comes. */
static int l_download(lua_State *co) {
    kit_must_wait(co);
    const char *url = luaL_checkstring(co, 1);
    QString path = check_path(co, 2);
    Download *d = new Download();
    d->co = co;
    d->url = QUrl(QString::fromUtf8(url));
    d->path = path;
    d->part = QFileInfo(path).path() + QStringLiteral("/.") + QFileInfo(path).fileName() + QStringLiteral(".part");
    d->size = -1;
    d->limit = 512LL * 1024 * 1024;
    d->shown = -1;
    if (lua_istable(co, 3)) {
        read_headers(co, 3, &d->headers);
        d->md5 = opt_string(co, 3, "md5").toLower();
        d->sha512 = opt_string(co, 3, "sha512").toLower();
        d->referer = opt_string(co, 3, "referer");
        d->size = opt_number(co, 3, "size", -1);
        d->limit = opt_number(co, 3, "limit", d->limit);
        if (lua_getfield(co, 3, "progress") == LUA_TFUNCTION) d->progress = luaL_ref(co, LUA_REGISTRYINDEX);
        else lua_pop(co, 1);
    }
    QDir().mkpath(QFileInfo(path).path());
    d->fd = open(QFile::encodeName(d->part).constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool valid = d->url.isValid() && (d->url.scheme() == QLatin1String("http") || d->url.scheme() == QLatin1String("https"));
    if (!valid || d->fd < 0) {
        if (d->fd >= 0) close(d->fd);
        QFile::remove(d->part);
        if (d->progress) luaL_unref(co, LUA_REGISTRYINDEX, d->progress);
        delete d;
        lua_pushboolean(co, 0);
        lua_pushstring(co, valid ? "couldn't be saved there" : "not an http or https address");
        lua_pushinteger(co, 0);
        return 3;
    }
    d->hash_md5 = new QCryptographicHash(QCryptographicHash::Md5);
    d->hash_sha512 = new QCryptographicHash(QCryptographicHash::Sha512);
    d->stalled = new QTimer();
    d->stalled->setSingleShot(true);
    fetch_file(d);
    return lua_yield(co, 0);
}

/* nk.import(): Kobo's import, as after a USB connection, so new books show up. */
static int l_import(lua_State *) {
    nk_failsafe(30);
    PlugWorkflowManager::sharedInstance()->sync();
    return 0;
}

/* ---- nk.json ---- */

static char json_null;

static void push_json(lua_State *co, const QJsonValue &value, int depth) {
    if (depth > 200) {
        lua_pushnil(co);
        return;
    }
    switch (value.type()) {
    case QJsonValue::Bool: lua_pushboolean(co, value.toBool()); break;
    case QJsonValue::Double: {
        double n = value.toDouble();
        if (n == floor(n) && fabs(n) < 9007199254740992.0) lua_pushinteger(co, (lua_Integer)n);
        else lua_pushnumber(co, n);
        break;
    }
    case QJsonValue::String: {
        QByteArray text = value.toString().toUtf8();
        lua_pushlstring(co, text.constData(), text.size());
        break;
    }
    case QJsonValue::Array: {
        QJsonArray array = value.toArray();
        lua_createtable(co, array.size(), 0);
        for (int i = 0; i < array.size(); i++) {
            push_json(co, array.at(i), depth + 1);
            lua_rawseti(co, -2, i + 1);
        }
        break;
    }
    case QJsonValue::Object: {
        QJsonObject object = value.toObject();
        lua_createtable(co, 0, object.size());
        for (QJsonObject::const_iterator i = object.constBegin(); i != object.constEnd(); ++i) {
            QByteArray key = i.key().toUtf8();
            lua_pushlstring(co, key.constData(), key.size());
            push_json(co, i.value(), depth + 1);
            lua_rawset(co, -3);
        }
        break;
    }
    default: lua_pushlightuserdata(co, &json_null); break;
    }
}

/* nk.json.decode(text): the value, or nil and why. JSON's null is nk.json.null. */
static int l_decode(lua_State *co) {
    size_t size;
    const char *text = luaL_checklstring(co, 1, &size);
    QJsonParseError error;
    QJsonDocument document = QJsonDocument::fromJson(QByteArray(text, (int)size), &error);
    if (document.isNull()) {
        QByteArray why = error.errorString().toUtf8() + " at " + QByteArray::number(error.offset);
        lua_pushnil(co);
        lua_pushstring(co, why.constData());
        return 2;
    }
    if (document.isArray()) push_json(co, QJsonValue(document.array()), 0);
    else push_json(co, QJsonValue(document.object()), 0);
    return 1;
}

/* A table is an array when its keys are 1..n, and an object otherwise. An empty table is an object. */
static bool is_array(lua_State *co, int index) {
    lua_Integer n = (lua_Integer)lua_rawlen(co, index);
    if (n == 0) return false;
    lua_Integer keys = 0;
    lua_pushnil(co);
    while (lua_next(co, index)) {
        lua_pop(co, 1);
        keys++;
    }
    return keys == n;
}

static bool to_json(lua_State *co, int index, QJsonValue *out, QByteArray *why, int depth) {
    index = lua_absindex(co, index);
    if (depth > 100) {
        *why = "too deep, or a table that holds itself";
        return false;
    }
    switch (lua_type(co, index)) {
    case LUA_TNIL: *out = QJsonValue(); return true;
    case LUA_TBOOLEAN: *out = QJsonValue(lua_toboolean(co, index) != 0); return true;
    case LUA_TNUMBER: *out = QJsonValue(lua_tonumber(co, index)); return true;
    case LUA_TSTRING: *out = QJsonValue(QString::fromUtf8(lua_tostring(co, index))); return true;
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(co, index) == &json_null) {
            *out = QJsonValue(QJsonValue::Null);
            return true;
        }
        break;
    case LUA_TTABLE:
        if (is_array(co, index)) {
            QJsonArray array;
            lua_Integer n = (lua_Integer)lua_rawlen(co, index);
            for (lua_Integer i = 1; i <= n; i++) {
                lua_rawgeti(co, index, i);
                QJsonValue item;
                bool ok = to_json(co, -1, &item, why, depth + 1);
                lua_pop(co, 1);
                if (!ok) return false;
                array.append(item);
            }
            *out = array;
        } else {
            QJsonObject object;
            lua_pushnil(co);
            while (lua_next(co, index)) {
                if (lua_type(co, -2) != LUA_TSTRING && lua_type(co, -2) != LUA_TNUMBER) {
                    lua_pop(co, 2);
                    *why = "an object's keys must be strings";
                    return false;
                }
                lua_pushvalue(co, -2);
                QString key = QString::fromUtf8(lua_tostring(co, -1));
                lua_pop(co, 1);
                QJsonValue item;
                if (!to_json(co, -1, &item, why, depth + 1)) {
                    lua_pop(co, 2);
                    return false;
                }
                object.insert(key, item);
                lua_pop(co, 1);
            }
            *out = object;
        }
        return true;
    }
    *why = QByteArray("can't be JSON: ") + luaL_typename(co, index);
    return false;
}

/* nk.json.encode(value): the text, or nil and why. */
static int l_encode(lua_State *co) {
    luaL_checkany(co, 1);
    bool ok;
    QByteArray text, why;
    {
        QJsonValue value;
        ok = to_json(co, 1, &value, &why, 0);
        if (ok) {
            /* Qt 5.2 only writes documents, so a single value is written in an array and taken out. */
            QJsonArray holder;
            holder.append(value);
            text = QJsonDocument(holder).toJson(QJsonDocument::Compact);
            text = text.mid(1, text.size() - 2);
        }
    }
    if (!ok) {
        lua_pushnil(co);
        lua_pushlstring(co, why.constData(), why.size());
        return 2;
    }
    lua_pushlstring(co, text.constData(), text.size());
    return 1;
}

/* ---- nk.re ---- */

/* Compiled patterns are kept, since parsers use the same ones over and over. */
static QRegularExpression *pattern(lua_State *co, int index, int flags_index, QByteArray *why) {
    static QHash<QByteArray, QRegularExpression *> *compiled;
    if (!compiled) compiled = new QHash<QByteArray, QRegularExpression *>;
    const char *text = luaL_checkstring(co, index), *flags = luaL_optstring(co, flags_index, "");
    QByteArray key = QByteArray(flags) + '/' + text;
    QRegularExpression *re = compiled->value(key);
    if (re) return re;
    QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
    if (strchr(flags, 'i')) options |= QRegularExpression::CaseInsensitiveOption;
    if (strchr(flags, 's')) options |= QRegularExpression::DotMatchesEverythingOption;
    if (strchr(flags, 'm')) options |= QRegularExpression::MultilineOption;
    re = new QRegularExpression(QString::fromUtf8(text), options);
    if (!re->isValid()) {
        *why = QByteArray("bad pattern: ") + re->errorString().toUtf8();
        delete re;
        return 0;
    }
    if (compiled->size() > 100) {
        qDeleteAll(*compiled);
        compiled->clear();
    }
    compiled->insert(key, re);
    return re;
}

/* The pattern at index 2, with its flags at flags_index, or a Lua error. */
static QRegularExpression *check_pattern(lua_State *co, int flags_index) {
    QByteArray why;
    QRegularExpression *re = pattern(co, 2, flags_index, &why);
    if (!re) {
        char message[256];
        snprintf(message, sizeof message, "%s", why.constData());
        why = QByteArray();
        luaL_error(co, "%s", message);
    }
    return re;
}

static void push_qstring(lua_State *co, const QString &text) {
    QByteArray utf8 = text.toUtf8();
    lua_pushlstring(co, utf8.constData(), utf8.size());
}

/* A match's captures, or the whole match when the pattern has none. Returns how many. */
static int push_captures(lua_State *co, const QRegularExpressionMatch &match) {
    int groups = match.lastCapturedIndex();
    if (groups == 0) {
        push_qstring(co, match.captured(0));
        return 1;
    }
    for (int i = 1; i <= groups; i++) {
        if (match.capturedStart(i) < 0) lua_pushnil(co);
        else push_qstring(co, match.captured(i));
    }
    return groups;
}

/* nk.re.match(text, pattern, flags): the first match's captures, or nil. Flags: i to ignore case,
 * s for a dot that matches new lines, m for ^ and $ at each line. */
static int l_match(lua_State *co) {
    const char *text = luaL_checkstring(co, 1);
    QRegularExpression *re = check_pattern(co, 3);
    QRegularExpressionMatch match = re->match(QString::fromUtf8(text));
    if (!match.hasMatch()) {
        lua_pushnil(co);
        return 1;
    }
    return push_captures(co, match);
}

/* nk.re.find(text, pattern, flags): where the first match starts and ends, in bytes from 1, then
 * its captures; or nil. */
static int l_find(lua_State *co) {
    const char *text = luaL_checkstring(co, 1);
    QRegularExpression *re = check_pattern(co, 3);
    QString string = QString::fromUtf8(text);
    QRegularExpressionMatch match = re->match(string);
    if (!match.hasMatch()) {
        lua_pushnil(co);
        return 1;
    }
    lua_pushinteger(co, string.left(match.capturedStart(0)).toUtf8().size() + 1);
    lua_pushinteger(co, string.left(match.capturedEnd(0)).toUtf8().size());
    return 2 + push_captures(co, match);
}

/* nk.re.all(text, pattern, flags): every match, as a list of lists of captures. */
static int l_all(lua_State *co) {
    const char *text = luaL_checkstring(co, 1);
    QRegularExpression *re = check_pattern(co, 3);
    lua_newtable(co);
    QRegularExpressionMatchIterator matches = re->globalMatch(QString::fromUtf8(text));
    for (int n = 1; matches.hasNext(); n++) {
        QRegularExpressionMatch match = matches.next();
        int count = push_captures(co, match);
        lua_createtable(co, count, 0);
        lua_insert(co, -(count + 1));
        for (int i = count; i >= 1; i--) lua_rawseti(co, -(i + 1), i);
        lua_rawseti(co, -2, n);
    }
    return 1;
}

/* nk.re.gsub(text, pattern, replacement, flags): every match replaced, with \1 for a capture. */
static int l_gsub(lua_State *co) {
    const char *text = luaL_checkstring(co, 1), *replacement = luaL_checkstring(co, 3);
    QRegularExpression *re = check_pattern(co, 4);
    QString string = QString::fromUtf8(text);
    string.replace(*re, QString::fromUtf8(replacement));
    push_qstring(co, string);
    return 1;
}

/* ---- nk.text ---- */

/* nk.text.lower(text): lower case in every alphabet, where Lua's string.lower only does ASCII. */
static int l_lower(lua_State *co) {
    push_qstring(co, QString::fromUtf8(luaL_checkstring(co, 1)).toLower());
    return 1;
}

/* nk.text.decompose(text): letters apart from their accents (Unicode's NFKD), so é is e then an
 * accent, which nk.re can take out with \p{M}. */
static int l_decompose(lua_State *co) {
    push_qstring(co, QString::fromUtf8(luaL_checkstring(co, 1)).normalized(QString::NormalizationForm_KD));
    return 1;
}

/* nk.text.language_name(code): a language's name in itself, capitalised, as Kobo's language
 * picker shows them: "ru" is Русский, "sv" Svenska. Empty for a code Qt doesn't know. */
static int l_language_name(lua_State *co) {
    QLocale locale(QString::fromUtf8(luaL_checkstring(co, 1)));
    /* Qt names English by its country, as U.S. English; Kobo's picker just says English. */
    QString name = locale.language() == QLocale::C ? QString()
                   : locale.language() == QLocale::English ? QStringLiteral("English") : locale.nativeLanguageName();
    if (!name.isEmpty()) name = locale.toUpper(name.left(1)) + name.mid(1);
    push_qstring(co, name);
    return 1;
}

/* ---- nk.file ---- */

static int file_failed(lua_State *co, const char *why) {
    lua_pushnil(co);
    lua_pushstring(co, why);
    return 2;
}

/* nk.file.read(path): the file's bytes, or nil and why. A relative path is in .adds/<mod>. */
static int l_read(lua_State *co) {
    QString path = check_path(co, 1);
    bool ok;
    QByteArray data;
    {
        QFile file(path);
        ok = file.open(QIODevice::ReadOnly);
        if (ok) data = file.readAll();
    }
    if (!ok) return file_failed(co, "couldn't be read");
    lua_pushlstring(co, data.constData(), data.size());
    return 1;
}

/* nk.file.write(path, data): true, or nil and why. The old file stays until the new one is whole. */
static int l_write(lua_State *co) {
    QString path = check_path(co, 1);
    size_t size;
    const char *data = luaL_checklstring(co, 2, &size);
    if (!write_whole(path, QByteArray(data, (int)size))) return file_failed(co, "couldn't be written");
    lua_pushboolean(co, 1);
    return 1;
}

static int l_exists(lua_State *co) {
    QString path = check_path(co, 1);
    lua_pushboolean(co, QFileInfo(path).exists());
    return 1;
}

/* nk.file.list(folder): the names in it, folders with a / after them. */
static int l_list(lua_State *co) {
    QString path = check_path(co, 1);
    QFileInfoList entries = QDir(path).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    lua_createtable(co, entries.size(), 0);
    for (int i = 0; i < entries.size(); i++) {
        push_qstring(co, entries[i].fileName() + (entries[i].isDir() ? QStringLiteral("/") : QString()));
        lua_rawseti(co, -2, i + 1);
    }
    return 1;
}

static int l_remove(lua_State *co) {
    QString path = check_path(co, 1);
    lua_pushboolean(co, QFile::remove(path) || QDir(path).rmdir(path));
    return 1;
}

static int l_mkdir(lua_State *co) {
    QString path = check_path(co, 1);
    lua_pushboolean(co, QDir().mkpath(path));
    return 1;
}

/* ---- nk.crypto ---- */

static int hash(lua_State *co, QCryptographicHash::Algorithm algorithm) {
    size_t size;
    const char *data = luaL_checklstring(co, 1, &size);
    QByteArray hex = QCryptographicHash::hash(QByteArray::fromRawData(data, (int)size), algorithm).toHex();
    lua_pushlstring(co, hex.constData(), hex.size());
    return 1;
}

static int l_md5(lua_State *co) { return hash(co, QCryptographicHash::Md5); }
static int l_sha1(lua_State *co) { return hash(co, QCryptographicHash::Sha1); }
static int l_sha256(lua_State *co) { return hash(co, QCryptographicHash::Sha256); }
static int l_sha512(lua_State *co) { return hash(co, QCryptographicHash::Sha512); }

/* nk.crypto.ed25519_verify(signature, key, message): whether message carries signature from key.
 * The signature and key are in hex. */
static int l_ed25519_verify(lua_State *co) {
    size_t signature_size, key_size, size;
    const char *signature_hex = luaL_checklstring(co, 1, &signature_size), *key_hex = luaL_checklstring(co, 2, &key_size);
    const char *message = luaL_checklstring(co, 3, &size);
    bool ok = false;
    {
        QByteArray signature = QByteArray::fromHex(QByteArray(signature_hex, (int)signature_size));
        QByteArray key = QByteArray::fromHex(QByteArray(key_hex, (int)key_size));
        if (signature.size() == 64 && key.size() == 32 && signature_size == 128 && key_size == 64)
            ok = !crypto_ed25519_check(reinterpret_cast<const uint8_t *>(signature.constData()),
                                       reinterpret_cast<const uint8_t *>(key.constData()), reinterpret_cast<const uint8_t *>(message), size);
    }
    lua_pushboolean(co, ok);
    return 1;
}

/* ---- nk.device ---- */

static char model[8], firmware[32];

/* .kobo/version is comma-separated: serial, kernel, firmware, ... The serial starts with the model. */
static void read_version() {
    static bool done;
    if (done) return;
    done = true;
    char line[256] = "";
    int fd = open("/mnt/onboard/.kobo/version", O_RDONLY);
    if (fd < 0) return;
    ssize_t length = read(fd, line, sizeof line - 1);
    close(fd);
    line[length > 0 ? length : 0] = 0;
    char *kernel = strchr(line, ','), *field = kernel ? strchr(kernel + 1, ',') : 0;
    if (!field) return;
    char *end = strpbrk(++field, ",\r\n");
    if (end) *end = 0;
    snprintf(model, sizeof model, "%.4s", line);
    snprintf(firmware, sizeof firmware, "%s", field);
}

const char *kit_model() {
    read_version();
    return model;
}

const char *kit_firmware() {
    read_version();
    return firmware;
}

static int l_model(lua_State *co) {
    lua_pushstring(co, kit_model());
    return 1;
}

static int l_firmware(lua_State *co) {
    lua_pushstring(co, kit_firmware());
    return 1;
}

/* Kobo's own settings, in .kobo/Kobo/Kobo eReader.conf, go through its Settings class: it keeps
 * them in memory for all of Nickel, writes the file, and tells Nickel what changed with a
 * SettingEvent, which screens like More listen for. Each section is a subclass that only gives its
 * name, through the virtual sectionName, so the kit's is a plain Settings on a copy of its vtable
 * whose sectionName gives the section asked for. */
class Device {
public:
    static Device &getCurrentDevice();
};

class Settings {
public:
    Settings(const Device &device, bool deferred);
    ~Settings();
    QVariant getSetting(const QString &key, const QVariant &otherwise);
    void saveSetting(const QString &key, const QVariant &value, bool force);
    void removeSetting(const QString &key);
};

extern void *settings_vtable[] __asm__("_ZTV8Settings");
extern void settings_section_name() __asm__("_ZNK8Settings11sectionNameEv");

/* Settings' vtable: offset and type, then the two destructors, two more, and sectionName. */
enum { SETTINGS_VTABLE_WORDS = 7, SECTION_NAME_SLOT = 6 };

/* Only ever used on Nickel's main thread, for the length of one call. */
static QLatin1String section_now("");

static const QLatin1String *section_name(const void *) { return &section_now; }

/* A Settings for key's section, made in memory, which has room for Nickel's 0x1c bytes. Or 0 when
 * the key isn't "Section/name", or Settings isn't what's expected. section and name get its parts. */
static Settings *open_settings(void *memory, const QByteArray &key, QByteArray *section, QString *name) {
    static void **vtable;
    if (!vtable) {
        if (settings_vtable[SECTION_NAME_SLOT] != reinterpret_cast<void *>(&settings_section_name)) {
            nk_log("Kobo's Settings isn't what's expected, so its settings are left alone");
            return 0;
        }
        vtable = static_cast<void **>(malloc(sizeof *vtable * SETTINGS_VTABLE_WORDS));
        memcpy(vtable, settings_vtable, sizeof *vtable * SETTINGS_VTABLE_WORDS);
        vtable[SECTION_NAME_SLOT] = reinterpret_cast<void *>(&section_name);
    }
    int slash = key.indexOf('/');
    if (slash < 1 || slash == key.size() - 1) return 0;
    *section = key.left(slash);
    *name = QString::fromUtf8(key.mid(slash + 1));
    section_now = QLatin1String(section->constData(), section->size());
    Settings *settings = new (memory) Settings(Device::getCurrentDevice(), false);
    *reinterpret_cast<void ***>(settings) = vtable + 2;
    return settings;
}

static void close_settings(Settings *settings) {
    settings->~Settings();
    section_now = QLatin1String("");
}

/* nk.device.setting(key): one of Kobo's own settings, as a string, or nil when it isn't set. key is
 * "Section/name". It's Nickel's value, as it has it in memory. Only once Nickel is running: never
 * from main.lua's top level. */
static int l_setting(lua_State *co) {
    const char *key = luaL_checkstring(co, 1);
    bool found = false;
    QByteArray text;
    {
        alignas(8) char memory[64];
        QByteArray section;
        QString name;
        Settings *settings = open_settings(memory, key, &section, &name);
        if (settings) {
            QVariant value = settings->getSetting(name, QVariant());
            close_settings(settings);
            found = value.isValid();
            text = value.toString().toUtf8();
        }
    }
    if (found) lua_pushlstring(co, text.constData(), text.size());
    else lua_pushnil(co);
    return 1;
}

/* nk.device.set_setting(key, value): sets one of Kobo's settings, or removes it when value is nil,
 * the way Nickel does: in its memory, in the file straight away, and with a SettingEvent, so what
 * listens for it follows. Returns whether it was set. Only from a tap or another callback, never
 * from main.lua's top level. */
static int l_set_setting(lua_State *co) {
    const char *key = luaL_checkstring(co, 1);
    const char *value = lua_isnoneornil(co, 2) ? 0 : luaL_checkstring(co, 2);
    bool ok = false;
    {
        alignas(8) char memory[64];
        QByteArray section;
        QString name;
        Settings *settings = open_settings(memory, key, &section, &name);
        if (settings) {
            /* Forced, because the file may have changed over USB since Nickel last read it. */
            if (value) settings->saveSetting(name, QString::fromUtf8(value), true);
            else settings->removeSetting(name);
            close_settings(settings);
            ok = true;
        }
    }
    lua_pushboolean(co, ok);
    return 1;
}

/* What Settings posts to the application after each change, Nickel's own and the kit's. */
class SettingEvent {
public:
    static int eventType();
    QString section() const;
    QString key() const;
    QVariant value() const;
};

static QList<int> *setting_watchers;

/* Sees every event the application gets, and passes on SettingEvents. It never takes one. */
class SettingWatch : public QObject {
public:
    bool eventFilter(QObject *, QEvent *event) {
        static int type = SettingEvent::eventType();
        if (event->type() != type) return false;
        const SettingEvent *change = reinterpret_cast<const SettingEvent *>(event);
        QByteArray key = (change->section() + QLatin1Char('/') + change->key()).toUtf8(), value;
        QVariant was = change->value();
        bool removed = !was.isValid();
        if (!removed) value = was.toString().toUtf8();
        QList<int> watchers = *setting_watchers;
        for (int i = 0; i < watchers.size(); i++) {
            lua_State *co = kit_task(watchers[i]);
            lua_pushlstring(co, key.constData(), key.size());
            if (removed) lua_pushnil(co);
            else lua_pushlstring(co, value.constData(), value.size());
            kit_resume(co, 2);
        }
        return false;
    }
};

/* Once Nickel's event loop runs, never while the mod loads. */
static void watch_settings(void *) {
    QCoreApplication::instance()->installEventFilter(new SettingWatch);
    nk_log("watching Kobo's settings");
}

/* nk.device.on_setting(fn): fn(key, value) runs after each change to one of Kobo's settings, by
 * Nickel or a mod, such as Kobo's sync writing what the store sends. key is "Section/name", and value
 * is a string, or nil when the setting was removed. It runs after the change, so setting it again
 * from fn wins, and that change comes back to fn too. From main.lua's top level it only records fn:
 * watching starts once Nickel is running. */
static int l_on_setting(lua_State *co) {
    int fn = kit_ref(co, 1);
    if (!setting_watchers) {
        if (!QCoreApplication::instance()) {
            nk_log("no Qt application, so Kobo's settings can't be watched");
            return 0;
        }
        setting_watchers = new QList<int>;
        QTimer *soon = new QTimer();
        soon->setSingleShot(true);
        kit_connect(soon, &QTimer::timeout, &QTimer::staticMetaObject, watch_settings, 0);
        QObject::connect(soon, &QTimer::timeout, soon, &QObject::deleteLater);
        soon->start(0);
    }
    setting_watchers->append(fn);
    return 0;
}

/* ---- the mod's own file ---- */

/* nk.self_sha512(): the hash of the mod's .so as installed, or nil. */
static int l_self_sha512(lua_State *co) {
    QByteArray hex;
    {
        QFile file(QString::fromUtf8(nk_self_path()));
        if (file.open(QIODevice::ReadOnly)) hex = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha512).toHex();
    }
    if (hex.isEmpty()) lua_pushnil(co);
    else lua_pushlstring(co, hex.constData(), hex.size());
    return 1;
}

/* nk.replace_self(data, sha512): installs data as the mod's new build, for when Nickel next starts,
 * if it has that hash. Returns whether it did. */
static int l_replace_self(lua_State *co) {
    size_t size, hash_size;
    const char *data = luaL_checklstring(co, 1, &size), *expected = luaL_checklstring(co, 2, &hash_size);
    bool ok = false;
    {
        QByteArray hex = QCryptographicHash::hash(QByteArray::fromRawData(data, (int)size), QCryptographicHash::Sha512).toHex();
        if (size && hash_size == 128 && hex == QByteArray(expected, (int)hash_size)) ok = nk_replace_self(data, (unsigned)size);
    }
    lua_pushboolean(co, ok);
    return 1;
}

static int l_reboot(lua_State *) {
    nk_reboot();
    return 0;
}

/* nk.spawn(command): runs a shell command outside Nickel, from /. */
static int l_spawn(lua_State *co) {
    nk_spawn(luaL_checkstring(co, 1));
    return 0;
}

static void add_library(lua_State *L, const char *name, const luaL_Reg *functions) {
    lua_newtable(L);
    luaL_setfuncs(L, functions, 0);
    lua_setfield(L, -2, name);
}

void kit_open_tools(lua_State *L) {
    static const luaL_Reg http_functions[] = {{"get", l_get}, {"post", l_post}, {0, 0}};
    static const luaL_Reg re_functions[] = {{"match", l_match}, {"find", l_find}, {"all", l_all}, {"gsub", l_gsub}, {0, 0}};
    static const luaL_Reg file_functions[] = {{"read", l_read}, {"write", l_write}, {"exists", l_exists}, {"list", l_list},
                                              {"remove", l_remove}, {"mkdir", l_mkdir}, {0, 0}};
    static const luaL_Reg crypto_functions[] = {{"md5", l_md5}, {"sha1", l_sha1}, {"sha256", l_sha256}, {"sha512", l_sha512},
                                                {"ed25519_verify", l_ed25519_verify}, {0, 0}};
    static const luaL_Reg device_functions[] = {{"model", l_model}, {"firmware", l_firmware}, {"setting", l_setting},
                                                {"set_setting", l_set_setting}, {"on_setting", l_on_setting}, {0, 0}};
    static const luaL_Reg text_functions[] = {{"lower", l_lower}, {"decompose", l_decompose}, {"language_name", l_language_name}, {0, 0}};
    add_library(L, "http", http_functions);
    add_library(L, "re", re_functions);
    add_library(L, "file", file_functions);
    add_library(L, "crypto", crypto_functions);
    add_library(L, "device", device_functions);
    add_library(L, "text", text_functions);
    static const luaL_Reg self_functions[] = {{"self_sha512", l_self_sha512}, {"replace_self", l_replace_self}, {"reboot", l_reboot},
                                              {"spawn", l_spawn}, {0, 0}};
    luaL_setfuncs(L, self_functions, 0);
    kit_open_patch(L);
    lua_newtable(L);
    lua_pushcfunction(L, l_decode);
    lua_setfield(L, -2, "decode");
    lua_pushcfunction(L, l_encode);
    lua_setfield(L, -2, "encode");
    lua_pushlightuserdata(L, &json_null);
    lua_setfield(L, -2, "null");
    lua_setfield(L, -2, "json");
    lua_pushcfunction(L, l_download);
    lua_setfield(L, -2, "download");
    lua_pushcfunction(L, l_import);
    lua_setfield(L, -2, "import");
}
