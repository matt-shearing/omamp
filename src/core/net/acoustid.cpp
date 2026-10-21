#include "acoustid.h"

#include "library.h"
#include "musicbrainz.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QUrlQuery>

namespace AcoustId {

QString applicationKey(const Library *library) {
    const QString env = qEnvironmentVariable("THRUM_ACOUSTID_KEY").trimmed();
    if (!env.isEmpty())
        return env;
    if (library && library->isOpen())
        return library->setting("acoustid.key").toString().trimmed();
    return {};
}

QString missingKeyMessage() {
    return "AcoustID was skipped: no application key. Register one at https://acoustid.org/new-application and set "
           "it with `thrum net set acoustid.key <key>` or THRUM_ACOUSTID_KEY. Matching used "
           "MusicBrainz tags and IDs only.";
}

Fingerprint compute(const QString &path, const QString &programIn) {
    Fingerprint f;
    const QString program = programIn.isEmpty() ? QStandardPaths::findExecutable("fpcalc") : programIn;
    if (program.isEmpty()) {
        f.error = "fpcalc is not installed (it comes with the chromaprint package)";
        return f;
    }
    QProcess p;
    // Decodes to memory; nothing is played.
    p.start(program, {"-json", "-length", "120", path});
    if (!p.waitForFinished(180000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        f.error = QString("fpcalc failed: %1").arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
        return f;
    }
    const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    f.fingerprint = o.value("fingerprint").toString();
    f.durationSeconds = qRound(o.value("duration").toDouble());
    f.ok = !f.fingerprint.isEmpty() && f.durationSeconds > 0;
    if (!f.ok)
        f.error = "fpcalc gave no fingerprint (the file may be too short)";
    return f;
}

Result lookup(Net::HttpClient &http, const QString &key, const QString &fingerprint, int durationSeconds) {
    Result result;
    if (key.isEmpty()) {
        result.missingKey = true;
        result.error = missingKeyMessage();
        return result;
    }
    // A POST keeps a long fingerprint out of the URL, as AcoustID suggests.
    QUrlQuery form;
    form.addQueryItem("client", key);
    form.addQueryItem("format", "json");
    form.addQueryItem("meta", "recordings releaseids releasegroupids");
    form.addQueryItem("duration", QString::number(durationSeconds));
    form.addQueryItem("fingerprint", fingerprint);
    Net::Request request;
    request.service = Net::Service::AcoustId;
    request.method = "POST";
    request.url = QUrl(Net::baseUrl(Net::Service::AcoustId, http.library()) + "/lookup");
    request.contentType = "application/x-www-form-urlencoded";
    request.body = form.query(QUrl::FullyEncoded).toUtf8();
    request.cacheSeconds = 30 * 86400;
    const Net::Response r = http.send(request);
    result.offline = r.offline;
    const QJsonObject o = r.json().object();
    if (o.value("status").toString() != "ok") {
        const QJsonObject e = o.value("error").toObject();
        const int code = e.value("code").toInt();
        result.error = e.isEmpty() ? QString("AcoustID: %1").arg(r.error.isEmpty() ? QString("no answer") : r.error)
                                   : QString("AcoustID: %1").arg(e.value("message").toString());
        // Codes 4 and 5: an invalid or missing application key.
        result.missingKey = code == 4 || code == 5;
        return result;
    }
    for (const QJsonValue &rv : o.value("results").toArray()) {
        const QJsonObject res = rv.toObject();
        const double score = res.value("score").toDouble();
        for (const QJsonValue &recv : res.value("recordings").toArray()) {
            const QJsonObject rec = recv.toObject();
            Match m;
            m.score = score;
            m.acoustId = res.value("id").toString();
            m.recordingId = rec.value("id").toString();
            m.title = rec.value("title").toString();
            m.artist = MusicBrainz::creditText(MusicBrainz::parseCredits(rec.value("artists").toArray()));
            m.lengthMs = qint64(rec.value("duration").toDouble() * 1000);
            for (const QJsonValue &v : rec.value("releases").toArray())
                m.releaseIds << v.toObject().value("id").toString();
            for (const QJsonValue &v : rec.value("releasegroups").toArray()) {
                const QJsonObject g = v.toObject();
                m.releaseGroupIds << g.value("id").toString();
                for (const QJsonValue &rel : g.value("releases").toArray())
                    m.releaseIds << rel.toObject().value("id").toString();
            }
            m.releaseIds.removeDuplicates();
            result.matches << m;
        }
    }
    std::stable_sort(result.matches.begin(), result.matches.end(),
                     [](const Match &a, const Match &b) { return a.score > b.score; });
    result.ok = true;
    return result;
}

}
