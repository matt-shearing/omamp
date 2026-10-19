#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>

// Asks xdg-desktop-portal for music folders, so the picker is the desktop's
// own (and works the same under Flatpak or a sandbox).
class FolderPicker : public QObject {
    Q_OBJECT

public:
    explicit FolderPicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual void pickFolders();

signals:
    void picked(const QStringList &paths);
    void failed(const QString &message);

private slots:
    void onResponse(uint response, const QVariantMap &results);

private:
    QString m_requestPath;
};
