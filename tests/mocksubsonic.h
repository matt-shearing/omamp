// A mock Navidrome (OpenSubsonic) server on 127.0.0.1 for the tests: a
// QTcpServer on the thread that makes it, serving three albums in the
// Subsonic API's JSON shape and checking every request's token against its
// password. No real server is ever contacted.
#pragma once

#include <QBuffer>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include "subsonic.h"

namespace {

inline QJsonObject song(const QString &id, const QString &title, const QString &album, const QString &albumId,
                 const QString &artist, int track, int seconds) {
    return {{"id", id},
            {"parent", albumId},
            {"isDir", false},
            {"title", title},
            {"album", album},
            {"albumId", albumId},
            {"artist", artist},
            {"track", track},
            {"discNumber", 1},
            {"year", 1997},
            {"genre", "Synthwave"},
            {"coverArt", albumId},
            {"size", 1000000 + track},
            {"contentType", "audio/flac"},
            {"suffix", "flac"},
            {"duration", seconds},
            {"bitRate", 900},
            {"path", QString("%1/%2/%3 %4.flac").arg(artist, album).arg(track, 2, 10, QChar('0')).arg(title)},
            {"created", "2026-01-02T03:04:05.000Z"},
            {"type", "music"}};
}

// Navidrome, as far as these tests need it.
class MockSubsonic : public QObject {
public:
    QString user = "listener", password = "sesame", apiKey = "key-123";
    bool refuse = false;
    bool realPictures = false;              // getCoverArt answers with a real PNG
    QList<QJsonObject> albums;              // getAlbumList2 entries, kept sorted by name
    QHash<QString, QJsonArray> songs;       // album id -> songs
    QStringList methods;                    // every call, in order
    QList<QUrlQuery> queries;

    MockSubsonic() {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, &MockSubsonic::accept);
        addAlbum({{"id", "al-1"}, {"name", "Night Drive"}, {"artist", "Neon Coast"}, {"year", 1997},
                  {"genre", "Synthwave"}, {"coverArt", "al-1"}, {"created", "2026-01-01T00:00:00Z"}},
                 {song("s-1", "Highway", "Night Drive", "al-1", "Neon Coast", 1, 200),
                  song("s-2", "Tunnel", "Night Drive", "al-1", "Neon Coast", 2, 180)});
        QJsonObject quiet = song("s-3", "Low Light", "Quiet Hours", "al-2", "Lena Park", 1, 240);
        // OpenSubsonic fields: several artists, genres, ReplayGain, a rating.
        quiet["artists"] = QJsonArray{QJsonObject{{"id", "ar-2"}, {"name", "Lena Park"}},
                                      QJsonObject{{"id", "ar-3"}, {"name", "Guest Voice"}}};
        quiet["genres"] = QJsonArray{QJsonObject{{"name", "Ambient"}}, QJsonObject{{"name", "Electronic"}}};
        quiet["replayGain"] = QJsonObject{{"trackGain", -6.5}, {"trackPeak", 0.98}};
        addAlbum({{"id", "al-2"}, {"name", "Quiet Hours"}, {"artist", "Lena Park"}, {"year", 2001},
                  {"coverArt", "al-2"}, {"created", "2026-01-01T00:00:00Z"}},
                 {quiet});
        QJsonObject live = song("s-4", "Highway (Live)", "Live", "al-3", "Neon Coast", 1, 260);
        live["userRating"] = 4;
        live["playCount"] = 3;
        addAlbum({{"id", "al-3"}, {"name", "Live"}, {"artist", "Neon Coast"}, {"year", 1999}, {"coverArt", "al-3"},
                  {"created", "2026-01-01T00:00:00Z"}},
                 {live});
    }

    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1").arg(m_server.serverPort())); }

