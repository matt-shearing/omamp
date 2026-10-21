#pragma once

#include "http.h"

#include <QJsonArray>
#include <QObject>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// "About the artist" and "About the album" for the sidebar, from public
// data, and the notes that come packaged with an album.
//
// The web route follows links that people have checked rather than
// guessing from names: MusicBrainz (the artist, or the release group) →
// its Wikidata relation → the Wikipedia article in the reader's language
// (English when there is none) → Wikipedia's REST summary: the first
// paragraph, a picture and the article's address. A name is matched on
// MusicBrainz only when the search scores it 100 and the names agree.
// Every answer is cached in the HTTP cache (MusicBrainz for a week,
// Wikidata and Wikipedia for 30 days), each service at its own polite pace,
// with Thrum's User-Agent.
//
// Packaged notes are text files an album carries in its folder: album.txt,
// info.txt, notes.md, liner notes.*, README*, credits.txt, *.nfo.
namespace About {

struct Summary {
    QString title;       // the article's title
    QString description; // Wikipedia's one-line description
    QString extract;     // the first paragraph, plain text
    QString pageUrl;     // the article, for "Read more"
    QString imageUrl;    // the picture on the web
    QString imageFile;   // the picture saved in the cache, or empty
    QString lang;        // the article's language
    QString wikidataId;  // Q…
    QString mbid;        // the MusicBrainz artist or release group
    bool found() const { return !extract.isEmpty(); }
    QJsonObject toJson() const;
};

struct Note {
    QString name; // the file's name
    QString path;
    QString text;
};

struct Credit {
    QString role, name;
};

// True for a file name that holds an album's own notes.
bool isNoteFile(const QString &fileName);
// The notes in an album's folder (and, for a "CD1"-style folder, the
// folder above), best names first, each cut to maxBytes. Text that isn't
// UTF-8 is read as Latin-1 (old .nfo files).
QList<Note> packagedNotes(const QString &folder, int maxBytes = 64 * 1024);

// The language code Wikipedia uses for a locale name: "pt_BR" → "pt",
// "zh_TW" → "zh", "" → "en".
QString wikiLanguage(const QString &locale);

// The Wikidata item named by a MusicBrainz entity's url relations, and a
// direct Wikipedia link when there is one.
QString wikidataFromRelations(const QJsonArray &relations, QString *wikipediaUrl = nullptr);
// "Q1299" from "https://www.wikidata.org/wiki/Q1299".
QString wikidataIdFromUrl(const QString &url);

class Client {
public:
    // cacheDir holds the pictures (empty: ~/.cache/thrum/about).
    explicit Client(Net::HttpClient &http, const QString &cacheDir = {});
    static QString defaultCacheDir();

    // MusicBrainz: an artist's id for a name (score 100, same name), or empty.
    QString findArtist(const QString &name);
    // MusicBrainz: a release group's id for an artist and album, or empty.
    QString findReleaseGroup(const QString &artist, const QString &album);
    // MusicBrainz: the release group of a release.
    QString releaseGroupOf(const QString &releaseId);
    // MusicBrainz: an entity ("artist", "release-group") with its url
    // relations; empty on failure.
    QJsonObject withUrls(const QString &entity, const QString &mbid);
    // Wikidata: the article title in lang, else English; usedLang says which.
    QString wikipediaTitle(const QString &qid, const QString &lang, QString *usedLang);
    // Wikipedia: the REST summary of an article; a disambiguation page is
    // no answer.
    Summary summary(const QString &lang, const QString &title);

    // The whole route for an artist (by MusicBrainz id when known, else by
    // name) and for an album (by release id when known, else by names).
    Summary artist(const QString &name, const QString &mbArtistId, const QString &lang);
    Summary album(const QString &artist, const QString &album, const QString &mbReleaseId, const QString &lang);

    // Credits from MusicBrainz: the recording's producers and engineers,
    // its ISRCs, and the release's labels.
    QList<Credit> credits(const QString &recordingId, const QString &releaseId);

    bool offline() const { return m_offline; }
    QString lastError() const { return m_error; }

    static constexpr int wikiCacheSeconds = 30 * 86400;

private:
    QJsonObject musicBrainz(const QString &path, const QList<QPair<QString, QString>> &query);
    void savePicture(Summary *s);
    void note(const Net::Response &r);

    Net::HttpClient &m_http;
    QString m_dir;
    bool m_offline = false;
    QString m_error;
};

}
