#include "about.h"

#include "artwork.h"
#include "musicbrainz.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringDecoder>

namespace About {

namespace {

// Higher is better: the names an album's own notes usually have.
int noteRank(const QString &fileName) {
    const QString n = fileName.toLower();
    const QString base = QFileInfo(n).completeBaseName();
    if (base == "album" || base == "about")
        return 0;
    if (base.startsWith("liner notes") || base.startsWith("liner_notes") || base.startsWith("linernotes"))
        return 1;
    if (base == "notes" || base == "info")
        return 2;
    if (base == "credits")
        return 3;
    if (base.startsWith("readme"))
        return 4;
    if (n.endsWith(".nfo"))
        return 5;
    return -1;
}

bool textSuffix(const QString &fileName) {
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    return suffix.isEmpty() || suffix == "txt" || suffix == "md" || suffix == "nfo" || suffix == "text";
}

QString decode(const QByteArray &bytes) {
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8.decode(bytes);
    if (utf8.hasError())
        text = QString::fromLatin1(bytes);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    text.replace("\r\n", "\n");
    return text.trimmed();
}

QString firstUuid(const QString &text) {
    static const QRegularExpression uuid("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    const auto m = uuid.match(text);
    return m.hasMatch() ? m.captured(0).toLower() : QString();
}

QString simplified(const QString &name) {
    QString s = name.toLower();
    s.remove(QRegularExpression("[^\\p{L}\\p{N}]"));
    return s;
}

} // namespace

QJsonObject Summary::toJson() const {
    return {{"title", title},     {"description", description}, {"extract", extract},
            {"pageUrl", pageUrl}, {"imageUrl", imageUrl},       {"imageFile", imageFile},
            {"lang", lang},       {"wikidataId", wikidataId},   {"mbid", mbid}};
}

bool isNoteFile(const QString &fileName) { return noteRank(fileName) >= 0 && textSuffix(fileName); }

QList<Note> packagedNotes(const QString &folder, int maxBytes) {
    QList<Note> notes;
    QStringList folders{folder};
    static const QRegularExpression disc("^(cd|disc|disk)\\s*\\d+$", QRegularExpression::CaseInsensitiveOption);
    if (disc.match(QFileInfo(folder).fileName()).hasMatch())
        folders << QFileInfo(folder).path();
    for (const QString &dir : folders) {
        QList<QPair<int, QString>> found;
        for (const QFileInfo &f : QDir(dir).entryInfoList(QDir::Files | QDir::Readable)) {
            if (!isNoteFile(f.fileName()) || f.size() == 0)
                continue;
            found.append({noteRank(f.fileName()), f.absoluteFilePath()});
        }
        std::sort(found.begin(), found.end());
        for (const auto &item : found) {
            QFile file(item.second);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QString text = decode(file.read(maxBytes));
            if (text.isEmpty())
                continue;
            notes.append({QFileInfo(item.second).fileName(), item.second, text});
        }
    }
    return notes;
}

QString wikiLanguage(const QString &locale) {
    QString code = locale.section('_', 0, 0).section('-', 0, 0).section('.', 0, 0).toLower();
    if (code.isEmpty() || code == "c" || code == "posix")
        return "en";
    static const QRegularExpression valid("^[a-z]{2,3}$");
    return valid.match(code).hasMatch() ? code : "en";
}

QString wikidataIdFromUrl(const QString &url) {
    static const QRegularExpression q("/(Q\\d+)$");
    const auto m = q.match(url.trimmed());
    return m.hasMatch() ? m.captured(1) : QString();
}

QString wikidataFromRelations(const QJsonArray &relations, QString *wikipediaUrl) {
    QString qid;
    for (const QJsonValue &v : relations) {
        const QJsonObject r = v.toObject();
        const QString type = r.value("type").toString();
        const QString resource = r.value("url").toObject().value("resource").toString();
        if (type == "wikidata" && qid.isEmpty())
            qid = wikidataIdFromUrl(resource);
        else if (type == "wikipedia" && wikipediaUrl && wikipediaUrl->isEmpty())
            *wikipediaUrl = resource;
    }
    return qid;
}

Client::Client(Net::HttpClient &http, const QString &cacheDir)
    : m_http(http), m_dir(cacheDir.isEmpty() ? defaultCacheDir() : cacheDir) {}

QString Client::defaultCacheDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omamp/about";
}

void Client::note(const Net::Response &r) {
    if (r.offline)
        m_offline = true;
    if (!r.ok() && !r.notFound() && m_error.isEmpty())
        m_error = r.error.isEmpty() ? QString("HTTP %1").arg(r.status) : r.error;
}

QJsonObject Client::musicBrainz(const QString &path, const QList<QPair<QString, QString>> &query) {
    QList<QPair<QString, QString>> q = query;
    q.append(QPair<QString, QString>("fmt", "json"));
    const QUrl url = Net::makeUrl(Net::baseUrl(Net::Service::MusicBrainz, m_http.library()), path, q);
    const Net::Response r = m_http.get(Net::Service::MusicBrainz, url, MusicBrainz::Client::cacheSeconds);
    note(r);
    return r.ok() ? r.json().object() : QJsonObject();
}

QString Client::findArtist(const QString &name) {
    if (name.trimmed().isEmpty())
        return {};
    const QJsonObject o = musicBrainz("/artist", {{"query", "artist:" + MusicBrainz::quoted(name)}, {"limit", "3"}});
    for (const QJsonValue &v : o.value("artists").toArray()) {
        const QJsonObject a = v.toObject();
        if (a.value("score").toInt() < 100)
            continue;
        if (simplified(a.value("name").toString()) == simplified(name))
            return a.value("id").toString();
    }
    return {};
}

QString Client::findReleaseGroup(const QString &artist, const QString &album) {
    if (album.trimmed().isEmpty())
        return {};
    QString query = "releasegroup:" + MusicBrainz::quoted(album);
    if (!artist.trimmed().isEmpty())
        query += " AND artist:" + MusicBrainz::quoted(artist);
    const QJsonObject o = musicBrainz("/release-group", {{"query", query}, {"limit", "3"}});
    for (const QJsonValue &v : o.value("release-groups").toArray()) {
        const QJsonObject g = v.toObject();
        if (g.value("score").toInt() >= 95 && simplified(g.value("title").toString()) == simplified(album))
            return g.value("id").toString();
    }
    return {};
}

QString Client::releaseGroupOf(const QString &releaseId) {
    const QJsonObject o = musicBrainz("/release/" + releaseId, {{"inc", "release-groups"}});
    return o.value("release-group").toObject().value("id").toString();
}

QJsonObject Client::withUrls(const QString &entity, const QString &mbid) {
    return musicBrainz("/" + entity + "/" + mbid, {{"inc", "url-rels"}});
}

QString Client::wikipediaTitle(const QString &qid, const QString &lang, QString *usedLang) {
    QStringList sites{lang + "wiki"};
    if (lang != "en")
        sites << "enwiki";
    const QUrl url = Net::makeUrl(Net::baseUrl(Net::Service::Wikidata, m_http.library()), "/w/api.php",
                                  {{"action", "wbgetentities"},
                                   {"ids", qid},
                                   {"props", "sitelinks"},
                                   {"sitefilter", sites.join('|')},
                                   {"format", "json"}});
    const Net::Response r = m_http.get(Net::Service::Wikidata, url, wikiCacheSeconds);
    note(r);
    if (!r.ok())
        return {};
    const QJsonObject links =
        r.json().object().value("entities").toObject().value(qid).toObject().value("sitelinks").toObject();
    for (const QString &site : sites) {
        const QString title = links.value(site).toObject().value("title").toString();
        if (!title.isEmpty()) {
            if (usedLang)
                *usedLang = site.chopped(4);
            return title;
        }
    }
    return {};
}

Summary Client::summary(const QString &lang, const QString &title) {
    Summary s;
    if (title.isEmpty())
        return s;
    QString base = Net::baseUrl(Net::Service::Wikipedia, m_http.library());
    base.replace("{lang}", lang);
    QString path = title;
    path.replace(' ', '_');
    const QUrl url(base + "/api/rest_v1/page/summary/" + QString::fromLatin1(QUrl::toPercentEncoding(path, "()_,'!")));
    const Net::Response r = m_http.get(Net::Service::Wikipedia, url, wikiCacheSeconds);
    note(r);
    if (!r.ok())
        return s;
    const QJsonObject o = r.json().object();
    if (o.value("type").toString() == "disambiguation")
        return s;
    s.title = o.value("title").toString();
    s.description = o.value("description").toString();
    s.extract = o.value("extract").toString().trimmed();
    s.pageUrl = o.value("content_urls").toObject().value("desktop").toObject().value("page").toString();
    if (s.pageUrl.isEmpty())
        s.pageUrl = QString("https://%1.wikipedia.org/wiki/%2").arg(lang, path);
    s.imageUrl = o.value("thumbnail").toObject().value("source").toString();
    s.lang = lang;
    savePicture(&s);
    return s;
}

void Client::savePicture(Summary *s) {
    if (s->imageUrl.isEmpty())
        return;
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(s->imageUrl.toUtf8(), QCryptographicHash::Sha1).toHex().left(20));
    for (const char *ext : {".jpeg", ".png", ".webp", ".gif", ".bmp"}) {
        if (QFileInfo::exists(m_dir + "/" + hash + ext)) {
            s->imageFile = m_dir + "/" + hash + ext;
            return;
        }
    }
    const Net::Response r = m_http.get(Net::Service::Wikipedia, QUrl(s->imageUrl), wikiCacheSeconds);
    // Only a picture in a format named by its first bytes is kept, under
    // that format's name, so the window never guesses (ArtworkCache::pictureFormat).
    const QByteArray kind = r.ok() ? ArtworkCache::pictureFormat(r.body) : QByteArray();
    if (kind.isEmpty())
        return;
    const QString file = m_dir + "/" + hash + "." + QString::fromLatin1(kind);
    QDir().mkpath(m_dir);
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly) && out.write(r.body) == r.body.size() && out.commit())
        s->imageFile = file;
}

