// Main.qml — UI window top-level layout per Guide 03 §1.
//
// Skeleton:
//   ┌────────────────────────────────────────────────────────┐
//   │ [Menu bar (deferred)]                                  │
//   ├──────────┬─────────────────────────────┬───────────────┤
//   │ LeftRail │  ViewportOverlay (~36 px)   │ RightRail     │
//   │          │ ┌─────────────────────────┐ │               │
//   │ (stub)   │ │  centerStage (player)   │ │ (stub)        │
//   │          │ │                         │ │               │
//   │          │ └─────────────────────────┘ │               │
//   ├──────────┴─────────────────────────────┴───────────────┤
//   │ TransportBar (~56 px row 1, two rows future)           │
//   ├────────────────────────────────────────────────────────┤
//   │ TimelinePanel (~60 px Phase 2.4, ~220 OTIO future)     │
//   ├────────────────────────────────────────────────────────┤
//   │ ColorPanel (~180 px, toggleable)                       │
//   ├────────────────────────────────────────────────────────┤
//   │ StatusStrip (~22 px)                                   │
//   └────────────────────────────────────────────────────────┘
//
// SplitView handles the rail row's interactive resizing. The bottom
// stack uses a ColumnLayout with each band's preferred height.
// ColorPanel toggles via TransportBar's Color button → setting
// `colorPanelVisible` to false drops the panel's height to 0, the
// rail row reclaims the space, and centerStage's heightChanged
// signal triggers WindowManager::syncPlayerGeometry to resize the
// player child window automatically.

import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCore
import Qcv
import Qcv.Render

