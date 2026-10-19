#include "migrations.h"

// Times are Unix seconds, except file modification times, which keep the
// filesystem's nanoseconds so a rescan can compare them exactly.
//
// Ratings are 0 to 100 (20 per star, so half stars fit), NULL when unrated,
// the same scale as MediaMonkey and the POPM/FMPS tags.

static const QStringList version1 = {
    R"(CREATE TABLE settings (
        key TEXT PRIMARY KEY,
        value TEXT
    ))",

    // A volume is a filesystem (local) or a share (network). Its key never
    // changes; root_path is wherever it is mounted right now.
    R"(CREATE TABLE volumes (
        id INTEGER PRIMARY KEY,
        key TEXT NOT NULL UNIQUE,
        kind TEXT NOT NULL CHECK (kind IN ('local', 'network')),
        label TEXT,
        fs_type TEXT,
        source TEXT,
        mount_subpath TEXT NOT NULL DEFAULT '',
        root_path TEXT NOT NULL,
        online INTEGER NOT NULL DEFAULT 1,
        last_seen INTEGER,
        added INTEGER NOT NULL
    ))",

    // Every folder that holds tracks, plus the library roots the user added.
    R"(CREATE TABLE folders (
        id INTEGER PRIMARY KEY,
        volume_id INTEGER NOT NULL REFERENCES volumes(id) ON DELETE CASCADE,
        rel_path TEXT NOT NULL,
        mtime_ns INTEGER,
        is_root INTEGER NOT NULL DEFAULT 0,
        added INTEGER NOT NULL,
        UNIQUE (volume_id, rel_path)
    ))",

    R"(CREATE TABLE artwork (
        id INTEGER PRIMARY KEY,
        hash TEXT NOT NULL UNIQUE,
        mime TEXT,
        width INTEGER,
        height INTEGER,
        source TEXT,
        cache_path TEXT
    ))",

    R"(CREATE TABLE albums (
        id INTEGER PRIMARY KEY,
        title TEXT NOT NULL,
        album_artist TEXT,
        artist_key TEXT NOT NULL,
        title_key TEXT NOT NULL,
        year INTEGER,
        compilation INTEGER NOT NULL DEFAULT 0,
        artwork_id INTEGER REFERENCES artwork(id) ON DELETE SET NULL,
        mb_release_id TEXT,
        UNIQUE (artist_key, title_key)
    ))",

    R"(CREATE TABLE tracks (
        id INTEGER PRIMARY KEY,
        volume_id INTEGER NOT NULL REFERENCES volumes(id),
        folder_id INTEGER REFERENCES folders(id),
        rel_path TEXT NOT NULL,
        file_name TEXT NOT NULL,
        format TEXT,
        size INTEGER NOT NULL,
        mtime_ns INTEGER NOT NULL,

        title TEXT,
        artist TEXT,
        album_artist TEXT,
        sort_artist TEXT,
        album TEXT,
        album_id INTEGER REFERENCES albums(id) ON DELETE SET NULL,
        composer TEXT,
        genre TEXT,
        year INTEGER,
        original_year INTEGER,
        track_no INTEGER,
        track_total INTEGER,
        disc_no INTEGER,
        disc_total INTEGER,
        compilation INTEGER NOT NULL DEFAULT 0,
        comment TEXT,
        grouping TEXT,
        publisher TEXT,
        isrc TEXT,
        bpm INTEGER,
        mb_track_id TEXT,
        mb_release_id TEXT,
        mb_artist_id TEXT,

        duration_ms INTEGER,
        bitrate INTEGER,
        sample_rate INTEGER,
        channels INTEGER,

        rating INTEGER CHECK (rating IS NULL OR rating BETWEEN 0 AND 100),
        play_count INTEGER NOT NULL DEFAULT 0,
        skip_count INTEGER NOT NULL DEFAULT 0,
        last_played INTEGER,
        added INTEGER NOT NULL,
        updated INTEGER,

        rg_track_gain REAL,
        rg_track_peak REAL,
        rg_album_gain REAL,
        rg_album_peak REAL,

        content_type TEXT NOT NULL DEFAULT 'music'
            CHECK (content_type IN ('music', 'audiobook', 'podcast', 'classical')),
        custom1 TEXT, custom2 TEXT, custom3 TEXT, custom4 TEXT, custom5 TEXT,
        availability TEXT NOT NULL DEFAULT 'online'
            CHECK (availability IN ('online', 'offline', 'missing')),
        artwork_id INTEGER REFERENCES artwork(id) ON DELETE SET NULL,

        UNIQUE (volume_id, rel_path)
    ))",
    "CREATE INDEX tracks_album ON tracks(album_id)",
    "CREATE INDEX tracks_folder ON tracks(folder_id)",
    "CREATE INDEX tracks_availability ON tracks(availability)",
    "CREATE INDEX tracks_year ON tracks(year)",
    "CREATE INDEX tracks_rating ON tracks(rating)",
    "CREATE INDEX tracks_added ON tracks(added)",
    "CREATE INDEX tracks_order ON tracks(sort_artist COLLATE NOCASE, album COLLATE NOCASE, disc_no, track_no)",

    // People. Artist, album artist and composer are multi-value, as in
    // MediaMonkey, so a duet shows under both singers.
    R"(CREATE TABLE artists (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL UNIQUE COLLATE NOCASE,
        sort_name TEXT,
        mb_artist_id TEXT
    ))",
    R"(CREATE TABLE track_artists (
        track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
        artist_id INTEGER NOT NULL REFERENCES artists(id) ON DELETE CASCADE,
        role TEXT NOT NULL CHECK (role IN ('artist', 'album_artist', 'composer')),
        position INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (track_id, role, artist_id)
    ) WITHOUT ROWID)",
    "CREATE INDEX track_artists_artist ON track_artists(artist_id, role)",

    // Genres are multi-value too, but they are not people, so they get their
    // own table rather than a role in track_artists.
    R"(CREATE TABLE genres (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL UNIQUE COLLATE NOCASE
    ))",
    R"(CREATE TABLE track_genres (
        track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
        genre_id INTEGER NOT NULL REFERENCES genres(id) ON DELETE CASCADE,
        position INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (track_id, genre_id)
    ) WITHOUT ROWID)",
    "CREATE INDEX track_genres_genre ON track_genres(genre_id)",

    // kind is 'static' or 'auto'; an auto-playlist keeps its query text.
    R"(CREATE TABLE playlists (
        id INTEGER PRIMARY KEY,
        parent_id INTEGER REFERENCES playlists(id) ON DELETE CASCADE,
        name TEXT NOT NULL,
        kind TEXT NOT NULL DEFAULT 'static' CHECK (kind IN ('static', 'auto', 'folder')),
        query TEXT,
        sort TEXT,
        item_limit INTEGER,
        created INTEGER NOT NULL,
        modified INTEGER
    ))",
    R"(CREATE TABLE playlist_items (
        id INTEGER PRIMARY KEY,
        playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
        track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
        position INTEGER NOT NULL
    ))",
    "CREATE INDEX playlist_items_order ON playlist_items(playlist_id, position)",
    "CREATE INDEX playlist_items_track ON playlist_items(track_id)",

    // A collection is a saved query with its own columns, tree and sort.
    R"(CREATE TABLE collections (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL UNIQUE,
        query TEXT NOT NULL DEFAULT '',
        columns TEXT,
        tree TEXT,
        sort TEXT,
        view TEXT,
        visible TEXT NOT NULL DEFAULT 'always' CHECK (visible IN ('always', 'never', 'nonempty')),
        position INTEGER NOT NULL DEFAULT 0
    ))",
    "INSERT INTO collections (name, query, position) VALUES ('Music', 'type:music', 0)",
    "INSERT INTO collections (name, query, position) VALUES ('Classical', 'type:classical', 1)",
    "INSERT INTO collections (name, query, position) VALUES ('Audiobooks', 'type:audiobook', 2)",
    "INSERT INTO collections (name, query, position) VALUES ('Podcasts', 'type:podcast', 3)",

    R"(CREATE TABLE play_history (
        id INTEGER PRIMARY KEY,
        track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
        played_at INTEGER NOT NULL,
        played_ms INTEGER,
        skipped INTEGER NOT NULL DEFAULT 0
    ))",
    "CREATE INDEX play_history_track ON play_history(track_id, played_at)",
    "CREATE INDEX play_history_time ON play_history(played_at)",

    // Every tag change, so any edit, batch or web lookup can be undone.
    // batch groups the changes made by one action.
    R"(CREATE TABLE tag_journal (
        id INTEGER PRIMARY KEY,
        batch TEXT NOT NULL,
        track_id INTEGER REFERENCES tracks(id) ON DELETE SET NULL,
        field TEXT NOT NULL,
        old_value TEXT,
        new_value TEXT,
        source TEXT,
        changed_at INTEGER NOT NULL,
        undone INTEGER NOT NULL DEFAULT 0
    ))",
    "CREATE INDEX tag_journal_batch ON tag_journal(batch)",
    "CREATE INDEX tag_journal_track ON tag_journal(track_id)",

    R"(CREATE TABLE jobs (
        id INTEGER PRIMARY KEY,
        kind TEXT NOT NULL,
        state TEXT NOT NULL CHECK (state IN ('queued', 'running', 'done', 'failed', 'cancelled')),
        args TEXT,
        result TEXT,
        progress INTEGER NOT NULL DEFAULT 0,
        total INTEGER NOT NULL DEFAULT 0,
        error TEXT,
        created INTEGER NOT NULL,
        started INTEGER,
        finished INTEGER
    ))",
};

