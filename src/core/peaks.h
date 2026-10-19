#pragma once

#include <QByteArray>
#include <QString>

class Library;

// What identifies one version of one file's waveform: the volume it is on,
// its path there, and its size and modification time. A file changed in
// place gets a new key, so stale peaks are never shown; a share mounted at a
// new path keeps its key, since the volume key and relative path don't change.
struct PeaksKey {
    QString volumeKey, relPath;
    qint64 size = 0, mtimeNs = 0;
    bool isValid() const { return !volumeKey.isEmpty() && !relPath.isEmpty(); }
    // SHA-1 of the four fields, as hex: the cache file's name.
    QString id() const;
};

// A local cache of waveform peaks, so drawing a waveform for a track on the
// NAS never reads the file over the network twice. Files live in
// ~/.cache/omamp/peaks/<2 hex>/<id>.peaks (following XDG_CACHE_HOME).
//
// This is the storage half only. Nothing computes peaks yet: that belongs
// with the decoder in the engine, which would call store() after decoding a
// track once (or while playing it) and load() before drawing.
//
// File format: "OMPK", a version byte (1), three reserved bytes, the number
// of source samples per peak (uint32, little endian), the number of peaks
// (uint32), then the peaks as the caller gave them (opaque bytes; the
// intended layout is one min/max pair of int8 per peak and channel).
class PeaksCache {
public:
    explicit PeaksCache(const QString &dir = {});
    QString dir() const { return m_dir; }

    // The key for a library track, or an invalid key with error set (the
    // track is gone, or comes from a Subsonic server, whose waveform would
    // be keyed the same way by song id).
    static PeaksKey keyFor(const Library &library, qint64 trackId, QString *error = nullptr);

    QString pathFor(const PeaksKey &key) const;
    bool contains(const PeaksKey &key) const;
    bool store(const PeaksKey &key, const QByteArray &peaks, quint32 samplesPerPeak, quint32 count,
               QString *error = nullptr);
    bool load(const PeaksKey &key, QByteArray *peaks, quint32 *samplesPerPeak = nullptr, quint32 *count = nullptr) const;
    bool remove(const PeaksKey &key);
    // Deletes the least recently written files until the cache is at most
    // maxBytes. Returns the bytes freed.
    qint64 prune(qint64 maxBytes);
    qint64 sizeOnDisk() const;

private:
    QString m_dir;
};