ApplicationWindow {
    id: root
    visible: true
    width: 1480
    height: 900
    // The native player surface is a separate top-level QWindow,
    // so closing the main UI window doesn't auto-quit the app —
    // which makes Finder think it's still running and refuses
    // to relaunch. Force a clean shutdown when the user closes
    // the main window via its red-X button.
    onClosing: Qt.quit()

    // Phase F.2.8 follow-up — minimize/restore UI repaint nudge.
    // On Windows the Qt RHI Vulkan backend doesn't eagerly recreate
    // its swapchain after the window's surface size goes to 0×0 on
    // minimize. It waits for a paint trigger, which on idle desktops
    // can take 3-4 seconds. The D3D11 + DComp player visual is
    // unaffected (DWM paints it from cached content), so the
    // viewport pops back instantly while the surrounding UI stays
    // blank. Hooking visibility transitions out of Minimized and
    // forcing requestUpdate() gives Qt the trigger it needs.
    onVisibilityChanged: {
        if (visibility !== ApplicationWindow.Minimized
            && visibility !== ApplicationWindow.Hidden) {
            requestUpdate()
        }
    }
    // Just the app name — project state lives in the inspector
    // / file menu, not the title bar. Keeps the macOS title strip
    // visually quiet.
    title: "QCView"
    color: "#101010"

    // Fusion's ScrollBar reads palette.mid (thumb at rest) and
    // palette.dark (thumb pressed). Default Fusion palette is too
    // dark against our #161616 / #1a1a1a surfaces — bars vanish.
    // Bump the thumb to a solid mid-grey so they read clearly,
    // pressed pops to bright. base = track behind the thumb.
    palette.mid:  Theme.textMuted      // #666 — thumb at rest
    palette.dark: Theme.textPrimary    // #ddd — thumb pressed
    palette.base: Theme.bgAlt          // #181818 — track bg

    // ---- File menu dialogs --------------------------------------
    // All Project / Open Media flows route through this. Centralized
    // in C++ on WindowManager so all QML call sites (file dialogs +
    // every drop handler) share one source of truth — UNC support
    // missed several places before; pulling it out into a Q_INVOKABLE
    // makes the next bug fix-once.
    function _urlToPath(url) {
        return url ? WindowManager.urlToOsPath(url) : "";
    }

    FileDialog {
        id: openMediaDialog
        title: qsTr("Open Media…")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("Media files (*.mov *.mp4 *.m4v *.mxf *.mkv *.avi *.webm "
                 + "*.wav *.aif *.aiff *.mp3 *.flac *.m4a "
                 + "*.png *.jpg *.jpeg *.tif *.tiff *.exr *.dpx)"),
            qsTr("All files (*)"),
        ]
        onAccepted: {
            const p = _urlToPath(selectedFile);
            if (WindowManager.project) {
                const id = WindowManager.project.addMediaFile(p);
                // Activate it so the viewport loads — matches the
                // drag-and-drop flow.
                if (id && id.length > 0)
                    WindowManager.project.setActiveItem(id);
            }
        }
    }
    FileDialog {
        id: openProjectDialog
        title: qsTr("Open Project…")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("QCView Project (*.qcvproj)")]
        onAccepted: {
            const p = _urlToPath(selectedFile);
            if (WindowManager.project)
                WindowManager.project.openProject(p);
        }
    }
    FileDialog {
        id: saveProjectAsDialog
        title: qsTr("Save Project As…")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "qcvproj"
        nameFilters: [qsTr("QCView Project (*.qcvproj)")]
        onAccepted: {
            const p = _urlToPath(selectedFile);
            if (WindowManager.project)
                WindowManager.project.saveProject(p);
        }
    }

    function projectSave() {
        const pm = WindowManager.project;
        if (!pm) return;
        if (pm.projectFilePath && pm.projectFilePath.length > 0) {
            pm.saveProject(pm.projectFilePath);
        } else {
            saveProjectAsDialog.open();
        }
    }
    function projectNew() {
        if (WindowManager.project)
            WindowManager.project.newProject();
    }

    // Recent menu callbacks run on the root ApplicationWindow's
    // context (stable across the menu rebuild that addMediaFile
    // synchronously kicks off via mediaFileAdded → recents
    // bumpToFront → recentMediaChanged → Repeater destroy/recreate).
    // Calling them from the MenuItem.onTriggered would access
    // WindowManager from the dying MenuItem context and throw
    // "WindowManager is not defined", skipping setActiveItem.
    // Normalize a stored path string for Windows: older sessions may
    // have persisted "/C:/..." (leading slash from a half-stripped
    // file:/// URL). Strip the leading slash when followed by a drive
    // letter so addMediaFile / openProject see a path Win32 can open.
    function _fixWindowsPath(p) {
        if (Qt.platform.os !== "windows" || !p) return p;
        if (p.length >= 3 && p.charAt(0) === "/"
            && p.charAt(2) === ":") {
            return p.substring(1);
        }
        return p;
    }
    function loadRecentMedia(path) {
        if (!WindowManager.project || !path) return;
        path = _fixWindowsPath(path);
        const id = WindowManager.project.addMediaFile(path);
        if (id && id.length > 0)
            WindowManager.project.setActiveItem(id);
    }
    function openRecentProject(path) {
        if (!WindowManager.project || !path) return;
        path = _fixWindowsPath(path);
        WindowManager.project.openProject(path);
    }

    // Surface project-IO errors to the user. Lightweight popup; the
    // C++ side emits projectError(msg) on save / load failure.
    Connections {
        target: WindowManager.project
        function onProjectError(message) {
            projectErrorDialog.text = message;
            projectErrorDialog.open();
        }
    }
    MessageDialog {
        id: projectErrorDialog
        title: qsTr("Project error")
        buttons: MessageDialog.Ok
    }

    menuBar: MenuBar {
        id: appMenuBar
        // Collapse to zero height in fullscreen — ApplicationWindow
        // honours menuBar.height for its layout, so this fully hides
        // the strip without recreating the menu (which would lose
        // popup wiring + accelerator state).
        height: root.inFullscreen ? 0 : implicitHeight
        visible: !root.inFullscreen
        // All menus use ThemedMenu — it sets popupType: Popup.Window
        // (required so the popup z-orders above the player child HWND
        // on Windows, per F.2.1) and recolors the popup to our Theme
        // so parent and submenus read as the same surface.
        ThemedMenu {
            id: fileMenu
            title: qsTr("&File")
            Action {
                text: qsTr("Open Media…")
                shortcut: StandardKey.Open
                onTriggered: openMediaDialog.open()
            }
            ThemedMenu {
                id: recentMediaMenu
                title: qsTr("Open Recent Media")
                // Popup.Window doesn't auto-grow to the widest item the
                // way the in-scene popup does — pin a width that fits
                // typical media filenames (long render-output names,
                // dated sequences, etc.) without forcing the user to
                // hover-to-elide.
                width: 520
                // Repeater (vs Instantiator + insertItem/removeItem)
                // — when the list bumps-to-front on click, Repeater
                // tears down and rebuilds delegates cleanly. Manual
                // insertItem on the macOS native menu was leaving
                // stale slots with empty text after a reorder.
                Repeater {
                    model: WindowManager.recentMedia
                    MenuItem {
                        required property string modelData
                        text: {
                            if (!modelData) return "";
                            // Split on BOTH separators — QSettings can
                            // round-trip native (\\) paths on Windows,
                            // and the prior /-only split left items
                            // showing the full drive-letter path.
                            const i = Math.max(modelData.lastIndexOf("/"),
                                                modelData.lastIndexOf("\\"));
                            return i >= 0 ? modelData.substring(i + 1)
                                          : modelData;
                        }
                        // Delegate to root.loadRecentMedia — its
                        // context survives the Repeater rebuild
                        // that addMediaFile triggers via the
                        // recents bumpToFront chain.
                        onTriggered: root.loadRecentMedia(modelData)
                    }
                }
                // macOS native menu doesn't reliably honor `visible:
                // false` on a MenuItem after it's been rendered once.
                // Use Repeaters with 0/1 model size so the items
                // exist only in the matching state.
                Repeater {
                    model: WindowManager.recentMedia.length > 0 ? 1 : 0
                    ThemedMenuSeparator { }
                }
                Repeater {
                    model: WindowManager.recentMedia.length === 0 ? 1 : 0
                    MenuItem {
                        text: qsTr("(none)")
                        enabled: false
                    }
                }
                Repeater {
                    model: WindowManager.recentMedia.length > 0 ? 1 : 0
                    MenuItem {
                        text: qsTr("Clear Menu")
                        onTriggered: WindowManager.clearRecentMedia()
                    }
                }
            }
            ThemedMenuSeparator {}
            Action {
                text: qsTr("New Project")
                onTriggered: root.projectNew()
            }
            Action {
                text: qsTr("Open Project…")
                shortcut: "Ctrl+Shift+O"
                onTriggered: openProjectDialog.open()
            }
            ThemedMenu {
                id: recentProjectsMenu
                title: qsTr("Open Recent Project")
                width: 520                // see recentMediaMenu note above
                Repeater {
                    model: WindowManager.recentProjects
                    MenuItem {
                        required property string modelData
                        text: {
                            if (!modelData) return "";
                            // Split on BOTH separators — see recentMedia
                            // note above for the Windows-path reason.
                            const i = Math.max(modelData.lastIndexOf("/"),
                                                modelData.lastIndexOf("\\"));
                            return i >= 0 ? modelData.substring(i + 1)
                                          : modelData;
                        }
                        // Delegate to root.openRecentProject so the
                        // open survives the Repeater rebuild that
                        // openProject triggers via the recents
                        // bumpToFront chain.
                        onTriggered: root.openRecentProject(modelData)
                    }
                }
                Repeater {
                    model: WindowManager.recentProjects.length > 0 ? 1 : 0
                    ThemedMenuSeparator { }
                }
                Repeater {
                    model: WindowManager.recentProjects.length === 0 ? 1 : 0
                    MenuItem {
                        text: qsTr("(none)")
                        enabled: false
                    }
                }
                Repeater {
                    model: WindowManager.recentProjects.length > 0 ? 1 : 0
                    MenuItem {
                        text: qsTr("Clear Menu")
                        onTriggered: WindowManager.clearRecentProjects()
                    }
                }
            }
            Action {
                text: qsTr("Save Project")
                shortcut: StandardKey.Save
                onTriggered: root.projectSave()
            }
            Action {
                text: qsTr("Save Project As…")
                shortcut: StandardKey.SaveAs
                onTriggered: saveProjectAsDialog.open()
            }
            ThemedMenuSeparator {}
            Action {
                text: qsTr("Copy Project Link")
                // Enabled when the project has been saved (so its
                // UUID is meaningful and shareable) AND a media
                // item is currently active. The C++ side returns
                // empty for buildProjectLink() in either gap; we
                // bind to that as the single source of truth.
                enabled: WindowManager.project
                         && WindowManager.project.projectFilePath
                         && WindowManager.project.projectFilePath.length > 0
                         && WindowManager.project.activeItemId
                         && WindowManager.project.activeItemId.length > 0
                onTriggered: WindowManager.copyProjectLinkToClipboard()
            }
        }
        ThemedMenu {
            id: viewMenu
            title: qsTr("&View")
            Action {
                text: qsTr("Minimal Mode")
                shortcut: "Ctrl+-"
                onTriggered: root.viewMinimal()
            }
            Action {
                text: qsTr("Show All Panels")
                shortcut: "Ctrl+9"
                onTriggered: root.viewAllPanels()
            }
            Action {
                // F (no modifier) is handled by WindowManager's C++
                // event filter so it auto-bails when a text input
                // has focus. Adding a menu shortcut would bypass
                // that bailout and steal F mid-typing.
                text: qsTr("Fullscreen")
                onTriggered: root.toggleFullscreen()
            }
            ThemedMenuSeparator {}
            // Panel toggles — explicitly NOT checkable. macOS
            // native menus reserve a checkmark indicator column
            // for the entire menu when any item is checkable,
            // and we'd rather have a clean text-only menu. Panel
            // visibility is already obvious in the UI itself.
            Action {
                text: qsTr("Left Rail")
                shortcut: "Ctrl+1"
                onTriggered: root.leftRailCollapsed = !root.leftRailCollapsed
            }
            Action {
                text: qsTr("Right Rail")
                shortcut: "Ctrl+2"
                onTriggered: root.rightRailCollapsed = !root.rightRailCollapsed
            }
            Action {
                text: qsTr("Color Panel")
                shortcut: "Ctrl+3"
                onTriggered: root.colorPanelVisible = !root.colorPanelVisible
            }
            Action {
                text: qsTr("Timeline")
                onTriggered: root.timelineVisible = !root.timelineVisible
            }
            Action {
                text: qsTr("Notes")
                shortcut: "Ctrl+4"
                onTriggered: root.notesPanelVisible = !root.notesPanelVisible
            }
        }
        // Tools menu — section reveals + shortcut viewer +
        // settings. Split out from View so the macOS native
        // icon column reserved for "Fullscreen" doesn't bleed
        // across to these items. Tools has no icon-triggering
        // entries, so it stays flush-left.
        ThemedMenu {
            id: toolsMenu
            title: qsTr("&Tools")
            Action {
                text: qsTr("Background")
                onTriggered: {
                    root.leftRailCollapsed = false;
                    if (typeof leftRail !== "undefined" && leftRail.revealSection)
                        leftRail.revealSection("background");
                }
            }
            Action {
                text: qsTr("Safety Guides")
                onTriggered: {
                    root.leftRailCollapsed = false;
                    if (typeof leftRail !== "undefined" && leftRail.revealSection)
                        leftRail.revealSection("safety");
                }
            }
            Action {
                text: qsTr("Keyboard Shortcuts")
                shortcut: "Ctrl+/"
                onTriggered: {
                    // Shortcuts now live in the RightRail under the
                    // Inspector. Open / uncollapse right + reveal.
                    root.rightRailVisible  = true;
                    root.rightRailCollapsed = false;
                    if (typeof rightRail !== "undefined" && rightRail.revealSection)
                        rightRail.revealSection("shortcuts");
                }
            }
            Action {
                text: qsTr("Settings…")
                shortcut: "Ctrl+,"
                onTriggered: {
                    root.leftRailCollapsed = false;
                    if (typeof leftRail !== "undefined" && leftRail.revealSection)
                        leftRail.revealSection("settings");
                }
            }
        }
        // About menu — version stamp + a link to the public docs
        // site. Version comes from Qt.application.version, populated
        // by QGuiApplication::setApplicationVersion(QCV_VERSION_STR)
        // in main.cpp (which is fed by PROJECT_VERSION in the top
        // CMakeLists.txt).
        ThemedMenu {
            id: aboutMenu
            title: qsTr("&About")
            Action {
                text: qsTr("QCView v%1").arg(Qt.application.version)
                enabled: false
            }
            ThemedMenuSeparator {}
            Action {
                text: qsTr("Docs")
                onTriggered: Qt.openUrlExternally("https://qcview.app/")
            }
        }
    }

    // Install a window-level keyboard event filter so transport keys
    // (A/D held FF/RW, J/L mirrors) fire regardless of which QML item
    // owns focus. The filter is in WindowManager (C++); QML side
    // just hands it the window. Text fields stay typeable — the
    // filter checks for QQuickTextInput/Edit and lets those keys
    // pass through unmodified.
    Component.onCompleted: {
        WindowManager.installGlobalKeyFilter(root);
        WindowManager.setNotesPanelVisible(fxNotesPanel);
    }

    // App-level panel visibility. Each is a plain bool the panel
    // QML files bind their `visible` / preferred-width to. Keyboard
    // shortcuts mutate them via the toggle methods below.
    property bool leftRailVisible:    true
    property bool rightRailVisible:   true
    property bool timelineVisible:    true
    property bool transportVisible:   true
    property bool statusStripVisible: true
    property bool colorPanelVisible:  false
    // Phase 3.H.6 Stage B — bottom notes filmstrip. Default off
    // (most sessions don't need it); user toggles via Ctrl+5 or
    // its dedicated button. Auto-hidden when annotations aren't
    // allowed for the active item (dual / playlist / non-AV).
    property bool notesPanelVisible:  false

    // Color and Notes share the bottom band, so only one can be
    // open at a time. Enforced at the property level so every
    // toggle site (menus, transport button, view* helpers, future
    // callers) gets the mutex for free — no risk of one site
    // forgetting. The "if (...)" guards short-circuit the no-op
    // case so we don't bounce signals when the other panel was
    // already closed.
    onColorPanelVisibleChanged: {
        if (colorPanelVisible && notesPanelVisible) notesPanelVisible = false;
    }
    onNotesPanelVisibleChanged: {
        if (notesPanelVisible && colorPanelVisible) colorPanelVisible = false;
    }

    // Fullscreen short-circuits *every* panel: when the window's
    // visibility flips to FullScreen, the player surface is the
    // only thing on screen. Each panel binds `visible` and its
    // Layout slot through these `fx*` derived bools so a single
    // F press hides the chrome, and Esc / second F restores
    // exactly the panel state the user had configured.
    readonly property bool inFullscreen:
        borderlessFs || visibility === ApplicationWindow.FullScreen
    readonly property bool fxLeftRail:    leftRailVisible    && !inFullscreen
    readonly property bool fxRightRail:   rightRailVisible   && !inFullscreen
    readonly property bool fxTimeline:    timelineVisible    && !inFullscreen
    readonly property bool fxTransport:   transportVisible   && !inFullscreen
    readonly property bool fxStatusStrip: statusStripVisible && !inFullscreen
    readonly property bool fxColorPanel:  colorPanelVisible  && !inFullscreen
    readonly property bool fxNotesPanel:
        notesPanelVisible
        && !inFullscreen
        && WindowManager.annotationsAllowed
    onFxNotesPanelChanged: WindowManager.setNotesPanelVisible(fxNotesPanel)

    // ---- Resizable layout state -----------------------------------
    // Widths in pixels for the rails when expanded; the rails go to
    // kSlimWidth when collapsed instead of fully hiding (a slim
    // strip with a chevron stays visible so the user can re-expand
    // without keyboard / menu help). Persisted to QSettings via the
    // Settings element below.
    // Collapsed rail width — locked to Theme.gutterWidth so the
    // slim strip lines up vertically with the timeline's track-
    // header gutter and the transport row's left padding.
    readonly property int kSlimWidth: Theme.gutterWidth
    readonly property int kMinRailWidth: 200
    readonly property int kMaxRailWidth: 600
    readonly property int kMinColorPanelHeight: 140
    readonly property int kMaxColorPanelHeight: 800

    property int  leftRailWidth:     280
    property int  rightRailWidth:    320
    property int  colorPanelHeight:  360
    property bool leftRailCollapsed:  false
    property bool rightRailCollapsed: false

    // QtCore.Settings round-trips the layout state to QSettings
    // (org/app names set in main.cpp). Property aliases are
    // bidirectional — startup loads the stored value, runtime
    // changes write back automatically.
    Settings {
        category: "ui.layout"
        property alias leftRailWidth:      root.leftRailWidth
        property alias rightRailWidth:     root.rightRailWidth
        property alias colorPanelHeight:   root.colorPanelHeight
        property alias leftRailCollapsed:  root.leftRailCollapsed
        property alias rightRailCollapsed: root.rightRailCollapsed
    }

    // Effective widths fed to the layout: clamped + collapsed.
    readonly property int effLeftRailWidth:
        leftRailCollapsed ? kSlimWidth
                          : Math.max(kMinRailWidth,
                                     Math.min(kMaxRailWidth, leftRailWidth))
    readonly property int effRightRailWidth:
        rightRailCollapsed ? kSlimWidth
                           : Math.max(kMinRailWidth,
                                      Math.min(kMaxRailWidth, rightRailWidth))

    // ---- Inline components: drag handles --------------------------
    // 4 px ribbons sandwiched between fixed-size and fillWidth/Height
    // siblings. The hit area extends 3 px beyond the visible
    // ribbon so the cursor target isn't pixel-perfect. Each handle
    // emits dragged(delta) per move; the consumer applies the
    // delta to the appropriate property. Tracking via global
    // coordinates avoids the local-coord drift that happens when
    // the parent's layout shifts mid-drag.
    // Resize handles. The visible body is transparent in the
    // resting state — the rail's own 1 px edge divider is what
    // the user sees. On hover / drag the handle paints a 1-px-
    // wide accent strip down its center, matching the sister
    // app's "splitter goes blue when grabbable" pattern. The
    // 4-px layout slot keeps the hit area generous without
    // making the resting separator visually heavy.
    // 1-px wide layout slot — the visible separator IS the
    // handle, sitting flush next to the rail/panel content.
    // The MouseArea overshoots ±3 px on each side via negative
    // margins, giving a ~7-px hit zone without bloating the
    // resting visual.
    component HResizeHandle: Item {
        Layout.preferredWidth: 1
        Layout.fillHeight: true
        signal dragged(real dx)
        Rectangle {
            anchors.fill: parent
            // Solid Theme.divider at rest so the 1-px slot
            // doesn't show the ApplicationWindow background
            // through it. Sits flush next to the rail's own
            // edge divider — together they read as a single
            // 2-px seam (acceptable; was 1 px solid + 1 px
            // transparent gap before).
            color: ma.pressed || ma.containsMouse
                   ? Theme.accent : Theme.divider
        }
        MouseArea {
            id: ma
            anchors.fill: parent
            anchors.leftMargin:  -3
            anchors.rightMargin: -3
            cursorShape: Qt.SplitHCursor
            hoverEnabled: true
            property real lastX: 0
            onPressed: lastX = mapToGlobal(Qt.point(mouseX, 0)).x
            onPositionChanged: {
                if (!pressed) return;
                const cur = mapToGlobal(Qt.point(mouseX, 0)).x;
                parent.dragged(cur - lastX);
                lastX = cur;
            }
        }
    }
    component VResizeHandle: Item {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        signal dragged(real dy)
        Rectangle {
            anchors.fill: parent
            color: ma.pressed || ma.containsMouse
                   ? Theme.accent : Theme.divider
        }
        MouseArea {
            id: ma
            anchors.fill: parent
            anchors.topMargin:    -3
            anchors.bottomMargin: -3
            cursorShape: Qt.SplitVCursor
            hoverEnabled: true
            property real lastY: 0
            onPressed: lastY = mapToGlobal(Qt.point(0, mouseY)).y
            onPositionChanged: {
                if (!pressed) return;
                const cur = mapToGlobal(Qt.point(0, mouseY)).y;
                parent.dragged(cur - lastY);
                lastY = cur;
            }
        }
    }

    // ---- View modes ---------------------------------------------
    // "default" = everything on. "minimal" = viewport + transport
    // only (matches old app's Ctrl+- minimal view). "all" = same
    // as default for now (panels we haven't built yet will show
    // when their flags here flip on).
    function viewDefault() {
        root.leftRailVisible    = true;
        root.rightRailVisible   = true;
        root.leftRailCollapsed  = false;
        root.rightRailCollapsed = false;
        root.timelineVisible    = true;
        root.transportVisible   = true;
        root.statusStripVisible = true;
        root.colorPanelVisible  = true;
    }
    function viewMinimal() {
        // Collapse the rails (slim chevron strip stays visible)
        // rather than hide them — keeps the re-expand affordance
        // a single click away.
        root.leftRailVisible    = true;
        root.rightRailVisible   = true;
        root.leftRailCollapsed  = true;
        root.rightRailCollapsed = true;
        root.timelineVisible    = true;
        root.transportVisible   = true;
        root.statusStripVisible = false;
        root.colorPanelVisible  = false;
    }
    function viewAllPanels() {
        viewDefault();
    }

    function cycleBackgroundMode() {
        const cur = WindowManager.backgroundMode;
        WindowManager.backgroundMode = (cur + 1) % 4;
    }

    // Fullscreen — on macOS we route through WindowManager's
    // borderless-fullscreen helper (NSWindow level/styleMask/
    // presentationOptions manipulation) so the OS zoom-into-Space
    // animation is bypassed; on other platforms (or if the helper
    // call returns false) we fall back to setVisibility(FullScreen).
    // The borderless flag drives `inFullscreen` so the panels still
    // hide.
    property bool borderlessFs: false
    property int  preFullscreenVisibility: ApplicationWindow.Windowed

    function toggleFullscreen() {
        if (borderlessFs ||
            root.visibility === ApplicationWindow.FullScreen) {
            exitFullscreen();
        } else {
            if (WindowManager.enterBorderlessFullscreen(root)) {
                borderlessFs = true;
                reclaimKeyboardFocus();
            } else {
                preFullscreenVisibility = root.visibility;
                root.visibility = ApplicationWindow.FullScreen;
            }
        }
    }
    function exitFullscreen() {
        if (borderlessFs) {
            WindowManager.exitBorderlessFullscreen(root);
            borderlessFs = false;
            reclaimKeyboardFocus();
        } else if (root.visibility === ApplicationWindow.FullScreen) {
            root.visibility = preFullscreenVisibility !== 0
                              ? preFullscreenVisibility
                              : ApplicationWindow.Windowed;
        }
    }
    // Restoring NSWindow key status from C++ isn't always enough —
    // Qt's focus chain can still be pointed at a stale FocusScope.
    // Re-poke from QML so the window regains keyboard activity.
    function reclaimKeyboardFocus() {
        root.requestActivate();
    }

    // ---- Keyboard shortcuts -------------------------------------
    // Old app's shortcut_manager mappings ported as Qt Shortcut
    // items. Hold-down FF/RW (A/D + J/L mirrors) lives in
    // WindowManager's window-level event filter (see
    // installGlobalKeyFilter above) so they fire regardless of
    // which QML item owns focus.
    //
    // Phase 3.H.6 — All single-letter transport / scope keys are
    // now serviced by the C++ application event filter (in
    // window_manager.cpp::eventFilter) which auto-bails when a
    // QQuickTextInput / QQuickTextEdit has focus. The QML Shortcut
    // elements with Qt.ApplicationShortcut context bypass that
    // bailout — typing in a TextArea would re-fire Space/W/B etc.
    // and clobber the input. Removing them here lets the C++
    // filter own the routing, keeping the text-input bailout
    // intact.
    //
    // Modifier-prefixed shortcuts (Ctrl+L, Ctrl+1..5, etc.) stay
    // here because Cmd / Ctrl + letter doesn't conflict with
    // typing.
    Shortcut { sequence: "Ctrl+L";   context: Qt.ApplicationShortcut; onActivated: WindowManager.loopEnabled = !WindowManager.loopEnabled }
    Connections {
        target: WindowManager
        function onFullscreenToggleRequested() { root.toggleFullscreen(); }
    }
    // Phase 3.H.4 — gate Esc on actually-fullscreen so it doesn't
    // swallow inline rename / edit-cancel Esc handlers when the
    // window isn't fullscreen.
    Shortcut {
        sequence: "Escape"
        enabled: root.borderlessFs
                 || root.visibility === ApplicationWindow.FullScreen
        onActivated: root.exitFullscreen()
    }
    // Menu items' `shortcut` properties handle the file/view
    // accelerators (Cmd+O, Cmd+S, Cmd+1..4, etc.). Standalone
    // Shortcut elements only remain for keys NOT in the menu.
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; onActivated: viewDefault() }
    Shortcut { sequence: "Ctrl+R"; context: Qt.ApplicationShortcut; onActivated: viewDefault() }

    // Auto-reveal of the left rail on EXR load was removed —
    // forcing the rail open on every load was disruptive when the
    // user had collapsed it intentionally. They can toggle the rail
    // with Ctrl+1 or the chevron when they need the layer chooser.

    FileDialog {
        id: openVideoDialog
        title: qsTr("Open media")
        // Phase 7.4.a: image extensions added so the dialog can
        // pick any frame of a sequence — ProjectManager.addMediaFile
        // detects siblings and creates one ImageSequence MediaItem.
        nameFilters: [
            qsTr("Media files (*.mov *.mp4 *.m4v *.mxf *.mkv *.avi *.png *.jpg *.jpeg *.tif *.tiff *.exr *.dpx)"),
            qsTr("Video files (*.mov *.mp4 *.m4v *.mxf *.mkv *.avi)"),
            qsTr("Image / sequence files (*.png *.jpg *.jpeg *.tif *.tiff *.exr *.dpx)"),
            qsTr("All files (*)")
        ]
        fileMode: FileDialog.OpenFile
        onAccepted: {
            // Phase 7.1: route through ProjectManager so the file
            // lands in the bin AND becomes the active selection
            // (which loads it into the player via WindowManager's
            // loadRequested wiring). Opening a path that's already
            // in the bin returns the existing id without dupe.
            const path = _urlToPath(selectedFile);
            if (!WindowManager.project) {
                console.warn("Main: ProjectManager unavailable");
                return;
            }
            const id = WindowManager.project.addMediaFile(path);
            if (id) WindowManager.project.setActiveItem(id);
            else    console.warn("Main: addMediaFile failed:", path);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Rail row
        // RowLayout with fixed-width rails for Phase 2.4. Switching
        // to SplitView (draggable dividers) is a follow-up — the
        // macOS-native SplitView style overrides preferredWidth /
        // implicitWidth / explicit width on children at startup,
        // making it impossible to land the initial 280/center/320
        // split reliably. Re-add SplitView with the QtQuick.Controls
        // Basic style (or our own handle item) when we wire layout
        // persistence in Phase 7.
        RowLayout {
            id: railRow
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            LeftRail {
                id: leftRail
                // Bind so the rail's Del / Backspace shortcuts know
                // when to defer to the TimelinePanel's clip-delete
                // (only when a playlist track is actually in edit
                // mode, not whenever a playlist is active).
                timelineTrackEditing:
                    timelinePanel.editingA || timelinePanel.editingB
                Layout.preferredWidth:
                    root.fxLeftRail ? root.effLeftRailWidth : 0
                Layout.minimumWidth:
                    root.fxLeftRail
                        ? (root.leftRailCollapsed ? root.kSlimWidth
                                                  : root.kMinRailWidth)
                        : 0
                Layout.fillHeight: true
                visible: root.fxLeftRail
                collapsed: root.leftRailCollapsed
                onToggleCollapsed: root.leftRailCollapsed = !root.leftRailCollapsed
                Behavior on Layout.preferredWidth {
                    NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                }
            }
            HResizeHandle {
                visible: root.fxLeftRail && !root.leftRailCollapsed
                onDragged: dx => root.leftRailWidth = Math.max(
                    root.kMinRailWidth,
                    Math.min(root.kMaxRailWidth, root.leftRailWidth + dx))
            }

            // Center pane: viewport overlay strip + center stage.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 320

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    ViewportOverlay {
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.inFullscreen ? 0 : 36
                        visible: !root.inFullscreen
                    }

                    // The player child window is reparented onto this
                    // Item via WindowManager::initialize; it tracks the
                    // Item's position+size every frame to keep the
                    // child window pinned to it.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        // Sits UNDER the D3D11 child HWND. Peeks through
                        // whenever the child can't keep up with QML
                        // layout changes (e.g. rail-collapse animation).
                        // Matched to #1b1b1b along with the WM_ERASEBKGND
                        // fill in window_manager.cpp and the D3D11
                        // seed-present in d3d11_player_renderer.cpp, so
                        // the three seam surfaces blend into one tone.
                        // macOS Metal renderer clears to black, so keep
                        // that match on that platform.
                        color: Qt.platform.os === "osx" ? "black" : "#1b1b1b"

                        Item {
                            id: centerStage
                            objectName: "centerStage"
                            anchors.fill: parent

                            // Phase F.2.3 (Windows): drag-drop into the
                            // centerStage. The D3D11 child HWND uses
                            // HTTRANSPARENT in WM_NCHITTEST, so native
                            // OS drops fall through to Qt's UI HWND →
                            // QML scene → this DropArea. macOS already
                            // covers drag-drop via PlayerWindow's
                            // filesDropped signal (it owns the surface
                            // HWND on macOS), so this DropArea is the
                            // Windows-equivalent path. Harmless on
                            // macOS where it sits under the PlayerWindow.
                            DropArea {
                                anchors.fill: parent
                                onDropped: (drop) => {
                                    if (!drop.hasUrls) return;
                                    if (!WindowManager.project) return;
                                    let lastId = "";
                                    for (let i = 0; i < drop.urls.length; ++i) {
                                        const path = WindowManager.urlToOsPath(drop.urls[i]);
                                        if (!path) continue;
                                        const id = WindowManager.project.addMediaFile(path);
                                        if (id && id.length > 0) lastId = id;
                                    }
                                    if (lastId.length > 0)
                                        WindowManager.project.setActiveItem(lastId);
                                    drop.accept();
                                }
                            }

                            // Phase F.2.7 (Windows) — annotation pointer
                            // routing. On macOS PlayerWindow's QWindow
                            // mouse virtuals forward to ViewportAnnotator
                            // directly. On Windows the D3D11 child HWND
                            // is HTTRANSPARENT so mouse events arrive
                            // here in QML; we forward them via
                            // WindowManager.forwardViewportPointer().
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton
                                // hoverEnabled lets us see Move events
                                // even without a button held — needed by
                                // some tool modes (e.g., eraser hover
                                // preview). Cheap; one Qt event per move.
                                hoverEnabled: false
                                onPressed: (mouse) => {
                                    WindowManager.forwardViewportPointer(
                                        0, mouse.x, mouse.y, Date.now());
                                }
                                onPositionChanged: (mouse) => {
                                    WindowManager.forwardViewportPointer(
                                        1, mouse.x, mouse.y, Date.now());
                                }
                                onReleased: (mouse) => {
                                    WindowManager.forwardViewportPointer(
                                        2, mouse.x, mouse.y, Date.now());
                                }
                            }

                            // Split-wipe seam drag handle. Only present in
                            // compositorMode === 2 (the wipe seam mode);
                            // sits above the annotation MouseArea on a
                            // narrow vertical strip centered on the seam,
                            // so clicks/drags on the seam adjust splitPos
                            // instead of feeding the annotator. The strip
                            // is kSeamGrabWidth px wide — wide enough to
                            // grab comfortably, narrow enough that the
                            // annotation footprint is barely affected.
                            // Windows-only path: the D3D11 child HWND is
                            // HTTRANSPARENT so QML sees the mouse events.
                            // On macOS PlayerWindow owns the QWindow and
                            // would need its own forwarding hook to drive
                            // splitPos — deferred until that path lands.
                            MouseArea {
                                id: splitWipeSeamDrag
                                readonly property int kSeamGrabWidth: 14
                                visible: WindowManager.compositorMode === 2
                                         && WindowManager.dualController
                                enabled: visible
                                width: kSeamGrabWidth
                                height: parent.height
                                // Clamp so the strip stays fully inside
                                // centerStage even when splitPos is 0 / 1.
                                x: Math.max(0,
                                    Math.min(parent.width - kSeamGrabWidth,
                                             parent.width * WindowManager.splitPos
                                             - kSeamGrabWidth / 2))
                                z: 20
                                cursorShape: Qt.SplitHCursor
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton
                                function updateFromMouse(mx) {
                                    if (parent.width <= 0) return;
                                    const cursorX = x + mx;
                                    WindowManager.splitPos =
                                        Math.max(0, Math.min(1, cursorX / parent.width));
                                }
                                // Click without drag snaps the seam to
                                // the click location; drag tracks the
                                // cursor frame-by-frame.
                                onPressed: (mouse) => updateFromMouse(mouse.x)
                                onPositionChanged: (mouse) => {
                                    if (!pressed) return;
                                    updateFromMouse(mouse.x);
                                }
                            }
                        }

                        // Detached state placeholder — visible only when
                        // the player has been popped into its own window.
                        Column {
                            anchors.centerIn: parent
                            spacing: 6
                            visible: WindowManager.detached

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qsTr("Player detached")
                                font.pixelSize: 16
                                color: "#888888"
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qsTr("Click 'Reattach' to dock back")
                                font.pixelSize: 11
                                color: "#666666"
                            }
                        }
                    }
                }
            }

            HResizeHandle {
                visible: root.fxRightRail && !root.rightRailCollapsed
                // Drag right shrinks the right rail (inverse sign).
                onDragged: dx => root.rightRailWidth = Math.max(
                    root.kMinRailWidth,
                    Math.min(root.kMaxRailWidth, root.rightRailWidth - dx))
            }
            RightRail {
                id: rightRail
                Layout.preferredWidth:
                    root.fxRightRail ? root.effRightRailWidth : 0
                Layout.minimumWidth:
                    root.fxRightRail
                        ? (root.rightRailCollapsed ? root.kSlimWidth
                                                   : root.kMinRailWidth)
                        : 0
                Layout.fillHeight: true
                visible: root.fxRightRail
                collapsed: root.rightRailCollapsed
                onToggleCollapsed: root.rightRailCollapsed = !root.rightRailCollapsed
                Behavior on Layout.preferredWidth {
                    NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                }
            }
        }

        // ---- Bottom band 0: Timeline status row (frame / TC / fps).
        // Sits between viewport area and the transport row so
        // transport tooltips render up into this strip rather than
        // off the bottom of the viewport.
        TimelineStatus {
            Layout.fillWidth: true
            Layout.preferredHeight: root.fxStatusStrip ? Theme.toolStripHeight : 0
            visible: root.fxStatusStrip
        }

        // ---- Bottom band 1: Transport
        TransportBar {
            Layout.fillWidth: true
            Layout.preferredHeight: root.fxTransport ? Theme.toolStripHeight : 0
            visible: root.fxTransport
            colorPanelVisible: root.colorPanelVisible
            onToggleColorPanel: root.colorPanelVisible = !root.colorPanelVisible
            notesPanelVisible: root.notesPanelVisible
            onToggleNotesPanel: root.notesPanelVisible = !root.notesPanelVisible
            onToggleFullscreenRequested: root.toggleFullscreen()
            onOpenVideoRequested: openVideoDialog.open()
            onShowRightRailSection: name => {
                // Force-open the rail and expand the requested
                // section. Same pattern the EXR auto-reveal uses
                // for the left rail.
                root.rightRailVisible   = true;
                root.rightRailCollapsed = false;
                if (rightRail) rightRail.revealSection(name);
            }
            onShowLeftRailSection: name => {
                root.leftRailVisible   = true;
                root.leftRailCollapsed = false;
                if (leftRail) leftRail.revealSection(name);
            }
        }

        // ---- Bottom band 2: Timeline
        // Single source: ruler (20) + track A (44) + borders ≈ 68.
        // Dual source: + track B (28) + separator ≈ 96. Bound to
        // TimelinePanel.contentHeight so we don't have to keep two
        // numbers in sync; the panel itself drives its own height.
        TimelinePanel {
            id: timelinePanel
            Layout.fillWidth: true
            Layout.preferredHeight:
                root.fxTimeline ? timelinePanel.desiredHeight : 0
            visible: root.fxTimeline
            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
            }
            onPlaylistContentDropped: leftRail.commitActiveRename()
        }

        // ---- Bottom band: Notes filmstrip (Phase 3.H.6 Stage B)
        // Toggleable via Ctrl+5; auto-hides when annotations
        // aren't allowed for the active item (dual / playlist).
        NotesPanel {
            id: notesPanel
            Layout.fillWidth: true
            Layout.preferredHeight: root.fxNotesPanel ? 270 : 0
            visible: root.fxNotesPanel
            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
            }
        }

        // ---- Bottom band 3: Color (toggleable + resizable)
        // Height bound to root.colorPanelHeight (persisted) when
        // visible. The handle above lets the user drag the panel's
        // top edge to resize.
        VResizeHandle {
            visible: root.fxColorPanel
            // Drag DOWN shrinks the color panel (its bottom edge
            // is pinned to the StatusStrip below); inverse sign.
            onDragged: dy => root.colorPanelHeight = Math.max(
                root.kMinColorPanelHeight,
                Math.min(root.kMaxColorPanelHeight,
                         root.colorPanelHeight - dy))
        }
        ColorPanel {
            Layout.fillWidth: true
            Layout.preferredHeight:
                root.fxColorPanel ? root.colorPanelHeight : 0
            visible: root.fxColorPanel

            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
            }
        }

        // ---- Status strip (very bottom). Hidden during fullscreen
        // so the very last horizontal sliver doesn't block the
        // viewport from filling.
        StatusStrip {
            Layout.fillWidth: true
            Layout.preferredHeight: root.inFullscreen ? 0 : 22
            visible: !root.inFullscreen
        }
    }

    // 2026-05-14 — click-eater while a MenuBar menu is open.
    // The menu popups use Popup.Window so they z-order above the
    // player's child HWND (F.2.1 invariant). On Qt 6.11 those
    // Window-type popups don't create a modal overlay in the parent
    // QtQuick scene, so clicks "outside" the popup leak through to
    // QML items underneath (e.g. clicking File > Open also fires
    // the LeftRail row right under the popup). modal:true on the
    // menu doesn't fix it on Window-type popups.
    //
    // This MouseArea sits in the parent QtQuick scene above
    // everything else, enabled only while a MenuBar menu is open
    // (appMenuBar.currentIndex !== -1). It eats any click that
    // would otherwise hit a QML item beneath the popup. The menu
    // itself closes via its own CloseOnPressOutside / focus-loss
    // policy when the click lands here.
    readonly property bool anyMenuOpen:
        fileMenu.opened || viewMenu.opened || toolsMenu.opened

    MouseArea {
        id: menuClickEater
        anchors.fill: parent
        z: 10000
        enabled: root.anyMenuOpen
        visible: enabled
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        hoverEnabled: false
        // Just consume the event; don't call close() on the menus.
        // Calling close() here races with the menu item's own auto-
        // close after MenuItem.triggered, leaving the menu stuck
        // open after a successful click (e.g. Open Recent → file).
        // Qt's own CloseOnPressOutside policy still fires the close
        // before our MouseArea sees the event because the underlying
        // QWindow press is observed by Qt before QML hit-testing.
        onPressed: function(m) { m.accepted = true; }
    }

    function graphicsApiName(): string {
        switch (root.GraphicsInfo.api) {
            case GraphicsInfo.OpenGL:        return "OpenGL"
            case GraphicsInfo.Direct3D11:    return "Direct3D 11"
            case GraphicsInfo.Direct3D12:    return "Direct3D 12"
            case GraphicsInfo.Vulkan:        return "Vulkan"
            case GraphicsInfo.Metal:         return "Metal"
            case GraphicsInfo.Null:          return "Null"
            default:                         return "Unknown"
        }
    }
}
