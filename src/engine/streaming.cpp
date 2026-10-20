#include "streaming.h"

#include <QCoreApplication>
#include <QMutex>
#include <QRegularExpression>

#include "audioengine.h"

namespace {
QMutex optionsMutex;
Streaming::Options current;
} // namespace

namespace Streaming {

Options options() {
    QMutexLocker lock(&optionsMutex);
    return current;
}

void setOptions(const Options &options) {
    QMutexLocker lock(&optionsMutex);
    current = options;
}

QString userAgent() {
    QString version = QCoreApplication::applicationVersion();
    if (version.isEmpty())
        version = QStringLiteral("dev");
    return QStringLiteral("Omamp/") + version;
}

QByteArray ffmpegLocation(const QString &path, const QUrl &url) {
    if (!url.isValid() || url.isEmpty())
        return path.toUtf8();
    if (url.isLocalFile())
        return url.toLocalFile().toUtf8();
    const QByteArray encoded = url.toEncoded();
    // Not for live radio: the read-ahead wrapper hides the HTTP layer and
    // with it the stream's ICY titles.
    if (AudioEngine::isNetworkUrl(url) && options().readAhead && !isLive(path))
        return "async:" + encoded;
    return encoded;
}

} // namespace Streaming

QString AudioEngine::redact(const QString &text) {
    // Query items that sign or authenticate a Subsonic request (t, s, p,
    // apiKey) and the usual names elsewhere, then user:password@ in a URL.
    static const QRegularExpression query(
        QStringLiteral("([?&;](?:t|s|p|apikey|api_key|token|access_token|password|key|auth)=)[^&\\s\"'<>#]*"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression userInfo(QStringLiteral("(://[^/\\s:@]+:)[^@\\s/]+@"));
    QString out = text;
    out.replace(query, QStringLiteral("\\1***"));
    out.replace(userInfo, QStringLiteral("\\1***@"));
    return out;
}
