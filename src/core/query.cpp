#include "query.h"

#include "library.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace Query {

const char *const defaultOrder = "t.sort_artist COLLATE NOCASE, t.album COLLATE NOCASE, t.disc_no, t.track_no, t.rel_path";

QJsonObject Node::toJson() const {
    static const char *kinds[] = {"and", "or", "not", "term"};
    QJsonObject o{{"kind", kinds[kind]}};
    if (kind == Term) {
        if (!field.isEmpty())
            o["field"] = field;
        o["op"] = op;
        if (op != "none" && op != "any")
            o["value"] = value;
        if (op == "..")
            o["value2"] = value2;
        if (quoted)
            o["quoted"] = true;
        if (wordStart)
            o["word_start"] = true;
    } else {
        QJsonArray list;
        for (const Node &child : children)
            list << child.toJson();
        o["children"] = list;
    }
    return o;
}

QString Limit::toString() const {
    switch (kind) {
    case Count:
        return QString::number(value);
    case Duration: {
        const qint64 s = value / 1000;
        QString out;
        if (s >= 3600)
            out += QString::number(s / 3600) + 'h';
        if (s % 3600 >= 60)
            out += QString::number(s % 3600 / 60) + 'm';
        if (s % 60 || out.isEmpty())
            out += QString::number(s % 60) + 's';
        return out;
    }
    case Bytes:
        return QString::number(value / (1024.0 * 1024.0), 'g', 6) + "mb";
    case None:
        break;
    }
    return {};
}

QJsonObject Parsed::toJson() const {
    QJsonObject o{{"where", root.toJson()}};
    QJsonArray keys;
    for (const SortKey &k : sort) {
        QJsonObject key{{"field", k.field}, {"descending", k.descending}};
        if (k.seed >= 0)
            key["seed"] = k.seed;
        keys << key;
    }
    if (!keys.isEmpty())
        o["sort"] = keys;
    if (limit.kind != Limit::None) {
        static const char *kinds[] = {"none", "count", "duration_ms", "bytes"};
        o["limit"] = QJsonObject{{kinds[limit.kind], limit.value}};
    }
    return o;
}

// ---- Fields ------------------------------------------------------------------

const QList<Field> &fields() {
    static const QList<Field> list = {
        {"title", {"ti"}, Field::Text, "title", "title", {}, {}},
        {"artist", {"ar"}, Field::MultiText, "artist", "artist", "artist", {}},
        {"albumartist", {"album_artist", "aa"}, Field::MultiText, "album_artist", "album_artist", "album_artist", {}},
        {"album", {"al"}, Field::Text, "album", "album", {}, {}},
        {"composer", {"co"}, Field::MultiText, "composer", "composer", "composer", {}},
        {"genre", {"ge"}, Field::MultiText, "genre", "genre", "genre", {}},
        {"filename", {"file"}, Field::Text, "file_name", "file_name", {}, {}},
        {"path", {"folder"}, Field::Text, "rel_path", {}, {}, {}},
        {"comment", {}, Field::Text, "comment", {}, {}, {}},
        {"grouping", {}, Field::Text, "grouping", {}, {}, {}},
        {"publisher", {"label"}, Field::Text, "publisher", {}, {}, {}},
        {"isrc", {}, Field::Text, "isrc", {}, {}, {}},
        {"custom1", {}, Field::Text, "custom1", {}, {}, {}},
        {"custom2", {}, Field::Text, "custom2", {}, {}, {}},
        {"custom3", {}, Field::Text, "custom3", {}, {}, {}},
        {"custom4", {}, Field::Text, "custom4", {}, {}, {}},
        {"custom5", {}, Field::Text, "custom5", {}, {}, {}},
        {"year", {"date"}, Field::Number, "year", {}, {}, {}},
        {"originalyear", {"original_year"}, Field::Number, "original_year", {}, {}, {}},
        {"track", {"tracknumber", "tn"}, Field::Number, "track_no", {}, {}, {}},
        {"disc", {"discnumber"}, Field::Number, "disc_no", {}, {}, {}},
        {"bpm", {"tempo"}, Field::Number, "bpm", {}, {}, {}},
        {"bitrate", {}, Field::Number, "bitrate", {}, {}, {}},
        {"samplerate", {"sample_rate"}, Field::Number, "sample_rate", {}, {}, {}},
        {"channels", {}, Field::Number, "channels", {}, {}, {}},
        {"plays", {"playcount", "play_count"}, Field::Number, "play_count", {}, {}, {}},
        {"skips", {"skipcount", "skip_count"}, Field::Number, "skip_count", {}, {}, {}},
        {"rating", {"stars"}, Field::Rating, "rating", {}, {}, {}},
        {"length", {"duration", "time"}, Field::Duration, "duration_ms", {}, {}, {}},
        {"size", {"filesize"}, Field::Bytes, "size", {}, {}, {}},
        {"added", {"dateadded", "date_added"}, Field::Date, "added", {}, {}, {}},
        {"played", {"lastplayed", "last_played"}, Field::Date, "last_played", {}, {}, {}},
        {"modified", {"updated"}, Field::Date, "updated", {}, {}, {}},
        {"type", {"content_type"}, Field::Enum, "content_type", {}, {}, {"music", "audiobook", "podcast", "classical"}},
        {"status", {"availability"}, Field::Enum, "availability", {}, {}, {"online", "offline", "missing"}},
        {"format", {"ext", "codec"}, Field::Enum, "format", {}, {}, {}},
        {"compilation", {}, Field::Number, "compilation", {}, {}, {}},
    };
    return list;
}

const Field *field(const QString &name) {
    const QString key = name.toLower();
    for (const Field &f : fields())
        if (f.name == key || f.aliases.contains(key))
            return &f;
    return nullptr;
}

