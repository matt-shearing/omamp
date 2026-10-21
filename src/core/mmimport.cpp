#include "mmimport.h"

#include "job.h"
#include "library.h"
#include "m3u.h"
#include "playlists.h"
#include "tagwriter.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>

#include <sqlite3.h>

#include <functional>
#include <memory>

namespace MmImport {

namespace {

// ---- Reading MediaMonkey's database ----------------------------------------

// MediaMonkey's IUNICODE: case- and accent-insensitive. Folding case and
// dropping combining marks after decomposition is close enough to read and
// sort with; the importer never writes to the copy.
QString foldForCollation(const void *data, int length) {
    QString s = QString::fromUtf8(static_cast<const char *>(data), length).normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(s.size());
    for (QChar c : s)
        if (c.category() != QChar::Mark_NonSpacing)
            out += c;
    return out.toCaseFolded();
}

int compareIUnicode(void *, int la, const void *a, int lb, const void *b) {
    return foldForCollation(a, la).compare(foldForCollation(b, lb));
}

// NUMERICSTRING sorts "2" before "10": compare leading numbers as numbers,
// then the rest as text.
int compareNumeric(void *, int la, const void *a, int lb, const void *b) {
    const QString sa = QString::fromUtf8(static_cast<const char *>(a), la);
    const QString sb = QString::fromUtf8(static_cast<const char *>(b), lb);
    static const QRegularExpression lead("^\\s*(\\d+)");
    const auto ma = lead.match(sa), mb = lead.match(sb);
    if (ma.hasMatch() && mb.hasMatch()) {
        const qlonglong na = ma.captured(1).toLongLong(), nb = mb.captured(1).toLongLong();
        if (na != nb)
            return na < nb ? -1 : 1;
    }
    return sa.compare(sb, Qt::CaseInsensitive);
}

void collationNeeded(void *, sqlite3 *db, int, const char *name) {
    const QByteArray n(name);
    if (n.compare("NUMERICSTRING", Qt::CaseInsensitive) == 0)
        sqlite3_create_collation(db, name, SQLITE_UTF8, nullptr, compareNumeric);
    else // IUNICODE and anything else MediaMonkey may declare
        sqlite3_create_collation(db, name, SQLITE_UTF8, nullptr, compareIUnicode);
}

class MmDb {
public:
    ~MmDb() {
        if (m_db)
            sqlite3_close(m_db);
    }
    bool open(const QString &path, QString *error) {
        if (sqlite3_open_v2(QFile::encodeName(path).constData(), &m_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            *error = QString("Can't open %1: %2").arg(path, sqlite3_errmsg(m_db));
            return false;
        }
        sqlite3_collation_needed(m_db, nullptr, collationNeeded);
        sqlite3_create_collation(m_db, "IUNICODE", SQLITE_UTF8, nullptr, compareIUnicode);
        sqlite3_create_collation(m_db, "NUMERICSTRING", SQLITE_UTF8, nullptr, compareNumeric);
        // A quick read proves it is a database at all.
        QList<QVariantList> rows;
        if (!select("SELECT count(*) FROM sqlite_master", 1, rows, error))
            return false;
        return true;
    }
    bool hasTable(const QString &table) {
        QList<QVariantList> rows;
        QString error;
        select(QString("SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = '%1' COLLATE NOCASE")
                   .arg(table),
               1, rows, &error);
        return !rows.isEmpty() && rows.first().value(0).toInt() > 0;
    }
    QSet<QString> columns(const QString &table) {
        QSet<QString> out;
        QList<QVariantList> rows;
        QString error;
        if (select(QString("PRAGMA table_info(\"%1\")").arg(table), 2, rows, &error))
            for (const QVariantList &r : rows)
                out.insert(r.value(1).toString().toLower());
        return out;
    }
    // Runs a query and calls `row` with each row's values.
    bool each(const QString &sql, int columns, const std::function<void(const QVariantList &)> &row, QString *error) {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.toUtf8().constData(), -1, &st, nullptr) != SQLITE_OK) {
            *error = QString("%1 (in: %2)").arg(sqlite3_errmsg(m_db), sql.left(200));
            return false;
        }
        int rc;
        QVariantList values;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            values.clear();
            for (int i = 0; i < columns; ++i) {
                switch (sqlite3_column_type(st, i)) {
                case SQLITE_INTEGER:
                    values << qint64(sqlite3_column_int64(st, i));
                    break;
                case SQLITE_FLOAT:
                    values << sqlite3_column_double(st, i);
                    break;
                case SQLITE_NULL:
                    values << QVariant();
                    break;
                default:
                    values << QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(st, i)),
                                                sqlite3_column_bytes(st, i));
                }
            }
            row(values);
        }
        const bool ok = rc == SQLITE_DONE;
        if (!ok)
            *error = QString("%1 (in: %2)").arg(sqlite3_errmsg(m_db), sql.left(200));
        sqlite3_finalize(st);
        return ok;
    }
    bool select(const QString &sql, int columns, QList<QVariantList> &rows, QString *error) {
        return each(sql, columns, [&](const QVariantList &r) { rows << r; }, error);
    }

private:
    sqlite3 *m_db = nullptr;
};

// Copies the database (and a WAL or journal beside it, so the copy is
// consistent) to a temporary folder. MediaMonkey may be running on the
// original; the copy is all this importer ever opens.
QString copyDatabase(const QString &source, const QTemporaryDir &dir, QString *error) {
    if (!QFileInfo(source).isFile()) {
        *error = QString("%1 is not a file").arg(source);
        return {};
    }
    const QString target = dir.filePath(QFileInfo(source).fileName());
    if (!QFile::copy(source, target)) {
        *error = QString("Can't copy %1 to a temporary folder").arg(source);
        return {};
    }
    for (const char *suffix : {"-wal", "-journal"})
        if (QFileInfo::exists(source + suffix))
            QFile::copy(source + suffix, target + suffix);
    QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner);
    return target;
}

QString normalSlashes(QString p) {
    p.replace('\\', '/');
    while (p.size() > 1 && p.endsWith('/'))
        p.chop(1);
    return p;
}

QChar driveLetter(const QVariant &v) {
    if (v.typeId() == QMetaType::QString && v.toString().size() == 1)
        return v.toString().at(0).toUpper();
    const int n = v.toInt();
    if (n >= 0 && n < 26)
        return QChar('A' + n);
    if (n >= 'A' && n <= 'Z')
        return QChar(n);
    if (n >= 'a' && n <= 'z')
        return QChar(n).toUpper();
    return QChar('?');
}

QString textOrNull(const QVariant &v) { return v.isNull() ? QString() : v.toString(); }

QVariant journalValue(const QString &s, bool isNull) {
    return isNull ? QVariant(QMetaType(QMetaType::QString)) : QVariant(s);
}

