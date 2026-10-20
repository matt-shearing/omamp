#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// The library window's layout choices, from Settings > Appearance > Layout
// and the View menu: the Art column and the row height, the sidebar's
// sections, and the now playing bar's size and place. QML sees it as
// `appearance.layout`. Kept in window.conf under [layout].
//
// The sidebar's own switch and width stay with the window's other panes
// (window/infoVisible and window/infoWidth, as the info panel had them);
// this object mirrors the switch in sidebarShown and asks the window to
// flip it with sidebarToggleRequested(), so Settings can offer it too.
class LayoutPrefs : public QObject {
    Q_OBJECT
    // A small cover first in every track list.
    Q_PROPERTY(bool artColumn READ artColumn WRITE setArtColumn NOTIFY changed)
    // "compact" (24 px rows), "comfortable" (32) or "large" (44).
    Q_PROPERTY(QString rowHeight READ rowHeight WRITE setRowHeight NOTIFY changed)
    Q_PROPERTY(int rowPixels READ rowPixels NOTIFY changed)
    // The thumbnail's side in the Art column, for the row height.
    Q_PROPERTY(int artPixels READ artPixels NOTIFY changed)

    // The sidebar's sections, in order; hidden and folded ones by id.
    Q_PROPERTY(QStringList sections READ sections NOTIFY sectionsChanged)
    Q_PROPERTY(QStringList hiddenSections READ hiddenSections NOTIFY sectionsChanged)
    Q_PROPERTY(QStringList collapsedSections READ collapsedSections NOTIFY sectionsChanged)
    // "selection": the chosen track, else the playing one; "playing": always the playing one.
    Q_PROPERTY(QString follow READ follow WRITE setFollow NOTIFY changed)
    // Look the artist and album up on MusicBrainz, Wikidata and Wikipedia.
    Q_PROPERTY(bool onlineInfo READ onlineInfo WRITE setOnlineInfo NOTIFY changed)
    // Set by the window: whether the sidebar shows.
    Q_PROPERTY(bool sidebarShown READ sidebarShown WRITE setSidebarShown NOTIFY sidebarShownChanged)

    // "compact" (the strip) or "large" (Spotify's bar); "top" or "bottom".
    Q_PROPERTY(QString barSize READ barSize WRITE setBarSize NOTIFY changed)
    Q_PROPERTY(QString barPosition READ barPosition WRITE setBarPosition NOTIFY changed)

public:
    // settingsPath: window.conf; empty keeps everything in memory (tests).
    explicit LayoutPrefs(const QString &settingsPath = {}, QObject *parent = nullptr);

    bool artColumn() const { return m_artColumn; }
    void setArtColumn(bool on);
    QString rowHeight() const { return m_rowHeight; }
    void setRowHeight(const QString &height);
    int rowPixels() const;
    int artPixels() const;

    QStringList sections() const { return m_sections; }
    QStringList hiddenSections() const { return m_hidden; }
    QStringList collapsedSections() const { return m_collapsed; }
    QString follow() const { return m_follow; }
    void setFollow(const QString &follow);
    bool onlineInfo() const { return m_onlineInfo; }
    void setOnlineInfo(bool on);
    bool sidebarShown() const { return m_sidebarShown; }
    void setSidebarShown(bool on);

    QString barSize() const { return m_barSize; }
    void setBarSize(const QString &size);
    QString barPosition() const { return m_barPosition; }
    void setBarPosition(const QString &position);

    // Every section, in the default order.
    static QStringList allSections();
    static QStringList rowHeights() { return {"compact", "comfortable", "large"}; }

    Q_INVOKABLE QString sectionName(const QString &id) const;
    Q_INVOKABLE bool sectionShown(const QString &id) const { return !m_hidden.contains(id); }
    Q_INVOKABLE void setSectionShown(const QString &id, bool on);
    Q_INVOKABLE bool isCollapsed(const QString &id) const { return m_collapsed.contains(id); }
    Q_INVOKABLE void setCollapsed(const QString &id, bool on);
    // Moves a section up (-1) or down (+1) the sidebar.
    Q_INVOKABLE void moveSection(const QString &id, int delta);
    // Asks the window to show or hide the sidebar.
    Q_INVOKABLE void toggleSidebar() { emit sidebarToggleRequested(); }
    // Everything here back to its default, and resetRequested() so the
    // window puts its columns and panes back too.
    Q_INVOKABLE void reset();

signals:
    void changed();
    void sectionsChanged();
    void sidebarShownChanged();
    void sidebarToggleRequested();
    void resetRequested();

private:
    void save(const QString &key, const QVariant &value);
    void saveSections();

    QString m_path;
    bool m_artColumn = true;
    QString m_rowHeight = "compact";
    QStringList m_sections, m_hidden, m_collapsed;
    QString m_follow = "selection";
    bool m_onlineInfo = true;
    bool m_sidebarShown = false;
    QString m_barSize = "compact";
    QString m_barPosition = "top";
};
