#pragma once

#include <QString>

// The files and folders this process is changing itself: the tag writer's
// replacements, the organiser's moves and the folders it makes and removes,
// and files sent to the trash. The folder watcher asks here before it treats
// an inotify event as a change from outside, so Omamp's own writes never
// start a rescan of their own (the writers update the library rows
// themselves).
//
// A path counts as our own for a few seconds after it was last noted. The
// writers note it before they touch it and again when they are done, so
// the events the change causes, which arrive after it, always find it.
// Every thread may note and ask; it is one process-wide list.
namespace OwnWrites {

// Notes one file or folder (any path; it is cleaned first).
void note(const QString &path);
// Notes a folder that is about to be made, and each of its parents that
// doesn't exist yet: mkpath makes them all, and each one is an event.
void noteFolder(const QString &folder);
// True when the path was noted within the lifetime.
bool isOwn(const QString &path);

// How long a note lasts, in milliseconds (default 5000). For tests.
void setLifetime(int ms);
int lifetime();
// Forgets every note. For tests.
void clear();

// The temporary files Omamp writes beside a target start with this; both
// the watcher and the scanner skip them, as they skip every hidden name.
inline constexpr char temporaryPrefix[] = ".omamp-";

} // namespace OwnWrites