const QStringList trackFields = {"rating",  "play_count", "skip_count", "last_played", "added",
                                 "custom1", "custom2",    "custom3",    "custom4",     "custom5"};

// The Thrum values a change is measured against, kept up to date as changes
// are planned, so two MediaMonkey rows for one track merge rather than fight.
struct Current {
    QVariant values[10]; // in trackFields order
};

// ---- AutoPlaylist criteria -------------------------------------------------

struct Token {
    enum Kind { Word, String, Number, Op, Open, Close, Comma, Minus, End };
    Kind kind;
    QString text;
};

QList<Token> tokenize(const QString &s, QString *why) {
    QList<Token> out;
    int i = 0;
    while (i < s.size()) {
        const QChar c = s[i];
        if (c.isSpace()) {
            ++i;
        } else if (c == '(') {
            out << Token{Token::Open, "("};
            ++i;
        } else if (c == ')') {
            out << Token{Token::Close, ")"};
            ++i;
        } else if (c == ',') {
            out << Token{Token::Comma, ","};
            ++i;
        } else if (c == '\'' || c == '"') {
            QString text;
            ++i;
            while (i < s.size()) {
                if (s[i] == c) {
                    if (i + 1 < s.size() && s[i + 1] == c) {
                        text += c;
                        i += 2;
                        continue;
                    }
                    break;
                }
                text += s[i++];
            }
            if (i >= s.size()) {
                *why = "a quoted value never ends";
                return {};
            }
            ++i;
            out << Token{Token::String, text};
        } else if (c.isDigit() || (c == '.' && i + 1 < s.size() && s[i + 1].isDigit())) {
            int j = i;
            while (j < s.size() && (s[j].isDigit() || s[j] == '.'))
                ++j;
            out << Token{Token::Number, s.mid(i, j - i)};
            i = j;
        } else if (c == '-') {
            out << Token{Token::Minus, "-"};
            ++i;
        } else if (QString("<>=!").contains(c)) {
            QString op = c;
            if (i + 1 < s.size() && QString("<>=").contains(s[i + 1]))
                op += s[i + 1];
            i += op.size();
            out << Token{Token::Op, op == "!=" ? QString("<>") : op == "==" ? QString("=") : op};
        } else if (c.isLetter() || c == '_' || c == '[') {
            int j = i;
            while (j < s.size() && (s[j].isLetterOrNumber() || s[j] == '_' || s[j] == '.' || s[j] == '[' || s[j] == ']'))
                ++j;
            QString word = s.mid(i, j - i);
            word.remove('[').remove(']');
            out << Token{Token::Word, word};
            i = j;
        } else {
            *why = QString("can't read \"%1\"").arg(c);
            return {};
        }
    }
    out << Token{Token::End, {}};
    return out;
}

struct FieldInfo {
    const char *mm, *thrum;
    enum Type { Text, Number, Rating, Date, LengthMs, Bitrate, Year } type;
};
const FieldInfo knownFields[] = {
    {"songtitle", "title", FieldInfo::Text},       {"title", "title", FieldInfo::Text},
    {"artist", "artist", FieldInfo::Text},         {"albumartist", "albumartist", FieldInfo::Text},
    {"album", "album", FieldInfo::Text},           {"genre", "genre", FieldInfo::Text},
    {"composer", "composer", FieldInfo::Text},     {"comment", "comment", FieldInfo::Text},
    {"publisher", "publisher", FieldInfo::Text},   {"grouping", "grouping", FieldInfo::Text},
    {"custom1", "custom1", FieldInfo::Text},       {"custom2", "custom2", FieldInfo::Text},
    {"custom3", "custom3", FieldInfo::Text},       {"custom4", "custom4", FieldInfo::Text},
    {"custom5", "custom5", FieldInfo::Text},       {"year", "year", FieldInfo::Year},
    {"rating", "rating", FieldInfo::Rating},       {"playcounter", "plays", FieldInfo::Number},
    {"skipcount", "skips", FieldInfo::Number},     {"bpm", "bpm", FieldInfo::Number},
    {"tracknumber", "track", FieldInfo::Number},   {"discnumber", "disc", FieldInfo::Number},
    {"lasttimeplayed", "played", FieldInfo::Date}, {"dateadded", "added", FieldInfo::Date},
    {"songlength", "length", FieldInfo::LengthMs}, {"bitrate", "bitrate", FieldInfo::Bitrate},
};

const FieldInfo *fieldInfo(QString name) {
    name = name.toLower();
    const int dot = name.lastIndexOf('.');
    if (dot >= 0)
        name = name.mid(dot + 1);
    for (const FieldInfo &f : knownFields)
        if (name == QLatin1String(f.mm))
            return &f;
    return nullptr;
}

QString quoteValue(const QString &v) {
    static const QRegularExpression plain("^[^\\s\"()|;:]+$");
    if (plain.match(v).hasMatch() && v.compare("none", Qt::CaseInsensitive) && v.compare("any", Qt::CaseInsensitive) &&
        v.compare("or", Qt::CaseInsensitive))
        return v;
    QString e = v;
    e.replace('"', "\\\"");
    return '"' + e + '"';
}

// True when a translated term holds a space outside quotes and brackets,
// so it is several terms and needs brackets beside an OR.
bool hasTopLevelSpace(const QString &s) {
    int depth = 0;
    bool quoted = false;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (quoted && c == '\\')
            ++i;
        else if (c == '"')
            quoted = !quoted;
        else if (!quoted && c == '(')
            ++depth;
        else if (!quoted && c == ')')
            --depth;
        else if (!quoted && depth == 0 && c == ' ')
            return true;
    }
    return false;
}

class CriteriaParser {
public:
    CriteriaParser(const QList<Token> &tokens) : t(tokens) {}
    QString why;

    // Returns the translation, wrapped in brackets when it holds an OR so
    // the caller can put it beside other terms safely.
    QString expression(bool *isOr = nullptr) {
        QStringList parts;
        bool sawOr = false;
        parts << conjunction();
        while (ok() && isWord("OR")) {
            ++p;
            sawOr = true;
            parts << conjunction();
        }
        if (isOr)
            *isOr = sawOr;
        // In the query language OR binds tighter than a space, so each AND
        // group inside an OR gets brackets.
        if (sawOr) {
            for (QString &part : parts)
                if (hasTopLevelSpace(part))
                    part = '(' + part + ')';
            return parts.join(" OR ");
        }
        return parts.join(' ');
    }
    bool atEnd() const { return t[p].kind == Token::End; }
    bool ok() const { return why.isEmpty(); }

private:
    bool isWord(const char *w) const { return t[p].kind == Token::Word && t[p].text.compare(w, Qt::CaseInsensitive) == 0; }

    QString conjunction() {
        QStringList parts;
        parts << unary();
        while (ok() && isWord("AND")) {
            ++p;
            parts << unary();
        }
        return parts.join(' ');
    }

