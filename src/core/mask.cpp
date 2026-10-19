#include "mask.h"

#include "library.h"

#include <QDateTime>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlQuery>

namespace Mask {

namespace {

// Stands for a folder separator while a mask is evaluated, so a '/' inside a
// tag value can be told from one written in the mask. A private-use
// character never appears in real tags.
const QChar separatorMark(0xE000);

struct FieldInfo {
    const char *canonical;
    const char *display;
    QStringList aliases;
    bool numeric;
};

const QList<FieldInfo> &fieldTable() {
    static const QList<FieldInfo> table = {
        {"artist", "Artist", {}, false},
        {"albumartist", "Album Artist", {}, false},
        {"album", "Album", {}, false},
        {"title", "Title", {}, false},
        {"genre", "Genre", {}, false},
        {"composer", "Composer", {}, false},
        {"year", "Year", {"date"}, true},
        {"originalyear", "Original Date", {"originaldate", "originalyear"}, true},
        {"track", "Track#", {"track#", "tracknumber", "tracknum"}, true},
        {"tracktotal", "Track Total", {"totaltracks"}, true},
        {"disc", "Disc#", {"disc#", "discnumber", "discnum"}, true},
        {"disctotal", "Disc Total", {"totaldiscs"}, true},
        {"autonumber", "Auto Number", {}, true},
        {"extension", "Extension", {"ext"}, false},
        {"filename", "Filename", {"file"}, false},
        {"folder", "Folder", {}, false},
        {"path", "Path", {}, false},
        {"bitrate", "Bitrate", {}, true},
        {"bpm", "BPM", {"tempo"}, true},
        {"comment", "Comment", {}, false},
        {"type", "Type", {"contenttype"}, false},
        {"rating", "Rating", {"stars"}, true},
        {"added", "Added", {"dateadded"}, false},
        {"length", "Length", {"duration"}, false},
        {"format", "Format", {"codec"}, false},
        {"custom1", "Custom1", {}, false},
        {"custom2", "Custom2", {}, false},
        {"custom3", "Custom3", {}, false},
        {"custom4", "Custom4", {}, false},
        {"custom5", "Custom5", {}, false},
        {"skip", "Skip", {}, false},
    };
    return table;
}

struct FunctionInfo {
    const char *name;
    const char *display;
    int minArgs, maxArgs; // maxArgs -1 means any number
};

const QList<FunctionInfo> &functionTable() {
    static const QList<FunctionInfo> table = {
        {"if", "$If", 2, 3},         {"ifempty", "$IfEmpty", 2, 2},       {"first", "$First", 1, -1},
        {"replace", "$Replace", 3, -1}, {"removeprefix", "$RemovePrefix", 1, -1},
        {"moveprefix", "$MovePrefix", 1, -1}, {"left", "$Left", 2, 2},    {"right", "$Right", 2, 2},
        {"mid", "$Mid", 2, 3},       {"trim", "$Trim", 1, 1},             {"upper", "$Upper", 1, 1},
        {"lower", "$Lower", 1, 1},   {"title", "$Title", 1, 1},           {"len", "$Len", 1, 1},
        {"pad", "$Pad", 2, 3},
    };
    return table;
}

const FunctionInfo *functionInfo(const QString &name) {
    for (const FunctionInfo &f : functionTable())
        if (name.compare(QLatin1String(f.name), Qt::CaseInsensitive) == 0)
            return &f;
    return nullptr;
}

class Parser {
public:
    Parser(const QString &text, bool allowSkip) : s(text), skipAllowed(allowSkip) {}

    Parsed run() {
        Parsed p;
        p.nodes = sequence(false);
        if (error.isEmpty() && i < s.size())
            fail(QString("Unexpected '%1'").arg(s[i]), i);
        p.error = error;
        p.position = errorAt;
        if (!p.ok())
            p.nodes.clear();
        return p;
    }

private:
    bool fail(const QString &message, int at) {
        if (error.isEmpty()) {
            error = message;
            errorAt = at;
        }
        return false;
    }

    static void flush(QList<Node> &out, QString &text, int start) {
        if (text.isEmpty())
            return;
        Node n;
        n.kind = Node::Text;
        n.text = text;
        n.position = start;
        out << n;
        text.clear();
    }

