#include "remote.h"

#include "job.h"
#include "library.h"
#include "scanner.h"
#include "subsonic.h"
#include "tagreader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

#include <cmath>

// --- Secrets ------------------------------------------------------------

namespace Secrets {

QString preferredStore() {
    const QString forced = qEnvironmentVariable("OMAMP_SECRET_STORE");
    if (forced == "file" || forced == "libsecret")
        return forced;
    if (!QStandardPaths::findExecutable("secret-tool").isEmpty() && qEnvironmentVariableIsSet("DBUS_SESSION_BUS_ADDRESS"))
        return "libsecret";
    return "file";
}

QString filePath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omamp/credentials.json";
}

static QJsonObject readFile() {
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

static bool writeFile(const QJsonObject &all, QString *error) {
    const QString path = filePath();
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QString("Can't create %1").arg(dir);
        return false;
    }
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QString("Can't write %1: %2").arg(path, f.errorString());
        return false;
    }
    // Private before a byte of the secret is written.
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(all).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error)
            *error = QString("Can't write %1: %2").arg(path, f.errorString());
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

static bool runSecretTool(const QStringList &args, const QByteArray &input, QByteArray *output, QString *error) {
    QProcess p;
    p.start("secret-tool", args);
    if (!p.waitForStarted(5000)) {
        if (error)
            *error = "secret-tool could not be started";
        return false;
    }
    if (!input.isNull()) {
        p.write(input);
    }
    p.closeWriteChannel();
    if (!p.waitForFinished(15000)) {
        p.kill();
        p.waitForFinished(1000);
        if (error)
            *error = "secret-tool did not finish (is the keyring locked?)";
        return false;
    }
    if (output)
        *output = p.readAllStandardOutput();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error)
            *error = QString("secret-tool failed: %1").arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
        return false;
    }
    return true;
}

bool store(const QString &storeName, const QString &ref, const QString &secret, const QString &label, QString *error) {
    if (storeName == "libsecret")
        return runSecretTool({"store", "--label=" + label, "application", "omamp", "remote", ref}, secret.toUtf8(),
                             nullptr, error);
    QJsonObject all = readFile();
    all[ref] = secret;
    return writeFile(all, error);
}

QString lookup(const QString &storeName, const QString &ref, QString *error) {
    if (storeName == "libsecret") {
        QByteArray out;
        if (!runSecretTool({"lookup", "application", "omamp", "remote", ref}, QByteArray(), &out, error))
            return {};
        if (out.isEmpty() && error)
            *error = "The keyring has no secret for this source; add it again";
        return QString::fromUtf8(out);
    }
    const QJsonObject all = readFile();
    if (!all.contains(ref)) {
        if (error)
            *error = QString("%1 has no secret for this source; add it again").arg(filePath());
        return {};
    }
    return all.value(ref).toString();
}

bool remove(const QString &storeName, const QString &ref) {
    if (storeName == "libsecret")
        return runSecretTool({"clear", "application", "omamp", "remote", ref}, QByteArray(), nullptr, nullptr);
    QJsonObject all = readFile();
    if (!all.contains(ref))
        return true;
    all.remove(ref);
    return writeFile(all, nullptr);
}

}

// --- Sources ------------------------------------------------------------

QJsonObject RemoteSource::toJson() const {
    return {{"id", id},
            {"kind", kind},
            {"name", name},
            {"url", url},
            {"username", username},
            {"auth", auth},
            {"secret_store", secretStore},
            {"server_type", serverType},
            {"server_version", serverVersion},
            {"api_version", apiVersion},
            {"open_subsonic", openSubsonic},
            {"max_bitrate", maxBitrate},
            {"stream_format", streamFormat},
            {"volume_id", volumeId},
            {"last_sync", lastSync},
            {"last_error", lastError}};
}

