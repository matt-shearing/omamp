#pragma once

#include <QList>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// The auto-playlist editor's rule builder: rows of field, operator and
// value, matched all or any, plus sort and limit, turned into query text and
// back. The text stays the source of truth: fromQuery() reads what it can,
// and says the query is "advanced" when the rows can't hold it (brackets,
// AND mixed with OR, +word). A query counts as representable only when the
// rows give back a query that compiles to exactly the same SQL.
namespace RuleBuilder {

// Operators, as the builder names them:
//   contains, is, not_contains, is_not, lt, le, gt, ge, between, matches,
//   empty, set
struct Rule {
    QString field; // a query field name; empty means any text field
    QString op = "contains";
    QString value, value2;
};

struct Rules {
    bool any = false; // match any rule, rather than all
    QList<Rule> rules;
    QString sort;           // sort text without "sort:", e.g. "-rating,artist" or "random"
    QString limitKind = "none"; // none, count, time, size
    QString limitValue;     // "50", "2h", "700mb"
    bool representable = true;
    QString reason; // why not, when it isn't

    QVariantMap toMap() const;
    static Rules fromMap(const QVariantMap &map);
};

Rules fromQuery(const QString &text);
QString toQuery(const Rules &rules);

// The fields the builder offers: [{name, type, values}], type being text,
// number, rating, length, size, date or choice.
QVariantList fieldList();
// The operators for a field type: [{id, label}].
QVariantList operatorsFor(const QString &type);

} // namespace RuleBuilder
