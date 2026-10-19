#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVariantList>

class Library;

// The query language: search box, auto-playlists, collections, Auto-DJ,
// sync lists and the CLI. docs/query-language.md is the full reference.
//
//   neon coast                   words anywhere (AND)
//   "neon coast"                 a phrase
//   +neo                         a word that starts with "neo"
//   artist:"neon coast"          a field contains
//   artist:=Björk                a field is exactly (any one value of a multi-value field)
//   year:1997..2001  rating:4..  ranges; year:>2000, plays:<=3 comparisons
//   length:>10m  length:3:00..5:00  size:>20mb
//   added:<30d  added:2024  added:2024-01..2024-06  played:never  played:>1y
//   album:none  genre:any        empty and non-empty fields
//   -genre:christmas             NOT
//   genre:rock OR genre:metal    OR (binds tighter than AND, as in MediaMonkey); also | and ;
//   title:/^live/                regular expression (case-insensitive)
//   ( ... )                      grouping
//   sort:-rating,artist          sort keys; - means descending; sort:random, sort:random:42
//   limit:50  limit:2h  limit:700mb   at most 50 tracks, two hours, 700 MB
//
// Parsing builds a tree (Query::Node) plus the sort and limit clauses, and
// compiling turns the tree into a parameterised SQL condition over `tracks t`
// and the clauses into an ORDER BY and a limit. The two are separate so the
// window can show or edit the tree (a rule builder) and so new fields and
// operators only touch the field table and the compiler.
namespace Query {

struct Node {
    enum Kind { And, Or, Not, Term };
    Kind kind = And;
    QList<Node> children;

    // Term only. field is empty for words that search every text field.
    QString field;
    // ':' contains or equals (by field type), '=' exactly, '<' '<=' '>' '>=',
    // '..' range (value..value2, either side may be empty), '~' regex,
    // 'none' empty or unset, 'any' set to something.
    QString op = ":";
    QString value, value2;
    bool quoted = false;
    bool wordStart = false; // +word: matches at the start of a word

    // Where the term is in the text, for error messages.
    int position = -1;
    int length = 0;

    QJsonObject toJson() const;
};

struct SortKey {
    // A field name from fields(), or "random" or "randomalbum".
    QString field;
    bool descending = false;
    qint64 seed = -1; // random only; -1 means the caller's seed (Context::seed)
};

struct Limit {
    enum Kind { None, Count, Duration, Bytes };
    Kind kind = None;
    qint64 value = 0; // tracks, milliseconds or bytes
    QString toString() const;
};

struct Parsed {
    Node root;
    QList<SortKey> sort;
    Limit limit;
    QString error;
    int position = -1; // character offset of the error
    bool ok() const { return error.isEmpty(); }
    QJsonObject toJson() const;
};

Parsed parse(const QString &text);

struct Field {
    enum Type { Text, MultiText, Number, Rating, Duration, Enum, Date, Bytes };
    QString name;
    QStringList aliases;
    Type type;
    QString column;     // tracks column
    QString ftsColumn;  // column in tracks_fts, when the field is indexed
    QString role;       // track_artists role for exact matches on people
    QStringList values; // allowed values for Enum
};

const QList<Field> &fields();
const Field *field(const QString &name);

// What a query is evaluated against: the time "30d ago" counts back from, and
// the seed sort:random uses when the query does not name one. Tests pin both.
struct Context {
    QDateTime now;      // invalid means the current time
    quint32 seed = 0;
};

struct Compiled {
    QString where = "1";
    QVariantList params;
    // The ORDER BY expressions over `tracks t`, without the keywords. Empty
    // means the library order (defaultOrder), which the tracks_order index serves.
    QString orderBy;
    Limit limit;
    QString error;
    int position = -1;
    bool ok() const { return error.isEmpty(); }
    QString order() const;
};

// Album artist, album, disc, track: the order of the tracks_order index.
extern const char *const defaultOrder;

Compiled compile(const Node &root, const Context &context = {});
Compiled compile(const Parsed &parsed, const Context &context = {});
Compiled compile(const QString &text, const Context &context = {});

struct Track {
    qint64 id = 0;
    QString path; // absolute; empty when the volume is offline
    QString title, artist, albumArtist, album, genre, format, availability;
    int year = 0, disc = 0, track = 0;
    qint64 durationMs = 0;
    qint64 size = 0;
    int rating = -1; // 0..100, -1 unrated
    int plays = 0;
    qint64 added = 0, lastPlayed = 0; // Unix seconds; 0 when never played

    QJsonObject toJson() const;
};

struct Options {
    // One page of the query's result. The query's own limit: clause applies
    // first; these page through what it leaves.
    int limit = 0; // 0 means no limit
    int offset = 0;
    bool countTotal = true;
    bool countOnly = false; // fill total and nothing else
    Context context;
};

struct Result {
    QList<Track> tracks;
    qint64 total = 0;        // every track the query selects, after its limit: clause
    qint64 durationMs = 0;   // of the tracks returned
    QString error;
    qint64 elapsedUs = 0;
    bool ok() const { return error.isEmpty(); }
};

// Runs a query. Without a sort: clause the order is album artist, album,
// disc and track.
Result run(Library &library, const QString &text, const Options &options = {});

// The ids a query selects, in order, after its sort: and limit: clauses.
// For auto-playlists, Auto-DJ and sync lists.
QList<qint64> ids(Library &library, const QString &text, const Context &context = {}, QString *error = nullptr);

}
