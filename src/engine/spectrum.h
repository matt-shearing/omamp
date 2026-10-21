#pragma once

#include <QAudioBuffer>
#include <QList>

// Turns decoded audio into the bouncing bars of a Winamp-style analyser:
// one Goertzel filter per band on log-spaced frequencies, then a fast attack
// and a slow fall so the bars move like the original.
//
// The levels are auto-ranged, so a loud modern master and a quiet old
// recording both move the bars over most of their height. Each group of
// four bands keeps a ceiling: the loudest it has been lately, in decibels,
// which jumps up at once, holds for a second and then falls slowly. A band
// is placed in a window of about 34 dB below a reference made from the
// loudest group and its own group's ceiling (half way between, so treble
// still moves without looking as loud as the bass), then put through a
// gentle curve that keeps the average low and the peaks tall. A jump in
// the bass (a kick drum) lifts the lowest bands a little further. The
// sensitivity (Low, Normal, High) moves the reference, the curve and the
// kick; Normal suits loud masters as well as quiet ones.
class Spectrum {
public:
    explicit Spectrum(int bands = 24);

    void feed(const QAudioBuffer &buffer);
    void decay();
    void reset();
    // "low", "normal" (the default) or "high"; anything else is normal.
    void setSensitivity(const QString &sensitivity);
    QString sensitivity() const;
    QList<qreal> levels() const { return m_levels; }
    QList<qreal> peaks() const { return m_peaks; }
    // The last few milliseconds of sound as 128 points from -1 to 1, for
    // the oscilloscope. Each point is the strongest sample of its stretch,
    // so short peaks survive the thinning.
    QList<qreal> waveform() const { return m_wave; }
    // The same stretch per channel, left then right (wavePoints each), for
    // the waveform ribbon. A mono source repeats its one channel.
    QList<qreal> stereoWaveform() const { return m_stereo; }
    static constexpr int wavePoints = 128;

    // Exposed for tests: band levels for mono samples at a sample rate.
    void analyse(const QList<float> &mono, int sampleRate);

private:
    int m_bands;
    int m_sensitivity = 1; // 0 low, 1 normal, 2 high
    QList<qreal> m_levels, m_peaks, m_wave, m_stereo;
    // The auto-range: each group's ceiling in decibels and how long it has
    // held, the last bass level (for the kick), and per-band scratch space.
    QList<double> m_ceiling, m_held, m_db;
    double m_lastBass = 0;
    void takeWave(const QList<float> &mono);
    void takeStereo(const QAudioBuffer &buffer, int start, int take);
};
