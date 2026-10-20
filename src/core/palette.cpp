#include "palette.h"

#include "library.h"
#include "query.h"

#include <QSqlQuery>
#include <algorithm>
#include <cmath>

namespace {

// Lower case without accents, so "bjork" finds "Björk".
QString fold(const QString &s) {
    const QString d = s.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(d.size());
    for (const QChar c : d)
        if (c.category() != QChar::Mark_NonSpacing)
            out += c.toCaseFolded();
    return out;
}

QStringList wordsOf(const QString &folded) {
    QStringList out;
    QString word;
    for (const QChar c : folded) {
        if (c.isLetterOrNumber()) {
            word += c;
        } else if (!word.isEmpty()) {
            out << word;
            word.clear();
        }
    }
    if (!word.isEmpty())
        out << word;
    return out;
}

bool wordStart(const QString &s, int at) { return at == 0 || !s[at - 1].isLetterOrNumber(); }

// Letters in order, with points for landing on word starts and for runs.
double fuzzy(const QString &q, const QString &c) {
    QString needle = q;
    needle.remove(' ');
    if (needle.size() < 2)
        return 0;
    double points = 0;
    int qi = 0, last = -2;
    bool firstAtStart = false;
    for (int ci = 0; ci < c.size() && qi < needle.size(); ++ci) {
        if (c[ci] != needle[qi])
            continue;
        // Prefer a later word start over a letter in the middle of a word.
        if (!wordStart(c, ci) && ci != last + 1) {
            const int later = [&] {
                for (int k = ci + 1; k < c.size(); ++k)
                    if (c[k] == needle[qi] && wordStart(c, k))
                        return k;
                return -1;
            }();
            if (later > 0)
                ci = later;
        }
        const bool start = wordStart(c, ci);
        if (qi == 0)
            firstAtStart = start;
        points += 1 + (start ? 2 : 0) + (ci == last + 1 ? 1.5 : 0);
        last = ci;
        ++qi;
    }
    if (qi < needle.size() || !firstAtStart)
        return 0;
    return 15 + 30 * points / (4.5 * needle.size());
}

// The search text, folded and split once per search rather than once per
// candidate: a search compares it with tens of thousands of names.
struct Needle {
    QString text;
    QStringList words;
    explicit Needle(const QString &folded) : text(folded), words(wordsOf(folded)) {}
};

bool hasWordStartingWith(const QString &c, const QString &w) {
    for (qsizetype at = c.indexOf(w); at >= 0; at = c.indexOf(w, at + 1))
        if (wordStart(c, int(at)))
            return true;
    return false;
}

// Every word of the needle appears somewhere in one of the two.
bool allWordsIn(const Needle &n, const QString &a, const QString &b) {
    return std::all_of(n.words.begin(), n.words.end(), [&](const QString &w) { return a.contains(w) || b.contains(w); });
}

// c is already folded.
double scoreFolded(const Needle &n, const QString &c, bool allowFuzzy) {
    const QString &q = n.text;
    if (q.isEmpty() || c.isEmpty())
        return 0;
    const double extra = std::min(8.0, (c.size() - q.size()) / 6.0);
    if (c == q)
        return 100;
    if (c.startsWith(q))
        return 92 - extra;
    if (n.words.isEmpty())
        return c.contains(q) ? 60 : 0;
    bool allPrefix = true;
    for (const QString &w : n.words) {
        if (!c.contains(w))
            return allowFuzzy ? fuzzy(q, c) : 0;
        if (allPrefix && !hasWordStartingWith(c, w))
            allPrefix = false;
    }
    if (allPrefix)
        return 80 - extra;
    if (c.contains(q))
        return 68 - extra;
    return 55 - extra;
}

QString clock(qint64 ms) {
    const qint64 s = ms / 1000;
    if (s >= 3600)
        return QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

// Any field, sort: or limit: makes the text a query rather than words.
bool usesFields(const Query::Node &n) {
    if (n.kind == Query::Node::Term)
        return !n.field.isEmpty();
    return std::any_of(n.children.begin(), n.children.end(), usesFields);
}

const char *kindNames[] = {"action", "artist", "album", "track", "search", "external"};

} // namespace

Palette::Palette(QObject *parent) : QAbstractListModel(parent) {}

Palette::~Palette() = default;

void Palette::setLibrary(Library *library) {
    m_library = library;
    m_artists.clear();
    m_albums.clear();
    m_namesSignature.clear();
    search();
}

void Palette::setText(const QString &text) {
    if (text == m_text)
        return;
    m_text = text;
    emit textChanged();
    search();
}

void Palette::addAction(const QString &id, const QString &title, const QStringList &keywords, const QString &shortcut) {
    for (ActionEntry &a : m_actions) {
        if (a.id == id) {
            a.title = title;
            a.keywords = keywords;
            a.shortcut = shortcut;
            search();
            return;
        }
    }
    m_actions.append({id, title, shortcut, keywords});
    search();
}

void Palette::removeAction(const QString &id) {
    m_actions.removeIf([&](const ActionEntry &a) { return a.id == id; });
    search();
}

void Palette::setActionEnabled(const QString &id, bool enabled) {
    for (ActionEntry &a : m_actions) {
        if (a.id == id && a.enabled != enabled) {
            a.enabled = enabled;
            search();
        }
    }
}

QStringList Palette::actionIds() const {
    QStringList out;
    for (const ActionEntry &a : m_actions)
        out << a.id;
    return out;
}

void Palette::refresh() {
    m_namesSignature.clear();
    search();
}

void Palette::setLimits(int actions, int artists, int albums, int tracks) {
    m_maxActions = actions;
    m_maxArtists = artists;
    m_maxAlbums = albums;
    m_maxTracks = tracks;
    search();
}

double Palette::score(const QString &text, const QString &candidate, bool fuzzy) {
    return scoreFolded(Needle(fold(text).simplified()), fold(candidate), fuzzy);
}

void Palette::searchActions(const QString &text, QList<Item> *out) const {
    const Needle needle(fold(text).simplified());
    QList<Item> found;
    for (const ActionEntry &a : m_actions) {
        double s = needle.text.isEmpty() ? 1 : scoreFolded(needle, fold(a.title), true);
        for (const QString &k : a.keywords)
            s = std::max(s, scoreFolded(needle, fold(k), true) * 0.9);
        if (s <= 0)
            continue;
        Item item;
        item.kind = Action;
        item.title = a.title;
        item.detail = a.shortcut;
        item.shortcut = a.shortcut;
        item.actionId = a.id;
        item.enabled = a.enabled;
        // Recently used actions first when nothing is typed, and a little
        // ahead of their equals when something is.
        item.score = needle.text.isEmpty() ? double(a.lastUsed) : s + 3 + (a.lastUsed ? 2 : 0) - (a.enabled ? 0 : 5);
        found << item;
    }
    std::stable_sort(found.begin(), found.end(), [](const Item &a, const Item &b) { return a.score > b.score; });
    const int max = needle.text.isEmpty() || m_text.startsWith('>') ? 200 : m_maxActions;
    *out += found.mid(0, max);
}

void Palette::loadNames() {
    QSqlDatabase db = m_library->db();
    QSqlQuery q(db);
    q.setForwardOnly(true);
    // Changes when a scan adds or removes artists, albums or tracks.
    q.exec("SELECT (SELECT count(*) FROM artists) || ':' || (SELECT max(id) FROM artists) || ':' || "
           "(SELECT count(*) FROM albums) || ':' || (SELECT max(id) FROM albums) || ':' || "
           "(SELECT count(*) FROM tracks) || ':' || (SELECT max(id) FROM tracks)");
    const QString signature = q.next() ? q.value(0).toString() : QString();
    m_namesChecked.start();
    if (signature == m_namesSignature)
        return;
    m_namesSignature = signature;
    m_artists.clear();
    m_albums.clear();
    q.exec("SELECT a.id, a.name, count(DISTINCT ta.track_id) FROM artists a "
           "JOIN track_artists ta ON ta.artist_id = a.id GROUP BY a.id");
    while (q.next()) {
        const QString name = q.value(1).toString();
        m_artists.append({q.value(0).toLongLong(), name, fold(name), {}, {}, 0, q.value(2).toInt()});
    }
    q.exec("SELECT al.id, al.title, coalesce(al.album_artist, ''), coalesce(al.year, 0), count(t.id) "
           "FROM albums al JOIN tracks t ON t.album_id = al.id GROUP BY al.id");
    while (q.next()) {
        const QString title = q.value(1).toString(), artist = q.value(2).toString();
        m_albums.append({q.value(0).toLongLong(), title, fold(title), artist, fold(artist), q.value(3).toInt(),
                         q.value(4).toInt()});
    }
}

void Palette::searchLibrary(const QString &text, QList<Item> *out) {
    const Query::Parsed parsed = Query::parse(text);
    const bool isQuery = parsed.ok() && (usesFields(parsed.root) || !parsed.sort.isEmpty() ||
                                         parsed.limit.kind != Query::Limit::None);
    const Needle needle(fold(text).simplified());
    const QString &q = needle.text;
    const QStringList &words = needle.words;

    if (!isQuery && !words.isEmpty()) {
        if (m_namesSignature.isEmpty() || !m_namesChecked.isValid() || m_namesChecked.elapsed() > 2000)
            loadNames();
        auto best = [](QList<Item> &list, int max) {
            std::partial_sort(list.begin(), list.begin() + std::min<qsizetype>(max, list.size()), list.end(),
                              [](const Item &a, const Item &b) { return a.score > b.score; });
            return list.mid(0, max);
        };
        QList<Item> artists;
        for (const Named &a : m_artists) {
            const double s = scoreFolded(needle, a.folded, false);
            if (s <= 0)
                continue;
            Item item;
            item.kind = Artist;
            item.id = a.id;
            item.title = a.name;
            item.detail = tr("%n tracks", "", a.tracks);
            item.score = s + 4 + std::min(3.0, std::log10(a.tracks + 1.0));
            artists << item;
        }
        *out += best(artists, m_maxArtists);

        QList<Item> albums;
        for (const Named &a : m_albums) {
            double s = scoreFolded(needle, a.folded, false);
            // "neon night drive": words across the album artist and title.
            // The artist alone ("neon") finds the album too, lower down.
            if (s < 55 && !a.extraFolded.isEmpty() && allWordsIn(needle, a.folded, a.extraFolded)) {
                const double weight = words.size() > 1 ? 0.95 : 0.75;
                s = std::max({s, scoreFolded(needle, a.extraFolded + ' ' + a.folded, false) * weight,
                              scoreFolded(needle, a.folded + ' ' + a.extraFolded, false) * weight});
            }
            if (s <= 0)
                continue;
            Item item;
            item.kind = Album;
            item.id = a.id;
            item.title = a.name;
            item.subtitle = a.extra;
            item.detail = a.year > 0 ? QString::number(a.year) : QString();
            item.score = s + 2 + std::min(1.0, a.tracks / 20.0);
            albums << item;
        }
        *out += best(albums, m_maxAlbums);
    }

    // Tracks need the search index, which answers words of three or more
    // letters; shorter text would scan every row, so it waits for more.
    const bool indexable = std::any_of(words.begin(), words.end(), [](const QString &w) { return w.size() >= 3; });
    if (!parsed.ok() || (!isQuery && !indexable))
        return;
    const Query::Compiled c = Query::compile(parsed);
    if (!c.ok())
        return;
    QSqlQuery tq(m_library->db());
    tq.setForwardOnly(true);
    const int candidates = isQuery ? m_maxTracks : 200;
    // A query keeps its own order; words prefer the tracks played most.
    tq.prepare("SELECT t.id, t.title, t.artist, t.album, t.duration_ms, t.play_count, t.file_name FROM tracks t WHERE " +
               c.where + " ORDER BY " + (isQuery ? c.order() : QString("t.play_count DESC, t.id")) +
               QString(" LIMIT %1").arg(candidates));
    for (const QVariant &p : c.params)
        tq.addBindValue(p);
    if (!tq.exec())
        return;
    QList<Item> tracks;
    while (tq.next()) {
        Item item;
        item.kind = Track;
        item.id = tq.value(0).toLongLong();
        item.title = tq.value(1).toString();
        if (item.title.isEmpty())
            item.title = tq.value(6).toString();
        const QString artist = tq.value(2).toString(), album = tq.value(3).toString();
        const QString shownArtist = QString(artist).replace("; ", ", ");
        item.subtitle = shownArtist.isEmpty() || album.isEmpty() ? shownArtist + album
                                                                 : shownArtist + QString::fromUtf8(" · ") + album;
        item.detail = tq.value(4).toLongLong() > 0 ? clock(tq.value(4).toLongLong()) : QString();
        const int plays = tq.value(5).toInt();
        if (isQuery) {
            // In the query's order, after anything else that matched well.
            item.score = 50 - tracks.size() * 0.01;
        } else {
            const QString title = fold(item.title), a = fold(artist);
            double s = scoreFolded(needle, title, false);
            if (s < 80 && words.size() > 1 && allWordsIn(needle, title, a))
                s = std::max({s, scoreFolded(needle, a + ' ' + title, false) * 0.95,
                              scoreFolded(needle, title + ' ' + a, false) * 0.95});
            s = std::max(s, scoreFolded(needle, fold(album), false) * 0.6);
            // The index matched something (a composer, the file name).
            item.score = std::max(s, 30.0) + std::min(2.0, std::log10(plays + 1.0));
        }
        tracks << item;
    }
    std::stable_sort(tracks.begin(), tracks.end(), [](const Item &a, const Item &b) { return a.score > b.score; });
    *out += tracks.mid(0, m_maxTracks);
}

void Palette::search() {
    QElapsedTimer timer;
    timer.start();
    QList<Item> items;
    const QString text = m_text.trimmed();
    if (text.startsWith('>')) {
        searchActions(text.mid(1), &items);
    } else if (text.isEmpty()) {
        searchActions({}, &items);
    } else {
        searchActions(text, &items);
        if (m_library)
            searchLibrary(text, &items);
        // Best first; equal scores keep kinds together in a fixed order.
        std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
            if (a.score != b.score)
                return a.score > b.score;
            return a.kind < b.kind;
        });
        if (m_library) {
            Item search;
            search.kind = Search;
            search.title = tr("Search the library for “%1”").arg(text);
            items << search;
        }
        // Spotify's answers for this text, after everything local.
        QStringList sources = m_external.keys();
        sources.sort();
        for (const QString &source : sources) {
            const ExternalSet &set = m_external.value(source);
            if (set.text.trimmed() == text)
                items += set.items;
        }
    }
    beginResetModel();
    const bool countChanges = items.size() != m_items.size();
    m_items = items;
    endResetModel();
    if (countChanges)
        emit countChanged();
    m_lastSearchMs = timer.nsecsElapsed() / 1e6;
    emit searched();
}

