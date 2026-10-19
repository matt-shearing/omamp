#include "playlist.h"

#include <QCollator>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>

QVariant Playlist::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_tracks.size())
        return {};
    const Track &t = m_tracks.at(index.row());
    switch (role) {
    case PathRole: return t.path;
    case TitleRole: return t.title;
    case ArtistRole: return t.artist;
    case DurationRole: return t.durationMs;
    case NumberRole: return index.row() + 1;
    case Qt::DisplayRole: return t.artist.isEmpty() ? t.title : t.artist + " - " + t.title;
    }
    return {};
}

QHash<int, QByteArray> Playlist::roleNames() const {
    return {{PathRole, "path"}, {TitleRole, "title"}, {ArtistRole, "artist"},
            {DurationRole, "duration"}, {NumberRole, "number"}, {Qt::DisplayRole, "display"}};
}

qint64 Playlist::totalKnownMs() const {
    qint64 total = 0;
    for (const auto &t : m_tracks)
        total += t.durationMs;
    return total;
}

QStringList Playlist::paths() const {
    QStringList out;
    for (const auto &t : m_tracks)
        out << t.path;
    return out;
}

bool Playlist::isAudio(const QString &path) {
    static const QStringList extensions{"mp3", "flac", "ogg", "oga", "opus", "m4a", "aac", "wav",
                                        "wma", "aiff", "aif", "ape", "wv", "mka", "alac"};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

// "03 - Artist - Song_Name.mp3" reads as artist "Artist", title "Song Name".
Playlist::Track Playlist::trackFromFile(const QString &path) {
    Track track;
    track.path = path;
    QString name = QFileInfo(path).completeBaseName();
    name.replace('_', ' ');
    static const QRegularExpression number(R"(^\s*\d{1,3}\s*[-.)]?\s+)");
    name.remove(number);
    const int dash = name.indexOf(" - ");
    if (dash > 0) {
        track.artist = name.left(dash).trimmed();
        track.title = name.mid(dash + 3).trimmed();
    } else {
        track.title = name.trimmed();
    }
    return track;
}

int Playlist::addPaths(const QStringList &paths) {
    QStringList files;
    for (const QString &raw : paths) {
        const QFileInfo info(raw);
        if (info.isDir()) {
            QStringList found;
            QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::Readable,
                            QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
            while (it.hasNext()) {
                const QString file = it.next();
                if (isAudio(file))
                    found << file;
            }
            QCollator collator;
            collator.setNumericMode(true);
            std::sort(found.begin(), found.end(), collator);
            files << found;
        } else if (info.isFile() && isAudio(raw)) {
            files << info.absoluteFilePath();
        }
    }
    if (files.isEmpty())
        return 0;
    beginInsertRows({}, m_tracks.size(), m_tracks.size() + files.size() - 1);
    for (const auto &file : files)
        m_tracks << trackFromFile(file);
    endInsertRows();
    emit countChanged();
    return files.size();
}

int Playlist::insertTracks(int row, const QList<Track> &tracks) {
    if (tracks.isEmpty())
        return 0;
    row = qBound(0, row, int(m_tracks.size()));
    beginInsertRows({}, row, row + tracks.size() - 1);
    for (int i = 0; i < tracks.size(); ++i)
        m_tracks.insert(row + i, tracks[i]);
    endInsertRows();
    if (row + tracks.size() < m_tracks.size())
        emit dataChanged(index(row + tracks.size()), index(m_tracks.size() - 1), {NumberRole});
    emit countChanged();
    return tracks.size();
}

void Playlist::replaceTracks(const QList<Track> &tracks) {
    beginResetModel();
    m_tracks = tracks;
    endResetModel();
    emit countChanged();
}

void Playlist::remove(int row) {
    if (row < 0 || row >= m_tracks.size())
        return;
    beginRemoveRows({}, row, row);
    m_tracks.removeAt(row);
    endRemoveRows();
    // Every row after it has a new number.
    if (row < m_tracks.size())
        emit dataChanged(index(row), index(m_tracks.size() - 1), {NumberRole});
    emit countChanged();
}

void Playlist::clear() {
    beginResetModel();
    m_tracks.clear();
    endResetModel();
    emit countChanged();
}

void Playlist::move(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= m_tracks.size() || to >= m_tracks.size())
        return;
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    m_tracks.move(from, to);
    endMoveRows();
    emit dataChanged(index(qMin(from, to)), index(qMax(from, to)), {NumberRole});
}

void Playlist::updateTags(int row, const QString &title, const QString &artist, qint64 durationMs) {
    if (row < 0 || row >= m_tracks.size())
        return;
    Track &t = m_tracks[row];
    if (!title.isEmpty())
        t.title = title;
    if (!artist.isEmpty())
        t.artist = artist;
    if (durationMs > 0)
        t.durationMs = durationMs;
    emit dataChanged(index(row), index(row));
    emit countChanged();
}
