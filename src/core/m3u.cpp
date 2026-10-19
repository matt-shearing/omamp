#include "m3u.h"

#include "library.h"
#include "playlists.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QUrl>

namespace PlaylistFiles {

namespace {

QString decode(const QByteArray &bytes, bool forceUtf8) {
    QByteArray data = bytes;
    if (data.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(data.mid(3));
    if (forceUtf8)
        return QString::fromUtf8(data);
    // Old .m3u files are often Latin-1 (Winamp, MediaMonkey 3). Use UTF-8
    // only when the bytes really are UTF-8.
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = utf8.decode(data);
    if (!utf8.hasError())
        return text;
    return QString::fromLatin1(data);
}

bool isWindowsPath(const QString &s) {
    static const QRegularExpression drive("^[A-Za-z]:[\\\\/]");
    return drive.match(s).hasMatch() || s.startsWith("\\\\");
}

bool isUrl(const QString &s) {
    static const QRegularExpression scheme("^[A-Za-z][A-Za-z0-9+.-]*://");
    return scheme.match(s).hasMatch() && !s.startsWith("file://", Qt::CaseInsensitive);
}

QString resolveLocation(const QString &location, const QDir &base) {
    QString loc = location.trimmed();
    if (loc.startsWith("file://", Qt::CaseInsensitive))
        return QDir::cleanPath(QUrl(loc).toLocalFile());
    if (isUrl(loc) || isWindowsPath(loc))
        return loc;
    if (!loc.startsWith('/'))
        loc.replace('\\', '/');
    return QDir::cleanPath(base.absoluteFilePath(loc));
}

// "Artist - Title" from #EXTINF or a PLS title. A title with no " - " is
// taken as a title alone.
void splitDisplay(const QString &display, FileEntry *e) {
    const int dash = display.indexOf(" - ");
    if (dash > 0) {
        e->artist = display.left(dash).trimmed();
        e->title = display.mid(dash + 3).trimmed();
    } else {
        e->title = display.trimmed();
    }
}

QString fold(const QString &s) { return s.toCaseFolded(); }

QString tagKey(const QString &artist, const QString &title) {
    return fold(artist.simplified()) + QChar(0x1f) + fold(title.simplified());
}

QString suffixKey(const QString &path, int parts) {
    QString p = path;
    p.replace('\\', '/');
    const QStringList all = p.split('/', Qt::SkipEmptyParts);
    if (all.size() < parts)
        return {};
    return fold(all.mid(all.size() - parts).join('/'));
}

} // namespace

Parsed read(const QString &file) {
    return file.endsWith(".pls", Qt::CaseInsensitive) ? readPls(file) : readM3u(file);
}

Parsed readM3u(const QString &file) {
    Parsed out;
    QFile f(file);
    out.name = QFileInfo(file).completeBaseName();
    if (!f.open(QIODevice::ReadOnly)) {
        out.error = QString("Can't read %1: %2").arg(file, f.errorString());
        return out;
    }
    const QString text = decode(f.readAll(), file.endsWith(".m3u8", Qt::CaseInsensitive));
    const QDir base = QFileInfo(file).absoluteDir();
    FileEntry pending;
    bool havePending = false;
    for (QString line : text.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith("#EXTINF:", Qt::CaseInsensitive)) {
            // #EXTINF:<seconds>[ attributes],<display>
            pending = {};
            havePending = true;
            const QString rest = line.mid(8);
            const int comma = rest.indexOf(',');
            const QString head = comma >= 0 ? rest.left(comma) : rest;
            bool ok = false;
            const double seconds = head.section(' ', 0, 0).toDouble(&ok);
            if (ok && seconds >= 0)
                pending.durationMs = qint64(seconds * 1000);
            if (comma >= 0)
                splitDisplay(rest.mid(comma + 1), &pending);
            continue;
        }
        if (line.startsWith('#'))
            continue;
        FileEntry e = havePending ? pending : FileEntry();
        havePending = false;
        e.location = line;
        e.path = resolveLocation(line, base);
        out.entries << e;
    }
    return out;
}

Parsed readPls(const QString &file) {
    Parsed out;
    QFile f(file);
    out.name = QFileInfo(file).completeBaseName();
    if (!f.open(QIODevice::ReadOnly)) {
        out.error = QString("Can't read %1: %2").arg(file, f.errorString());
        return out;
    }
    const QString text = decode(f.readAll(), false);
    const QDir base = QFileInfo(file).absoluteDir();
    QMap<int, FileEntry> byNumber;
    static const QRegularExpression key("^(File|Title|Length)(\\d+)$", QRegularExpression::CaseInsensitiveOption);
    bool sawHeader = false;
    for (QString line : text.split('\n')) {
        line = line.trimmed();
        if (line.compare("[playlist]", Qt::CaseInsensitive) == 0) {
            sawHeader = true;
            continue;
        }
        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const auto m = key.match(line.left(eq).trimmed());
        if (!m.hasMatch())
            continue;
        const QString value = line.mid(eq + 1).trimmed();
        FileEntry &e = byNumber[m.captured(2).toInt()];
        const QString what = m.captured(1).toLower();
        if (what == "file") {
            e.location = value;
            e.path = resolveLocation(value, base);
        } else if (what == "title") {
            splitDisplay(value, &e);
        } else {
            bool ok = false;
            const double seconds = value.toDouble(&ok);
            if (ok && seconds >= 0)
                e.durationMs = qint64(seconds * 1000);
        }
    }
    if (!sawHeader && byNumber.isEmpty()) {
        out.error = QString("%1 is not a PLS playlist").arg(file);
        return out;
    }
    for (const FileEntry &e : byNumber)
        if (!e.location.isEmpty())
            out.entries << e;
    return out;
}

Matcher::Matcher(Library &library) {
    QSqlQuery q(library.db());
    q.setForwardOnly(true);
    q.exec("SELECT t.id, v.root_path, t.rel_path, t.artist, t.title, t.duration_ms, t.album FROM tracks t "
           "JOIN volumes v ON v.id = t.volume_id");
    while (q.next()) {
        const qint64 id = q.value(0).toLongLong();
        const QString root = q.value(1).toString();
        const QString path = root == "/" ? '/' + q.value(2).toString() : root + '/' + q.value(2).toString();
        m_paths.insert(id, path);
        m_exact.insert(path, id);
        m_folded[fold(path)] << id;
        const QString title = q.value(4).toString();
        if (!title.isEmpty())
            m_tags[tagKey(q.value(3).toString(), title)] << Row{id, q.value(5).toLongLong(), q.value(6).toString()};
    }
}

qint64 Matcher::byPath(const QString &absolutePath) const {
    const QString clean = QDir::cleanPath(absolutePath);
    if (const auto it = m_exact.constFind(clean); it != m_exact.constEnd())
        return *it;
    const QList<qint64> folded = m_folded.value(fold(clean));
    return folded.size() == 1 ? folded.first() : 0;
}

qint64 Matcher::bySuffix(const QString &anyPath, int parts) const {
    if (parts < 1)
        return 0;
    if (!m_suffixes.contains(parts)) {
        QHash<QString, QList<qint64>> &index = m_suffixes[parts];
        for (auto it = m_paths.constBegin(); it != m_paths.constEnd(); ++it) {
            const QString k = suffixKey(it.value(), parts);
            if (!k.isEmpty())
                index[k] << it.key();
        }
    }
    const QString k = suffixKey(anyPath, parts);
    if (k.isEmpty())
        return 0;
    const QList<qint64> hits = m_suffixes.value(parts).value(k);
    return hits.size() == 1 ? hits.first() : 0;
}

qint64 Matcher::byTags(const QString &artist, const QString &title, qint64 durationMs, const QString &album,
                       qint64 toleranceMs) const {
    if (title.trimmed().isEmpty())
        return 0;
    const QList<Row> rows = m_tags.value(tagKey(artist, title));
    QList<Row> fits;
    for (const Row &r : rows)
        if (durationMs <= 0 || r.durationMs <= 0 || qAbs(r.durationMs - durationMs) <= toleranceMs)
            fits << r;
    if (fits.isEmpty())
        return 0;
    if (fits.size() > 1 && !album.isEmpty()) {
        QList<Row> sameAlbum;
        for (const Row &r : fits)
            if (fold(r.album.simplified()) == fold(album.simplified()))
                sameAlbum << r;
        if (!sameAlbum.isEmpty())
            fits = sameAlbum;
    }
    std::sort(fits.begin(), fits.end(), [durationMs](const Row &a, const Row &b) {
        if (durationMs > 0) {
            const qint64 da = a.durationMs > 0 ? qAbs(a.durationMs - durationMs) : LLONG_MAX;
            const qint64 db = b.durationMs > 0 ? qAbs(b.durationMs - durationMs) : LLONG_MAX;
            if (da != db)
                return da < db;
        }
        return a.id < b.id;
    });
    return fits.first().id;
}

QJsonObject ImportResult::toJson() const {
    QJsonObject o{{"ok", ok()},
                  {"file", file},
                  {"playlist_id", playlistId},
                  {"name", name},
                  {"entries", entries},
                  {"matched_by_path", byPath},
                  {"matched_by_tags", byTags},
                  {"unmatched", unmatched},
                  {"unmatched_locations", QJsonArray::fromStringList(unmatchedLocations)}};
    if (!ok())
        o["error"] = error;
    return o;
}

ImportResult import(Library &library, const QString &file, qint64 parentId, const QString &name,
                    const Matcher *matcher) {
    ImportResult r;
    r.file = QFileInfo(file).absoluteFilePath();
    const Parsed parsed = read(file);
    if (!parsed.ok()) {
        r.error = parsed.error;
        return r;
    }
    std::unique_ptr<Matcher> own;
    if (!matcher) {
        own = std::make_unique<Matcher>(library);
        matcher = own.get();
    }
    QList<Playlists::Entry> entries;
    for (const FileEntry &f : parsed.entries) {
        Playlists::Entry e;
        e.path = f.path;
        e.title = f.title;
        e.artist = f.artist;
        e.durationMs = f.durationMs;
        if (f.path.startsWith('/'))
            e.trackId = matcher->byPath(f.path);
        if (e.trackId) {
            ++r.byPath;
        } else if ((e.trackId = matcher->byTags(f.artist, f.title, f.durationMs))) {
            ++r.byTags;
        } else {
            ++r.unmatched;
            r.unmatchedLocations << f.location;
        }
        if (e.trackId)
            e.path = matcher->pathOf(e.trackId);
        entries << e;
    }
    r.entries = entries.size();
    r.name = name.isEmpty() ? parsed.name : name;
    QSqlDatabase db = library.db();
    const bool own_tx = db.transaction();
    r.playlistId = Playlists::create(library, r.name, parentId, &r.error);
    if (r.playlistId && !entries.isEmpty() && Playlists::addEntries(library, r.playlistId, entries, -1, &r.error) < 0)
        r.playlistId = 0;
    if (r.playlistId)
        Playlists::setNote(library, r.playlistId, {}, "m3u:" + r.file);
    if (own_tx) {
        if (r.playlistId)
            db.commit();
        else
            db.rollback();
    }
    return r;
}

static bool holdsPlaylists(const QString &folder) {
    QDirIterator it(folder, {"*.m3u", "*.m3u8", "*.pls", "*.M3U", "*.M3U8", "*.PLS"}, QDir::Files,
                    QDirIterator::Subdirectories);
    return it.hasNext();
}

static void importInto(Library &library, const QString &folder, qint64 folderId, const Matcher &matcher,
                       QList<ImportResult> &results) {
    QDir dir(folder);
    QStringList files = dir.entryList({"*.m3u", "*.m3u8", "*.pls"}, QDir::Files, QDir::Name | QDir::IgnoreCase);
    // entryList's name filters ignore case on Linux only with the flag; be sure.
    files.removeDuplicates();
    for (const QString &f : files)
        results << import(library, dir.filePath(f), folderId, {}, &matcher);
    for (const QString &sub : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        const QString path = dir.filePath(sub);
        if (!holdsPlaylists(path))
            continue;
        const qint64 id = Playlists::createFolder(library, sub, folderId);
        if (id)
            importInto(library, path, id, matcher, results);
    }
}

QList<ImportResult> importFolder(Library &library, const QString &folder, qint64 parentId, const QString &name,
                                 qint64 *folderId, QString *error) {
    QList<ImportResult> results;
    if (!QFileInfo(folder).isDir()) {
        if (error)
            *error = QString("%1 is not a folder").arg(folder);
        return results;
    }
    const Matcher matcher(library);
    QSqlDatabase db = library.db();
    const bool own = db.transaction();
    const qint64 top = Playlists::createFolder(
        library, name.isEmpty() ? QFileInfo(QDir::cleanPath(QFileInfo(folder).absoluteFilePath())).fileName() : name,
        parentId, error);
    if (top)
        importInto(library, folder, top, matcher, results);
    if (own)
        top ? db.commit() : db.rollback();
    if (folderId)
        *folderId = top;
    return results;
}

static QString display(const Playlists::Item &it) {
    if (it.artist.isEmpty())
        return it.title.isEmpty() ? QFileInfo(it.path).completeBaseName() : it.title;
    return it.artist + " - " + it.title;
}

bool exportFile(Library &library, qint64 playlistId, const QString &file, const ExportOptions &options, int *written,
                QString *error) {
    QString why;
    const QList<Playlists::Item> items = Playlists::items(library, playlistId, &why);
    if (!why.isEmpty()) {
        if (error)
            *error = why;
        return false;
    }
    const QDir base = QFileInfo(file).absoluteDir();
    auto location = [&](const Playlists::Item &it) {
        if (!options.relative || !it.path.startsWith('/'))
            return it.path;
        return base.relativeFilePath(it.path);
    };
    QString text;
    int count = 0;
    if (file.endsWith(".pls", Qt::CaseInsensitive)) {
        text = "[playlist]\n";
        for (const Playlists::Item &it : items) {
            if (it.path.isEmpty())
                continue;
            ++count;
            text += QString("File%1=%2\nTitle%1=%3\nLength%1=%4\n")
                        .arg(count)
                        .arg(location(it), display(it))
                        .arg(it.durationMs > 0 ? it.durationMs / 1000 : -1);
        }
        text += QString("NumberOfEntries=%1\nVersion=2\n").arg(count);
    } else {
        text = "#EXTM3U\n";
        for (const Playlists::Item &it : items) {
            if (it.path.isEmpty())
                continue;
            ++count;
            text += QString("#EXTINF:%1,%2\n%3\n")
                        .arg(it.durationMs > 0 ? it.durationMs / 1000 : -1)
                        .arg(display(it), location(it));
        }
    }
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(text.toUtf8()) < 0 || !out.commit()) {
        if (error)
            *error = QString("Can't write %1: %2").arg(file, out.errorString());
        return false;
    }
    if (written)
        *written = count;
    return true;
}

}
