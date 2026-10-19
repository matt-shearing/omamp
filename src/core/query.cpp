#include "query.h"

#include "library.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QSqlError>
#include <QSqlQuery>

namespace Query {

QJsonObject Node::toJson() const {
    static const char *kinds[] = {"and", "or", "not", "term"};
    QJsonObject o{{"kind", kinds[kind]}};
    if (kind == Term) {
        if (!field.isEmpty())
            o["field"] = field;
        o["op"] = op;
        o["value"] = value;
        if (op == "..")
            o["value2"] = value2;
        if (quoted)
            o["quoted"] = true;
    } else {
        QJsonArray list;
        for (const Node &child : children)
            list << child.toJson();
        o["children"] = list;
    }
    return o;
}

// ---- Parsing ---------------------------------------------------------------

namespace {

class Parser {
public:
    explicit Parser(const QString &text) : s(text) {}

    Parsed run() {
        Parsed p;
        p.root = parseAnd();
        if (error.isEmpty() && i < s.size())
            fail("Unexpected )");
        p.error = error;
        p.position = error.isEmpty() ? -1 : errorAt;
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
    void fail(const QString &why) {
        if (error.isEmpty()) {
            error = why;
            errorAt = i;
        }
    }
    bool isBoundary(int at) const { return at >= s.size() || s[at].isSpace() || s[at] == '(' || s[at] == ')'; }
    bool atOr() const {
        if (s.mid(i, 2) == "OR" && isBoundary(i + 2))
            return true;
        return peek() == '|' && isBoundary(i + 1);
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
            i += peek() == '|' ? 1 : 2;
            skip();
            if (atEnd() || peek() == ')') {
                fail("OR needs something after it");
                break;
            }
            node.children << parseUnary();
        }
        return node.children.size() == 1 ? first : node;
    }

    Node parseUnary() {
        skip();
        if (peek() == '-' && !isBoundary(i + 1)) {
            ++i;
            Node node;
            node.kind = Node::Not;
            node.children << parseUnary();
            return node;
        }
        return parsePrimary();
    }

    Node parsePrimary() {
        if (peek() == '(') {
            ++i;
            ++depth;
            Node inner = parseAnd();
            --depth;
            skip();
            if (peek() != ')')
                fail("Missing )");
            else
                ++i;
            return inner;
        }
        if (atOr()) {
            fail("OR needs something before it");
            return {};
        }
        return parseTerm();
    }

    QString readQuoted() {
        ++i; // opening quote
        QString out;
        while (!atEnd() && s[i] != '"') {
            if (s[i] == '\\' && (peek(1) == '"' || peek(1) == '\\'))
                ++i;
            out += s[i++];
        }
        if (atEnd())
            fail("Missing closing quote");
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
        if (peek() == '"') {
            node.value = readQuoted();
            node.quoted = true;
            return node;
        }
        const int start = i;
        while (!atEnd() && (s[i].isLetterOrNumber() || s[i] == '_'))
            ++i;
        if (i > start && peek() == ':') {
            node.field = s.mid(start, i - start).toLower();
            ++i;
            parseValue(node);
            return node;
        }
        i = start;
        node.value = readBare();
        return node;
    }

