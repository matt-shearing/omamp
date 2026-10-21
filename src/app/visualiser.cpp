#include "visualiser.h"

#include <QElapsedTimer>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QRadialGradient>
#include <QSGRendererInterface>
#include <cmath>

Visualiser::Visualiser(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    m_tick.setTimerType(Qt::PreciseTimer);
    connect(&m_tick, &QTimer::timeout, this, &Visualiser::onTick);
    m_points.reserve(260);
    m_curve.reserve(1200);
}

bool Visualiser::isAnimatedStyle(const QString &style) {
    return style == "skyline" || style == "ribbon" || style == "line" || style == "pulse" || style == "led"
        || style == "synthwave" || style == "stars";
}

void Visualiser::setStyle(const QString &style) {
    if (style == m_style)
        return;
    m_style = style;
    m_heat.fill(0);
    invalidateCaches();
    m_maskValid = false;
    emit styleChanged();
    updateClock();
    update();
}

void Visualiser::setLevels(const QList<qreal> &levels) {
    m_levels = levels;
    if (m_style == "fire")
        stepFire();
    emit dataChanged();
    // The animated styles repaint on their own clock, and the oscilloscope
    // when the waveform arrives.
    if ((!animated() || !m_tick.isActive()) && m_style != "scope")
        update();
}

void Visualiser::setPeaks(const QList<qreal> &peaks) {
    m_peaks = peaks;
    if ((!animated() || !m_tick.isActive()) && m_style != "scope")
        update();
}

void Visualiser::setWaveform(const QList<qreal> &wave) {
    m_wave = wave;
    if (m_style == "scope")
        update();
}

void Visualiser::setStereoWaveform(const QList<qreal> &wave) {
    m_stereo = wave;
}

void Visualiser::setIdle(bool on) {
    if (on == m_idle)
        return;
    m_idle = on;
    emit idleChanged();
    updateClock();
    update();
}

void Visualiser::setReduceMotion(bool on) {
    if (on == m_reduceMotion)
        return;
    m_reduceMotion = on;
    emit idleChanged();
    updateClock();
    update();
}

void Visualiser::setFade(bool on) {
    if (on == m_fade)
        return;
    m_fade = on;
    m_maskValid = false;
    emit fadeChanged();
    updateClock();
    update();
}

void Visualiser::setQuietEdge(const QString &edge) {
    const QString e = edge == "bottom" ? QStringLiteral("bottom") : QStringLiteral("top");
    if (e == m_quietEdge)
        return;
    m_quietEdge = e;
    m_maskValid = false;
    emit fadeChanged();
    update();
}

void Visualiser::setQuietAreas(const QVariantList &areas) {
    if (areas == m_quietAreas)
        return;
    m_quietAreas = areas;
    m_maskValid = false;
    emit fadeChanged();
    update();
}

void Visualiser::setBehind(bool on) {
    if (on == m_behind)
        return;
    m_behind = on;
    m_maskValid = false;
    emit fadeChanged();
    updateClock();
    update();
}

void Visualiser::setCentre(qreal x) {
    if (qFuzzyCompare(x + 2, m_centre + 2))
        return;
    m_centre = x;
    // The synthwave backdrop and the fade are drawn around the centre.
    invalidateCaches();
    m_maskValid = false;
    emit fadeChanged();
    update();
}

qreal Visualiser::centreX() const { return m_centre >= 0 ? qBound(0.0, m_centre, width()) : width() / 2; }

void Visualiser::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged || change == ItemSceneChange)
        updateClock();
}

void Visualiser::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        invalidateCaches();
        m_maskValid = false;
        updateClock();
    }
}

// The clock runs only while an animated style shows. Idle, it slows to 30
// frames a second; idle with reduced motion, it stops on a still frame.
void Visualiser::updateClock() {
    const bool run = animated() && isVisible() && window() && width() >= 2 && height() >= 2
                     && !(m_idle && m_reduceMotion);
    if (!run) {
        m_tick.stop();
        return;
    }
    // Behind the whole bar on the software renderer, every frame also
    // redraws the words and buttons over the visualiser on the CPU, which
    // costs several times the visualiser itself: 30 frames a second there.
    const bool heavy = m_fade && m_behind && window()->rendererInterface()
                       && window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    m_tick.setInterval(m_idle || m_reduceMotion || heavy ? 33 : 16);
    if (!m_tick.isActive()) {
        m_clock.restart();
        m_tick.start();
    }
}

void Visualiser::onTick() {
    const qreal dt = qBound(0.0, m_clock.restart() / 1000.0, 0.1);
    advance(dt);
}

void Visualiser::invalidateCaches() { m_cachesValid = false; }

qreal Visualiser::random() {
    m_seed = m_seed * 1664525u + 1013904223u;
    return ((m_seed >> 8) & 0xffffff) / qreal(0x1000000);
}

qreal Visualiser::levelAt(qreal pos) const {
    const int n = m_smooth.size();
    if (n == 0)
        return 0;
    const qreal f = qBound(0.0, pos, 1.0) * (n - 1);
    const int i = int(f);
    if (i + 1 >= n)
        return m_smooth[n - 1];
    return m_smooth[i] + (m_smooth[i + 1] - m_smooth[i]) * (f - i);
}

int Visualiser::ledRows() const { return qMax(3, int(height() / qBound(4.0, height() / 10, 10.0))); }
int Visualiser::ledColumns() const { return qMax(4, int(width() / (height() / ledRows()))); }

