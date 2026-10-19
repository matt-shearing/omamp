// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this] { m_roundingTimeout.stop(); });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    m_roundingPoll.setInterval(1000);
    connect(&m_roundingPoll, &QTimer::timeout, this, &Theme::refreshRounding);
    if (!m_hyprctl.isEmpty())
        m_roundingPoll.start();
    reload();
}

Theme::~Theme() {
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty() || m_roundingProcess.state() != QProcess::NotRunning)
        return;
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

// A dark palette in Omarchy's own default shape, used when there is no theme.
QVariantMap Theme::defaults() {
    return {
        {"accent", "#FFD60A"},          {"background", "#1a1b26"},
        {"dark_background", "#16161e"}, {"darker_background", "#0f0f14"},
        {"lighter_background", "#24283b"}, {"selection", "#33467c"},
        {"foreground", "#c0caf5"},      {"bright_foreground", "#ffffff"},
        {"light_foreground", "#a9b1d6"}, {"muted", "#565f89"},
        {"red", "#f7768e"},             {"yellow", "#e0af68"},
        {"green", "#9ece6a"},           {"cyan", "#7dcfff"},
        {"blue", "#7aa2f7"},            {"magenta", "#bb9af7"},
    };
}

// Every `name = "#rrggbb"` line in colors.toml, over the defaults, so a theme
// that leaves a colour out still gets a sensible one.
QVariantMap Theme::readColors(const QString &path, bool *light) {
    QVariantMap colors = defaults();
    if (light)
        *light = false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return colors;
    const QString text = QString::fromUtf8(file.readAll());
    static const QRegularExpression line(R"re(^\s*([a-z_]+)\s*=\s*["'](#[0-9a-fA-F]{6})["'])re",
                                         QRegularExpression::MultilineOption);
    for (auto it = line.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        colors.insert(match.captured(1), match.captured(2));
    }
    static const QRegularExpression mode(R"re(^\s*mode\s*=\s*["']light["'])re", QRegularExpression::MultilineOption);
    if (light)
        *light = mode.match(text).hasMatch();
    return colors;
}

QString Theme::listSelection() const {
    const QColor selection(m_colors.value("selection").toString());
    const QColor background(m_colors.value("background").toString());
    const QColor lighter(m_colors.value("lighter_background").toString());
    auto distance = [](const QColor &a, const QColor &b) {
        return std::sqrt(std::pow(a.red() - b.red(), 2) + std::pow(a.green() - b.green(), 2) +
                         std::pow(a.blue() - b.blue(), 2));
    };
    if (distance(selection, lighter) >= 28 && distance(selection, background) >= 28)
        return selection.name();
    const QColor accent(this->accent());
    const double t = 0.3;
    return QColor::fromRgbF(background.redF() * (1 - t) + accent.redF() * t,
                            background.greenF() * (1 - t) + accent.greenF() * t,
                            background.blueF() * (1 - t) + accent.blueF() * t)
        .name();
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QString Theme::accentForeground() const {
    const QColor c(accent());
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance = .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    return luminance > .179 ? "black" : "white";
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    bool light = false;
    const auto colors = readColors(m_directory + "/theme/colors.toml", &light);
    if (colors != m_colors || light != m_light) {
        m_colors = colors;
        m_light = light;
        emit changed();
    }
}
