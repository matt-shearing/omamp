#include "djcontroller.h"

#include "librarycontroller.h"
#include "library.h"
#include "player.h"
#include "playlists.h"
#include "stats.h"

#include <QFileInfo>
#include <QSqlQuery>

DjController::DjController(LibraryController *library) : QObject(library), m_controller(library) {
    // Track changes and list edits arrive in bursts (load, requeue,
    // metadata); one refill after they settle is enough.
    m_refillTimer.setSingleShot(true);
    m_refillTimer.setInterval(0);
    connect(&m_refillTimer, &QTimer::timeout, this, &DjController::refill);
}

void DjController::attach() {
    Library *lib = m_controller->library();
    if (!lib)
        return;
    m_settings = AutoDj::Settings::load(*lib);
    m_party = PartyMode::Settings::load(*lib);
    m_display = lib->setting("party_display", "0").toString() == "1";
    m_dj.reset();
    if (Player *player = m_controller->player()) {
        disconnect(player, nullptr, this, nullptr);
        disconnect(player->playlist(), nullptr, this, nullptr);
        connect(player, &Player::trackChanged, this, &DjController::scheduleRefill);
        connect(player->playlist(), &Playlist::countChanged, this, &DjController::scheduleRefill);
    }
    const bool wasOn = m_enabled;
    m_enabled = lib->setting("autodj_enabled", "0").toString() == "1";
    if (m_enabled != wasOn)
        emit enabledChanged();
    emit settingsChanged();
    emit partyChanged();
    if (m_enabled)
        scheduleRefill();
}

void DjController::loadSaved(const QString &databasePath) {
    // Party mode must hold across a restart even if the library window
    // never opens, so read it straight away when there is a library.
    if (!QFileInfo::exists(databasePath))
        return;
    Library lib(databasePath);
    if (!lib.open())
        return;
    m_party = PartyMode::Settings::load(lib);
    m_display = lib.setting("party_display", "0").toString() == "1";
    emit partyChanged();
}

void DjController::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

void DjController::scheduleRefill() {
    // Even while refilling: appending changes the track, and the timer
    // runs once the refill is done.
    if (m_enabled)
        m_refillTimer.start();
}

void DjController::setEnabled(bool on) {
    if (on == m_enabled)
        return;
    if (on && !m_controller->activate())
        return;
    m_enabled = on;
    if (Library *lib = m_controller->library())
        lib->setSetting("autodj_enabled", on ? "1" : "0");
    if (!on)
        setError({});
    emit enabledChanged();
    emit m_controller->toast(on ? tr("Auto-DJ on: keeping %n tracks queued", "", m_settings.upcoming)
                                : tr("Auto-DJ off"));
    if (on)
        refill();
}

QVariantMap DjController::settings() const {
    return {{"source", m_settings.source.toString()},
            {"upcoming", m_settings.upcoming},
            {"artistSeparation", m_settings.artistSeparation},
            {"skipPlayedWithinDays", m_settings.skipPlayedWithinDays},
            {"weightByRating", m_settings.weightByRating},
            {"neverPlayedBoost", m_settings.neverPlayedBoost}};
}

QString DjController::sourceLabel() const {
    const AutoDj::Source &s = m_settings.source;
    switch (s.kind) {
    case AutoDj::Source::Library: return tr("the whole library");
    case AutoDj::Source::Collection: {
        if (Library *lib = m_controller->library()) {
            Collections::Collection c;
            if (Collections::get(*lib, Collections::resolve(*lib, s.ref), &c))
                return tr("collection %1").arg(c.name);
        }
        return tr("collection %1").arg(s.ref);
    }
    case AutoDj::Source::Playlist: {
        if (Library *lib = m_controller->library()) {
            Playlists::Playlist p;
            if (Playlists::get(*lib, Playlists::resolve(*lib, s.ref), &p))
                return tr("playlist %1").arg(p.name);
        }
        return tr("playlist %1").arg(s.ref);
    }
    case AutoDj::Source::Query: return tr("query %1").arg(s.ref);
    }
    return {};
}

void DjController::saveSettings(const QVariantMap &map) {
    if (!m_controller->activate())
        return;
    if (map.contains("source"))
        m_settings.source = AutoDj::Source::parse(map.value("source").toString());
    if (map.contains("upcoming"))
        m_settings.upcoming = qBound(1, map.value("upcoming").toInt(), 500);
    if (map.contains("artistSeparation"))
        m_settings.artistSeparation = qBound(0, map.value("artistSeparation").toInt(), 500);
    if (map.contains("skipPlayedWithinDays"))
        m_settings.skipPlayedWithinDays = qMax(0, map.value("skipPlayedWithinDays").toInt());
    if (map.contains("weightByRating"))
        m_settings.weightByRating = map.value("weightByRating").toBool();
    if (map.contains("neverPlayedBoost"))
        m_settings.neverPlayedBoost = qMax(0.0, map.value("neverPlayedBoost").toDouble());
    m_settings.save(*m_controller->library());
    // New settings, new picker.
    m_dj.reset();
    setError({});
    emit settingsChanged();
    scheduleRefill();
}