QJsonObject RemoteSyncResult::toJson() const {
    QJsonObject o{{"ok", ok},
                  {"albums", albums},
                  {"albums_fetched", albumsFetched},
                  {"albums_unchanged", albumsUnchanged},
                  {"albums_gone", albumsGone},
                  {"tracks_added", tracksAdded},
                  {"tracks_updated", tracksUpdated},
                  {"tracks_missing", tracksMissing},
                  {"tracks_back", tracksBack},
                  {"requests", requests},
                  {"elapsed_ms", elapsedMs},
                  {"cancelled", cancelled}};
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

namespace RemoteSources {

namespace {

QMutex g_cacheMutex;
QHash<QString, QString> g_secrets; // secret_ref -> secret, for this process

const char *selectSource =
    "SELECT id, kind, name, url, username, auth, secret_store, secret_ref, coalesce(server_type, ''), "
    "coalesce(server_version, ''), coalesce(api_version, ''), open_subsonic, max_bitrate, coalesce(stream_format, ''), "
    "coalesce(volume_id, 0), coalesce(last_sync, 0), coalesce(last_error, '') FROM remote_sources ";

RemoteSource fromQuery(const QSqlQuery &q) {
    RemoteSource s;
    s.id = q.value(0).toLongLong();
    s.kind = q.value(1).toString();
    s.name = q.value(2).toString();
    s.url = q.value(3).toString();
    s.username = q.value(4).toString();
    s.auth = q.value(5).toString();
    s.secretStore = q.value(6).toString();
    s.secretRef = q.value(7).toString();
    s.serverType = q.value(8).toString();
    s.serverVersion = q.value(9).toString();
    s.apiVersion = q.value(10).toString();
    s.openSubsonic = q.value(11).toBool();
    s.maxBitrate = q.value(12).toInt();
    s.streamFormat = q.value(13).toString();
    s.volumeId = q.value(14).toLongLong();
    s.lastSync = q.value(15).toLongLong();
    s.lastError = q.value(16).toString();
    return s;
}

RemoteSource byId(const Library &library, qint64 id) {
    QSqlQuery q(library.db());
    q.prepare(QString(selectSource) + "WHERE id = ?");
    q.addBindValue(id);
    if (q.exec() && q.next())
        return fromQuery(q);
    return {};
}

QString secretFor(const RemoteSource &s, QString *error) {
    {
        QMutexLocker lock(&g_cacheMutex);
        const auto it = g_secrets.constFind(s.secretRef);
        if (it != g_secrets.constEnd())
            return it.value();
    }
    const QString secret = Secrets::lookup(s.secretStore, s.secretRef, error);
    if (!secret.isEmpty()) {
        QMutexLocker lock(&g_cacheMutex);
        g_secrets.insert(s.secretRef, secret);
    }
    return secret;
}

QStringList names(const QJsonValue &list, const QString &fallback) {
    QStringList out;
    for (const QJsonValue &v : list.toArray()) {
        const QString name = v.isObject() ? v.toObject().value("name").toString() : v.toString();
        if (!name.isEmpty())
            out << name;
    }
    if (out.isEmpty() && !fallback.isEmpty())
        out << fallback;
    return out;
}

// A song as the scanner would have read it from the file.
Scanner::TrackRow rowFor(const QJsonObject &song, const QJsonObject &album) {
    Scanner::TrackRow row;
    TrackTags &t = row.tags;
    t.ok = true;
    t.title = song.value("title").toString();
    t.album = song.value("album").toString(album.value("name").toString());
    t.artists = names(song.value("artists"), song.value("artist").toString());
    t.albumArtists = names(song.value("albumArtists"), song.value("displayAlbumArtist").toString(album.value("artist").toString()));
    t.composers = names(QJsonValue(), song.value("displayComposer").toString());
    t.genres = names(song.value("genres"), song.value("genre").toString());
    t.year = song.value("year").toInt(album.value("year").toInt());
    t.trackNo = song.value("track").toInt();
    t.discNo = song.value("discNumber").toInt();
    t.bpm = song.value("bpm").toInt();
    t.comment = song.value("comment").toString();
    t.compilation = album.value("isCompilation").toBool();
    t.mbTrackId = song.value("musicBrainzId").toString();
    t.mbReleaseId = album.value("musicBrainzId").toString();
    const QJsonValue isrc = song.value("isrc");
    t.isrc = isrc.isArray() ? isrc.toArray().first().toString() : isrc.toString();
    t.durationMs = qint64(song.value("duration").toDouble() * 1000.0);
    t.bitrate = song.value("bitRate").toInt();
    t.sampleRate = song.value("samplingRate").toInt();
    t.channels = song.value("channelCount").toInt();
    const QJsonObject gain = song.value("replayGain").toObject();
    if (gain.contains("trackGain"))
        t.rgTrackGain = gain.value("trackGain").toDouble();
    if (gain.contains("trackPeak"))
        t.rgTrackPeak = gain.value("trackPeak").toDouble();
    if (gain.contains("albumGain"))
        t.rgAlbumGain = gain.value("albumGain").toDouble();
    if (gain.contains("albumPeak"))
        t.rgAlbumPeak = gain.value("albumPeak").toDouble();
    // The server's own rating and play count fill the library's while those
    // are empty, as a file's tags do; the library's values always win.
    if (song.contains("userRating"))
        t.tagRating = qBound(0, song.value("userRating").toInt(), 5) * 20;
    if (song.contains("playCount"))
        t.tagPlayCount = song.value("playCount").toInt();
    t.hasEmbeddedArt = song.contains("coverArt");
    const QString type = song.value("mediaType").toString(song.value("type").toString());
    if (type == "podcast" || type == "audiobook")
        t.contentType = type;

    row.relPath = song.value("id").toString();
    const QString path = song.value("path").toString();
    QString file = path.section('/', -1);
    const int dot = file.lastIndexOf('.');
    if (dot > 0)
        file = file.left(dot);
    row.fileName = file.isEmpty() ? t.title : file;
    row.format = song.value("suffix").toString().toLower();
    row.size = song.value("size").toVariant().toLongLong();
    const QDateTime created = QDateTime::fromString(song.value("created").toString(), Qt::ISODateWithMs);
    row.mtimeNs = created.isValid() ? created.toMSecsSinceEpoch() * 1000000LL : 0;
    return row;
}

QString fingerprint(const QJsonObject &album) {
    QJsonObject keep;
    for (const char *key : {"name", "artist", "artistId", "songCount", "duration", "year", "genre", "coverArt",
                            "created", "changed", "musicBrainzId", "isCompilation"})
        if (album.contains(key))
            keep[key] = album.value(key);
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(keep).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1).toHex());
}

void setOffline(Library &library, const RemoteSource &s, const QString &error) {
    QSqlDatabase db = library.db();
    QSqlQuery q(db);
    q.prepare("UPDATE remote_sources SET last_error = ? WHERE id = ?");
    q.addBindValue(error);
    q.addBindValue(s.id);
    q.exec();
    if (!s.volumeId)
        return;
    q.prepare("UPDATE volumes SET online = 0 WHERE id = ?");
    q.addBindValue(s.volumeId);
    q.exec();
    q.prepare("UPDATE tracks SET availability = 'offline' WHERE volume_id = ? AND availability = 'online'");
    q.addBindValue(s.volumeId);
    q.exec();
}

} // namespace

