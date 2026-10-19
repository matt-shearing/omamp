#include "playlistscontroller.h"

#include "librarycontroller.h"
#include "library.h"
#include "m3u.h"
#include "player.h"
#include "playlists.h"
#include "query.h"
#include "rulebuilder.h"
#include "stats.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>

namespace {

QList<Playlist::Track> playable(const QList<Playlists::Item> &items, QList<int> *rows = nullptr) {
    QList<Playlist::Track> out;
    for (int i = 0; i < items.size(); ++i) {
        const Playlists::Item &it = items[i];
        if (it.path.isEmpty() || it.availability != QLatin1String("online"))
            continue;
        Playlist::Track t;
        t.path = it.path;
        t.title = it.title.isEmpty() ? QFileInfo(it.path).completeBaseName() : it.title;
        t.artist = it.artist;
        t.durationMs = it.durationMs;
        out << t;
        if (rows)
            rows->append(i);
    }
    return out;
}

QStringList toStrings(const QJsonArray &a) {
    QStringList out;
    for (const auto &v : a)
        out << v.toString();
    return out;
}

} // namespace

PlaylistsController::PlaylistsController(LibraryController *library) : QObject(library), m_controller(library) {
    m_importPicker.setup(tr("Import playlists"), tr("Import"), false, true, tr("Playlists"),
                         {"*.m3u", "*.m3u8", "*.pls"});
    m_exportPicker.setup(tr("Export the playlist to a folder"), tr("Export here"), true, false);
    connect(&m_importPicker, &FolderPicker::picked, this, [this](const QStringList &files) {
        int lists = 0, entries = 0, unmatched = 0;
        qint64 last = 0;
        QStringList errors;
        for (const QString &file : files) {
            const QVariantMap r = importFile(file);
            if (!r.value("ok").toBool()) {
                errors << r.value("error").toString();
                continue;
            }
            ++lists;
            entries += r.value("entries").toInt();
            unmatched += r.value("unmatched").toInt();
            last = r.value("id").toLongLong();
        }
        if (!errors.isEmpty())
            emit m_controller->toast(errors.first());
        else if (lists > 0)
            emit m_controller->toast(unmatched > 0
                                         ? tr("Imported %n playlists: %1 entries, %2 not in the library", "", lists)
                                               .arg(entries)
                                               .arg(unmatched)
                                         : tr("Imported %n playlists: %1 entries", "", lists).arg(entries));
        if (last)
            show(last);
    });
    connect(&m_exportPicker, &FolderPicker::picked, this, [this](const QStringList &folders) {
        if (folders.isEmpty() || !m_exporting)
            return;
        Playlists::Playlist p;
        if (!lib() || !Playlists::get(*lib(), m_exporting, &p))
            return;
        QString name = p.name;
        name.replace('/', '-');
        const QVariantMap r = exportTo(m_exporting, QDir(folders.first()).filePath(name + ".m3u8"));
        m_exporting = 0;
        emit m_controller->toast(r.value("ok").toBool()
                                     ? tr("Exported %n entries to %1", "", r.value("written").toInt())
                                           .arg(r.value("file").toString())
                                     : r.value("error").toString());
    });
    for (FolderPicker *picker : {&m_importPicker, &m_exportPicker})
        connect(picker, &FolderPicker::failed, this, [this](const QString &message) { emit m_controller->toast(message); });
    connect(&m_items, &PlaylistItemsModel::edited, this, [this] { touched(); });
    connect(&m_items, &PlaylistItemsModel::failed, this, [this](const QString &message) { emit m_controller->toast(message); });
}

Library *PlaylistsController::lib() const { return m_controller->library(); }

void PlaylistsController::attach() { m_items.setLibrary(lib()); }

void PlaylistsController::touched() {
    ++m_revision;
    m_controller->reloadTree();
    emit changed();
}

bool PlaylistsController::fail(const QString &error) {
    emit m_controller->toast(error);
    return false;
}

