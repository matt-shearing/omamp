#include "devhooks.h"

#include "librarycontroller.h"

#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTextStream>
#include <QTimer>

static QElapsedTimer processTimer;

void DevHooks::markStart() { processTimer.start(); }

static double sinceStartMs() { return processTimer.isValid() ? processTimer.nsecsElapsed() / 1e6 : 0; }

static void say(const QString &line) {
    QTextStream err(stderr);
    err << "omamp: " << line << Qt::endl;
}

DevHooks::DevHooks(QQmlApplicationEngine *engine, LibraryController *library)
    : QObject(engine), m_engine(engine), m_library(library) {
    m_profile = qEnvironmentVariableIntValue("OMAMP_PROFILE") > 0;
    m_screenshot = qEnvironmentVariable("OMAMP_SCREENSHOT");
    if (!m_profile && m_screenshot.isEmpty() && qEnvironmentVariableIsEmpty("OMAMP_DEMO"))
        return;
    connect(engine, &QQmlApplicationEngine::objectCreated, this, [this](QObject *object) {
        if (auto *window = qobject_cast<QQuickWindow *>(object))
            attach(window);
    });
}

void DevHooks::attach(QQuickWindow *window) {
    m_window = window;
    connect(window, &QQuickWindow::frameSwapped, this, [this] {
        if (m_firstFrame) {
            m_firstFrame = false;
            if (m_profile)
                say(QString("first frame %1 ms after start").arg(sinceStartMs(), 0, 'f', 1));
            // Let the first frame finish before changing anything.
            QTimer::singleShot(0, this, &DevHooks::afterFirstFrame);
            return;
        }
        if (m_step >= 0 && m_stepTimer.isValid()) {
            const double ms = m_stepTimer.nsecsElapsed() / 1e6;
            m_stepTimer.invalidate();
            say(QString("%1: %2 ms to the screen").arg(m_steps.value(m_step)).arg(ms, 0, 'f', 1));
            QTimer::singleShot(50, this, &DevHooks::nextProfileStep);
        }
    });
}

void DevHooks::afterFirstFrame() {
    runDemo();
    const bool advanced = m_window && m_window->property("advanced").toBool();
    if (m_profile) {
        if (advanced && m_library)
            say(QString("library: %1 tracks listed in %2 ms")
                    .arg(m_library->tracks()->count())
                    .arg(m_library->tracks()->lastQueryMs(), 0, 'f', 1));
        // Started on the deck: time the switch into advanced mode first.
        m_steps = {advanced ? "already in advanced mode" : "Tab into advanced mode", "jump to the last row", "sort by title", "sort by title, descending",
                   "search: coast", "search: go (two letters, no index)",
                   "search: artist:\"neon coast\" year:1997..2001", "clear the search"};
        m_step = -1;
        QTimer::singleShot(200, this, &DevHooks::nextProfileStep);
        return;
    }
    QTimer::singleShot(qEnvironmentVariableIntValue("OMAMP_SCREENSHOT_DELAY") > 0
                           ? qEnvironmentVariableIntValue("OMAMP_SCREENSHOT_DELAY")
                           : 800,
                       this, &DevHooks::finish);
}

void DevHooks::runDemo() {
    if (!m_library)
        return;
    const QStringList steps = qEnvironmentVariable("OMAMP_DEMO").split(';', Qt::SkipEmptyParts);
    for (const QString &step : steps) {
        const QString verb = step.section(':', 0, 0), arg = step.section(':', 1);
        LibraryTree *tree = m_library->tree();
        TrackListModel *tracks = m_library->tracks();
        if (verb == "expand")
            tree->expand(tree->rowOfKey(arg));
        else if (verb == "node")
            m_library->selectNode(tree->rowOfKey(arg));
        else if (verb == "select")
            tracks->select(arg.toInt(), Qt::ControlModifier);
        else if (verb == "queue") {
            tracks->clearSelection();
            tracks->select(arg.toInt());
            m_library->appendSelection();
            m_library->dismissMessage();
            tracks->clearSelection();
        } else if (verb == "filter")
            tracks->setFilter(arg);
        else if (verb == "sort")
            tracks->setSort(arg.toInt(), false);
        else if (verb == "help")
            m_window->setProperty("helpVisible", true);
        else if (verb == "browse" || verb == "browse+") {
            // browse:<pane>:<label> chooses a value; browse+ adds it (Ctrl).
            const int pane = arg.section(':', 0, 0).toInt();
            const QString label = arg.section(':', 1);
            FacetModel *facet = m_library->browser()->pane(pane);
            int row = -1;
            for (int i = 1; facet && i < facet->count(); ++i)
                if (facet->label(i) == label)
                    row = i;
            if (row < 0)
                say("no browser value " + arg);
            else
                m_library->browser()->select(pane, row, verb == "browse+" ? Qt::ControlModifier : 0);
        } else if (verb == "grid" || verb == "info" || verb == "browser") {
            const char *method = verb == "grid" ? "toggleGrid" : verb == "info" ? "toggleInfo" : "toggleBrowser";
            QMetaObject::invokeMethod(m_window, method);
        } else if (verb == "album")
            m_library->albumGrid()->setCurrentRow(arg.toInt());
        else if (verb == "palette") {
            if (QObject *palette = m_window->findChild<QObject *>("commandPalette"))
                QMetaObject::invokeMethod(palette, "openWith", Q_ARG(QVariant, arg));
        } else if (verb == "current")
            tracks->setCurrentRow(arg.toInt());
        else
            say("unknown demo step " + step);
    }
}

void DevHooks::nextProfileStep() {
    ++m_step;
    if (m_step >= m_steps.size()) {
        finish();
        return;
    }
    TrackListModel *tracks = m_library->tracks();
    m_stepTimer.start();
    switch (m_step) {
    case 0:
        if (m_window->property("advanced").toBool()) {
            m_stepTimer.invalidate();
            QTimer::singleShot(0, this, &DevHooks::nextProfileStep);
            return;
        }
        QMetaObject::invokeMethod(m_window, "setAdvanced", Q_ARG(QVariant, true));
        break;
    case 1: {
        QObject *view = m_window->findChild<QObject *>("advancedView");
        if (view)
            QMetaObject::invokeMethod(view, "jumpToEnd");
        break;
    }
    case 2: tracks->setSort(TrackListModel::Title, false); break;
    case 3: tracks->setSort(TrackListModel::Title, true); break;
    case 4: tracks->setFilter("coast"); break;
    case 5: tracks->setFilter("go"); break;
    case 6: tracks->setFilter("artist:\"neon coast\" year:1997..2001"); break;
    case 7: tracks->setFilter(""); break;
    }
    say(QString("  (%1: query %2 ms, %3 rows)").arg(m_steps[m_step]).arg(tracks->lastQueryMs(), 0, 'f', 1).arg(tracks->count()));
    m_window->update();
}

// Peak resident memory, from the kernel.
static QString peakMemory() {
    QFile status("/proc/self/status");
    if (!status.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : status.readAll().split('\n'))
        if (line.startsWith("VmHWM:"))
            return QString::fromLatin1(line.mid(6).trimmed());
    return {};
}

void DevHooks::finish() {
    if (m_profile)
        say("peak memory " + peakMemory());
    if (!m_screenshot.isEmpty() && m_window) {
        const QImage image = m_window->grabWindow();
        if (image.save(m_screenshot))
            say("saved " + m_screenshot);
        else
            say("could not save " + m_screenshot);
    }
    if (m_profile || !m_screenshot.isEmpty())
        QCoreApplication::quit();
}
