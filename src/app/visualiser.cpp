#include "visualiser.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

Visualiser::Visualiser(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
}

void Visualiser::setStyle(const QString &style) {
    if (style == m_style)
        return;
    m_style = style;
    m_heat.fill(0);
    emit styleChanged();
    update();
}

void Visualiser::setLevels(const QList<qreal> &levels) {
    m_levels = levels;
    if (m_style == "fire")
        stepFire();
    emit dataChanged();
    update();
}

void Visualiser::setPeaks(const QList<qreal> &peaks) {
    m_peaks = peaks;
    update();
}

void Visualiser::setWaveform(const QList<qreal> &wave) {
    m_wave = wave;
    if (m_style == "scope")
        update();
}

#define COLOUR_SETTER(name, member)                                                                                    \
    void Visualiser::name(const QColor &c) {                                                                           \
        if (c == member)                                                                                               \
            return;                                                                                                    \
        member = c;                                                                                                    \
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
    if (m_style == "scope")
        paintScope(painter);
    else if (m_style == "mirror")
        paintMirror(painter);
    else if (m_style == "fire")
        paintFire(painter);
    else
        paintBars(painter, m_style != "bars");
    ++m_frames;
    emit framesChanged();
}

// The gradient spans the whole height, so a quiet bar stays the low colour
// and only loud ones reach the top colour, as in Winamp.
void Visualiser::paintBars(QPainter *p, bool caps) {
    const int n = m_levels.size();
    if (n == 0)
        return;
    const qreal w = width(), h = height();
    const qreal gap = m_spacing > 0 ? m_spacing : (w / n > 8 ? 2 : 1);
    const qreal bar = (w - gap * (n - 1)) / n;
    QLinearGradient g(0, h, 0, 0);
    g.setColorAt(0, m_low);
    g.setColorAt(0.5, m_mid);
    g.setColorAt(1, m_high);
    p->setPen(Qt::NoPen);
    for (int i = 0; i < n; ++i) {
        const qreal x = i * (bar + gap);
        const qreal level = qBound(0.0, m_levels[i], 1.0);
        const qreal top = h - qMax(1.0, level * h);
        p->fillRect(QRectF(x, top, bar, h - top), g);
        if (caps && i < m_peaks.size() && m_peaks[i] > 0.02) {
            const qreal y = h - m_peaks[i] * h - 2;
            p->fillRect(QRectF(x, qMax(0.0, y), bar, 2), m_peak);
        }
    }
}

// The bars grow up and down from the middle line; the lower half is the
// reflection, fainter.
void Visualiser::paintMirror(QPainter *p) {
    const int n = m_levels.size();
    if (n == 0)
        return;
    const qreal w = width(), h = height(), mid = h * 0.58;
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
        const qreal level = qBound(0.0, m_levels[band], 1.0);
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