    // Inside a function's arguments, a ',' or ')' at the argument's own
    // bracket depth ends it; brackets that open inside are literal text.
    QList<Node> sequence(bool inArgument) {
        QList<Node> out;
        QString text;
        int textStart = i, depth = 0;
        while (i < s.size() && error.isEmpty()) {
            const QChar c = s[i];
            if (inArgument && depth == 0 && (c == ',' || c == ')'))
                break;
            if (text.isEmpty())
                textStart = i;
            if (c == '<') {
                flush(out, text, textStart);
                Node n;
                if (!field(n))
                    break;
                out << n;
            } else if (c == '$') {
                if (i + 1 < s.size() && s[i + 1] == '$') {
                    text += '$';
                    i += 2;
                } else if (i + 1 < s.size() && s[i + 1].isLetter()) {
                    flush(out, text, textStart);
                    Node n;
                    if (!function(n))
                        break;
                    out << n;
                } else {
                    text += '$';
                    ++i;
                }
            } else if (c == '/' || c == '\\') {
                flush(out, text, textStart);
                Node n;
                n.kind = Node::Separator;
                n.position = i++;
                out << n;
            } else if (inArgument && c == '"') {
                const int close = s.indexOf('"', i + 1);
                if (close < 0) {
                    fail("Unclosed quote", i);
                    break;
                }
                text += s.mid(i + 1, close - i - 1);
                i = close + 1;
            } else {
                if (inArgument && c == '(')
                    ++depth;
                else if (inArgument && c == ')')
                    --depth;
                text += c;
                ++i;
            }
        }
        flush(out, text, textStart);
        return out;
    }

    bool field(Node &n) {
        const int start = i;
        const int close = s.indexOf('>', i + 1);
        if (close < 0)
            return fail("Unclosed '<': a field ends with '>'", start);
        const QString inner = s.mid(i + 1, close - i - 1);
        i = close + 1;
        static const QRegularExpression modifier("^(.*?)\\s*(?:([:@])\\s*(\\d+))?$");
        const auto m = modifier.match(inner);
        const QString name = m.captured(1);
        const QString canonical = canonicalField(name);
        if (canonical.isEmpty())
            return fail(QString("Unknown field <%1>").arg(name.trimmed()), start);
        if (canonical == "skip" && !skipAllowed)
            return fail("<Skip> works only when tagging from file names", start);
        n.kind = Node::Field;
        n.text = canonical;
        n.position = start;
        if (!m.captured(2).isEmpty()) {
            const int number = m.captured(3).toInt();
            if (m.captured(2) == "@") {
                if (number < 1)
                    return fail("A group size after '@' must be 1 or more", start);
                n.group = number;
            } else {
                if ((canonical == "folder" || canonical == "path") && number < 1)
                    return fail(QString("<%1:n> counts from 1").arg(canonical == "folder" ? "Folder" : "Path"), start);
                n.width = number;
            }
        }
        return true;
    }

    bool function(Node &n) {
        const int start = i;
        int end = i + 1;
        while (end < s.size() && s[end].isLetterOrNumber())
            ++end;
        const QString name = s.mid(i + 1, end - i - 1);
        const FunctionInfo *info = functionInfo(name);
        if (!info)
            return fail(QString("Unknown function $%1").arg(name), start);
        if (end >= s.size() || s[end] != '(')
            return fail(QString("Expected '(' after %1").arg(info->display), end);
        i = end + 1;
        n.kind = Node::Function;
        n.text = info->name;
        n.position = start;
        while (true) {
            n.args << sequence(true);
            if (!error.isEmpty())
                return false;
            if (i >= s.size())
                return fail(QString("Unclosed %1( : it needs a ')'").arg(info->display), start);
            if (s[i] == ',') {
                ++i;
                continue;
            }
            ++i; // ')'
            break;
        }
        const int count = n.args.size();
        if (count < info->minArgs || (info->maxArgs >= 0 && count > info->maxArgs)) {
            const QString wanted = info->maxArgs < 0 ? QString("at least %1").arg(info->minArgs)
                                   : info->minArgs == info->maxArgs
                                       ? QString::number(info->minArgs)
                                       : QString("%1 or %2").arg(info->minArgs).arg(info->maxArgs);
            return fail(QString("%1 takes %2 arguments, not %3").arg(info->display, wanted).arg(count), start);
        }
        if (n.text == "if")
            splitComparison(n);
        return true;
    }