// One step of the animation: the bands ease towards the analyser (or, idle,
// towards a slow gentle wave), the bass is followed for the pulse and the
// kick, and the stars and the grid move.
void Visualiser::advance(qreal dt) {
    m_dt = dt;
    const qreal speed = m_reduceMotion ? 0.5 : 1.0;
    m_time += dt * speed;
    const int n = m_levels.isEmpty() ? 24 : int(m_levels.size());
    if (m_smooth.size() != n)
        m_smooth = QVector<qreal>(n, 0.0);
    const qreal attack = 1 - std::exp(-dt * (m_reduceMotion ? 8 : 18));
    const qreal release = 1 - std::exp(-dt * (m_reduceMotion ? 3 : 6));
    qreal sum = 0;
    for (int i = 0; i < n; ++i) {
        const qreal target = m_idle ? 0.05 + 0.05 * (0.5 + 0.5 * std::sin(m_time * 0.9 + i * 0.45))
                                    : qBound(0.0, m_levels.value(i), 1.0);
        m_smooth[i] += (target - m_smooth[i]) * (target > m_smooth[i] ? attack : release);
        sum += m_smooth[i];
    }
    m_energy = sum / n;
    qreal bass = 0;
    for (int i = 0; i < qMin(3, n); ++i)
        bass += m_smooth[i];
    bass /= qMin(3, n);
    m_bass += (bass - m_bass) * (bass > m_bass ? attack : release);
    m_bassAverage += (m_bass - m_bassAverage) * (1 - std::exp(-dt * 1.5));
    // A kick: the bass jumps well above its recent average.
    if (!m_reduceMotion && !m_idle && m_bass - m_bassAverage > 0.08 && m_kick < 0.35)
        m_kick = 1;
    m_kick *= std::exp(-dt * 4);

    if (m_style == "ribbon") {
        const int points = 2 * 128;
        if (m_ribbon.size() != points)
            m_ribbon = QVector<qreal>(points, 0.0);
        const bool stereo = m_stereo.size() == points;
        const qreal k = 1 - std::exp(-dt * (m_reduceMotion ? 10 : 25));
        qreal loudest = 0;
        for (int i = 0; i < points; ++i) {
            qreal target = 0;
            if (!m_idle)
                target = stereo ? m_stereo[i] : m_wave.value(i % 128);
            m_ribbon[i] += (target - m_ribbon[i]) * k;
            loudest = qMax(loudest, qAbs(m_ribbon[i]));
        }
        const qreal gain = m_idle ? 1.0 : loudest > 0.01 ? qMin(6.0, 0.8 / loudest) : m_ribbonGain;
        m_ribbonGain += (gain - m_ribbonGain) * (1 - std::exp(-dt * 3));
    } else if (m_style == "led") {
        const int cols = ledColumns();
        if (m_hold.size() != cols)
            m_hold = QVector<qreal>(cols, 0.0);
        for (int c = 0; c < cols; ++c)
            m_hold[c] = qMax(levelAt(cols > 1 ? qreal(c) / (cols - 1) : 0), m_hold[c] - dt * 0.45 * speed);
    } else if (m_style == "synthwave") {
        m_scroll += dt * speed * (m_idle ? 0.25 : 0.35 + 1.4 * m_energy + 1.2 * m_kick);
        m_scroll = std::fmod(m_scroll, 1000.0);
    } else if (m_style == "stars") {
        if (m_starsW != width() || m_starsH != height())
            seedStars();
        const qreal push = m_idle ? 5 : 14 + 70 * m_energy + 260 * m_kick;
        for (Star &s : m_stars) {
            s.x += push * s.speed * speed * dt;
            s.y += std::sin(m_time * 0.7 + s.twinkle) * dt * 2.5 * speed;
            if (s.x > width() + 4) {
                s.x = -4;
                s.y = random() * height();
            }
            if (s.y < 0)
                s.y += height();
            else if (s.y > height())
                s.y -= height();
        }
    }
    update();
}

void Visualiser::seedStars() {
    m_starsW = width();
    m_starsH = height();
    const int count = qBound(30, int(width() * height() / 450), 300);
    m_stars.resize(count);
    const int bands = qMax(1, int(m_smooth.size()));
    for (Star &s : m_stars) {
        s.x = random() * width();
        s.y = random() * height();
        s.speed = 0.3 + 0.7 * random();
        const qreal r = random();
        s.size = r < 0.7 ? 1.0 : r < 0.93 ? 1.6 : 2.4;
        s.twinkle = random() * 6.283;
        s.band = int(random() * bands);
    }
}

#define COLOUR_SETTER(name, member)                                                                                    \
    void Visualiser::name(const QColor &c) {                                                                           \
        if (c == member)                                                                                               \
            return;                                                                                                    \
        member = c;                                                                                                    \
        invalidateCaches();                                                                                            \
        emit coloursChanged();                                                                                         \
        update();                                                                                                      \
    }
COLOUR_SETTER(setLow, m_low)
COLOUR_SETTER(setMid, m_mid)
COLOUR_SETTER(setHigh, m_high)
COLOUR_SETTER(setPeak, m_peak)
#undef COLOUR_SETTER

void Visualiser::setSpacing(int s) {
    if (s == m_spacing)
        return;
    m_spacing = s;
    emit coloursChanged();
    update();
}

