#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

// Follows the whole Omarchy palette live, not just the accent, so the player
// looks like the rest of the desktop in every theme (Night Ride included).
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QVariantMap c READ colors NOTIFY changed)
    Q_PROPERTY(bool light READ light NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QString accent() const { return m_colors.value("accent").toString(); }
    QString accentForeground() const;
    QVariantMap colors() const { return m_colors; }
    bool light() const { return m_light; }
    int radius() const { return m_radius; }

    static QVariantMap defaults();
    static QVariantMap readColors(const QString &path, bool *light = nullptr);

signals:
    void changed();

private:
    void reload();
    void refreshRounding();

    QString m_directory;
    QVariantMap m_colors = defaults();
    bool m_light = false;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
