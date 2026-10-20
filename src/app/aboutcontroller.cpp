#include "aboutcontroller.h"

#include "about.h"
#include "http.h"
#include "library.h"
#include "librarycontroller.h"

#include <QFileInfo>
#include <QPointer>
#include <QSqlQuery>
#include <QThread>
#include <QUrl>

namespace {

QVariantMap summaryMap(const About::Summary &s) {
    QVariantMap m = s.toJson().toVariantMap();
    m["found"] = s.found();
    m["imageFile"] = s.imageFile.isEmpty() ? QString() : QUrl::fromLocalFile(s.imageFile).toString();
    return m;
}

QVariantMap credit(const QString &role, const QString &name) { return {{"role", role}, {"name", name}}; }

// What the library knows of a track, read on the worker's own connection.
QVariantMap trackFacts(Library &library, qint64 trackId) {
    QVariantMap m;
    QSqlQuery q(library.db());
    q.prepare("SELECT t.artist, t.album_artist, t.album, t.title, t.composer, t.publisher, t.year, t.isrc, "
              "t.comment, t.mb_artist_id, t.mb_release_id, t.mb_track_id, t.rel_path, v.root_path, v.online, v.kind "
              "FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.id = ?");
    q.addBindValue(trackId);
    if (!q.exec() || !q.next())
        return m;
    m["artist"] = q.value(1).toString().isEmpty() ? q.value(0).toString() : q.value(1).toString();
    m["trackArtist"] = q.value(0).toString();
    m["album"] = q.value(2).toString();
    m["title"] = q.value(3).toString();
    m["composer"] = q.value(4).toString();
    m["publisher"] = q.value(5).toString();
    m["year"] = q.value(6).toInt();
    m["isrc"] = q.value(7).toString();
    m["comment"] = q.value(8).toString();
    m["mbArtistId"] = q.value(9).toString();
    m["mbReleaseId"] = q.value(10).toString();
    m["mbTrackId"] = q.value(11).toString();
    const QString root = q.value(13).toString();
    // The album's folder, when it can be read now (a volume marked offline
    // may be back already; a Navidrome track has none).
    const QString folder = QFileInfo((root == "/" ? QString() : root) + '/' + q.value(12).toString()).path();
    if (q.value(15).toString() != QLatin1String("subsonic") && QFileInfo(folder).isDir())
        m["folder"] = folder;
    return m;
}

} // namespace

AboutController::AboutController(LibraryController *library)
    : QObject(library), m_library(library), m_generation(std::make_shared<std::atomic_int>(0)) {
    m_pool.setMaxThreadCount(1);
    m_pool.setThreadPriority(QThread::LowPriority);
    m_pool.setExpiryTimeout(-1);
    // Arrowing down a list shows each track for a moment: look up the one
    // the cursor rests on.
    m_delay.setSingleShot(true);
    m_delay.setInterval(350);
    connect(&m_delay, &QTimer::timeout, this, &AboutController::start);
}

AboutController::~AboutController() {
    ++*m_generation;
    m_pool.waitForDone();
}

void AboutController::show(const QVariantMap &subject) {
    const qint64 trackId = subject.value("trackId").toLongLong();
    const QString key = trackId > 0 ? QString("track:%1").arg(trackId)
                                    : QString("names:%1|%2").arg(subject.value("artist").toString(),
                                                                  subject.value("album").toString());
    const bool empty = trackId <= 0 && subject.value("artist").toString().isEmpty() &&
                       subject.value("album").toString().isEmpty();
    if (empty) {
        clear();
        return;
    }
    const bool online = subject.value("online", true).toBool();
    if (key == m_subject && !subject.value("refresh").toBool() && online == m_wanted.value("online", true).toBool())
        return;
    m_wanted = subject;
    if (key != m_subject) {
        m_subject = key;
        m_artist.clear();
        m_album.clear();
        m_notes.clear();
        m_credits.clear();
        m_artistName = subject.value("artist").toString();
        m_albumName = subject.value("album").toString();
    }
    m_artistState = m_albumState = "loading";
    emit changed();
    m_delay.start();
}

void AboutController::retry() {
    if (m_subject.isEmpty())
        return;
    m_wanted["refresh"] = true;
    m_artistState = m_albumState = "loading";
    emit changed();
    start();
}

void AboutController::clear() {
    ++*m_generation;
    m_delay.stop();
    m_wanted.clear();
    if (m_subject.isEmpty())
        return;
    m_subject.clear();
    m_artist.clear();
    m_album.clear();
    m_notes.clear();
    m_credits.clear();
    m_artistName.clear();
    m_albumName.clear();
    m_artistState = m_albumState = "idle";
    emit changed();
}

