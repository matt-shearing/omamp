#pragma once

#include <QBrush>
#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPolygonF>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QVariantList>
#include <QVector>

// The analyser and its friends, drawn on the CPU with QPainter so they work
// with the software renderer and cost little: a few dozen rectangles or one
// path a frame, or one small image for the fire.
//
//   Visualiser {
//       style: "peaks"            // see Appearance::visualisers()
//       levels: backend.levels    // 0..1 per band
//       peaks: backend.peaks
//       waveform: backend.waveform // -1..1, the last few milliseconds
//       stereoWaveform: backend.stereoWaveform // left then right
//       idle: backend.state !== "playing"     // no sound reaches Thrum
//       reduceMotion: appearance.reduceMotion
//       low: "green"; mid: "cyan"; high: "yellow"; peak: "white"
//   }
//
// The first five follow the data as it arrives, about 30 times a second:
//
// bars:   bars rising from green through the middle colour to the top one
// peaks:  the same with a falling cap on each bar (Thrum's own analyser)
// mirror: the bars reflected about the middle line, quieter below
// scope:  the waveform as a line, Winamp's oscilloscope
// fire:   flames fed by the spectrum, a nod to Winamp's AVS and MilkDrop
//
// The rest were made for the large bar's wide, short strip and animate on
// their own clock (60 frames a second, 30 with reduceMotion), easing
// between the analyser's updates. While idle they show a calm idle
// animation instead, or one still frame with reduceMotion.
//
// skyline:   soft rounded bars, bass in the middle, with a fading reflection
// ribbon:    the left and right waveforms as two smooth, filled ribbons
// line:      the spectrum as one smooth line with a glow and a filled shade
// pulse:     a soft glow that swells with the bass and kicks on the beat
// led:       a Winamp-era LED matrix with peak lights
// synthwave: a sun on the horizon, a city of bars, a grid rolling forward
// stars:     a drifting star field, pushed along by the bass
//
// With `fade` on, the picture is drawn to sit behind other things, as the
// large bar's "Full bar" placement does: it fades out towards the left and
// right edges and towards `quietEdge`, is dimmed under `quietAreas` (the
// words and buttons drawn over it), and is held to a strength that suits
// its style. The fade is one alpha mask, made when the size, the style or
// the areas change and applied with one composition each frame. `centre`
// puts the centrepiece of the styles that have one (the synthwave sun, the
// pulse, the skyline's bass) at that x instead of the middle.
class Visualiser : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString style READ style WRITE setStyle NOTIFY styleChanged)
    Q_PROPERTY(QList<qreal> levels READ levels WRITE setLevels NOTIFY dataChanged)
    Q_PROPERTY(QList<qreal> peaks READ peaks WRITE setPeaks NOTIFY dataChanged)
    Q_PROPERTY(QList<qreal> waveform READ waveform WRITE setWaveform NOTIFY dataChanged)
    Q_PROPERTY(QList<qreal> stereoWaveform READ stereoWaveform WRITE setStereoWaveform NOTIFY dataChanged)
    Q_PROPERTY(QColor low READ low WRITE setLow NOTIFY coloursChanged)
    Q_PROPERTY(QColor mid READ mid WRITE setMid NOTIFY coloursChanged)
    Q_PROPERTY(QColor high READ high WRITE setHigh NOTIFY coloursChanged)
    Q_PROPERTY(QColor peak READ peak WRITE setPeak NOTIFY coloursChanged)
    // Gap between bars in pixels; 0 picks one from the width.
    Q_PROPERTY(int spacing READ spacing WRITE setSpacing NOTIFY coloursChanged)
    // No sound reaches the analyser: paused, stopped, or Spotify playing.
    Q_PROPERTY(bool idle READ idle WRITE setIdle NOTIFY idleChanged)
    // Slower, calmer animation and no beat flashes.
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY idleChanged)
    // Whether the style runs on its own clock (the newer styles).
    Q_PROPERTY(bool animated READ animated NOTIFY styleChanged)
    // Fade out towards the edges, to sit behind other things.
    Q_PROPERTY(bool fade READ fade WRITE setFade NOTIFY fadeChanged)
    // The edge that fades out the most: "top" or "bottom".
    Q_PROPERTY(QString quietEdge READ quietEdge WRITE setQuietEdge NOTIFY fadeChanged)
    // Rectangles (in the item's coordinates) to dim, under words and buttons.
    Q_PROPERTY(QVariantList quietAreas READ quietAreas WRITE setQuietAreas NOTIFY fadeChanged)
    // Whether words and buttons are drawn over it (the default): the fade
    // then also holds each style to the strength that keeps them readable.
    Q_PROPERTY(bool behind READ behind WRITE setBehind NOTIFY fadeChanged)
    // Where the centrepiece sits across the item; negative is the middle.
    Q_PROPERTY(qreal centre READ centre WRITE setCentre NOTIFY fadeChanged)
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
    QList<qreal> stereoWaveform() const { return m_stereo; }
    void setStereoWaveform(const QList<qreal> &wave);
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
    bool idle() const { return m_idle; }
    void setIdle(bool on);
    bool reduceMotion() const { return m_reduceMotion; }
    void setReduceMotion(bool on);
    bool animated() const { return isAnimatedStyle(m_style); }
    int frames() const { return m_frames; }
    bool fade() const { return m_fade; }
    void setFade(bool on);
    QString quietEdge() const { return m_quietEdge; }
    void setQuietEdge(const QString &edge);
    QVariantList quietAreas() const { return m_quietAreas; }
    void setQuietAreas(const QVariantList &areas);
    bool behind() const { return m_behind; }
    void setBehind(bool on);
    qreal centre() const { return m_centre; }
    void setCentre(qreal x);

    void paint(QPainter *painter) override;

    // The colour at a height (0 bottom, 1 top) of a bar.
    QColor colourAt(qreal t) const;

    // How strongly a style shows behind other things with `fade` on, 0..1:
    // busy, bright styles are held back further so text stays readable.
    static qreal fadeStrength(const QString &style);
    // The fade mask's alpha (0..1) at a point, for tests.
    qreal fadeAt(qreal x, qreal y);

    // The styles that run on their own clock.
    static bool isAnimatedStyle(const QString &style);
    // For the CPU measurement: time spent in paint() since the last reset.
    qint64 paintNanos() const { return m_paintNs; }
    void resetPaintStats() { m_paintNs = 0; }
    // Whether the clock is running (tests: idle with reduceMotion stops it).
    bool ticking() const { return m_tick.isActive(); }
    // Moves the animation on by dt seconds and repaints, as the clock does.
    void advance(qreal dt);

