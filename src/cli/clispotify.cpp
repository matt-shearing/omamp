// `thrum spotify`: Spotify through the configured backend, from a shell or
// an agent. The same rules as every command: --json, results on stdout,
// errors on stderr, exit 0 or 1. Thrum never signs in itself; `thrum
// spotify status` says what to do when the backend isn't signed in.
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "cli.h"
#include "spotify.h"
#include "spotifysession.h"

namespace Cli {

namespace {

QString toJson(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)); }

QString length(qint64 ms) {
    const qint64 s = ms / 1000;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

void printItems(QTextStream &out, const QList<Spotify::Item> &items) {
    for (const Spotify::Item &i : items) {
        out << i.uri() << '\t' << i.name;
        if (!i.artists.isEmpty())
            out << " — " << i.artistText();
        if (i.kind == "track" && !i.album.isEmpty())
            out << " (" << i.album << ')';
        if (i.kind == "playlist" && !i.owner.isEmpty())
            out << " by " << i.owner;
        if (i.durationMs > 0)
            out << '\t' << length(i.durationMs);
        out << '\n';
    }
}

QJsonArray itemsJson(const QList<Spotify::Item> &items) {
    QJsonArray a;
    for (const Spotify::Item &i : items)
        a << i.toJson();
    return a;
}

} // namespace

const char *spotifyUsage = R"(       thrum spotify status              the backend: installed, signed in, running, device
       thrum spotify search <text>       search Spotify (tracks, albums, artists, playlists)
       thrum spotify playlists|liked|albums|artists
                                         your playlists, Liked Songs, saved albums, followed artists
       thrum spotify tracks <uri>        a playlist's or album's tracks, an artist's top tracks
       thrum spotify play <uri or link>  play it on the backend's Spotify Connect device
       thrum spotify now                 what the backend is playing
       thrum spotify pause|resume
                    [--backend spotify_player|spotifast]  (default: Settings → Spotify)
)";