void Palette::setExternalResults(const QString &source, const QString &forText, const QList<Item> &items) {
    QList<Item> rows = items;
    for (Item &i : rows) {
        i.kind = External;
        i.source = source;
    }
    if (rows.isEmpty())
        m_external.remove(source);
    else
        m_external.insert(source, {forText, rows});
    search();
}

bool Palette::hasExternalRows() const {
    for (const Item &i : m_items)
        if (i.kind == External)
            return true;
    return false;
}

int Palette::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_items.size()); }

QVariant Palette::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size())
        return {};
    const Item &item = m_items[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return item.title;
    case KindRole:
        return QString::fromLatin1(kindNames[item.kind]);
    case KindLabelRole:
        switch (item.kind) {
        case Action:
            return tr("Action");
        case Artist:
            return tr("Artist");
        case Album:
            return tr("Album");
        case Track:
            return tr("Track");
        case Search:
            return tr("Search");
        case External:
            return item.kindLabel;
        }
        return {};
    case SubtitleRole:
        return item.subtitle;
    case DetailRole:
        return item.detail;
    case ShortcutRole:
        return item.shortcut;
    case ItemIdRole:
        return item.kind == Action || item.kind == External ? QVariant(item.actionId) : QVariant(item.id);
    case SectionRole:
        if (item.kind == External)
            return item.source;
        return hasExternalRows() ? tr("Local") : QString();
    case ScoreRole:
        return item.score;
    case EnabledRole:
        return item.enabled;
    }
    return {};
}

