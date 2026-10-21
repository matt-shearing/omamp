// `thrum radio`: the stations, tuning in, radio-browser.info search and
// station lists from the command line. Kept apart from cli.cpp, as the NAS
// and sync commands are.
#include "cli.h"

#include "http.h"
#include "library.h"
#include "radio.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

namespace Cli {

namespace {

struct RadioOptions {
    bool json = false;
    QString db, group = QStringLiteral("all"), name;
    int limit = 30;
    QStringList rest;
};

QString toJson(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)); }

int fail(const RadioOptions &o, QTextStream &out, QTextStream &err, const QString &message) {
    err << "thrum: " << message << Qt::endl;
    if (o.json)
        out << toJson({{"ok", false}, {"error", message}}) << Qt::flush;
    return 1;
}

// Hands radio://<key> to the running Thrum over MPRIS (OpenUri), or starts
// Thrum with it.
RadioLauncher &launcher() {
    static RadioLauncher l = [](const QString &uri, QString *how) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (bus.isConnected()) {
            const QString service = QStringLiteral("org.mpris.MediaPlayer2.thrum");
            if (bus.interface() && bus.interface()->isServiceRegistered(service).value()) {
                QDBusMessage call = QDBusMessage::createMethodCall(service, "/org/mpris/MediaPlayer2",
                                                                   "org.mpris.MediaPlayer2.Player", "OpenUri");
                call << uri;
                const QDBusMessage reply = bus.call(call, QDBus::Block, 5000);
                if (reply.type() != QDBusMessage::ErrorMessage) {
                    *how = QStringLiteral("running");
                    return true;
                }
            }
        }
        if (QProcess::startDetached(QCoreApplication::applicationFilePath(), {uri})) {
            *how = QStringLiteral("started");
            return true;
        }
        *how = QStringLiteral("Can't start Thrum");
        return false;
    };
    return l;
}

void printStation(QTextStream &out, const Radio::Station &s) {
    out << (s.favourite ? "* " : "  ") << s.fullName();
    if (!s.description.isEmpty())
        out << "  —  " << s.description;
    out << "\n      " << s.key << "  " << s.streams.value(0) << "\n";
}

} // namespace

void setRadioLauncher(RadioLauncher l) {
    if (l)
        launcher() = std::move(l);
}

