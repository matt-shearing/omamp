#include "coverart.h"

#include "artwork.h"
#include "audit.h"
#include "job.h"
#include "library.h"
#include "musicbrainz.h"
#include "tagwriter.h"
#include "textmatch.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QSqlQuery>

namespace CoverArt {

namespace {

constexpr int indexCacheSeconds = 7 * 86400;

QString extensionFor(const QString &mime) {
    if (mime.contains("png"))
        return "png";
    if (mime.contains("gif"))
        return "gif";
    if (mime.contains("webp"))
        return "webp";
    return "jpg";
}

// The archive's links say http; ask for https so nothing is redirected to a
// weaker scheme.
QUrl secure(const QString &link) {
    QUrl url(link);
    if (url.scheme() == "http" && url.host().endsWith("coverartarchive.org"))
        url.setScheme("https");
    return url;
}

bool hasPictureFile(const QString &folder) {
    const QStringList names = QDir(folder).entryList(QDir::Files);
    for (const QString &n : names) {
        const QString base = QFileInfo(n).completeBaseName().toLower();
        const QString ext = QFileInfo(n).suffix().toLower();
        if (QStringList{"cover", "folder", "front"}.contains(base) &&
            QStringList{"jpg", "jpeg", "png", "webp", "bmp", "gif"}.contains(ext))
            return true;
    }
    return false;
}

} // namespace

int snapSize(int requested) {
    if (requested <= 0)
        return 0;
    if (requested <= 250)
        return 250;
    if (requested <= 500)
        return 500;
    return 1200;
}

Fetched front(Net::HttpClient &http, const QString &releaseId, int sizeIn, const QString &releaseGroupId) {
    Fetched f;
    const int size = snapSize(sizeIn);
    const QString base = Net::baseUrl(Net::Service::CoverArt, http.library());

    QJsonObject index;
    auto tryIndex = [&](const QString &kind, const QString &id) {
        if (id.isEmpty())
            return false;
        const Net::Response r = http.get(Net::Service::CoverArt, QUrl(QString("%1/%2/%3").arg(base, kind, id)),
                                         indexCacheSeconds);
        f.offline = f.offline || r.offline;
        if (!r.ok()) {
            if (!r.notFound())
                f.error = QString("Cover Art Archive: %1").arg(r.error);
            return false;
        }
        index = r.json().object();
        return true;
    };
    bool found = tryIndex("release", releaseId);
    if (found)
        f.releaseId = releaseId;
    else if (tryIndex("release-group", releaseGroupId))
        f.releaseGroupId = releaseGroupId, found = true;
    if (!found) {
        f.notFound = f.error.isEmpty();
        if (f.notFound)
            f.error = "The Cover Art Archive has no picture for this release";
        return f;
    }

    QJsonObject chosen;
    for (const QJsonValue &v : index.value("images").toArray()) {
        const QJsonObject image = v.toObject();
        if (image.value("front").toBool() || image.value("types").toArray().contains(QJsonValue("Front"))) {
            chosen = image;
            break;
        }
    }
    if (chosen.isEmpty()) {
        f.notFound = true;
        f.error = "The Cover Art Archive has pictures for this release, but no front cover";
        return f;
    }
    QString link = chosen.value("image").toString();
    if (size > 0) {
        const QJsonObject thumbs = chosen.value("thumbnails").toObject();
        const QString legacy = size == 250 ? "small" : size == 500 ? "large" : QString();
        QString thumb = thumbs.value(QString::number(size)).toString();
        if (thumb.isEmpty() && !legacy.isEmpty())
            thumb = thumbs.value(legacy).toString();
        if (!thumb.isEmpty())
            link = thumb;
    }
    if (link.isEmpty()) {
        f.error = "The Cover Art Archive's answer has no image link";
        return f;
    }
    f.url = secure(link).toString();
    const Net::Response image = http.get(Net::Service::CoverArt, QUrl(f.url));
    f.offline = f.offline || image.offline;
    if (!image.ok() || image.body.isEmpty()) {
        f.error = QString("Can't download the cover: %1").arg(image.error);
        return f;
    }
    f.bytes = image.body;
    f.mime = QMimeDatabase().mimeTypeForData(f.bytes).name();
    if (!f.mime.startsWith("image/")) {
        f.error = "The Cover Art Archive sent something that is not a picture";
        f.bytes.clear();
        return f;
    }
    f.ok = true;
    return f;
}

QJsonObject AlbumResult::toJson() const {
    QJsonObject o{{"album_id", albumId}, {"album", album},   {"artist", artist},
                  {"status", status},    {"release_id", releaseId}, {"release_group_id", releaseGroupId},
                  {"how", how}};
    if (!picture.isEmpty())
        o["picture"] = picture;
    if (!coverFile.isEmpty())
        o["cover_file"] = coverFile;
    if (!batch.isEmpty())
        o["batch"] = batch;
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

QList<qint64> albumsMissingArt(Library &library) {
    QList<qint64> out;
    const Audits::Audit *audit = Audits::audit("missing-artwork");
    if (!audit)
        return out;
    const Query::Compiled where = Audits::compile(*audit);
    QSqlQuery q(library.db());
    q.prepare(QString("SELECT DISTINCT t.album_id FROM tracks t WHERE t.album_id IS NOT NULL AND (%1) ORDER BY t.album_id")
                  .arg(where.where));
    for (const QVariant &p : where.params)
        q.addBindValue(p);
    if (q.exec())
        while (q.next())
            out << q.value(0).toLongLong();
    return out;
}

AlbumResult fetchForAlbum(Library &library, Net::HttpClient &http, qint64 albumId, const AlbumOptions &options,
                          Job *job) {
    AlbumResult result;
    result.albumId = albumId;
    QSqlQuery a(library.db());
    a.prepare("SELECT title, album_artist, mb_release_id FROM albums WHERE id = ?");
    a.addBindValue(albumId);
    if (!a.exec() || !a.next()) {
        result.status = "failed";
        result.error = "No such album";
        return result;
    }
    result.album = a.value(0).toString();
    result.artist = a.value(1).toString();
    QString releaseId = a.value(2).toString();
    a.finish();

    // The tracks' own IDs, most common first; their artist when the album
    // has no album artist; and their folders, for cover.jpg.
    QMap<QString, int> releases, groups, artists;
    QList<qint64> trackIds;
    QStringList folders;
    QSqlQuery t(library.db());
    t.prepare("SELECT id, mb_release_id, mb_release_group_id, artist FROM tracks WHERE album_id = ? "
              "ORDER BY disc_no, track_no");
    t.addBindValue(albumId);
    t.exec();
    while (t.next()) {
        trackIds << t.value(0).toLongLong();
        if (!t.value(1).toString().isEmpty())
            ++releases[t.value(1).toString()];
        if (!t.value(2).toString().isEmpty())
            ++groups[t.value(2).toString()];
        if (!t.value(3).toString().isEmpty())
            ++artists[t.value(3).toString().section(';', 0, 0).trimmed()];
    }
    t.finish();
    auto mostCommon = [](const QMap<QString, int> &m) {
        QString best;
        int n = 0;
        for (auto it = m.constBegin(); it != m.constEnd(); ++it)
            if (it.value() > n)
                best = it.key(), n = it.value();
        return best;
    };
    if (releaseId.isEmpty())
        releaseId = mostCommon(releases);
    QString groupId = mostCommon(groups);
    if (result.artist.isEmpty())
        result.artist = mostCommon(artists);
    for (qint64 id : std::as_const(trackIds)) {
        const QString path = library.absolutePath(id);
        if (!path.isEmpty() && !folders.contains(QFileInfo(path).absolutePath()))
            folders << QFileInfo(path).absolutePath();
    }

    const int size = options.size > 0 ? options.size : library.setting("coverart.size", 500).toInt();
    QList<QPair<QString, QString>> tries; // release, group
    if (!releaseId.isEmpty() || !groupId.isEmpty()) {
        tries.append({releaseId, groupId});
        result.how = releaseId.isEmpty() ? "release-group" : "mbid";
    } else if (options.search) {
        result.how = "search";
        MusicBrainz::Client mb(http);
        QString error;
        const QList<MusicBrainz::Release> found = mb.searchReleases(result.artist, result.album, 0, 10, &error);
        if (found.isEmpty() && mb.lastWasOffline()) {
            result.status = "offline";
            result.error = error;
            return result;
        }
        for (const MusicBrainz::Release &r : found) {
            const double title = TextMatch::similarity(r.title, result.album);
            const double artist = TextMatch::similarity(MusicBrainz::creditText(r.credits), result.artist);
            if (r.searchScore >= 80 && title >= 0.85 && (result.artist.isEmpty() || artist >= 0.7))
                tries.append({r.id, r.releaseGroupId});
            if (tries.size() >= 3)
                break;
        }
    }
    if (tries.isEmpty()) {
        result.status = "no-match";
        result.error = result.album.isEmpty() ? "The album has no title to search by"
                                              : "No MusicBrainz release matches this album closely enough";
        return result;
    }

    Fetched fetched;
    for (const auto &attempt : std::as_const(tries)) {
        if (job && job->isCancelled())
            break;
        fetched = front(http, attempt.first, size, attempt.second);
        if (fetched.ok || fetched.offline)
            break;
    }
    result.releaseId = fetched.releaseId.isEmpty() ? tries.first().first : fetched.releaseId;
    result.releaseGroupId = fetched.releaseGroupId.isEmpty() ? tries.first().second : fetched.releaseGroupId;
    if (!fetched.ok) {
        result.status = fetched.offline ? "offline" : fetched.notFound ? "not-found" : "failed";
        result.error = fetched.error;
        return result;
    }
    if (options.dryRun) {
        result.status = "would-fetch";
        return result;
    }

    // The original goes beside the artwork cache, so the cache can remake
    // its sizes from it later.
    const QString cacheDir = options.artCacheDir.isEmpty() ? ArtworkCache::defaultCacheDir() : options.artCacheDir;
    const QString fetchedDir = cacheDir + "/fetched";
    QDir().mkpath(fetchedDir);
    const QString id = fetched.releaseId.isEmpty() ? "rg-" + fetched.releaseGroupId : fetched.releaseId;
    result.picture = QString("%1/%2-%3.%4").arg(fetchedDir, id).arg(CoverArt::snapSize(size)).arg(extensionFor(fetched.mime));
    {
        QSaveFile out(result.picture);
        if (!out.open(QIODevice::WriteOnly) || out.write(fetched.bytes) != fetched.bytes.size() || !out.commit()) {
            result.status = "failed";
            result.error = QString("Can't save %1").arg(result.picture);
            return result;
        }
    }
    ArtworkCache cache(library, cacheDir);
    if (!cache.setAlbumPicture(albumId, result.picture)) {
        result.status = "failed";
        result.error = cache.lastError();
        return result;
    }
    result.status = "fetched";

    if (options.saveFile && folders.size() == 1 && !hasPictureFile(folders.first())) {
        const QString target = folders.first() + "/cover." + extensionFor(fetched.mime);
        if (Journal::insideSandbox(options.sandbox, target)) {
            QSaveFile out(target);
            if (out.open(QIODevice::WriteOnly) && out.write(fetched.bytes) == fetched.bytes.size() && out.commit())
                result.coverFile = target;
        }
    }
    if (options.embed) {
        QList<TagEdit> edits;
        for (qint64 trackId : std::as_const(trackIds))
            edits << TagEdit{trackId, "artwork", {}, fetched.bytes, fetched.mime};
        TagWriter writer(library, job);
        writer.setSandbox(options.sandbox);
        TagWriteOptions o;
        o.source = "fetch-art";
        const TagBatchResult r = writer.apply(edits, o);
        result.batch = r.batch;
        if (!r.ok) {
            result.error = r.error.isEmpty() ? QString("%1 of %2 files could not be written").arg(r.failed).arg(r.tracks)
                                             : r.error;
        } else {
            result.status = "embedded";
        }
    }
    return result;
}

}