QString PlaylistsController::pathOf(qint64 id) const {
    QStringList names;
    Playlists::Playlist p;
    for (int guard = 0; id > 0 && guard < 64 && lib() && Playlists::get(*lib(), id, &p); ++guard) {
        names.prepend(p.name);
        id = p.parentId;
    }
    return names.join(" / ");
}

qint64 PlaylistsController::createPlaylist(const QString &name, qint64 parentId) {
    if (!m_controller->activate())
        return 0;
    QString error;
    const qint64 id = Playlists::create(*lib(), name.trimmed(), parentId, &error);
    if (!id)
        return fail(error), 0;
    touched();
    return id;
}

qint64 PlaylistsController::createFolder(const QString &name, qint64 parentId) {
    if (!m_controller->activate())
        return 0;
    QString error;
    const qint64 id = Playlists::createFolder(*lib(), name.trimmed(), parentId, &error);
    if (!id)
        return fail(error), 0;
    touched();
    return id;
}

qint64 PlaylistsController::createAutoPlaylist(const QString &name, const QString &query, qint64 parentId) {
    if (!m_controller->activate())
        return 0;
    QString error;
    const qint64 id = Playlists::createAuto(*lib(), name.trimmed(), query, parentId, &error);
    if (!id)
        return fail(error), 0;
    touched();
    return id;
}

bool PlaylistsController::saveAutoPlaylist(qint64 id, const QString &name, const QString &query) {
    if (!lib())
        return false;
    Playlists::Playlist p;
    if (!Playlists::get(*lib(), id, &p))
        return fail(tr("That playlist is gone"));
    QString error;
    if (p.query != query && !Playlists::setQuery(*lib(), id, query, &error))
        return fail(error);
    if (p.name != name.trimmed() && !Playlists::rename(*lib(), id, name.trimmed(), &error))
        return fail(error);
    touched();
    if (m_items.playlistId() == id)
        m_items.reload();
    return true;
}

bool PlaylistsController::rename(qint64 id, const QString &name) {
    if (!lib())
        return false;
    QString error;
    if (!Playlists::rename(*lib(), id, name.trimmed(), &error))
        return fail(error);
    touched();
    if (m_items.playlistId() == id)
        m_items.reload();
    return true;
}

bool PlaylistsController::remove(qint64 id) {
    if (!lib())
        return false;
    QString error;
    if (!Playlists::remove(*lib(), id, &error))
        return fail(error);
    if (m_items.playlistId() == id)
        m_items.setPlaylist(0);
    touched();
    return true;
}

bool PlaylistsController::moveTo(qint64 id, qint64 parentId) {
    if (!lib())
        return false;
    QString error;
    if (!Playlists::move(*lib(), id, parentId, -1, &error))
        return fail(error);
    touched();
    return true;
}

QVariantMap PlaylistsController::get(qint64 id) const {
    Playlists::Playlist p;
    if (!lib() || !Playlists::get(*lib(), id, &p))
        return {};
    return {{"id", p.id},       {"name", p.name},       {"kind", p.kind},   {"query", p.query},
            {"parentId", p.parentId}, {"items", p.items}, {"path", pathOf(id)}};
}

QVariantList PlaylistsController::folderChoices(qint64 movingId) const {
    QVariantList out{QVariantMap{{"id", 0}, {"name", tr("Top level")}}};
    if (!lib())
        return out;
    // A folder can't go inside itself or anything under it.
    QSet<qint64> under;
    if (movingId)
        under.insert(movingId);
    for (const Playlists::Playlist &p : Playlists::list(*lib())) {
        if (under.contains(p.parentId)) {
            under.insert(p.id);
            continue;
        }
        if (p.kind == "folder" && !under.contains(p.id))
            out << QVariantMap{{"id", p.id}, {"name", pathOf(p.id)}};
    }
    return out;
}

