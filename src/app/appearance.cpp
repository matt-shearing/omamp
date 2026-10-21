#include "appearance.h"

#include "layoutprefs.h"
#include "theme.h"

#include <QColor>
#include <QSettings>

namespace {

QString mix(const QString &a, const QString &b, double t) {
    const QColor x(a), y(b);
    return QColor::fromRgbF(x.redF() + (y.redF() - x.redF()) * t, x.greenF() + (y.greenF() - x.greenF()) * t,
                            x.blueF() + (y.blueF() - x.blueF()) * t)
        .name();
}

QString alpha(const QString &colour, double a) {
    QColor c(colour);
    c.setAlphaF(a);
    return c.name(QColor::HexArgb);
}

} // namespace

Appearance::Appearance(const QString &settingsPath, Theme *theme, QObject *parent)
    : QObject(parent), m_path(settingsPath), m_theme(theme), m_layout(new LayoutPrefs(settingsPath, this)) {
    if (!m_path.isEmpty()) {
        QSettings s(m_path, QSettings::IniFormat);
        s.beginGroup("appearance");
        const QString style = s.value("deckStyle", m_deckStyle).toString();
        if (deckStyles().contains(style))
            m_deckStyle = style;
        const QString vis = s.value("visualiser", m_visualiser).toString();
        if (visualisers().contains(vis))
            m_visualiser = vis;
        m_coverInDeck = s.value("coverInDeck", m_coverInDeck).toBool();
        const QString bg = s.value("bigBackground", m_bigBackground).toString();
        if (bg == "blur" || bg == "theme")
            m_bigBackground = bg;
        m_bigLyrics = s.value("bigLyrics", m_bigLyrics).toBool();
        m_bigVisualiserFade = s.value("bigVisualiserFade", m_bigVisualiserFade).toBool();
        const QString place = s.value("barVisualiser", m_barVisualiser).toString();
        if (barPlacements().contains(place))
            m_barVisualiser = place;
        const QString barStyle = s.value("barVisualiserStyle").toString();
        if (visualisers().contains(barStyle))
            m_barVisualiserStyle = barStyle;
        const QString idle = s.value("idleVisualiser", m_idleVisualiser).toString();
        if (idle == "calm" || idle == "hide")
            m_idleVisualiser = idle;
        m_reduceMotion = s.value("reduceMotion", m_reduceMotion).toBool();
        const QString sensitivity = s.value("visualiserSensitivity", m_sensitivity).toString();
        if (sensitivities().contains(sensitivity))
            m_sensitivity = sensitivity;
    }
    if (m_theme)
        connect(m_theme, &Theme::changed, this, &Appearance::rebuild);
    rebuild();
}

QStringList Appearance::deckStyles() { return {"omarchy", "classic", "silver"}; }
QStringList Appearance::visualisers() {
    return {"bars", "peaks", "mirror", "scope", "fire", "skyline", "ribbon", "line", "pulse", "led", "synthwave", "stars"};
}

QString Appearance::deckStyleName(const QString &style) const {
    if (style == "classic")
        return tr("Classic");
    if (style == "silver")
        return tr("Silver");
    return tr("Omarchy");
}

QString Appearance::visualiserName(const QString &style) const {
    if (style == "bars")
        return tr("Bars");
    if (style == "mirror")
        return tr("Mirrored spectrum");
    if (style == "scope")
        return tr("Oscilloscope");
    if (style == "fire")
        return tr("Fire");
    if (style == "skyline")
        return tr("Skyline");
    if (style == "ribbon")
        return tr("Waveform ribbon");
    if (style == "line")
        return tr("Spectrum line");
    if (style == "pulse")
        return tr("Pulse");
    if (style == "led")
        return tr("LED matrix");
    if (style == "synthwave")
        return tr("Synthwave");
    if (style == "stars")
        return tr("Star field");
    return tr("Bars with peaks");
}

QString Appearance::barPlacementName(const QString &place) const {
    if (place == "behind")
        return tr("Behind the seek bar");
    if (place == "full")
        return tr("Behind the whole bar");
    if (place == "edge")
        return tr("Along the edge");
    if (place == "beside")
        return tr("Beside the cover");
    return tr("Off");
}