static QColor blend(const QColor &a, const QColor &b, qreal t) {
    t = qBound(0.0, t, 1.0);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

QColor Visualiser::colourAt(qreal t) const {
    return t < 0.5 ? blend(m_low, m_mid, t * 2) : blend(m_mid, m_high, (t - 0.5) * 2);
}

void Visualiser::paint(QPainter *painter) {
    if (width() < 2 || height() < 2)
        return;
    QElapsedTimer timer;
    timer.start();
    if (animated()) {
        // Painted before the clock has stepped (a first frame, or a still
        // frame while idle with reduced motion): take one step now.
        if (m_smooth.isEmpty())
            advance(1.0 / 60);
        ensureCaches();
    }
    if (m_style == "skyline")
        paintSkyline(painter);
    else if (m_style == "ribbon")
        paintRibbon(painter);
    else if (m_style == "line")
        paintLine(painter);
    else if (m_style == "pulse")
        paintPulse(painter);
    else if (m_style == "led")
        paintLed(painter);
    else if (m_style == "synthwave")
        paintSynthwave(painter);
    else if (m_style == "stars")
        paintStars(painter);
    else if (m_style == "scope")
        paintScope(painter);
    else if (m_style == "mirror")
        paintMirror(painter);
    else if (m_style == "fire")
        paintFire(painter);
    else
        paintBars(painter, m_style != "bars");
    if (m_fade) {
        // Everything drawn so far keeps only as much of itself as the mask
        // allows: one pass over the item, with the mask made ahead.
        ensureMask();
        painter->save();
        painter->resetTransform();
        painter->setOpacity(1);
        painter->setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter->drawImage(QRectF(0, 0, width(), height()), m_mask);
        painter->restore();
    }
    m_paintNs += timer.nsecsElapsed();
    ++m_frames;
    emit framesChanged();
}

// A list's value at a position 0..1, straight between its entries.
static qreal sampleAt(const QList<qreal> &values, qreal pos) {
    const int n = values.size();
    if (n == 0)
        return 0;
    const qreal f = qBound(0.0, pos, 1.0) * (n - 1);
    const int i = int(f);
    return i + 1 >= n ? values[n - 1] : values[i] + (values[i + 1] - values[i]) * (f - i);
}

// How many columns the bar styles draw. One per band, except behind other
// things (fade), where a band per column would make slabs as wide as a
// button: there the bands are spread over columns about 12 pixels wide.
static int barColumns(int bands, qreal width, bool fade) {
    return fade ? qBound(bands, int(width / 12), 160) : bands;
}

// The gradient spans the whole height, so a quiet bar stays the low colour
// and only loud ones reach the top colour, as in Winamp.
void Visualiser::paintBars(QPainter *p, bool caps) {
    const int bands = m_levels.size();
    if (bands == 0)
        return;
    const qreal w = width(), h = height();
    const int n = barColumns(bands, w, m_fade);
    const qreal gap = m_spacing > 0 ? m_spacing : (w / n > 8 ? 2 : 1);
    const qreal bar = (w - gap * (n - 1)) / n;
    QLinearGradient g(0, h, 0, 0);
    g.setColorAt(0, m_low);
    g.setColorAt(0.5, m_mid);
    g.setColorAt(1, m_high);
    p->setPen(Qt::NoPen);
    for (int i = 0; i < n; ++i) {
        const qreal x = i * (bar + gap);
        const qreal pos = n > 1 ? qreal(i) / (n - 1) : 0;
        const qreal level = qBound(0.0, n == bands ? m_levels[i] : sampleAt(m_levels, pos), 1.0);
        const qreal top = h - qMax(1.0, level * h);
        p->fillRect(QRectF(x, top, bar, h - top), g);
        const qreal peak = n == bands ? m_peaks.value(i) : sampleAt(m_peaks, pos);
        if (caps && peak > 0.02) {
            const qreal y = h - peak * h - 2;
            p->fillRect(QRectF(x, qMax(0.0, y), bar, 2), m_peak);
        }
    }
}

// The bars grow up and down from the middle line; the lower half is the
// reflection, fainter.
void Visualiser::paintMirror(QPainter *p) {
    const int bands = m_levels.size();
    if (bands == 0)
        return;
    const qreal w = width(), h = height(), mid = h * 0.58;
    const int n = barColumns(bands, w / 2, m_fade);
    const qreal gap = m_spacing > 0 ? m_spacing : (w / (2 * n) > 8 ? 2 : 1);
    // Low frequencies in the middle, spreading out to both sides.
    const int columns = n * 2;
    const qreal bar = (w - gap * (columns - 1)) / columns;
    QLinearGradient up(0, mid, 0, 0), down(0, mid, 0, h);
    up.setColorAt(0, m_low);
    up.setColorAt(0.5, m_mid);
    up.setColorAt(1, m_high);
    QColor lowFaint = m_low, midFaint = m_mid;
    lowFaint.setAlphaF(m_low.alphaF() * 0.55);
    midFaint.setAlphaF(m_mid.alphaF() * 0.15);
    down.setColorAt(0, lowFaint);
    down.setColorAt(1, midFaint);
    p->setPen(Qt::NoPen);
    for (int c = 0; c < columns; ++c) {
        const int band = c < n ? n - 1 - c : c - n;
        const qreal level =
            qBound(0.0, n == bands ? m_levels[band] : sampleAt(m_levels, n > 1 ? qreal(band) / (n - 1) : 0), 1.0);
        const qreal x = c * (bar + gap);
        const qreal above = qMax(1.0, level * mid);
        const qreal below = qMax(1.0, level * (h - mid));
        p->fillRect(QRectF(x, mid - above, bar, above), up);
        p->fillRect(QRectF(x, mid, bar, below), down);
    }
}

// The waveform as one line across the middle, with a faint centre line.
void Visualiser::paintScope(QPainter *p) {
    const qreal w = width(), h = height(), mid = h / 2;
    QColor centre = m_mid;
    centre.setAlphaF(0.25);
    p->setPen(QPen(centre, 1));
    p->drawLine(QPointF(0, mid), QPointF(w, mid));
    const int n = m_wave.size();
    if (n < 2)
        return;
    // A quiet track still fills most of the height: the gain eases towards
    // what would put the loudest point at 85 %, never more than eight times.
    qreal loudest = 0;
    for (qreal v : m_wave)
        loudest = qMax(loudest, qAbs(v));
    const qreal target = loudest > 0.01 ? qMin(8.0, 0.85 / loudest) : m_gain;
    m_gain += (target - m_gain) * 0.15;
    QPainterPath path;
    for (int i = 0; i < n; ++i) {
        const qreal x = i * w / (n - 1);
        const qreal y = mid - qBound(-1.0, m_wave[i] * m_gain, 1.0) * (h / 2 - 2);
        if (i == 0)
            path.moveTo(x, y);
        else
            path.lineTo(x, y);
    }
    QLinearGradient g(0, 0, w, 0);
    g.setColorAt(0, m_low);
    g.setColorAt(0.5, m_mid);
    g.setColorAt(1, m_high);
    p->setPen(QPen(QBrush(g), h > 120 ? 2.5 : 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(Qt::NoBrush);
    p->drawPath(path);
}

// The Doom fire: the bottom row is set alight by the spectrum, and every
// frame each cell's heat moves up a row, drifting a cell left or right at
// random and cooling a little on the way, which makes the tongues of flame.
void Visualiser::stepFire() {
    const int w = qBound(24, int(width() / 6), 200), h = qBound(12, int(height() / 3), 80);
    if (w != m_fireW || h != m_fireH) {
        m_fireW = w;
        m_fireH = h;
        m_heat = QList<quint8>(w * h, 0);
        m_fire = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
        m_fire.fill(Qt::transparent);
    }
    const int n = m_levels.size();
    auto rand = [this] {
        m_seed = m_seed * 1664525u + 1013904223u;
        return int((m_seed >> 16) & 0x7fff);
    };
    // The bottom row follows the spectrum, smoothly across the bands, with
    // a little flicker.
    for (int x = 0; x < w; ++x) {
        const qreal pos = n > 1 ? qreal(x) * (n - 1) / qMax(1, w - 1) : 0;
        const int i = int(pos);
        const qreal level = n == 0 ? 0 : i + 1 < n ? m_levels[i] + (m_levels[i + 1] - m_levels[i]) * (pos - i) : m_levels[i];
        const int heat = int(qBound(0.0, level * 1.25, 1.0) * 255);
        m_heat[(h - 1) * w + x] = quint8(qMax(0, heat - rand() % 24));
    }
    // Each cell takes the average of the cells under it and cools a little,
    // the old demo-scene fire. The cooling is set so that full heat climbs
    // most of the way up, and a quiet band stays a low glow.
    const int cooling = qMax(2, 2 * 290 / h);
    for (int y = 0; y < h - 1; ++y) {
        const int below = (y + 1) * w, further = qMin(h - 1, y + 2) * w;
        for (int x = 0; x < w; ++x) {
            const int l = qMax(0, x - 1), r = qMin(w - 1, x + 1);
            const int sum = m_heat[below + l] + m_heat[below + x] + m_heat[below + r] + m_heat[further + x];
            m_heat[y * w + x] = quint8(qMax(0, sum / 4 - rand() % cooling));
        }
    }
    // The heat as colours: clear, then the top colour, the middle one and
    // the peak colour at the hottest.
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(m_fire.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const qreal t = m_heat[y * w + x] / 255.0;
            QColor c;
            if (t < 0.3) {
                c = m_high;
                c.setAlphaF(m_high.alphaF() * t / 0.3);
            } else if (t < 0.7) {
                c = blend(m_high, m_mid, (t - 0.3) / 0.4);
            } else {
                c = blend(m_mid, m_peak, (t - 0.7) / 0.3);
            }
            line[x] = qPremultiply(c.rgba());
        }
    }
}

void Visualiser::paintFire(QPainter *p) {
    if (m_fire.isNull())
        return;
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);
    p->drawImage(QRectF(0, 0, width(), height()), m_fire);
}

// ---------------------------------------------------------------------------
// The animated styles. Everything that depends only on the size and the
// colours (gradients, the LED grid, the sun, the glow) is drawn once into
// m_* members here and reused every frame.

static qreal smoothStep(qreal from, qreal to, qreal x) {
    const qreal t = qBound(0.0, (x - from) / (to - from), 1.0);
    return t * t * (3 - 2 * t);
}

static QColor withAlpha(QColor c, qreal a) {
    c.setAlphaF(qBound(0.0, c.alphaF() * a, 1.0));
    return c;
}

// Where the synthwave horizon sits: low in a thin strip, which has room for
// the sun and the city but not much floor.
static qreal synthHorizon(qreal h) { return h < 32 ? h * 0.8 : h * 0.58; }

// Tall enough for the whole scene.
static bool synthGrand(qreal h) { return h >= 72; }

// A soft round glow in one colour, as a small image stretched when drawn:
// cheaper than a radial gradient over the whole strip every frame.
static QImage glowImage(const QColor &colour) {
    QImage img(128, 32, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    QRadialGradient g(QPointF(0, 0), 1);
    g.setColorAt(0, withAlpha(colour, 1));
    g.setColorAt(0.4, withAlpha(colour, 0.45));
    g.setColorAt(1, withAlpha(colour, 0));
    QBrush b(g);
    b.setTransform(QTransform::fromTranslate(64, 16).scale(64, 16));
    p.fillRect(img.rect(), b);
    return img;
}

void Visualiser::ensureCaches() {
    if (m_cachesValid)
        return;
    m_cachesValid = true;
    const qreal w = width(), h = height();
    if (m_style == "skyline") {
        const qreal base = h * (h > 40 ? 0.72 : 0.78);
        QLinearGradient up(0, base, 0, 0);
        up.setColorAt(0, m_low);
        up.setColorAt(0.5, m_mid);
        up.setColorAt(1, m_high);
        m_skyBrush = QBrush(up);
        QLinearGradient down(0, base, 0, h);
        down.setColorAt(0, withAlpha(m_mid, 0.35));
        down.setColorAt(1, withAlpha(m_mid, 0));
        m_reflectBrush = QBrush(down);
    } else if (m_style == "line") {
        QLinearGradient fill(0, 0, 0, h);
        fill.setColorAt(0, withAlpha(m_high, 0.32));
        fill.setColorAt(0.5, withAlpha(m_mid, 0.14));
        fill.setColorAt(1, withAlpha(m_mid, 0));
        m_lineFill = QBrush(fill);
        QLinearGradient stroke(0, 0, w, 0);
        stroke.setColorAt(0, m_low);
        stroke.setColorAt(0.5, m_mid);
        stroke.setColorAt(1, m_high);
        m_lineStroke = QBrush(stroke);
    } else if (m_style == "pulse" || m_style == "synthwave") {
        m_glowCore = glowImage(m_high);
        m_glowWide = glowImage(m_mid);
        QLinearGradient haze(0, 0, 0, h);
        haze.setColorAt(0, withAlpha(m_high, 0));
        haze.setColorAt(0.58, withAlpha(m_high, 0.16));
        haze.setColorAt(0.6, withAlpha(m_mid, 0.10));
        haze.setColorAt(1, withAlpha(m_mid, 0));
        m_skyBrush = QBrush(haze);
        if (m_style == "synthwave")
            makeSynthwaveBackdrop();
    } else if (m_style == "led") {
        const int rows = ledRows(), cols = ledColumns();
        const qreal pitch = h / rows, cell = pitch - qMax(1.0, pitch * 0.22);
        const qreal x0 = (w - cols * pitch) / 2;
        m_rowColours.resize(rows);
        for (int r = 0; r < rows; ++r)
            m_rowColours[r] = colourAt(rows > 1 ? qreal(r) / (rows - 1) : 0);
        m_ledGrid = QImage(qMax(1, int(std::ceil(w))), qMax(1, int(std::ceil(h))), QImage::Format_ARGB32_Premultiplied);
        m_ledGrid.fill(Qt::transparent);
        QPainter p(&m_ledGrid);
        for (int c = 0; c < cols; ++c)
            for (int r = 0; r < rows; ++r)
                p.fillRect(QRectF(x0 + c * pitch, h - (r + 1) * pitch, cell, cell), withAlpha(m_rowColours[r], 0.13));
    }
}

// ---------------------------------------------------------------------------
// The fade for drawing behind other things (`fade`).

qreal Visualiser::fadeStrength(const QString &style) {
    // The scenes and the sparse styles carry the bar; the dense ones would
    // crowd the words, so they are held back.
    if (style == "synthwave" || style == "stars")
        return 0.95;
    if (style == "pulse")
        return 0.9;
    if (style == "line" || style == "scope")
        return 0.8;
    if (style == "ribbon")
        return 0.7;
    if (style == "skyline" || style == "fire")
        return 0.6;
    return 0.45; // bars, peaks, mirror, led
}


// The mask: full strength in the middle, falling smoothly to nothing over
// the last sixth of the width at each side; strongest towards the edge away
// from quietEdge and faint at quietEdge; and dimmed under each quiet area,
// with a soft border so no box shows. Made per pixel once per size, style
// and set of areas, and reused every frame.
void Visualiser::ensureMask() {
    const int w = qMax(1, int(std::ceil(width()))), h = qMax(1, int(std::ceil(height())));
    if (m_maskValid && m_mask.width() == w && m_mask.height() == h)
        return;
    m_maskValid = true;
    if (m_mask.width() != w || m_mask.height() != h)
        m_mask = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    const qreal strength = m_behind ? fadeStrength(m_style) : 1.0;
    const qreal side = qBound(24.0, w * 0.17, 300.0);
    QVector<qreal> across(w), down(h);
    for (int x = 0; x < w; ++x)
        across[x] = smoothStep(0, side, qMin(x + 0.5, w - x - 0.5));
    const bool quietTop = m_quietEdge != "bottom";
    for (int y = 0; y < h; ++y) {
        // s runs from 0 at the quiet edge to 1 at the other.
        const qreal s = quietTop ? (y + 0.5) / h : 1 - (y + 0.5) / h;
        down[y] = (0.2 + 0.8 * smoothStep(0, 0.55, s)) * (0.75 + 0.25 * smoothStep(0, 0.12, 1 - s));
    }
    // The last few pixels before the quiet edge go to nothing, centrepiece
    // included, so no line shows where the item ends.
    QVector<qreal> rim(h);
    const qreal rimPixels = qMax(6.0, h * 0.08);
    for (int y = 0; y < h; ++y)
        rim[y] = smoothStep(0, rimPixels, quietTop ? y + 0.5 : h - y - 0.5);
    // Around the centrepiece the picture stays at full strength, however
    // near the quiet edge: an oval about the centre, soft at its rim.
    const qreal cx = centreX(), cy = h * 0.55, rx = qMax(40.0, w * 0.11), ry = h * 0.75;
    QVector<QRectF> areas;
    for (const QVariant &v : std::as_const(m_quietAreas)) {
        const QRectF r = v.toRectF();
        if (r.isValid())
            areas.append(r);
    }
    const qreal feather = qMax(10.0, h * 0.22);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(m_mask.scanLine(y));
        const qreal py = y + 0.5;
        for (int x = 0; x < w; ++x) {
            const qreal px = x + 0.5;
            const qreal ex = (px - cx) / rx, ey = (py - cy) / ry;
            const qreal spot = 1 - smoothStep(0.55, 1.0, std::sqrt(ex * ex + ey * ey));
            qreal a = strength * across[x] * rim[y] * qMax(down[y], spot);
            for (const QRectF &r : std::as_const(areas)) {
                const qreal dx = qMax(0.0, qMax(r.left() - px, px - r.right()));
                const qreal dy = qMax(0.0, qMax(r.top() - py, py - r.bottom()));
                const qreal d = dx == 0 || dy == 0 ? dx + dy : std::sqrt(dx * dx + dy * dy);
                a *= 1 - 0.72 * (1 - smoothStep(0, feather, d));
            }
            line[x] = qRgba(0, 0, 0, qBound(0, int(a * 255 + 0.5), 255));
        }
    }
}

qreal Visualiser::fadeAt(qreal x, qreal y) {
    if (!m_fade)
        return 1;
    ensureMask();
    const int px = qBound(0, int(x), m_mask.width() - 1), py = qBound(0, int(y), m_mask.height() - 1);
    return qAlpha(m_mask.pixel(px, py)) / 255.0;
}

// Rounded bars with the bass in the middle (or at `centre`), spreading out
// to both sides, standing on a line with their reflection fading below it.
void Visualiser::paintSkyline(QPainter *p) {
    const qreal w = width(), h = height(), cx = centreX();
    const qreal base = h * (h > 40 ? 0.72 : 0.78);
    int cols = qBound(12, int(w / 11), 128);
    cols -= cols % 2;
    const qreal colW = w / cols, gap = qMax(1.0, colW * 0.28), bar = colW - gap;
    const qreal radius = qMin(bar / 2, 3.5);
    // The spectrum runs from the centre out to the further edge, so the
    // two sides match where they overlap.
    const qreal span = qMax(colW, qMax(cx, w - cx) - colW / 2);
    auto tallAt = [&](int c) {
        const qreal d = qMax(0.0, qAbs(c * colW + colW / 2 - cx) - colW / 2) / span;
        return qMax(1.5, levelAt(d) * base * 0.96);
    };
    p->setPen(Qt::NoPen);
    p->setBrush(m_skyBrush);
    // The bars' rounded feet stand below the floor, clipped off.
    p->save();
    p->setClipRect(QRectF(0, 0, w, base));
    for (int c = 0; c < cols; ++c) {
        const qreal tall = tallAt(c);
        p->drawRoundedRect(QRectF(c * colW + gap / 2, base - tall, bar, tall + radius), radius, radius);
    }
    p->restore();
    for (int c = 0; c < cols; ++c)
        p->fillRect(QRectF(c * colW + gap / 2, base + 1, bar, qMin(tallAt(c) * 0.55, h - base - 1)), m_reflectBrush);
    // The floor.
    p->fillRect(QRectF(0, base, w, 1), withAlpha(m_mid, 0.3));
}

// Catmull-Rom through n points, `steps` straight pieces per span, appended
// to out. Thin straight lines are the cheapest thing QPainter strokes and
// fills; a path of curves costs several times as much on the CPU.
static void appendSpline(const QPointF *pts, int n, int steps, qreal top, qreal bottom, QPolygonF &out) {
    for (int i = 0; i + 1 < n; ++i) {
        const QPointF p0 = pts[qMax(0, i - 1)], p1 = pts[i], p2 = pts[i + 1], p3 = pts[qMin(n - 1, i + 2)];
        for (int s = 0; s < steps; ++s) {
            const qreal t = qreal(s) / steps, t2 = t * t, t3 = t2 * t;
            QPointF q = 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2
                               + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
            q.setY(qBound(top, q.y(), bottom));
            out.append(q);
        }
    }
    out.append(pts[n - 1]);
}

// Two smooth ribbons, one per channel, whose centre follows the waveform
// and whose thickness follows how loud each point is. Each ribbon is filled
// as one-pixel columns (plain rectangles are what QPainter draws fastest)
// and edged with thin smoothed lines.
void Visualiser::paintRibbon(QPainter *p) {
    const qreal w = width(), h = height(), mid = h / 2, amp = h / 2 - 2;
    const int n = 128;
    if (m_ribbon.size() != 2 * n)
        return;
    p->fillRect(QRectF(0, mid - 0.5, w, 1), withAlpha(m_low, 0.25));
    const int columns = qMax(2, int(w));
    const qreal minThick = qMax(0.8, h * 0.035);
    for (int c = 1; c >= 0; --c) {
        // The waveform at each point, then Catmull-Rom between them per pixel.
        m_points.resize(n);
        for (int i = 0; i < n; ++i) {
            qreal v = qBound(-1.0, m_ribbon[c * n + i] * m_ribbonGain, 1.0);
            if (m_idle)
                v += 0.05 * std::sin(i * 0.12 + m_time * 1.3 + c * 1.7);
            m_points[i] = QPointF(i, v);
        }
        const QColor colour = c == 0 ? m_mid : m_high;
        const QColor fill = withAlpha(colour, 0.38);
        QPolygonF &upper = m_curve, &lower = m_lower;
        upper.resize(0);
        lower.resize(0);
        p->setRenderHint(QPainter::Antialiasing, false);
        for (int x = 0; x < columns; ++x) {
            const qreal f = qreal(x) * (n - 1) / (columns - 1);
            const int i = qMin(n - 2, int(f));
            const qreal t = f - i, t2 = t * t, t3 = t2 * t;
            const qreal p0 = m_points[qMax(0, i - 1)].y(), p1 = m_points[i].y(), p2 = m_points[i + 1].y(),
                        p3 = m_points[qMin(n - 1, i + 2)].y();
            const qreal v = qBound(-1.0, 0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2
                                               + (-p0 + 3 * p1 - 3 * p2 + p3) * t3), 1.0);
            const qreal y = mid - v * amp * 0.82, thick = minThick + qAbs(v) * h * 0.09;
            p->fillRect(QRectF(x, y - thick, 1, 2 * thick), fill);
            if (x % 3 == 0 || x == columns - 1) {
                upper.append(QPointF(x, y - thick));
                lower.append(QPointF(x, y + thick));
            }
        }
        p->setRenderHint(QPainter::Antialiasing, true);
        QPen edge(withAlpha(colour, 0.9), 1);
        edge.setCosmetic(true);
        p->setPen(edge);
        p->drawPolyline(upper);
        p->drawPolyline(lower);
    }
}

