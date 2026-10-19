#pragma once

#include "http.h"

#include <QList>
#include <QString>
#include <QStringList>

class Library;

// AcoustID: from a Chromaprint fingerprint to MusicBrainz recordings.
//
// Lookups need an application key (the "client" parameter). Omamp has none
// registered yet, so the key comes from the setting acoustid.key or the
// environment variable OMAMP_ACOUSTID_KEY; without one, identification
// says so and carries on with the MusicBrainz search alone. AcoustID is free
// for non-commercial use only, at most three requests a second. Omamp never
// submits fingerprints.
namespace AcoustId {

// The application key, or empty. The environment wins over the setting.
QString applicationKey(const Library *library);
// Why identification can't use AcoustID, in a sentence, or empty.
QString missingKeyMessage();

struct Fingerprint {
    bool ok = false;
    QString fingerprint; // fpcalc's compressed, base64 form, as lookups take it
    int durationSeconds = 0;
    QString error;
};
// Runs fpcalc (Chromaprint) on the first two minutes of a file. program
// empty finds fpcalc on PATH.
Fingerprint compute(const QString &path, const QString &program = {});

struct Match {
    double score = 0; // 0 to 1, AcoustID's confidence in the fingerprint
    QString acoustId, recordingId, title, artist;
    qint64 lengthMs = 0;
    QStringList releaseIds, releaseGroupIds;
};

struct Result {
    bool ok = false;
    bool missingKey = false;
    bool offline = false;
    QString error;
    QList<Match> matches; // best first
};

Result lookup(Net::HttpClient &http, const QString &key, const QString &fingerprint, int durationSeconds);

}