// Search. An external-content FTS5 table over tracks with the trigram
// tokeniser, so any three or more letters match inside a word ("oast" finds
// "Neon Coast"), ignoring case and accents. Triggers keep it in step; the
// update trigger fires only when a searched column changes, so play counts
// and availability updates never touch the index.
static const QStringList version2 = {
    R"(CREATE VIRTUAL TABLE tracks_fts USING fts5(
        title, artist, album, album_artist, composer, genre, file_name,
        content = 'tracks', content_rowid = 'id',
        tokenize = 'trigram remove_diacritics 1'
    ))",
    R"(CREATE TRIGGER tracks_fts_insert AFTER INSERT ON tracks BEGIN
        INSERT INTO tracks_fts (rowid, title, artist, album, album_artist, composer, genre, file_name)
        VALUES (new.id, new.title, new.artist, new.album, new.album_artist, new.composer, new.genre, new.file_name);
    END)",
    R"(CREATE TRIGGER tracks_fts_delete AFTER DELETE ON tracks BEGIN
        INSERT INTO tracks_fts (tracks_fts, rowid, title, artist, album, album_artist, composer, genre, file_name)
        VALUES ('delete', old.id, old.title, old.artist, old.album, old.album_artist, old.composer, old.genre, old.file_name);
    END)",
    R"(CREATE TRIGGER tracks_fts_update
        AFTER UPDATE OF title, artist, album, album_artist, composer, genre, file_name ON tracks BEGIN
        INSERT INTO tracks_fts (tracks_fts, rowid, title, artist, album, album_artist, composer, genre, file_name)
        VALUES ('delete', old.id, old.title, old.artist, old.album, old.album_artist, old.composer, old.genre, old.file_name);
        INSERT INTO tracks_fts (rowid, title, artist, album, album_artist, composer, genre, file_name)
        VALUES (new.id, new.title, new.artist, new.album, new.album_artist, new.composer, new.genre, new.file_name);
    END)",
    // Tracks added before this step get indexed now.
    "INSERT INTO tracks_fts (tracks_fts) VALUES ('rebuild')",
};