    void parseValue(Node &node) {
        for (const char *op : {">=", "<=", ">", "<", "="}) {
            if (s.mid(i, int(strlen(op))) == op) {
                node.op = op;
                i += int(strlen(op));
                break;
            }
        }
        if (peek() == '"') {
            node.value = readQuoted();
            node.quoted = true;
        } else if (peek() == '/' && node.op == ":") {
            ++i;
            const int start = i;
            while (!atEnd() && !(s[i] == '/' && s[i - 1] != '\\'))
                ++i;
            if (atEnd()) {
                fail("Missing closing / in the regular expression");
                return;
            }
            node.value = s.mid(start, i - start).replace("\\/", "/");
            node.op = "~";
            ++i;
        } else {
            node.value = readBare();
            const int dots = node.value.indexOf("..");
            if (node.op == ":" && dots >= 0) {
                node.op = "..";
                node.value2 = node.value.mid(dots + 2);
                node.value = node.value.left(dots);
                if (node.value.isEmpty() && node.value2.isEmpty())
                    fail(QString("%1: needs a value on at least one side of ..").arg(node.field));
                return;
            }
        }
        if (node.value.isEmpty() && !node.quoted)
            fail(QString("%1: needs a value").arg(node.field));
    }
};

} // namespace

Parsed parse(const QString &text) { return Parser(text).run(); }

// ---- Fields ------------------------------------------------------------------

const QList<Field> &fields() {
    static const QList<Field> list = {
        {"title", {}, Field::Text, "title", "title", {}, {}},
        {"artist", {}, Field::MultiText, "artist", "artist", "artist", {}},
        {"albumartist", {"album_artist", "aa"}, Field::MultiText, "album_artist", "album_artist", "album_artist", {}},
        {"album", {}, Field::Text, "album", "album", {}, {}},
        {"composer", {}, Field::MultiText, "composer", "composer", "composer", {}},
        {"genre", {}, Field::MultiText, "genre", "genre", "genre", {}},
        {"filename", {"file"}, Field::Text, "file_name", "file_name", {}, {}},
        {"path", {}, Field::Text, "rel_path", {}, {}, {}},
        {"comment", {}, Field::Text, "comment", {}, {}, {}},
        {"grouping", {}, Field::Text, "grouping", {}, {}, {}},
        {"publisher", {"label"}, Field::Text, "publisher", {}, {}, {}},
        {"custom1", {}, Field::Text, "custom1", {}, {}, {}},
        {"custom2", {}, Field::Text, "custom2", {}, {}, {}},
        {"custom3", {}, Field::Text, "custom3", {}, {}, {}},
        {"custom4", {}, Field::Text, "custom4", {}, {}, {}},
        {"custom5", {}, Field::Text, "custom5", {}, {}, {}},
        {"year", {}, Field::Number, "year", {}, {}, {}},
        {"originalyear", {"original_year"}, Field::Number, "original_year", {}, {}, {}},
        {"track", {"tracknumber"}, Field::Number, "track_no", {}, {}, {}},
        {"disc", {"discnumber"}, Field::Number, "disc_no", {}, {}, {}},
        {"bpm", {}, Field::Number, "bpm", {}, {}, {}},
        {"bitrate", {}, Field::Number, "bitrate", {}, {}, {}},
        {"samplerate", {"sample_rate"}, Field::Number, "sample_rate", {}, {}, {}},
        {"channels", {}, Field::Number, "channels", {}, {}, {}},
        {"plays", {"playcount", "play_count"}, Field::Number, "play_count", {}, {}, {}},
        {"skips", {"skipcount", "skip_count"}, Field::Number, "skip_count", {}, {}, {}},
        {"rating", {"stars"}, Field::Rating, "rating", {}, {}, {}},
        {"length", {"duration", "time"}, Field::Duration, "duration_ms", {}, {}, {}},
        {"type", {"content_type"}, Field::Enum, "content_type", {}, {}, {"music", "audiobook", "podcast", "classical"}},
        {"status", {"availability"}, Field::Enum, "availability", {}, {}, {"online", "offline", "missing"}},
        {"format", {"ext", "codec"}, Field::Enum, "format", {}, {}, {}},
        {"compilation", {}, Field::Number, "compilation", {}, {}, {}},
    };
    return list;
}

const Field *field(const QString &name) {
    for (const Field &f : fields())
        if (f.name == name || f.aliases.contains(name))
            return &f;
    return nullptr;
}

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

class Compiler {
public:
    Compiled out;

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
            return "NOT " + node(n.children.value(0));
        case Node::Term:
            return term(n);
        }
        return "1";
    }

