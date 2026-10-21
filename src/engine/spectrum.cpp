#include "spectrum.h"

#include <QAudioFormat>
#include <cmath>

namespace {

// The auto-range for each sensitivity (see spectrum.h). window: decibels
// from the bottom of a bar to the top. offset: where the top sits against
// the reference (negative fills sooner). floor: the lowest the reference
// goes, so hiss and silence stay low. curve: the exponent applied after
// (above 1 keeps the average low and the peaks tall). whiten: how far each
// group's own ceiling pulls its reference down from the loudest group's.
// kick: how much a jump in the bass adds to the lowest bands.
struct Range {
    double window, offset, floor, curve, whiten, kick;
};
constexpr Range kRanges[3] = {
    {36, 3, -54, 1.35, 0.5, 1.0},  // low
    {34, 0, -62, 1.2, 0.65, 1.6},  // normal
    {32, -4, -70, 1.05, 0.75, 2.2}, // high
};
constexpr int kGroup = 4;          // bands per ceiling
constexpr double kStep = 0.033;    // seconds between decay() calls
constexpr double kHold = 1.0;      // seconds a ceiling holds before falling
constexpr double kFall = 6.0;      // decibels a second it then falls
constexpr double kNothing = -200;

} // namespace

Spectrum::Spectrum(int bands) : m_bands(bands) { reset(); }

void Spectrum::setSensitivity(const QString &sensitivity) {
    m_sensitivity = sensitivity == QLatin1String("low") ? 0 : sensitivity == QLatin1String("high") ? 2 : 1;
}

QString Spectrum::sensitivity() const {
    return m_sensitivity == 0 ? QStringLiteral("low") : m_sensitivity == 2 ? QStringLiteral("high") : QStringLiteral("normal");
}

void Spectrum::reset() {
    m_levels = QList<qreal>(m_bands, 0.0);
    m_peaks = QList<qreal>(m_bands, 0.0);
    const int groups = qMax(1, (m_bands + kGroup - 1) / kGroup);
    m_ceiling = QList<double>(groups, kNothing);
    m_held = QList<double>(groups, 0.0);
    m_db = QList<double>(m_bands, kNothing);
    m_lastBass = 0;
    m_wave = QList<qreal>(wavePoints, 0.0);
    m_stereo = QList<qreal>(2 * wavePoints, 0.0);
}

// The left and right channels over the scope's stretch, each point the
// strongest sample of its share, as takeWave does for the mono line.
void Spectrum::takeStereo(const QAudioBuffer &buffer, int start, int take) {
    const QAudioFormat format = buffer.format();
    const int channels = format.channelCount();
    const int frames = qMin(take, 512);
    const int first = start + take - frames;
    if (frames < wavePoints)
        return;
    auto sample = [&](int frame, int channel) -> float {
        const int s = frame * channels + qMin(channel, channels - 1);
        switch (format.sampleFormat()) {
        case QAudioFormat::Float: return buffer.constData<float>()[s];
        case QAudioFormat::Int16: return buffer.constData<qint16>()[s] / 32768.f;
        case QAudioFormat::Int32: return buffer.constData<qint32>()[s] / 2147483648.f;
        case QAudioFormat::UInt8: return (buffer.constData<quint8>()[s] - 128) / 128.f;
        default: return 0.f;
        }
    };
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < wavePoints; ++i) {
            const int from = first + i * frames / wavePoints, to = first + (i + 1) * frames / wavePoints;
            float best = 0;
            for (int k = from; k < to; ++k) {
                const float v = sample(k, c);
                if (std::abs(v) > std::abs(best))
                    best = v;
            }
            m_stereo[c * wavePoints + i] = best;
        }
    }
}

void Spectrum::takeWave(const QList<float> &mono) {
    const int n = mono.size();
    if (n < wavePoints)
        return;
    // The newest 512 frames (about 11 ms at 44.1 kHz) fill the scope.
    const int take = qMin(n, 512);
    const int start = n - take;
    for (int i = 0; i < wavePoints; ++i) {
        const int from = start + i * take / wavePoints, to = start + (i + 1) * take / wavePoints;
        float best = 0;
        for (int k = from; k < to; ++k)
            if (std::abs(mono[k]) > std::abs(best))
                best = mono[k];
        m_wave[i] = best;
    }
}

