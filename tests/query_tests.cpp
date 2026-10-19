// The query language: parsing, compiling and running against a small library
// built directly in SQL, so dates, play counts and ratings are exact. The
// clock is pinned (Context::now) so relative dates never drift.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "cli.h"
#include "library.h"
#include "query.h"
#include "volumes.h"

#include "fixture_library.h"

namespace {

using namespace Fixture;
Query::Options pinned(quint32 seed = 0) {
    Query::Options o;
    o.context.now = now;
    o.context.seed = seed;
    return o;
}

} // namespace

class QueryTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    Library *m_library = nullptr;

    // Titles in the query's own order; untitled tracks show their file name.
    QStringList ordered(const QString &query, Query::Options o = pinned()) {
        const Query::Result r = Query::run(*m_library, query, o);
        if (!r.ok())
            qWarning() << query << r.error;
        QStringList out;
        for (const auto &t : r.tracks)
            out << (t.title.isEmpty() ? QString("(untagged)") : t.title);
        return out;
    }
    QStringList titles(const QString &query) {
        QStringList out = ordered(query);
        out.sort();
        return out;
    }
    static QStringList sorted(QStringList list) {
        list.sort();
        return list;
    }
    static QString errorOf(const QString &query) { return Query::compile(query).error; }

private slots:
    void initTestCase() {
        m_library = new Library(m_dir.filePath("library.db"));
        QVERIFY(m_library->open());
        QVERIFY(fill(*m_library));
        QCOMPARE(titles("").size(), 12);
    }
    void cleanupTestCase() { delete m_library; }

    // ---- Parsing ------------------------------------------------------------

    void parsesWordsPhrasesAndWordStarts() {
        Query::Parsed p = Query::parse("+neo \"night drive\" title:+lo +\"neon co\"");
        QVERIFY2(p.ok(), qPrintable(p.error));
        QCOMPARE(p.root.children.size(), 4);
        QVERIFY(p.root.children[0].wordStart);
        QCOMPARE(p.root.children[0].value, QString("neo"));
        QVERIFY(p.root.children[1].quoted);
        QVERIFY(!p.root.children[1].wordStart);
        QCOMPARE(p.root.children[2].field, QString("title"));
        QVERIFY(p.root.children[2].wordStart);
        QCOMPARE(p.root.children[2].value, QString("lo"));
        QVERIFY(p.root.children[3].wordStart && p.root.children[3].quoted);
        QCOMPARE(p.root.children[3].value, QString("neon co"));

        // A lone + or - is a word, and so is anything that starts with a digit.
        p = Query::parse("+ - 3:20 12:00");
        QVERIFY(p.ok());
        QCOMPARE(p.root.children.size(), 4);
        QCOMPARE(p.root.children[2].value, QString("3:20"));
        QVERIFY(p.root.children[2].field.isEmpty());

        // Positions point at each term.
        p = Query::parse("  year:1997 -genre:pop");
        QCOMPARE(p.root.children[0].position, 2);
        QCOMPARE(p.root.children[0].length, 9);
        QCOMPARE(p.root.children[1].children[0].position, 13);
    }

    void parsesOperators() {
        auto term = [](const QString &text) { return Query::parse(text).root; };
        QCOMPARE(term("year:>2000").op, QString(">"));
        QCOMPARE(term("year:>=2000").op, QString(">="));
        QCOMPARE(term("year:<2000").op, QString("<"));
        QCOMPARE(term("year:<=2000").op, QString("<="));
        QCOMPARE(term("artist:=Lia").op, QString("="));
        QCOMPARE(term("title:/^a/").op, QString("~"));
        QCOMPARE(term("year:1990..").value2, QString());
        QCOMPARE(term("year:..1990").value, QString());
        QCOMPARE(term("album:none").op, QString("none"));
        QCOMPARE(term("played:never").op, QString("none"));
        QCOMPARE(term("genre:any").op, QString("any"));
        QCOMPARE(term("genre:ANY").op, QString("any"));
        // Quoted, they are words to look for.
        QCOMPARE(term("album:\"none\"").op, QString(":"));
        QCOMPARE(term("album:\"none\"").value, QString("none"));
        QCOMPARE(term("ARTIST:x").field, QString("artist"));
        QCOMPARE(term("title:\"say \\\"hi\\\"\"").value, QString("say \"hi\""));
        QCOMPARE(term("title:/a\\/b/").value, QString("a/b"));
    }

    void parsesOrAndGrouping() {
        Query::Parsed p = Query::parse("rating:4.. genre:rock OR genre:metal -live");
        QVERIFY(p.ok());
        QCOMPARE(p.root.kind, Query::Node::And);
        QCOMPARE(p.root.children.size(), 3);
        QCOMPARE(p.root.children[1].kind, Query::Node::Or); // OR binds tighter than AND
        QCOMPARE(p.root.children[2].kind, Query::Node::Not);

        for (const QString &text : {"a OR b", "a | b", "a ; b"}) {
            p = Query::parse(text);
            QCOMPARE(p.root.kind, Query::Node::Or);
            QCOMPARE(p.root.children.size(), 2);
        }
        // "or" in lower case, and a semicolon inside a word, are words.
        QCOMPARE(Query::parse("this or that").root.children.size(), 3);
        QCOMPARE(Query::parse("a;b").root.value, QString("a;b"));

        p = Query::parse("(a OR b) (c d)");
        QCOMPARE(p.root.children[0].kind, Query::Node::Or);
        QCOMPARE(p.root.children[1].kind, Query::Node::And);
        p = Query::parse("-(a OR b)");
        QCOMPARE(p.root.kind, Query::Node::Not);
        QCOMPARE(p.root.children[0].kind, Query::Node::Or);
    }

    void parsesSortClauses() {
        Query::Parsed p = Query::parse("neon sort:-rating,artist");
        QVERIFY(p.ok());
        QCOMPARE(p.root.kind, Query::Node::Term); // the clause leaves the tree
        QCOMPARE(p.sort.size(), 2);
        QCOMPARE(p.sort[0].field, QString("rating"));
        QVERIFY(p.sort[0].descending);
        QCOMPARE(p.sort[1].field, QString("artist"));
        QVERIFY(!p.sort[1].descending);

        p = Query::parse("sort:aa,+year,-stars");
        QCOMPARE(p.sort[0].field, QString("albumartist")); // aliases become the field's name
        QCOMPARE(p.sort[1].field, QString("year"));
        QCOMPARE(p.sort[2].field, QString("rating"));
        QCOMPARE(p.root.kind, Query::Node::And);
        QVERIFY(p.root.children.isEmpty());

        p = Query::parse("sort:random");
        QCOMPARE(p.sort[0].field, QString("random"));
        QCOMPARE(p.sort[0].seed, -1);
        p = Query::parse("sort:random:42");
        QCOMPARE(p.sort[0].seed, 42);
        p = Query::parse("sort:randomalbum:7");
        QCOMPARE(p.sort[0].field, QString("randomalbum"));
        QCOMPARE(p.sort[0].seed, 7);
        p = Query::parse("sort:shuffle");
        QCOMPARE(p.sort[0].field, QString("random"));
        // Two sort: clauses add up.
        p = Query::parse("sort:year sort:title");
        QCOMPARE(p.sort.size(), 2);
    }

    void parsesLimitClauses() {
        auto limit = [](const QString &text) {
            const Query::Parsed p = Query::parse(text);
            if (!p.ok())
                qWarning() << text << p.error;
            return p.limit;
        };
        QCOMPARE(limit("limit:50").kind, Query::Limit::Count);
        QCOMPARE(limit("limit:50").value, 50);
        QCOMPARE(limit("limit:2h").kind, Query::Limit::Duration);
        QCOMPARE(limit("limit:2h").value, 7200000);
        QCOMPARE(limit("limit:90m").value, 5400000);
        QCOMPARE(limit("limit:90min").value, 5400000);
        QCOMPARE(limit("limit:1h30m").value, 5400000);
        QCOMPARE(limit("limit:1:30:00").value, 5400000);
        QCOMPARE(limit("limit:2.5hours").value, 9000000);
        QCOMPARE(limit("limit:700mb").kind, Query::Limit::Bytes);
        QCOMPARE(limit("limit:700mb").value, 700LL * 1048576);
        QCOMPARE(limit("limit:4GB").value, 4LL * 1073741824);
        QCOMPARE(limit("limit:1.5g").value, qint64(1.5 * 1073741824));
        QCOMPARE(limit("limit:2h").toString(), QString("2h"));
        QCOMPARE(limit("limit:1:30:15").toString(), QString("1h30m15s"));
        QCOMPARE(limit("limit:700mb").toString(), QString("700mb"));
        QCOMPARE(limit("neon").kind, Query::Limit::None);
    }

    void reportsErrorsWithPositions() {
        struct Case {
            QString text, contains;
            int position; // 1-based, as in the message
        };
        const QList<Case> cases = {
            {"(a b", "Missing ) for the ( here", 1},
            {"a)", "Unexpected )", 2},
            {"a OR", "OR needs something after it", 3},
            {"OR a", "OR needs something before it", 1},
            {"title:\"open", "Missing closing quote", 7},
            {"year:", "year: needs a value", 6},
            {"year:..", "needs a value on at least one side", 6},
            {"title:/abc", "Missing closing /", 7},
            {"title:/a(b/", "Bad regular expression", -1},
            {"()", "Empty ( )", 1},
            {"neon nosuchfield:1", "Unknown field \"nosuchfield\"", 6},
            {"artst:x", "did you mean artist:?", 1},
            {"geenre:rock", "did you mean genre:?", 1},
            {"a year:abc", "year: needs a number, not \"abc\"", 3},
            {"type:polka", "type: is one of music, audiobook, podcast, classical", 1},
            {"title:>3", "title: is text", 1},
            {"year:/9/", "regular expressions only work on text", 1},
            {"year:+19", "+ only works on text", 1},
            {"length:long", "length: needs a length", 1},
            {"size:big", "size: needs a size", 1},
            {"added:soon", "added: needs a date", 1},
            {"added:2024-13", "added: needs a date", 1},
            {"added:=2024", "added: is a date", 1},
            {"(a sort:title)", "sort: applies to the whole query", 4},
            {"-limit:5", "limit: applies to the whole query", 2},
            {"a OR sort:year", "sort: applies to the whole query", 6},
            {"sort:nosuch", "Unknown field \"nosuch\"", 6},
            {"sort:random:x", "needs a whole number as its seed", 6},
            {"sort:year,,title", "sort: has an empty key", 11},
            {"limit:lots", "limit: takes a count", 7},
            {"limit:0", "above 0", 7},
            {"limit:>5", "limit: takes a count", 7},
            {"limit:5 limit:6", "Only one limit: per query", 9},
        };
        for (const Case &c : cases) {
            const QString error = errorOf(c.text);
            QVERIFY2(error.contains(c.contains), qPrintable(c.text + " -> " + error));
            if (c.position > 0)
                QVERIFY2(error.endsWith(QString("(at character %1)").arg(c.position)),
                         qPrintable(c.text + " -> " + error));
        }
        // The tree API gives the offset on its own.
        const Query::Parsed p = Query::parse("abc (def");
        QCOMPARE(p.position, 4);
        const Query::Compiled c = Query::compile(Query::parse("a year:x"));
        QCOMPARE(c.position, 2);
        QVERIFY(!c.error.contains("at character"));
        QCOMPARE(c.where, QString("0"));
        QVERIFY(c.params.isEmpty());
    }

    // ---- Compiling ----------------------------------------------------------

    void compilesToParameterisedSql() {
        Query::Compiled c = Query::compile("rating:4.. year:1990..1999");
        QVERIFY(c.ok());
        QCOMPARE(c.params, (QVariantList{80.0, 1990.0, 1999.0}));
        QVERIFY(c.orderBy.isEmpty());
        QCOMPARE(c.order(), QString(Query::defaultOrder));

        // User text only ever reaches SQL as a parameter.
        const QString nasty = "x'); DROP TABLE tracks; --";
        for (const QString &q : {QString("artist:\"%1\"").arg(nasty), QString("\"%1\"").arg(nasty),
                                 QString("artist:=\"%1\"").arg(nasty), QString("title:/%1/").arg("x'; DROP TABLE tracks; --"),
                                 QString("+\"%1\"").arg(nasty), QString("ab'")}) {
            c = Query::compile(q);
            QVERIFY2(c.ok(), qPrintable(c.error));
            QVERIFY2(!c.where.contains("DROP") && !c.where.contains("ab'"), qPrintable(c.where));
        }

        // Short words cannot use the trigram index; longer ones must.
        QVERIFY(!Query::compile("go").where.contains("tracks_fts"));
        QVERIFY(Query::compile("west").where.contains("tracks_fts"));
        QVERIFY(Query::compile("artist:west").where.contains("tracks_fts"));
        QVERIFY(Query::compile("+west").where.contains("tracks_fts"));
        QVERIFY(Query::compile("+west").where.contains("REGEXP"));
        QVERIFY(!Query::compile("+we").where.contains("tracks_fts"));
        QVERIFY(!Query::compile("path:west").where.contains("tracks_fts")); // not indexed

        QVERIFY(Query::compile("album:none").where.contains("= ''"));
        QVERIFY(Query::compile("rating:none").where.contains("IS NULL"));
        QVERIFY(Query::compile("played:any").where.contains("IS NOT NULL"));
        QVERIFY(Query::compile("plays:none").where.contains("coalesce(t.play_count, 0) = 0"));
        QVERIFY(Query::compile("-year:2000").where.startsWith("NOT coalesce("));
    }

    void compilesDates() {
        Query::Context ctx;
        ctx.now = now;
        auto params = [&](const QString &q) {
            const Query::Compiled c = Query::compile(q, ctx);
            if (!c.ok())
                qWarning() << q << c.error;
            return c.params;
        };
        auto sql = [&](const QString &q) { return Query::compile(q, ctx).where; };
        const qint64 y2024 = QDate(2024, 1, 1).startOfDay().toSecsSinceEpoch();
        const qint64 y2025 = QDate(2025, 1, 1).startOfDay().toSecsSinceEpoch();
        const qint64 jul2024 = QDate(2024, 7, 1).startOfDay().toSecsSinceEpoch();

        QCOMPARE(params("added:<30d"), QVariantList{daysAgo(30)});
        QCOMPARE(sql("added:<30d"), QString("t.added > ?"));
        QCOMPARE(sql("added:30d"), QString("t.added > ?")); // "in the last 30 days"
        QCOMPARE(sql("added:>30d"), QString("t.added < ?"));
        QCOMPARE(sql("added:<=30d"), QString("t.added >= ?"));
        QCOMPARE(sql("added:>=30d"), QString("t.added <= ?"));
        QCOMPARE(params("played:<12h"), QVariantList{now.toSecsSinceEpoch() - 12 * 3600});
        QCOMPARE(params("played:<90min"), QVariantList{now.toSecsSinceEpoch() - 90 * 60});
        QCOMPARE(params("played:<2w"), QVariantList{daysAgo(14)});
        QCOMPARE(params("played:>1y"), QVariantList{now.addYears(-1).toSecsSinceEpoch()});
        QCOMPARE(params("played:>6m"), QVariantList{now.addMonths(-6).toSecsSinceEpoch()});
        QCOMPARE(params("played:>6months"), QVariantList{now.addMonths(-6).toSecsSinceEpoch()});
        QCOMPARE(params("lastplayed:<7d"), QVariantList{daysAgo(7)});

        QCOMPARE(params("added:2024"), (QVariantList{y2024, y2025}));
        QCOMPARE(params("added:2024-01..2024-06"), (QVariantList{y2024, jul2024}));
        QCOMPARE(params("added:2024-06..2024-01"), (QVariantList{y2024, jul2024})); // either way round
        QCOMPARE(params("added:2024-1-10"),
                 (QVariantList{QDate(2024, 1, 10).startOfDay().toSecsSinceEpoch(),
                               QDate(2024, 1, 11).startOfDay().toSecsSinceEpoch()}));
        QCOMPARE(params("added:<2024"), QVariantList{y2024});
        QCOMPARE(params("added:<=2024"), QVariantList{y2025});
        QCOMPARE(params("added:>2024"), QVariantList{y2025});
        QCOMPARE(params("added:>=2024"), QVariantList{y2024});
        QCOMPARE(params("added:2024.."), QVariantList{y2024});
        QCOMPARE(params("added:..2024"), QVariantList{y2025});
        QCOMPARE(params("added:today"),
                 (QVariantList{now.date().startOfDay().toSecsSinceEpoch(),
                               now.date().addDays(1).startOfDay().toSecsSinceEpoch()}));
        QCOMPARE(params("added:yesterday").first(), QVariant(now.date().addDays(-1).startOfDay().toSecsSinceEpoch()));
        // Ages and dates mix in a range: from the earlier moment to the later.
        QCOMPARE(params("added:30d..7d"), (QVariantList{daysAgo(30), daysAgo(7) + 1}));
        QCOMPARE(params("added:2024..30d"), (QVariantList{y2024, daysAgo(30) + 1}));
    }

    void compilesLengthsAndSizes() {
        QCOMPARE(Query::compile("length:>10m").params, QVariantList{600000.0});
        QCOMPARE(Query::compile("length:<200").params, QVariantList{200000.0});
        QCOMPARE(Query::compile("length:>1h30m").params, QVariantList{5400000.0});
        QCOMPARE(Query::compile("length:3:00..5:00").params, (QVariantList{180000.0, 300999.0}));
        QCOMPARE(Query::compile("length:3m..5m").params, (QVariantList{180000.0, 359999.0}));
        QCOMPARE(Query::compile("length:2").params, (QVariantList{2000.0, 2999.0}));
        QCOMPARE(Query::compile("length:4m").params, (QVariantList{240000.0, 299999.0}));
        QCOMPARE(Query::compile("duration:..90s").params, QVariantList{90999.0});
        QCOMPARE(Query::compile("size:>10mb").params, QVariantList{10.0 * 1048576});
        QCOMPARE(Query::compile("size:1gb..").params, QVariantList{1073741824.0});
        QCOMPARE(Query::compile("rating:4").params, (QVariantList{70.0, 89.0}));
        QCOMPARE(Query::compile("rating:4.5").params, (QVariantList{80.0, 99.0}));
    }

    void compilesSortAndLimit() {
        Query::Context ctx;
        ctx.seed = 9;
        Query::Compiled c = Query::compile("sort:-rating,artist limit:50", ctx);
        QVERIFY(c.ok());
        QVERIFY(c.orderBy.startsWith("t.rating DESC NULLS LAST, coalesce(t.artist, '') = '', t.artist COLLATE NOCASE"));
        QVERIFY(c.orderBy.endsWith(", t.id"));
        QCOMPARE(c.limit.kind, Query::Limit::Count);
        QCOMPARE(c.where, QString("1"));
        // A seed in the text wins over the context's.
        QVERIFY(Query::compile("sort:random", ctx).orderBy.contains("t.id + 9)"));
        QVERIFY(Query::compile("sort:random:12", ctx).orderBy.contains("t.id + 12)"));
        QVERIFY(Query::compile("sort:-random", ctx).orderBy.contains("4294967296) DESC"));
    }

    // ---- Running ------------------------------------------------------------

    void runsWordsAndFields() {
        QCOMPARE(titles("neon"), (QStringList{"Afterglow", "Coastline", "Midnight Highway"}));
        QCOMPARE(titles("oast").size(), 3);
        QCOMPARE(titles("bjork"), QStringList{"Tidal"});
        QCOMPARE(titles("\"night drive\"").size(), 3);
        QCOMPARE(titles("untagged"), QStringList{"(untagged)"}); // file names are searched
        QCOMPARE(titles("夜の歌"), QStringList{"夜の歌"});
        QCOMPARE(titles("aa:\"neon coast\"").size(), 3);
        QCOMPARE(titles("ar:lia"), QStringList{"Afterglow"});
        QCOMPARE(titles("artist:=Lia"), QStringList{"Afterglow"});
        QCOMPARE(titles("albumartist:=\"neon coast\"").size(), 3);
        QCOMPARE(titles("al:summer").size(), 3);
        QCOMPARE(titles("genre:=pop"), (QStringList{"Afterglow", "Go West", "Heatwave", "Snow Day"}));
        QCOMPARE(titles("genre:pop").size(), 5); // contains, so J-Pop too
        QCOMPARE(titles("type:audiobook"), QStringList{"Chapter One"});
        QCOMPARE(titles("year:1997..2001").size(), 4);
        QCOMPARE(titles("year:>2003"), (QStringList{"Heatwave", "夜の歌"}));
        QCOMPARE(titles("title:/^(after|coast)/"), (QStringList{"Afterglow", "Coastline"}));
        QCOMPARE(titles("compilation:1").size(), 3);
    }

    void runsWordStarts() {
        QCOMPARE(titles("title:love"), (QStringList{"Glove Box", "Love Song"}));
        QCOMPARE(titles("title:+love"), QStringList{"Love Song"});
        QCOMPARE(titles("ove").size(), 2);
        QCOMPARE(titles("+ove"), QStringList{});
        QCOMPARE(titles("+go"), QStringList{"Go West"});       // short: no index, still exact
        QCOMPARE(titles("+wes"), QStringList{"Go West"});
        QCOMPARE(titles("+\"night dr\"").size(), 3);            // a phrase at a word start
        QCOMPARE(titles("+\"ight dr\""), QStringList{});
        QCOMPARE(titles("+lia"), QStringList{"Afterglow"});     // after "; " in a multi-value field
        QCOMPARE(titles("artist:+club"), QStringList{"Snow Day"});
        QCOMPARE(titles("+夜"), QStringList{"夜の歌"});
    }

    void runsOrAndNot() {
        QCOMPARE(titles("genre:rock ; genre:christmas"), (QStringList{"Glove Box", "Love Song", "Snow Day", "Tidal"}));
        QCOMPARE(titles("genre:rock | genre:christmas"), titles("genre:rock OR genre:christmas"));
        QCOMPARE(titles("summer -genre:christmas"), (QStringList{"Heatwave", "Tidal"}));
        QCOMPARE(titles("-(genre:rock OR genre:pop OR genre:synthwave)"), (QStringList{"(untagged)", "Chapter One"}));
        // NOT keeps tracks whose value is unset: unrated tracks are not 4 stars.
        QCOMPARE(titles("-rating:4..").size(), 9);
        QCOMPARE(titles("-year:1997").size(), 9);
    }

    void runsEmptyAndSetFields() {
        QCOMPARE(titles("album:none"), QStringList{"(untagged)"});
        QCOMPARE(titles("genre:none"), (QStringList{"(untagged)", "Chapter One"}));
        QCOMPARE(titles("genre:any").size(), 10);
        QCOMPARE(titles("-genre:none"), titles("genre:any"));
        QCOMPARE(titles("albumartist:none"), (QStringList{"(untagged)", "Heatwave", "Snow Day", "Tidal"}));
        QCOMPARE(titles("year:none"), (QStringList{"(untagged)", "Chapter One"}));
        QCOMPARE(titles("rating:none"), (QStringList{"(untagged)", "Afterglow", "Chapter One", "Go West", "夜の歌"}));
        QCOMPARE(titles("rating:any").size(), 7);
        QCOMPARE(titles("rating:0"), QStringList{"Glove Box"}); // rated no stars is not unrated
        QCOMPARE(titles("album:\"none\""), QStringList{});      // the word, quoted
    }

    void runsDates() {
        QCOMPARE(titles("added:<30d"), (QStringList{"Heatwave", "Snow Day", "Tidal"}));
        QCOMPARE(titles("added:<7d"), QStringList{"Tidal"});
        QCOMPARE(titles("added:>1y").size(), 9);
        QCOMPARE(titles("added:2024"), (QStringList{"(untagged)", "Chapter One", "Glove Box", "Love Song"}));
        QCOMPARE(titles("added:2023"), QStringList{"夜の歌"}); // 23:30 on New Year's Eve, local time
        QCOMPARE(titles("added:2024-01..2024-06"), (QStringList{"Chapter One", "Glove Box", "Love Song"}));
        QCOMPARE(titles("added:2024-01-10"), (QStringList{"Glove Box", "Love Song"}));
        QCOMPARE(titles("added:2024-07"), QStringList{"(untagged)"});
        QCOMPARE(titles("added:<2024"), QStringList{"夜の歌"});
        QCOMPARE(titles("added:2025-06-30.."),
                 (QStringList{"Afterglow", "Coastline", "Go West", "Heatwave", "Midnight Highway", "Snow Day", "Tidal"}));
        QCOMPARE(titles("added:30d..7d"), (QStringList{"Heatwave", "Snow Day"}));

        const QStringList never{"(untagged)", "Afterglow", "Go West", "Snow Day", "夜の歌"};
        QCOMPARE(titles("played:never"), never);
        QCOMPARE(titles("played:none"), never);
        QCOMPARE(titles("plays:0"), never);
        QCOMPARE(titles("played:any").size(), 7);
        QCOMPARE(titles("played:<7d"), (QStringList{"Midnight Highway", "Tidal"}));
        QCOMPARE(titles("lastplayed:<7d"), titles("played:<7d"));
        QCOMPARE(titles("played:>1y"), (QStringList{"Glove Box", "Heatwave", "Love Song"}));
        QCOMPARE(titles("played:>1y OR played:never").size(), 8);
        QCOMPARE(titles("played:today"), QStringList{});
        QCOMPARE(titles("played:yesterday"), QStringList{"Tidal"});
    }

    void runsCountsAndLengths() {
        QCOMPARE(titles("plays:10.."), (QStringList{"Coastline", "Love Song", "Midnight Highway"}));
        QCOMPARE(titles("plays:1..3"), (QStringList{"Chapter One", "Glove Box", "Heatwave"}));
        QCOMPARE(titles("plays:>=30"), (QStringList{"Coastline", "Love Song"}));
        QCOMPARE(titles("length:>10m"), (QStringList{"Chapter One", "Glove Box"}));
        QCOMPARE(titles("length:10m"), QStringList{"Love Song"});
        QCOMPARE(titles("length:3:00..5:00"),
                 (QStringList{"Coastline", "Go West", "Heatwave", "Midnight Highway", "Tidal", "夜の歌"}));
        QCOMPARE(titles("length:<1:30"), QStringList{"(untagged)"});
        QCOMPARE(titles("length:1h"), QStringList{"Chapter One"});
        QCOMPARE(titles("size:>15mb"), (QStringList{"Chapter One", "Glove Box"}));
        QCOMPARE(titles("size:..4mb"), (QStringList{"(untagged)", "Snow Day", "夜の歌"}));
    }

    void runsSorts() {
        QCOMPARE(ordered("sort:-rating").mid(0, 7), (QStringList{"Coastline", "Tidal", "Midnight Highway", "Heatwave",
                                                                 "Snow Day", "Love Song", "Glove Box"}));
        // Unrated tracks go last both ways round.
        QCOMPARE(ordered("sort:rating").mid(0, 2), (QStringList{"Glove Box", "Love Song"}));
        QCOMPARE(ordered("sort:rating").last(), ordered("sort:-rating").last());
        // Untitled sorts last too.
        QCOMPARE(ordered("sort:title").first(), QString("Afterglow"));
        QCOMPARE(ordered("sort:title").last(), QString("(untagged)"));
        QCOMPARE(ordered("sort:-title").first(), QString("夜の歌"));
        QCOMPARE(ordered("sort:-title").last(), QString("(untagged)"));
        QCOMPARE(ordered("neon sort:-length"), (QStringList{"Afterglow", "Midnight Highway", "Coastline"}));
        QCOMPARE(ordered("rating:any sort:year,-plays").mid(0, 3), (QStringList{"Love Song", "Glove Box", "Coastline"}));
        QCOMPARE(ordered("sort:-added limit:2"), (QStringList{"Tidal", "Heatwave"}));
        QCOMPARE(ordered("sort:played,title limit:2"), (QStringList{"Glove Box", "Heatwave"}));
        // The default: album artist, album, disc, track.
        QCOMPARE(ordered("neon"), (QStringList{"Midnight Highway", "Coastline", "Afterglow"}));
    }

    void randomSortsAreStablePerSeed() {
        const QStringList a = ordered("sort:random", pinned(42));
        QCOMPARE(a.size(), 12);
        QCOMPARE(ordered("sort:random", pinned(42)), a);
        QCOMPARE(ordered("sort:random:42", pinned(7)), a);
        QCOMPARE(sorted(a), sorted(ordered("")));
        // Different seeds give different orders (these two are known to).
        QVERIFY(ordered("sort:random", pinned(7)) != a);
        QVERIFY(ordered("sort:random", pinned(8)) != ordered("sort:random", pinned(7)));
        QVERIFY(a != ordered("") && a != ordered("sort:title"));
        // A shuffle of albums keeps each album together and in order.
        for (quint32 seed : {1u, 2u, 3u, 4u}) {
            const Query::Result r = Query::run(*m_library, "sort:randomalbum", pinned(seed));
            QStringList seen;
            QString last;
            for (const auto &t : r.tracks) {
                if (t.album != last) {
                    QVERIFY2(!seen.contains(t.album), qPrintable(t.album));
                    seen << t.album;
                    last = t.album;
                }
            }
            QCOMPARE(r.tracks.size(), 12);
        }
        QCOMPARE(ordered("album:\"night drive\" sort:randomalbum:5"),
                 (QStringList{"Midnight Highway", "Coastline", "Afterglow"}));
    }

    void limitsByCountLengthAndSize() {
        QCOMPARE(ordered("neon limit:2"), (QStringList{"Midnight Highway", "Coastline"}));
        QCOMPARE(Query::run(*m_library, "limit:2", pinned()).total, 2);
        QCOMPARE(Query::run(*m_library, "neon limit:50", pinned()).total, 3);
        // Tracks in order until the next would go over.
        QCOMPARE(ordered("sort:length limit:10m"), (QStringList{"(untagged)", "Snow Day", "夜の歌", "Coastline"}));
        QCOMPARE(ordered("sort:length limit:9:36"), (QStringList{"(untagged)", "Snow Day", "夜の歌", "Coastline"}));
        QCOMPARE(ordered("sort:length limit:9:35"), (QStringList{"(untagged)", "Snow Day", "夜の歌"}));
        QCOMPARE(ordered("sort:size limit:20mb"), (QStringList{"(untagged)", "夜の歌", "Snow Day", "Heatwave", "Coastline"}));
        QCOMPARE(ordered("sort:-length limit:30m"), QStringList{});   // the first is already too long
        QCOMPARE(ordered("type:audiobook limit:2h"), QStringList{"Chapter One"});
        QCOMPARE(ordered("neon limit:9m"), (QStringList{"Midnight Highway", "Coastline"}));

        // Pages inside a limit.
        Query::Options o = pinned();
        o.limit = 2;
        o.offset = 4;
        Query::Result r = Query::run(*m_library, "limit:5", o);
        QCOMPARE(r.tracks.size(), 1);
        QCOMPARE(r.total, 5);
        o.offset = 1;
        r = Query::run(*m_library, "sort:length limit:10m", o);
        QCOMPARE(r.total, 4);
        QCOMPARE(r.tracks.size(), 2);
        QCOMPARE(r.tracks[0].title, QString("Snow Day"));
        QCOMPARE(r.durationMs, 150000 + 180000);
        o.offset = 10;
        QCOMPARE(Query::run(*m_library, "limit:5", o).tracks.size(), 0);

        // Counting only.
        o = pinned();
        o.countOnly = true;
        r = Query::run(*m_library, "genre:any", o);
        QCOMPARE(r.total, 10);
        QVERIFY(r.tracks.isEmpty());
        QCOMPARE(Query::run(*m_library, "genre:any limit:4", o).total, 4);
        QCOMPARE(Query::run(*m_library, "sort:size limit:20mb", o).total, 5);

        // ids() follows the same order and limit.
        QString error;
        const QList<qint64> ids = Query::ids(*m_library, "sort:size limit:20mb", {now, 0}, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(ids.size(), 5);
        QCOMPARE(ids.first(), Query::run(*m_library, "sort:size limit:1", pinned()).tracks.first().id);
        QCOMPARE(Query::ids(*m_library, "sort:random:3", {now, 0}).size(), 12);
        QCOMPARE(Query::ids(*m_library, "limit:3").size(), 3);
        QVERIFY(Query::ids(*m_library, "year:x", {}, &error).isEmpty());
        QVERIFY(error.contains("needs a number"));
    }

    void tracksCarryTheirDetails() {
        const Query::Result r = Query::run(*m_library, "title:\"midnight highway\"", pinned());
        QCOMPARE(r.tracks.size(), 1);
        const Query::Track &t = r.tracks.first();
        QCOMPARE(t.path, QString("/music/Night Drive/1-01 Midnight Highway.flac"));
        QCOMPARE(t.plays, 12);
        QCOMPARE(t.rating, 80);
        QCOMPARE(t.added, daysAgo(400));
        QCOMPARE(t.lastPlayed, daysAgo(3));
        QCOMPARE(t.size, 8 * 1048576);
        const QJsonObject json = t.toJson();
        QCOMPARE(json["last_played"].toInteger(), daysAgo(3));
        QCOMPARE(json["size"].toInteger(), 8 * 1048576);
        QVERIFY(Query::run(*m_library, "played:never", pinned()).tracks.first().toJson()["last_played"].isNull());
        QVERIFY(Query::run(*m_library, "year:x").error.contains("(at character 1)"));
    }

    void parsedQueriesBecomeJson() {
        const QJsonObject o = Query::parse("genre:any -album:none sort:random:4,-year limit:2h").toJson();
        QCOMPARE(o["where"].toObject()["kind"].toString(), QString("and"));
        const QJsonArray children = o["where"].toObject()["children"].toArray();
        QCOMPARE(children[0].toObject()["op"].toString(), QString("any"));
        QVERIFY(!children[0].toObject().contains("value"));
        QCOMPARE(o["sort"].toArray()[0].toObject()["seed"].toInt(), 4);
        QCOMPARE(o["sort"].toArray()[1].toObject()["descending"].toBool(), true);
        QCOMPARE(o["limit"].toObject()["duration_ms"].toInteger(), 7200000);
        QVERIFY(Query::parse("+abc").toJson()["where"].toObject()["word_start"].toBool());
    }

    // ---- The command line ---------------------------------------------------

    void cliRunsTheWholeLanguage() {
        const QString db = m_dir.filePath("library.db");
        QTemporaryDir fakeDir;
        // A machine with nothing mounted: the volume goes offline, which
        // changes paths but not which tracks a query selects.
        const VolumeResolver resolver(fakeDir.filePath("mountinfo"), fakeDir.filePath("by-uuid"));
        auto cli = [&](const QStringList &args, QJsonObject *json, QString *text = nullptr) {
            QString outText, errText;
            QTextStream out(&outText), err(&errText);
            const int code = Cli::run(args, out, err, &resolver);
            out.flush();
            err.flush();
            if (json)
                *json = QJsonDocument::fromJson(outText.toUtf8()).object();
            if (text)
                *text = outText + errText;
            return code;
        };
        QJsonObject json;
        QString text;
        QCOMPARE(cli({"query", "--count", "--db", db, "genre:any"}, nullptr, &text), 0);
        QCOMPARE(text.trimmed(), QString("10"));
        QCOMPARE(cli({"query", "--count", "--json", "--db", db, "genre:any", "limit:3"}, &json), 0);
        QCOMPARE(json["total"].toInt(), 3);

        QCOMPARE(cli({"query", "--json", "--db", db, "sort:-plays", "limit:2"}, &json), 0);
        QCOMPARE(json["count"].toInt(), 2);
        QCOMPARE(json["tracks"].toArray()[0].toObject()["title"].toString(), QString("Love Song"));
        QCOMPARE(json["duration_ms"].toInteger(), 600000 + 185000);

        // --seed repeats a shuffle; the JSON reports the seed it used.
        QCOMPARE(cli({"query", "--json", "--db", db, "sort:random"}, &json), 0);
        const qint64 seed = json["seed"].toInteger();
        const QJsonArray first = json["tracks"].toArray();
        QCOMPARE(cli({"query", "--json", "--seed", QString::number(seed), "--db", db, "sort:random"}, &json), 0);
        QCOMPARE(json["tracks"].toArray(), first);

        QCOMPARE(cli({"query", "--json", "--offset", "1", "--limit", "1", "--db", db, "neon"}, &json), 0);
        QCOMPARE(json["tracks"].toArray()[0].toObject()["title"].toString(), QString("Coastline"));
        QCOMPARE(json["total"].toInt(), 3);

        QCOMPARE(cli({"query", "--explain", "--json", "--db", db, "rating:4..", "sort:-year", "limit:2h"}, &json), 0);
        QCOMPARE(json["where"].toString(), QString("t.rating >= ?"));
        QVERIFY(json["order_by"].toString().startsWith("t.year DESC"));
        QCOMPARE(json["limit"].toString(), QString("2h"));
        QCOMPARE(json["params"].toArray()[0].toDouble(), 80.0);

        QCOMPARE(cli({"query", "--db", db, "neon"}, nullptr, &text), 0);
        QVERIFY2(text.contains("3 of 3 tracks, 12:30, 24.0 MB"), qPrintable(text));

        QCOMPARE(cli({"query", "--json", "--db", db, "added:soon"}, &json, &text), 1);
        QCOMPARE(json["ok"].toBool(), false);
        QVERIFY(json["error"].toString().contains("(at character 1)"));
        QCOMPARE(cli({"query", "--seed", "x", "--db", db, "a"}, nullptr, &text), 1);
        QVERIFY(text.contains("--seed needs a number"));
    }
};

int runQueryTests(int argc, char **argv) {
    QueryTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "query_tests.moc"
