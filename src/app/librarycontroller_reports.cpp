// Reports from the window (Phase 6): the list on screen as CSV or HTML.
#include "librarycontroller.h"

#include "library.h"
#include "playlistitemsmodel.h"
#include "playlistscontroller.h"
#include "report.h"

#include <QDir>
#include <QFileInfo>

QList<qint64> LibraryController::listTrackIds() const {
    QList<qint64> ids;
    const QString kind = currentKind();
    if (kind == QLatin1String("playlist") || kind == QLatin1String("autoplaylist") || kind == QLatin1String("plfolder")) {
        PlaylistItemsModel *items = m_playlists->items();
        for (int row = 0; row < items->rowCount(); ++row)
            if (const qint64 id = items->trackId(row))
                ids << id;
        return ids;
    }
    const QVector<qint64> all = m_tracks.ids();
    return QList<qint64>(all.begin(), all.end());
}

void LibraryController::exportList(const QString &format) {
    if (!m_ready)
        return;
    if (listTrackIds().isEmpty()) {
        emit toast(tr("There are no tracks in this list to save"));
        return;
    }
    m_reportFormat = format.toLower() == QLatin1String("html") ? QString("html") : QString("csv");
    m_reportPicker.pickFolders();
}

int LibraryController::exportListTo(const QString &file, const QString &format) {
    if (!m_ready)
        return -1;
    if (!m_sandbox.isEmpty() && !QFileInfo(file).absoluteFilePath().startsWith(QDir(m_sandbox).absolutePath() + '/')) {
        emit toast(tr("Can't write outside %1").arg(m_sandbox));
        return -1;
    }
    Report::Options options;
    options.format = format.toLower();
    options.title = currentLabel();
    const Report::Output out = Report::write(*m_library, listTrackIds(), options, file);
    if (!out.ok()) {
        emit toast(out.error);
        return -1;
    }
    emit toast(tr("Saved %n tracks to %1", "", out.tracks).arg(QFileInfo(file).fileName()));
    return out.tracks;
}