QHash<int, QByteArray> Palette::roleNames() const {
    return {{KindRole, "kind"},         {KindLabelRole, "kindLabel"}, {TitleRole, "title"},
            {SubtitleRole, "subtitle"}, {DetailRole, "detail"},       {ShortcutRole, "shortcut"},
            {ItemIdRole, "itemId"},     {ScoreRole, "score"},         {EnabledRole, "actionEnabled"},
            {SectionRole, "section"}};
}

QVariantMap Palette::get(int row) const {
    QVariantMap out;
    if (row < 0 || row >= m_items.size())
        return out;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.begin(); it != roles.end(); ++it)
        out.insert(QString::fromLatin1(it.value()), data(index(row), it.key()));
    return out;
}

bool Palette::activate(int row) {
    if (row < 0 || row >= m_items.size())
        return false;
    // Copies, because a slot may change the text and with it every row.
    const Item item = m_items[row];
    const QVariantMap map = get(row);
    const QString text = m_text.trimmed();
    switch (item.kind) {
    case Action:
        if (!item.enabled)
            return false;
        for (ActionEntry &a : m_actions)
            if (a.id == item.actionId)
                a.lastUsed = ++m_useCounter;
        emit actionTriggered(item.actionId);
        break;
    case Artist:
        emit artistActivated(item.id, item.title);
        break;
    case Album:
        emit albumActivated(item.id, item.title, item.subtitle);
        break;
    case Track:
        emit trackActivated(item.id);
        break;
    case Search:
        emit searchRequested(text);
        break;
    case External:
        emit externalActivated(item.source, item.actionId);
        break;
    }
    emit activated(map);
    return true;
}
