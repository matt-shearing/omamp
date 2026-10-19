#include "settingscontroller.h"

#include "http.h"
#include "librarycontroller.h"
#include "library.h"
#include "scanner.h"
#include "scrobble.h"

#include <QSqlQuery>

SettingsController::SettingsController(LibraryController *library) : QObject(library), m_controller(library) {}

SettingsController::~SettingsController() {
    if (m_validateThread) {
        m_validateThread->wait();
        delete m_validateThread;
    }
}

bool SettingsController::editable(const QString &key) {
    static const QStringList exact = {"tags.write_ratings", "watch.enabled",  "watch.network_minutes",
                                      "acoustid.key",       "coverart.size",  "scrobble.enabled",
                                      "scrobble.listenbrainz", "scrobble.lastfm"};
    static const QStringList prefixes = {"identify."};
    if (exact.contains(key))
        return true;
    for (const QString &p : prefixes)
        if (key.startsWith(p))
            return true;
    return false;
}

QVariant SettingsController::value(const QString &key, const QVariant &fallback) const {
    Library *lib = m_controller->library();
    return lib ? lib->setting(key, fallback) : fallback;
}

bool SettingsController::flag(const QString &key, bool fallback) const {
    const QString v = value(key, fallback ? "1" : "0").toString().trimmed().toLower();
    return !(v == "0" || v == "false" || v == "no" || v == "off");
}

bool SettingsController::setValue(const QString &key, const QVariant &value) {
    if (!editable(key) || !m_controller->activate())
        return false;
    Library *lib = m_controller->library();
    const QString text = value.toString();
    if (lib->setting(key).toString() == text && !lib->setting(key).isNull())
        return true;
    lib->setSetting(key, text.isEmpty() ? QVariant() : QVariant(text));
    ++m_revision;
    if (key.startsWith("watch."))
        m_controller->applyWatchSettings();
    emit changed();
    return true;
}

QStringList SettingsController::folders() const {
    Library *lib = m_controller->library();
    return lib ? Scanner::roots(*lib) : QStringList();
}

int SettingsController::removeFolder(const QString &folder) {
    Library *lib = m_controller->library();
    if (!lib)
        return 0;
    QSqlQuery q(lib->db());
    q.exec("SELECT f.volume_id, v.root_path, f.rel_path FROM folders f JOIN volumes v ON v.id = f.volume_id "
           "WHERE f.is_root");
    qint64 volume = 0;
    QString rel;
    while (q.next()) {
        const QString root = q.value(1).toString();
        const QString r = q.value(2).toString();
        const QString path = r.isEmpty() ? root : (root == "/" ? "" : root) + '/' + r;
        if (path == folder) {
            volume = q.value(0).toLongLong();
            rel = r;
        }
    }
    if (!volume)
        return 0;
    QVariantList ids;
    QSqlQuery t(lib->db());
    if (rel.isEmpty()) {
        t.prepare("SELECT id FROM tracks WHERE volume_id = ?");
        t.addBindValue(volume);
    } else {
        t.prepare("SELECT id FROM tracks WHERE volume_id = ? AND rel_path >= ? AND rel_path < ?");
        t.addBindValue(volume);
        t.addBindValue(rel + '/');
        t.addBindValue(rel + '0');
    }
    if (t.exec())
        while (t.next())
            ids << t.value(0);
    QSqlQuery u(lib->db());
    u.prepare("UPDATE folders SET is_root = 0 WHERE volume_id = ? AND rel_path = ?");
    u.addBindValue(volume);
    u.addBindValue(rel);
    u.exec();
    const int removed = ids.isEmpty() ? 0 : m_controller->removeFromLibrary(ids);
    if (ids.isEmpty())
        m_controller->refreshAll();
    m_controller->applyWatchSettings();
    ++m_revision;
    emit changed();
    return removed;
}

QVariantList SettingsController::scrobbleStatus() const {
    QVariantList out;
    Library *lib = m_controller->library();
    if (!lib)
        return out;
    Scrobble::Scrobbler scrobbler(*lib);
    for (const Scrobble::ServiceStatus &s : scrobbler.status())
        out << s.toJson().toVariantMap();
    return out;
}

void SettingsController::validateListenBrainz(const QString &token) {
    if (m_validateThread || !m_controller->activate())
        return;
    const QString path = m_controller->databasePath();
    const QString t = token.trimmed();
    m_validateThread = QThread::create([this, path, t] {
        Library library(path);
        QString user, error;
        bool ok = false;
        if (library.open()) {
            Net::HttpClient http(&library);
            ok = Scrobble::ListenBrainz::validateToken(http, t, &user, &error);
        } else {
            error = tr("Can't open the library");
        }
        QMetaObject::invokeMethod(
            this,
            [this, ok, user, error, t] {
                m_validateThread->wait();
                delete m_validateThread;
                m_validateThread = nullptr;
                emit validatingChanged();
                if (ok && m_controller->library()) {
                    m_controller->library()->setSetting("listenbrainz.token", t);
                    m_controller->library()->setSetting("listenbrainz.user", user);
                    ++m_revision;
                    emit changed();
                }
                emit validated(ok, user, error);
            },
            Qt::QueuedConnection);
    });
    m_validateThread->start();
    emit validatingChanged();
}

void SettingsController::forgetListenBrainz() {
    Library *lib = m_controller->library();
    if (!lib)
        return;
    lib->setSetting("listenbrainz.token", QVariant());
    lib->setSetting("listenbrainz.user", QVariant());
    ++m_revision;
    emit changed();
}