std::unique_ptr<SubsonicClient> client(const Library &library, const RemoteSource &source, QString *error) {
    Q_UNUSED(library)
    const QString secret = secretFor(source, error);
    if (secret.isEmpty())
        return nullptr;
    return std::make_unique<SubsonicClient>(QUrl(source.url), source.username, secret,
                                            source.auth == "apikey" ? SubsonicClient::Auth::ApiKey
                                                                    : SubsonicClient::Auth::Token);
}

qint64 add(Library &library, const QUrl &url, const QString &user, const QString &secret, const QString &auth,
           const QString &name, QString *error, RemoteSource *added) {
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return qint64(0);
    };
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty())
        return fail(QString("%1 is not an http:// or https:// address").arg(url.toString()));
    if (!url.userInfo().isEmpty())
        return fail("Put the user in --user, not in the address");
    if (auth != "token" && auth != "apikey")
        return fail("auth must be token or apikey");
    if (auth == "token" && user.isEmpty())
        return fail("A password needs a user: --user <name>");
    if (secret.isEmpty())
        return fail(auth == "apikey" ? "The API key is empty" : "The password is empty");

    SubsonicClient c(url, user, secret, auth == "apikey" ? SubsonicClient::Auth::ApiKey : SubsonicClient::Auth::Token);
    SubsonicServer server;
    QString why;
    if (!c.ping(&server, &why))
        return fail(QString("Could not sign in to %1: %2").arg(url.toString(), why));

    const QString store = Secrets::preferredStore();
    const QString ref = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString label = name.isEmpty() ? url.host() : name;
    if (!Secrets::store(store, ref, secret, QString("Omamp: %1").arg(label), &why))
        return fail(QString("Could not store the secret (%1): %2").arg(store, why));

    QSqlQuery q(library.db());
    q.prepare("INSERT INTO remote_sources (kind, name, url, username, auth, secret_store, secret_ref, server_type, "
              "server_version, api_version, open_subsonic, added) VALUES ('subsonic', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    QUrl clean = url;
    QString path = clean.path();
    while (path.endsWith('/'))
        path.chop(1);
    clean.setPath(path);
    for (const QVariant &v : {QVariant(label), QVariant(clean.toString()), QVariant(user), QVariant(auth), QVariant(store),
                              QVariant(ref), QVariant(server.type), QVariant(server.serverVersion),
                              QVariant(server.apiVersion), QVariant(server.openSubsonic ? 1 : 0),
                              QVariant(QDateTime::currentSecsSinceEpoch())})
        q.addBindValue(v);
    if (!q.exec()) {
        Secrets::remove(store, ref);
        const QString message = q.lastError().text();
        return fail(message.contains("UNIQUE") ? QString("%1 is already a source for %2").arg(clean.toString(), user)
                                               : message);
    }
    const qint64 id = q.lastInsertId().toLongLong();
    {
        QMutexLocker lock(&g_cacheMutex);
        g_secrets.insert(ref, secret);
    }
    if (added)
        *added = byId(library, id);
    return id;
}

