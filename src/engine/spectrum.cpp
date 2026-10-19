#include "spectrum.h"

#include <QAudioFormat>
#include <cmath>

Spectrum::Spectrum(int bands) : m_bands(bands) { reset(); }

void Spectrum::reset() {
    m_levels = QList<qreal>(m_bands, 0.0);
    m_peaks = QList<qreal>(m_bands, 0.0);
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
        // Decibels mapped to 0..1 over a 60 dB range; treble gets a gentle lift
        // the way analysers do, or the top half of the bars barely moves.
        const double db = 20 * std::log10(magnitude + 1e-9) + 4.0 * b / m_bands;
        const qreal level = qBound(0.0, (db + 60) / 60, 1.0);
        m_levels[b] = qMax(level, m_levels[b] * 0.7);
        if (m_levels[b] > m_peaks[b])
            m_peaks[b] = m_levels[b];
    }
}

void Spectrum::decay() {
    for (int b = 0; b < m_bands; ++b) {
        m_levels[b] = qMax(0.0, m_levels[b] - 0.04);
        m_peaks[b] = qMax(m_levels[b], m_peaks[b] - 0.012);
    }
}
