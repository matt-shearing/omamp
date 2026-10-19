#pragma once

#include <QDate>
#include <QLocale>
#include <QString>

// Dates as the window shows them, in the list, the info panel and the undo
// history. Qt's English short date is the American "7/12/26", which reads
// as December to most of the world; English gets "12 Jul 2026" instead,
// which reads the same in Sydney and in Boston. Other languages get their
// own short date with a four-digit year.
namespace Dates {

inline QString text(const QDate &date, const QLocale &locale = QLocale()) {
    if (!date.isValid())
        return {};
    if (locale.language() == QLocale::English || locale.language() == QLocale::C)
        return locale.toString(date, QStringLiteral("d MMM yyyy"));
    QString format = locale.dateFormat(QLocale::ShortFormat);
    if (!format.contains(QLatin1String("yyyy")))
        format.replace(QLatin1String("yy"), QLatin1String("yyyy"));
    return locale.toString(date, format);
}

} // namespace Dates