private:
    QString bind(const QVariant &value) {
        out.params << value;
        return "?";
    }

    // Binds in order: the arguments of one arg() call are evaluated in an
    // unspecified order, which would swap the parameters.
    QString between(const QString &col, double low, double high) {
        bind(low);
        bind(high);
        return col + " BETWEEN ? AND ?";
    }

    QString fail(const QString &why) {
        if (out.error.isEmpty())
            out.error = why;
        return "0";
    }

    QString term(const Node &n) {
        if (n.field.isEmpty()) {
            if (indexable(n.value))
                return "t.id IN (SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH " + bind(ftsPhrase(n.value)) + ')';
            QStringList cols;
            for (const QString &c : anyColumns)
                cols << QString("coalesce(t.%1, '')").arg(c);
            return '(' + cols.join(" || ' ' || ") + " LIKE " + bind(likePattern(n.value)) + " ESCAPE '\\')";
        }
        const Field *f = field(n.field);
        if (!f) {
            QStringList names;
            for (const Field &known : fields())
                names << known.name;
            return fail(QString("Unknown field \"%1\". Fields: %2").arg(n.field, names.join(", ")));
        }
        switch (f->type) {
        case Field::Text:
        case Field::MultiText:
            return text(*f, n);
        case Field::Enum:
            return enumerated(*f, n);
        default:
            return number(*f, n);
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
            return fail(QString("%1: is text; use %1:word, %1:=exact or %1:/regex/").arg(f.name));
        if (!f.ftsColumn.isEmpty() && indexable(n.value))
            return "t.id IN (SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH " +
                   bind(f.ftsColumn + " : " + ftsPhrase(n.value)) + ')';
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

    // Numbers in the units people think in: stars for ratings, seconds or
    // m:ss for lengths, kbps for bitrates.
    bool toNumber(const Field &f, const QString &text, double *value) {
        bool ok = false;
        if (f.type == Field::Duration && text.contains(':')) {
            const QStringList parts = text.split(':');
            double seconds = 0;
            for (const QString &part : parts) {
                const double v = part.toDouble(&ok);
                if (!ok)
                    return false;
                seconds = seconds * 60 + v;
            }
            *value = seconds * 1000;
            return true;
        }
        double v = text.toDouble(&ok);
        if (!ok)
            return false;
        if (f.type == Field::Rating)
            v *= 20;
        else if (f.type == Field::Duration)
            v *= 1000;
        *value = v;
        return true;
    }

    QString number(const Field &f, const Node &n) {
        const QString col = "t." + f.column;
        double a = 0, b = 0;
        auto bad = [&](const QString &v) { return fail(QString("%1: needs a number, not \"%2\"").arg(f.name, v)); };
        if (n.op == "..") {
            const bool hasA = !n.value.isEmpty(), hasB = !n.value2.isEmpty();
            if (hasA && !toNumber(f, n.value, &a))
                return bad(n.value);
            if (hasB && !toNumber(f, n.value2, &b))
                return bad(n.value2);
            if (hasA && hasB)
                return between(col, a, b);
            return hasA ? QString("%1 >= %2").arg(col, bind(a)) : QString("%1 <= %2").arg(col, bind(b));
        }
        if (n.op == "~")
            return fail(QString("%1: is a number; regular expressions only work on text").arg(f.name));
        if (!toNumber(f, n.value, &a))
            return bad(n.value);
        const QString op = n.op == ":" ? "=" : n.op;
        // A rating of 4 stars means 80, but also anything that rounds to 4 stars.
        if (f.type == Field::Rating && op == "=")
            return between(col, a - 10, a + 9);
        if (f.type == Field::Duration && op == "=")
            return between(col, a, a + 999);
        return QString("%1 %2 %3").arg(col, op, bind(a));
    }
};

} // namespace

Compiled compile(const Node &root) {
    Compiler c;
    c.out.where = c.node(root);
    if (!c.out.ok()) {
        c.out.where = "0";
        c.out.params.clear();
    }
    return c.out;
}

Compiled compile(const QString &text) {
    const Parsed p = parse(text);
    if (!p.ok()) {
        Compiled c;
        c.where = "0";
        c.error = p.position >= 0 ? QString("%1 (at character %2)").arg(p.error).arg(p.position + 1) : p.error;
        return c;
    }
    return compile(p.root);
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
                  {"rating", rating < 0 ? QJsonValue() : QJsonValue(rating)},
                  {"plays", plays},
                  {"format", format},
                  {"availability", availability}};
    return o;
}

Result run(Library &library, const QString &text, const Options &options) {
    Result result;
    QElapsedTimer timer;
    timer.start();
    const Compiled c = compile(text);
    if (!c.ok()) {
        result.error = c.error;
        return result;
    }
    QSqlQuery q(library.db());
    q.setForwardOnly(true);
    QString sql = "SELECT t.id, v.root_path, v.online, t.rel_path, t.title, t.artist, t.album_artist, t.album, "
                  "t.genre, t.format, t.availability, t.year, t.disc_no, t.track_no, t.duration_ms, t.rating, "
                  "t.play_count FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE " +
                  c.where +
                  " ORDER BY t.sort_artist COLLATE NOCASE, t.album COLLATE NOCASE, t.disc_no, t.track_no, t.rel_path";
    if (options.limit > 0)
        sql += QString(" LIMIT %1 OFFSET %2").arg(options.limit).arg(options.offset);
    q.prepare(sql);
    for (const QVariant &p : c.params)
        q.addBindValue(p);
    if (!q.exec()) {
        result.error = q.lastError().text();
        return result;
    }
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
        result.tracks << t;
    }
    result.total = result.tracks.size();
    if (options.countTotal && options.limit > 0) {
        QSqlQuery count(library.db());
        count.prepare("SELECT count(*) FROM tracks t WHERE " + c.where);
        for (const QVariant &p : c.params)
            count.addBindValue(p);
        if (count.exec() && count.next())
            result.total = count.value(0).toLongLong();
    }
    result.elapsedUs = timer.nsecsElapsed() / 1000;
    return result;
}

}