QList<RemoteSource> list(Library &library) {
    QList<RemoteSource> out;
    QSqlQuery q(library.db());
    q.exec(QString(selectSource) + "ORDER BY id");
    while (q.next())
        out << fromQuery(q);
    return out;
}

RemoteSource find(Library &library, const QString &which, QString *error) {
    bool number = false;
    const qint64 id = which.toLongLong(&number);
    for (const RemoteSource &s : list(library))
        if ((number && s.id == id) || s.name == which)
            return s;
    if (error)
        *error = QString("No remote source matches %1 (see: omamp remote list)").arg(which);
    return {};
}

bool remove(Library &library, qint64 id, bool keepTracks, QString *error) {
    const RemoteSource s = byId(library, id);
    if (!s.id) {
        if (error)
            *error = QString("No remote source %1").arg(id);
        return false;
    }
    QSqlDatabase db = library.db();
    db.transaction();
    QSqlQuery q(db);
    bool ok = true;
    if (s.volumeId) {
        if (keepTracks) {
            q.prepare("UPDATE volumes SET online = 0, remote_source_id = NULL WHERE id = ?");
            q.addBindValue(s.volumeId);
            ok = q.exec();
            q.prepare("UPDATE tracks SET availability = 'offline' WHERE volume_id = ? AND availability = 'online'");
            q.addBindValue(s.volumeId);
            ok = ok && q.exec();
        } else {
            q.prepare("DELETE FROM tracks WHERE volume_id = ?");
            q.addBindValue(s.volumeId);
            ok = q.exec();
            q.prepare("DELETE FROM folders WHERE volume_id = ?");
            q.addBindValue(s.volumeId);
            ok = ok && q.exec();
            q.prepare("DELETE FROM volumes WHERE id = ?");
            q.addBindValue(s.volumeId);
            ok = ok && q.exec();
        }
    }
    q.prepare("DELETE FROM remote_sources WHERE id = ?");
    q.addBindValue(id);
    ok = ok && q.exec();
    if (!ok) {
        const QString message = q.lastError().text();
        db.rollback();
        if (error)
            *error = message;
        return false;
    }
    db.commit();
    if (!keepTracks)
        Scanner::tidyLibrary(library);
    Secrets::remove(s.secretStore, s.secretRef);
    QMutexLocker lock(&g_cacheMutex);
    g_secrets.remove(s.secretRef);
    return true;
}

