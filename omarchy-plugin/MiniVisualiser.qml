import QtQuick
import "Model.js" as Model

// A small, faint copy of Omamp's visualiser, drawn behind words. It fades
// out towards its left and right ends and towards the bottom, and hides
// when nothing plays. It draws only when a frame arrives (up to 20 a
// second from Omamp), so it costs nothing while idle. Visual only: the
// levels come from Service.qml.
Item {
  id: viz

  // 0..1 per band, low to high.
  property var levels: []
  // One of Model.miniStyle()'s: bars, skyline, led, synthwave, line,
  // ribbon, pulse, stars.
  property string style: "bars"
  // Something plays through Omamp; otherwise it fades away.
  property bool active: false
  // Calmer: eased levels, no drift, no fades.
  property bool reduceMotion: false
  // Its opacity while active (Model.vizOpacity).
  property real strength: 0.32
  property color low: "#9ece6a"
  property color high: "#7aa2f7"
  // The share of the width over which each end fades out.
  property real edgeFade: 0.18
  // Bar width in pixels; the gaps are 0.6 of it, at least one pixel.
  property real barWidth: 3

  // What is drawn: the levels eased (reduceMotion) and kept while fading out.
  property var shown: []
  // Moves the stars and the synthwave grid on, one step per frame.
  property int step: 0
  readonly property var stars: Model.starField(28, Math.round(width) + 7 * Math.round(height))

  opacity: active && levels.length > 0 ? strength : 0
  visible: opacity > 0.001
  Behavior on opacity {
    enabled: !viz.reduceMotion
    NumberAnimation { duration: 400; easing.type: Easing.InOutQuad }
  }

  onLevelsChanged: {
    if (levels.length === 0) return
    shown = Model.easeLevels(shown, levels, reduceMotion ? 0.45 : 1)
    if (!reduceMotion) step++
    if (visible) canvas.requestPaint()
  }
  onStyleChanged: canvas.requestPaint()
  onVisibleChanged: if (visible) canvas.requestPaint()

  Canvas {
    id: canvas
    anchors.fill: parent
    renderStrategy: Canvas.Cooperative

    function col(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    function verticalGradient(ctx, h) {
      var g = ctx.createLinearGradient(0, h, 0, 0)
      g.addColorStop(0, viz.low)
      g.addColorStop(1, viz.high)
      return g
    }

    function drawBars(ctx, w, h, mirror) {
      var gap = Math.max(1, Math.round(viz.barWidth * 0.6))
      var rects = Model.barRects(viz.shown, w, h, viz.barWidth, gap, mirror)
      ctx.fillStyle = verticalGradient(ctx, h)
      for (var i = 0; i < rects.length; i++) {
        var r = rects[i]
        ctx.fillRect(r.x, r.y, r.w, r.h)
      }
    }

    // Columns of lit cells: bars snapped to whole cells, then the gaps
    // between rows cut out, a few operations a frame rather than one per cell.
    function drawLed(ctx, w, h) {
      var cell = Math.max(2, Math.round(viz.barWidth - 1))
      var pitch = cell + 1
      var rows = Math.max(1, Math.floor((h + 1) / pitch))
      var rects = Model.barRects(viz.shown, w, h, cell, 1, false)
      ctx.fillStyle = verticalGradient(ctx, h)
      for (var i = 0; i < rects.length; i++) {
        var lit = Math.max(1, Math.round(rects[i].h / h * rows))
        ctx.fillRect(rects[i].x, h - lit * pitch + 1, cell, lit * pitch - 1)
      }
      for (var r = 1; r < rows; r++) ctx.clearRect(0, h - r * pitch, w, 1)
    }

    // The levels as points across the width, for the line and the ribbon.
    function points(w, n) {
      var v = Model.resample(viz.shown, n)
      var pts = []
      for (var i = 0; i < n; i++) pts.push({ x: n === 1 ? w / 2 : i * w / (n - 1), v: v[i] })
      return pts
    }

    function smoothPath(ctx, pts, yOf) {
      ctx.moveTo(pts[0].x, yOf(pts[0].v))
      for (var i = 1; i < pts.length; i++) {
        var mx = (pts[i - 1].x + pts[i].x) / 2
        ctx.bezierCurveTo(mx, yOf(pts[i - 1].v), mx, yOf(pts[i].v), pts[i].x, yOf(pts[i].v))
      }
    }

    function drawLine(ctx, w, h) {
      var pts = points(w, Math.max(4, Math.min(24, Math.round(w / 14))))
      var top = function(v) { return h - 1 - v * (h - 2) }
      ctx.beginPath()
      smoothPath(ctx, pts, top)
      ctx.lineTo(w, h)
      ctx.lineTo(0, h)
      ctx.closePath()
      ctx.fillStyle = col(viz.low, 0.6)
      ctx.fill()
      ctx.beginPath()
      smoothPath(ctx, pts, top)
      ctx.lineWidth = 1.5
      ctx.strokeStyle = viz.high
      ctx.stroke()
    }

    function drawRibbon(ctx, w, h) {
      var pts = points(w, Math.max(4, Math.min(24, Math.round(w / 14))))
      var mid = h / 2
      var up = function(v) { return mid - v * (h / 2 - 1) - 0.5 }
      var down = function(v) { return mid + v * (h / 2 - 1) + 0.5 }
      ctx.beginPath()
      smoothPath(ctx, pts, up)
      for (var i = pts.length - 1; i >= 0; i--) ctx.lineTo(pts[i].x, down(pts[i].v))
      ctx.closePath()
      var g = ctx.createLinearGradient(0, 0, 0, h)
      g.addColorStop(0, col(viz.high, 0.9))
      g.addColorStop(0.5, col(viz.low, 0.9))
      g.addColorStop(1, col(viz.high, 0.9))
      ctx.fillStyle = g
      ctx.fill()
    }

    function drawPulse(ctx, w, h) {
      var b = Model.bass(viz.shown)
      var rx = w * (0.18 + 0.32 * b)
      var g = ctx.createRadialGradient(w / 2, h / 2, 0, w / 2, h / 2, rx)
      g.addColorStop(0, col(viz.high, 0.55 + 0.45 * b))
      g.addColorStop(0.45, col(viz.low, 0.35 + 0.3 * b))
      g.addColorStop(1, col(viz.low, 0))
      ctx.fillStyle = g
      ctx.fillRect(0, 0, w, h)
    }

    function drawSynthwave(ctx, w, h) {
      var horizon = Math.round(h * 0.62)
      var b = Model.bass(viz.shown)
      // The sun: a half disc on the horizon with stripes cut out of it.
      var r = Math.min(w * 0.12, horizon * (0.82 + 0.15 * b))
      var sun = ctx.createLinearGradient(0, horizon - r, 0, horizon)
      sun.addColorStop(0, viz.high)
      sun.addColorStop(1, viz.low)
      ctx.fillStyle = sun
      ctx.beginPath()
      ctx.arc(w / 2, horizon, r, Math.PI, 2 * Math.PI)
      ctx.closePath()
      ctx.fill()
      ctx.save()
      ctx.globalCompositeOperation = "destination-out"
      for (var s = 1; s <= 3; s++) {
        var y = horizon - r * s * 0.24
        ctx.fillRect(w / 2 - r, y, 2 * r, Math.max(1, s * 0.6))
      }
      ctx.restore()
      // The city: spectrum bars either side of the sun, standing on the horizon.
      var gap = 1
      var side = Math.max(0, w / 2 - r - 2)
      var rects = Model.barRects(viz.shown, side, horizon, 2, gap, false)
      ctx.fillStyle = col(viz.low, 0.9)
      for (var i = 0; i < rects.length; i++) {
        var q = rects[i]
        var bh = q.h * 0.85
        ctx.fillRect(side - q.x - q.w, horizon - bh, q.w, bh)  // bass nearest the sun
        ctx.fillRect(w - side + q.x, horizon - bh, q.w, bh)
      }
      // The grid rolling forward.
      ctx.strokeStyle = col(viz.high, 0.8)
      ctx.lineWidth = 1
      ctx.beginPath()
      ctx.moveTo(0, horizon + 0.5)
      ctx.lineTo(w, horizon + 0.5)
      var depth = h - horizon
      var roll = (viz.step % 8) / 8
      for (var k = 0; k < 3; k++) {
        var t = (k + roll) / 3
        var gy = horizon + depth * t * t
        ctx.moveTo(0, gy + 0.5)
        ctx.lineTo(w, gy + 0.5)
      }
      for (var v = -6; v <= 6; v++) {
        ctx.moveTo(w / 2 + v * w * 0.02, horizon)
        ctx.lineTo(w / 2 + v * w * 0.12, h)
      }
      ctx.stroke()
    }

    function drawStars(ctx, w, h) {
      var list = viz.stars
      var n = viz.shown.length
      for (var i = 0; i < list.length; i++) {
        var s = list[i]
        var level = n > 0 ? viz.shown[Math.min(n - 1, Math.floor(s.band * n))] : 0
        var x = ((s.x * w + viz.step * s.speed * (0.6 + Model.bass(viz.shown) * 2)) % w + w) % w
        var size = s.size * (1 + level * 1.6)
        ctx.fillStyle = col(level > 0.5 ? viz.high : viz.low, 0.35 + 0.65 * level)
        ctx.fillRect(x, s.y * (h - size), size, size)
      }
    }

    onPaint: {
      var ctx = getContext("2d")
      var w = width, h = height
      ctx.reset()
      if (w <= 0 || h <= 0 || viz.shown.length === 0) return
      var st = viz.style
      if (st === "skyline") drawBars(ctx, w, h, true)
      else if (st === "led") drawLed(ctx, w, h)
      else if (st === "line") drawLine(ctx, w, h)
      else if (st === "ribbon") drawRibbon(ctx, w, h)
      else if (st === "pulse") drawPulse(ctx, w, h)
      else if (st === "synthwave") drawSynthwave(ctx, w, h)
      else if (st === "stars") drawStars(ctx, w, h)
      else drawBars(ctx, w, h, false)

      // Fade out to both ends and towards the bottom: two masks, multiplied.
      ctx.globalCompositeOperation = "destination-in"
      var e = Math.max(0.01, Math.min(0.45, viz.edgeFade))
      var across = ctx.createLinearGradient(0, 0, w, 0)
      across.addColorStop(0, "transparent")
      across.addColorStop(e, "black")
      across.addColorStop(1 - e, "black")
      across.addColorStop(1, "transparent")
      ctx.fillStyle = across
      ctx.fillRect(0, 0, w, h)
      var down = ctx.createLinearGradient(0, 0, 0, h)
      down.addColorStop(0, Qt.rgba(0, 0, 0, 0.9))
      down.addColorStop(0.45, Qt.rgba(0, 0, 0, 1))
      down.addColorStop(1, Qt.rgba(0, 0, 0, 0.3))
      ctx.fillStyle = down
      ctx.fillRect(0, 0, w, h)
      ctx.globalCompositeOperation = "source-over"
    }
  }
}