QVariantList DjController::sourceChoices() const {
    QVariantList out{QVariantMap{{"value", "library"}, {"label", tr("The whole library")}}};
    Library *lib = m_controller->library();
    if (!lib)
        return out;
    for (const Collections::Collection &c : Collections::list(*lib))
        out << QVariantMap{{"value", QString("collection:%1").arg(c.id)}, {"label", tr("Collection: %1").arg(c.name)}};
    for (const Playlists::Playlist &p : Playlists::list(*lib))
        if (p.kind != "folder")
            out << QVariantMap{{"value", QString("playlist:%1").arg(p.id)}, {"label", tr("Playlist: %1").arg(p.name)}};
    return out;
}

int DjController::refill() {
    Player *player = m_controller->player();
    Library *lib = m_controller->library();
    if (!m_enabled || !player || !lib || m_refilling)
        return 0;
    Playlist *list = player->playlist();
    const int current = player->currentIndex();
    const int queued = list->count() - current - 1;
    if (queued >= m_settings.upcoming)
        return 0;
    if (!m_dj) {
        m_dj = std::make_unique<AutoDj::Dj>(*lib, m_settings);
        m_dj->reload();
    }
    // What was played lately and is queued, oldest first; only its tail
    // matters to the artist and repeat rules.
    QList<qint64> history;
    for (int row = qMax(0, current - 50); row < list->count(); ++row)
        if (const qint64 id = Stats::trackIdForPath(*lib, list->at(row).path))
            history << id;
    const QList<AutoDj::Pick> picks = m_dj->refill(history, qMax(0, queued));
    if (picks.isEmpty()) {
        setError(m_dj->error().isEmpty() ? tr("Auto-DJ found nothing to play in %1").arg(sourceLabel())
                                         : m_dj->error());
        return 0;
    }
    setError({});
    QList<Playlist::Track> tracks;
    QSqlQuery q(lib->db());
    q.prepare("SELECT title, artist, duration_ms, file_name FROM tracks WHERE id = ?");
    for (const AutoDj::Pick &p : picks) {
        const QString path = lib->absolutePath(p.trackId);
        if (path.isEmpty())
            continue;
        Playlist::Track t;
        t.path = path;
        q.addBindValue(p.trackId);
        if (q.exec() && q.next()) {
            t.title = q.value(0).toString().isEmpty() ? q.value(3).toString() : q.value(0).toString();
            t.artist = q.value(1).toString();
            t.durationMs = q.value(2).toLongLong();
        }
        tracks << t;
    }
    m_refilling = true;
    const int added = player->appendTracks(tracks);
    m_refilling = false;
    return added;
}

bool DjController::enableParty(const QString &preset, const QString &pin, bool display) {
    if (!m_controller->activate())
        return false;
    Library *lib = m_controller->library();
    if (!PartyMode::enable(*lib, preset, pin)) {
        emit m_controller->toast(tr("Can't start party mode"));
        return false;
    }
    m_display = display;
    lib->setSetting("party_display", display ? "1" : "0");
    m_party = PartyMode::Settings::load(*lib);
    emit partyChanged();
    emit m_controller->toast(m_party.hasPin() ? tr("Party mode on. The PIN unlocks it.") : tr("Party mode on"));
    return true;
}

bool DjController::disableParty(const QString &pin) {
    Library *lib = m_controller->library();
    if (!lib && !m_controller->activate())
        return false;
    lib = m_controller->library();
    if (!PartyMode::disable(*lib, pin))
        return false;
    m_party = PartyMode::Settings::load(*lib);
    emit partyChanged();
    emit m_controller->toast(tr("Party mode off"));
    return true;
}

bool DjController::allowed(const QString &action) const {
    PartyMode::Action a;
    if (!PartyMode::parseAction(action, &a))
        return !m_party.enabled;
    return PartyMode::allowed(m_party, a);
}

void DjController::setPartyDisplay(bool on) {
    if (on == m_display)
        return;
    m_display = on;
    if (Library *lib = m_controller->library())
        lib->setSetting("party_display", on ? "1" : "0");
    emit partyChanged();
}