// Phase 1C: more from the tags, moved-file identity and the artwork cache.
//
// - tag_version records which TagReader read a row. Rows read before this
//   step are 0, so the next scan reads them again once (even unchanged) to
//   pick up the new fields; after that a rescan is stat-only again.
// - tag_rating and tag_play_count keep what the file itself says. The
//   library's own rating and play_count are filled from them only while
//   empty, and only when the file's value is new, so a rating the user
//   cleared in Omamp is not brought back by every rescan.
// - quick_hash is a hash of 64 KiB from the middle of the file, which
//   identifies a moved file even when its tags are empty.
// - albums.art_checked is when the album was last searched for artwork; NULL
//   means look again (the scanner clears it when the album's tracks change).
static const QStringList version3 = {
    "ALTER TABLE tracks ADD COLUMN tag_version INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE tracks ADD COLUMN has_embedded_art INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE tracks ADD COLUMN has_lyrics INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE tracks ADD COLUMN mb_release_group_id TEXT",
    "ALTER TABLE tracks ADD COLUMN mb_album_artist_id TEXT",
    "ALTER TABLE tracks ADD COLUMN tag_rating INTEGER",
    "ALTER TABLE tracks ADD COLUMN tag_play_count INTEGER",
    "ALTER TABLE tracks ADD COLUMN quick_hash TEXT",
    "CREATE INDEX tracks_size ON tracks(size)",
    "CREATE INDEX tracks_quick_hash ON tracks(quick_hash) WHERE quick_hash IS NOT NULL",
    "ALTER TABLE albums ADD COLUMN mb_release_group_id TEXT",
    "ALTER TABLE albums ADD COLUMN art_checked INTEGER",
    "ALTER TABLE artwork ADD COLUMN added INTEGER",
};

const QList<Migration> &migrations() {
    static const QList<Migration> all = {
        {1, "library tables", version1},
        {2, "trigram search index", version2},
        {3, "tag extras, moved files, artwork cache", version3},
    };
    return all;
}
