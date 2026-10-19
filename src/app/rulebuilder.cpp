#include "rulebuilder.h"

#include "query.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QRegularExpression>

namespace RuleBuilder {

namespace {

QString tr(const char *text) { return QCoreApplication::translate("RuleBuilder", text); }

// A value as the query language reads it back unchanged: quoted when it
// has spaces or characters the parser gives a meaning.
QString quote(const QString &value) {
    static const QRegularExpression plain("^[^\\s\"():|;/]+$");
    const bool special = !plain.match(value).hasMatch() || value.startsWith('-') || value.startsWith('+') ||
                         value.contains("..") || value.compare("OR", Qt::CaseInsensitive) == 0 ||
                         value.compare("AND", Qt::CaseInsensitive) == 0 ||
                         value.compare("none", Qt::CaseInsensitive) == 0 ||
                         value.compare("never", Qt::CaseInsensitive) == 0 ||
                         value.compare("any", Qt::CaseInsensitive) == 0 || value.startsWith('<') ||
                         value.startsWith('>') || value.startsWith('=');
    if (!special)
        return value;
    QString v = value;
    v.remove('"');
    return '"' + v + '"';
}

QString ruleText(const Rule &r) {
    QString text;
    const QString prefix = r.field.isEmpty() ? QString() : r.field + ':';
    if (r.op == "contains" || r.op == "not_contains")
        text = prefix + quote(r.value);
    else if (r.op == "is" || r.op == "is_not")
        text = r.field.isEmpty() ? quote(r.value) : r.field + ":=" + quote(r.value);
    else if (r.op == "lt")
        text = prefix + '<' + r.value.trimmed();
    else if (r.op == "le")
        text = prefix + "<=" + r.value.trimmed();
    else if (r.op == "gt")
        text = prefix + '>' + r.value.trimmed();
    else if (r.op == "ge")
        text = prefix + ">=" + r.value.trimmed();
    else if (r.op == "between")
        text = prefix + r.value.trimmed() + ".." + r.value2.trimmed();
    else if (r.op == "matches")
        text = prefix + '/' + r.value + '/';
    else if (r.op == "empty")
        text = prefix + "none";
    else if (r.op == "set")
        text = prefix + "any";
    else
        text = prefix + quote(r.value);
    if (r.op == "not_contains" || r.op == "is_not")
        text.prepend('-');
    return text;
}

bool ruleFromTerm(const Query::Node &term, bool negated, Rule *out, QString *why) {
    if (term.wordStart) {
        *why = tr("a word that must start a word (+word)");
        return false;
    }
    Rule r;
    if (!term.field.isEmpty()) {
        const Query::Field *f = Query::field(term.field);
        r.field = f ? f->name : term.field;
    }
    r.value = term.value;
    r.value2 = term.value2;
    const QString op = term.op;
    if (op == ":")
        r.op = negated ? "not_contains" : "contains";
    else if (op == "=")
        r.op = negated ? "is_not" : "is";
    else if (negated) {
        *why = tr("NOT in front of a comparison");
        return false;
    } else if (op == "<")
        r.op = "lt";
    else if (op == "<=")
        r.op = "le";
    else if (op == ">")
        r.op = "gt";
    else if (op == ">=")
        r.op = "ge";
    else if (op == "..")
        r.op = "between";
    else if (op == "~")
        r.op = "matches";
    else if (op == "none")
        r.op = "empty";
    else if (op == "any")
        r.op = "set";
    else {
        *why = tr("an operator the rows don't offer");
        return false;
    }
    *out = r;
    return true;
}

// Two queries mean the same when they compile to the same SQL.
bool sameMeaning(const QString &a, const QString &b) {
    Query::Context context;
    context.now = QDateTime::fromSecsSinceEpoch(1700000000);
    context.seed = 7;
    const Query::Compiled x = Query::compile(a, context), y = Query::compile(b, context);
    return x.ok() && y.ok() && x.where == y.where && x.params == y.params && x.orderBy == y.orderBy &&
           x.limit.kind == y.limit.kind && x.limit.value == y.limit.value;
}

} // namespace

QVariantMap Rules::toMap() const {
    QVariantList list;
    for (const Rule &r : rules)
        list << QVariantMap{{"field", r.field}, {"op", r.op}, {"value", r.value}, {"value2", r.value2}};
    return {{"any", any},
            {"rules", list},
            {"sort", sort},
            {"limitKind", limitKind},
            {"limitValue", limitValue},
            {"representable", representable},
            {"reason", reason}};
}

Rules Rules::fromMap(const QVariantMap &map) {
    Rules r;
    r.any = map.value("any").toBool();
    for (const QVariant &v : map.value("rules").toList()) {
        const QVariantMap m = v.toMap();
        Rule rule;
        rule.field = m.value("field").toString();
        rule.op = m.value("op", "contains").toString();
        rule.value = m.value("value").toString();
        rule.value2 = m.value("value2").toString();
        r.rules << rule;
    }
    r.sort = map.value("sort").toString();
    r.limitKind = map.value("limitKind", "none").toString();
    r.limitValue = map.value("limitValue").toString();
    return r;
}

QString toQuery(const Rules &rules) {
    QStringList parts;
    for (const Rule &r : rules.rules) {
        const bool needsValue = r.op != "empty" && r.op != "set";
        if (needsValue && r.value.trimmed().isEmpty() && (r.op != "between" || r.value2.trimmed().isEmpty()))
            continue; // a row still being filled in
        parts << ruleText(r);
    }
    QString text = parts.join(rules.any ? " OR " : " ");
    QStringList tail;
    if (!rules.sort.trimmed().isEmpty())
        tail << "sort:" + rules.sort.trimmed();
    if (rules.limitKind != "none" && !rules.limitValue.trimmed().isEmpty())
        tail << "limit:" + rules.limitValue.trimmed();
    if (!tail.isEmpty())
        text += (text.isEmpty() ? "" : " ") + tail.join(' ');
    return text;
}

Rules fromQuery(const QString &text) {
    Rules out;
    const Query::Parsed parsed = Query::parse(text);
    if (!parsed.ok()) {
        out.representable = false;
        out.reason = parsed.error;
        return out;
    }
    QStringList keys;
    for (const Query::SortKey &k : parsed.sort) {
        QString key = (k.descending ? "-" : "") + k.field;
        if (k.seed >= 0)
            key += ':' + QString::number(k.seed);
        keys << key;
    }
    out.sort = keys.join(',');
    switch (parsed.limit.kind) {
    case Query::Limit::Count: out.limitKind = "count"; break;
    case Query::Limit::Duration: out.limitKind = "time"; break;
    case Query::Limit::Bytes: out.limitKind = "size"; break;
    case Query::Limit::None: break;
    }
    out.limitValue = parsed.limit.toString();

    QString why;
    QList<Query::Node> terms;
    const Query::Node &root = parsed.root;
    if (root.kind == Query::Node::Term || root.kind == Query::Node::Not)
        terms << root;
    else {
        out.any = root.kind == Query::Node::Or;
        terms = root.children;
    }
    for (const Query::Node &n : std::as_const(terms)) {
        Rule r;
        bool ok = false;
        if (n.kind == Query::Node::Term)
            ok = ruleFromTerm(n, false, &r, &why);
        else if (n.kind == Query::Node::Not && n.children.size() == 1 && n.children[0].kind == Query::Node::Term)
            ok = ruleFromTerm(n.children[0], true, &r, &why);
        else
            why = out.any ? tr("brackets or AND inside OR") : tr("brackets or OR mixed with AND");
        if (!ok) {
            out.representable = false;
            out.reason = why;
            out.rules.clear();
            return out;
        }
        out.rules << r;
    }
    // Any rows that don't give back the same query can't be trusted to edit it.
    if (!sameMeaning(text, toQuery(out))) {
        out.representable = false;
        out.reason = tr("the rows can't say it exactly");
        out.rules.clear();
    }
    return out;
}

QVariantList fieldList() {
    QVariantList out;
    out << QVariantMap{{"name", QString()}, {"label", tr("Any text")}, {"type", "text"}, {"values", QStringList()}};
    for (const Query::Field &f : Query::fields()) {
        QString type;
        switch (f.type) {
        case Query::Field::Text:
        case Query::Field::MultiText: type = "text"; break;
        case Query::Field::Number: type = "number"; break;
        case Query::Field::Rating: type = "rating"; break;
        case Query::Field::Duration: type = "length"; break;
        case Query::Field::Bytes: type = "size"; break;
        case Query::Field::Date: type = "date"; break;
        case Query::Field::Enum: type = "choice"; break;
        }
        out << QVariantMap{{"name", f.name}, {"label", f.name}, {"type", type}, {"values", f.values}};
    }
    return out;
}

QVariantList operatorsFor(const QString &type) {
    auto op = [](const char *id, const QString &label) { return QVariantMap{{"id", id}, {"label", label}}; };
    QVariantList out;
    if (type == "text" || type == "choice") {
        out << op("contains", tr("contains")) << op("not_contains", tr("does not contain")) << op("is", tr("is"))
            << op("is_not", tr("is not"));
        if (type == "text")
            out << op("matches", tr("matches the pattern"));
    } else {
        out << op("contains", tr("is")) << op("lt", tr("less than")) << op("le", tr("at most"))
            << op("gt", tr("more than")) << op("ge", tr("at least")) << op("between", tr("between"));
    }
    out << op("empty", tr("is empty")) << op("set", tr("is set"));
    return out;
}

} // namespace RuleBuilder
