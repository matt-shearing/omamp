#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>

class LibraryController;
class QQmlApplicationEngine;
class QQuickWindow;

// Hooks for screenshots and timings, driven by environment variables and
// off unless one is set. They let the README pictures and the performance
// numbers come from the real window, offscreen, without touching anyone's
// screen.
//
//   THRUM_DEMO="expand:artists;node:artist:12;select:3;queue:0;filter:coast;sort:1"
//       steps run once the window is up: open a tree node (by key), choose
//       a node, select a row, add a row to Now Playing (not played), search,
//       sort by a column. Also: action:<id> (any palette action), mask:<text>
//       (organise), tagset:<field>=<value> (the tag editor), audits (count
//       Files to Edit), browse:<pane>:<value> (browse+ adds with
//       Ctrl), grid, info and browser (toggle them), album:<row> (choose a
//       grid tile), current:<row> (move the cursor), palette:<text>,
//       party:<preset> (party mode with the big display), playlist:<id>,
//       set:<objectName>.<property>=<value> (any named item, sheets too),
//       click:<objectName> (press a named button), wait:<ms> (let
//       background work land), spotifysection:<section> and
//       spotifyselect:<rows> ("1-4,+6": choose Spotify rows).
//   THRUM_SCREENSHOT=<file.png>   save the window there, then quit
//   THRUM_PROFILE=1               print the time to the first frame and, in
//       advanced mode, how long a jump to the end, a sort and two searches
//       take to reach the screen, then quit
class DevHooks : public QObject {
    Q_OBJECT
public:
    DevHooks(QQmlApplicationEngine *engine, LibraryController *library);

    // Called first thing in main(), so times count from process start.
    static void markStart();

private:
    void attach(QQuickWindow *window);
    void afterFirstFrame();
    void runDemo();
    void nextProfileStep();
    void finish();

    QQmlApplicationEngine *m_engine;
    LibraryController *m_library;
    QPointer<QQuickWindow> m_window;
    bool m_profile = false, m_firstFrame = true;
    QString m_screenshot;
    QStringList m_steps;
    int m_step = -1;
    QElapsedTimer m_stepTimer;
};
