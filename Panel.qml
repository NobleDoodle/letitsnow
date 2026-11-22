import QtQuick
import Quickshell
import Quickshell.Io
import qs.Ui
import qs.Commons
import qs.Commons as Commons

// Let It Snow: the bar button and panel for letitsnow, the snow service.
// Every change goes through the `letitsnow` command and every reading comes
// back from `letitsnow status --json`, so this panel and a terminal always
// agree. The snow keeps falling with or without this plugin.
//
// The service ships in this same checkout, as Omarchroma ships hyprchroma:
// Install and Update run bin/letitsnow-setup in Omarchy's presented terminal,
// which builds the package from this folder and installs it with pacman. The
// plugin's own version (manifest.json) is the version it expects.
Panel {
  id: root
  moduleName: "io.github.nobledoodle.letitsnow"
  ipcTarget: "io.github.nobledoodle.letitsnow"

  // ------------------------------------------------------------ state

  property string cli: ""          // path of the letitsnow command, "" while missing
  property bool probed: false
  readonly property bool installed: cli !== ""
  property bool running: false
  property bool hasUnit: false     // the user service is installed (autostart possible)
  property bool autostart: false
  property real speed: 1
  property real density: 1
  property real snowOpacity: 1     // `opacity` belongs to Item
  property string snowLayer: "desktop"
  property string serviceVersion: ""

  // What this checkout would install, read from the manifest beside this file,
  // so "omarchy plugin update" moving the checkout ahead lights the alert.
  readonly property string pluginDir: decodeURIComponent(Qt.resolvedUrl(".").toString().replace(/^file:\/\//, "").replace(/\/$/, ""))
  property string expectedVersion: ""
  readonly property bool outdated: installed && serviceVersion !== "" && expectedVersion !== ""
    && olderThan(serviceVersion, expectedVersion)
  readonly property bool installPending: probed && !installed
  readonly property bool serviceAlert: outdated || installPending
  readonly property bool setupRunning: setupProc.running || uninstallProc.running

  // Actions run one at a time, in order; status reads wait until they finish
  // so an optimistic value is never overwritten by a read taken before it landed.
  property var queue: []
  property var pendingSet: ({})
  readonly property bool busy: actionProc.running || queue.length > 0 || flushTimer.running

  readonly property string configPath:
    (Quickshell.env("XDG_CONFIG_HOME") || (Quickshell.env("HOME") + "/.config")) + "/letitsnow/config"

  readonly property var layerOptions: [
    { value: "desktop", label: "Desktop" },
    { value: "dynamic", label: "Dynamic" },
    { value: "top", label: "Above all" }
  ]

  readonly property string statusLine: !probed ? ""
    : !installed ? "SERVICE NOT INSTALLED"
    : running ? "SNOWING · " + (snowLayer === "top" ? "ABOVE ALL" : snowLayer === "dynamic" ? "DYNAMIC" : "ON THE DESKTOP")
    : "STOPPED"

  // Keyboard cursor, shared with the mouse.
  readonly property var sections: !installed ? []
    : hasUnit ? ["power", "speed", "density", "brightness", "layer", "login"]
    : ["power", "speed", "density", "brightness", "layer"]
  property string focusSection: "power"
  property int selectedIndex: 0
  property bool cursorActive: false
  property real wheelAccumulator: 0

  readonly property var sliders: ({
    speed: { key: "speed", min: 0.1, max: 3, step: 0.05 },
    density: { key: "density", min: 0.1, max: 4, step: 0.05 },
    brightness: { key: "opacity", min: 0.1, max: 1, step: 0.05 }
  })

  function fmt(v) { return String(Number(Number(v).toFixed(2))) }
  // Field by field: "0.10.0" is newer than "0.9.0", which a string compare gets backwards.
  function olderThan(have, want) {
    var a = String(have).split("."), b = String(want).split(".")
    for (var i = 0; i < Math.max(a.length, b.length); i++) {
      var x = parseInt(a[i] || "0", 10), y = parseInt(b[i] || "0", 10)
      if (isNaN(x)) x = 0
      if (isNaN(y)) y = 0
      if (x !== y) return x < y
    }
    return false
  }
  // PanelSlider leaves snapping to its caller.
  function snap(spec, v) {
    var x = Math.max(spec.min, Math.min(spec.max, Math.round(v / spec.step) * spec.step))
    return Number(x.toFixed(2))
  }
  function times(v) { return Number(v).toFixed(2).replace(/0$/, "") + "×" }

  // ------------------------------------------------------------ actions

  // Install, update and remove all happen in Omarchy's presented terminal: they
  // ask questions and pacman asks for a password, and neither has anywhere to
  // happen inside omarchy-shell. The panel closes first so the terminal gets
  // the keyboard; when the terminal closes, the service is looked for again.
  function runSetup() {
    if (setupRunning) return
    root.close()
    setupProc.running = true
  }

  function runUninstall() {
    if (setupRunning || !installed) return
    root.close()
    uninstallProc.running = true
  }

  function reprobe() {
    probed = false
    cli = ""
    serviceVersion = ""
    refresh()
  }

  function refresh() {
    if (!probed) { if (!findProc.running) findProc.running = true; return }
    if (installed && !statusProc.running && !busy) statusProc.running = true
  }

  function enqueue(args) {
    queue = queue.concat([args])
    pump()
  }

  function pump() {
    if (actionProc.running || queue.length === 0 || !installed) return
    var next = queue[0]
    queue = queue.slice(1)
    actionProc.command = [root.cli].concat(next)
    actionProc.running = true
  }

  // Sliders send while they move, so the snow follows the knob; settings
  // gathered inside one short beat go out as a single `letitsnow set`.
  function setValue(key, value) {
    var p = Object.assign({}, pendingSet)
    p[key] = value
    pendingSet = p
    if (key === "speed") speed = value
    else if (key === "density") density = value
    else if (key === "opacity") snowOpacity = value
    else if (key === "layer") snowLayer = value
    flushTimer.restart()
  }

  function flush() {
    var keys = Object.keys(pendingSet)
    if (keys.length === 0) return
    var args = ["set"]
    for (var i = 0; i < keys.length; i++) {
      var v = pendingSet[keys[i]]
      args.push(keys[i] + "=" + (typeof v === "number" ? fmt(v) : v))
    }
    pendingSet = ({})
    enqueue(args)
  }

  function toggleSnow() {
    if (!installed) return
    running = !running
    enqueue([running ? "start" : "stop"])
  }

  function setAutostart(on) {
    if (!hasUnit) return
    autostart = on
    if (on) running = true   // enable starts it too
    enqueue([on ? "enable" : "disable"])
  }

  function nudge(section, steps) {
    var s = sliders[section]
    if (!s) return
    var cur = s.key === "speed" ? speed : s.key === "density" ? density : snowOpacity
    setValue(s.key, snap(s, cur + steps * s.step))
  }

  function moveCursor(delta) {
    var idx = sections.indexOf(focusSection)
    var next = idx < 0 ? 0 : idx + (delta > 0 ? 1 : -1)
    if (next < 0 || next > sections.length - 1) return
    focusSection = sections[next]
    selectedIndex = focusSection === "layer" ? Math.max(0, layerIndex()) : 0
  }

  function layerIndex() {
    for (var i = 0; i < layerOptions.length; i++) if (layerOptions[i].value === snowLayer) return i
    return 0
  }

  function moveCursorH(delta) {
    if (sliders[focusSection]) nudge(focusSection, delta)
    else if (focusSection === "layer")
      selectedIndex = Math.max(0, Math.min(layerOptions.length - 1, selectedIndex + delta))
  }

  function activateCursor() {
    if (focusSection === "power") toggleSnow()
    else if (focusSection === "login") setAutostart(!autostart)
    else if (focusSection === "layer") setValue("layer", layerOptions[selectedIndex].value)
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  Component.onCompleted: refresh()

  onOpenedChanged: {
    if (opened) {
      if (!installed) probed = false   // installed since? look again
      refresh()
      focusSection = sections.length ? sections[0] : "power"
      selectedIndex = 0
      cursorActive = false
    }
  }

  Timer {
    interval: root.opened ? 2000 : 15000
    running: true
    repeat: true
    onTriggered: root.refresh()
  }

  Timer {
    id: flushTimer
    interval: 120
    onTriggered: root.flush()
  }

  // A change made in a terminal shows here at once.
  FileView {
    path: root.configPath
    watchChanges: true
    printErrors: false
    onFileChanged: root.refresh()
  }

  FileView {
    path: root.pluginDir + "/manifest.json"
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: {
      try { root.expectedVersion = String(JSON.parse(text()).version || "") } catch (e) { root.expectedVersion = "" }
    }
  }

  // pacman replacing the command (an update run from a terminal) clears the alert.
  FileView {
    path: "/usr/bin/letitsnow"
    watchChanges: true
    preload: false
    printErrors: false
    onFileChanged: root.reprobe()
  }

  // The package's command, or else a `make install-user` copy -- looked for at
  // those two places, not along PATH.
  Process {
    id: findProc
    command: ["/usr/bin/sh", "-c", "for c in /usr/bin/letitsnow \"$HOME/.local/bin/letitsnow\"; do [ -x \"$c\" ] && { echo \"$c\"; exit 0; }; done; exit 0"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        root.cli = String(text || "").trim().split("\n")[0] || ""
        root.probed = true
        root.refresh()
      }
    }
  }

  Process {
    id: statusProc
    command: [root.cli, "status", "--json"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        var s
        try { s = JSON.parse(String(text || "{}")) } catch (e) { return }
        if (root.busy) return
        root.running = !!s.running
        root.hasUnit = !!s.unit
        root.autostart = !!s.autostart
        root.speed = Number(s.speed) || 1
        root.density = Number(s.density) || 1
        root.snowOpacity = Number(s.opacity) || 1
        root.snowLayer = s.layer === "top" || s.layer === "dynamic" ? s.layer : "desktop"
        root.serviceVersion = String(s.version || "")
      }
    }
  }

  Process {
    id: actionProc
    stdout: StdioCollector { waitForEnd: true }
    stderr: StdioCollector { waitForEnd: true }
    onRunningChanged: {
      if (running) return
      if (root.queue.length > 0) root.pump()
      else settleRefresh.restart()
    }
  }

  Process {
    id: setupProc
    command: ["omarchy", "launch", "floating", "terminal", "with", "presentation", root.pluginDir + "/bin/letitsnow-setup"]
    onExited: root.reprobe()
  }

  Process {
    id: uninstallProc
    command: ["omarchy", "launch", "floating", "terminal", "with", "presentation", root.pluginDir + "/bin/letitsnow-uninstall"]
    onExited: root.reprobe()
  }

  Timer {
    id: settleRefresh
    interval: 300
    onTriggered: root.refresh()
  }

  // ------------------------------------------------------------ bar button

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰜗"
    active: root.running
    tooltipText: root.installPending ? "Let It Snow · install the snow service (open, then i)"
      : root.outdated ? "Let It Snow · service update ready (open, then i)"
      : root.running ? "Let It Snow · snowing (right click stops it)"
      : "Let It Snow · stopped (right click starts it)"
    onPressed: function (b) {
      if (b === Qt.RightButton) root.toggleSnow()
      else root.toggle()
    }
    // The wheel thickens or thins the snow.
    onWheelMoved: function (delta) {
      var wheel = Util.wheelSteps(root.wheelAccumulator, delta)
      root.wheelAccumulator = wheel.remainder
      if (wheel.steps !== 0 && root.installed) root.nudge("density", wheel.steps > 0 ? 5 : -5)
    }
  }

  // An install or update waiting: a dot in the theme's alert color, raised to
  // the icon's top right, cleared by the panel's Install / Update.
  Rectangle {
    visible: root.serviceAlert
    width: Math.max(5, Math.round(Style.bar.iconFont * 0.32))
    height: width
    radius: width / 2
    color: root.bar ? root.bar.urgent : Commons.Color.urgent
    x: Math.round(button.x + (button.width + Style.bar.iconCanvas) / 2 - width * 0.6)
    y: Math.round(button.y + (button.height - Style.bar.iconCanvas) / 2 - height * 0.2)
    z: 2
  }

  // ------------------------------------------------------------ panel

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(380))
    contentHeight: panel.fittedContentHeight(panelColumn.implicitHeight, Style.space(600))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onMoveRequested: function (dx, dy) {
        if (!root.cursorActive) {
          root.cursorActive = true
          return
        }
        if (dy !== 0) root.moveCursor(dy)
        else if (dx !== 0) root.moveCursorH(dx)
      }
      onActivateRequested: if (root.cursorActive) root.activateCursor()
      onCloseRequested: root.close()
      onTextKey: function (text, modifiers) {
        if (text === "i" && root.serviceAlert) root.runSetup()
      }
      onTabRequested: function (direction) { root.switchPanel(direction) }

      Column {
        id: panelColumn
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Style.space(14)

        PanelHero {
          id: hero
          width: parent.width
          title: "Let It Snow"
          meta: root.statusLine
          foreground: root.bar.foreground
          fontFamily: root.bar.fontFamily
          iconOpacity: root.running ? 1.0 : 0.5
          iconComponent: Component {
            Text {
              text: "󰜗"
              color: root.bar.foreground
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.display
            }
          }
          trailingControl: Component {
            ToggleSwitch {
              visible: root.installed
              checked: root.running
              hasCursor: root.cursorActive && root.focusSection === "power"
              foreground: hero.foreground
              onToggled: {
                root.focusSection = "power"
                root.toggleSnow()
              }
              onHovered: function (on) {
                if (!on) return
                root.cursorActive = true
                root.focusSection = "power"
              }
            }
          }
        }

        // ---------- Install / update the service ----------
        PanelSeparator {
          visible: root.serviceAlert
          foreground: root.bar.foreground
        }

        Text {
          visible: root.serviceAlert
          width: parent.width
          wrapMode: Text.WordWrap
          textFormat: Text.PlainText
          text: root.installPending
            ? "The snow is drawn by letitsnow, a small user service that comes in this plugin's own folder. Install builds it there and installs it with pacman, in a terminal that asks before each step."
            : "letitsnow " + root.serviceVersion + " is installed; this plugin comes with " + root.expectedVersion + ". Update builds the new one from this plugin's folder."
          color: Qt.darker(root.bar.foreground, 1.2)
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.caption
        }

        Button {
          visible: root.serviceAlert
          width: parent.width
          enabled: !root.setupRunning
          text: root.setupRunning ? "Working..." : root.installPending ? "Install  (i)" : "Update  (i)"
          // Lit in the alert color the bar dot takes, since this is what clears it.
          readonly property color alertColor: root.bar ? root.bar.urgent : Commons.Color.urgent
          foreground: root.bar.foreground
          fontFamily: root.bar.fontFamily
          background: Util.alpha(alertColor, 0.30)
          Rectangle {
            anchors.fill: parent
            color: "transparent"
            radius: parent.radius
            border.width: 1
            border.color: parent.alertColor
          }
          onClicked: root.runSetup()
        }

        // ---------- Sliders ----------
        Repeater {
          model: root.installed ? [
            { section: "speed", title: "SPEED", hint: "How fast it falls" },
            { section: "density", title: "DENSITY", hint: "How many flakes" },
            { section: "brightness", title: "BRIGHTNESS", hint: "How bright they are" }
          ] : []

          delegate: Column {
            id: sliderBlock
            required property var modelData
            readonly property var spec: root.sliders[modelData.section]
            readonly property real current: spec.key === "speed" ? root.speed
              : spec.key === "density" ? root.density : root.snowOpacity
            width: panelColumn.width
            spacing: Style.space(6)

            PanelSeparator {
              width: parent.width
              foreground: root.bar.foreground
            }

            Item {
              width: parent.width
              implicitHeight: Math.max(sliderHeader.implicitHeight, sliderValue.implicitHeight)

              PanelSectionHeader {
                id: sliderHeader
                text: sliderBlock.modelData.title
                foreground: root.bar.foreground
                fontFamily: root.bar.fontFamily
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
              }

              Text {
                id: sliderValue
                textFormat: Text.PlainText
                readonly property real shown: slider.dragging ? slider.liveValue : sliderBlock.current
                text: sliderBlock.spec.key === "opacity" ? Math.round(shown * 100) + "%" : root.times(shown)
                color: Qt.darker(root.bar.foreground, 1.4)
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.caption
                font.bold: true
                anchors.right: parent.right
                anchors.rightMargin: Style.space(6)
                anchors.verticalCenter: parent.verticalCenter
              }
            }

            CursorSurface {
              width: parent.width
              height: slider.implicitHeight + Style.spacing.controlGap
              hasCursor: root.cursorActive && root.focusSection === sliderBlock.modelData.section
              foreground: root.bar.foreground
              outline: true

              PanelSlider {
                id: slider
                bar: root.bar
                anchors.fill: parent
                anchors.leftMargin: Style.space(6)
                anchors.rightMargin: Style.space(6)
                minimum: sliderBlock.spec.min
                maximum: sliderBlock.spec.max
                step: sliderBlock.spec.step
                value: sliderBlock.current
                onMoved: function (v) { root.setValue(sliderBlock.spec.key, root.snap(sliderBlock.spec, v)) }
                onReleased: function (v) { root.setValue(sliderBlock.spec.key, root.snap(sliderBlock.spec, v)) }
              }

              HoverHandler {
                onHoveredChanged: if (hovered) {
                  root.cursorActive = true
                  root.focusSection = sliderBlock.modelData.section
                }
              }
            }
          }
        }

        // ---------- Layer ----------
        PanelSeparator {
          visible: root.installed
          foreground: root.bar.foreground
        }

        Column {
          visible: root.installed
          width: parent.width
          spacing: Style.space(6)

          PanelSectionHeader {
            text: "WHERE IT FALLS"
            foreground: root.bar.foreground
            fontFamily: root.bar.fontFamily
          }

          ButtonGroup {
            options: root.layerOptions
            value: root.snowLayer
            foreground: root.bar.foreground
            background: root.bar.background
            fontFamily: root.bar.fontFamily
            fontSize: Style.font.caption
            focusable: false
            cursorIndex: root.cursorActive && root.focusSection === "layer" ? root.selectedIndex : -1
            onChanged: function (v) {
              root.focusSection = "layer"
              root.setValue("layer", v)
            }
            onHovered: function (index, isHovered) {
              if (!isHovered) return
              root.cursorActive = true
              root.focusSection = "layer"
              root.selectedIndex = index
            }
          }

          Text {
            width: parent.width
            text: root.snowLayer === "top"
              ? "Over every window, the bar and notifications. Clicks pass through."
              : root.snowLayer === "dynamic"
                ? "Full snow on the wallpaper, and a faint tenth of it over your windows. Clicks pass through."
                : "On the wallpaper, behind windows and widgets. Pauses behind fullscreen windows."
            color: Qt.darker(root.bar.foreground, 1.6)
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.caption
            wrapMode: Text.WordWrap
          }
        }

        // ---------- Start with the session ----------
        PanelSeparator {
          visible: root.installed && root.hasUnit
          foreground: root.bar.foreground
        }

        Toggle {
          visible: root.installed && root.hasUnit
          width: parent.width
          label: "Start with the session"
          description: "Runs as the letitsnow user service"
          checked: root.autostart
          hasCursor: root.cursorActive && root.focusSection === "login"
          foreground: root.bar.foreground
          fontFamily: root.bar.fontFamily
          onClicked: {
            root.focusSection = "login"
            root.setAutostart(!root.autostart)
          }
          onHovered: function (on) {
            if (!on) return
            root.cursorActive = true
            root.focusSection = "login"
          }
        }

        // ---------- The service itself ----------
        PanelSeparator {
          visible: root.installed
          foreground: root.bar.foreground
        }

        Item {
          visible: root.installed
          width: parent.width
          implicitHeight: Math.max(serviceLabel.implicitHeight, removeButton.implicitHeight)

          Text {
            id: serviceLabel
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: "letitsnow " + root.serviceVersion
            color: Qt.darker(root.bar.foreground, 1.6)
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.caption
          }

          Button {
            id: removeButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            enabled: !root.setupRunning
            text: "Remove..."
            tooltipText: "Stop the snow and uninstall the service (the plugin stays)"
            foreground: root.bar.foreground
            fontFamily: root.bar.fontFamily
            fontSize: Style.font.caption
            onClicked: root.runUninstall()
          }
        }
      }
    }
  }
}
