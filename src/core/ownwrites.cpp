#include "ownwrites.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QMutex>

namespace {

struct Registry {
    QMutex mutex;
    QHash<QString, qint64> until; // path -> monotonic ms when the note runs out
    QElapsedTimer clock;
    int lifetimeMs = 5000;

    Registry() { clock.start(); }
    qint64 now() const { return clock.elapsed(); }

    // Drops notes that have run out, now and then, so the list stays small.
    void prune() {
        if (until.size() < 512)
            return;
        const qint64 t = now();
        for (auto it = until.begin(); it != until.end();)
            it = it.value() < t ? until.erase(it) : std::next(it);
    }
};

Registry &registry() {
    static Registry r;
    return r;
}

QString clean(const QString &path) { return QDir::cleanPath(path); }

} // namespace

namespace OwnWrites {

void note(const QString &path) {
    if (path.isEmpty())
        return;
    Registry &r = registry();
    QMutexLocker lock(&r.mutex);
    r.prune();
    r.until.insert(clean(path), r.now() + r.lifetimeMs);
}

void noteFolder(const QString &folder) {
    QString path = clean(folder);
    // The folder, then each parent that isn't there yet, up to one that is.
    while (!path.isEmpty() && path != "/") {
        note(path);
        const QString parent = QFileInfo(path).path();
        if (parent == path || QFileInfo::exists(parent))
            break;
        path = parent;
    }
}

bool isOwn(const QString &path) {
    Registry &r = registry();
    QMutexLocker lock(&r.mutex);
    const auto it = r.until.constFind(clean(path));
    return it != r.until.constEnd() && it.value() >= r.now();
}

void setLifetime(int ms) {
    Registry &r = registry();
    QMutexLocker lock(&r.mutex);
    r.lifetimeMs = qMax(0, ms);
}

int lifetime() {
    Registry &r = registry();
    QMutexLocker lock(&r.mutex);
    return r.lifetimeMs;
}

void clear() {
    Registry &r = registry();
    QMutexLocker lock(&r.mutex);
    r.until.clear();
}

} // namespace OwnWrites