QVariantList PlaylistsController::staticPlaylists() const {
    QVariantList out;
    if (!lib())
        return out;
    for (const Playlists::Playlist &p : Playlists::list(*lib()))
        if (p.kind == "static")
            out << QVariantMap{{"id", p.id}, {"name", pathOf(p.id)}};
    return out;
}

int PlaylistsController::show(qint64 id) {
    if (!m_controller->activate())
        return -1;
    QStringList keys;
    Playlists::Playlist p;
    for (qint64 at = id; at > 0 && Playlists::get(*lib(), at, &p); at = p.parentId)
        keys.prepend(QString("playlist:%1").arg(at));
    keys.prepend("playlists");
    const int row = m_controller->tree()->reveal(keys);
    if (row >= 0)
        m_controller->selectNode(row);
    return row;
}

int PlaylistsController::addTracks(qint64 id, const QVariantList &trackIds, int at) {
    if (!lib())
        return 0;
    QList<qint64> ids;
    for (const QVariant &v : trackIds)
        if (v.toLongLong() > 0)
            ids << v.toLongLong();
    if (ids.isEmpty())
        return 0;
    QString error;
    const int added = Playlists::addTracks(*lib(), id, ids, at, &error);
    if (!error.isEmpty()) {
        fail(error);
        return 0;
    }
    if (m_items.playlistId() == id)
        m_items.reload();
    touched();
    Playlists::Playlist p;
    Playlists::get(*lib(), id, &p);
    emit m_controller->toast(tr("Added %n tracks to %1", "", added).arg(p.name));
    return added;
}

int PlaylistsController::play(qint64 id, int fromRow) {
    Player *player = m_controller->player();
    if (!lib() || !player)
        return 0;
    QList<Playlists::Item> items = id == m_items.playlistId() ? m_items.items() : Playlists::items(*lib(), id);
    if (items.size() > LibraryController::maxPlayNow) {
        const int first = qBound(0, fromRow, int(items.size()) - LibraryController::maxPlayNow);
        items = items.mid(first, LibraryController::maxPlayNow);
        fromRow -= first;
    }
    QList<int> rows;
    const QList<Playlist::Track> tracks = playable(items, &rows);
    if (tracks.isEmpty())
        return fail(tr("Nothing in that playlist can play")), 0;
    int start = 0;
    while (start < rows.size() && rows[start] < fromRow)
        ++start;
    return player->playTracks(tracks, qMin(start, int(tracks.size()) - 1));
}

int PlaylistsController::queueNext(qint64 id) {
    Player *player = m_controller->player();
    if (!lib() || !player)
        return 0;
    const int n = player->queueNext(playable(Playlists::items(*lib(), id).mid(0, LibraryController::maxPlayNow)));
    emit m_controller->toast(tr("%n tracks will play next", "", n));
    return n;
}

int PlaylistsController::append(qint64 id) {
    Player *player = m_controller->player();
    if (!lib() || !player)
        return 0;
    const int n = player->appendTracks(playable(Playlists::items(*lib(), id).mid(0, LibraryController::maxPlayNow)));
    emit m_controller->toast(tr("%n tracks added to Now Playing", "", n));
    return n;
}

int PlaylistsController::playSelection() {
    const QList<int> rows = m_items.selectedRows();
    if (rows.isEmpty() || !m_controller->player())
        return 0;
    if (rows.size() == 1)
        return play(m_items.playlistId(), rows.first());
    QList<Playlists::Item> chosen;
    for (int r : rows)
        chosen << m_items.items().at(r);
    return m_controller->player()->playTracks(playable(chosen), 0);
}

int PlaylistsController::queueSelection() {
    const QList<int> rows = m_items.selectedRows();
    if (rows.isEmpty() || !m_controller->player())
        return 0;
    QList<Playlists::Item> chosen;
    for (int r : rows)
        chosen << m_items.items().at(r);
    const int n = m_controller->player()->queueNext(playable(chosen));
    emit m_controller->toast(tr("%n tracks will play next", "", n));
    return n;
}