    QString unary() {
        if (!ok())
            return {};
        if (isWord("NOT")) {
            ++p;
            const QString inner = unary();
            return inner.startsWith('(') ? '-' + inner : "-(" + inner + ')';
        }
        if (t[p].kind == Token::Open) {
            ++p;
            bool isOr = false;
            const QString inner = expression(&isOr);
            if (t[p].kind != Token::Close) {
                why = "a bracket is never closed";
                return {};
            }
            ++p;
            return '(' + inner + ')';
        }
        return condition();
    }

    // A value: a number (maybe negative), a string, or Date()/Now() minus days.
    struct Value {
        bool relative = false; // Date() - days
        double days = 0;
        QString text;
        bool isNumber = false;
        double number = 0;
    };

    bool value(Value *v) {
        if (t[p].kind == Token::Minus && t[p + 1].kind == Token::Number) {
            p += 2;
            v->isNumber = true;
            v->number = -t[p - 1].text.toDouble();
            v->text = t[p - 1].text.prepend('-');
            return true;
        }
        if (t[p].kind == Token::Number) {
            v->isNumber = true;
            v->number = t[p].text.toDouble();
            v->text = t[p].text;
            ++p;
            return true;
        }
        if (t[p].kind == Token::String) {
            v->text = t[p].text;
            bool numeric = false;
            v->number = v->text.toDouble(&numeric);
            v->isNumber = numeric;
            ++p;
            return true;
        }
        if (t[p].kind == Token::Word && (isWord("Date") || isWord("Now") || isWord("CurrentDate")) &&
            t[p + 1].kind == Token::Open && t[p + 2].kind == Token::Close) {
            p += 3;
            v->relative = true;
            if (t[p].kind == Token::Minus && t[p + 1].kind == Token::Number) {
                v->days = t[p + 1].text.toDouble();
                p += 2;
            }
            return true;
        }
        why = QString("expected a value, found \"%1\"").arg(t[p].text);
        return false;
    }

    QString condition() {
        if (t[p].kind != Token::Word) {
            why = QString("expected a field, found \"%1\"").arg(t[p].text);
            return {};
        }
        const QString name = t[p].text;
        const FieldInfo *f = fieldInfo(name);
        if (!f) {
            why = QString("Thrum has no field for MediaMonkey's %1").arg(name);
            return {};
        }
        ++p;
        const QString field = f->thrum;
        // IS [NOT] NULL
        if (isWord("IS")) {
            ++p;
            bool negate = false;
            if (isWord("NOT")) {
                negate = true;
                ++p;
            }
            if (!isWord("NULL")) {
                why = "IS must be followed by NULL or NOT NULL";
                return {};
            }
            ++p;
            return field + ':' + (negate ? "any" : "none");
        }
        bool notLike = false;
        if (isWord("NOT") && t[p + 1].kind == Token::Word && t[p + 1].text.compare("LIKE", Qt::CaseInsensitive) == 0) {
            notLike = true;
            ++p;
        }
        if (isWord("LIKE")) {
            ++p;
            Value v;
            if (!value(&v))
                return {};
            if (f->type != FieldInfo::Text) {
                why = QString("LIKE on %1 is not supported").arg(name);
                return {};
            }
            QString pattern = v.text;
            const bool startAny = pattern.startsWith('%'), endAny = pattern.endsWith('%');
            pattern = pattern.mid(startAny ? 1 : 0);
            if (endAny && !pattern.isEmpty())
                pattern.chop(1);
            if (pattern.contains('%') || pattern.contains('_')) {
                why = QString("the pattern %1 is too complex to translate").arg(v.text);
                return {};
            }
            QString term;
            if (startAny && endAny)
                term = field + ':' + quoteValue(pattern);
            else if (!startAny && !endAny)
                term = field + ":=" + quoteValue(pattern);
            else {
                QString re = QRegularExpression::escape(pattern);
                re.replace('/', "\\/");
                term = field + ":/" + (startAny ? "" : "^") + re + (endAny ? "" : "$") + '/';
            }
            return notLike ? '-' + term : term;
        }
        if (isWord("BETWEEN")) {
            ++p;
            Value a, b;
            if (!value(&a))
                return {};
            if (!isWord("AND")) {
                why = "BETWEEN needs AND";
                return {};
            }
            ++p;
            if (!value(&b))
                return {};
            const QString lo = convert(*f, a), hi = convert(*f, b);
            if (!ok())
                return {};
            return field + ':' + lo + ".." + hi;
        }
        if (t[p].kind != Token::Op) {
            why = QString("expected a comparison after %1").arg(name);
            return {};
        }
        const QString op = t[p].text;
        ++p;
        Value v;
        if (!value(&v))
            return {};
        // Unknown rating: MediaMonkey writes -1.
        if (f->type == FieldInfo::Rating && v.isNumber && v.number < 0) {
            if (op == "=" || op == "<" || op == "<=")
                return "rating:none";
            if (op == "<>" || op == ">" || op == ">=")
                return "rating:any";
        }
        if (f->type == FieldInfo::Date && v.relative) {
            // LastTimePlayed > Date() - 30: within the last 30 days.
            const QString age = QString::number(qMax(0.0, v.days), 'g', 10) + 'd';
            if (op == ">" || op == ">=")
                return field + ":<" + age;
            if (op == "<" || op == "<=")
                return field + ":>" + age;
            why = "a date can only be compared before or after Date() - days";
            return {};
        }
        if (f->type == FieldInfo::Text) {
            if (op == "=")
                return field + ":=" + quoteValue(v.text);
            if (op == "<>")
                return '-' + field + ":=" + quoteValue(v.text);
            why = QString("%1 %2 on text is not supported").arg(name, op);
            return {};
        }
        const QString x = convert(*f, v);
        if (!ok())
            return {};
        if (op == "=")
            return field + ':' + x;
        if (op == "<>")
            return '-' + field + ':' + x;
        return field + ':' + op + x;
    }

    // A MediaMonkey value in the query language's units.
    QString convert(const FieldInfo &f, const Value &v) {
        if (!v.isNumber) {
            why = QString("expected a number, found \"%1\"").arg(v.text);
            return {};
        }
        switch (f.type) {
        case FieldInfo::Rating:
            return QString::number(qBound(0.0, v.number, 100.0) / 20.0, 'g', 4);
        case FieldInfo::Date: {
            const qint64 seconds = oleToUnix(v.number);
            if (!seconds) {
                why = QString("%1 is not a date").arg(v.text);
                return {};
            }
            return QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC).date().toString("yyyy-MM-dd");
        }
        case FieldInfo::LengthMs:
            return QString::number(qint64(v.number / 1000)) + 's';
        case FieldInfo::Bitrate:
            return QString::number(qint64(v.number >= 10000 ? v.number / 1000 : v.number));
        case FieldInfo::Year:
            return QString::number(qint64(v.number > 9999 ? v.number / 10000 : v.number));
        default:
            return QString::number(v.number, 'g', 12);
        }
    }

    QList<Token> t;
    int p = 0;
};

} // namespace

