#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class Library;

// The mask language: MediaMonkey's Auto-Organize masks with a few of
// foobar2000's title-formatting functions. docs/masks.md is the reference.
//
//   <Album Artist>/<Album> (<Year>)/<Disc#>-<Track#:2> <Title>
//   $If(<Artist>=<Album Artist>,,<Artist> - )<Title>
//   <Artist@1>/$MovePrefix(<Artist>)/<Album>
//
// A mask is parsed once into a list of nodes and evaluated against the
// values of one track. In path mode, '/' and '\' written in the mask are
// folder separators, while the same characters inside a tag value are
// replaced, so an artist called "AC/DC" never makes a folder. Each folder
// and file name is then made safe for the filesystem (see Sanitise).
//
// The reverse, tagging from file names, matches a pattern such as
// `<Album>/<Track#> - <Artist> - <Title>` against the end of a file's path.
namespace Mask {

// What makes a folder or file name safe. The defaults suit a disk shared
// with Windows or a NAS over SMB: characters Windows refuses are replaced
// too, as are trailing dots and spaces and names like CON and NUL.
struct Sanitise {
    QString replacement = "_";   // stands in for each unsafe character
    bool windowsSafe = true;     // also replace < > : " | ? * and avoid reserved names
    int maxComponentBytes = 255; // per folder or file name, in UTF-8 bytes (ext4, btrfs, SMB)
    int maxPathChars = 0;        // the whole relative path; 0 means no limit
};

// The values a mask is evaluated against, keyed by canonical field name
// (canonicalField()). fromTrack() fills them from the library; tests and
// the window can fill them directly.
struct Values {
    QHash<QString, QString> fields;
    QString path;     // the file's absolute path (for <Folder:n>, <Filename>, <Extension>)
    QString rootPath; // the library folder holding it (for <Path:n>), or its volume's root
    bool inLibraryFolder = false; // rootPath is a library folder, not just the volume

    QString value(const QString &field) const { return fields.value(field); }
};

// A track's values. <Album Artist> falls back to "Various Artists" for a
// compilation and to the artist otherwise, as the library's sort order does.
Values fromTrack(Library &library, qint64 trackId);

// "Album Artist", "albumartist", "album_artist" all give "albumartist";
// "Track#" gives "track". Empty for an unknown name.
QString canonicalField(const QString &name);
// The names a mask can use, as written in docs/masks.md.
QStringList fieldNames();
QStringList functionNames();
// Fields holding numbers: ":n" zero-pads them.
bool isNumericField(const QString &canonical);

struct Node {
    enum Kind { Text, Separator, Field, Function };
    Kind kind = Text;
    QString text;    // Text: the literal; Field: canonical name; Function: lower-case name
    int width = -1;  // <Field:n>: pad numbers, cut text; for folder and path, the index
    int group = -1;  // <Field@n>: alphabetical group of n letters
    int position = 0;
    QList<QList<Node>> args; // Function only
    // $If's condition, when it compares: left op right, with op '=' or '!='.
    QString compare;
    QList<Node> left, right;
};

struct Parsed {
    QList<Node> nodes;
    QString error;
    int position = -1; // character offset of the error
    bool ok() const { return error.isEmpty(); }
};

// allowSkip admits <Skip>, which only patterns for tagging from file names use.
Parsed parse(const QString &mask, bool allowSkip = false);

struct Context {
    bool pathMode = true;  // separators make folders and each part is sanitised
    Sanitise sanitise;
    int autoNumber = 0;    // what <Auto Number> gives; organise numbers the batch from 1
};

// Evaluates a parsed mask. In path mode the result is a relative path with
// '/' between parts, every part sanitised and empty folders dropped.
QString evaluate(const Parsed &mask, const Values &values, const Context &context = {});
// Parses and evaluates; on a parse error returns empty and sets error.
QString evaluate(const QString &mask, const Values &values, const Context &context, QString *error);

// One folder or file name made safe. keepExtension keeps the text after the
// last dot when the name has to be cut to fit.
QString sanitiseComponent(const QString &name, const Sanitise &rules, bool keepExtension = false);

// Tagging from file names.
struct FilenameMatch {
    bool ok = false;
    QString error;                 // a pattern error, or why the path did not match
    int position = -1;
    QHash<QString, QString> values; // canonical field -> text from the path
};

// Matches a pattern against the end of a path (without its extension). The
// pattern's separators decide how many folders it covers. Fields allowed:
// any text or number tag; <Skip> matches anything and keeps nothing.
FilenameMatch matchFilename(const QString &pattern, const QString &path);
// The fields a tag-from-filename pattern may fill.
bool isTaggableField(const QString &canonical);

}