bool setStreaming(Library &library, qint64 id, int maxBitrate, const QString &format, QString *error) {
    QSqlQuery q(library.db());
    q.prepare("UPDATE remote_sources SET max_bitrate = ?, stream_format = ? WHERE id = ?");
    q.addBindValue(qMax(0, maxBitrate));
    q.addBindValue(format.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(format));
    q.addBindValue(id);
    if (!q.exec() || q.numRowsAffected() != 1) {
        if (error)
            *error = QString("No remote source %1").arg(id);
        return false;
    }
    return true;
}

RemoteSyncResult sync(Library &library, qint64 id, bool full, Job *job) {
    RemoteSyncResult result;
    QElapsedTimer timer;
    timer.start();
    auto finish = [&](const QString &error = {}) {
        if (!error.isEmpty()) {
            result.ok = false;
            result.error = error;
        }
        result.elapsedMs = timer.elapsed();
        if (job) {
            if (result.ok)
                job->finish();
            else
                job->fail(result.error);
        }
        return result;
    };
    if (job)
        job->start();
    RemoteSource s = byId(library, id);
    if (!s.id)
        return finish(QString("No remote source %1").arg(id));
    QString why;
    std::unique_ptr<SubsonicClient> c = client(library, s, &why);
    if (!c)
        return finish(why);
    SubsonicServer server;
    if (!c->ping(&server, &why)) {
        setOffline(library, s, why);
        result.requests = c->requests();
        return finish(QString("%1 is offline: %2").arg(s.name, why));
    }

    QSqlDatabase db = library.db();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // The volume that holds this source's tracks.
    {
        QSqlQuery v(db);
        v.prepare("INSERT INTO volumes (key, kind, label, fs_type, source, mount_subpath, root_path, online, last_seen, "
                  "added, remote_source_id) VALUES (?, 'subsonic', ?, 'subsonic', ?, '', ?, 1, ?, ?, ?) "
                  "ON CONFLICT(key) DO UPDATE SET label = excluded.label, source = excluded.source, online = 1, "
                  "last_seen = excluded.last_seen, remote_source_id = excluded.remote_source_id RETURNING id");
        for (const QVariant &value : {QVariant(QString("subsonic:%1").arg(s.id)), QVariant(s.name), QVariant(s.url),
                                      QVariant(QString("subsonic://%1").arg(s.id)), QVariant(now), QVariant(now),
                                      QVariant(s.id)})
            v.addBindValue(value);
        if (!v.exec() || !v.next())
            return finish(v.lastError().text());
        s.volumeId = v.value(0).toLongLong();
        v.finish();
        QSqlQuery u(db);
        u.prepare("UPDATE remote_sources SET volume_id = ?, server_type = ?, server_version = ?, api_version = ?, "
                  "open_subsonic = ? WHERE id = ?");
        for (const QVariant &value : {QVariant(s.volumeId), QVariant(server.type), QVariant(server.serverVersion),
                                      QVariant(server.apiVersion), QVariant(server.openSubsonic ? 1 : 0), QVariant(s.id)})
            u.addBindValue(value);
        u.exec();
    }

    // 1. Every album, page by page. A failed page stops the sync before
    // anything is marked missing.
    QList<QJsonObject> albums;
    constexpr int page = 500;
    for (int offset = 0;; offset += page) {
        if (job && job->isCancelled()) {
            result.cancelled = true;
            result.requests = c->requests();
            return finish();
        }
        why.clear();
        const QJsonArray batch = c->albumList2("alphabeticalByName", page, offset, &why);
        if (!why.isEmpty()) {
            result.requests = c->requests();
            return finish(QString("Listing albums failed at %1: %2").arg(offset).arg(why));
        }
        for (const QJsonValue &a : batch)
            albums << a.toObject();
        if (job)
            job->setProgress("Listing albums", albums.size(), 0);
        if (batch.size() < page || offset > 5000000)
            break;
    }
    result.albums = albums.size();

    // 2. What the last sync saw.
    QHash<QString, QString> known; // album id -> fingerprint
    {
        QSqlQuery q(db);
        q.prepare("SELECT album_id, fingerprint FROM remote_albums WHERE source_id = ?");
        q.addBindValue(s.id);
        q.exec();
        while (q.next())
            known.insert(q.value(0).toString(), q.value(1).toString());
    }
    QHash<QString, qint64> tracks;      // song id -> track id
    QHash<qint64, QString> availability;
    {
        QSqlQuery q(db);
        q.prepare("SELECT id, rel_path, availability FROM tracks WHERE volume_id = ?");
        q.addBindValue(s.volumeId);
        q.exec();
        while (q.next()) {
            tracks.insert(q.value(1).toString(), q.value(0).toLongLong());
            availability.insert(q.value(0).toLongLong(), q.value(2).toString());
        }
    }

    // 3. Fetch what changed; keep what didn't.
    QSet<qint64> present;
    QStringList unchangedAlbums;
    QSet<QString> listed;
    QSqlQuery saveAlbum(db), link(db);
    saveAlbum.prepare("INSERT INTO remote_albums (source_id, album_id, fingerprint, synced) VALUES (?, ?, ?, ?) "
                      "ON CONFLICT(source_id, album_id) DO UPDATE SET fingerprint = excluded.fingerprint, "
                      "synced = excluded.synced");
    link.prepare("INSERT INTO remote_tracks (track_id, source_id, album_id, cover_art) VALUES (?, ?, ?, ?) "
                 "ON CONFLICT(track_id) DO UPDATE SET album_id = excluded.album_id, cover_art = excluded.cover_art");
    int done = 0;
    for (const QJsonObject &summary : std::as_const(albums)) {
        if (job && job->isCancelled()) {
            result.cancelled = true;
            break;
        }
        const QString albumId = summary.value("id").toString();
        listed.insert(albumId);
        const QString print = fingerprint(summary);
        if (job && ++done % 50 == 0)
            job->setProgress("Syncing albums", done, albums.size());
        if (!full && known.value(albumId) == print) {
            ++result.albumsUnchanged;
            unchangedAlbums << albumId;
            continue;
        }
        why.clear();
        const QJsonObject album = c->album(albumId, &why);
        if (!why.isEmpty()) {
            // One album that fails is retried next time; it isn't recorded.
            result.error = QString("Album %1: %2").arg(albumId, why);
            unchangedAlbums << albumId;
            continue;
        }
        ++result.albumsFetched;
        QList<Scanner::TrackRow> rows;
        QStringList covers;
        for (const QJsonValue &v : album.value("song").toArray()) {
            const QJsonObject song = v.toObject();
            if (song.value("isVideo").toBool() || song.value("id").toString().isEmpty())
                continue;
            Scanner::TrackRow row = rowFor(song, album);
            row.trackId = tracks.value(row.relPath, 0);
            rows << row;
            covers << song.value("coverArt").toString(album.value("coverArt").toString());
        }
        db.transaction();
        QString writeError;
        if (!Scanner::writeTrackRows(library, s.volumeId, rows, &writeError)) {
            db.rollback();
            result.requests = c->requests();
            return finish(writeError);
        }
        for (int i = 0; i < rows.size(); ++i) {
            const Scanner::TrackRow &row = rows[i];
            if (tracks.contains(row.relPath)) {
                ++result.tracksUpdated;
                if (availability.value(row.trackId) != "online")
                    ++result.tracksBack;
            } else {
                ++result.tracksAdded;
                tracks.insert(row.relPath, row.trackId);
            }
            present.insert(row.trackId);
            link.addBindValue(row.trackId);
            link.addBindValue(s.id);
            link.addBindValue(albumId);
            link.addBindValue(covers[i].isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(covers[i]));
            link.exec();
        }
        saveAlbum.addBindValue(s.id);
        saveAlbum.addBindValue(albumId);
        saveAlbum.addBindValue(print);
        saveAlbum.addBindValue(now);
        saveAlbum.exec();
        db.commit();
    }

    // 4. Songs of unchanged albums are still there; anything else on this
    // volume is not (unless the sync was cut short).
    db.transaction();
    {
        QSqlQuery q(db);
        q.prepare("SELECT track_id FROM remote_tracks WHERE source_id = ? AND album_id = ?");
        for (const QString &albumId : std::as_const(unchangedAlbums)) {
            q.addBindValue(s.id);
            q.addBindValue(albumId);
            q.exec();
            while (q.next())
                present.insert(q.value(0).toLongLong());
        }
    }
    QSqlQuery mark(db);
    mark.prepare("UPDATE tracks SET availability = ? WHERE id = ?");
    for (auto it = availability.cbegin(); it != availability.cend(); ++it) {
        const bool here = present.contains(it.key());
        if (here && it.value() != "online") {
            mark.addBindValue("online");
            mark.addBindValue(it.key());
            mark.exec();
            // Rewritten rows were counted already.
        } else if (!here && !result.cancelled && it.value() != "missing") {
            mark.addBindValue("missing");
            mark.addBindValue(it.key());
            mark.exec();
            ++result.tracksMissing;
        }
    }
    if (!result.cancelled) {
        QSqlQuery gone(db);
        gone.prepare("SELECT album_id FROM remote_albums WHERE source_id = ?");
        gone.addBindValue(s.id);
        gone.exec();
        QStringList drop;
        while (gone.next())
            if (!listed.contains(gone.value(0).toString()))
                drop << gone.value(0).toString();
        QSqlQuery del(db);
        del.prepare("DELETE FROM remote_albums WHERE source_id = ? AND album_id = ?");
        for (const QString &albumId : std::as_const(drop)) {
            del.addBindValue(s.id);
            del.addBindValue(albumId);
            del.exec();
        }
        result.albumsGone = drop.size();
    }
    QSqlQuery done2(db);
    done2.prepare("UPDATE remote_sources SET last_sync = ?, last_error = ? WHERE id = ?");
    done2.addBindValue(now);
    done2.addBindValue(result.error.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(result.error));
    done2.addBindValue(s.id);
    done2.exec();
    db.commit();
    if (result.tracksUpdated || result.tracksMissing || result.albumsGone)
        Scanner::tidyLibrary(library);
    result.requests = c->requests();
    // A failed album leaves result.error set (it is retried next time), but
    // the sync as a whole went through, so ok stays true.
    result.elapsedMs = timer.elapsed();
    if (job)
        job->finish();
    return result;
}