signals:
    void styleChanged();
    void dataChanged();
    void coloursChanged();
    void framesChanged();
    void idleChanged();
    void fadeChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void paintBars(QPainter *p, bool caps);
    void paintMirror(QPainter *p);
    void paintScope(QPainter *p);
    void paintFire(QPainter *p);
    void stepFire();

    void paintSkyline(QPainter *p);
    void paintRibbon(QPainter *p);
    void paintLine(QPainter *p);
    void paintPulse(QPainter *p);
    void paintLed(QPainter *p);
    void paintSynthwave(QPainter *p);
    void paintStars(QPainter *p);

    // Starts or stops the clock for the style, visibility and idle state.
    void updateClock();
    void onTick();
    // Drops what was drawn ahead for a size or a palette.
    void invalidateCaches();
    // The eased band level at a position 0..1 across the bands.
    qreal levelAt(qreal pos) const;
    // A random number 0..1 from the item's own generator.
    qreal random();
    void seedStars();
    void ensureCaches();
    void ensureMask();
    void makeSynthwaveBackdrop();
    // The centrepiece's x: `centre` when set, else the middle.
    qreal centreX() const;
    int ledRows() const;
    int ledColumns() const;

    QString m_style = "peaks";
    QList<qreal> m_levels, m_peaks, m_wave, m_stereo;
    QColor m_low = QColor("#9ece6a"), m_mid = QColor("#7dcfff"), m_high = QColor("#FFD60A"),
           m_peak = QColor("#ffffff");
    int m_spacing = 0;
    int m_frames = 0;
    bool m_idle = false;
    bool m_reduceMotion = false;
    // The fire: heat per cell on a small grid shaped like the item (cells
    // about 6 pixels wide and 3 high), scaled up smoothly when drawn.
    int m_fireW = 0, m_fireH = 0;
    QList<quint8> m_heat;
    QImage m_fire;
    quint32 m_seed = 12345;
    // The oscilloscope's gain, eased towards filling most of the height.
    qreal m_gain = 2.0;

    // The clock of the animated styles and what it eases.
    QTimer m_tick;
    QElapsedTimer m_clock;
    qreal m_time = 0;          // seconds of animation
    QVector<qreal> m_smooth;   // eased band levels
    QVector<qreal> m_hold;     // the LED matrix's peak lights, per column
    QVector<qreal> m_ribbon;   // eased stereo waveform, left then right
    qreal m_bass = 0, m_bassAverage = 0, m_kick = 0, m_energy = 0;
    qreal m_ribbonGain = 2.0;
    qreal m_scroll = 0;        // the synthwave grid's travel
    qreal m_dt = 0;            // the last step, for the LED peak lights

    // Drawn ahead and reused while the size and colours stay.
    bool m_cachesValid = false;
    QImage m_ledGrid, m_sun, m_backdrop, m_glowCore, m_glowWide;
    QBrush m_skyBrush, m_reflectBrush, m_lineFill, m_lineStroke, m_cityBrush;
    QVector<QColor> m_rowColours;
    // Reused every frame, so painting allocates nothing.
    QPolygonF m_points, m_curve, m_lower;
    QVector<QRectF> m_rects;

    struct Star {
        qreal x, y, speed, size, twinkle;
        int band;
    };
    QVector<Star> m_stars;
    qreal m_starsW = 0, m_starsH = 0;

    bool m_fade = false;
    QString m_quietEdge = "top";
    QVariantList m_quietAreas;
    qreal m_centre = -1;
    bool m_behind = true;
    bool m_maskValid = false;
    QImage m_mask;

    qint64 m_paintNs = 0;
};
