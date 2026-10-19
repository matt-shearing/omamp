// A small HTTP/1.1 file server on 127.0.0.1 for the streaming tests, so no
// test reaches a real server. It serves registered files at /stream/<name>,
// with or without byte ranges, all at once or slowly in chunks (optionally
// with chunked transfer encoding), after a delay, or breaking off part-way.
// Every connection runs on a thread of its own with blocking sockets, and
// is closed after one answer. It records what it was asked.
#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QMap>
#include <QMutex>
#include <QSemaphore>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <atomic>
#include <list>
#include <memory>
#include <thread>

class AudioHttp {
public:
    struct Behaviour {
        bool ranges = true;          // Accept-Ranges and 206 answers to Range
        bool chunked = false;        // Transfer-Encoding: chunked, no length
        int pieceBytes = 0;          // write in pieces of this size (0: at once)
        int pieceDelayMs = 0;        // wait between pieces
        int delayMs = 0;             // wait before answering at all
        qint64 dropAfter = -1;       // close the connection after this many body bytes
        bool dropOnce = true;        // only on the first answer for the file
        int status = 200;            // 404 etc. instead of the file
    };
    struct Request {
        QByteArray method, path, query;
        QMap<QByteArray, QByteArray> headers; // names in lower case
    };

    AudioHttp() {
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
    ~AudioHttp() {
        m_stopping = true;
        m_accept.join();
        for (auto &t : m_connections)
            t.join();
    }

    quint16 port() const { return m_port; }
    QString url(const QString &name, const QString &query = {}) const {
        return QStringLiteral("http://127.0.0.1:%1/stream/%2%3")
            .arg(m_port)
            .arg(name, query.isEmpty() ? QString() : '?' + query);
    }

    void addFile(const QString &name, const QString &path) {
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        QMutexLocker l(&m_mutex);
        m_files.insert(name.toUtf8(), f.readAll());
    }
    qint64 fileSize(const QString &name) {
        QMutexLocker l(&m_mutex);
        return m_files.value(name.toUtf8()).size();
    }
    void setBehaviour(const QString &name, const Behaviour &b) {
        QMutexLocker l(&m_mutex);
        m_behaviour.insert(name.toUtf8(), b);
        m_dropped.remove(name.toUtf8());
    }
    QList<Request> requests() {
        QMutexLocker l(&m_mutex);
        return m_requests;
    }
    int requestsFor(const QString &name) {
        int n = 0;
        for (const Request &r : requests())
            if (r.path == "/stream/" + name.toUtf8())
                ++n;
        return n;
    }
    void clearRequests() {
        QMutexLocker l(&m_mutex);
        m_requests.clear();
    }

private:
    class Server : public QTcpServer {
    public:
        explicit Server(AudioHttp *owner) : m_owner(owner) {}

    protected:
        void incomingConnection(qintptr fd) override {
            QMutexLocker l(&m_owner->m_mutex);
            m_owner->m_connections.emplace_back([owner = m_owner, fd] { owner->serve(fd); });
        }

    private:
        AudioHttp *m_owner;
    };

    bool writeAll(QTcpSocket &s, const QByteArray &bytes) {
        s.write(bytes);
        while (s.bytesToWrite() > 0) {
            if (m_stopping || s.state() != QAbstractSocket::ConnectedState)
                return false;
            s.waitForBytesWritten(100);
        }
        return true;
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
        const QByteArray name = r.path.mid(QByteArray("/stream/").size());
        QByteArray body;
        Behaviour b;
        bool drop = false;
        {
            QMutexLocker l(&m_mutex);
            m_requests << r;
            body = m_files.value(name);
            b = m_behaviour.value(name);
            if (b.dropAfter >= 0 && (!b.dropOnce || !m_dropped.contains(name))) {
                drop = true;
                m_dropped.insert(name);
            }
        }
        for (int waited = 0; waited < b.delayMs && !m_stopping; waited += 20)
            QThread::msleep(20);
        if (!r.path.startsWith("/stream/") || body.isEmpty() || b.status != 200) {
            const int status = b.status != 200 ? b.status : 404;
            writeAll(s, "HTTP/1.1 " + QByteArray::number(status) +
                            " Nope\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s.disconnectFromHost();
            return;
        }
        const qint64 total = body.size();
        qint64 from = 0, to = total - 1;
        bool partial = false;
        const QByteArray range = r.headers.value("range");
        if (b.ranges && range.startsWith("bytes=")) {
            const QList<QByteArray> ends = range.mid(6).split('-');
            from = ends.value(0).toLongLong();
            if (!ends.value(1).isEmpty())
                to = qMin(total - 1, ends.value(1).toLongLong());
            partial = true;
            if (from >= total) {
                writeAll(s, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                                QByteArray::number(total) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                s.disconnectFromHost();
                return;
            }
        }
        QByteArray header = partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
        header += "Content-Type: audio/flac\r\nConnection: close\r\n";
        if (b.ranges)
            header += "Accept-Ranges: bytes\r\n";
        if (partial)
            header += "Content-Range: bytes " + QByteArray::number(from) + '-' + QByteArray::number(to) + '/' +
                      QByteArray::number(total) + "\r\n";
        if (b.chunked)
            header += "Transfer-Encoding: chunked\r\n";
        else
            header += "Content-Length: " + QByteArray::number(to - from + 1) + "\r\n";
        header += "\r\n";
        if (!writeAll(s, header) || r.method == "HEAD")
            return;
        const QByteArray payload = body.mid(from, to - from + 1);
        const int piece = b.pieceBytes > 0 ? b.pieceBytes : int(payload.size());
        qint64 sent = 0;
        while (sent < payload.size()) {
            qint64 n = qMin<qint64>(piece, payload.size() - sent);
            if (drop && sent + n > b.dropAfter)
                n = qMax<qint64>(0, b.dropAfter - sent);
            const QByteArray part = payload.mid(sent, n);
            const QByteArray framed =
                b.chunked ? QByteArray::number(part.size(), 16) + "\r\n" + part + "\r\n" : part;
            if (n > 0 && !writeAll(s, framed))
                return;
            sent += n;
            if (drop && sent >= b.dropAfter) {
                // Broken off mid-stream, as a server or network failing.
                s.abort();
                return;
            }
            for (int waited = 0; waited < b.pieceDelayMs && !m_stopping; waited += 5)
                QThread::msleep(5);
        }
        if (b.chunked)
            writeAll(s, "0\r\n\r\n");
        s.disconnectFromHost();
        if (s.state() != QAbstractSocket::UnconnectedState)
            s.waitForDisconnected(1000);
    }

    std::atomic<bool> m_stopping{false};
    std::thread m_accept;
    std::list<std::thread> m_connections;
    quint16 m_port = 0;
    QMutex m_mutex;
    QHash<QByteArray, QByteArray> m_files;
    QHash<QByteArray, Behaviour> m_behaviour;
    QSet<QByteArray> m_dropped;
    QList<Request> m_requests;
};
