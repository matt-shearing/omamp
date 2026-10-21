#include "peaks.h"

#include "library.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>

QString PeaksKey::id() const {
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(volumeKey.toUtf8());
    h.addData(QByteArray(1, '\0'));
    h.addData(relPath.toUtf8());
    h.addData(QByteArray(1, '\0'));
    h.addData(QByteArray::number(size) + ':' + QByteArray::number(mtimeNs));
    return QString::fromLatin1(h.result().toHex());
}

PeaksCache::PeaksCache(const QString &dir)
    : m_dir(dir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thrum/peaks"
                          : dir) {}

PeaksKey PeaksCache::keyFor(const Library &library, qint64 trackId, QString *error) {
    QSqlQuery q(library.db());
    q.prepare("SELECT v.key, t.rel_path, t.size, t.mtime_ns FROM tracks t JOIN volumes v ON v.id = t.volume_id "
              "WHERE t.id = ?");
    q.addBindValue(trackId);
    if (!q.exec() || !q.next()) {
        if (error)
            *error = QString("Track %1 is not in the library").arg(trackId);
        return {};
    }
    return {q.value(0).toString(), q.value(1).toString(), q.value(2).toLongLong(), q.value(3).toLongLong()};
}

QString PeaksCache::pathFor(const PeaksKey &key) const {
    const QString id = key.id();
    return QString("%1/%2/%3.peaks").arg(m_dir, id.left(2), id);
}

bool PeaksCache::contains(const PeaksKey &key) const { return key.isValid() && QFileInfo::exists(pathFor(key)); }

bool PeaksCache::store(const PeaksKey &key, const QByteArray &peaks, quint32 samplesPerPeak, quint32 count,
                       QString *error) {
    if (!key.isValid()) {
        if (error)
            *error = "No key for these peaks";
        return false;
    }
    const QString path = pathFor(key);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QByteArray header("OMPK\x01\0\0\0", 8);
    char numbers[8];
    qToLittleEndian<quint32>(samplesPerPeak, numbers);
    qToLittleEndian<quint32>(count, numbers + 4);
    header.append(numbers, 8);
    f.write(header);
    f.write(peaks);
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool PeaksCache::load(const PeaksKey &key, QByteArray *peaks, quint32 *samplesPerPeak, quint32 *count) const {
    if (!key.isValid())
        return false;
    QFile f(pathFor(key));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray all = f.readAll();
    if (all.size() < 16 || !all.startsWith("OMPK") || all[4] != 1)
        return false;
    if (samplesPerPeak)
        *samplesPerPeak = qFromLittleEndian<quint32>(all.constData() + 8);
    if (count)
        *count = qFromLittleEndian<quint32>(all.constData() + 12);
    if (peaks)
        *peaks = all.mid(16);
    return true;
}

bool PeaksCache::remove(const PeaksKey &key) { return QFile::remove(pathFor(key)); }

qint64 PeaksCache::sizeOnDisk() const {
    qint64 total = 0;
    QDirIterator it(m_dir, {"*.peaks"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        total += it.nextFileInfo().size();
    return total;
}

qint64 PeaksCache::prune(qint64 maxBytes) {
    QList<QFileInfo> files;
    qint64 total = 0;
    QDirIterator it(m_dir, {"*.peaks"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info = it.nextFileInfo();
        files << info;
        total += info.size();
    }
    std::sort(files.begin(), files.end(),
              [](const QFileInfo &a, const QFileInfo &b) { return a.lastModified() < b.lastModified(); });
    qint64 freed = 0;
    for (const QFileInfo &info : std::as_const(files)) {
        if (total - freed <= maxBytes)
            break;
        if (QFile::remove(info.absoluteFilePath()))
            freed += info.size();
    }
    return freed;
}
