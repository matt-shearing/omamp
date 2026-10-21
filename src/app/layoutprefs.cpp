#include "layoutprefs.h"

#include <QSettings>

LayoutPrefs::LayoutPrefs(const QString &settingsPath, QObject *parent)
    : QObject(parent), m_path(settingsPath), m_sections(allSections()) {
    if (m_path.isEmpty())
        return;
    QSettings s(m_path, QSettings::IniFormat);
    s.beginGroup("layout");
    m_artColumn = s.value("artColumn", m_artColumn).toBool();
    const QString height = s.value("rowHeight", m_rowHeight).toString();
    if (rowHeights().contains(height))
        m_rowHeight = height;
    // Sections from an older or newer Thrum: unknown ones go, new ones join
    // at the end, so the order a person chose survives.
    const QStringList order = s.value("sections").toStringList();
    if (!order.isEmpty()) {
        QStringList known;
        for (const QString &id : order)
            if (allSections().contains(id) && !known.contains(id))
                known << id;
        for (const QString &id : allSections())
            if (!known.contains(id))
                known << id;
        m_sections = known;
    }
    for (const QString &id : s.value("hiddenSections").toStringList())
        if (allSections().contains(id) && !m_hidden.contains(id))
            m_hidden << id;
    for (const QString &id : s.value("collapsedSections").toStringList())
        if (allSections().contains(id) && !m_collapsed.contains(id))
            m_collapsed << id;
    const QString follow = s.value("follow", m_follow).toString();
    if (follow == "selection" || follow == "playing")
        m_follow = follow;
    m_onlineInfo = s.value("onlineInfo", m_onlineInfo).toBool();
    const QString size = s.value("barSize", m_barSize).toString();
    if (size == "compact" || size == "large")
        m_barSize = size;
    const QString position = s.value("barPosition", m_barPosition).toString();
    if (position == "top" || position == "bottom")
        m_barPosition = position;
}

QStringList LayoutPrefs::allSections() {
    return {"cover", "title", "lyrics", "artist", "album", "credits", "upnext", "details"};
}

QString LayoutPrefs::sectionName(const QString &id) const {
    if (id == "cover")
        return tr("Cover");
    if (id == "title")
        return tr("Title, artist and rating");
    if (id == "lyrics")
        return tr("Lyrics");
    if (id == "artist")
        return tr("About the artist");
    if (id == "album")
        return tr("About the album");
    if (id == "credits")
        return tr("Credits");
    if (id == "upnext")
        return tr("Up next");
    if (id == "details")
        return tr("Track details");
    return id;
}

void LayoutPrefs::save(const QString &key, const QVariant &value) {
    if (m_path.isEmpty())
        return;
    QSettings s(m_path, QSettings::IniFormat);
    s.setValue("layout/" + key, value);
}

void LayoutPrefs::saveSections() {
    save("sections", m_sections);
    save("hiddenSections", m_hidden);
    save("collapsedSections", m_collapsed);
    emit sectionsChanged();
}

void LayoutPrefs::setArtColumn(bool on) {
    if (on == m_artColumn)
        return;
    m_artColumn = on;
    save("artColumn", on);
    emit changed();
}

void LayoutPrefs::setRowHeight(const QString &height) {
    if (height == m_rowHeight || !rowHeights().contains(height))
        return;
    m_rowHeight = height;
    save("rowHeight", height);
    emit changed();
}

int LayoutPrefs::rowPixels() const {
    if (m_rowHeight == "large")
        return 44;
    if (m_rowHeight == "comfortable")
        return 32;
    return 24;
}

int LayoutPrefs::artPixels() const { return rowPixels() - 4; }

void LayoutPrefs::setFollow(const QString &follow) {
    if (follow == m_follow || (follow != "selection" && follow != "playing"))
        return;
    m_follow = follow;
    save("follow", follow);
    emit changed();
}

void LayoutPrefs::setOnlineInfo(bool on) {
    if (on == m_onlineInfo)
        return;
    m_onlineInfo = on;
    save("onlineInfo", on);
    emit changed();
}

void LayoutPrefs::setSidebarShown(bool on) {
    if (on == m_sidebarShown)
        return;
    m_sidebarShown = on;
    emit sidebarShownChanged();
}

void LayoutPrefs::setBarSize(const QString &size) {
    if (size == m_barSize || (size != "compact" && size != "large"))
        return;
    m_barSize = size;
    save("barSize", size);
    emit changed();
}

void LayoutPrefs::setBarPosition(const QString &position) {
    if (position == m_barPosition || (position != "top" && position != "bottom"))
        return;
    m_barPosition = position;
    save("barPosition", position);
    emit changed();
}

void LayoutPrefs::setSectionShown(const QString &id, bool on) {
    if (!allSections().contains(id) || on == sectionShown(id))
        return;
    if (on)
        m_hidden.removeAll(id);
    else
        m_hidden << id;
    saveSections();
}

void LayoutPrefs::setCollapsed(const QString &id, bool on) {
    if (!allSections().contains(id) || on == isCollapsed(id))
        return;
    if (on)
        m_collapsed << id;
    else
        m_collapsed.removeAll(id);
    saveSections();
}

void LayoutPrefs::moveSection(const QString &id, int delta) {
    const int from = int(m_sections.indexOf(id));
    const int to = from + delta;
    if (from < 0 || to < 0 || to >= m_sections.size() || delta == 0)
        return;
    m_sections.move(from, to);
    saveSections();
}

void LayoutPrefs::reset() {
    m_artColumn = true;
    m_rowHeight = "compact";
    m_sections = allSections();
    m_hidden.clear();
    m_collapsed.clear();
    m_follow = "selection";
    m_onlineInfo = true;
    m_barSize = "compact";
    m_barPosition = "top";
    if (!m_path.isEmpty()) {
        QSettings s(m_path, QSettings::IniFormat);
        s.remove("layout");
    }
    emit changed();
    emit sectionsChanged();
    emit resetRequested();
}
