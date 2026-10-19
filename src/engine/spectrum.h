#pragma once

#include <QAudioBuffer>
#include <QList>

// Turns decoded audio into the bouncing bars of a Winamp-style analyser:
// one Goertzel filter per band on log-spaced frequencies, then a fast attack
// and a slow fall so the bars move like the original.
class Spectrum {
public:
    explicit Spectrum(int bands = 24);

    void feed(const QAudioBuffer &buffer);
    void decay();
    void reset();
    QList<qreal> levels() const { return m_levels; }
    QList<qreal> peaks() const { return m_peaks; }
    // The last few milliseconds of sound as 128 points from -1 to 1, for
    // the oscilloscope. Each point is the strongest sample of its stretch,
    // so short peaks survive the thinning.
    QList<qreal> waveform() const { return m_wave; }
    static constexpr int wavePoints = 128;

    // Exposed for tests: band levels for mono samples at a sample rate.
    void analyse(const QList<float> &mono, int sampleRate);

private:
    int m_bands;
    QList<qreal> m_levels, m_peaks, m_wave;
    void takeWave(const QList<float> &mono);
};