void Spectrum::feed(const QAudioBuffer &buffer) {
    const QAudioFormat format = buffer.format();
    const int channels = format.channelCount();
    const int frames = buffer.frameCount();
    if (channels <= 0 || frames <= 0 || format.sampleRate() <= 0)
        return;

    // Down-mix to mono floats. Only the last 1024 frames matter for a 60 Hz
    // display, which keeps the work small.
    const int take = qMin(frames, 1024);
    const int start = frames - take;
    QList<float> mono(take, 0.f);
    for (int i = 0; i < take; ++i) {
        float sum = 0;
        for (int c = 0; c < channels; ++c) {
            const int s = (start + i) * channels + c;
            switch (format.sampleFormat()) {
            case QAudioFormat::Float: sum += buffer.constData<float>()[s]; break;
            case QAudioFormat::Int16: sum += buffer.constData<qint16>()[s] / 32768.f; break;
            case QAudioFormat::Int32: sum += buffer.constData<qint32>()[s] / 2147483648.f; break;
            case QAudioFormat::UInt8: sum += (buffer.constData<quint8>()[s] - 128) / 128.f; break;
            default: return;
            }
        }
        mono[i] = sum / channels;
    }
    takeWave(mono);
    takeStereo(buffer, start, take);
    analyse(mono, format.sampleRate());
}

void Spectrum::analyse(const QList<float> &mono, int sampleRate) {
    const int n = mono.size();
    if (n < 16)
        return;
    const double low = 50, high = qMin(16000.0, sampleRate / 2.0 - 1);
    for (int b = 0; b < m_bands; ++b) {
        const double freq = low * std::pow(high / low, (b + 0.5) / m_bands);
        const double w = 2 * M_PI * freq / sampleRate;
        const double coeff = 2 * std::cos(w);
        double s1 = 0, s2 = 0;
        for (int i = 0; i < n; ++i) {
            // A Hann window keeps a loud bass note from smearing into every band.
            const double hann = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
            const double s = mono[i] * hann + coeff * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        const double magnitude = std::sqrt(qMax(0.0, power)) / (n / 4.0);
        // Decibels, with a gentle lift for the treble the way analysers do,
        // or the top half of the bars barely moves.
        m_db[b] = 20 * std::log10(magnitude + 1e-9) + 4.0 * b / m_bands;
    }

    // Each group's ceiling jumps up to its loudest band at once.
    const Range &range = kRanges[m_sensitivity];
    const int groups = int(m_ceiling.size());
    for (int g = 0; g < groups; ++g) {
        double loudest = kNothing;
        for (int b = g * kGroup; b < qMin(m_bands, (g + 1) * kGroup); ++b)
            loudest = qMax(loudest, m_db[b]);
        if (loudest >= m_ceiling[g]) {
            m_ceiling[g] = loudest;
            m_held[g] = 0;
        }
    }
    double top = range.floor;
    for (double c : std::as_const(m_ceiling))
        top = qMax(top, c);
    const int kickBands = qMax(1, m_bands / 4);
    double bass = 0;
    QList<qreal> placed(m_bands, 0.0);
    for (int b = 0; b < m_bands; ++b) {
        // The group's ceiling at this band, straight between the groups'
        // middles so neighbouring groups don't step.
        const double at = qBound(0.0, (b + 0.5) / kGroup - 0.5, double(groups - 1));
        const int g = int(at);
        const double own = g + 1 < groups ? m_ceiling[g] + (m_ceiling[g + 1] - m_ceiling[g]) * (at - g) : m_ceiling[g];
        const double reference = top - range.whiten * (top - qMax(own, range.floor)) + range.offset;
        const double t = qBound(0.0, (m_db[b] - (reference - range.window)) / range.window, 1.0);
        placed[b] = std::pow(t, range.curve);
        if (b < 3)
            bass += placed[b] / qMin(3, m_bands);
    }
    // A kick: the bass jumping since the last buffer lifts the lowest bands,
    // most at the bottom.
    const double kick = qMin(0.3, range.kick * qMax(0.0, bass - m_lastBass));
    m_lastBass = bass;
    for (int b = 0; b < m_bands; ++b) {
        const qreal level = qMin(1.0, placed[b] + kick * qMax(0.0, 1.0 - double(b) / kickBands));
        m_levels[b] = qMax(level, m_levels[b] * 0.7);
        if (m_levels[b] > m_peaks[b])
            m_peaks[b] = m_levels[b];
    }
}

void Spectrum::decay() {
    // The ceilings hold for a moment after the music gets quieter, then
    // fall, so a quiet passage soon fills the bars again.
    for (int g = 0; g < m_ceiling.size(); ++g) {
        m_held[g] += kStep;
        if (m_held[g] > kHold && m_ceiling[g] > kNothing)
            m_ceiling[g] = qMax(kNothing, m_ceiling[g] - kFall * kStep);
    }
    for (int b = 0; b < m_bands; ++b) {
        m_levels[b] = qMax(0.0, m_levels[b] - 0.04);
        m_peaks[b] = qMax(m_levels[b], m_peaks[b] - 0.012);
    }
    // Between buffers the line settles towards silence.
    for (qreal &v : m_wave)
        v *= 0.85;
    for (qreal &v : m_stereo)
        v *= 0.85;
}