// The spectrum as one smooth line (Catmull-Rom through the bands) over a
// shade that fades to nothing at the bottom. Faint copies a pixel or two
// above and below make the glow; thin lines and plain columns cost far less
// than wide strokes and curved fills.
void Visualiser::paintLine(QPainter *p) {
    const qreal w = width(), h = height();
    const int n = m_smooth.size();
    if (n < 2)
        return;
    m_points.resize(n);
    for (int i = 0; i < n; ++i)
        m_points[i] = QPointF(i * w / (n - 1), h - 2 - m_smooth[i] * (h - 6));
    m_curve.clear();
    appendSpline(m_points.constData(), n, qBound(2, int(w / (n - 1) / 4), 24), 2, h - 1, m_curve);
    // The shade: two-pixel columns down from the line.
    p->setRenderHint(QPainter::Antialiasing, false);
    const int count = m_curve.size();
    int k = 0;
    for (qreal x = 0; x < w; x += 2) {
        while (k + 2 < count && m_curve[k + 1].x() < x)
            ++k;
        const QPointF a = m_curve[k], b = m_curve[qMin(count - 1, k + 1)];
        const qreal y = b.x() > a.x() ? a.y() + (b.y() - a.y()) * qBound(0.0, (x - a.x()) / (b.x() - a.x()), 1.0) : a.y();
        p->fillRect(QRectF(x, y, 2, h - y), m_lineFill);
    }
    p->setRenderHint(QPainter::Antialiasing, true);
    QPen pen(withAlpha(m_mid, 0.14), 1);
    pen.setCosmetic(true);
    const qreal spread = h > 100 ? 2 : 1;
    for (qreal d : {-2 * spread, 2 * spread, -spread, spread}) {
        if (qAbs(d) < 2 * spread)
            pen.setColor(withAlpha(m_mid, 0.32));
        p->translate(0, d);
        p->setPen(pen);
        p->drawPolyline(m_curve);
        p->translate(0, -d);
    }
    // The line itself: one-pixel strokes side by side, which QPainter draws
    // several times faster than one wider stroke; three on a tall strip.
    pen.setBrush(m_lineStroke);
    pen.setWidthF(1);
    pen.setCosmetic(true);
    p->setPen(pen);
    const int passes = h > 100 ? 3 : 2;
    for (int i = 0; i < passes; ++i) {
        p->translate(0, i * 0.6);
        p->drawPolyline(m_curve);
        p->translate(0, -i * 0.6);
    }
}