QUrl streamUrl(const Library &library, qint64 sourceId, const QString &songId, QString *error) {
    const RemoteSource s = byId(library, sourceId);
    if (!s.id) {
        if (error)
            *error = QString("The track's remote source (%1) is gone").arg(sourceId);
        return {};
    }
    std::unique_ptr<SubsonicClient> c = client(library, s, error);
    if (!c)
        return {};
    return c->streamUrl(songId, s.maxBitrate, s.streamFormat);
}

namespace {

struct RemoteTrack {
    RemoteSource source;
    QString songId, coverArt;
};

RemoteTrack remoteTrack(Library &library, qint64 trackId, QString *error) {
    RemoteTrack r;
    QSqlQuery q(library.db());
    q.prepare("SELECT v.remote_source_id, t.rel_path, coalesce(rt.cover_art, '') FROM tracks t "
              "JOIN volumes v ON v.id = t.volume_id LEFT JOIN remote_tracks rt ON rt.track_id = t.id "
              "WHERE t.id = ? AND v.kind = 'subsonic'");
    q.addBindValue(trackId);
    if (!q.exec() || !q.next()) {
        if (error)
            *error = QString("Track %1 is not from a remote source").arg(trackId);
        return r;
    }
    r.source = byId(library, q.value(0).toLongLong());
    r.songId = q.value(1).toString();
    r.coverArt = q.value(2).toString();
    if (!r.source.id && error)
        *error = QString("Track %1's remote source is gone").arg(trackId);
    return r;
}

} // namespace