// ---- Values with units ---------------------------------------------------------

namespace {

int editDistance(const QString &a, const QString &b) {
    QList<int> prev(b.size() + 1), cur(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j)
        prev[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (int j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

QString unknownField(const QString &name) {
    QString best;
    int bestDistance = 3;
    for (const Field &f : fields()) {
        for (const QString &candidate : QStringList{f.name} + f.aliases) {
            const int d = editDistance(name, candidate);
            if (d < bestDistance && candidate.size() > 2) {
                bestDistance = d;
                best = f.name;
            }
        }
    }
    QStringList names;
    for (const Field &known : fields())
        names << known.name;
    if (!best.isEmpty())
        return QString("Unknown field \"%1\"; did you mean %2:?").arg(name, best);
    return QString("Unknown field \"%1\". Fields: %2").arg(name, names.join(", "));
}

// A length: "200" or "200s" (seconds), "3:20", "1:02:03", "10m", "45min",
// "2h", "1h30m", "2.5h". precisionMs is the size of the smallest unit given,
// so length:3m means anything from 3:00 to 3:59.
bool parseDuration(const QString &text, qint64 *ms, qint64 *precisionMs = nullptr) {
    const QString t = text.trimmed().toLower();
    if (t.isEmpty())
        return false;
    if (t.contains(':')) {
        const QStringList parts = t.split(':');
        if (parts.size() > 3)
            return false;
        double seconds = 0;
        for (const QString &part : parts) {
            bool ok = false;
            const double v = part.toDouble(&ok);
            if (!ok || v < 0)
                return false;
            seconds = seconds * 60 + v;
        }
        *ms = qint64(std::llround(seconds * 1000));
        if (precisionMs)
            *precisionMs = 1000;
        return true;
    }
    static const QRegularExpression part(R"(^(\d+(?:\.\d+)?)\s*(hours?|hrs?|h|minutes?|mins?|m|seconds?|secs?|s)?)");
    int at = 0;
    double total = 0;
    qint64 precision = 1000;
    bool any = false;
    while (at < t.size()) {
        const auto m = part.match(t.mid(at));
        if (!m.hasMatch())
            return false;
        const QString unit = m.captured(2);
        qint64 scale = 1000;
        if (unit.startsWith('h'))
            scale = 3600000;
        else if (unit.startsWith('m'))
            scale = 60000;
        // A bare number is seconds, but only on its own: "1h30" is ambiguous.
        if (unit.isEmpty() && (any || at + m.capturedLength() < t.size()))
            return false;
        total += m.captured(1).toDouble() * scale;
        precision = scale;
        any = true;
        at += m.capturedLength();
    }
    *ms = qint64(std::llround(total));
    if (precisionMs)
        *precisionMs = precision;
    return any;
}

// A size: "700mb", "1.5gb", "512k", "1000" (bytes). Binary units, as file
// managers and MediaMonkey count them: 1 MB is 1,048,576 bytes.
bool parseBytes(const QString &text, qint64 *bytes, qint64 *precision = nullptr, bool needUnit = false) {
    static const QRegularExpression re(R"(^(\d+(?:\.\d+)?)\s*(b|k|kb|kib|m|mb|mib|g|gb|gib|t|tb|tib)?$)");
    const auto m = re.match(text.trimmed().toLower());
    if (!m.hasMatch() || (needUnit && m.captured(2).isEmpty()))
        return false;
    const QChar unit = m.captured(2).isEmpty() ? QChar('b') : m.captured(2)[0];
    qint64 scale = 1;
    if (unit == 'k')
        scale = 1024;
    else if (unit == 'm')
        scale = 1024LL * 1024;
    else if (unit == 'g')
        scale = 1024LL * 1024 * 1024;
    else if (unit == 't')
        scale = 1024LL * 1024 * 1024 * 1024;
    *bytes = qint64(std::llround(m.captured(1).toDouble() * double(scale)));
    if (precision)
        *precision = scale;
    return true;
}

// A point in time or a stretch of it, as Unix seconds [start, end).
struct Span {
    qint64 start = 0, end = 0;
    bool relative = false; // an age ("30d"): a point, counted back from now
};

// Dates in the user's local time: "2024", "2024-06", "2024-06-30", "today",
// "yesterday", or an age: "90min", "12h", "30d", "2w", "6m" (months), "1y".
bool parseDate(const QString &text, const QDateTime &now, Span *span) {
    const QString t = text.trimmed().toLower();
    auto day = [&](const QDate &d) {
        span->start = d.startOfDay().toSecsSinceEpoch();
        span->end = d.addDays(1).startOfDay().toSecsSinceEpoch();
        return true;
    };
    if (t == "today")
        return day(now.date());
    if (t == "yesterday")
        return day(now.date().addDays(-1));
    static const QRegularExpression absolute(R"(^(\d{4})(?:-(\d{1,2})(?:-(\d{1,2}))?)?$)");
    if (const auto m = absolute.match(t); m.hasMatch()) {
        const int y = m.captured(1).toInt();
        if (m.captured(2).isEmpty()) {
            span->start = QDate(y, 1, 1).startOfDay().toSecsSinceEpoch();
            span->end = QDate(y + 1, 1, 1).startOfDay().toSecsSinceEpoch();
            return true;
        }
        const int mo = m.captured(2).toInt();
        if (m.captured(3).isEmpty()) {
            const QDate first(y, mo, 1);
            if (!first.isValid())
                return false;
            span->start = first.startOfDay().toSecsSinceEpoch();
            span->end = first.addMonths(1).startOfDay().toSecsSinceEpoch();
            return true;
        }
        const QDate d(y, mo, m.captured(3).toInt());
        return d.isValid() && day(d);
    }
    static const QRegularExpression age(
        R"(^(\d+(?:\.\d+)?)\s*(minutes?|mins?|hours?|hrs?|h|days?|d|weeks?|w|months?|mos|mo|m|years?|yrs?|y)$)");
    const auto m = age.match(t);
    if (!m.hasMatch())
        return false;
    const double n = m.captured(1).toDouble();
    const QString unit = m.captured(2);
    const bool whole = n == std::floor(n);
    QDateTime at;
    if (unit.startsWith("mi"))
        at = now.addSecs(-qint64(n * 60));
    else if (unit.startsWith('h'))
        at = now.addSecs(-qint64(n * 3600));
    else if (unit.startsWith('d'))
        at = now.addSecs(-qint64(n * 86400));
    else if (unit.startsWith('w'))
        at = now.addSecs(-qint64(n * 7 * 86400));
    else if (unit.startsWith('m')) // months: "m" is months in dates, minutes are "min"
        at = whole ? now.addMonths(-int(n)) : now.addSecs(-qint64(n * 30.436875 * 86400));
    else
        at = whole ? now.addYears(-int(n)) : now.addSecs(-qint64(n * 365.2425 * 86400));
    span->start = at.toSecsSinceEpoch();
    span->end = span->start + 1;
    span->relative = true;
    return true;
}

// ---- Parsing ---------------------------------------------------------------

class Parser {
public:
    explicit Parser(const QString &text) : s(text) {}

    Parsed run() {
        Parsed p;
        p.root = parseAnd();
        if (error.isEmpty() && i < s.size())
            fail("Unexpected )");
        if (error.isEmpty())
            hoistClauses(p);
        p.error = error;
        p.position = error.isEmpty() ? -1 : errorAt;
        if (!error.isEmpty())
            p.root = {};
        return p;
    }

private:
    const QString s;
    int i = 0, depth = 0, errorAt = -1;
    QString error;

    bool atEnd() const { return i >= s.size(); }
    QChar peek(int ahead = 0) const { return i + ahead < s.size() ? s[i + ahead] : QChar(); }
    void skip() {
        while (!atEnd() && s[i].isSpace())
            ++i;
    }
    void fail(const QString &why, int at = -1) {
        if (error.isEmpty()) {
            error = why;
            errorAt = at >= 0 ? at : i;
        }
    }
    bool isBoundary(int at) const { return at >= s.size() || s[at].isSpace() || s[at] == '(' || s[at] == ')'; }
    // OR, | and ; (MediaMonkey's) between terms.
    bool atOr() const {
        if (s.mid(i, 2) == "OR" && isBoundary(i + 2))
            return true;
        return (peek() == '|' || peek() == ';') && isBoundary(i + 1);
    }

    Node parseAnd() {
        Node node;
        node.kind = Node::And;
        while (error.isEmpty()) {
            skip();
            if (atEnd() || (peek() == ')' && depth > 0))
                break;
            if (peek() == ')') {
                fail("Unexpected )");
                break;
            }
            node.children << parseOr();
        }
        if (node.children.size() == 1)
            return node.children.first();
        return node;
    }

    Node parseOr() {
        Node first = parseUnary();
        Node node;
        node.kind = Node::Or;
        node.children << first;
        while (error.isEmpty()) {
            const int save = i;
            skip();
            if (!atOr()) {
                i = save;
                break;
            }
            const int at = i;
            i += s[i] == 'O' ? 2 : 1;
            skip();
            if (atEnd() || peek() == ')') {
                fail("OR needs something after it", at);
                break;
            }
            node.children << parseUnary();
        }
        return node.children.size() == 1 ? first : node;
    }

    Node parseUnary() {
        skip();
        // -word and -( ... ) are NOT; a lone - is a word.
        if (peek() == '-' && i + 1 < s.size() && !s[i + 1].isSpace() && s[i + 1] != ')') {
            const int at = i++;
            Node node;
            node.kind = Node::Not;
            node.position = at;
            node.children << parseUnary();
            return node;
        }
        return parsePrimary();
    }

    Node parsePrimary() {
        if (peek() == '(') {
            const int at = i++;
            ++depth;
            Node inner = parseAnd();
            --depth;
            skip();
            if (peek() != ')')
                fail("Missing ) for the ( here", at);
            else
                ++i;
            if (inner.kind == Node::And && inner.children.isEmpty() && error.isEmpty())
                fail("Empty ( )", at);
            return inner;
        }
        if (atOr()) {
            fail("OR needs something before it");
            return {};
        }
        return parseTerm();
    }

    QString readQuoted() {
        const int at = i++; // opening quote
        QString out;
        while (!atEnd() && s[i] != '"') {
            if (s[i] == '\\' && (peek(1) == '"' || peek(1) == '\\'))
                ++i;
            out += s[i++];
        }
        if (atEnd())
            fail("Missing closing quote", at);
        else
            ++i;
        return out;
    }

    QString readBare() {
        const int start = i;
        while (!isBoundary(i))
            ++i;
        return s.mid(start, i - start);
    }

    Node parseTerm() {
        Node node;
        node.kind = Node::Term;
        node.position = i;
        auto done = [&] {
            node.length = i - node.position;
            return node;
        };
        // +word: the start of a word. A lone + is just a word.
        if (peek() == '+' && !isBoundary(i + 1)) {
            ++i;
            node.wordStart = true;
            if (peek() == '"') {
                node.value = readQuoted();
                node.quoted = true;
            } else {
                node.value = readBare();
            }
            return done();
        }
        if (peek() == '"') {
            node.value = readQuoted();
            node.quoted = true;
            return done();
        }
        // A field name starts with a letter, so 3:20 or 12:00 stay words.
        const int start = i;
        if (peek().isLetter()) {
            while (!atEnd() && (s[i].isLetterOrNumber() || s[i] == '_'))
                ++i;
            if (peek() == ':') {
                node.field = s.mid(start, i - start).toLower();
                ++i;
                parseValue(node);
                // Brackets vanish from the tree, so catch these here.
                if (depth > 0 && (node.field == "sort" || node.field == "limit"))
                    fail(QString("%1: applies to the whole query, so it can't go inside ( ), after - or next to OR")
                             .arg(node.field),
                         start);
                return done();
            }
        }
        i = start;
        node.value = readBare();
        return done();
    }

    void parseValue(Node &node) {
        const int valueAt = i;
        for (const char *op : {">=", "<=", ">", "<", "="}) {
            if (s.mid(i, int(strlen(op))) == op) {
                node.op = op;
                i += int(strlen(op));
                break;
            }
        }
        if (peek() == '+' && node.op == ":" && !isBoundary(i + 1)) {
            ++i;
            node.wordStart = true;
        }
        if (peek() == '"') {
            node.value = readQuoted();
            node.quoted = true;
        } else if (peek() == '/' && node.op == ":" && !node.wordStart) {
            const int at = i++;
            const int start = i;
            while (!atEnd() && !(s[i] == '/' && s[i - 1] != '\\'))
                ++i;
            if (atEnd()) {
                fail("Missing closing / in the regular expression", at);
                return;
            }
            node.value = s.mid(start, i - start).replace("\\/", "/");
            node.op = "~";
            ++i;
            const QRegularExpression re("(?i)" + node.value);
            if (!re.isValid())
                fail(QString("Bad regular expression: %1").arg(re.errorString()), start + std::max(0, int(re.patternErrorOffset()) - 4));
        } else {
            node.value = readBare();
            const int dots = node.value.indexOf("..");
            if (node.op == ":" && dots >= 0 && !node.wordStart) {
                node.op = "..";
                node.value2 = node.value.mid(dots + 2);
                node.value = node.value.left(dots);
                if (node.value.isEmpty() && node.value2.isEmpty())
                    fail(QString("%1: needs a value on at least one side of ..").arg(node.field), valueAt);
                return;
            }
            // Unquoted none, any and never test for an empty field; quote
            // them to search for the word.
            const QString word = node.value.toLower();
            if (node.op == ":" && !node.wordStart && (word == "none" || word == "never" || word == "any")) {
                node.op = word == "any" ? "any" : "none";
                node.value.clear();
                return;
            }
        }
        if (node.value.isEmpty() && !node.quoted)
            fail(QString("%1: needs a value").arg(node.field), valueAt);
    }

    // sort: and limit: shape the whole result, so they only make sense at the
    // top level. Take them out of the tree; anywhere else is an error.
    static bool isClause(const Node &n) { return n.kind == Node::Term && (n.field == "sort" || n.field == "limit"); }

    void misplaced(const Node &n) {
        for (const Node &child : n.children) {
            if (isClause(child))
                fail(QString("%1: applies to the whole query, so it can't go inside ( ), after - or next to OR")
                         .arg(child.field),
                     child.position);
            misplaced(child);
        }
    }

    void hoistClauses(Parsed &p) {
        QList<Node> top;
        if (p.root.kind == Node::And)
            top = p.root.children;
        else
            top = {p.root};
        QList<Node> rest;
        for (const Node &n : top) {
            if (!isClause(n)) {
                rest << n;
                continue;
            }
            if (n.field == "sort")
                parseSort(n, &p.sort);
            else
                parseLimit(n, &p.limit);
        }
        for (const Node &n : rest) {
            if (isClause(n))
                continue;
            Node wrapper;
            wrapper.children = {n};
            misplaced(wrapper);
        }
        if (rest.size() == 1)
            p.root = rest.first();
        else {
            p.root = Node();
            p.root.kind = Node::And;
            p.root.children = rest;
        }
    }

    void parseSort(const Node &n, QList<SortKey> *keys) {
        if (n.op != ":" || n.quoted) {
            fail("sort: takes field names, for example sort:-rating,artist", n.position);
            return;
        }
        int at = n.position + 5;
        for (const QString &raw : n.value.split(',')) {
            SortKey key;
            QString name = raw.trimmed().toLower();
            if (name.startsWith('-')) {
                key.descending = true;
                name = name.mid(1);
            } else if (name.startsWith('+')) {
                name = name.mid(1);
            }
            if (name.startsWith("random") || name.startsWith("shuffle")) {
                const QString base = name.section(':', 0, 0);
                if (base == "random" || base == "shuffle")
                    key.field = "random";
                else if (base == "randomalbum" || base == "random_album" || base == "shufflealbums")
                    key.field = "randomalbum";
                if (!key.field.isEmpty() && name.contains(':')) {
                    bool ok = false;
                    key.seed = name.section(':', 1).toLongLong(&ok);
                    if (!ok || key.seed < 0) {
                        fail(QString("sort:%1 needs a whole number as its seed, as in sort:random:42").arg(base), at);
                        return;
                    }
                }
            }
            if (key.field.isEmpty()) {
                const Field *f = field(name);
                if (name.isEmpty() || !f) {
                    fail(name.isEmpty() ? QString("sort: has an empty key")
                                        : QString("sort: %1; or random, randomalbum")
                                              .arg(unknownField(name)),
                         at);
                    return;
                }
                key.field = f->name;
            }
            keys->append(key);
            at += int(raw.size()) + 1;
        }
    }

    void parseLimit(const Node &n, Limit *limit) {
        const int at = n.position + 6;
        if (n.op != ":") {
            fail("limit: takes a count (limit:50), a length (limit:2h) or a size (limit:700mb)", at);
            return;
        }
        if (limit->kind != Limit::None) {
            fail("Only one limit: per query", n.position);
            return;
        }
        static const QRegularExpression count(R"(^\d+$)");
        qint64 v = 0;
        if (count.match(n.value).hasMatch()) {
            bool ok = false;
            v = n.value.toLongLong(&ok);
            if (!ok || v <= 0) {
                fail("limit: needs a number above 0", at);
                return;
            }
            *limit = {Limit::Count, v};
        } else if (parseBytes(n.value, &v, nullptr, true) && !n.value.toLower().endsWith('m')) {
            *limit = {Limit::Bytes, v};
        } else if (parseDuration(n.value, &v)) {
            *limit = {Limit::Duration, v};
        } else {
            fail(QString("limit: takes a count (50), a length (2h, 90m, 1:30:00) or a size (700mb, 4gb), not \"%1\"")
                     .arg(n.value),
                 at);
            return;
        }
        if (v <= 0)
            fail("limit: needs a value above 0", at);
    }
};

} // namespace

Parsed parse(const QString &text) { return Parser(text).run(); }

// ---- Compiling ---------------------------------------------------------------

namespace {

const QStringList anyColumns = {"title", "artist", "album", "album_artist", "composer", "genre", "file_name"};

QString ftsPhrase(const QString &value) { return '"' + QString(value).replace('"', "\"\"") + '"'; }

QString likePattern(const QString &value) {
    QString v = value;
    v.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
    return '%' + v + '%';
}

// The trigram index can only answer three or more characters.
bool indexable(const QString &value) { return value.size() >= 3; }

QString allColumns() {
    QStringList cols;
    for (const QString &c : anyColumns)
        cols << QString("coalesce(t.%1, '')").arg(c);
    return cols.join(" || ' ' || ");
}

class Compiler {
public:
    Compiled out;
    QDateTime now;

    QString node(const Node &n) {
        if (!out.error.isEmpty())
            return "0";
        switch (n.kind) {
        case Node::And:
        case Node::Or: {
            if (n.children.isEmpty())
                return "1";
            QStringList parts;
            for (const Node &child : n.children)
                parts << node(child);
            if (parts.size() == 1)
                return parts.first();
            return '(' + parts.join(n.kind == Node::And ? " AND " : " OR ") + ')';
        }
        case Node::Not:
            // A comparison with an unset value is NULL, and NOT NULL is NULL,
            // so -rating:4 would drop unrated tracks. Count unknown as false.
            return "NOT coalesce(" + node(n.children.value(0)) + ", 0)";
        case Node::Term:
            return term(n);
        }
        return "1";
    }

private:
    const Node *current = nullptr;

    QString bind(const QVariant &value) {
        out.params << value;
        return "?";
    }

    // Binds in order: the arguments of one arg() call are evaluated in an
    // unspecified order, which would swap the parameters.
    QString between(const QString &col, const QVariant &low, const QVariant &high) {
        bind(low);
        bind(high);
        return col + " BETWEEN ? AND ?";
    }

    QString fail(const QString &why) {
        if (out.error.isEmpty()) {
            out.error = why;
            out.position = current ? current->position : -1;
        }
        return "0";
    }

    QString wordStartPattern(const QString &value) {
        return "(?i)(?<![\\p{L}\\p{N}])" + QRegularExpression::escape(value);
    }

    QString term(const Node &n) {
        current = &n;
        if (n.field.isEmpty()) {
            if (n.wordStart) {
                // The index narrows the rows, then the pattern checks the word
                // start. Parameters bind in the order they appear.
                // Too short for the index, LIKE is a cheaper first sieve than the pattern.
                QString first;
                if (indexable(n.value))
                    first = "t.id IN (SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH " + bind(ftsPhrase(n.value)) +
                            ") AND ";
                else
                    first = '(' + allColumns() + ") LIKE " + bind(likePattern(n.value)) + " ESCAPE '\\' AND ";
                return "(" + first + '(' + allColumns() + ") REGEXP " + bind(wordStartPattern(n.value)) + ')';
            }
            if (indexable(n.value))
                return "t.id IN (SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH " + bind(ftsPhrase(n.value)) + ')';
            return '(' + allColumns() + " LIKE " + bind(likePattern(n.value)) + " ESCAPE '\\')";
        }
        const Field *f = field(n.field);
        if (!f)
            return fail(unknownField(n.field));
        if (n.op == "none" || n.op == "any")
            return empty(*f, n.op == "none");
        switch (f->type) {
        case Field::Text:
        case Field::MultiText:
            return text(*f, n);
        case Field::Enum:
            return enumerated(*f, n);
        case Field::Date:
            return date(*f, n);
        default:
            return number(*f, n);
        }
    }

    QString empty(const Field &f, bool none) {
        const QString col = "t." + f.column;
        switch (f.type) {
        case Field::Text:
        case Field::MultiText:
        case Field::Enum:
            return QString(none ? "coalesce(%1, '') = ''" : "coalesce(%1, '') <> ''").arg(col);
        case Field::Rating:
        case Field::Date:
            return QString(none ? "%1 IS NULL" : "%1 IS NOT NULL").arg(col);
        default:
            return QString(none ? "coalesce(%1, 0) = 0" : "coalesce(%1, 0) <> 0").arg(col);
        }
    }

    QString text(const Field &f, const Node &n) {
        const QString col = "t." + f.column;
        if (n.op == "~")
            return QString("coalesce(%1, '') REGEXP %2").arg(col, bind("(?i)" + n.value));
        if (n.op == "=") {
            // An exact match on a multi-value field matches any one value.
            if (f.name == "genre")
                return "t.id IN (SELECT tg.track_id FROM track_genres tg JOIN genres g ON g.id = tg.genre_id "
                       "WHERE g.name = " + bind(n.value) + ')';
            if (!f.role.isEmpty())
                return QString("t.id IN (SELECT ta.track_id FROM track_artists ta JOIN artists a ON a.id = ta.artist_id "
                               "WHERE ta.role = '%1' AND a.name = %2)").arg(f.role, bind(n.value));
            return QString("%1 = %2 COLLATE NOCASE").arg(col, bind(n.value));
        }
        if (n.op != ":")
            return fail(QString("%1: is text; use %1:word, %1:=exact, %1:+start or %1:/regex/").arg(f.name));
        QString fts;
        if (!f.ftsColumn.isEmpty() && indexable(n.value))
            fts = "t.id IN (SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH " +
                  bind(f.ftsColumn + " : " + ftsPhrase(n.value)) + ')';
        if (n.wordStart) {
            const QString re = QString("coalesce(%1, '') REGEXP %2").arg(col, bind(wordStartPattern(n.value)));
            return fts.isEmpty() ? re : '(' + fts + " AND " + re + ')';
        }
        if (!fts.isEmpty())
            return fts;
        return QString("coalesce(%1, '') LIKE %2 ESCAPE '\\'").arg(col, bind(likePattern(n.value)));
    }

    QString enumerated(const Field &f, const Node &n) {
        const QString value = n.value.toLower();
        if (n.op != ":" && n.op != "=")
            return fail(QString("%1: takes a single value").arg(f.name));
        if (!f.values.isEmpty() && !f.values.contains(value))
            return fail(QString("%1: is one of %2").arg(f.name, f.values.join(", ")));
        return QString("t.%1 = %2").arg(f.column, bind(value));
    }

    QString date(const Field &f, const Node &n) {
        const QString col = "t." + f.column;
        auto bad = [&](const QString &v) {
            return fail(QString("%1: needs a date (2024, 2024-06, 2024-06-30, today) or an age (30d, 2w, 6m, 1y), "
                                "not \"%2\"").arg(f.name, v));
        };
        if (n.op == "~" || n.op == "=")
            return fail(QString("%1: is a date; use %1:2024, %1:<30d or %1:2024-01..2024-06").arg(f.name));
        if (n.op == "..") {
            Span a, b;
            const bool hasA = !n.value.isEmpty(), hasB = !n.value2.isEmpty();
            if (hasA && !parseDate(n.value, now, &a))
                return bad(n.value);
            if (hasB && !parseDate(n.value2, now, &b))
                return bad(n.value2);
            // Either side may be a date or an age; the range runs from the
            // earlier moment to the later one, whichever way round they are.
            if (hasA && hasB) {
                const qint64 lo = std::min(a.start, b.start), hi = std::max(a.end, b.end);
                bind(lo);
                bind(hi);
                return QString("(%1 >= ? AND %1 < ?)").arg(col);
            }
            if (hasA)
                return QString("%1 >= %2").arg(col, bind(a.start));
            return QString("%1 < %2").arg(col, bind(b.end));
        }
        Span s;
        if (!parseDate(n.value, now, &s))
            return bad(n.value);
        // An age reads as "how long ago": added:<30d is newer than 30 days.
        // A date reads as a calendar: added:<2024 is before 2024.
        if (s.relative) {
            if (n.op == ":" || n.op == "<")
                return QString("%1 > %2").arg(col, bind(s.start));
            if (n.op == "<=")
                return QString("%1 >= %2").arg(col, bind(s.start));
            if (n.op == ">")
                return QString("%1 < %2").arg(col, bind(s.start));
            return QString("%1 <= %2").arg(col, bind(s.start));
        }
        if (n.op == ":") {
            bind(s.start);
            bind(s.end);
            return QString("(%1 >= ? AND %1 < ?)").arg(col);
        }
        if (n.op == "<")
            return QString("%1 < %2").arg(col, bind(s.start));
        if (n.op == "<=")
            return QString("%1 < %2").arg(col, bind(s.end));
        if (n.op == ">")
            return QString("%1 >= %2").arg(col, bind(s.end));
        return QString("%1 >= %2").arg(col, bind(s.start));
    }

    // Numbers in the units people think in: stars for ratings, m:ss or 10m
    // for lengths, mb for sizes. precision is how much one step of the value
    // as written covers, so length:3m and size:20mb match a whole step.
    bool toNumber(const Field &f, const QString &text, double *value, double *precision) {
        *precision = 1;
        if (f.type == Field::Duration) {
            qint64 ms = 0, p = 0;
            if (!parseDuration(text, &ms, &p))
                return false;
            *value = double(ms);
            *precision = double(p);
            return true;
        }
        if (f.type == Field::Bytes) {
            qint64 b = 0, p = 0;
            if (!parseBytes(text, &b, &p))
                return false;
            *value = double(b);
            *precision = double(p);
            return true;
        }
        bool ok = false;
        double v = text.toDouble(&ok);
        if (!ok)
            return false;
        if (f.type == Field::Rating)
            v *= 20;
        *value = v;
        return true;
    }

    QString number(const Field &f, const Node &n) {
        const QString col = "t." + f.column;
        double a = 0, b = 0, pa = 1, pb = 1;
        auto bad = [&](const QString &v) {
            if (f.type == Field::Duration)
                return fail(QString("%1: needs a length such as 200, 3:20, 10m or 1h30m, not \"%2\"").arg(f.name, v));
            if (f.type == Field::Bytes)
                return fail(QString("%1: needs a size such as 20mb or 1.5gb, not \"%2\"").arg(f.name, v));
            return fail(QString("%1: needs a number, not \"%2\"").arg(f.name, v));
        };
        if (n.op == "~")
            return fail(QString("%1: is a number; regular expressions only work on text").arg(f.name));
        if (n.wordStart)
            return fail(QString("%1: is a number; + only works on text").arg(f.name));
        if (n.op == "..") {
            const bool hasA = !n.value.isEmpty(), hasB = !n.value2.isEmpty();
            if (hasA && !toNumber(f, n.value, &a, &pa))
                return bad(n.value);
            if (hasB && !toNumber(f, n.value2, &b, &pb))
                return bad(n.value2);
            // The top of a range includes its whole step: length:3:00..5:00 takes 5:00.4.
            if (pb > 1)
                b += pb - 1;
            if (hasA && hasB)
                return between(col, a, b);
            return hasA ? QString("%1 >= %2").arg(col, bind(a)) : QString("%1 <= %2").arg(col, bind(b));
        }
        if (!toNumber(f, n.value, &a, &pa))
            return bad(n.value);
        const QString op = n.op == ":" ? "=" : n.op;
        // A rating of 4 stars means 80, but also anything that rounds to 4 stars.
        if (f.type == Field::Rating && op == "=")
            return between(col, a - 10, a + 9);
        if (pa > 1 && op == "=")
            return between(col, a, a + pa - 1);
        return QString("%1 %2 %3").arg(col, op, bind(a));
    }
};

// A shuffle that is the same every time for one seed, in plain SQL so any
// connection can run the compiled query. SQLite has no XOR, so a ^ b is
// written (a | b) - (a & b). Every product stays below 2^63.
QString shuffleKey(const QString &column, qint64 seed) {
    const QString x = QString("(((%1 + %2) * 1597334677) % 4294967296)").arg(column).arg(seed % 2147483648LL);
    const QString mixed = QString("((%1 | (%1 >> 15)) - (%1 & (%1 >> 15)))").arg(x);
    return QString("((%1 * 1103515245 + 12345) % 4294967296)").arg(mixed);
}

QString sortExpression(const SortKey &key, quint32 seed) {
    const qint64 s = key.seed >= 0 ? key.seed : qint64(seed);
    const QString dir = key.descending ? " DESC" : "";
    if (key.field == "random")
        return shuffleKey("t.id", s) + dir;
    if (key.field == "randomalbum")
        return shuffleKey("coalesce(t.album_id, -t.id)", s) + dir + ", t.disc_no, t.track_no";
    const Field *f = field(key.field);
    const bool text = f->type == Field::Text || f->type == Field::MultiText || f->type == Field::Enum;
    // Empty values go last either way round: a list sorted by rating shows
    // the rated tracks first, descending or not.
    if (text)
        return QString("coalesce(t.%1, '') = '', t.%1 COLLATE NOCASE%2").arg(f->column, dir);
    return QString("t.%1%2 NULLS LAST").arg(f->column, dir);
}

} // namespace

QString Compiled::order() const { return orderBy.isEmpty() ? QString(defaultOrder) : orderBy; }

Compiled compile(const Node &root, const Context &context) {
    Compiler c;
    c.now = context.now.isValid() ? context.now : QDateTime::currentDateTime();
    c.out.where = c.node(root);
    if (!c.out.ok()) {
        c.out.where = "0";
        c.out.params.clear();
    }
    return c.out;
}

Compiled compile(const Parsed &parsed, const Context &context) {
    if (!parsed.ok()) {
        Compiled c;
        c.where = "0";
        c.error = parsed.error;
        c.position = parsed.position;
        return c;
    }
    Compiled c = compile(parsed.root, context);
    if (!c.ok())
        return c;
    c.limit = parsed.limit;
    if (!parsed.sort.isEmpty()) {
        QStringList keys;
        for (const SortKey &key : parsed.sort)
            keys << sortExpression(key, context.seed);
        // Ties fall back to the library order, then the id, so a page never
        // shuffles between two runs.
        c.orderBy = keys.join(", ") + ", " + defaultOrder + ", t.id";
    }
    return c;
}

Compiled compile(const QString &text, const Context &context) {
    Compiled c = compile(parse(text), context);
    if (!c.ok() && c.position >= 0)
        c.error = QString("%1 (at character %2)").arg(c.error).arg(c.position + 1);
    return c;
}

// ---- Running -----------------------------------------------------------------

QJsonObject Track::toJson() const {
    QJsonObject o{{"id", id},
                  {"path", path.isEmpty() ? QJsonValue() : QJsonValue(path)},
                  {"title", title},
                  {"artist", artist},
                  {"album_artist", albumArtist},
                  {"album", album},
                  {"genre", genre},
                  {"year", year ? QJsonValue(year) : QJsonValue()},
                  {"disc", disc ? QJsonValue(disc) : QJsonValue()},
                  {"track", track ? QJsonValue(track) : QJsonValue()},
                  {"duration_ms", durationMs},
                  {"size", size},
                  {"rating", rating < 0 ? QJsonValue() : QJsonValue(rating)},
                  {"plays", plays},
                  {"added", added},
                  {"last_played", lastPlayed ? QJsonValue(lastPlayed) : QJsonValue()},
                  {"format", format},
                  {"availability", availability}};
    return o;
}

namespace {

bool prepare(QSqlQuery &q, const QString &sql, const QVariantList &params, QString *error) {
    q.setForwardOnly(true);
    if (!q.prepare(sql)) {
        *error = q.lastError().text();
        return false;
    }
    for (const QVariant &p : params)
        q.addBindValue(p);
    if (!q.exec()) {
        *error = q.lastError().text();
        return false;
    }
    return true;
}

// The ids a limit by length or size keeps: tracks in order until the next
// one would go over. Reading stops there, so with the library order (an
// index) this touches only the rows it keeps.
QList<qint64> fillLimit(Library &library, const Compiled &c, QString *error) {
    QList<qint64> out;
    QSqlQuery q(library.db());
    const QString column = c.limit.kind == Limit::Duration ? "t.duration_ms" : "t.size";
    if (!prepare(q, "SELECT t.id, coalesce(" + column + ", 0) FROM tracks t WHERE " + c.where + " ORDER BY " + c.order(),
                 c.params, error))
        return out;
    qint64 sum = 0;
    while (q.next()) {
        sum += q.value(1).toLongLong();
        if (sum > c.limit.value)
            break;
        out << q.value(0).toLongLong();
    }
    return out;
}

} // namespace

Result run(Library &library, const QString &text, const Options &options) {
    Result result;
    QElapsedTimer timer;
    timer.start();
    const Compiled c = compile(text, options.context);
    if (!c.ok()) {
        result.error = c.error;
        return result;
    }

    const QString columns =
        "SELECT t.id, v.root_path, v.online, t.rel_path, t.title, t.artist, t.album_artist, t.album, "
        "t.genre, t.format, t.availability, t.year, t.disc_no, t.track_no, t.duration_ms, t.rating, "
        "t.play_count, t.size, t.added, t.last_played FROM tracks t JOIN volumes v ON v.id = t.volume_id ";
    QString sql;
    QVariantList params = c.params;
    QList<qint64> filled; // the ids a length or size limit kept, in order
    bool filling = c.limit.kind == Limit::Duration || c.limit.kind == Limit::Bytes;

    if (filling) {
        filled = fillLimit(library, c, &result.error);
        if (!result.ok())
            return result;
        result.total = filled.size();
        if (options.countOnly) {
            result.elapsedUs = timer.nsecsElapsed() / 1000;
            return result;
        }
        const qsizetype from = std::min<qsizetype>(options.offset, filled.size());
        const qsizetype n = options.limit > 0 ? std::min<qsizetype>(options.limit, filled.size() - from) : filled.size() - from;
        QStringList list;
        for (qsizetype k = from; k < from + n; ++k)
            list << QString::number(filled[k]);
        // The ids are integers from the database, so they go in as text.
        sql = columns + "WHERE t.id IN (" + (list.isEmpty() ? QString("NULL") : list.join(',')) + ')';
        params.clear();
    } else {
        if (options.countOnly || (options.countTotal && options.limit > 0)) {
            QSqlQuery count(library.db());
            const QString inner = c.limit.kind == Limit::Count
                                      ? QString("SELECT count(*) FROM (SELECT 1 FROM tracks t WHERE %1 LIMIT %2)")
                                            .arg(c.where)
                                            .arg(c.limit.value)
                                      : "SELECT count(*) FROM tracks t WHERE " + c.where;
            if (!prepare(count, inner, c.params, &result.error))
                return result;
            if (count.next())
                result.total = count.value(0).toLongLong();
            if (options.countOnly) {
                result.elapsedUs = timer.nsecsElapsed() / 1000;
                return result;
            }
        }
        sql = columns + "WHERE " + c.where + " ORDER BY " + c.order();
        qint64 limit = options.limit > 0 ? options.limit : -1;
        if (c.limit.kind == Limit::Count) {
            const qint64 left = std::max<qint64>(0, c.limit.value - options.offset);
            limit = limit < 0 ? left : std::min<qint64>(limit, left);
        }
        if (limit >= 0 || options.offset > 0)
            sql += QString(" LIMIT %1 OFFSET %2").arg(limit).arg(options.offset);
    }

    QSqlQuery q(library.db());
    if (!prepare(q, sql, params, &result.error))
        return result;
    while (q.next()) {
        Track t;
        t.id = q.value(0).toLongLong();
        if (q.value(2).toBool()) {
            const QString root = q.value(1).toString();
            t.path = (root == "/" ? QString() : root) + '/' + q.value(3).toString();
        }
        t.title = q.value(4).toString();
        t.artist = q.value(5).toString();
        t.albumArtist = q.value(6).toString();
        t.album = q.value(7).toString();
        t.genre = q.value(8).toString();
        t.format = q.value(9).toString();
        t.availability = q.value(10).toString();
        t.year = q.value(11).toInt();
        t.disc = q.value(12).toInt();
        t.track = q.value(13).toInt();
        t.durationMs = q.value(14).toLongLong();
        t.rating = q.value(15).isNull() ? -1 : q.value(15).toInt();
        t.plays = q.value(16).toInt();
        t.size = q.value(17).toLongLong();
        t.added = q.value(18).toLongLong();
        t.lastPlayed = q.value(19).toLongLong();
        result.durationMs += t.durationMs;
        result.tracks << t;
    }
    if (filling) {
        // IN (...) comes back in any order; put the rows back in the query's.
        QHash<qint64, int> position;
        for (int k = 0; k < filled.size(); ++k)
            position.insert(filled[k], k);
        std::sort(result.tracks.begin(), result.tracks.end(),
                  [&](const Track &a, const Track &b) { return position.value(a.id) < position.value(b.id); });
    } else if (!(options.countTotal && options.limit > 0)) {
        // No count was run: without a page limit every row after the offset
        // was read, so the rows are the count.
        result.total = result.tracks.size() + (options.limit > 0 ? 0 : options.offset);
    }
    result.elapsedUs = timer.nsecsElapsed() / 1000;
    return result;
}

QList<qint64> ids(Library &library, const QString &text, const Context &context, QString *error) {
    QString failure;
    QList<qint64> out;
    const Compiled c = compile(text, context);
    if (!c.ok()) {
        failure = c.error;
    } else if (c.limit.kind == Limit::Duration || c.limit.kind == Limit::Bytes) {
        out = fillLimit(library, c, &failure);
    } else {
        QSqlQuery q(library.db());
        QString sql = "SELECT t.id FROM tracks t WHERE " + c.where + " ORDER BY " + c.order();
        if (c.limit.kind == Limit::Count)
            sql += QString(" LIMIT %1").arg(c.limit.value);
        if (prepare(q, sql, c.params, &failure))
            while (q.next())
                out << q.value(0).toLongLong();
    }
    if (error)
        *error = failure;
    return out;
}

}