void Appearance::save(const QString &key, const QVariant &value) {
    if (m_path.isEmpty())
        return;
    QSettings s(m_path, QSettings::IniFormat);
    s.setValue("appearance/" + key, value);
}

void Appearance::setDeckStyle(const QString &style) {
    if (style == m_deckStyle || !deckStyles().contains(style))
        return;
    m_deckStyle = style;
    save("deckStyle", style);
    rebuild();
    emit changed();
}

void Appearance::setVisualiser(const QString &style) {
    if (style == m_visualiser || !visualisers().contains(style))
        return;
    m_visualiser = style;
    save("visualiser", style);
    emit changed();
}

namespace {
// The style `delta` places from `from`, round the list either way.
QString stepped(const QString &from, int delta) {
    const QStringList all = Appearance::visualisers();
    const int n = int(all.size());
    return all.value(((all.indexOf(from) + delta) % n + n) % n);
}
} // namespace

QString Appearance::stepVisualiser(int delta) {
    setVisualiser(stepped(m_visualiser, delta));
    return m_visualiser;
}

QString Appearance::stepBarVisualiser(int delta) {
    if (m_barVisualiserStyle.isEmpty())
        return stepVisualiser(delta);
    setBarVisualiserStyle(stepped(m_barVisualiserStyle, delta));
    return m_barVisualiserStyle;
}

void Appearance::setPickerAnchor(QObject *anchor) {
    if (anchor == m_pickerAnchor)
        return;
    m_pickerAnchor = anchor;
    emit pickerAnchorChanged();
}

QString Appearance::cycleVisualiser(bool inBar) {
    return inBar && m_barVisualiser != "off" ? nextBarVisualiser() : nextVisualiser();
}

void Appearance::setBarVisualiser(const QString &place) {
    if (place == m_barVisualiser || !barPlacements().contains(place))
        return;
    m_barVisualiser = place;
    save("barVisualiser", place);
    emit changed();
}

// "" shares the player's style.
void Appearance::setBarVisualiserStyle(const QString &style) {
    if (style == m_barVisualiserStyle || (!style.isEmpty() && !visualisers().contains(style)))
        return;
    m_barVisualiserStyle = style;
    save("barVisualiserStyle", style);
    emit changed();
}

void Appearance::setIdleVisualiser(const QString &kind) {
    if (kind == m_idleVisualiser || (kind != "calm" && kind != "hide"))
        return;
    m_idleVisualiser = kind;
    save("idleVisualiser", kind);
    emit changed();
}

void Appearance::setReduceMotion(bool on) {
    if (on == m_reduceMotion)
        return;
    m_reduceMotion = on;
    save("reduceMotion", on);
    emit changed();
}

void Appearance::setVisualiserSensitivity(const QString &sensitivity) {
    if (sensitivity == m_sensitivity || !sensitivities().contains(sensitivity))
        return;
    m_sensitivity = sensitivity;
    save("visualiserSensitivity", sensitivity);
    emit changed();
}

QString Appearance::sensitivityName(const QString &sensitivity) const {
    if (sensitivity == "low")
        return tr("Low");
    if (sensitivity == "high")
        return tr("High");
    return tr("Normal");
}

void Appearance::setCoverInDeck(bool on) {
    if (on == m_coverInDeck)
        return;
    m_coverInDeck = on;
    save("coverInDeck", on);
    emit changed();
}

void Appearance::setBigBackground(const QString &kind) {
    if (kind == m_bigBackground || (kind != "blur" && kind != "theme"))
        return;
    m_bigBackground = kind;
    save("bigBackground", kind);
    emit changed();
}

void Appearance::setBigLyrics(bool on) {
    if (on == m_bigLyrics)
        return;
    m_bigLyrics = on;
    save("bigLyrics", on);
    emit changed();
}

void Appearance::setBigVisualiserFade(bool on) {
    if (on == m_bigVisualiserFade)
        return;
    m_bigVisualiserFade = on;
    save("bigVisualiserFade", on);
    emit changed();
}

void Appearance::rebuild() {
    const QVariantMap colours = m_theme ? m_theme->colors() : Theme::defaults();
    const int radius = m_theme ? m_theme->radius() : 0;
    const bool light = m_theme ? m_theme->light() : false;
    m_plain = palette("omarchy", colours, radius, light);
    m_deck = palette(m_deckStyle, colours, radius, light);
    emit paletteChanged();
}