// A soft glow in the middle that swells with the bass and brightens on a
// kick, with a line along the bottom that widens with the whole spectrum.
void Visualiser::paintPulse(QPainter *p) {
    const qreal w = width(), h = height(), cx = centreX();
    const qreal strength = qBound(0.0, 0.14 + 0.6 * m_bass + 0.25 * m_kick, 0.9);
    const qreal spread = qMin(1.0, 0.25 + 0.55 * m_energy + 0.12 * m_kick);
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);
    p->setOpacity(strength * 0.7);
    p->drawImage(QRectF(cx - w * spread, -h * 0.2, 2 * w * spread, h * 1.4), m_glowWide);
    p->setOpacity(strength);
    p->drawImage(QRectF(cx - w * spread * 0.45, h * 0.05, w * spread * 0.9, h * 0.9), m_glowCore);
    p->setOpacity(qMin(1.0, 0.35 + strength));
    const qreal line = w * qMin(1.0, 0.15 + 0.85 * m_energy);
    p->drawImage(QRectF(cx - line / 2, h - 3, line, 3), m_glowCore);
    p->setOpacity(1);
}

// Winamp-era LEDs: unlit cells drawn once, lit ones each frame, coloured by
// row, with a peak light that falls slowly.
void Visualiser::paintLed(QPainter *p) {
    const qreal w = width(), h = height();
    const int rows = ledRows(), cols = ledColumns();
    if (m_rowColours.size() != rows)
        return;
    const qreal pitch = h / rows, cell = pitch - qMax(1.0, pitch * 0.22);
    const qreal x0 = (w - cols * pitch) / 2;
    p->setRenderHint(QPainter::Antialiasing, false);
    // Behind other things the unlit cells would only be clutter.
    if (!m_fade)
        p->drawImage(QPointF(0, 0), m_ledGrid);
    for (int c = 0; c < cols; ++c) {
        const qreal level = levelAt(cols > 1 ? qreal(c) / (cols - 1) : 0);
        const int lit = qMin(rows, int(level * rows + 0.35));
        const qreal x = x0 + c * pitch;
        for (int r = 0; r < lit; ++r)
            p->fillRect(QRectF(x, h - (r + 1) * pitch, cell, cell), m_rowColours[r]);
        if (!m_idle && c < m_hold.size()) {
            const int top = qMin(rows - 1, int(m_hold[c] * rows));
            if (top >= lit && m_hold[c] > 0.08)
                p->fillRect(QRectF(x, h - (top + 1) * pitch, cell, cell), m_peak);
        }
    }
}