// ---- Public helpers ----------------------------------------------------------

bool PathMap::parse(const QString &text, PathMap *out) {
    const int eq = text.indexOf('=');
    if (eq <= 0 || eq == text.size() - 1)
        return false;
    out->from = text.left(eq).trimmed();
    out->to = text.mid(eq + 1).trimmed();
    return !out->from.isEmpty() && out->to.startsWith('/');
}

QString mapPath(const QString &windowsPath, const QList<PathMap> &maps, bool *mapped) {
    const QString p = normalSlashes(windowsPath);
    const PathMap *best = nullptr;
    int bestLength = -1;
    for (const PathMap &m : maps) {
        const QString from = normalSlashes(m.from);
        const bool fits = p.compare(from, Qt::CaseInsensitive) == 0 ||
                          (p.startsWith(from, Qt::CaseInsensitive) &&
                           (from.endsWith(':') || from.endsWith('/') || p.at(from.size()) == '/'));
        if (fits && from.size() > bestLength) {
            best = &m;
            bestLength = from.size();
        }
    }
    if (mapped)
        *mapped = best != nullptr;
    if (!best)
        return p;
    QString rest = p.mid(bestLength);
    if (!rest.isEmpty() && !rest.startsWith('/'))
        rest.prepend('/');
    QString to = best->to;
    while (to.size() > 1 && to.endsWith('/'))
        to.chop(1);
    return QDir::cleanPath(to + rest);
}

qint64 oleToUnix(double ole) {
    // 25569 is 1 January 1970. Anything before 1980 or past 2200 is a
    // placeholder, not a real play or addition.
    if (!(ole > 29221.0 && ole < 109574.0))
        return 0;
    return qint64((ole - 25569.0) * 86400.0 + 0.5);
}

QString translateCriteria(const QString &criteria, QString *why) {
    QString text = criteria.trimmed();
    QString reason;
    if (text.isEmpty()) {
        if (why)
            *why = "the AutoPlaylist has no criteria";
        return {};
    }
    // INI-style criteria: take the lines that hold conditions, and the
    // sort and limit keys, as documented in docs/mediamonkey-import.md.
    QStringList conditions;
    QString order, limit;
    bool any = false;
    if (text.contains('\n') || text.startsWith('[')) {
        for (QString line : text.split('\n')) {
            line = line.trimmed();
            if (line.isEmpty() || line.startsWith('[') || line.startsWith(';'))
                continue;
            const int eq = line.indexOf('=');
            if (eq <= 0) {
                conditions << line;
                continue;
            }
            const QString key = line.left(eq).trimmed().toLower();
            const QString value = line.mid(eq + 1).trimmed();
            if (key.startsWith("condition") || key == "criteria" || key == "sql" || key == "where")
                conditions << value;
            else if (key == "matchany" || key == "anycondition")
                any = value == "1" || value.compare("true", Qt::CaseInsensitive) == 0;
            else if (key == "matchall")
                any = value == "0" || value.compare("false", Qt::CaseInsensitive) == 0;
            else if (key == "order" || key == "orderby" || key == "sort")
                order = value;
            else if (key == "limit" || key == "maxtracks" || key == "maxfiles")
                limit = value;
        }
        text = conditions.isEmpty() ? QString() : '(' + conditions.join(any ? ") OR (" : ") AND (") + ')';
        if (conditions.size() == 1)
            text = conditions.first();
    }
    // A WHERE clause, with an optional ORDER BY and LIMIT after it.
    static const QRegularExpression where("^\\s*WHERE\\s+", QRegularExpression::CaseInsensitiveOption);
    text.remove(where);
    static const QRegularExpression limitRe("\\s+LIMIT\\s+(\\d+)\\s*$", QRegularExpression::CaseInsensitiveOption);
    if (const auto m = limitRe.match(text); m.hasMatch()) {
        limit = m.captured(1);
        text = text.left(m.capturedStart());
    }
    static const QRegularExpression orderRe("\\s*ORDER\\s+BY\\s+(.+)$", QRegularExpression::CaseInsensitiveOption);
    if (const auto m = orderRe.match(text); m.hasMatch()) {
        order = m.captured(1);
        text = text.left(m.capturedStart());
    }
    QString query;
    if (!text.trimmed().isEmpty()) {
        const QList<Token> tokens = tokenize(text, &reason);
        if (tokens.isEmpty()) {
            if (why)
                *why = reason;
            return {};
        }
        CriteriaParser parser(tokens);
        query = parser.expression();
        if (parser.ok() && !parser.atEnd())
            parser.why = "unexpected text after the conditions";
        if (!parser.ok()) {
            if (why)
                *why = parser.why;
            return {};
        }
    }
    // Sort keys: "Random()", "Rating DESC, Artist".
    QStringList keys;
    for (QString key : order.split(',', Qt::SkipEmptyParts)) {
        key = key.trimmed();
        if (key.startsWith("random", Qt::CaseInsensitive)) {
            keys << "random";
            continue;
        }
        const bool desc = key.endsWith(" desc", Qt::CaseInsensitive);
        key = key.section(' ', 0, 0);
        const FieldInfo *f = fieldInfo(key);
        if (!f) {
            if (why)
                *why = QString("Thrum can't sort by MediaMonkey's %1").arg(key);
            return {};
        }
        keys << (desc ? "-" : "") + QString(f->thrum);
    }
    if (!keys.isEmpty())
        query += (query.isEmpty() ? "" : " ") + QString("sort:") + keys.join(',');
    if (!limit.isEmpty()) {
        bool numeric = false;
        const int n = limit.toInt(&numeric);
        if (numeric && n > 0)
            query += (query.isEmpty() ? "" : " ") + QString("limit:%1").arg(n);
    }
    // The translation must be a valid query, or it isn't one.
    const Query::Compiled compiled = Query::compile(query);
    if (!compiled.ok()) {
        if (why)
            *why = "the translation is not a valid query: " + compiled.error;
        return {};
    }
    return query;
}

// ---- Reports -----------------------------------------------------------------

QJsonObject TrackChange::toJson() const {
    QJsonArray list;
    for (const FieldChange &c : changes)
        list << QJsonObject{{"field", c.field}, {"old", c.oldValue}, {"new", c.newValue}};
    return {{"mm_id", mmId}, {"track_id", trackId}, {"mm_path", mmPath}, {"matched_by", how}, {"changes", list}};
}

