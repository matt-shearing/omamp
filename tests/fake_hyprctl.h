#pragma once

// A pretend hyprctl for the window tests (menu_tests, setup_tests).

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QStringList>

// A hyprctl that keeps one client (this process, address 0x5afe, on
// monitor 1) in files beside it, applies the Lua dispatches it is sent to
// that state, logs every batch to `log`, and fails each batch while a file
// called `fail` is there. floating: how the window starts.
inline QString fakeHyprctl(const QString &dir, bool floating, const QString &floatingSize = "1100,760",
                           const QString &floatingAt = "100,200") {
    auto put = [&](const QString &name, const QByteArray &text) {
        QFile f(dir + "/" + name);
        f.open(QIODevice::WriteOnly);
        f.write(text);
    };
    put("floating", floating ? "true\n" : "false\n");
    put("pinned", "false\n");
    put("at", (floating ? floatingAt : QString("12,42")).toUtf8() + "\n");
    put("size", (floating ? floatingSize : QString("1700,1000")).toUtf8() + "\n");
    // Monitor 1 sits right of monitor 0 and scales by 1.5: 2560 x 1440.
    put("monitors.json", R"([{"id":0,"name":"DP-1","x":0,"y":0,"width":6144,"height":2560,"scale":1,"transform":0},
                            {"id":1,"name":"HDMI-A-1","x":6144,"y":0,"width":3840,"height":2160,"scale":1.5,"transform":0,"reserved":[0,32,0,0]}])");
    const QString script = dir + "/hyprctl";
    put("hyprctl", QString(R"SH(#!/bin/sh
d=$(dirname "$0")
st() { tr -d '\n' < "$d/$1"; }
xy() { echo "$1" | sed 's/.*x = \([0-9-]*\), y = \([0-9-]*\).*/\1,\2/'; }
case "$1" in
-j)
  if [ "$2" = monitors ]; then cat "$d/monitors.json"; exit 0; fi
  printf '[{"address":"0x5afe","pid":%1,"title":"Thrum","floating":%s,"pinned":%s,"fullscreen":0,"monitor":1,"at":[%s],"size":[%s]}]\n' \
    "$(st floating)" "$(st pinned)" "$(st at)" "$(st size)" ;;
--batch)
  printf '%s\n' "$2" >> "$d/log"
  if [ -e "$d/fail" ]; then echo ok; echo 'error: [string "x"]:1: attempt to call a nil value'; exit 0; fi
  echo "$2" | tr ';' '\n' | while IFS= read -r c; do
    case "$c" in
      *'float({'*'"on"'*) echo true > "$d/floating" ;;
      *'float({'*'"off"'*) echo false > "$d/floating"; echo 1700,1000 > "$d/size" ;;
      *'pin({'*'"on"'*) echo true > "$d/pinned" ;;
      *'pin({'*'"off"'*) echo false > "$d/pinned" ;;
      *'resize({'*) xy "$c" > "$d/size" ;;
      *'move({'*) xy "$c" > "$d/at" ;;
    esac
    echo ok
  done ;;
esac
)SH").arg(QCoreApplication::applicationPid()).toUtf8());
    QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return script;
}

inline QStringList batches(const QString &dir) {
    QFile f(dir + "/log");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
}