// Every colour the deck chrome draws with, by role. Components read these
// names and nothing else, so a style is just another map.
QVariantMap Appearance::palette(const QString &style, const QVariantMap &c, int themeRadius, bool themeLight) {
    auto t = [&](const char *name) { return c.value(name).toString(); };
    QVariantMap p;
    if (style == "classic") {
        // Winamp 2.x: charcoal frame, grey bevelled panels and buttons, a
        // black LCD with the green readout, gold stripes in the title bars,
        // and the analyser running green to yellow to red.
        p = {
            {"window", "#1e1e26"},       {"panel", "#3a3a48"},        {"panelTop", "#4a4a5a"},
            {"panelBottom", "#2e2e3a"},  {"panelBorder", "#101016"},  {"bevelLight", "#8a8a9c"},
            {"bevelDark", "#0a0a10"},    {"label", "#e8e8f0"},         {"labelDim", "#9090a0"},
            {"stripe", "#c8aa52"},       {"lcd", "#000000"},           {"lcdBorder", "#5a5a6a"},
            {"lcdText", "#00e000"},      {"lcdDim", "#1f6a1f"},        {"lcdAccent", "#f0d040"},
            {"visLow", "#18c018"},       {"visMid", "#e0d020"},        {"visHigh", "#e83818"},
            {"visPeak", "#c8c8d0"},      {"field", "#000000"},         {"fieldBorder", "#5a5a6a"},
            {"text", "#00e000"},         {"textCurrent", "#ffffff"},   {"muted", "#6c8c6c"},
            {"selection", "#0a3a8a"},    {"hover", "#102a50"},         {"buttonTop", "#d4d4dc"},
            {"buttonBottom", "#9a9aa8"}, {"buttonBorder", "#0a0a10"},  {"buttonGlyph", "#16161e"},
            {"buttonGlyphHover", "#000000"}, {"buttonPressed", "#7a7a88"}, {"buttonLit", "#007000"},
            {"lamp", "#16161e"},         {"lampOn", "#00e000"},        {"lampText", "#9090a0"},
            {"lampTextOn", "#e8e8f0"},   {"lampBorder", "#5a5a6a"},    {"lampHover", "#2a2a36"},
            {"sliderTrack", "#101016"},  {"sliderBorder", "#5a5a6a"},  {"sliderFill", "#c8aa52"},
            {"volumeFill", "#18c018"},   {"knob", "#c0c0cc"},          {"knobHover", "#ffffff"},
            {"accent", "#c8aa52"},       {"error", "#e83818"},         {"placeholder", "#4a4a5a"},
            {"radius", 0},               {"smallRadius", 0},           {"light", false},
            {"bevel", true},             {"gradient", true},
        };
    } else if (style == "silver") {
        // The 2000s MediaMonkey look: brushed silver panels with a blue tint,
        // glossy buttons, a navy readout with pale cyan digits, blue accents.
        p = {
            {"window", "#c9d1de"},       {"panel", "#e4e9f1"},         {"panelTop", "#f7f9fc"},
            {"panelBottom", "#cdd5e2"},  {"panelBorder", "#7d8ca6"},   {"bevelLight", "#ffffff"},
            {"bevelDark", "#9aa6bb"},    {"label", "#1d2a44"},          {"labelDim", "#5c6a84"},
            {"stripe", "#3a78d8"},       {"lcd", "#0d1f3c"},            {"lcdBorder", "#4a5f86"},
            {"lcdText", "#9fe6ff"},      {"lcdDim", "#3f6890"},         {"lcdAccent", "#ffd36a"},
            {"visLow", "#2f7fe0"},       {"visMid", "#38c0f0"},         {"visHigh", "#b8f0ff"},
            {"visPeak", "#ffffff"},      {"field", "#ffffff"},          {"fieldBorder", "#8c9ab3"},
            {"text", "#1d2a44"},         {"textCurrent", "#1d5fc8"},    {"muted", "#6a7690"},
            {"selection", "#bcd4f6"},    {"hover", "#e2ecfa"},          {"buttonTop", "#ffffff"},
            {"buttonBottom", "#c3cddd"}, {"buttonBorder", "#7d8ca6"},   {"buttonGlyph", "#24467a"},
            {"buttonGlyphHover", "#1d5fc8"}, {"buttonPressed", "#aebbd0"}, {"buttonLit", "#1d5fc8"},
            {"lamp", "#f4f6fa"},         {"lampOn", "#2f7fe0"},         {"lampText", "#5c6a84"},
            {"lampTextOn", "#1d2a44"},   {"lampBorder", "#8c9ab3"},     {"lampHover", "#e2ecfa"},
            {"sliderTrack", "#f4f6fa"},  {"sliderBorder", "#8c9ab3"},   {"sliderFill", "#3a78d8"},
            {"volumeFill", "#38a0e8"},   {"knob", "#ffffff"},           {"knobHover", "#e2ecfa"},
            {"accent", "#1d5fc8"},       {"error", "#c8283c"},          {"placeholder", "#b4bfd0"},
            {"radius", 4},               {"smallRadius", 3},            {"light", true},
            {"bevel", true},             {"gradient", true},
        };
    } else {
        // Omarchy: the theme's own colours in the roles the deck has always
        // used them for, so this is the look Omamp had before styles.
        const QString accent = t("accent");
        p = {
            {"window", t("background")},           {"panel", t("dark_background")},
            {"panelTop", t("dark_background")},    {"panelBottom", t("dark_background")},
            {"panelBorder", t("lighter_background")}, {"bevelLight", "transparent"},
            {"bevelDark", "transparent"},          {"label", t("foreground")},
            {"labelDim", t("muted")},              {"stripe", alpha(accent, 0.55)},
            {"lcd", t("darker_background")},       {"lcdBorder", t("lighter_background")},
            {"lcdText", t("green")},               {"lcdDim", t("muted")},
            {"lcdAccent", accent},                 {"visLow", t("green")},
            {"visMid", t("cyan")},                 {"visHigh", accent},
            {"visPeak", alpha(t("bright_foreground"), 0.8)}, {"field", t("darker_background")},
            {"fieldBorder", t("lighter_background")}, {"text", t("foreground")},
            {"textCurrent", accent},               {"muted", t("muted")},
            {"selection", t("selection")},         {"hover", alpha(t("selection"), 0.45)},
            {"buttonTop", t("lighter_background")}, {"buttonBottom", t("lighter_background")},
            {"buttonBorder", t("darker_background")}, {"buttonGlyph", t("foreground")},
            {"buttonGlyphHover", t("bright_foreground")}, {"buttonPressed", t("darker_background")},
            {"buttonLit", accent},                 {"buttonHover", t("selection")},
            {"lamp", t("darker_background")},      {"lampOn", accent},
            {"lampText", t("muted")},              {"lampTextOn", t("bright_foreground")},
            {"lampBorder", t("lighter_background")}, {"lampHover", t("selection")},
            {"sliderTrack", t("darker_background")}, {"sliderBorder", t("lighter_background")},
            {"sliderFill", accent},                {"volumeFill", t("cyan")},
            {"knob", t("light_foreground")},       {"knobHover", t("bright_foreground")},
            {"accent", accent},                    {"error", t("red")},
            {"placeholder", t("lighter_background")}, {"radius", themeRadius},
            {"smallRadius", qMin(4, themeRadius)}, {"light", themeLight},
            {"bevel", false},                      {"gradient", false},
        };
        // The faint highlight along the top of a button, as before.
        p["buttonShine"] = alpha(t("light_foreground"), 0.25);
        // The big view's text over the picture.
        p["bigText"] = t("bright_foreground");
        p["bigSubtext"] = t("light_foreground");
        p["bigShade"] = t("background");
    }
    if (!p.contains("buttonHover"))
        p["buttonHover"] = mix(p.value("buttonTop").toString(), p.value("buttonBottom").toString(), 0.2);
    if (!p.contains("buttonShine"))
        p["buttonShine"] = alpha(p.value("bevelLight").toString(), 0.6);
    if (!p.contains("bigText")) {
        // The big view keeps a dark stage in every style, so the cover and
        // the words stand out; it borrows the style's readout colours.
        p["bigText"] = "#ffffff";
        p["bigSubtext"] = style == "classic" ? "#00e000" : "#9fe6ff";
        p["bigShade"] = style == "classic" ? "#000000" : "#0d1f3c";
    }
    p["style"] = style;
    return p;
}