QJsonObject PlaylistReport::toJson() const {
    QJsonObject o{{"mm_id", mmId}, {"name", name}, {"kind", kind}, {"entries", entries}, {"matched", matched}};
    if (!parent.isEmpty())
        o["parent"] = parent;
    if (!query.isEmpty())
        o["query"] = query;
    if (!criteria.isEmpty())
        o["criteria"] = criteria;
    if (!reason.isEmpty())
        o["reason"] = reason;
    if (playlistId)
        o["playlist_id"] = playlistId;
    return o;
}

QJsonObject Report::toJson() const {
    QJsonObject byField;
    for (auto it = changesByField.constBegin(); it != changesByField.constEnd(); ++it)
        byField[it.key()] = it.value();
    QJsonArray lists, sample;
    for (const PlaylistReport &p : playlists)
        lists << p.toJson();
    for (const TrackChange &c : changeSample)
        sample << c.toJson();
    QJsonObject o{{"ok", ok},
                  {"applied", applied},
                  {"dry_run", !applied},
                  {"database", database},
                  {"schema", schema},
                  {"missing_columns", QJsonArray::fromStringList(missingColumns)},
                  {"notes", QJsonArray::fromStringList(notes)},
                  {"mm_tracks", mmTracks},
                  {"matched_by_path", matchedByPath},
                  {"matched_by_suffix", matchedBySuffix},
                  {"matched_by_tags", matchedByTags},
                  {"unmatched", unmatched},
                  {"tracks_changed", tracksChanged},
                  {"field_changes", fieldChanges},
                  {"changes_by_field", byField},
                  {"history_rows", historyRows},
                  {"history_duplicates", historyDuplicates},
                  {"playlists", lists},
                  {"unmatched_sample", QJsonArray::fromStringList(unmatchedSample)},
                  {"change_sample", sample},
                  {"suggested_maps", QJsonArray::fromStringList(suggestedMaps)},
                  {"elapsed_ms", elapsedMs}};
    if (!batch.isEmpty())
        o["batch"] = batch;
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

QJsonObject UndoResult::toJson() const {
    QJsonObject o{{"ok", ok},         {"batch", batch},       {"tracks", tracks},
                  {"fields", fields}, {"playlists", playlists}, {"history_rows", historyRows}};
    if (!error.isEmpty())
        o["error"] = error;
    return o;
}

bool isImportBatch(const QString &batch) { return batch.startsWith("mmimport-"); }

// ---- The import --------------------------------------------------------------

Report run(Library &library, const QString &mmDatabase, const Options &options, Job *job) {
    QElapsedTimer timer;
    timer.start();
    Report r;
    r.database = QFileInfo(mmDatabase).absoluteFilePath();
    auto failed = [&](const QString &message) {
        r.ok = false;
        r.error = message;
        r.elapsedMs = timer.elapsed();
        if (job)
            job->fail(message);
        return r;
    };
    if (job)
        job->start();

    QTemporaryDir temp;
    if (!temp.isValid())
        return failed("Can't make a temporary folder for the copy");
    QString error;
    const QString copy = copyDatabase(mmDatabase, temp, &error);
    if (copy.isEmpty())
        return failed(error);
    MmDb mm;
    if (!mm.open(copy, &error))
        return failed(error);
    if (!mm.hasTable("Songs"))
        return failed(QString("%1 has no Songs table; it is not a MediaMonkey database").arg(mmDatabase));

    // ---- What this database has.
    const QSet<QString> songCols = mm.columns("Songs");
    const bool mm5 = mm.hasTable("Collections") || songCols.contains("custom6") ||
                     QFileInfo(mmDatabase).fileName().compare("MM5.DB", Qt::CaseInsensitive) == 0;
    r.schema = mm5 ? "MM5" : "MM4";
    auto col = [&](const char *name) {
        if (songCols.contains(QString(name).toLower()))
            return QString("Songs.") + name;
        r.missingColumns << QString("Songs.") + name;
        return QString("NULL");
    };
    if (!songCols.contains("id") || !songCols.contains("songpath"))
        return failed("The Songs table has no ID or SongPath column; this MediaMonkey schema is not supported");
    const QStringList customCols = {col("Custom1"), col("Custom2"), col("Custom3"), col("Custom4"), col("Custom5")};
    const QString songSql =
        QString("SELECT Songs.ID, Songs.SongPath, %1, %2, %3, %4, %5, %6, %7, %8, %9, %10, %11, ")
            .arg(col("IDMedia"), col("SongTitle"), col("Artist"), col("Album"), col("SongLength"), col("Rating"),
                 col("PlayCounter"), col("SkipCount"), col("LastTimePlayed"))
            .arg(col("DateAdded"), col("AlbumArtist")) +
        customCols.join(", ") + " FROM Songs ORDER BY Songs.ID";

    QHash<qint64, QChar> drives;
    if (mm.hasTable("Medias")) {
        const QSet<QString> mediaCols = mm.columns("Medias");
        if (mediaCols.contains("idmedia") && mediaCols.contains("driveletter"))
            mm.each("SELECT IDMedia, DriveLetter FROM Medias", 2,
                    [&](const QVariantList &v) { drives.insert(v[0].toLongLong(), driveLetter(v[1])); }, &error);
    } else {
        r.notes << "No Medias table: paths that start with ':' can't get their drive letter";
    }

    struct Song {
        qint64 id;
        QString path; // Windows path, rebuilt with its drive letter
        QString title, artist, album, albumArtist;
        qint64 lengthMs;
        QVariant rating, plays, skips, lastPlayed, added;
        QString custom[5];
    };
    QList<Song> songs;
    if (!mm.each(songSql, 18,
                 [&](const QVariantList &v) {
                     Song s;
                     s.id = v[0].toLongLong();
                     s.path = v[1].toString();
                     // MediaMonkey 4 stores local paths without the drive
                     // letter (":\Music\..."); the letter is on the Medias row.
                     if (s.path.startsWith(':'))
                         s.path.prepend(drives.value(v[2].toLongLong(), QChar('?')));
                     s.title = textOrNull(v[3]);
                     s.artist = textOrNull(v[4]);
                     s.album = textOrNull(v[5]);
                     s.lengthMs = v[6].toLongLong();
                     s.rating = v[7];
                     s.plays = v[8];
                     s.skips = v[9];
                     s.lastPlayed = v[10];
                     s.added = v[11];
                     s.albumArtist = textOrNull(v[12]);
                     for (int i = 0; i < 5; ++i)
                         s.custom[i] = textOrNull(v[13 + i]);
                     songs << s;
                 },
                 &error))
        return failed(error);
    r.mmTracks = songs.size();

    // ---- Matching.
    if (job)
        job->setProgress("Matching tracks", 0, songs.size());
    const PlaylistFiles::Matcher matcher(library);
    QHash<qint64, qint64> songToTrack; // MediaMonkey song id -> Thrum track id
    QHash<qint64, QString> songHow;
    QHash<QString, int> mapGuesses;
    for (int i = 0; i < songs.size(); ++i) {
        if (job && job->isCancelled())
            return failed("Cancelled");
        const Song &s = songs[i];
        bool mapped = false;
        const QString linuxPath = mapPath(s.path, options.maps, &mapped);
        qint64 track = 0;
        QString how;
        if (mapped && (track = matcher.byPath(linuxPath)))
            how = "path";
        if (!track && options.matchBySuffix) {
            for (int parts : {3, 2})
                if ((track = matcher.bySuffix(s.path, parts))) {
                    how = "suffix";
                    // Guess the mapping: the parts before the shared tail.
                    const QStringList win = normalSlashes(s.path).split('/');
                    const QStringList lin = matcher.pathOf(track).split('/');
                    int shared = 0;
                    while (shared < win.size() && shared < lin.size() &&
                           win[win.size() - 1 - shared].compare(lin[lin.size() - 1 - shared], Qt::CaseInsensitive) == 0)
                        ++shared;
                    const QString from = win.mid(0, win.size() - shared).join('\\');
                    const QString to = lin.mid(0, lin.size() - shared).join('/');
                    if (!from.isEmpty() && !to.isEmpty())
                        ++mapGuesses[from + '=' + to];
                    break;
                }
        }
        if (!track && options.matchByTags &&
            (track = matcher.byTags(s.artist, s.title, s.lengthMs, s.album, options.durationToleranceMs)))
            how = "tags";
        if (track) {
            songToTrack.insert(s.id, track);
            songHow.insert(s.id, how);
            if (how == "path")
                ++r.matchedByPath;
            else if (how == "suffix")
                ++r.matchedBySuffix;
            else
                ++r.matchedByTags;
        } else {
            ++r.unmatched;
            if (r.unmatchedSample.size() < options.sampleLimit)
                r.unmatchedSample << s.path;
        }
        if (job && i % 500 == 0)
            job->setProgress("Matching tracks", i, songs.size());
    }
    {
        QList<QPair<int, QString>> guesses;
        for (auto it = mapGuesses.constBegin(); it != mapGuesses.constEnd(); ++it)
            guesses << qMakePair(it.value(), it.key());
        std::sort(guesses.begin(), guesses.end(), [](auto &a, auto &b) { return a.first > b.first; });
        for (int i = 0; i < guesses.size() && i < 5; ++i)
            r.suggestedMaps << QString("%1  (%2 tracks)").arg(guesses[i].second).arg(guesses[i].first);
    }

    // ---- Thrum's current values for every matched track.
    QHash<qint64, Current> current;
    {
        QSqlQuery q(library.db());
        q.setForwardOnly(true);
        q.exec("SELECT id, " + trackFields.join(", ") + " FROM tracks");
        const QSet<qint64> wanted(songToTrack.cbegin(), songToTrack.cend());
        while (q.next()) {
            const qint64 id = q.value(0).toLongLong();
            if (!wanted.contains(id))
                continue;
            Current c;
            for (int i = 0; i < trackFields.size(); ++i)
                c.values[i] = q.value(i + 1);
            current.insert(id, c);
        }
    }

    // ---- Plan the field changes.
    struct Planned {
        qint64 trackId;
        QString field;
        QVariant oldValue, newValue;
    };
    QList<Planned> planned;
    QSet<qint64> changedTracks;
    for (const Song &s : songs) {
        const qint64 track = songToTrack.value(s.id);
        if (!track || !current.contains(track))
            continue;
        Current &c = current[track];
        TrackChange change;
        change.mmId = s.id;
        change.trackId = track;
        change.mmPath = s.path;
        change.how = songHow.value(s.id);
        auto propose = [&](int index, const QVariant &value) {
            const QVariant old = c.values[index];
            if (old == value && old.isNull() == value.isNull())
                return;
            if (!old.isNull() && !value.isNull() && old.toString() == value.toString())
                return;
            planned << Planned{track, trackFields[index], old, value};
            change.changes << FieldChange{trackFields[index], old.isNull() ? QString() : old.toString(),
                                          value.toString()};
            ++r.changesByField[trackFields[index]];
            c.values[index] = value;
        };
        if (options.ratings && !s.rating.isNull()) {
            const int rating = s.rating.toInt();
            if (rating >= 0 && (c.values[0].isNull() || options.overwrite))
                propose(0, qBound(0, rating, 100));
        }
        auto mergeCount = [&](int index, const QVariant &mmValue) {
            if (mmValue.isNull())
                return;
            const qint64 n = mmValue.toLongLong();
            const qint64 have = c.values[index].toLongLong();
            if (n < 0)
                return;
            if (n > have || (options.overwrite && n != have))
                propose(index, n);
        };
        if (options.playCounts)
            mergeCount(1, s.plays);
        if (options.skips)
            mergeCount(2, s.skips);
        if (options.lastPlayed && !s.lastPlayed.isNull()) {
            const qint64 when = oleToUnix(s.lastPlayed.toDouble());
            if (when && (c.values[3].isNull() || when > c.values[3].toLongLong() || options.overwrite))
                propose(3, when);
        }
        if (options.dateAdded && !s.added.isNull()) {
            const qint64 when = oleToUnix(s.added.toDouble());
            if (when && (when < c.values[4].toLongLong() || options.overwrite))
                propose(4, when);
        }
        if (options.custom)
            for (int i = 0; i < 5; ++i)
                if (!s.custom[i].trimmed().isEmpty() && (c.values[5 + i].toString().isEmpty() || options.overwrite))
                    propose(5 + i, s.custom[i]);
        if (!change.changes.isEmpty()) {
            changedTracks.insert(track);
            r.fieldChanges += change.changes.size();
            if (r.changeSample.size() < options.sampleLimit)
                r.changeSample << change;
        }
    }
    r.tracksChanged = changedTracks.size();

    // ---- Play history.
    QList<QPair<qint64, qint64>> history; // track, played_at
    if (options.history && mm.hasTable("Played")) {
        const QSet<QString> playedCols = mm.columns("Played");
        if (!playedCols.contains("idsong") || !playedCols.contains("playdate")) {
            r.missingColumns << "Played.IDSong" << "Played.PlayDate";
        } else {
            QSet<QPair<qint64, qint64>> existing;
            QSqlQuery q(library.db());
            q.setForwardOnly(true);
            q.exec("SELECT track_id, played_at FROM play_history");
            while (q.next())
                existing.insert({q.value(0).toLongLong(), q.value(1).toLongLong()});
            mm.each("SELECT IDSong, PlayDate FROM Played ORDER BY PlayDate", 2,
                    [&](const QVariantList &v) {
                        const qint64 track = songToTrack.value(v[0].toLongLong());
                        const qint64 when = oleToUnix(v[1].toDouble());
                        if (!track || !when)
                            return;
                        const QPair<qint64, qint64> key{track, when};
                        if (existing.contains(key)) {
                            ++r.historyDuplicates;
                            return;
                        }
                        existing.insert(key);
                        history << key;
                    },
                    &error);
            r.historyRows = history.size();
        }
    } else if (options.history) {
        r.notes << "No Played table: no play history to import";
    }

    // ---- Playlists.
    struct MmPlaylist {
        qint64 id, parent;
        QString name, criteria;
        bool isAuto;
        QList<qint64> songs;
    };
    QList<MmPlaylist> mmLists;
    const QString sourceTag = "mediamonkey:" + QFileInfo(mmDatabase).fileName() + ':';
    if (options.playlists && mm.hasTable("Playlists")) {
        const QSet<QString> pc = mm.columns("Playlists");
        auto pcol = [&](const char *name) {
            if (pc.contains(QString(name).toLower()))
                return QString(name);
            r.missingColumns << QString("Playlists.") + name;
            return QString("NULL");
        };
        const QString sql = QString("SELECT %1, %2, %3, %4, %5 FROM Playlists")
                                .arg(pcol("IDPlaylist"), pcol("PlaylistName"), pcol("ParentPlaylist"),
                                     pcol("isAutoPlaylist"), pcol("QueryData"));
        QHash<qint64, int> byId;
        mm.each(sql, 5,
                [&](const QVariantList &v) {
                    if (v[0].isNull())
                        return;
                    byId.insert(v[0].toLongLong(), mmLists.size());
                    mmLists << MmPlaylist{v[0].toLongLong(), v[2].toLongLong(), v[1].toString(), textOrNull(v[4]),
                                          v[3].toInt() != 0, {}};
                },
                &error);
        if (mm.hasTable("PlaylistSongs"))
            mm.each("SELECT IDPlaylist, IDSong FROM PlaylistSongs ORDER BY IDPlaylist, SongOrder, IDPlaylistSong", 2,
                    [&](const QVariantList &v) {
                        const auto it = byId.constFind(v[0].toLongLong());
                        if (it != byId.constEnd())
                            mmLists[*it].songs << v[1].toLongLong();
                    },
                    &error);
        // Parents before children; anything whose parent is unknown goes to the top.
        QList<MmPlaylist> ordered;
        QSet<qint64> placed;
        std::function<void(qint64, int)> walk = [&](qint64 parent, int depth) {
            if (depth > 64)
                return;
            QList<MmPlaylist> kids;
            for (const MmPlaylist &p : mmLists)
                if (!placed.contains(p.id) && (p.parent == parent || (parent == 0 && !byId.contains(p.parent))))
                    kids << p;
            std::sort(kids.begin(), kids.end(), [](auto &a, auto &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
            for (const MmPlaylist &k : kids) {
                if (placed.contains(k.id))
                    continue;
                placed.insert(k.id);
                ordered << k;
                walk(k.id, depth + 1);
            }
        };
        walk(0, 0);
        mmLists = ordered;
        QHash<qint64, QString> names;
        QSet<qint64> hasChildren;
        for (const MmPlaylist &p : mmLists) {
            names.insert(p.id, p.name);
            hasChildren.insert(p.parent);
        }
        for (const MmPlaylist &p : mmLists) {
            PlaylistReport pr;
            pr.mmId = p.id;
            pr.name = p.name;
            pr.parent = names.value(p.parent);
            pr.entries = p.songs.size();
            for (qint64 song : p.songs)
                if (songToTrack.contains(song))
                    ++pr.matched;
            if (p.isAuto) {
                pr.criteria = p.criteria;
                QString why;
                pr.query = translateCriteria(p.criteria, &why);
                if (pr.query.isEmpty() && why.isEmpty())
                    why = "the criteria select nothing Thrum can express";
                if (!why.isEmpty()) {
                    pr.kind = "snapshot";
                    pr.reason = why;
                    pr.query.clear();
                } else {
                    pr.kind = "auto";
                }
            } else {
                pr.kind = p.songs.isEmpty() && hasChildren.contains(p.id) ? "folder" : "static";
            }
            r.playlists << pr;
        }
    } else if (options.playlists) {
        r.notes << "No Playlists table: no playlists to import";
    }

    if (!options.apply) {
        r.elapsedMs = timer.elapsed();
        if (job)
            job->finish();
        return r;
    }

    // ---- Apply, in one transaction, journalled.
    if (job)
        job->setProgress("Writing", 0, 0);
    QSqlDatabase db = library.db();
    if (!db.transaction())
        return failed(db.lastError().text());
    auto rollback = [&](const QString &message) {
        db.rollback();
        for (PlaylistReport &p : r.playlists)
            p.playlistId = 0;
        return failed(message);
    };
    r.batch = TagWriter::newBatchId("mmimport");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery journal(db);
    journal.prepare("INSERT INTO import_journal (batch, kind, target_id, field, old_value, new_value, source, "
                    "changed_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    auto record = [&](const QString &kind, qint64 target, const QString &field, const QVariant &old,
                      const QVariant &now_) {
        journal.addBindValue(r.batch);
        journal.addBindValue(kind);
        journal.addBindValue(target);
        journal.addBindValue(field);
        journal.addBindValue(journalValue(old.toString(), old.isNull()));
        journal.addBindValue(journalValue(now_.toString(), now_.isNull()));
        journal.addBindValue(r.database);
        journal.addBindValue(now);
        return journal.exec();
    };
    for (const Planned &p : planned) {
        QSqlQuery u(db);
        u.prepare(QString("UPDATE tracks SET %1 = ? WHERE id = ?").arg(p.field)); // field is from trackFields
        u.addBindValue(p.newValue);
        u.addBindValue(p.trackId);
        if (!u.exec() || !record("track", p.trackId, p.field, p.oldValue, p.newValue))
            return rollback(u.lastError().isValid() ? u.lastError().text() : journal.lastError().text());
    }
    if (!history.isEmpty()) {
        QSqlQuery h(db);
        h.prepare("INSERT INTO play_history (track_id, played_at, skipped, source) VALUES (?, ?, 0, ?)");
        for (const auto &row : history) {
            h.addBindValue(row.first);
            h.addBindValue(row.second);
            h.addBindValue(r.batch);
            if (!h.exec())
                return rollback(h.lastError().text());
        }
        if (!record("history", 0, "rows", {}, qint64(history.size())))
            return rollback(journal.lastError().text());
    }
    if (!r.playlists.isEmpty()) {
        // Skip playlists already imported from this database.
        QSet<QString> already;
        QSqlQuery q(db);
        q.prepare("SELECT source FROM playlists WHERE source LIKE ?");
        q.addBindValue(sourceTag + '%');
        if (q.exec())
            while (q.next())
                already.insert(q.value(0).toString());
        int fresh = 0;
        for (const PlaylistReport &p : r.playlists)
            if (!already.contains(sourceTag + QString::number(p.mmId)))
                ++fresh;
        qint64 top = 0;
        if (fresh) {
            top = Playlists::createFolder(library, options.playlistFolder, 0, &error);
            if (!top)
                return rollback(error);
            Playlists::setNote(library, top, QString("Imported from %1").arg(r.database), sourceTag + "folder");
            if (!record("playlist", top, "created", {}, options.playlistFolder))
                return rollback(journal.lastError().text());
        } else {
            r.notes << "Every playlist was imported from this database before; none were added again";
        }
        QHash<qint64, qint64> created; // MM playlist id -> Thrum playlist id
        QHash<qint64, Song> songById;
        for (const Song &s : songs)
            songById.insert(s.id, s);
        for (int i = 0; i < r.playlists.size(); ++i) {
            PlaylistReport &pr = r.playlists[i];
            const MmPlaylist &mp = mmLists[i];
            if (already.contains(sourceTag + QString::number(pr.mmId)))
                continue;
            const qint64 parent = created.value(mp.parent, top);
            qint64 id = 0;
            if (pr.kind == "auto")
                id = Playlists::createAuto(library, pr.name, pr.query, parent, &error);
            else if (pr.kind == "folder")
                id = Playlists::createFolder(library, pr.name, parent, &error);
            else
                id = Playlists::create(library, pr.name, parent, &error);
            if (!id)
                return rollback(error);
            created.insert(pr.mmId, id);
            pr.playlistId = id;
            QString note;
            if (pr.kind == "snapshot")
                note = QString("A MediaMonkey AutoPlaylist that Thrum could not translate (%1). Its criteria: %2")
                           .arg(pr.reason, pr.criteria);
            else if (pr.kind == "auto")
                note = "MediaMonkey criteria: " + pr.criteria;
            Playlists::setNote(library, id, note, sourceTag + QString::number(pr.mmId));
            if (pr.kind == "static" || pr.kind == "snapshot") {
                QList<Playlists::Entry> entries;
                for (qint64 song : mp.songs) {
                    Playlists::Entry e;
                    const Song s = songById.value(song);
                    e.trackId = songToTrack.value(song);
                    e.path = e.trackId ? matcher.pathOf(e.trackId) : mapPath(s.path, options.maps);
                    e.title = s.title;
                    e.artist = s.artist;
                    e.durationMs = s.lengthMs;
                    entries << e;
                }
                if (!entries.isEmpty() && Playlists::addEntries(library, id, entries, -1, &error) < 0)
                    return rollback(error);
            }
            if (!record("playlist", id, "created", {}, pr.name))
                return rollback(journal.lastError().text());
        }
    }
    // A batch that changed nothing leaves no journal and needs no undo.
    {
        QSqlQuery written(db);
        written.prepare("SELECT count(*) FROM import_journal WHERE batch = ?");
        written.addBindValue(r.batch);
        if (written.exec() && written.next() && written.value(0).toInt() == 0) {
            r.batch.clear();
            r.notes << "Nothing needed changing";
        }
    }
    if (!db.commit())
        return rollback(db.lastError().text());
    r.applied = true;
    r.elapsedMs = timer.elapsed();
    if (job)
        job->finish();
    return r;
}

QString lastBatch(Library &library) {
    QSqlQuery q(library.db());
    q.exec("SELECT batch FROM import_journal GROUP BY batch HAVING min(undone) = 0 ORDER BY max(id) DESC LIMIT 1");
    return q.next() ? q.value(0).toString() : QString();
}

UndoResult undo(Library &library, const QString &batch) {
    UndoResult r;
    r.batch = batch;
    QSqlDatabase db = library.db();
    struct Row {
        qint64 id;
        QString kind;
        qint64 target;
        QString field;
        QVariant oldValue, newValue;
    };
    QList<Row> rows;
    QSqlQuery q(db);
    q.prepare("SELECT id, kind, target_id, field, old_value, new_value FROM import_journal "
              "WHERE batch = ? AND undone = 0 ORDER BY id DESC");
    q.addBindValue(batch);
    if (!q.exec()) {
        r.ok = false;
        r.error = q.lastError().text();
        return r;
    }
    while (q.next())
        rows << Row{q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toLongLong(), q.value(3).toString(),
                    q.value(4), q.value(5)};
    if (rows.isEmpty()) {
        r.ok = false;
        r.error = QString("No import batch %1 to undo").arg(batch);
        return r;
    }
    if (!db.transaction()) {
        r.ok = false;
        r.error = db.lastError().text();
        return r;
    }
    auto fail = [&](const QString &message) {
        db.rollback();
        r.ok = false;
        r.error = message;
        return r;
    };
    QSet<qint64> tracks;
    for (const Row &row : rows) {
        if (row.kind == "track") {
            if (!trackFields.contains(row.field))
                continue;
            QSqlQuery cur(db);
            cur.prepare(QString("SELECT %1 FROM tracks WHERE id = ?").arg(row.field));
            cur.addBindValue(row.target);
            if (!cur.exec() || !cur.next())
                continue; // the track is gone
            const QVariant now = cur.value(0);
            QVariant restore = row.oldValue;
            if (row.field == "play_count" || row.field == "skip_count") {
                // Plays since the import stay: take back only what the import added.
                const qint64 added = row.newValue.toLongLong() - row.oldValue.toLongLong();
                restore = qMax<qint64>(0, now.toLongLong() - added);
            } else if (now.toString() != row.newValue.toString()) {
                continue; // changed since the import; leave the newer value
            }
            QSqlQuery u(db);
            u.prepare(QString("UPDATE tracks SET %1 = ? WHERE id = ?").arg(row.field));
            u.addBindValue(restore.isNull() ? QVariant(QMetaType(QMetaType::LongLong)) : restore);
            u.addBindValue(row.target);
            if (!u.exec())
                return fail(u.lastError().text());
            tracks.insert(row.target);
            ++r.fields;
        } else if (row.kind == "playlist") {
            QSqlQuery d(db);
            d.prepare("DELETE FROM playlists WHERE id = ?");
            d.addBindValue(row.target);
            if (!d.exec())
                return fail(d.lastError().text());
            r.playlists += d.numRowsAffected() > 0 ? 1 : 0;
        } else if (row.kind == "history") {
            QSqlQuery d(db);
            d.prepare("DELETE FROM play_history WHERE source = ?");
            d.addBindValue(batch);
            if (!d.exec())
                return fail(d.lastError().text());
            r.historyRows += d.numRowsAffected();
        }
    }
    QSqlQuery mark(db);
    mark.prepare("UPDATE import_journal SET undone = 1 WHERE batch = ?");
    mark.addBindValue(batch);
    if (!mark.exec())
        return fail(mark.lastError().text());
    if (!db.commit())
        return fail(db.lastError().text());
    r.tracks = tracks.size();
    return r;
}

}
