#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <array>

// A 10-band graphic equaliser: one peaking biquad per band (RBJ cookbook,
// about an octave wide) on interleaved float stereo, with a preamp. Flat and
// switched off it is skipped entirely, so the signal is untouched.
class Equalizer {
public:
    static constexpr int Bands = 10;
    static const std::array<double, Bands> &frequencies();
    static QStringList presetNames();
    // Gains in dB for a preset name, or an empty list if there is none.
    static QList<double> preset(const QString &name);

    void setSampleRate(int rate);
    // Gains are clamped to ±12 dB, the preamp to ±12 dB.
    void configure(bool enabled, double preampDb, const QList<double> &bandsDb);
    void reset();
    bool active() const { return m_active; }

    void process(float *stereo, int frames);

    // The filter's gain at a frequency, in dB, for tests and a future curve.
    double responseDb(double frequency) const;

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2] = {0, 0}, z2[2] = {0, 0};
    };
    void design();

    int m_rate = 48000;
    bool m_enabled = false, m_active = false;
    double m_preampDb = 0, m_preamp = 1;
    std::array<double, Bands> m_gains{};
    std::array<Biquad, Bands> m_filters;
    std::array<bool, Bands> m_on{};
};
