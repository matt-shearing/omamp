#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>

// Asks xdg-desktop-portal for music folders, so the picker is the desktop's
// own (and works the same under Flatpak or a sandbox).
//
// The same class picks other things: setup() turns it into a picker for one
// folder (an organise destination) or for picture files (the tag editor's
// artwork), with its own title and button.
class FolderPicker : public QObject {
    Q_OBJECT

public:
    explicit FolderPicker(QObject *parent = nullptr) : QObject(parent) {}
    // directory false picks files; filterName and patterns ("*.jpg") limit them.
    void setup(const QString &title, const QString &acceptLabel, bool directory, bool multiple,
               const QString &filterName = {}, const QStringList &patterns = {}) {
        m_title = title;
        m_acceptLabel = acceptLabel;
        m_directory = directory;
        m_multiple = multiple;
        m_filterName = filterName;
        m_patterns = patterns;
    }
    virtual void pickFolders();

signals:
    void picked(const QStringList &paths);
    void failed(const QString &message);

private slots:
    void onResponse(uint response, const QVariantMap &results);

private:
    QString m_requestPath;
    QString m_title, m_acceptLabel, m_filterName;
    QStringList m_patterns;
    bool m_directory = true, m_multiple = true;
};
