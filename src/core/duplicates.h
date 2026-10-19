#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

class Job;
class Library;

// Finding the same song twice. Three ways, from cheapest to surest:
//
// - Tags: the same artist and title once case, accents and punctuation are
//   ignored, and (by default) lengths within two seconds of each other.
// - Content: the same bytes, by the quick hash the scanner keeps (size plus
//   64 KiB from the middle of the file). Finds copies, not re-encodes.
// - Fingerprint: the same audio by Chromaprint, so an MP3 and a FLAC of one
//   recording match. Needs fingerprints, which Fingerprinter computes in the
//   background with the fpcalc tool and keeps in the fingerprints table.
//
// Each group suggests a track to keep (online first, then lossless, then
// the highest bitrate, the most plays, the best rating, the oldest in the
// library). Nothing is ever deleted: that is always the user's decision.
namespace Duplicates {

enum class By { Tags, Content, Fingerprint };
QString name(By by);
bool parse(const QString &text, By *by);

struct Options {
    By by = By::Tags;
    bool matchLength = true;       // tags: lengths must be close
    int lengthToleranceMs = 2000;
    bool includeMissing = false;   // also group tracks whose files are gone
    double minSimilarity = 0.85;   // fingerprint: share of matching bits, 0..1
};

struct Track {
    qint64 id = 0;
    QString path, title, artist, album, format, availability;
    int bitrate = 0, plays = 0, rating = -1;
    qint64 durationMs = 0, size = 0;
    bool keeper = false;
    QJsonObject toJson() const;
};

struct Group {
    QString key;
    QList<Track> tracks; // the keeper first
    qint64 keeperId = 0;
    QString reason;      // why that one: "lossless (flac)", "highest bitrate (320 kbps)"
    QJsonObject toJson() const;
};

struct Report {
    bool ok = true;
    QString error;
    By by = By::Tags;
    QList<Group> groups;
    int tracks = 0;              // tracks in all groups
    int withoutFingerprint = 0;  // fingerprint: online tracks not fingerprinted yet
    qint64 elapsedMs = 0;
    QJsonObject toJson() const;
};

Report find(Library &library, const Options &options = {});

// "The Beatles", "beatles" and "Beatlés!" all give "beatles".
QString normalise(const QString &text);
// Orders a group: the suggested keeper first. Fills keeper and reason.
void chooseKeeper(Group &group);
// Share of matching bits between two raw Chromaprint fingerprints at the
// best small offset, 0..1.
double similarity(const QList<quint32> &a, const QList<quint32> &b);

}

// Computes Chromaprint fingerprints with the fpcalc command (chromaprint),
// a few files at a time, for online tracks that have none or whose file
// changed. Runs on the calling thread; the window gives it a worker thread
// and its own Library. Never plays anything: fpcalc decodes to memory.
class Fingerprinter {
public:
    explicit Fingerprinter(Library &library, Job *job = nullptr);

    // fpcalc on PATH, or empty when chromaprint is not installed.
    static QString findProgram();
    void setProgram(const QString &path) { m_program = path; }
    void setParallel(int processes) { m_parallel = qMax(1, processes); }
    void setSandbox(const QString &folder) { m_sandbox = folder; }

    struct Result {
        bool ok = true;
        QString error;
        int total = 0, computed = 0, failed = 0;
        bool cancelled = false;
        qint64 elapsedMs = 0;
        QJsonObject toJson() const;
    };
    Result run();

private:
    Library &m_library;
    Job *m_job;
    QString m_program, m_sandbox;
    int m_parallel = 4;
};
