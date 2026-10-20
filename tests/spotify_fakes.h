#pragma once

// The Spotify fakes the tests share: an engine that plays nothing, and a
// fake spotify_player world (tests/fake-spotify on PATH, answering from a
// copy of tests/fixtures/spotify). Nothing here contacts Spotify, starts
// the real spotify_player or plays a sound.

#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include "audioengine.h"
#include "spotify.h"

namespace {

// An engine that plays nothing: it keeps what it was given and moves on
// when the test says the file ended.
class FakeEngine : public AudioEngine {
public:
    QString name() const override { return "native"; }
    bool gapless() const override { return true; }
    bool skipsUnplayable() const override { return true; }
    void load(const QString &path, const QUrl & = {}) override {
        m_source = path;
        m_queued.clear();
        setState(Stopped);
    }
    void queueNext(const QString &path, const QUrl & = {}) override { m_queued = path; }
    QString source() const override { return m_source; }
    void play() override {
        if (!m_source.isEmpty())
            setState(Playing);
    }
    void pause() override { setState(Paused); }
    void stop() override { setState(Stopped); }
    void seek(qint64) override {}
    qint64 position() const override { return 0; }
    qint64 duration() const override { return 0; }
    State state() const override { return m_state; }
    void setVolume(qreal) override {}
    void setMuted(bool) override {}
    QVariantMap metaData() const override { return {}; }

    // The current file played to its end.
    void end() {
        if (!m_queued.isEmpty()) {
            m_source = m_queued;
            m_queued.clear();
            emit advanced();
        } else {
            setState(Stopped);
            emit endOfQueue();
        }
    }
    QString queued() const { return m_queued; }

private:
    void setState(State s) {
        if (s != m_state) {
            m_state = s;
            emit stateChanged();
        }
    }
    QString m_source, m_queued;
    State m_state = Stopped;
};

[[maybe_unused]] QByteArray pngBytes(QColor color) {
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "png");
    return bytes;
}

// The fake's world: a copy of the fixtures it answers from and changes,
// spotify_player's cache (signed in or not) and config, and the
// environment that points Omamp at them. Restored on destruction.
struct FakeSpotify {
    QTemporaryDir root;
    QHash<QByteArray, QByteArray> saved;
    QHash<QString, QString> ids;

    explicit FakeSpotify(bool signedIn = true, const QString &artBase = "http://127.0.0.1:9") {
        const QString fixtures = QString(OMAMP_TESTS_DIR) + "/fixtures/spotify";
        QDirIterator it(fixtures, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString from = it.next();
            const QString to = state() + from.mid(fixtures.size());
            QDir().mkpath(QFileInfo(to).absolutePath());
            QFile in(from);
            in.open(QIODevice::ReadOnly);
            QByteArray data = in.readAll();
            data.replace("http://art.invalid", artBase.toUtf8());
            QFile out(to);
            out.open(QIODevice::WriteOnly);
            out.write(data);
        }
        QFile list(state() + "/ids.txt");
        list.open(QIODevice::ReadOnly);
        for (const QByteArray &line : list.readAll().split('\n')) {
            const QList<QByteArray> f = line.split('\t');
            if (f.size() == 3)
                ids.insert(QString::fromUtf8(f[0] + ':' + f[1]), QString::fromUtf8(f[2]));
        }
        QDir().mkpath(cache());
        QDir().mkpath(config());
        if (signedIn)
            signIn();
        set("PATH", QString(OMAMP_TESTS_DIR) + "/fake-spotify:" + qEnvironmentVariable("PATH"));
        set("FAKE_SPOTIFY_DIR", state());
        set("OMAMP_SPOTIFY_PLAYER_CACHE", cache());
        set("OMAMP_SPOTIFY_PLAYER_CONFIG", config());
        set("OMAMP_SPOTIFY_PLAYER", "");
        set("OMAMP_SPOTIFAST", "");
        set("OMAMP_SPOTIFY_OEMBED", "off");
        // No session reader unless a test brings the fake one.
        set("OMAMP_SPOTIFY_SESSION", "off");
        set("OMAMP_SPOTIFY_TIMEOUT_MS", "5000");
        set("OMAMP_SPOTIFY_LONG_TIMEOUT_MS", "8000");
        // Never Matt's real players: no MPRIS by default (a test that wants
        // one starts its own on a private bus), and no waiting.
        set("OMAMP_SPOTIFY_MPRIS_NAME", "org.mpris.MediaPlayer2.omamp_test_absent");
        set("OMAMP_SPOTIFY_MPRIS_BUS", "");
        set("OMAMP_SPOTIFY_GAP_MS", "0");
        set("OMAMP_SPOTIFY_WARMUP_MS", "0");
        set("XDG_CONFIG_HOME", root.filePath("xdg-config"));
        set("XDG_CACHE_HOME", root.filePath("xdg-cache"));
    }
    ~FakeSpotify() {
        stopDaemon();
        for (auto it = saved.cbegin(); it != saved.cend(); ++it)
            if (it.value().isNull())
                qunsetenv(it.key());
            else
                qputenv(it.key(), it.value());
    }
    void set(const char *name, const QString &value) {
        if (!saved.contains(name))
            saved.insert(name, qEnvironmentVariableIsSet(name) ? qgetenv(name) : QByteArray());
        qputenv(name, value.toUtf8());
    }
    QString state() const { return root.filePath("state"); }
    QString cache() const { return root.filePath("sp-cache"); }
    QString config() const { return root.filePath("sp-config"); }
    QString id(const QString &kindAndName) const { return ids.value(kindAndName); }
    void touch(const QString &file, const QByteArray &data = {}) {
        QFile f(file);
        f.open(QIODevice::WriteOnly);
        f.write(data);
    }
    void signIn() {
        touch(cache() + "/credentials.json");
        touch(cache() + "/0123456789abcdef_token.json");
    }
    void stopDaemon() {
        QFile::remove(state() + "/daemon.alive");
        Spotify::SpotifyPlayerBackend b;
        QElapsedTimer t;
        t.start();
        while (b.running() && t.elapsed() < 5000)
            QTest::qWait(50);
        if (b.running())
            qWarning("the fake daemon is still running after 5 s");
    }
    QStringList calls() const {
        QFile f(state() + "/calls.log");
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
    }
    void clearCalls() { QFile::remove(state() + "/calls.log"); }
    // What spotify_player would report: the track at a position.
    void setPlayback(const QString &trackId, qint64 progressMs, bool playing, const QString &contextUri = {}) {
        QFile item(state() + "/tracks/full-" + trackId + ".json");
        item.open(QIODevice::ReadOnly);
        const QByteArray json =
            "{\"device\":{\"id\":\"d1\",\"is_active\":true,\"is_private_session\":false,\"is_restricted\":false,"
            "\"name\":\"spotify-player\",\"type\":\"Speaker\",\"volume_percent\":70},\"repeat_state\":\"off\","
            "\"shuffle_state\":false,\"context\":" +
            (contextUri.isEmpty() ? QByteArray("null") : "{\"uri\":\"" + contextUri.toUtf8() + "\"}") +
            ",\"timestamp\":1760000000000,\"progress_ms\":" +
            QByteArray::number(progressMs) + ",\"is_playing\":" + (playing ? "true" : "false") +
            ",\"item\":" + item.readAll() +
            ",\"currently_playing_type\":\"track\",\"actions\":{\"disallows\":{}}}\n";
        touch(state() + "/playback.json.new", json);
        QFile::remove(state() + "/playback.json");
        QFile::rename(state() + "/playback.json.new", state() + "/playback.json");
    }
};

} // namespace