qint64 PlaylistsController::saveNowPlaying(const QString &name) {
    Player *player = m_controller->player();
    if (!player || !m_controller->activate())
        return 0;
    if (player->playlist()->count() == 0)
        return fail(tr("Now Playing is empty")), 0;
    QString error;
    const qint64 id = Playlists::create(*lib(), name.trimmed(), 0, &error);
    if (!id)
        return fail(error), 0;
    QList<Playlists::Entry> entries;
    for (int i = 0; i < player->playlist()->count(); ++i) {
        const Playlist::Track &t = player->playlist()->at(i);
        Playlists::Entry e;
        e.trackId = Stats::trackIdForPath(*lib(), t.path);
        e.path = t.path;
        e.title = t.title;
        e.artist = t.artist;
        e.durationMs = t.durationMs;
        entries << e;
    }
    Playlists::addEntries(*lib(), id, entries, -1, &error);
    touched();
    emit m_controller->toast(tr("Saved Now Playing as %1").arg(name.trimmed()));
    return id;
}

void PlaylistsController::reroll(qint64 id) {
    if (!lib())
        return;
    Playlists::reroll(*lib(), id);
    if (m_items.playlistId() == id)
        m_items.reload();
}

void PlaylistsController::importPlaylists() {
    if (m_controller->activate())
        m_importPicker.pickFolders();
}

void PlaylistsController::exportPlaylist(qint64 id) {
    if (!lib() || id <= 0)
        return;
    m_exporting = id;
    m_exportPicker.pickFolders();
}

QVariantMap PlaylistsController::importFile(const QString &file, qint64 parentId) {
    if (!m_controller->activate())
        return {{"ok", false}, {"error", m_controller->error()}};
    const PlaylistFiles::ImportResult r = PlaylistFiles::import(*lib(), file, parentId);
    if (!r.ok())
        return {{"ok", false}, {"error", tr("Can't import %1: %2").arg(QFileInfo(file).fileName(), r.error)}};
    touched();
    return {{"ok", true},
            {"id", r.playlistId},
            {"name", r.name},
            {"entries", r.entries},
            {"matched", r.byPath + r.byTags},
            {"unmatched", r.unmatched}};
}

QVariantMap PlaylistsController::exportTo(qint64 id, const QString &file, bool relative) {
    if (!lib())
        return {{"ok", false}, {"error", tr("The library isn't open")}};
    PlaylistFiles::ExportOptions o;
    o.relative = relative;
    int written = 0;
    QString error;
    if (!PlaylistFiles::exportFile(*lib(), id, file, o, &written, &error))
        return {{"ok", false}, {"error", tr("Can't export: %1").arg(error)}};
    return {{"ok", true}, {"file", file}, {"written", written}};
}

QVariantMap PlaylistsController::checkQuery(const QString &text, int previewRows) const {
    QVariantMap out;
    const Query::Parsed parsed = Query::parse(text);
    if (!parsed.ok()) {
        out["ok"] = false;
        out["error"] = parsed.error;
        out["position"] = parsed.position;
        return out;
    }
    out["ok"] = true;
    if (!lib())
        return out;
    Query::Options o;
    o.limit = previewRows;
    const Query::Result r = Query::run(*lib(), text, o);
    if (!r.ok()) {
        out["ok"] = false;
        out["error"] = r.error;
        return out;
    }
    QVariantList preview;
    for (const Query::Track &t : r.tracks)
        preview << QVariantMap{{"title", t.title}, {"artist", t.artist}, {"album", t.album}, {"duration", t.durationMs}};
    out["count"] = r.total;
    out["preview"] = preview;
    out["durationMs"] = r.durationMs;
    return out;
}

QVariantMap PlaylistsController::rulesFromQuery(const QString &text) const {
    return RuleBuilder::fromQuery(text).toMap();
}

