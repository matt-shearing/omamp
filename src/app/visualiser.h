#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QQuickPaintedItem>

// The analyser and its friends, drawn on the CPU with QPainter so they work
// with the software renderer and cost little: a few dozen rectangles or one
// path a frame, or one small image for the fire.
//
//   Visualiser {
//       style: "peaks"            // bars, peaks, mirror, scope, fire
//       levels: backend.levels    // 0..1 per band
//       peaks: backend.peaks
//       waveform: backend.waveform // -1..1, the last few milliseconds
//       low: "green"; mid: "cyan"; high: "yellow"; peak: "white"
//   }
//
// bars:   bars rising from green through the middle colour to the top one
// peaks:  the same with a falling cap on each bar (Omamp's own analyser)
// mirror: the bars reflected about the middle line, quieter below
// scope:  the waveform as a line, Winamp's oscilloscope
// fire:   flames fed by the spectrum, a nod to Winamp's AVS and MilkDrop
class Visualiser : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString style READ style WRITE setStyle NOTIFY styleChanged)
    Q_PROPERTY(QList<qreal> levels READ levels WRITE setLevels NOTIFY dataChanged)
    Q_PROPERTY(QList<qreal> peaks READ peaks WRITE setPeaks NOTIFY dataChanged)
    Q_PROPERTY(QList<qreal> waveform READ waveform WRITE setWaveform NOTIFY dataChanged)
    Q_PROPERTY(QColor low READ low WRITE setLow NOTIFY coloursChanged)
    Q_PROPERTY(QColor mid READ mid WRITE setMid NOTIFY coloursChanged)
    Q_PROPERTY(QColor high READ high WRITE setHigh NOTIFY coloursChanged)
    Q_PROPERTY(QColor peak READ peak WRITE setPeak NOTIFY coloursChanged)
    // Gap between bars in pixels; 0 picks one from the width.
    Q_PROPERTY(int spacing READ spacing WRITE setSpacing NOTIFY coloursChanged)
    // How many frames have been painted (tests check that drawing happens).
    Q_PROPERTY(int frames READ frames NOTIFY framesChanged)

public:
    explicit Visualiser(QQuickItem *parent = nullptr);

    QString style() const { return m_style; }
    void setStyle(const QString &style);
    QList<qreal> levels() const { return m_levels; }
    void setLevels(const QList<qreal> &levels);
    QList<qreal> peaks() const { return m_peaks; }
    void setPeaks(const QList<qreal> &peaks);
    QList<qreal> waveform() const { return m_wave; }
    void setWaveform(const QList<qreal> &wave);
    QColor low() const { return m_low; }
    void setLow(const QColor &c);
    QColor mid() const { return m_mid; }
    void setMid(const QColor &c);
    QColor high() const { return m_high; }
    void setHigh(const QColor &c);
    QColor peak() const { return m_peak; }
    void setPeak(const QColor &c);
    int spacing() const { return m_spacing; }
    void setSpacing(int s);
    int frames() const { return m_frames; }

    void paint(QPainter *painter) override;

    // The colour at a height (0 bottom, 1 top) of a bar.
    QColor colourAt(qreal t) const;

signals:
    void styleChanged();
    void dataChanged();
    void coloursChanged();
    void framesChanged();

private:
    void paintBars(QPainter *p, bool caps);
    void paintMirror(QPainter *p);
    void paintScope(QPainter *p);
    void paintFire(QPainter *p);
    void stepFire();

    QString m_style = "peaks";
    QList<qreal> m_levels, m_peaks, m_wave;
    QColor m_low = QColor("#9ece6a"), m_mid = QColor("#7dcfff"), m_high = QColor("#FFD60A"),
           m_peak = QColor("#ffffff");
    int m_spacing = 0;
    int m_frames = 0;
    // The fire: heat per cell on a small grid shaped like the item (cells
    // about 6 pixels wide and 3 high), scaled up smoothly when drawn.
    int m_fireW = 0, m_fireH = 0;
    QList<quint8> m_heat;
    QImage m_fire;
    quint32 m_seed = 12345;
    // The oscilloscope's gain, eased towards filling most of the height.
    qreal m_gain = 2.0;
};
