#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QVariantList>

class FolderPicker;
class LibraryController;

// The tag editor (Ctrl+E): MediaMonkey's Properties dialog, for one track or
// many at once.
//
// load() reads every chosen track's tags from its file on a worker thread
// (TagWriter::currentValues). A field the tracks agree on shows its value;
// one they don't is "mixed" and is kept as it is in each file unless the
// user types a new value or clears it. apply() writes only the fields that
// were edited, with TagWriter on the controller's job thread, so progress
// shows in the jobs bar and Ctrl+Z undoes the whole batch.
class TagEditor : public QObject {
    Q_OBJECT
    Q_PROPERTY(int trackCount READ trackCount NOTIFY loadedChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool applying READ applying NOTIFY applyingChanged)
    // Something about the tracks as a whole: offline files, unreadable tags.
    Q_PROPERTY(QString notice READ notice NOTIFY loadedChanged)
    // Goes up with every change, so QML bindings that call value() re-read.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QString heading READ heading NOTIFY loadedChanged)
    // The picture to show: a data: URL, or empty.
    Q_PROPERTY(QString artworkSource READ artworkSource NOTIFY changed)
    Q_PROPERTY(QString artworkNote READ artworkNote NOTIFY changed)
    // keep, new or remove: what Save does to the pictures in the files.
    Q_PROPERTY(QString artworkAction READ artworkAction NOTIFY changed)
    // After a save: one entry per file that failed ({path, error}).
    Q_PROPERTY(QVariantList failures READ failures NOTIFY appliedChanged)
    Q_PROPERTY(QString resultText READ resultText NOTIFY appliedChanged)

public:
    explicit TagEditor(LibraryController *controller, QObject *parent = nullptr);
    ~TagEditor() override;

    // The fields the editor shows, in order (TagWriter's canonical names).
    static const QStringList &editableFields();

    int trackCount() const { return m_ids.size(); }
    bool loading() const { return m_loadThread != nullptr; }
    bool applying() const { return m_applying; }
    QString notice() const { return m_notice; }
    int revision() const { return m_revision; }
    bool dirty() const { return !m_edits.isEmpty() || m_artworkAction != QLatin1String("keep"); }
    QString heading() const { return m_heading; }
    QString artworkSource() const;
    QString artworkNote() const;
    QString artworkAction() const { return m_artworkAction; }
    QVariantList failures() const { return m_failures; }
    QString resultText() const { return m_resultText; }
    QList<qint64> trackIds() const { return m_ids; }

    // Reads the tracks' tags; loadedChanged() follows when they are in.
    Q_INVOKABLE bool load(const QVariantList &ids);
    // Waits for a load to finish (tests and the demo hooks).
    void waitForLoad();
    // The value shown in the field: the edit, else the tracks' common value
    // (empty when they differ).
    Q_INVOKABLE QString value(const QString &field) const;
    // The tracks disagree and the user hasn't touched it: Save leaves it alone.
    Q_INVOKABLE bool mixed(const QString &field) const;
    Q_INVOKABLE bool edited(const QString &field) const;
    // What the tracks had before editing, for a tooltip on a mixed field:
    // up to five different values.
    Q_INVOKABLE QStringList originalValues(const QString &field) const;
    Q_INVOKABLE void setValue(const QString &field, const QString &text);
    // Empties a field in every track (even a mixed one).
    Q_INVOKABLE void clearField(const QString &field);
    Q_INVOKABLE void revert(const QString &field);
    Q_INVOKABLE void revertAll();

    Q_INVOKABLE bool setArtworkFromFile(const QString &path);
    // A picture on the clipboard, or a picture file copied in the file manager.
    Q_INVOKABLE bool pasteArtwork();
    Q_INVOKABLE void removeArtwork();
    Q_INVOKABLE void keepArtwork();
    // Opens the desktop's file picker for a picture.
    Q_INVOKABLE void pickArtwork();
    void setPicker(FolderPicker *picker);

    // Writes the edits as one batch (a job). false if nothing to save or
    // another job is running.
    Q_INVOKABLE bool apply();
    // F2: one field of one track, written straight away.
    Q_INVOKABLE bool quickSet(qint64 trackId, const QString &field, const QString &value);
    Q_INVOKABLE void close();

signals:
    void loadedChanged();
    void loadingChanged();
    void applyingChanged();
    void changed();
    void appliedChanged();
    // A save finished: ok when every file was written.
    void applied(bool ok);
    void message(const QString &text);

private:
    struct Loaded {
        QHash<QString, QStringList> values; // field -> distinct values, in the order met
        QByteArray picture;
        QString pictureMime, pictureNote;
        bool picturesDiffer = false;
        int offline = 0, unreadable = 0;
        QString firstError;
    };
    void onLoaded(int generation, const Loaded &loaded);
    void bump();
    static Loaded readTracks(const QString &dbPath, const QList<qint64> &ids);

    QPointer<LibraryController> m_controller;
    QPointer<FolderPicker> m_picker;
    QList<qint64> m_ids;
    QHash<QString, QStringList> m_original;
    QHash<QString, QString> m_edits;
    QString m_notice, m_heading;
    QByteArray m_picture, m_newPicture;
    QString m_pictureMime, m_newPictureMime, m_pictureNote, m_newPictureNote;
    bool m_picturesDiffer = false;
    QString m_artworkAction = "keep";
    QThread *m_loadThread = nullptr;
    int m_generation = 0;
    int m_revision = 0;
    bool m_applying = false;
    QVariantList m_failures;
    QString m_resultText;
};