// The synthwave scene's still parts, drawn once per size, centre and
// palette: the sun, the sky, and the lines running to the vanishing point.
// A strip at least 72 pixels high (the large bar's full width, the big
// view) gets the whole scene: stars, two ranges of hills either side of
// the sun, and lines that fade into the distance.
void Visualiser::makeSynthwaveBackdrop() {
    const qreal w = width(), h = height(), cx = centreX(), horizon = synthHorizon(h);
    const bool grand = synthGrand(h);
    // The sun: the upper half of a disc, from the top colour to the middle
    // one, with the gaps that widen towards the horizon.
    const int r = qMax(4, int(grand ? qMin(horizon * 0.86, w * 0.09) : qMin(horizon * 0.92, w * 0.07)));
    m_sun = QImage(2 * r, r, QImage::Format_ARGB32_Premultiplied);
    m_sun.fill(Qt::transparent);
    {
        QPainter p(&m_sun);
        p.setRenderHint(QPainter::Antialiasing);
        QLinearGradient g(0, 0, 0, r);
        g.setColorAt(0, grand ? blend(m_high, m_peak, 0.25) : m_high);
        g.setColorAt(grand ? 0.45 : 0.0, m_high);
        g.setColorAt(1, colourAt(0.5));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(QRectF(0, 0, 2 * r, 2 * r));
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        const int stripes = r >= 40 ? 5 : r >= 24 ? 4 : r >= 12 ? 3 : 2;
        for (int i = 0; i < stripes; ++i) {
            const qreal y = r * (0.45 + 0.55 * (i + 0.5) / stripes);
            const qreal thick = qMax(1.0, r * (grand ? 0.028 : 0.035) * (i + 1));
            p.fillRect(QRectF(0, y - thick / 2, 2 * r, thick), Qt::transparent);
        }
    }
    m_backdrop = QImage(qMax(1, int(std::ceil(w))), qMax(1, int(std::ceil(h))), QImage::Format_ARGB32_Premultiplied);
    m_backdrop.fill(Qt::transparent);
    QPainter b(&m_backdrop);
    if (grand) {
        QLinearGradient towers(0, horizon - horizon * 0.55, 0, horizon);
        towers.setColorAt(0, withAlpha(m_mid, 0.8));
        towers.setColorAt(1, withAlpha(m_mid, 0.3));
        m_cityBrush = QBrush(towers);
        // A sky that warms towards the horizon, and a faintly lit floor.
        const qreal at = horizon / h;
        QLinearGradient sky(0, 0, 0, h);
        sky.setColorAt(0, withAlpha(m_high, 0));
        sky.setColorAt(at * 0.5, withAlpha(m_high, 0.04));
        sky.setColorAt(at, withAlpha(m_high, 0.30));
        sky.setColorAt(qMin(1.0, at + 0.004), withAlpha(m_mid, 0.16));
        sky.setColorAt(1, withAlpha(m_mid, 0.04));
        b.fillRect(QRectF(0, 0, w, h), sky);
        // Stars, fewer and fainter towards the horizon. Their own generator,
        // so the sky is the same every time it is drawn.
        quint32 seed = 7;
        auto rnd = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return ((seed >> 8) & 0xffffff) / qreal(0x1000000);
        };
        const int stars = qBound(20, int(w * horizon / 1500), 400);
        for (int i = 0; i < stars; ++i) {
            const qreal x = rnd() * w, y = horizon * 0.92 * std::pow(rnd(), 1.5), big = rnd();
            const qreal size = big > 0.94 ? 2 : 1;
            b.fillRect(QRectF(x, y, size, size), withAlpha(m_peak, (0.25 + 0.55 * rnd()) * (1 - y / horizon)));
        }
        // Two ranges of hills, low near the sun and taller towards the
        // sides; the nearer range is lower and brighter.
        b.setRenderHint(QPainter::Antialiasing);
        for (int range = 0; range < 2; ++range) {
            const qreal step = w / (range == 0 ? 46 : 70);
            const qreal top = horizon * (range == 0 ? 0.62 : 0.34);
            QPolygonF ridge;
            ridge << QPointF(-step, horizon);
            for (qreal x = -step; x <= w + step; x += step * (0.6 + 0.8 * rnd())) {
                const qreal away = smoothStep(r * 1.05, r * 2.6, qAbs(x - cx));
                const qreal tall = top * (0.25 + 0.75 * rnd()) * (0.15 + 0.85 * away);
                ridge << QPointF(x, horizon - tall);
            }
            ridge << QPointF(w + step, horizon);
            QLinearGradient fill(0, horizon - top, 0, horizon);
            fill.setColorAt(0, withAlpha(m_mid, range == 0 ? 0.16 : 0.22));
            fill.setColorAt(1, withAlpha(m_mid, range == 0 ? 0.04 : 0.06));
            b.setBrush(fill);
            QPen edge(withAlpha(m_mid, range == 0 ? 0.35 : 0.55), 1);
            edge.setCosmetic(true);
            b.setPen(edge);
            b.drawPolygon(ridge);
        }
    } else {
        b.fillRect(QRectF(0, 0, w, h), m_skyBrush);
        b.setRenderHint(QPainter::Antialiasing);
    }
    // The lines from the vanishing point, which never move; in the whole
    // scene they fade into the distance. A strip too thin for a floor gets
    // none.
    const qreal reach = qMax(cx, w - cx);
    const int spokes = h - horizon < 6 ? -1 : qBound(6, int(2 * reach / 60), 48);
    const qreal spacing = reach / spokes;
    QPen pen(withAlpha(m_mid, 0.40), grand ? 1.2 : 1);
    if (grand) {
        QLinearGradient fade(0, horizon, 0, h);
        fade.setColorAt(0, withAlpha(m_mid, 0.04));
        fade.setColorAt(1, withAlpha(m_mid, 0.6));
        pen.setBrush(fade);
    } else {
        pen.setCosmetic(true);
    }
    b.setPen(pen);
    for (int i = -spokes; i <= spokes; ++i)
        b.drawLine(QPointF(cx + i * spacing * 0.18, horizon), QPointF(cx + i * spacing * 1.6, h));
}