    // $If(<Artist>=<Album Artist>,...) compares; the first '!=' or '=' in
    // the condition's literal text splits it.
    static void splitComparison(Node &n) {
        const QList<Node> condition = n.args[0];
        for (int k = 0; k < condition.size(); ++k) {
            const Node &part = condition[k];
            if (part.kind != Node::Text)
                continue;
            int at = part.text.indexOf("!=");
            QString op = "!=";
            const int eq = part.text.indexOf('=');
            if (at < 0 || (eq >= 0 && eq < at)) {
                at = eq;
                op = "=";
            }
            if (at < 0)
                continue;
            n.compare = op;
            n.left = condition.mid(0, k);
            n.right = condition.mid(k + 1);
            Node before = part, after = part;
            before.text = part.text.left(at);
            after.text = part.text.mid(at + op.size());
            if (!before.text.isEmpty())
                n.left << before;
            if (!after.text.isEmpty())
                n.right.prepend(after);
            return;
        }
    }

    const QString s;
    const bool skipAllowed;
    int i = 0;
    QString error;
    int errorAt = -1;
};

QString stripAccents(const QString &text) {
    const QString decomposed = text.normalized(QString::NormalizationForm_D);
    QString out;
    for (const QChar c : decomposed)
        if (c.category() != QChar::Mark_NonSpacing)
            out += c;
    return out;
}

// <Artist@1> gives "B" for "Beatles"; <Artist@3> gives "ABC" for "Abba".
QString alphabetGroup(const QString &value, int size) {
    const QString clean = stripAccents(value.trimmed());
    if (clean.isEmpty())
        return {};
    const QChar first = clean[0].toUpper();
    if (first >= 'A' && first <= 'Z') {
        if (size == 1)
            return QString(first);
        const int start = (first.unicode() - 'A') / size * size;
        QString letters;
        for (int k = start; k < start + size && k < 26; ++k)
            letters += QChar('A' + k);
        return letters;
    }
    if (first.isLetter() && size == 1)
        return QString(first);
    return "#";
}

QString fieldValue(const Node &n, const Values &values, const Context &context) {
    const QFileInfo file(values.path);
    QString value;
    if (n.text == "folder") {
        const QStringList parts = file.path().split('/', Qt::SkipEmptyParts);
        const int index = n.width > 0 ? n.width : 1;
        value = index <= parts.size() ? parts[parts.size() - index] : QString();
    } else if (n.text == "path") {
        QString folder = file.path();
        QString root = values.rootPath;
        while (root.endsWith('/') && root.size() > 1)
            root.chop(1);
        if (!root.isEmpty() && (folder == root || folder.startsWith(root + '/')))
            folder = folder.mid(root.size());
        const QStringList parts = folder.split('/', Qt::SkipEmptyParts);
        if (n.width > 0)
            return n.width <= parts.size() ? parts[n.width - 1] : QString();
        // The whole folder path keeps its structure.
        return parts.join(context.pathMode ? QString(separatorMark) : QString("/"));
    } else if (n.text == "filename") {
        value = values.fields.contains("filename") ? values.value("filename") : file.completeBaseName();
    } else if (n.text == "extension") {
        value = values.fields.contains("extension") ? values.value("extension") : file.suffix();
    } else if (n.text == "autonumber") {
        value = context.autoNumber > 0 ? QString::number(context.autoNumber) : QString();
    } else {
        value = values.value(n.text);
    }

    if (n.group > 0)
        return alphabetGroup(value, n.group);
    if (n.width > 0 && n.text != "folder") {
        if (isNumericField(n.text)) {
            if (!value.isEmpty())
                value = value.rightJustified(n.width, '0');
        } else {
            value = value.left(n.width);
        }
    }
    return value;
}

QString evaluateList(const QList<Node> &nodes, const Values &values, const Context &context);

bool truthy(const QString &value) {
    const QString v = value.trimmed();
    return !v.isEmpty() && v != QString(separatorMark);
}

int number(const QString &text) { return text.trimmed().toInt(); }

QString applyPrefix(const QString &value, const QStringList &prefixes, bool move) {
    for (const QString &prefix : prefixes) {
        const QString p = prefix.trimmed();
        if (p.isEmpty() || value.size() <= p.size() + 1)
            continue;
        if (value.startsWith(p + ' ', Qt::CaseInsensitive)) {
            const QString rest = value.mid(p.size() + 1).trimmed();
            return move ? rest + ", " + value.left(p.size()) : rest;
        }
    }
    return value;
}

QString titleCase(const QString &text) {
    QString out = text.toLower();
    bool start = true;
    for (QChar &c : out) {
        if (c.isLetter() && start)
            c = c.toUpper();
        start = !c.isLetterOrNumber() && c != '\'';
    }
    return out;
}

QString function(const Node &n, const Values &values, const Context &context) {
    auto arg = [&](int k) { return k < n.args.size() ? evaluateList(n.args[k], values, context) : QString(); };
    const QString &f = n.text;
    if (f == "if") {
        bool condition;
        if (!n.compare.isEmpty()) {
            const QString a = evaluateList(n.left, values, context).trimmed();
            const QString b = evaluateList(n.right, values, context).trimmed();
            bool okA = false, okB = false;
            const double x = a.toDouble(&okA), y = b.toDouble(&okB);
            const bool equal = okA && okB ? x == y : a.compare(b, Qt::CaseInsensitive) == 0;
            condition = n.compare == "=" ? equal : !equal;
        } else {
            condition = truthy(arg(0));
        }
        return condition ? arg(1) : arg(2);
    }
    if (f == "ifempty") {
        const QString a = arg(0);
        return truthy(a) ? a : arg(1);
    }
    if (f == "first") {
        if (n.args.size() == 1)
            return arg(0).section(';', 0, 0).trimmed();
        for (int k = 0; k < n.args.size(); ++k) {
            const QString a = arg(k);
            if (truthy(a))
                return a;
        }
        return {};
    }
    if (f == "replace") {
        QString s = arg(0);
        for (int k = 1; k + 1 < n.args.size(); k += 2) {
            const QString from = arg(k);
            if (!from.isEmpty())
                s.replace(from, arg(k + 1));
        }
        return s;
    }
    if (f == "removeprefix" || f == "moveprefix") {
        QStringList prefixes;
        for (int k = 1; k < n.args.size(); ++k)
            prefixes << arg(k);
        if (prefixes.isEmpty())
            prefixes << "The";
        return applyPrefix(arg(0), prefixes, f == "moveprefix");
    }
    if (f == "left")
        return arg(0).left(qMax(0, number(arg(1))));
    if (f == "right")
        return arg(0).right(qMax(0, number(arg(1))));
    if (f == "mid") {
        const QString s = arg(0);
        const int start = qMax(1, number(arg(1))) - 1;
        return n.args.size() > 2 ? s.mid(start, qMax(0, number(arg(2)))) : s.mid(start);
    }
    if (f == "trim")
        return arg(0).simplified();
    if (f == "upper")
        return arg(0).toUpper();
    if (f == "lower")
        return arg(0).toLower();
    if (f == "title")
        return titleCase(arg(0));
    if (f == "len")
        return QString::number(arg(0).size());
    if (f == "pad") {
        const QString fill = n.args.size() > 2 ? arg(2) : QString("0");
        return arg(0).rightJustified(qMax(0, number(arg(1))), fill.isEmpty() ? QChar(' ') : fill[0]);
    }
    return {};
}

QString evaluateList(const QList<Node> &nodes, const Values &values, const Context &context) {
    QString out;
    for (const Node &n : nodes) {
        switch (n.kind) {
        case Node::Text: out += n.text; break;
        case Node::Separator: out += context.pathMode ? separatorMark : QChar('/'); break;
        case Node::Field: out += fieldValue(n, values, context); break;
        case Node::Function: out += function(n, values, context); break;
        }
    }
    return out;
}

bool isReservedWindowsName(const QString &name) {
    static const QRegularExpression reserved("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\\..*)?$",
                                             QRegularExpression::CaseInsensitiveOption);
    return reserved.match(name).hasMatch();
}

QString stripTrailing(QString s, bool dots) {
    while (!s.isEmpty() && (s.back().isSpace() || (dots && s.back() == '.')))
        s.chop(1);
    return s;
}

} // namespace

QString canonicalField(const QString &name) {
    QString key = name.trimmed().toLower();
    key.remove(' ');
    key.remove('_');
    key.remove('-');
    for (const FieldInfo &f : fieldTable()) {
        if (key == QLatin1String(f.canonical) || f.aliases.contains(key))
            return f.canonical;
        QString display = QString(f.display).toLower();
        display.remove(' ');
        if (key == display)
            return f.canonical;
    }
    return {};
}

QStringList fieldNames() {
    QStringList out;
    for (const FieldInfo &f : fieldTable())
        out << QString("<%1>").arg(f.display);
    return out;
}

QStringList functionNames() {
    QStringList out;
    for (const FunctionInfo &f : functionTable())
        out << f.display;
    return out;
}

bool isNumericField(const QString &canonical) {
    for (const FieldInfo &f : fieldTable())
        if (canonical == QLatin1String(f.canonical))
            return f.numeric;
    return false;
}

bool isTaggableField(const QString &canonical) {
    static const QStringList taggable = {"artist", "albumartist", "album", "title", "genre", "composer",
                                         "year", "originalyear", "track", "tracktotal", "disc", "disctotal",
                                         "bpm", "comment", "custom1", "custom2", "custom3", "custom4",
                                         "custom5"};
    return taggable.contains(canonical);
}

Parsed parse(const QString &mask, bool allowSkip) { return Parser(mask, allowSkip).run(); }

QString sanitiseComponent(const QString &name, const Sanitise &rules, bool keepExtension) {
    QString out;
    out.reserve(name.size());
    static const QString windowsBad = QStringLiteral("\\<>:\"|?*");
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || c == '/' || c == QChar(0x7f) || c == separatorMark ||
            (rules.windowsSafe && windowsBad.contains(c)))
            out += rules.replacement;
        else
            out += c;
    }
    out = out.trimmed();
    if (out == "." || out == "..")
        out = rules.replacement;
    out = stripTrailing(out, rules.windowsSafe);
    // A leading dot would hide the file, and the scanner skips hidden files.
    while (out.startsWith('.'))
        out = rules.replacement + out.mid(1);
    if (rules.windowsSafe && isReservedWindowsName(out)) {
        const int dot = out.indexOf('.');
        out = dot < 0 ? out + rules.replacement : out.left(dot) + rules.replacement + out.mid(dot);
    }
    if (rules.maxComponentBytes > 0 && out.toUtf8().size() > rules.maxComponentBytes) {
        QString extension;
        if (keepExtension) {
            const int dot = out.lastIndexOf('.');
            if (dot > 0 && out.size() - dot <= 16)
                extension = out.mid(dot);
        }
        QString stem = out.left(out.size() - extension.size());
        const int budget = rules.maxComponentBytes - extension.toUtf8().size();
        while (!stem.isEmpty() && stem.toUtf8().size() > budget) {
            stem.chop(1);
            if (!stem.isEmpty() && stem.back().isHighSurrogate())
                stem.chop(1);
        }
        out = stripTrailing(stem, rules.windowsSafe) + extension;
    }
    return out;
}

