#include "textmatch.h"

#include <QRegularExpression>
#include <QVector>

namespace TextMatch {

QString normalise(const QString &text) {
    const QString decomposed = text.normalized(QString::NormalizationForm_KD).toLower();
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;
        if (c == '&') {
            out += " and ";
        } else if (c.isLetterOrNumber()) {
            out += c;
        } else {
            out += ' ';
        }
    }
    out = out.simplified();
    if (out.startsWith("the "))
        out = out.mid(4);
    return out;
}

int distance(const QString &a, const QString &b) {
    const int n = int(a.size()), m = int(b.size());
    if (!n)
        return m;
    if (!m)
        return n;
    QVector<int> previous(m + 1), current(m + 1);
    for (int j = 0; j <= m; ++j)
        previous[j] = j;
    for (int i = 1; i <= n; ++i) {
        current[0] = i;
        for (int j = 1; j <= m; ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            current[j] = qMin(qMin(previous[j] + 1, current[j - 1] + 1), previous[j - 1] + cost);
        }
        std::swap(previous, current);
    }
    return previous[m];
}

double similarity(const QString &a, const QString &b) {
    const QString x = normalise(a), y = normalise(b);
    if (x.isEmpty() && y.isEmpty())
        return 0.5;
    if (x.isEmpty() || y.isEmpty())
        return 0;
    if (x == y)
        return 1;
    const double edit = 1.0 - double(distance(x, y)) / qMax(x.size(), y.size());
    // "Song" against "Song (Remastered 2011)": one holds the other whole.
    double contained = 0;
    if (x.contains(y) || y.contains(x))
        contained = 0.85 + 0.1 * double(qMin(x.size(), y.size())) / qMax(x.size(), y.size());
    return qBound(0.0, qMax(edit, contained), 1.0);
}

double durationSimilarity(qint64 aMs, qint64 bMs) {
    if (aMs <= 0 || bMs <= 0)
        return 0.5;
    const double diff = qAbs(aMs - bMs) / 1000.0;
    if (diff <= 2)
        return 1;
    if (diff >= 30)
        return 0;
    return 1.0 - (diff - 2) / 28.0;
}

}