Summary Client::artist(const QString &name, const QString &mbArtistId, const QString &lang) {
    QString mbid = firstUuid(mbArtistId);
    if (mbid.isEmpty())
        mbid = findArtist(name);
    if (mbid.isEmpty())
        return {};
    QString wikipedia;
    const QString qid = wikidataFromRelations(withUrls("artist", mbid).value("relations").toArray(), &wikipedia);
    Summary s;
    if (!qid.isEmpty()) {
        QString used;
        const QString title = wikipediaTitle(qid, lang, &used);
        if (!title.isEmpty())
            s = summary(used, title);
    }
    if (!s.found() && !wikipedia.isEmpty()) {
        // An old-style direct link: https://en.wikipedia.org/wiki/Title.
        const QUrl link(wikipedia);
        const QString used = link.host().section('.', 0, 0);
        s = summary(used.isEmpty() ? "en" : used, QUrl::fromPercentEncoding(link.path().section("/wiki/", 1).toUtf8()));
    }
    s.wikidataId = qid;
    s.mbid = mbid;
    return s;
}

Summary Client::album(const QString &artist, const QString &album, const QString &mbReleaseId, const QString &lang) {
    QString group;
    const QString release = firstUuid(mbReleaseId);
    if (!release.isEmpty())
        group = releaseGroupOf(release);
    if (group.isEmpty())
        group = findReleaseGroup(artist, album);
    if (group.isEmpty())
        return {};
    const QString qid = wikidataFromRelations(withUrls("release-group", group).value("relations").toArray());
    Summary s;
    if (!qid.isEmpty()) {
        QString used;
        const QString title = wikipediaTitle(qid, lang, &used);
        if (!title.isEmpty())
            s = summary(used, title);
    }
    s.wikidataId = qid;
    s.mbid = group;
    return s;
}

