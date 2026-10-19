#pragma once

#include <QString>
#include <QStringList>

// Fuzzy comparison of titles, names and lengths, for scoring MusicBrainz and
// LRCLIB candidates against what the files say.
namespace TextMatch {

// Lower case, accents and punctuation removed, "&" read as "and", a leading
// "the " dropped, runs of spaces folded: "The Beatles!" and "beatles" agree.
QString normalise(const QString &text);
// 0 (nothing alike) to 1 (the same after normalising), by edit distance,
// with a bonus when one contains the other ("Song" and "Song (Remastered)").
double similarity(const QString &a, const QString &b);
// 1 within two seconds, falling to 0 at thirty; 0.5 when either is unknown.
double durationSimilarity(qint64 aMs, qint64 bMs);
// Levenshtein distance between two strings.
int distance(const QString &a, const QString &b);

}