QString coverArt(Library &library, qint64 trackId, int size, QString *error) {
    const RemoteTrack t = remoteTrack(library, trackId, error);
    if (!t.source.id)
        return {};
    const QString id = t.coverArt.isEmpty() ? t.songId : t.coverArt;
    // The id goes into a file name: keep it to safe characters.
    QString safe = id;
    for (QChar &ch : safe)
        if (!ch.isLetterOrNumber() && ch != '-' && ch != '_')
            ch = '_';
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
                        QString("/omamp/subsonic/%1").arg(t.source.id);
    const QString base = QString("%1/%2-%3").arg(dir, safe).arg(size);
    for (const char *ext : {".jpg", ".png", ".webp", ".img"})
        if (QFileInfo::exists(base + ext))
            return base + ext;
    std::unique_ptr<SubsonicClient> c = client(library, t.source, error);
    if (!c)
        return {};
    QString type;
    const QByteArray bytes = c->coverArt(id, size, &type, error);
    if (bytes.isEmpty())
        return {};
    const QString ext = type.contains("png") ? ".png" : type.contains("webp") ? ".webp" : type.contains("jpeg") || type.contains("jpg") ? ".jpg" : ".img";
    QDir().mkpath(dir);
    QSaveFile f(base + ext);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return {};
    }
    f.write(bytes);
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return {};
    }
    return base + ext;
}

bool scrobble(Library &library, qint64 trackId, bool submission, qint64 timeMs, QString *error) {
    const RemoteTrack t = remoteTrack(library, trackId, error);
    if (!t.source.id)
        return false;
    std::unique_ptr<SubsonicClient> c = client(library, t.source, error);
    return c && c->scrobble(t.songId, submission, timeMs, error);
}

bool setRating(Library &library, qint64 trackId, int rating, QString *error) {
    const RemoteTrack t = remoteTrack(library, trackId, error);
    if (!t.source.id)
        return false;
    std::unique_ptr<SubsonicClient> c = client(library, t.source, error);
    // 0..100 to whole stars, rounding half up (50 is 3 stars, 10 is 1).
    const int stars = rating <= 0 ? 0 : qBound(1, int(std::lround(rating / 20.0)), 5);
    return c && c->setRating(t.songId, stars, error);
}

void clearSecretCache() {
    QMutexLocker lock(&g_cacheMutex);
    g_secrets.clear();
}

}