QList<Credit> Client::credits(const QString &recordingId, const QString &releaseId) {
    QList<Credit> out;
    const QString recording = firstUuid(recordingId);
    if (!recording.isEmpty()) {
        const QJsonObject o = musicBrainz("/recording/" + recording, {{"inc", "artist-rels+isrcs"}});
        for (const QJsonValue &v : o.value("relations").toArray()) {
            const QJsonObject r = v.toObject();
            const QString type = r.value("type").toString();
            const QString name = r.value("artist").toObject().value("name").toString();
            if (name.isEmpty())
                continue;
            if (type == "producer")
                out.append({QObject::tr("Producer"), name});
            else if (type == "engineer" || type == "recording" || type == "mix" || type == "mastering")
                out.append({QObject::tr("Engineer"), name});
        }
        QStringList isrcs;
        for (const QJsonValue &v : o.value("isrcs").toArray())
            isrcs << v.toString();
        if (!isrcs.isEmpty())
            out.append({QStringLiteral("ISRC"), isrcs.join(", ")});
    }
    const QString release = firstUuid(releaseId);
    if (!release.isEmpty()) {
        const QJsonObject o = musicBrainz("/release/" + release, {{"inc", "labels"}});
        QStringList labels;
        for (const QJsonValue &v : o.value("label-info").toArray()) {
            const QString name = v.toObject().value("label").toObject().value("name").toString();
            if (!name.isEmpty() && !labels.contains(name))
                labels << name;
        }
        if (!labels.isEmpty())
            out.append({QObject::tr("Label"), labels.join(", ")});
    }
    return out;
}

}