// Night-drive synthwave: a striped sun sitting on the horizon, a city of
// spectrum bars either side of it, and a grid rolling towards the viewer,
// faster with the music. In the whole scene the sun's light shimmers on
// the floor below it, too.
void Visualiser::paintSynthwave(QPainter *p) {
    const qreal w = width(), h = height(), cx = centreX(), horizon = synthHorizon(h);
    const bool grand = synthGrand(h);
    p->drawImage(QPointF(0, 0), m_backdrop);
    const qreal r = m_sun.width() / 2.0;
    // The city: bars rising from the horizon, the bass nearest the sun, the
    // same height at the same distance on both sides.
    // In the whole scene the towers stand further apart, a little lower,
    // and fade towards their feet.
    const qreal pitch = grand ? qBound(8.0, w / 110, 15.0) : qBound(4.0, w / 140, 9.0);
    const qreal bar = pitch * (grand ? 0.56 : 0.72);
    const int left = qMax(0, int((cx - r * 1.25) / pitch)), right = qMax(0, int((w - cx - r * 1.25) / pitch));
    const int each = qMax(left, right);
    const QColor city = withAlpha(m_mid, 0.55);
    const qreal reach = horizon * (grand ? 0.55 : 0.85);
    for (int i = 0; i < each; ++i) {
        const qreal level = levelAt(each > 1 ? qreal(i) / (each - 1) : 0);
        const qreal tall = qMax(1.0, level * reach);
        const qreal off = r * 1.25 + i * pitch;
        if (grand) {
            if (i < right)
                p->fillRect(QRectF(cx + off, horizon - tall, bar, tall), m_cityBrush);
            if (i < left)
                p->fillRect(QRectF(cx - off - bar, horizon - tall, bar, tall), m_cityBrush);
            continue;
        }
        if (i < right)
            p->fillRect(QRectF(cx + off, horizon - tall, bar, tall), city);
        if (i < left)
            p->fillRect(QRectF(cx - off - bar, horizon - tall, bar, tall), city);
    }
    // The sun and its glow, which follows the bass.
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);
    p->setOpacity(qBound(0.0, 0.25 + 0.5 * m_bass + 0.3 * m_kick, 1.0));
    p->drawImage(QRectF(cx - r * 2.6, horizon - r * 1.4, r * 5.2, r * 2.0), m_glowCore);
    p->setOpacity(1);
    p->drawImage(QPointF(cx - r, horizon - r), m_sun);
    const qreal ground = h - horizon;
    if (grand) {
        // The sun's reflection: short bands under it that narrow and fade
        // towards the viewer and waver with the music.
        const QColor shine = blend(m_high, m_peak, 0.15);
        for (int k = 0; k < 6; ++k) {
            const qreal z = (k + 0.5) / 6;
            const qreal y = horizon + 2 + ground * 0.8 * z * z + 1.2 * std::sin(m_time * 2.1 + k * 1.3);
            const qreal half = r * (0.95 - 0.55 * z) * (1 + 0.08 * std::sin(m_time * 3.3 + k * 2.1) + 0.2 * m_bass);
            p->fillRect(QRectF(cx - half, y, 2 * half, z > 0.5 ? 2 : 1),
                        withAlpha(shine, (0.55 - 0.4 * z) * (0.6 + 0.6 * m_bass)));
        }
    }
    // The ground: rows rolling forward over the lines from the vanishing point.
    const int rowsN = ground < 6 ? 0 : grand ? 8 : 6;
    for (int i = 0; i < rowsN; ++i) {
        const qreal z = std::fmod(i + m_scroll * rowsN, qreal(rowsN)) / rowsN;
        const qreal y = horizon + ground * z * z;
        p->fillRect(QRectF(0, y, w, grand && z > 0.55 ? 2 : 1), withAlpha(m_high, 0.15 + 0.6 * z));
    }
    if (grand)
        p->fillRect(QRectF(0, horizon - 1, w, 3), withAlpha(m_high, 0.3));
    p->fillRect(QRectF(0, horizon, w, 1), withAlpha(m_high, 0.85));
}

