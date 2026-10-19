#include "equalizer.h"

#include <cmath>
#include <complex>

namespace {
// About one octave per band, so neighbouring bands overlap smoothly.
constexpr double kQ = 1.41;

struct Preset {
    const char *name;
    std::array<double, Equalizer::Bands> gains;
};

const Preset kPresets[] = {
    {"Flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Rock", {4, 3, 2, 0, -1, -1, 0, 2, 3, 4}},
    {"Pop", {-1, 1, 3, 4, 3, 0, -1, -1, -1, -1}},
    {"Jazz", {3, 2, 1, 2, -1, -1, 0, 1, 2, 3}},
    {"Classical", {0, 0, 0, 0, 0, 0, -2, -3, -3, -4}},
    {"Electronic", {4, 3, 1, 0, -2, 1, 0, 1, 3, 4}},
    {"Bass boost", {6, 5, 4, 2, 0, 0, 0, 0, 0, 0}},
    {"Treble boost", {0, 0, 0, 0, 0, 1, 2, 4, 5, 6}},
    {"Vocal", {-2, -2, -1, 1, 3, 3, 2, 1, 0, -1}},
    {"Loudness", {5, 4, 2, 0, -1, 0, 1, 3, 4, 3}},
};
} // namespace

const std::array<double, Equalizer::Bands> &Equalizer::frequencies() {
    static const std::array<double, Bands> f = {31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    return f;
}

QStringList Equalizer::presetNames() {
    QStringList names;
    for (const auto &p : kPresets)
        names << QString::fromLatin1(p.name);
    return names;
}

QList<double> Equalizer::preset(const QString &name) {
    for (const auto &p : kPresets)
        if (name.compare(QLatin1String(p.name), Qt::CaseInsensitive) == 0)
            return QList<double>(p.gains.begin(), p.gains.end());
    return {};
}

void Equalizer::setSampleRate(int rate) {
    if (rate == m_rate || rate <= 0)
        return;
    m_rate = rate;
    design();
    reset();
}

void Equalizer::configure(bool enabled, double preampDb, const QList<double> &bandsDb) {
    m_enabled = enabled;
    m_preampDb = qBound(-12.0, preampDb, 12.0);
    for (int b = 0; b < Bands; ++b)
        m_gains[b] = b < bandsDb.size() ? qBound(-12.0, bandsDb[b], 12.0) : 0.0;
    design();
}

void Equalizer::reset() {
    for (auto &f : m_filters)
        f.z1[0] = f.z1[1] = f.z2[0] = f.z2[1] = 0;
}

void Equalizer::design() {
    m_preamp = std::pow(10.0, m_preampDb / 20.0);
    bool any = m_preampDb != 0;
    for (int b = 0; b < Bands; ++b) {
        const double f = frequencies()[b];
        // A band at or past Nyquist (16 kHz at a low rate) is left out.
        m_on[b] = m_gains[b] != 0 && f < m_rate * 0.45;
        any = any || m_on[b];
        if (!m_on[b])
            continue;
        const double A = std::pow(10.0, m_gains[b] / 40.0);
        const double w0 = 2 * M_PI * f / m_rate;
        const double alpha = std::sin(w0) / (2 * kQ);
        const double cosw = std::cos(w0);
        const double a0 = 1 + alpha / A;
        Biquad &q = m_filters[b];
        // Coefficients change in place; the state carries over so a slider
        // move doesn't click.
        q.b0 = (1 + alpha * A) / a0;
        q.b1 = -2 * cosw / a0;
        q.b2 = (1 - alpha * A) / a0;
        q.a1 = -2 * cosw / a0;
        q.a2 = (1 - alpha / A) / a0;
    }
    m_active = m_enabled && any;
}

void Equalizer::process(float *stereo, int frames) {
    if (!m_active)
        return;
    for (int b = 0; b < Bands; ++b) {
        if (!m_on[b])
            continue;
        Biquad &q = m_filters[b];
        for (int c = 0; c < 2; ++c) {
            double z1 = q.z1[c], z2 = q.z2[c];
            float *s = stereo + c;
            for (int i = 0; i < frames; ++i, s += 2) {
                // Transposed direct form II, in double for low-frequency bands.
                const double x = *s;
                const double y = q.b0 * x + z1;
                z1 = q.b1 * x - q.a1 * y + z2;
                z2 = q.b2 * x - q.a2 * y;
                *s = float(y);
            }
            q.z1[c] = z1;
            q.z2[c] = z2;
        }
    }
    if (m_preamp != 1.0) {
        const float g = float(m_preamp);
        for (int i = 0; i < frames * 2; ++i)
            stereo[i] *= g;
    }
}

double Equalizer::responseDb(double frequency) const {
    if (!m_active)
        return 0;
    const std::complex<double> z = std::polar(1.0, -2 * M_PI * frequency / m_rate);
    std::complex<double> h = m_preamp;
    for (int b = 0; b < Bands; ++b) {
        if (!m_on[b])
            continue;
        const Biquad &q = m_filters[b];
        h *= (q.b0 + q.b1 * z + q.b2 * z * z) / (1.0 + q.a1 * z + q.a2 * z * z);
    }
    return 20 * std::log10(std::abs(h));
}
