// A pretend radio world on 127.0.0.1 for the radio tests, so no test ever
// reaches nightride.fm or radio-browser.info:
//
//   /live/<name>   an endless live stream of a registered MP3, looped, with
//                  ICY titles every metaInt bytes when asked (Icy-MetaData: 1).
//                  The title changes every titleEvery bytes of audio.
//   /meta          the Nightride feed: server-sent events, a snapshot on
//                  connect and whatever push() sends later.
//   /json/stations/search, /json/url/<uuid>
//                  radio-browser.info: a set answer, and the clicks counted.
//   /              nightride.fm's page (set with setHomepage).
//   /pic.png       a picture (a station's favicon).
//
// Each connection runs on a thread of its own with blocking sockets, as in
// audiohttp.h. It records what it was asked.
#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QMap>
#include <QMutex>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <atomic>
#include <list>
#include <thread>

class RadioHttp {
public:
    struct Live {
        QByteArray audio;
        QStringList titles;       // StreamTitle values, in turn
        qint64 titleEvery = 64000; // audio bytes per title
        int metaInt = 8000;
        int bytesPerSecond = 0;   // 0: as fast as the client reads
        int status = 200;
    };
    struct Request {
        QByteArray method, path, query;
        QMap<QByteArray, QByteArray> headers;
    };

    RadioHttp() {
        QSemaphore ready;
        m_accept = std::thread([this, &ready] {
            Server server(this);
            server.listen(QHostAddress::LocalHost, 0);
            m_port = server.serverPort();
            ready.release();
            while (!m_stopping)
                server.waitForNewConnection(50);
        });
        ready.acquire();
    }
    ~RadioHttp() {
        m_stopping = true;
        m_accept.join();
        for (auto &t : m_connections)
            t.join();
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_port.load()); }
    QString liveUrl(const QString &name) const { return base() + "/live/" + name; }

    void addLive(const QString &name, const Live &live) {
        QMutexLocker l(&m_mutex);
        m_live.insert(name.toUtf8(), live);
    }
    void setFeedSnapshot(const QList<QByteArray> &events) {
        QMutexLocker l(&m_mutex);
        m_snapshot = events;
    }
    // Sends one event to every open feed connection.
    void push(const QByteArray &data) {
        QMutexLocker l(&m_mutex);
        m_pushed << data;
    }
    void setSearchAnswer(const QByteArray &json) {
        QMutexLocker l(&m_mutex);
        m_search = json;
    }
    void setHomepage(const QByteArray &html) {
        QMutexLocker l(&m_mutex);
        m_homepage = html;
    }
    QList<Request> requests() {
        QMutexLocker l(&m_mutex);
        return m_requests;
    }
    int count(const QByteArray &pathPrefix) {
        int n = 0;
        for (const Request &r : requests())
            n += r.path.startsWith(pathPrefix);
        return n;
    }
    int openFeeds() const { return m_openFeeds; }