int runSpotify(const QStringList &arguments, QTextStream &out, QTextStream &err) {
    bool json = false;
    QString backendId;
    QStringList rest;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &a = arguments[i];
        if (a == "--json")
            json = true;
        else if (a == "--backend" && i + 1 < arguments.size())
            backendId = arguments[++i];
        else
            rest << a;
    }
    auto fail = [&](const QString &message) {
        err << "thrum: " << message << Qt::endl;
        if (json)
            out << toJson({{"ok", false}, {"error", message}}) << Qt::flush;
        return 1;
    };
    if (backendId.isEmpty())
        backendId = Spotify::Settings::load().backend;
    if (!Spotify::Backend::ids().contains(backendId))
        return fail(QString("unknown backend %1 (spotify_player or spotifast)").arg(backendId));
    const auto backend = Spotify::Backend::create(backendId);
    // Spotify's own playlists and artists' top tracks through the session
    // reader, as the window does, when it is installed and switched on.
    const QString reader = Spotify::SessionReader::findProgram();
    std::shared_ptr<SpotifyAudio> session;
    if (backendId == "spotify_player" && Spotify::Settings::load().sessionReads && !reader.isEmpty()) {
        session = std::make_shared<SpotifyAudio>(reader, Spotify::SessionReader::credentialsPath());
        backend->setCatalogue(std::make_unique<Spotify::SessionCatalogue>(
            backend.get(), std::make_shared<Spotify::SessionReader>(session)));
    }
    const QString verb = rest.value(0, "status");
    const QString argument = rest.mid(1).join(' ');
    QString error;

    if (verb == "status") {
        QJsonObject o{{"ok", true},
                      {"backend", backend->id()},
                      {"installed", backend->installed()},
                      {"signed_in", backend->signedIn()},
                      {"running", backend->running()},
                      {"device_name", backend->deviceName()}};
        QString premium = "unknown", account;
        QJsonArray devices;
        if (backend->running()) {
            for (const Spotify::Device &d : backend->devices(&error)) {
                devices << d.toJson();
                if (d.name == backend->deviceName())
                    premium = "yes";
            }
            account = backend->account(&error);
        }
        o.insert("devices", devices);
        o.insert("premium", premium);
        o.insert("account", account);
        if (!error.isEmpty())
            o.insert("error", error);
        QString next;
        if (!backend->installed())
            next = backendId == "spotifast" ? "install it: omarchy-pkg-aur-add spotifast-bin"
                                            : "install it: omarchy-pkg-add spotify-player";
        else if (!backend->signedIn() && !backend->signInCommand().isEmpty())
            next = "sign in, in a terminal: " + backend->signInCommand().join(' ');
        else if (!backend->running())
            next = backendId == "spotifast" ? "open Spotifast" : "start it: spotify_player --daemon";
        if (!next.isEmpty())
            o.insert("next_step", next);
        if (json) {
            out << toJson(o) << Qt::flush;
        } else {
            out << "Backend:    " << backend->name() << (backend->installed() ? "" : " (not installed)") << '\n';
            out << "Signed in:  " << (backend->signedIn() ? "yes" : "no") << '\n';
            out << "Running:    " << (backend->running() ? "yes" : "no") << '\n';
            if (!account.isEmpty())
                out << "Account:    " << account << '\n';
            out << "Premium:    " << premium << '\n';
            out << "Device:     " << backend->deviceName() << '\n';
            if (!next.isEmpty())
                out << "Next:       " << next << '\n';
            if (!error.isEmpty())
                out << "Error:      " << error << '\n';
            out << Qt::flush;
        }
        return 0;
    }
    if (verb == "search") {
        if (argument.trimmed().isEmpty())
            return fail("search needs some text");
        const Spotify::SearchResults r = backend->search(argument, &error);
        if (!error.isEmpty())
            return fail(error);
        if (json) {
            QJsonObject o = r.toJson();
            o.insert("ok", true);
            out << toJson(o) << Qt::flush;
        } else {
            printItems(out, r.tracks + r.albums + r.artists + r.playlists);
            out << Qt::flush;
        }
        return 0;
    }
    if (verb == "playlists" || verb == "liked" || verb == "albums" || verb == "artists" || verb == "tracks") {
        QList<Spotify::Item> items;
        if (verb == "tracks") {
            Spotify::Item container;
            if (!Spotify::parseReference(argument, &container.kind, &container.id))
                return fail("tracks needs a Spotify playlist, album or artist URI or link");
            // Artists and playlists through the catalogue, albums directly.
            if (container.kind == "artist") {
                for (const Spotify::Item &i : backend->catalogue()->artistPage(container, nullptr, &error))
                    if (i.kind == "track")
                        items << i;
            } else if (container.kind == "playlist") {
                items = backend->catalogue()->playlistTracks(container, &error);
            } else {
                items = backend->tracksOf(container, &error);
            }
        } else {
            items = backend->library(verb, &error);
        }
        if (!error.isEmpty())
            return fail(error);
        if (json)
            out << toJson({{"ok", true}, {"items", itemsJson(items)}}) << Qt::flush;
        else
            printItems(out, items), out << Qt::flush;
        return 0;
    }
    if (verb == "play") {
        Spotify::Item item;
        if (!Spotify::parseReference(argument, &item.kind, &item.id, "track"))
            return fail("play needs a Spotify URI or open.spotify.com link");
        if (!backend->playItem(item, &error))
            return fail(error);
        if (json)
            out << toJson({{"ok", true}, {"playing", item.uri()}}) << Qt::flush;
        else
            out << "Playing " << item.uri() << Qt::endl;
        return 0;
    }
    if (verb == "now") {
        const Spotify::Playback p = backend->playback(&error);
        if (!error.isEmpty())
            return fail(error);
        if (json) {
            QJsonObject o = p.toJson();
            o.insert("ok", true);
            out << toJson(o) << Qt::flush;
        } else if (!p.valid) {
            out << "Nothing is playing." << Qt::endl;
        } else {
            out << (p.playing ? "Playing: " : "Paused:  ") << p.track.name << " — " << p.track.artistText() << " ("
                << length(p.progressMs) << " / " << length(p.track.durationMs) << ") on " << p.deviceName
                << Qt::endl;
        }
        return 0;
    }
    if (verb == "pause" || verb == "resume") {
        const bool ok = verb == "pause" ? backend->pause(&error) : backend->resume(&error);
        if (!ok)
            return fail(error);
        if (json)
            out << toJson({{"ok", true}}) << Qt::flush;
        return 0;
    }
    return fail(QString("unknown spotify command %1; try thrum help").arg(verb));
}

} // namespace Cli

namespace Cli {

QList<SpotifyTrackInfo> lookUpSpotifyTracks(const QStringList &paths, QString *note) {
    QList<SpotifyTrackInfo> out;
    for (const QString &path : paths) {
        SpotifyTrackInfo t;
        t.path = path;
        out << t;
    }
    const Spotify::Settings settings = Spotify::Settings::load();
    if (!settings.enabled || settings.backend != "spotify_player") {
        if (note)
            *note = "Spotify is off in Thrum, so the tracks were added without their names; "
                    "they show their Spotify ids until they are added again with Spotify on";
        return out;
    }
    Spotify::SpotifyPlayerBackend backend;
    if (!backend.installed() || !backend.signedIn() || !backend.running()) {
        if (note)
            *note = "spotify_player isn't running, so the tracks were added without their names";
        return out;
    }
    int missing = 0;
    for (SpotifyTrackInfo &t : out) {
        QString kind, id, error;
        if (!Spotify::parseReference(t.path, &kind, &id))
            continue;
        const Spotify::Item item = backend.track(id, &error);
        if (!item.isValid() || item.name.isEmpty()) {
            ++missing;
            continue;
        }
        t.title = item.name;
        t.artist = item.artistText();
        t.album = item.album;
        t.albumId = item.albumId;
        t.durationMs = item.durationMs;
    }
    if (missing && note)
        *note = QString("Spotify didn't say the names of %1 of the tracks").arg(missing);
    return out;
}

}
