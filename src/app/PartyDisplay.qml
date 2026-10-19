import QtQuick

// Party mode's big now-playing display: the big Now Playing view, with the
// controls party mode locks taken away and a way to leave. It covers the
// window; the keys that party mode allows still work.
BigView {
    objectName: "partyDisplay"
    party: true
}
