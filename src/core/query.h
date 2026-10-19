#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVariantList>

class Library;

// The query language: search box, auto-playlists, collections and the CLI.
//
//   neon coast                   words anywhere (AND)
//   "neon coast"                 a phrase
//   artist:"neon coast"          a field contains
//   artist:=Björk                a field is exactly (any one value of a multi-value field)
//   year:1997..2001  rating:4..  ranges; year:>2000, plays:<=3 comparisons
//   -genre:christmas             NOT
//   genre:rock OR genre:metal    OR (binds tighter than AND, as in MediaMonkey)
//   title:/^live/                regular expression (case-insensitive)
//   ( ... )                      grouping
//
// Parsing builds a tree (Query::Node) and compiling turns the tree into a
// parameterised SQL condition over `tracks t`. The two are separate so the
// window can show or edit the tree (a rule builder) and so new fields and
// operators only touch the field table and compileTerm().
namespace Query {

struct Node {
    enum Kind { And, Or, Not, Term };
    Kind kind = And;
    QList<Node> children;

    // Term only. field is empty for words that search every text field.
    QString field;
    // ':' contains or equals (by field type), '=' exactly, '<' '<=' '>' '>=',
    // '..' range (value..value2, either side may be empty), '~' regex.
    QString op = ":";
    QString value, value2;
    bool quoted = false;

    QJsonObject toJson() const;
};

struct Parsed {
    Node root;
    QString error;
    int position = -1; // character offset of the error
    bool ok() const { return error.isEmpty(); }
};

Parsed parse(const QString &text);

struct Field {
    enum Type { Text, MultiText, Number, Rating, Duration, Enum };
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

struct Compiled {
    QString where = "1";
    QVariantList params;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

Compiled compile(const Node &root);
Compiled compile(const QString &text);

struct Track {
    qint64 id = 0;
    QString path; // absolute; empty when the volume is offline
    QString title, artist, albumArtist, album, genre, format, availability;
    int year = 0, disc = 0, track = 0;
    qint64 durationMs = 0;
    int rating = -1; // 0..100, -1 unrated
    int plays = 0;

    QJsonObject toJson() const;
};

struct Options {
    int limit = 0; // 0 means no limit
    int offset = 0;
    bool countTotal = true;
};

struct Result {
    QList<Track> tracks;
    qint64 total = 0;
    QString error;
    qint64 elapsedUs = 0;
    bool ok() const { return error.isEmpty(); }
};

// Runs a query, sorted by album artist, album, disc and track.
Result run(Library &library, const QString &text, const Options &options = {});

}