QString PlaylistsController::queryFromRules(const QVariantMap &rules) const {
    return RuleBuilder::toQuery(RuleBuilder::Rules::fromMap(rules));
}

QVariantList PlaylistsController::ruleFields() const { return RuleBuilder::fieldList(); }

QVariantList PlaylistsController::ruleOperators(const QString &type) const { return RuleBuilder::operatorsFor(type); }

// ---- Collections -----------------------------------------------------------

static QVariantMap collectionMap(const Collections::Collection &c) {
    return {{"id", c.id},       {"name", c.name},        {"query", c.query},
            {"columns", toStrings(c.columns)}, {"tree", toStrings(c.tree)}, {"sort", c.sort},
            {"view", c.view},   {"visible", c.visible}};
}

QVariantList PlaylistsController::collections() const {
    QVariantList out;
    if (lib())
        for (const Collections::Collection &c : Collections::list(*lib()))
            out << collectionMap(c);
    return out;
}

QVariantMap PlaylistsController::collection(qint64 id) const {
    Collections::Collection c;
    if (!lib() || !Collections::get(*lib(), id, &c))
        return {};
    return collectionMap(c);
}

qint64 PlaylistsController::saveCollection(const QVariantMap &map) {
    if (!m_controller->activate())
        return 0;
    Collections::Collection c;
    const qint64 id = map.value("id").toLongLong();
    if (id > 0 && !Collections::get(*lib(), id, &c))
        return fail(tr("That collection is gone")), 0;
    if (map.contains("name"))
        c.name = map.value("name").toString().trimmed();
    if (map.contains("query"))
        c.query = map.value("query").toString();
    if (map.contains("columns"))
        c.columns = QJsonArray::fromStringList(map.value("columns").toStringList());
    if (map.contains("tree"))
        c.tree = QJsonArray::fromStringList(map.value("tree").toStringList());
    if (map.contains("sort"))
        c.sort = map.value("sort").toString();
    if (map.contains("visible"))
        c.visible = map.value("visible").toString();
    QString error;
    qint64 saved = id;
    if (id > 0) {
        if (!Collections::update(*lib(), c, &error))
            return fail(error), 0;
    } else {
        saved = Collections::create(*lib(), c, &error);
        if (!saved)
            return fail(error), 0;
    }
    touched();
    return saved;
}

bool PlaylistsController::removeCollection(qint64 id) {
    if (!lib())
        return false;
    QString error;
    if (!Collections::remove(*lib(), id, &error))
        return fail(error);
    touched();
    return true;
}

int PlaylistsController::restoreCollections() {
    if (!m_controller->activate())
        return 0;
    const int n = Collections::ensureDefaults(*lib());
    touched();
    return n;
}

void PlaylistsController::setCollectionColumns(qint64 id, const QStringList &columns) {
    Collections::Collection c;
    if (!lib() || !Collections::get(*lib(), id, &c))
        return;
    const QJsonArray next = QJsonArray::fromStringList(columns);
    if (next == c.columns)
        return;
    c.columns = next;
    Collections::update(*lib(), c);
}

QStringList PlaylistsController::columnNames() const {
    return {"number", "title", "artist", "album", "year", "genre", "length", "rating", "plays", "added", "format"};
}

QVariantList PlaylistsController::treeNodeChoices() const {
    return {QVariantMap{{"id", "artist"}, {"label", tr("Artist")}},
            QVariantMap{{"id", "albumartist"}, {"label", tr("Album Artist")}},
            QVariantMap{{"id", "album"}, {"label", tr("Album")}},
            QVariantMap{{"id", "genre"}, {"label", tr("Genre")}},
            QVariantMap{{"id", "year"}, {"label", tr("Year")}},
            QVariantMap{{"id", "rating"}, {"label", tr("Rating")}},
            QVariantMap{{"id", "composer"}, {"label", tr("Composer")}}};
}