QString evaluate(const Parsed &mask, const Values &values, const Context &context) {
    const QString raw = evaluateList(mask.nodes, values, context);
    if (!context.pathMode)
        return raw;
    const QStringList parts = raw.split(separatorMark);
    QStringList clean;
    for (int k = 0; k < parts.size(); ++k) {
        const bool last = k == parts.size() - 1;
        const QString part = sanitiseComponent(parts[k], context.sanitise, false);
        if (!part.isEmpty())
            clean << part;
        else if (last)
            clean << "Untitled";
    }
    if (context.sanitise.maxPathChars > 0) {
        const int over = clean.join('/').size() - context.sanitise.maxPathChars;
        if (over > 0 && !clean.isEmpty()) {
            QString &name = clean.last();
            name = stripTrailing(name.left(qMax(8, name.size() - over)), context.sanitise.windowsSafe);
        }
    }
    return clean.join('/');
}

QString evaluate(const QString &mask, const Values &values, const Context &context, QString *error) {
    const Parsed p = parse(mask);
    if (!p.ok()) {
        if (error)
            *error = QString("%1 (at character %2)").arg(p.error).arg(p.position + 1);
        return {};
    }
    if (error)
        error->clear();
    return evaluate(p, values, context);
}

Values fromTrack(Library &library, qint64 trackId) {
    Values v;
    QSqlQuery q(library.db());
    q.prepare("SELECT t.title, t.artist, t.album_artist, t.sort_artist, t.album, t.genre, t.composer, t.year, "
              "t.original_year, t.track_no, t.track_total, t.disc_no, t.disc_total, t.bitrate, t.bpm, t.comment, "
              "t.content_type, t.rating, t.added, t.duration_ms, t.format, t.custom1, t.custom2, t.custom3, "
              "t.custom4, t.custom5, t.rel_path, v.root_path, t.volume_id, t.compilation "
              "FROM tracks t JOIN volumes v ON v.id = t.volume_id WHERE t.id = ?");
    q.addBindValue(trackId);
    if (!q.exec() || !q.next())
        return v;
    auto text = [&](int k) { return q.value(k).toString(); };
    auto num = [&](int k) { return q.value(k).isNull() || q.value(k).toInt() == 0 ? QString() : text(k); };
    v.fields["title"] = text(0);
    v.fields["artist"] = text(1);
    QString albumArtist = text(2);
    if (albumArtist.isEmpty())
        albumArtist = q.value(29).toInt() ? QString("Various Artists") : text(1);
    v.fields["albumartist"] = albumArtist;
    v.fields["album"] = text(4);
    v.fields["genre"] = text(5);
    v.fields["composer"] = text(6);
    v.fields["year"] = num(7);
    v.fields["originalyear"] = num(8);
    v.fields["track"] = num(9);
    v.fields["tracktotal"] = num(10);
    v.fields["disc"] = num(11);
    v.fields["disctotal"] = num(12);
    v.fields["bitrate"] = num(13);
    v.fields["bpm"] = num(14);
    v.fields["comment"] = text(15);
    v.fields["type"] = text(16);
    if (!q.value(17).isNull()) {
        const double stars = q.value(17).toInt() / 20.0;
        v.fields["rating"] = QString::number(stars, 'g', 2);
    }
    if (q.value(18).toLongLong() > 0)
        v.fields["added"] = QDateTime::fromSecsSinceEpoch(q.value(18).toLongLong()).toString("yyyy-MM-dd");
    const qint64 seconds = q.value(19).toLongLong() / 1000;
    if (seconds > 0)
        v.fields["length"] = QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
    v.fields["format"] = text(20);
    for (int k = 0; k < 5; ++k)
        v.fields[QString("custom%1").arg(k + 1)] = text(21 + k);
    const QString rel = text(26);
    QString root = text(27);
    if (root == "/")
        root.clear();
    v.path = root + '/' + rel;

    // The library root holding the file: the deepest root folder above it.
    QSqlQuery r(library.db());
    r.prepare("SELECT rel_path FROM folders WHERE volume_id = ? AND is_root = 1");
    r.addBindValue(q.value(28));
    QString best;
    bool found = false;
    if (r.exec()) {
        while (r.next()) {
            const QString folder = r.value(0).toString();
            if ((folder.isEmpty() || rel.startsWith(folder + '/')) && (!found || folder.size() > best.size())) {
                best = folder;
                found = true;
            }
        }
    }
    // Outside every library folder, the volume's root stands in.
    v.inLibraryFolder = found;
    v.rootPath = !found || best.isEmpty() ? (root.isEmpty() ? QString("/") : root) : root + '/' + best;
    return v;
}

