#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include "layoutprefs.h"

class Theme;

// How the player's chrome looks: the deck style, the visualiser, whether
// the deck shows the cover, and the big view's background. QML sees it as
// `appearance`. Kept in window.conf under [appearance], beside the window's
// own settings.
//
// The deck, the mini player and the big view draw with `appearance.deck`,
// a map of named colours (panelTop, lcdText, buttonGlyph…). In the Omarchy
// style every one of them comes from the current theme and changes with it;
// Classic and Silver are fixed palettes that ignore the theme. The library
// window always draws with `appearance.plain`, the Omarchy one, so advanced
// mode stays plain whatever the deck wears.
class Appearance : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString deckStyle READ deckStyle WRITE setDeckStyle NOTIFY changed)
    Q_PROPERTY(QString visualiser READ visualiser WRITE setVisualiser NOTIFY changed)
    Q_PROPERTY(bool coverInDeck READ coverInDeck WRITE setCoverInDeck NOTIFY changed)
    Q_PROPERTY(QString bigBackground READ bigBackground WRITE setBigBackground NOTIFY changed)
    Q_PROPERTY(bool bigLyrics READ bigLyrics WRITE setBigLyrics NOTIFY changed)
    // The big view's visualiser runs edge to edge and fades out to its ends
    // and its top, instead of sitting in a band inside the margins.
    Q_PROPERTY(bool bigVisualiserFade READ bigVisualiserFade WRITE setBigVisualiserFade NOTIFY changed)
    // The large now playing bar's visualiser: where it sits ("off",
    // "behind" the seek bar, behind the whole bar ("full"), along the
    // "edge", "beside" the cover), its own
    // style or "" to share the player's, and the style in effect.
    Q_PROPERTY(QString barVisualiser READ barVisualiser WRITE setBarVisualiser NOTIFY changed)
    Q_PROPERTY(QString barVisualiserStyle READ barVisualiserStyle WRITE setBarVisualiserStyle NOTIFY changed)
    Q_PROPERTY(QString barStyle READ barStyle NOTIFY changed)
    // While no sound reaches Omamp (paused, stopped, Spotify): "calm" keeps
    // a slow idle animation in the large bar, "hide" hides its visualiser.
    Q_PROPERTY(QString idleVisualiser READ idleVisualiser WRITE setIdleVisualiser NOTIFY changed)
    // Slower, calmer visualisers with no beat flashes.
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY changed)
    Q_PROPERTY(QStringList barPlacements READ barPlacements CONSTANT)
    Q_PROPERTY(QVariantMap deck READ deck NOTIFY paletteChanged)
    Q_PROPERTY(QVariantMap plain READ plain NOTIFY paletteChanged)
    Q_PROPERTY(QStringList deckStyles READ deckStyles CONSTANT)
    Q_PROPERTY(QStringList visualisers READ visualisers CONSTANT)
    // The library window's layout (layoutprefs.h): the Art column, the
    // sidebar's sections and the now playing bar.
    Q_PROPERTY(LayoutPrefs *layout READ layout CONSTANT)

public:
    // settingsPath: the window.conf file; empty keeps everything in memory
    // (tests). theme: the Omarchy theme the "omarchy" style follows.
    explicit Appearance(const QString &settingsPath = {}, Theme *theme = nullptr, QObject *parent = nullptr);

    QString deckStyle() const { return m_deckStyle; }
    void setDeckStyle(const QString &style);
    QString visualiser() const { return m_visualiser; }
    void setVisualiser(const QString &style);
    bool coverInDeck() const { return m_coverInDeck; }
    void setCoverInDeck(bool on);
    QString bigBackground() const { return m_bigBackground; }
    void setBigBackground(const QString &kind);
    bool bigLyrics() const { return m_bigLyrics; }
    void setBigLyrics(bool on);
    bool bigVisualiserFade() const { return m_bigVisualiserFade; }
    void setBigVisualiserFade(bool on);
    QString barVisualiser() const { return m_barVisualiser; }
    void setBarVisualiser(const QString &place);
    QString barVisualiserStyle() const { return m_barVisualiserStyle; }
    void setBarVisualiserStyle(const QString &style);
    QString barStyle() const { return m_barVisualiserStyle.isEmpty() ? m_visualiser : m_barVisualiserStyle; }
    QString idleVisualiser() const { return m_idleVisualiser; }
    void setIdleVisualiser(const QString &kind);
    bool reduceMotion() const { return m_reduceMotion; }
    void setReduceMotion(bool on);
    // "off", "behind", "full" (behind the whole bar, fading out to its
    // edges), "edge", "beside".
    static QStringList barPlacements() { return {"off", "behind", "full", "edge", "beside"}; }

    LayoutPrefs *layout() const { return m_layout; }

    QVariantMap deck() const { return m_deck; }
    QVariantMap plain() const { return m_plain; }

    // "omarchy", "classic", "silver".
    static QStringList deckStyles();
    // "bars", "peaks", "mirror", "scope", "fire", then the ones made for
    // the large bar: "skyline", "ribbon", "line", "pulse", "led",
    // "synthwave", "stars".
    static QStringList visualisers();

    // The next visualiser after the current one, round the list.
    Q_INVOKABLE QString nextVisualiser();
    // The large bar's next style: its own when it has one, else the shared one.
    Q_INVOKABLE QString nextBarVisualiser();
    // Shift+V: the large bar's style when the bar's visualiser is what
    // shows (inBar), else the shared one.
    Q_INVOKABLE QString cycleVisualiser(bool inBar);
    Q_INVOKABLE QString barPlacementName(const QString &place) const;
    // A style's name for the window ("Omarchy", "Classic", "Silver").
    Q_INVOKABLE QString deckStyleName(const QString &style) const;
    Q_INVOKABLE QString visualiserName(const QString &style) const;

    // The named colours for a style. The omarchy style needs the theme's
    // palette (Theme::colors()) and rounding.
    static QVariantMap palette(const QString &style, const QVariantMap &themeColors, int themeRadius, bool themeLight);

signals:
    void changed();
    void paletteChanged();

private:
    void rebuild();
    void save(const QString &key, const QVariant &value);

    QString m_path;
    QPointer<Theme> m_theme;
    QString m_deckStyle = "omarchy";
    QString m_visualiser = "peaks";
    bool m_coverInDeck = true;
    QString m_bigBackground = "blur";
    bool m_bigLyrics = true;
    bool m_bigVisualiserFade = false;
    QString m_barVisualiser = "behind";
    QString m_barVisualiserStyle;
    QString m_idleVisualiser = "calm";
    bool m_reduceMotion = false;
    QVariantMap m_deck, m_plain;
    LayoutPrefs *m_layout = nullptr;
};