private:
    class Server : public QTcpServer {
    public:
        explicit Server(RadioHttp *owner) : m_owner(owner) {}

    protected:
        void incomingConnection(qintptr fd) override {
            QMutexLocker l(&m_owner->m_mutex);
            m_owner->m_connections.emplace_back([owner = m_owner, fd] { owner->serve(fd); });
        }

    private:
        RadioHttp *m_owner;
    };

    bool writeAll(QTcpSocket &s, const QByteArray &bytes) {
        s.write(bytes);
        while (s.bytesToWrite() > 0) {
            if (m_stopping || s.state() != QAbstractSocket::ConnectedState)
                return false;
            s.waitForBytesWritten(50);
        }
        return s.state() == QAbstractSocket::ConnectedState;
    }

    void answer(QTcpSocket &s, int status, const QByteArray &type, const QByteArray &body) {
        writeAll(s, "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Nope") +
                        "\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body);
        s.disconnectFromHost();
        if (s.state() != QAbstractSocket::UnconnectedState)
            s.waitForDisconnected(1000);
    }

    void serve(qintptr fd) {
        QTcpSocket s;
        s.setSocketDescriptor(fd);
        QByteArray head;
        while (!head.contains("\r\n\r\n")) {
            if (m_stopping || !s.waitForReadyRead(5000))
                return;
            head += s.readAll();
        }
        Request r;
        const QList<QByteArray> lines = head.left(head.indexOf("\r\n\r\n")).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        r.method = first.value(0);
        const QByteArray target = first.value(1);
        const int q = target.indexOf('?');
        r.path = q < 0 ? target : target.left(q);
        r.query = q < 0 ? QByteArray() : target.mid(q + 1);
        for (int i = 1; i < lines.size(); ++i) {
            const int colon = lines[i].indexOf(':');
            if (colon > 0)
                r.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
        {
            QMutexLocker l(&m_mutex);
            m_requests << r;
        }
        if (r.path.startsWith("/live/"))
            return serveLive(s, r);
        if (r.path == "/meta")
            return serveFeed(s);
        if (r.path == "/json/stations/search") {
            QByteArray body;
            {
                QMutexLocker l(&m_mutex);
                body = m_search;
            }
            return answer(s, 200, "application/json", body.isEmpty() ? "[]" : body);
        }
        if (r.path.startsWith("/json/url/"))
            return answer(s, 200, "application/json", "{\"ok\":true}");
        if (r.path == "/pic.png") {
            QImage image(128, 128, QImage::Format_RGB32);
            image.fill(QColor("#336699"));
            QByteArray png;
            QBuffer b(&png);
            b.open(QIODevice::WriteOnly);
            image.save(&b, "PNG");
            return answer(s, 200, "image/png", png);
        }
        if (r.path == "/") {
            QByteArray body;
            {
                QMutexLocker l(&m_mutex);
                body = m_homepage;
            }
            return answer(s, body.isEmpty() ? 404 : 200, "text/html", body);
        }
        answer(s, 404, "text/plain", "");
    }

    void serveFeed(QTcpSocket &s) {
        if (!writeAll(s, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                         "Connection: close\r\n\r\n"))
            return;
        qsizetype seen = 0;
        QList<QByteArray> snapshot;
        {
            QMutexLocker l(&m_mutex);
            snapshot = m_snapshot;
            seen = m_pushed.size();
        }
        ++m_openFeeds;
        bool ok = true;
        for (const QByteArray &e : snapshot)
            ok = ok && writeAll(s, "data: " + e + "\n\n");
        while (ok && !m_stopping) {
            QList<QByteArray> fresh;
            {
                QMutexLocker l(&m_mutex);
                fresh = m_pushed.mid(seen);
                seen = m_pushed.size();
            }
            for (const QByteArray &e : fresh)
                ok = ok && writeAll(s, "data: " + e + "\n\n");
            if (!ok)
                break;
            s.waitForReadyRead(20);
            if (s.state() != QAbstractSocket::ConnectedState)
                break;
        }
        --m_openFeeds;
    }

    void serveLive(QTcpSocket &s, const Request &r) {
        const QByteArray name = r.path.mid(6);
        Live live;
        bool found = false;
        {
            QMutexLocker l(&m_mutex);
            found = m_live.contains(name);
            live = m_live.value(name);
        }
        if (!found || live.status != 200 || live.audio.isEmpty())
            return answer(s, found ? live.status : 404, "text/plain", "");
        const bool icy = r.headers.value("icy-metadata") == "1" && live.metaInt > 0;
        QByteArray header = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nConnection: close\r\nicy-name: Test FM\r\n";
        if (icy)
            header += "icy-metaint: " + QByteArray::number(live.metaInt) + "\r\n";
        header += "\r\n";
        if (!writeAll(s, header))
            return;
        qint64 sent = 0, pos = 0, sinceMeta = 0;
        QString lastTitle;
        QElapsedTimer clock;
        clock.start();
        while (!m_stopping) {
            int take = 4096;
            if (icy)
                take = int(qMin<qint64>(take, live.metaInt - sinceMeta));
            QByteArray chunk = live.audio.mid(int(pos), take);
            if (chunk.size() < take)
                chunk += live.audio.left(take - chunk.size());
            pos = (pos + take) % live.audio.size();
            if (!writeAll(s, chunk))
                return;
            sent += take;
            sinceMeta += take;
            if (icy && sinceMeta == live.metaInt) {
                sinceMeta = 0;
                QByteArray meta;
                const QString title = live.titles.isEmpty()
                                          ? QString()
                                          : live.titles.at(int((sent / qMax<qint64>(1, live.titleEvery)) % live.titles.size()));
                if (title != lastTitle) {
                    meta = "StreamTitle='" + title.toUtf8() + "';";
                    lastTitle = title;
                }
                const int blocks = int((meta.size() + 15) / 16);
                meta.resize(blocks * 16, '\0');
                if (!writeAll(s, QByteArray(1, char(blocks)) + meta))
                    return;
            }
            if (live.bytesPerSecond > 0) {
                const qint64 due = sent * 1000 / live.bytesPerSecond;
                if (due > clock.elapsed())
                    QThread::msleep(quint64(due - clock.elapsed()));
            }
        }
    }

    std::thread m_accept;
    std::list<std::thread> m_connections;
    std::atomic<quint16> m_port{0};
    std::atomic<bool> m_stopping{false};
    std::atomic<int> m_openFeeds{0};
    QMutex m_mutex;
    QHash<QByteArray, Live> m_live;
    QList<QByteArray> m_snapshot, m_pushed;
    QByteArray m_search, m_homepage;
    QList<Request> m_requests;
};