FilenameMatch matchFilename(const QString &pattern, const QString &path) {
    FilenameMatch result;
    const Parsed p = parse(pattern, true);
    if (!p.ok()) {
        result.error = p.error;
        result.position = p.position;
        return result;
    }
    QString regex = "^";
    QStringList captured;
    int separators = 0;
    for (const Node &n : p.nodes) {
        switch (n.kind) {
        case Node::Text: regex += QRegularExpression::escape(n.text); break;
        case Node::Separator:
            regex += '/';
            ++separators;
            break;
        case Node::Function:
            result.error = "A pattern for tagging from file names can't use functions";
            result.position = n.position;
            return result;
        case Node::Field:
            if (n.text == "skip") {
                regex += "[^/]*?";
            } else if (!isTaggableField(n.text)) {
                result.error = QString("<%1> can't be filled from a file name").arg(n.text);
                result.position = n.position;
                return result;
            } else if (isNumericField(n.text)) {
                regex += n.width > 0 ? QString("\\s*(\\d{%1})\\s*").arg(n.width) : QString("\\s*(\\d+)\\s*");
                captured << n.text;
            } else {
                regex += "([^/]+?)";
                captured << n.text;
            }
            break;
        }
    }
    regex += '$';

    const QFileInfo file(path);
    const QStringList folders = file.path().split('/', Qt::SkipEmptyParts);
    if (folders.size() < separators) {
        result.error = "The path has fewer folders than the pattern";
        return result;
    }
    QStringList tail = folders.mid(folders.size() - separators);
    tail << file.completeBaseName();
    const QString subject = tail.join('/');

    const QRegularExpression re(regex);
    const auto m = re.match(subject);
    if (!m.hasMatch()) {
        result.error = QString("\"%1\" doesn't match the pattern").arg(subject);
        return result;
    }
    for (int k = 0; k < captured.size(); ++k) {
        QString value = m.captured(k + 1).trimmed();
        if (isNumericField(captured[k]))
            value = value.isEmpty() ? QString() : QString::number(value.toInt());
        if (!value.isEmpty() && !result.values.contains(captured[k]))
            result.values.insert(captured[k], value);
    }
    result.ok = true;
    return result;
}

}