    void addAlbum(QJsonObject album, const QJsonArray &list) {
        album["songCount"] = list.size();
        int duration = 0;
        for (const QJsonValue &s : list)
            duration += s.toObject()["duration"].toInt();
        album["duration"] = duration;
        removeAlbum(album["id"].toString());
        albums << album;
        std::sort(albums.begin(), albums.end(),
                  [](const QJsonObject &a, const QJsonObject &b) { return a["name"].toString() < b["name"].toString(); });
        songs.insert(album["id"].toString(), list);
    }
    void removeAlbum(const QString &id) {
        albums.removeIf([&](const QJsonObject &a) { return a["id"].toString() == id; });
        songs.remove(id);
    }
    int count(const QString &method) const { return methods.count(method); }

private:
    void accept() {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            auto *buffer = new QByteArray;
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                buffer->append(socket->readAll());
                if (!buffer->contains("\r\n\r\n"))
                    return;
                const QByteArray line = buffer->left(buffer->indexOf("\r\n"));
                buffer->clear();
                respond(socket, QUrl(QString::fromLatin1(line.split(' ').value(1))));
            });
            connect(socket, &QTcpSocket::disconnected, socket, [socket, buffer] {
                delete buffer;
                socket->deleteLater();
            });
        }
    }

    static void send(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Error") + "\r\n" +
                      "Content-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) +
                      "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    static QByteArray envelope(QJsonObject payload, const QString &status = "ok") {
        payload["status"] = status;
        payload["version"] = "1.16.1";
        payload["type"] = "navidrome";
        payload["serverVersion"] = "0.53.3";
        payload["openSubsonic"] = true;
        return QJsonDocument(QJsonObject{{"subsonic-response", payload}}).toJson(QJsonDocument::Compact);
    }

    static QByteArray failure(int code, const QString &message) {
        return envelope({{"error", QJsonObject{{"code", code}, {"message", message}}}}, "failed");
    }

    void respond(QTcpSocket *socket, const QUrl &url) {
        if (refuse) {
            send(socket, 503, "text/html", "<html>Service Unavailable</html>");
            return;
        }
        QString method = url.path().section('/', -1);
        if (method.endsWith(".view"))
            method.chop(5);
        const QUrlQuery q(url);
        methods << method;
        queries << q;
        const QByteArray json = "application/json";
        // Auth: an API key, or a user with md5(password + salt).
        if (q.hasQueryItem("p")) {
            send(socket, 200, json, failure(10, "Plain passwords are not accepted here"));
            return;
        }
        if (q.hasQueryItem("apiKey")) {
            if (q.queryItemValue("apiKey") != apiKey) {
                send(socket, 200, json, failure(44, "Invalid API key"));
                return;
            }
        } else if (q.queryItemValue("u") != user || q.queryItemValue("s").size() < 6 ||
                   q.queryItemValue("t") != SubsonicClient::token(password, q.queryItemValue("s"))) {
            send(socket, 200, json, failure(40, "Wrong username or password"));
            return;
        }
        if (q.queryItemValue("f") != "json" || q.queryItemValue("c").isEmpty() || q.queryItemValue("v").isEmpty()) {
            send(socket, 200, json, failure(10, "Required parameter is missing"));
            return;
        }

        if (method == "ping" || method == "scrobble" || method == "setRating") {
            send(socket, 200, json, envelope({}));
        } else if (method == "getMusicFolders") {
            send(socket, 200, json,
                 envelope({{"musicFolders", QJsonObject{{"musicFolder", QJsonArray{QJsonObject{{"id", 1}, {"name", "Music"}}}}}}}));
        } else if (method == "getArtists") {
            const QJsonArray indexes{
                QJsonObject{{"name", "L"}, {"artist", QJsonArray{QJsonObject{{"id", "ar-2"}, {"name", "Lena Park"}, {"albumCount", 1}}}}},
                QJsonObject{{"name", "N"}, {"artist", QJsonArray{QJsonObject{{"id", "ar-1"}, {"name", "Neon Coast"}, {"albumCount", 2}}}}}};
            send(socket, 200, json, envelope({{"artists", QJsonObject{{"ignoredArticles", "The"}, {"index", indexes}}}}));
        } else if (method == "getAlbumList2") {
            const int size = q.queryItemValue("size").toInt();
            const int offset = q.queryItemValue("offset").toInt();
            QJsonArray page;
            for (int i = offset; i < albums.size() && i < offset + size; ++i)
                page.append(albums[i]);
            send(socket, 200, json, envelope({{"albumList2", QJsonObject{{"album", page}}}}));
        } else if (method == "getAlbum") {
            const QString id = q.queryItemValue("id");
            for (const QJsonObject &a : std::as_const(albums)) {
                if (a["id"].toString() == id) {
                    QJsonObject full = a;
                    full["song"] = songs.value(id);
                    send(socket, 200, json, envelope({{"album", full}}));
                    return;
                }
            }
            send(socket, 200, json, failure(70, "Album not found"));
        } else if (method == "search3") {
            const QString text = q.queryItemValue("query").toLower();
            QJsonArray found;
            for (const QJsonArray &list : std::as_const(songs))
                for (const QJsonValue &s : list)
                    if (s.toObject()["title"].toString().toLower().contains(text))
                        found.append(s);
            send(socket, 200, json, envelope({{"searchResult3", QJsonObject{{"song", found}}}}));
        } else if (method == "getCoverArt") {
            if (realPictures) {
                QImage image(16, 16, QImage::Format_RGB32);
                image.fill(QColor(30, 160, 200));
                QByteArray png;
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                image.save(&buffer, "PNG");
                send(socket, 200, "image/png", png);
                return;
            }
            send(socket, 200, "image/png", QByteArray("\x89PNG\r\n\x1a\n-cover-") + q.queryItemValue("id").toUtf8());
        } else if (method == "stream") {
            send(socket, 200, "audio/flac", "fLaC-not-really");
        } else {
            send(socket, 200, json, failure(0, "Unknown method " + method));
        }
    }

    QTcpServer m_server;
};

} // namespace