// A drifting star field: each star belongs to a band and brightens with it;
// the bass pushes the whole field along, and a kick leaves short trails.
void Visualiser::paintStars(QPainter *p) {
    if (m_stars.isEmpty())
        return;
    const int bands = qMax(1, int(m_smooth.size()));
    const qreal push = m_idle ? 0 : 70 * m_energy + 260 * m_kick;
    p->setRenderHint(QPainter::Antialiasing, false);
    for (const Star &s : m_stars) {
        const qreal band = m_smooth.value(qMin(s.band, bands - 1));
        const qreal glow = qBound(0.0, 0.4 + 0.25 * (0.5 + 0.5 * std::sin(m_time * 2 + s.twinkle)) + 0.75 * band
                                           + 0.35 * m_kick,
                                  1.0);
        // Behind other things (fade), the stars keep to the theme's colours
        // (the peak colour is near black in a light theme) and the bigger
        // ones get a soft halo, so they read as stars rather than specks;
        // trails stay short.
        const QColor colour = s.size > 2 ? m_high : band > 0.55 && !m_fade ? m_peak : m_mid;
        const qreal trail = push * s.speed * (m_fade ? 0.025 : 0.05);
        if (trail > 2)
            p->fillRect(QRectF(s.x - trail, s.y, trail, qMax(1.0, s.size * 0.6)), withAlpha(colour, glow * 0.25));
        if (m_fade && s.size > 1)
            p->fillRect(QRectF(s.x - 1, s.y - 1, s.size + 2, s.size + 2), withAlpha(colour, glow * 0.3));
        p->fillRect(QRectF(s.x, s.y, s.size, s.size), withAlpha(colour, glow));
    }
}