void AboutController::start() {
    const int generation = ++*m_generation;
    const QVariantMap wanted = m_wanted;
    m_wanted.remove("refresh");
    const qint64 trackId = wanted.value("trackId").toLongLong();
    const QString dbPath = trackId > 0 && m_library->activate() ? m_library->databasePath()
                           : m_library->hasDatabase()            ? m_library->databasePath()
                                                                 : QString();
    const QString cacheDir = m_cacheDir;
    auto counter = m_generation;
    QPointer<AboutController> self(this);
    m_pool.start([=] {
        auto current = [&] { return counter->load() == generation; };
        if (!current())
            return;
        std::unique_ptr<Library> library;
        if (!dbPath.isEmpty()) {
            library = std::make_unique<Library>(dbPath);
            if (!library->open())
                library.reset();
        }
        // The local part: tags and the album's folder.
        QVariantMap facts = trackId > 0 && library ? trackFacts(*library, trackId) : QVariantMap();
        for (const QString &k : {"artist", "album", "title"})
            if (facts.value(k).toString().isEmpty())
                facts[k] = wanted.value(k);
        QVariantList notes, credits;
        if (!facts.value("folder").toString().isEmpty())
            for (const About::Note &n : About::packagedNotes(facts.value("folder").toString()))
                notes << QVariantMap{{"name", n.name}, {"text", n.text}, {"path", n.path}};
        const QString comment = facts.value("comment").toString().trimmed();
        if (!comment.isEmpty())
            notes << QVariantMap{{"name", QObject::tr("Comment tag")}, {"text", comment}, {"path", QString()}};
        if (!facts.value("composer").toString().isEmpty())
            credits << credit(QObject::tr("Composer"), facts.value("composer").toString());
        if (!facts.value("publisher").toString().isEmpty())
            credits << credit(QObject::tr("Label"), facts.value("publisher").toString());
        if (facts.value("year").toInt() > 0)
            credits << credit(QObject::tr("Year"), QString::number(facts.value("year").toInt()));
        if (!facts.value("isrc").toString().isEmpty())
            credits << credit(QStringLiteral("ISRC"), facts.value("isrc").toString());
        for (const QString &k : {"composer", "label"})
            if (!wanted.value(k).toString().isEmpty() && facts.value(k == QLatin1String("label") ? "publisher" : k).toString().isEmpty())
                credits << credit(k == QLatin1String("label") ? QObject::tr("Label") : QObject::tr("Composer"),
                                  wanted.value(k).toString());
        QVariantMap local{{"notes", notes}, {"credits", credits}, {"artist", facts.value("artist")},
                          {"album", facts.value("album")}};
        QMetaObject::invokeMethod(self, [self, generation, local] {
            if (self) self->finishLocal(generation, local);
        }, Qt::QueuedConnection);

        // The web part.
        QVariantMap web;
        if (!wanted.value("online", true).toBool()) {
            web["state"] = "off";
        } else if (Net::Config::forcedOffline(library.get())) {
            web["state"] = "offline";
        } else {
            Net::HttpClient http(library.get());
            if (wanted.value("refresh").toBool())
                http.setCacheEnabled(false);
            About::Client client(http, cacheDir);
            const QString lang = About::wikiLanguage(wanted.value("language").toString());
            const QString artist = facts.value("artist").toString();
            const QString album = facts.value("album").toString();
            const About::Summary a = artist.isEmpty() || !current()
                                         ? About::Summary()
                                         : client.artist(artist, facts.value("mbArtistId").toString(), lang);
            const About::Summary b = album.isEmpty() || !current()
                                         ? About::Summary()
                                         : client.album(artist, album, facts.value("mbReleaseId").toString(), lang);
            QVariantList more;
            if (current())
                for (const About::Credit &c :
                     client.credits(facts.value("mbTrackId").toString(), facts.value("mbReleaseId").toString()))
                    more << credit(c.role, c.name);
            web["artist"] = summaryMap(a);
            web["album"] = summaryMap(b);
            web["credits"] = more;
            web["state"] = client.offline() ? "offline" : "done";
        }
        QMetaObject::invokeMethod(self, [self, generation, web] {
            if (self) self->finishWeb(generation, web);
        }, Qt::QueuedConnection);
    });
}

void AboutController::finishLocal(int generation, const QVariantMap &local) {
    if (generation != m_generation->load())
        return;
    m_notes = local.value("notes").toList();
    m_credits = local.value("credits").toList();
    if (!local.value("artist").toString().isEmpty())
        m_artistName = local.value("artist").toString();
    if (!local.value("album").toString().isEmpty())
        m_albumName = local.value("album").toString();
    emit changed();
}

void AboutController::finishWeb(int generation, const QVariantMap &web) {
    if (generation != m_generation->load())
        return;
    const QString state = web.value("state").toString();
    if (state == "off" || state == "offline") {
        m_artistState = m_albumState = state;
    } else {
        m_artist = web.value("artist").toMap();
        m_album = web.value("album").toMap();
        m_artistState = m_artist.value("found").toBool() ? "found" : "none";
        m_albumState = m_album.value("found").toBool() ? "found" : "none";
        // MusicBrainz's credits after the tags', without repeating one.
        for (const QVariant &v : web.value("credits").toList()) {
            const QVariantMap c = v.toMap();
            bool seen = false;
            for (const QVariant &have : std::as_const(m_credits))
                seen = seen || (have.toMap().value("role") == c.value("role") &&
                                have.toMap().value("name").toString().compare(c.value("name").toString(), Qt::CaseInsensitive) == 0);
            if (!seen)
                m_credits << c;
        }
    }
    emit changed();
    emit looked(m_subject);
}