int runRadio(const QStringList &arguments, QTextStream &out, QTextStream &err) {
    RadioOptions o;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &a = arguments[i];
        auto value = [&](QString *target) {
            if (i + 1 >= arguments.size())
                return false;
            *target = arguments[++i];
            return true;
        };
        QString v;
        if (a == "--json")
            o.json = true;
        else if (a == "--db") {
            if (!value(&o.db))
                return fail(o, out, err, "--db needs a path");
        } else if (a == "--group") {
            if (!value(&o.group))
                return fail(o, out, err, "--group needs nightride, rekt, favourites, mine, recent or all");
        } else if (a == "--name") {
            if (!value(&o.name))
                return fail(o, out, err, "--name needs a name");
        } else if (a == "--limit") {
            if (!value(&v) || v.toInt() <= 0)
                return fail(o, out, err, "--limit needs a number");
            o.limit = v.toInt();
        } else {
            o.rest << a;
        }
    }
    const QString sub = o.rest.value(0, "list");
    const QString arg = o.rest.mid(1).join(' ');

    Library library(o.db);
    QString error;
    if (!library.open(&error))
        return fail(o, out, err, error);
    Radio::Stations stations(library);
    if (!stations.seed(&error))
        return fail(o, out, err, error);

    if (sub == "list" || sub == "ls") {
        static const QStringList groups = {"all", "nightride", "rekt", "favourites", "mine", "recent"};
        if (!groups.contains(o.group))
            return fail(o, out, err, "--group needs nightride, rekt, favourites, mine, recent or all");
        const QList<Radio::Station> list = stations.group(o.group);
        if (o.json) {
            QJsonArray rows;
            for (const Radio::Station &s : list)
                rows << s.toJson();
            out << toJson({{"ok", true}, {"group", o.group}, {"stations", rows}}) << Qt::flush;
            return 0;
        }
        QString network;
        for (const Radio::Station &s : list) {
            if (s.networkName() != network) {
                network = s.networkName();
                out << (network == list.first().networkName() ? "" : "\n") << network << "\n";
            }
            printStation(out, s);
        }
        return 0;
    }

    if (sub == "play" || sub == "tune") {
        if (arg.isEmpty())
            return fail(o, out, err, "radio play needs a station: a name, a key or a Nightride channel");
        QStringList ambiguous;
        const Radio::Station s = stations.find(arg, &ambiguous);
        if (!s.isValid())
            return fail(o, out, err,
                        ambiguous.isEmpty() ? QString("No station called \"%1\" (thrum radio list shows them)").arg(arg)
                                            : QString("\"%1\" could be %2").arg(arg, ambiguous.join(", ")));
        stations.notePlayed(s.key);
        QString how;
        if (!launcher()(s.path(), &how))
            return fail(o, out, err, how);
        if (o.json)
            out << toJson({{"ok", true}, {"station", s.toJson()}, {"uri", s.path()}, {"via", how}}) << Qt::flush;
        else
            out << "Tuning in to " << s.fullName() << (how == "started" ? " (starting Thrum)" : "") << "\n";
        return 0;
    }

    if (sub == "search" || sub == "find") {
        if (arg.isEmpty())
            return fail(o, out, err, "radio search needs some words");
        if (Net::Config::forcedOffline(&library))
            return fail(o, out, err, "Thrum is set to stay offline (net.offline)");
        Net::HttpClient http(&library);
        const Radio::Browser::Result r = Radio::Browser::search(http, Radio::Browser::baseUrl(&http), arg, o.limit);
        if (!r.ok())
            return fail(o, out, err, r.error);
        if (o.json) {
            QJsonArray rows;
            for (const Radio::Station &s : r.stations) {
                QJsonObject j = s.toJson();
                j["known"] = stations.byKey(s.key).isValid() || stations.byStreamUrl(s.streams.value(0)).isValid();
                rows << j;
            }
            out << toJson({{"ok", true}, {"query", arg}, {"stations", rows}}) << Qt::flush;
            return 0;
        }
        if (r.stations.isEmpty())
            out << "No stations found for \"" << arg << "\"\n";
        for (const Radio::Station &s : r.stations) {
            out << "  " << s.name;
            QStringList bits;
            if (!s.codec.isEmpty())
                bits << (s.bitrate > 0 ? QString("%1 %2 kbps").arg(s.codec).arg(s.bitrate) : s.codec);
            if (!s.country.isEmpty())
                bits << s.country;
            if (!s.tags.isEmpty())
                bits << s.tags;
            if (!bits.isEmpty())
                out << "  —  " << bits.join(" · ");
            out << "\n      " << s.streams.value(0) << "\n";
        }
        out << "Add one with: thrum radio add <address> --name <name>\n";
        return 0;
    }

    if (sub == "add") {
        const QString url = o.rest.value(1);
        if (url.isEmpty())
            return fail(o, out, err, "radio add needs a stream address (http or https)");
        Radio::Station s;
        s.name = o.name.isEmpty() ? o.rest.mid(2).join(' ') : o.name;
        s.streams = {url};
        bool existed = false;
        const Radio::Station added = stations.add(s, &error, &existed);
        if (!added.isValid())
            return fail(o, out, err, error);
        if (o.json)
            out << toJson({{"ok", true}, {"station", added.toJson()}, {"existed", existed}}) << Qt::flush;
        else
            out << (existed ? "Already there: " : "Added ") << added.fullName() << " (" << added.key << ")\n";
        return 0;
    }

    if (sub == "remove" || sub == "rm" || sub == "favourite" || sub == "unfavourite") {
        const Radio::Station s = stations.find(arg);
        if (!s.isValid())
            return fail(o, out, err, QString("No station called \"%1\"").arg(arg));
        bool ok = false;
        if (sub == "remove" || sub == "rm")
            ok = stations.remove(s.key, &error);
        else
            ok = stations.setFavourite(s.key, sub == "favourite");
        if (!ok)
            return fail(o, out, err, error.isEmpty() ? QString("Can't change %1").arg(s.fullName()) : error);
        if (o.json)
            out << toJson({{"ok", true}, {"key", s.key}}) << Qt::flush;
        else
            out << (sub == "favourite" ? "Favourite: " : sub == "unfavourite" ? "No longer a favourite: " : "Removed ")
                << s.fullName() << "\n";
        return 0;
    }

    if (sub == "import") {
        if (arg.isEmpty())
            return fail(o, out, err, "radio import needs a .pls or .m3u file");
        const int added = stations.importFile(arg, &error);
        if (added < 0)
            return fail(o, out, err, error);
        if (o.json)
            out << toJson({{"ok", true}, {"added", added}}) << Qt::flush;
        else
            out << "Added " << added << (added == 1 ? " station\n" : " stations\n");
        return 0;
    }

    if (sub == "export") {
        if (arg.isEmpty())
            return fail(o, out, err, "radio export needs a file name (.pls or .m3u8)");
        if (!stations.exportFile(arg, o.group, &error))
            return fail(o, out, err, error);
        if (o.json)
            out << toJson({{"ok", true}, {"file", QFileInfo(arg).absoluteFilePath()}}) << Qt::flush;
        else
            out << "Saved " << QFileInfo(arg).absoluteFilePath() << "\n";
        return 0;
    }

    if (sub == "heard") {
        const QList<QVariantMap> rows = stations.recentlyHeard(o.limit);
        if (o.json) {
            QJsonArray list;
            for (const QVariantMap &m : rows)
                list << QJsonObject::fromVariantMap(m);
            out << toJson({{"ok", true}, {"heard", list}}) << Qt::flush;
            return 0;
        }
        for (const QVariantMap &m : rows)
            out << QDateTime::fromSecsSinceEpoch(m.value("started").toLongLong()).toString("yyyy-MM-dd HH:mm") << "  "
                << m.value("stationName").toString() << "  " << m.value("artist").toString() << " – "
                << m.value("title").toString() << "\n";
        return 0;
    }

    return fail(o, out, err, QString("Unknown radio command \"%1\" (list, play, search, add, remove, favourite, "
                                     "unfavourite, import, export, heard)")
                                 .arg(sub));
}

} // namespace Cli
