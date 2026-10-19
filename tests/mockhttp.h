// A tiny HTTP/1.1 server on 127.0.0.1 for the web tests, so no test ever
// reaches the internet. It runs on its own thread (the client under test
// blocks its own), answers each request through a handler the test sets,
// closes every connection after one answer, and records what it was asked.
#pragma once

#include <QEventLoop>
#include <QHostAddress>
#include <QMap>
#include <QMutex>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUrlQuery>
#include <atomic>
#include <functional>
#include <memory>

class MockHttp {
public:
    struct Request {
        QByteArray method, path, query; // path without the query
        QMap<QByteArray, QByteArray> headers; // names in lower case
        QByteArray body;
        QString item(const QString &name) const { return decoded(query, name); }
        QString formItem(const QString &name) const { return decoded(body, name); }
        // One form or query value, fully decoded ("artist%5B0%5D" is "artist[0]").
        static QString decoded(const QByteArray &text, const QString &name) {
            for (const QByteArray &pair : text.split('&')) {
                const int eq = pair.indexOf('=');
                QByteArray key = eq < 0 ? pair : pair.left(eq), value = eq < 0 ? QByteArray() : pair.mid(eq + 1);
                key.replace('+', ' ');
                value.replace('+', ' ');
                if (QUrl::fromPercentEncoding(key) == name)
                    return QUrl::fromPercentEncoding(value);
            }
            return {};
        }
    };
    struct Reply {
        int status = 200;
        QByteArray body;
        QByteArray contentType = "application/json";
        QList<QPair<QByteArray, QByteArray>> headers;
    };
    using Handler = std::function<Reply(const Request &)>;

    MockHttp() {
        QSemaphore ready;
        m_thread.reset(QThread::create([this, &ready] {
            QTcpServer server;
            server.listen(QHostAddress::LocalHost, 0);
            m_port = server.serverPort();
            QEventLoop loop;
            m_loop = &loop;
            QObject::connect(&server, &QTcpServer::newConnection, &server, [this, &server] {
                while (QTcpSocket *socket = server.nextPendingConnection())
                    serve(socket);
            });
            ready.release();
            loop.exec();
            m_loop = nullptr;
        }));
        m_thread->start();
        ready.acquire();
    }

    ~MockHttp() {
        QEventLoop *loop = m_loop.load();
        if (loop)
            QMetaObject::invokeMethod(loop, "quit", Qt::QueuedConnection);
        m_thread->wait();
    }

    QString url() const { return QString("http://127.0.0.1:%1").arg(m_port.load()); }

    void setHandler(Handler handler) {
        QMutexLocker lock(&m_mutex);
        m_handler = std::move(handler);
    }
    QList<Request> requests() {
        QMutexLocker lock(&m_mutex);
        return m_requests;
    }
    int count() {
        QMutexLocker lock(&m_mutex);
        return int(m_requests.size());
    }
    void clear() {
        QMutexLocker lock(&m_mutex);
        m_requests.clear();
    }

private:
    void serve(QTcpSocket *socket) {
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
            buffer->append(socket->readAll());
            const int end = buffer->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            Request r;
            const QList<QByteArray> lines = buffer->left(end).split('\n');
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
            const int length = r.headers.value("content-length").toInt();
            if (buffer->size() < end + 4 + length)
                return; // the body is still coming
            r.body = buffer->mid(end + 4, length);
            buffer->clear();
            Handler handler;
            {
                QMutexLocker lock(&m_mutex);
                m_requests << r;
                handler = m_handler;
            }
            Reply reply = handler ? handler(r) : Reply{404, "{\"error\":\"no handler\"}"};
            QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.status) + " X\r\n";
            out += "Content-Type: " + reply.contentType + "\r\n";
            out += "Content-Length: " + QByteArray::number(reply.body.size()) + "\r\n";
            out += "Connection: close\r\n";
            for (const auto &h : reply.headers)
                out += h.first + ": " + h.second + "\r\n";
            out += "\r\n" + reply.body;
            socket->write(out);
            socket->flush();
            socket->disconnectFromHost();
        });
    }

    std::unique_ptr<QThread> m_thread;
    std::atomic<QEventLoop *> m_loop = nullptr;
    std::atomic_int m_port = 0;
    QMutex m_mutex;
    Handler m_handler;
    QList<Request> m_requests;
};
