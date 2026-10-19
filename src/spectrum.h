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

    // Exposed for tests: band levels for mono samples at a sample rate.
    void analyse(const QList<float> &mono, int sampleRate);

private:
    int m_bands;
    QList<qreal> m_levels, m_peaks;
};
