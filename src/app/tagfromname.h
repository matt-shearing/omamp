#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class LibraryController;

// Tag from file names: a pattern such as `<Track#> - <Artist> - <Title>`
// matched against the end of each chosen file's path, with a live preview
// of what each file would get. apply() writes the matches with TagWriter as
// one batch (source "tag-from-filename"), so Ctrl+Z undoes it.
class TagFromNameModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString pattern READ pattern WRITE setPattern NOTIFY patternChanged)
    Q_PROPERTY(QString patternError READ patternError NOTIFY previewed)
    Q_PROPERTY(int patternErrorPosition READ patternErrorPosition NOTIFY previewed)
    // The fields the pattern fills, in order, as canonical names and as headings.
    Q_PROPERTY(QStringList fields READ fields NOTIFY previewed)
    Q_PROPERTY(QStringList fieldLabels READ fieldLabels NOTIFY previewed)
    Q_PROPERTY(int count READ count NOTIFY previewed)
    Q_PROPERTY(int matched READ matched NOTIFY previewed)
    Q_PROPERTY(bool applying READ applying NOTIFY appliedChanged)
    Q_PROPERTY(QString resultText READ resultText NOTIFY appliedChanged)
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)

public:
    enum Roles {
        PathRole = Qt::UserRole + 1, // the part of the path the pattern reads
        MatchedRole,
        ErrorRole,
        ValuesRole, // list of strings, one per field
        TrackIdRole,
    };

    explicit TagFromNameModel(LibraryController *controller, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString pattern() const { return m_pattern; }
    void setPattern(const QString &pattern);
    QString patternError() const { return m_error; }
    int patternErrorPosition() const { return m_errorPosition; }
    QStringList fields() const { return m_fields; }
    QStringList fieldLabels() const;
    int count() const { return m_rows.size(); }
    int matched() const;
    bool applying() const { return m_applying; }
    QString resultText() const { return m_resultText; }
    QVariantList presets() const;

    Q_INVOKABLE bool load(const QVariantList &ids);
    Q_INVOKABLE bool apply();
    Q_INVOKABLE void close();

signals:
    void patternChanged();
    void previewed();
    void appliedChanged();
    void applied(bool ok);

private:
    void preview();

    struct Row {
        qint64 id = 0;
        QString path;
        bool matched = false;
        QString error;
        QStringList values;
    };

    QPointer<LibraryController> m_controller;
    QList<Row> m_rows;
    QString m_pattern = "<Track#> - <Title>";
    QString m_error;
    int m_errorPosition = -1;
    QStringList m_fields;
    bool m_applying = false;
    QString m_resultText;
};
