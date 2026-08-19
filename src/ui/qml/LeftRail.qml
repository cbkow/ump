// LeftRail — Phase 7.1 (flat-list shape).
//
// Single flat list of every MediaItem in the project, with a type
// glyph per row (Videos / Audio / Images / Playlists). The four-bin
// data structure persists internally for routing + .qcvproj write
// ordering, but the UI doesn't expose it as collapsible sections —
// every item is one click from selection. A single "+" button at the
// top opens a multi-type file dialog.
//
// Inspector + EXR Layers sections are still stubbed (7.2 / 7.4).

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Qcv

// Rectangle root (was Pane). Pane's automatic
// implicitContentHeight = childrenRect.height creates a circular
// dependency with an anchored ColumnLayout that holds a fillHeight
// ScrollView — the ScrollView ends up sized to the natural
// content height instead of the available height, so it never
// scrolls. Rectangle has no implicit-size logic; the SplitView /
// Layout above passes the full rail height straight through.
Rectangle {
    id: root
    implicitWidth: 280
    // Darker well (aesthetics pass 2) — sections render as raised
    // card planes on this, mirroring the right rail's treatment.
    color: Theme.bg
    // Right-edge seam — COLLAPSED state only. When expanded, the
    // HResizeHandle beside the rail is itself a 1-px divider line
    // (accent on hover), so drawing this too rendered a 2-px double
    // border. Collapsed hides the handle, so the rail carries its
    // own seam against the viewport there.
    Rectangle {
        visible: root.collapsed
        anchors.right:  parent.right
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        // One device pixel — matches HResizeHandle's seam so the
        // line doesn't change weight between collapsed / expanded
        // at fractional display scales.
        width:  1 / Screen.devicePixelRatio
        color:  Theme.divider
        z: 100
    }

    // Slim-collapse state. When true, only the header strip's
    // chevron is visible; the body content is hidden so the rail
    // can shrink to ~32 px without trying to render its scroll
    // contents in a too-narrow band.
    property bool collapsed: false
    signal toggleCollapsed()

    // Programmatic reveal — Main.qml routes the transport bar's
    // "Safety" / "BG" buttons here. Sections were moved off the
    // right rail in Stage C polish so the right rail can be
    // Inspector-only.
    function revealSection(name) {
        if (typeof safetyGuidesSection !== "undefined") {
            safetyGuidesSection.expanded = (name === "safety");
        }
        if (typeof backgroundSection !== "undefined") {
            backgroundSection.expanded   = (name === "background");
        }
        if (typeof settingsSection !== "undefined") {
            settingsSection.expanded = (name === "settings");
        }
        // Defer one tick so the just-expanded Section has finished
        // laying out before we measure its position; then scroll
        // the target into view inside bodyScroll's Flickable.
        Qt.callLater(function() {
            var target = null;
            if (name === "safety"
                && typeof safetyGuidesSection !== "undefined")
                target = safetyGuidesSection;
            else if (name === "background"
                     && typeof backgroundSection !== "undefined")
                target = backgroundSection;
            else if (name === "settings"
                     && typeof settingsSection !== "undefined")
                target = settingsSection;
            if (!target) return;
            var flick = bodyScroll.contentItem;
            if (!flick) return;
            var pos  = target.mapToItem(flick.contentItem, 0, 0);
            var maxY = Math.max(0, flick.contentHeight - flick.height);
            flick.contentY = Math.max(0, Math.min(maxY, pos.y - 8));
        });
    }

    // Phase 3.H.3 — selection model. Decoupled from "active" so a
    // user can highlight rows (multi-select via Ctrl/Cmd or Shift)
    // without changing what's playing. Plain click replaces; Ctrl/
    // Cmd toggles; Shift range-selects within the same list (Media
    // or Playlists). Activation is now a separate gesture (double
    // click), in prep for drag-and-drop into playlists.
    property var    selectedItemIds: []
    property string lastSelectedItemId: ""

    function isItemSelected(id) {
        return selectedItemIds.indexOf(id) !== -1;
    }

    function selectItem(id, modifiers) {
        if (!id) return;
        const flat = WindowManager.project
                     ? WindowManager.project.flatItems : [];
        const isShift = (modifiers & Qt.ShiftModifier) !== 0;
        const isCtrl  = ((modifiers & Qt.ControlModifier) !== 0)
                     || ((modifiers & Qt.MetaModifier) !== 0);

        if (isShift && root.lastSelectedItemId) {
            // Range select within the list (media vs playlist) the
            // anchor lives in. If anchor + target span lists, fall
            // back to a single replace.
            const aItem = flat.find(function(it){ return it.id === root.lastSelectedItemId; });
            const tItem = flat.find(function(it){ return it.id === id; });
            if (aItem && tItem) {
                const aPl = (aItem.type || 0) === 4;
                const tPl = (tItem.type || 0) === 4;
                if (aPl === tPl) {
                    const list = flat.filter(function(it){
                        return ((it.type || 0) === 4) === aPl;
                    });
                    let ai = -1, ti = -1;
                    for (let i = 0; i < list.length; ++i) {
                        if (list[i].id === root.lastSelectedItemId) ai = i;
                        if (list[i].id === id) ti = i;
                    }
                    if (ai >= 0 && ti >= 0) {
                        const lo = Math.min(ai, ti);
                        const hi = Math.max(ai, ti);
                        const ns = [];
                        for (let i = lo; i <= hi; ++i) ns.push(list[i].id);
                        root.selectedItemIds = ns;
                        // anchor unchanged on shift-extend
                        return;
                    }
                }
            }
            // Fall through to replace.
            root.selectedItemIds = [id];
            root.lastSelectedItemId = id;
            return;
        }
        if (isCtrl) {
            const ix = root.selectedItemIds.indexOf(id);
            const ns = root.selectedItemIds.slice();
            if (ix >= 0) ns.splice(ix, 1);
            else         ns.push(id);
            root.selectedItemIds = ns;
            root.lastSelectedItemId = id;
            return;
        }
        // Plain click — replace selection.
        root.selectedItemIds = [id];
        root.lastSelectedItemId = id;
    }

    function clearSelection() {
        root.selectedItemIds = [];
        root.lastSelectedItemId = "";
    }

    // Ctrl/Cmd+A — select every source media row (Videos, Audio,
    // Images, Image Sequences). Playlists (type 4) and Dual Views
    // (type 5) are skipped so the result is the set you'd hand to a
    // new playlist. Anchor lands on the last selected row so a
    // following Shift-click extends sensibly.
    function selectAllMedia() {
        if (!WindowManager.project) return;
        const flat = WindowManager.project.flatItems || [];
        const ns = [];
        for (let i = 0; i < flat.length; ++i) {
            const t = flat[i].type || 0;
            if (t === 4 || t === 5) continue;   // skip playlists + dual views
            ns.push(flat[i].id);
        }
        root.selectedItemIds = ns;
        root.lastSelectedItemId = ns.length ? ns[ns.length - 1] : "";
    }

    function deleteSelectedItems() {
        if (!WindowManager.project) return;
        // One batch call — WindowManager tears down playback for any
        // active/B item, snapshots the batch for the undo-toast
        // ("Removed … · Undo"), and toasts.
        WindowManager.deleteMediaItems(root.selectedItemIds.slice());
        root.clearSelection();
    }

    // Del / Backspace — delete every selected item. Gated on the
    // timeline NOT being in dual / playlist edit mode (which owns
    // Del/Backspace for clip deletion in TimelinePanel.qml). The
    // compositorMode check excludes dual; the sourceMode check
    // excludes playlist (which is compositorMode 0 but timeline
    // sourceMode 1) — without this LeftRail's shortcut wins ambiguity
    // resolution and TimelinePanel's clip-delete never fires.
    // Application context — on Windows the player surface is a child
    // HWND that holds keyboard focus once clicked, so a default
    // (window-context) Shortcut never sees the keystroke. Promote to
    // ApplicationShortcut so Del/Backspace/F2 fire regardless of
    // which surface owns focus. Focused TextFields still get first
    // crack at the keys — Qt routes to the focused widget before
    // resolving Shortcuts — so this doesn't break in-field editing.
    //
    // Disambiguation vs. TimelinePanel's clip-delete shortcut: both
    // shortcuts have their own selection-required gates (rail needs
    // selectedItemIds.length > 0; timeline needs selectedClipId AND
    // an edited track). The earlier sourceMode !== 1 gate was too
    // aggressive — it disabled rail Del for the WHOLE app whenever a
    // playlist was active, even if no clip was selected, so creating
    // a playlist + renaming it left no way to Del any media item or
    // playlist from the bin. Selection-state mutual exclusion is
    // good enough: in normal usage the user has either a rail row OR
    // a timeline clip selected, not both.
    Shortcut {
        sequence: "Delete"
        context: Qt.ApplicationShortcut
        enabled: root.selectedItemIds.length > 0
                 && WindowManager.compositorMode === 0
                 && !root.timelineTrackEditing
        onActivated: root.deleteSelectedItems()
    }
    Shortcut {
        sequence: "Backspace"
        context: Qt.ApplicationShortcut
        enabled: root.selectedItemIds.length > 0
                 && WindowManager.compositorMode === 0
                 && !root.timelineTrackEditing
        onActivated: root.deleteSelectedItems()
    }
    // Ctrl/Cmd+A — select all source media in the bin (skips
    // playlists + dual views; see selectAllMedia). "Ctrl+A" maps to
    // Cmd+A on macOS automatically. Application context + the same
    // mode gates as Del/Backspace so it owns the key only in the
    // normal single-view bin context where playlist-building happens.
    // A focused rename/save TextField still gets Ctrl+A (select-all
    // text) first — Qt routes keys to the focused widget before
    // resolving Shortcuts.
    Shortcut {
        sequence: "Ctrl+A"
        context: Qt.ApplicationShortcut
        enabled: WindowManager.project
                 && WindowManager.compositorMode === 0
                 && !root.timelineTrackEditing
        onActivated: root.selectAllMedia()
    }
    // F2 — rename the single selected item. No-op when 0 or 2+
    // items are selected.
    Shortcut {
        sequence: "F2"
        context: Qt.ApplicationShortcut
        enabled: root.selectedItemIds.length === 1
        onActivated: {
            root.editingItemId = root.selectedItemIds[0];
        }
    }

    // Map MediaType int → section header label. ListView's
    // section.property gives us the int as a string; this is the
    // label we render above each group.
    function sectionLabel(sectionValue) {
        switch (parseInt(sectionValue)) {
        case 0: return qsTr("Videos");
        case 1: return qsTr("Audio");
        case 2: return qsTr("Images");
        case 3: return qsTr("Image Sequences");
        case 4: return qsTr("Playlists");
        case 5: return qsTr("Dual Views");
        case 6: return qsTr("Live");
        }
        return "";
    }

    FileDialog {
        id: addMediaDialog
        title: qsTr("Add Media")
        // All-types filter; addMediaFile detects the type by
        // extension and routes to the matching internal bin.
        nameFilters: [
            qsTr("All media (*.mov *.mp4 *.m4v *.mxf *.mkv *.avi *.webm "
               + "*.wav *.aif *.aiff *.mp3 *.flac *.m4a "
               + "*.png *.jpg *.jpeg *.tif *.tiff *.exr)"),
            qsTr("Video files (*.mov *.mp4 *.m4v *.mxf *.mkv *.avi *.webm)"),
            qsTr("Audio files (*.wav *.aif *.aiff *.mp3 *.flac *.m4a)"),
            qsTr("Image files (*.png *.jpg *.jpeg *.tif *.tiff *.exr)"),
            qsTr("All files (*)")
        ]
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            if (!WindowManager.project) return;
            let paths = [];
            for (let i = 0; i < selectedFiles.length; ++i) {
                const path = WindowManager.urlToOsPath(selectedFiles[i]);
                if (path) paths.push(path);
            }
            // openMediaPaths adds each + loads the last (matches the
            // prior behavior) and shows the loading spinner.
            if (paths.length > 0) WindowManager.openMediaPaths(paths);
        }
    }

    // Phase 3.H.3 — inline rename. When non-empty, the row matching
    // this id renders a TextField in place of its name label. Set
    // by the empty-playlist creation flow + the row's "Rename"
    // context menu / double-click. Cleared on Enter / Esc / focus
    // loss.
    property string editingItemId: ""
    // True iff the timeline has a track in edit mode. Bound from
    // Main.qml as `timelineTrackEditing: timelinePanel.editingA ||
    // timelinePanel.editingB`. Used to gate the rail's Del/Backspace
    // shortcuts off when the user is actively editing a playlist
    // track — in that state TimelinePanel's clip-delete shortcut
    // owns the keystroke. The earlier sourceMode-based gate was too
    // broad (disabled rail Del whenever a playlist was active, even
    // if no track was being edited), which is why creating + renaming
    // a playlist left no way to Del any media item from the bin.
    property bool timelineTrackEditing: false
    // Tracks the live text typed into the active rename TextField so
    // external callers (e.g. timeline drop) can commit it without
    // having to reach into the Repeater delegate.
    property string activeRenameText: ""
    // Reference to the currently-visible rename TextField, set by
    // the field's onVisibleChanged. The click-away overlay below
    // bounds-checks against this so taps INSIDE the field still
    // reach it (cursor placement, text selection) while taps
    // anywhere else commit + dismiss.
    property Item   activeRenameField: null

    function commitEditedItemName(id, newName) {
        if (id && newName && WindowManager.project) {
            WindowManager.project.renameMediaItem(id, newName);
        }
        root.editingItemId = "";
        // Drop focus from the now-invisible rename TextField (or
        // the QQuickTextInput inside it). Without this, focus stays
        // on the hidden editor and the next Del / Backspace gets
        // routed there instead of to the rail's Shortcut. The C++
        // event filter's stale-focus detector sometimes clears it
        // on the next keystroke, but doing it explicitly here is
        // race-free.
        WindowManager.dropTextInputFocus();
    }
    // Phase 3.H.4 — externally invoked when user drops media into a
    // playlist; commits the in-progress rename so the user doesn't
    // have to manually press Enter first.
    function commitActiveRename() {
        if (editingItemId === "") return;
        commitEditedItemName(editingItemId, activeRenameText);
    }
    // Esc on the inline rename editor — just dismisses the editor.
    // The row keeps its current name (the auto-name for a fresh
    // playlist; whatever was typed for an existing rename). Users
    // who want to bail on an unwanted playlist can right-click →
    // Delete or use the Delete shortcut.
    function cancelEditingFresh(/*id unused*/) {
        root.editingItemId = "";
        WindowManager.dropTextInputFocus();
    }

    // Phase 3.H.3 — empty-playlist creation. Used by both the
    // auto-section's "+" button (when at least one playlist already
    // exists) and the fallback Playlists header below the ListView
    // (when no playlists exist yet). Always opens the inline rename
    // editor on the new row so the user can name it immediately.
    function createEmptyPlaylistAndRename() {
        if (!WindowManager.project) return;
        const autoName = WindowManager.project.nextPlaylistAutoName();
        // Default gap dropped to 0 — user opted out of forced gaps.
        const id = WindowManager.project.createPlaylist(
            [], autoName, 24.0, 1920, 1080, 0);
        if (!id) return;
        WindowManager.project.setActiveItem(id);
        root.selectedItemIds = [id];
        root.lastSelectedItemId = id;
        root.editingItemId = id;
        // Expand the Playlists section if it was collapsed — the user
        // just made one, they almost certainly want to see it.
        if (typeof bodyColumn !== "undefined") {
            bodyColumn.playlistsExpanded = true;
        }
    }

    // Collect the on-disk paths of the current selection that are
    // eligible to live in a playlist: Videos (0), Images (2) and
    // Image Sequences (3). Audio (1) is excluded — createPlaylist
    // rejects it — as are Playlists (4) and Dual Views (5), which
    // have no single source path. Returned in flat-list (bin) order
    // so the new playlist matches the order shown in the rail.
    function playlistablePathsFromSelection() {
        if (!WindowManager.project) return [];
        const flat = WindowManager.project.flatItems || [];
        const paths = [];
        for (let i = 0; i < flat.length; ++i) {
            const it = flat[i];
            const t = it.type || 0;
            if (t !== 0 && t !== 2 && t !== 3) continue;
            if (!root.isItemSelected(it.id)) continue;
            if (!it.path) continue;
            paths.push(it.path);
        }
        return paths;
    }

    // Context-menu action: build a new playlist from the eligible
    // selection. Mirrors createEmptyPlaylistAndRename's post-create
    // behavior (activate, select, open inline rename, reveal the
    // Playlists section) so the two creation paths feel identical.
    // createPlaylist dedupes each path through addMediaFile, so the
    // already-imported media is referenced, not re-added.
    function createPlaylistFromSelection() {
        const paths = root.playlistablePathsFromSelection();
        if (paths.length === 0 || !WindowManager.project) return;
        const autoName = WindowManager.project.nextPlaylistAutoName();
        const id = WindowManager.project.createPlaylist(
            paths, autoName, 24.0, 1920, 1080, 0);
        if (!id) return;
        WindowManager.project.setActiveItem(id);
        root.selectedItemIds = [id];
        root.lastSelectedItemId = id;
        root.editingItemId = id;
        if (typeof bodyColumn !== "undefined")
            bodyColumn.playlistsExpanded = true;
    }

    // Phase 3.H.3 — playlists live in a dedicated section below
    // the Media bin. This filtered view drives the new section's
    // Repeater. flatItems already orders items by bin, so the
    // resulting array is the playlist bin's items in order.
    readonly property var playlistItems: {
        if (!WindowManager.project) return [];
        const flat = WindowManager.project.flatItems;
        if (!flat) return [];
        return flat.filter(function(it){ return (it.type || 0) === 4; });
    }

    // Saved dual views (MediaType::DualPair === 5). Drives their
    // dedicated section above Playlists. Same flatItems source as
    // playlistItems, so it repopulates automatically on project
    // load. The section only renders when this is non-empty.
    readonly property var dualViewItems: {
        if (!WindowManager.project) return [];
        const flat = WindowManager.project.flatItems;
        if (!flat) return [];
        return flat.filter(function(it){ return (it.type || 0) === 5; });
    }

    // ---- Row delegate (Phase 3.H.3) ----
    // Used by both the Media bin's ListView and the Playlists
    // section's Repeater. Renders a clickable row with inline
    // rename + drag-out-as-uri + context menu. The row's data is
    // pulled from the delegate's `modelData` (so any model whose
    // entries carry id / name / path / type works).
    //
    // `width: parent ? parent.width : 0` lets the same component
    // work as a ListView delegate (parent = contentItem) or a
    // Repeater delegate (parent = the surrounding column).
    Component {
        id: itemRowComponent
        Item {
            id: rowItem
            width: parent ? parent.width : 0
            // Unified list-row height (aesthetics pass 2) — media,
            // Dual Views and Playlists all share this delegate, so
            // one value fixes the old 22/26 drift across sections.
            height: Theme.rowHeight
            required property var modelData
            property string itemId: modelData.id
            property string itemName: modelData.name
            property string itemPath: modelData.path
            property int    itemType: modelData.type || 0
            readonly property bool isDualPair: itemType === 5
            readonly property bool isPlaylist: itemType === 4
            property bool   isActive:
                WindowManager.project
                && itemId === WindowManager.project.activeItemId
            property bool   isSelected:
                root.isItemSelected(itemId)
            readonly property bool isEditing:
                itemId === root.editingItemId

            // Row background. Priority: active (loaded) → selected →
            // hover → transparent. Active uses Theme.rowActive (a
            // brighter cobalt than Theme.selection) so a row that's
            // both selected and loaded still reads as "loaded".
            Rectangle {
                anchors.fill: parent
                color: rowItem.isActive
                       ? Theme.rowActive
                       : (rowItem.isSelected
                            ? Theme.selection
                            : (rowMa.containsMouse
                                 ? Theme.surfaceHover : "transparent"))
            }
            // Active (currently-loaded) media — accent left-rule.
            Rectangle {
                visible: rowItem.isActive
                anchors.left:   parent.left
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                width: 2
                color: Theme.accent
            }

            // Phosphor icon for the media type. Mapped from
            // MediaType enum (video=0, audio=1, image=2,
            // imageSeq=3, playlist=4, dualPair=5).
            function _typeIconName(t) {
                switch (t) {
                    case 0: return "video";
                    case 1: return "music-notes";
                    case 2: return "image";
                    case 3: return "film-strip";
                    case 4: return "playlist";
                    case 5: return "frame-corners";
                    case 6: return "broadcast";
                }
                return "file";
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin:  Theme.padding
                anchors.rightMargin: Theme.padding
                spacing: Theme.spacing

                Icon {
                    name: rowItem._typeIconName(rowItem.itemType)
                    size: Theme.iconSizeSmall
                    color: rowItem.isActive
                           ? Theme.accent : Theme.textMuted
                    Layout.alignment: Qt.AlignVCenter
                }
                // Live connection dot — only meaningful on the ACTIVE
                // live item (inactive streams have no receiver). Red =
                // live, amber = connecting/reconnecting.
                Rectangle {
                    visible: rowItem.itemType === 6 && rowItem.isActive
                             && !!WindowManager.liveDecoder
                    width: 6; height: 6; radius: 3
                    Layout.alignment: Qt.AlignVCenter
                    color: WindowManager.liveDecoder
                           && WindowManager.liveDecoder.status === 2
                           ? "#e5484d" : "#e6a23c"
                }
                Text {
                    Layout.fillWidth: true
                    visible: !rowItem.isEditing
                    text: rowItem.itemName
                    color: rowItem.isActive
                           ? Theme.textBright : Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideMiddle
                }
                TextField {
                    id: renameField
                    Layout.fillWidth: true
                    visible: rowItem.isEditing
                    text: rowItem.itemName
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textBright
                    selectByMouse: true
                    background: Rectangle {
                        color: Theme.bg
                        border.color: Theme.accent
                        border.width: 1
                        radius: 0
                    }
                    onVisibleChanged: {
                        if (visible) {
                            forceActiveFocus();
                            selectAll();
                            root.activeRenameText = text;
                            root.activeRenameField = renameField;
                        } else if (root.activeRenameField === renameField) {
                            root.activeRenameField = null;
                        }
                    }
                    onTextEdited: {
                        if (rowItem.isEditing) root.activeRenameText = text;
                    }
                    Keys.onEscapePressed: {
                        root.cancelEditingFresh(rowItem.itemId);
                    }
                    onAccepted: {
                        root.commitEditedItemName(rowItem.itemId, text);
                    }
                    onActiveFocusChanged: {
                        if (!activeFocus && rowItem.isEditing) {
                            root.commitEditedItemName(rowItem.itemId, text);
                        }
                    }
                }
            }

            // Drag source for bin → viewport / timeline DnD. Skipped
            // for playlists (no on-disk path to hand to a DropArea).
            // Phase 3.H.4 — when the dragged row is part of a multi-
            // selection, bundle every selected file's path into the
            // text/uri-list. text/uri-list is CRLF-separated per
            // RFC 2483; Qt's drop.urls splits accordingly. Falls
            // back to the single-row path when nothing is selected
            // or the dragged row isn't part of the selection.
            Item {
                id: dragGhost
                Drag.active: rowMa.drag.active && !rowItem.isPlaylist
                Drag.dragType: Drag.Automatic
                // Restrict the OS-visible action to LinkAction so a
                // drop outside any QCView window (Explorer / Finder)
                // refuses silently — neither file manager creates
                // symlinks from a drag drop, so nothing happens. The
                // intent was always intra-app DnD (bin → player or
                // timeline DropAreas, which round-trip through the
                // OS because the player is a separate native child
                // QQuickWindow that QML-internal drags can't reach
                // on Windows). The previous unrestricted form let
                // the OS show Move / Copy cursors and accept a drop
                // that the user thought was still inside the app —
                // the actual files would then be moved or copied,
                // a destructive accident. DropAreas inside QCView
                // accept any action so internal drops still work.
                Drag.supportedActions: Qt.LinkAction
                Drag.mimeData: {
                    if (rowItem.isPlaylist) return ({});
                    // Path → RFC 3986 file URI. On Windows itemPath
                    // is "C:/foo/bar"; canonical form is
                    // "file:///C:/foo/bar" (3 slashes — empty host,
                    // then absolute path starting with the drive).
                    // Two-slash form ("file://C:/foo") makes Qt's
                    // QUrl parser treat "C:" as the host and silently
                    // mangles the path on round-trip (breaks every
                    // drop target on Windows). POSIX paths already
                    // start with "/" so "file://" + path → 3 slashes
                    // naturally.
                    const _toFileUri = (p) => {
                        if (!p) return "";
                        const fwd = p.replace(/\\/g, "/");
                        if (Qt.platform.os === "windows")
                            return fwd.startsWith("/")
                                   ? "file://" + fwd
                                   : "file:///" + fwd;
                        return "file://" + fwd;
                    };
                    const flat = WindowManager.project
                                 ? WindowManager.project.flatItems : [];
                    const inSel = root.isItemSelected(rowItem.itemId)
                                  && root.selectedItemIds.length > 1;
                    if (!inSel) {
                        return ({
                            "text/uri-list": _toFileUri(rowItem.itemPath)
                        });
                    }
                    // Bundle all selected non-playlist items, in
                    // flat-list (canonical bin) order so the drop
                    // target sees the same ordering the rail shows.
                    const lines = [];
                    for (let i = 0; i < flat.length; ++i) {
                        const it = flat[i];
                        if ((it.type || 0) === 4) continue;  // skip playlists
                        if (!root.isItemSelected(it.id)) continue;
                        if (!it.path) continue;
                        lines.push(_toFileUri(it.path));
                    }
                    if (lines.length === 0) {
                        return ({
                            "text/uri-list": _toFileUri(rowItem.itemPath)
                        });
                    }
                    return ({ "text/uri-list": lines.join("\r\n") });
                }
            }
            MouseArea {
                id: rowMa
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                drag.target: rowItem.isPlaylist ? null : dragGhost
                drag.threshold: 6
                enabled: !rowItem.isEditing
                // Phase 3.H.3 — single click is selection only;
                // Ctrl/Cmd toggles into the multi-selection set;
                // Shift extends a range within the row's list.
                // Activation moved to double-click in prep for
                // drag-and-drop into playlists.
                //
                // Right-click opens the row context menu (Create
                // Playlist from Selection / Delete). It renders above
                // the player HWND via ThemedMenu's Popup.Window — the
                // earlier "menus tuck under the viewport" limitation
                // is gone. Rename = F2 still works as the keyboard path.
                onClicked: (mouse) => {
                    // Drop any lingering focus on a TextField /
                    // SpinBox text editor (e.g., the audio sync slider)
                    // so the next Del / Backspace press lands on the
                    // rail's Shortcut instead of being swallowed by
                    // the still-focused editor. Without this, clicking
                    // a row visually selects it but doesn't transfer
                    // focus, so the keystroke goes to whichever text
                    // input was last touched.
                    WindowManager.dropTextInputFocus();
                    if (mouse.button === Qt.RightButton) {
                        root.openRowMenu(rowItem.itemId);
                        return;
                    }
                    root.selectItem(rowItem.itemId, mouse.modifiers);
                }
                onDoubleClicked: (mouse) => {
                    if (rowItem.isDualPair) {
                        WindowManager.loadDualView(rowItem.itemId);
                    } else {
                        WindowManager.project.setActiveItem(rowItem.itemId);
                    }
                }
            }
        }
    }

    // Right-click context menu for bin / playlist rows. Uses
    // ThemedMenu (popupType: Popup.Window) so it z-orders ABOVE the
    // player's child HWND on Windows — the exact reason the rail had
    // no context menu before (see the rowMa note that used to live
    // here). Opened from a row's MouseArea via openRowMenu(); the
    // enabled state is snapshotted at open time so the items don't
    // re-evaluate while the popup is showing.
    property bool ctxCanCreatePlaylist: false
    function openRowMenu(rowItemId) {
        // Standard right-click selection semantics: if the clicked
        // row isn't already part of the selection, replace selection
        // with just it; if it IS already selected, leave the existing
        // multi-selection intact so the action applies to the group.
        if (rowItemId && !root.isItemSelected(rowItemId))
            root.selectItem(rowItemId, 0);
        root.ctxCanCreatePlaylist =
            root.playlistablePathsFromSelection().length > 0;
        rowContextMenu.popup();
    }
    ThemedMenu {
        id: rowContextMenu
        MenuItem {
            text: qsTr("Create Playlist from Selection")
            enabled: root.ctxCanCreatePlaylist
            onTriggered: root.createPlaylistFromSelection()
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Delete")
            enabled: root.selectedItemIds.length > 0
            onTriggered: root.deleteSelectedItems()
        }
    }

    // Click-away dismiss for the inline rename TextField. Sits on
    // top of the rail content but lets every press through
    // (propagateComposedEvents). Clicks INSIDE the active rename
    // field skip the commit so cursor placement / selection still
    // work; clicks anywhere else commit and clear the editing flag.
    // Only enabled while a rename is active so it doesn't interfere
    // with normal use of the rail.
    MouseArea {
        id: renameClickAway
        anchors.fill: parent
        z: 999
        enabled: root.editingItemId !== ""
        propagateComposedEvents: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: false
        onPressed: function(m) {
            // If the press is inside the active rename field, let
            // it through without committing — the user is placing
            // the caret or selecting text.
            const f = root.activeRenameField;
            if (f) {
                const local = f.mapFromItem(renameClickAway, m.x, m.y);
                if (local.x >= 0 && local.x <= f.width
                    && local.y >= 0 && local.y <= f.height) {
                    m.accepted = false;
                    return;
                }
            }
            root.commitActiveRename();
            // Always propagate so the click still reaches whatever
            // the user actually intended to interact with.
            m.accepted = false;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Header strip: project label + count. Stays fixed
        // at the top; only the body below scrolls when content
        // exceeds the rail height.
        Rectangle {
            Layout.fillWidth: true
            // Match the ViewportOverlay row height (36 px) so
            // the rail's collapse button lines up horizontally
            // with the A/B chips in the bar across the seam.
            Layout.preferredHeight: 36
            color: Theme.toolbar
            RowLayout {
                anchors.fill: parent
                spacing: 0

                // Chevron in a fixed Theme.gutterWidth slot. Same
                // x position whether the rail is expanded or
                // collapsed — reads as a single toggle button
                // that flips its glyph rather than a button that
                // jumps location.
                Item {
                    // Shown only while open — the re-open affordance is
                    // in the ViewportOverlay top bar when closed. Hiding
                    // it on collapse stops the arrow flashing through the
                    // gutter width during the close animation.
                    visible: !root.collapsed
                    Layout.preferredWidth: Theme.gutterWidth
                    Layout.fillHeight: true
                    // Subtle raised-gray idle fill so the toggle reads
                    // as a button at rest; hover steps up for clear
                    // click feedback. See Theme.affordanceIdle/Hover.
                    Rectangle {
                        anchors.fill: parent
                        color: railToggleMa.containsMouse
                               ? Theme.affordanceHover
                               : Theme.affordanceIdle
                    }
                    Icon {
                        anchors.centerIn: parent
                        name: root.collapsed ? "arrow-right" : "arrow-left"
                        size: Theme.iconSizeSmall
                        color: railToggleMa.containsMouse
                               ? Theme.textPrimary : Theme.textSecondary
                    }
                    MouseArea {
                        id: railToggleMa
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        onClicked: root.toggleCollapsed()
                        // Tooltip now renders over the native player
                        // surface (FlatToolTip is popupType:
                        // Popup.Window), so the rail-edge toggle can
                        // carry one without stacking behind the
                        // viewport.
                        FlatToolTip {
                            visible: railToggleMa.containsMouse
                            text: root.collapsed ? qsTr("Expand panel")
                                                 : qsTr("Collapse panel")
                        }
                    }
                }

                Icon {
                    visible: !root.collapsed
                    Layout.preferredWidth: Theme.iconSizeToolbar
                    Layout.preferredHeight: Theme.iconSizeToolbar
                    // Breathing room from the rail-collapse toggle
                    // in the gutter to its left.
                    Layout.leftMargin: Theme.padding
                    Layout.rightMargin: Theme.spacing
                    name: "folders"
                    size: Theme.iconSizeToolbar
                    color: Theme.textSecondary
                }
                Text {
                    Layout.fillWidth: true
                    visible: !root.collapsed
                    text: qsTr("Project")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeBase
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    visible: !root.collapsed
                    text: WindowManager.project
                          ? qsTr("(%1)").arg(
                              WindowManager.project.flatItems.length)
                          : ""
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                    Layout.rightMargin: Theme.padding
                }
            }
            // No bottom divider — the toolbar strip's tone against
            // the rail well below is the separation.
        }

        // ---- Scrollable body. Bin list + inspector + future panels
        // live in here. The rail header stays pinned above. ScrollView
        // is sized via Layout.fillHeight so it claims whatever space
        // remains after the header. clip prevents children from
        // bleeding past the viewport when scrolled.
        ScrollView {
            id: bodyScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.collapsed
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            // Auto-scroll-to-inspector on media load was removed —
            // forcing the user's view to jump on every load was
            // disorienting. The user can scroll manually if they
            // want to see the inspector / layer chooser; the bin
            // list stays put.

            ColumnLayout {
                id: bodyColumn
                // ScrollView's contentItem is a Flickable; binding
                // width to availableWidth lets the inner column
                // track the viewport (so children don't get clipped
                // when the vertical scrollbar appears or vanishes).
                // Setting height: implicitHeight makes Flickable's
                // contentHeight track the column's natural size so
                // the scrollbar appears once content > viewport.
                width: bodyScroll.availableWidth
                height: implicitHeight
                spacing: 0

                // User-facing collapse state for the bin section.
                // Independent from the inspector's collapse so the
                // user can hide one without the other.
                property bool binExpanded: true

                // Phase 3.H.3 — independent collapse state for the
                // Playlists section (now a top-level peer to Media).
                property bool playlistsExpanded: true

                // Independent collapse state for the saved Dual Views
                // section (shown only when at least one exists).
                property bool dualViewsExpanded: true

                // ---- Media card (aesthetics pass 2) ----
                // Bin header + list as one raised card plane on the
                // rail well, matching the right rail's InspectorCards.
                // Collapse behavior unchanged (bodyColumn.binExpanded);
                // the header uses the shared card-voice title (tiny
                // caps, muted) with the collapse caret in the gutter.
                Rectangle {
                    id: mediaCard
                    Layout.fillWidth: true
                    // Tight float (margins pass 2, user-tuned): 4px
                    // rail-edge gap — cards still read as planes
                    // without spending 8px per side.
                    Layout.leftMargin: Theme.paddingTight
                    Layout.rightMargin: Theme.paddingTight
                    Layout.topMargin: Theme.paddingTight
                    color: Theme.card
                    radius: Theme.radiusBase
                    clip: true
                    // Track the ANIMATED list height (not the
                    // binExpanded flag) so the card grows/shrinks
                    // with the list's 160 ms ease instead of
                    // snapping closed while the list is still
                    // animating under the clip.
                    implicitHeight: mediaCardHeader.height
                        + listFrame.height
                        + (listFrame.height > 0 ? Theme.padding : 0)

                    Rectangle {
                        id: mediaCardHeader
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: Theme.headerHeight
                        radius: Theme.radiusBase
                        color: binHeaderMa.containsMouse
                               ? Theme.surfaceHover : "transparent"

                        MouseArea {
                            id: binHeaderMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: bodyColumn.binExpanded =
                                       !bodyColumn.binExpanded
                        }

                        RowLayout {
                            anchors.fill: parent
                            spacing: 0
                            Item {
                                Layout.preferredWidth: Theme.gutterWidth
                                Layout.fillHeight: true
                                Icon {
                                    anchors.centerIn: parent
                                    name: bodyColumn.binExpanded
                                          ? "caret-down" : "caret-right"
                                    size: Theme.iconSizeSmall
                                    color: Theme.textSecondary
                                }
                            }
                            // Card-voice title — the film-strip icon
                            // is gone; cards are text-titled like the
                            // right rail.
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Media")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeTiny
                                font.bold: true
                                font.capitalization: Font.AllUppercase
                                font.letterSpacing: 0.8
                            }
                            Text {
                                text: "(" + mediaList.count + ")"
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeTiny
                                Layout.rightMargin: Theme.padding
                            }
                        }
                    }

        // ---- Flat media list — recessed well on the card (same
        // tone the inspector value wells use). Type glyph + name
        // rows; empty-state hint when nothing is added yet.
        // Animated height collapse via the binExpanded flag above.
        Rectangle {
            id: listFrame
            anchors.top: mediaCardHeader.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            // Tight inner inset (margins pass 2) — the outer
            // rail→card inset carries the plane look; every inner
            // pixel here comes straight out of filename width.
            anchors.leftMargin: Theme.paddingTight
            anchors.rightMargin: Theme.paddingTight
            // Grow with content — no max cap. A tall bin extends the
            // bodyColumn and the rail's own ScrollView (bodyScroll)
            // scrolls it; the inner ListView is sized to its full
            // content (interactive:false below) so there's a single
            // scroller, not a list-inside-a-scroller. The 120 px floor
            // keeps the empty-state hint readable.
            height: bodyColumn.binExpanded
                ? Math.max(120, mediaList.contentHeight + 8)
                : 0
            visible: height > 0
            Behavior on height {
                NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
            }
            // Drop highlight is rendered as a sibling overlay below
            // (dropHighlight), not as a border on this rectangle —
            // the ListView's section delegates and row backgrounds
            // are drawn on top of listFrame's fill and would eat
            // the top/bottom edges.
            color: Theme.surfaceRecess
            radius: Theme.radiusSmall
            clip: true

            DropArea {
                id: dropZone
                anchors.fill: parent
                onDropped: (drop) => {
                    if (!WindowManager.project) return;
                    if (!drop.hasUrls) return;
                    let paths = [];
                    for (let i = 0; i < drop.urls.length; ++i) {
                        // Centralized urlToOsPath handles file://,
                        // file:///, and UNC //server/share uniformly.
                        const u = WindowManager.urlToOsPath(drop.urls[i]);
                        if (u) paths.push(u);
                    }
                    // Drops into the project manager only ADD to the
                    // project (no auto-load); addMediaPaths shows the
                    // loading spinner during the add (e.g. large EXR
                    // directory scan). Drop into the viewport is the
                    // explicit "load now" gesture.
                    if (paths.length > 0) WindowManager.addMediaPaths(paths);
                    drop.accept();
                }
            }

            ListView {
                id: mediaList
                anchors.fill: parent
                anchors.margins: 2
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                // listFrame is sized to contentHeight, so the list
                // never overflows itself — disable its own flicking
                // so wheel/trackpad scrolling passes through to the
                // rail's ScrollView (bodyScroll) instead of being
                // swallowed here. Row drag-out (DnD) is unaffected.
                interactive: false
                // Playlists (type 4) and saved Dual Views (type 5)
                // each have their own dedicated section below this
                // listFrame, so the Media bin only shows Videos /
                // Audio / Images / Image Sequences.
                model: WindowManager.project
                       ? WindowManager.project.flatItems
                            .filter(function(it){
                                const t = (it.type || 0);
                                return t !== 4 && t !== 5;
                            })
                       : []
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                // Don't take active focus. The transport keys
                // (A/D held FF/RW, J/L mirrors) live on Main.qml's
                // heldKeysFocus item; if mediaList grabbed focus
                // on row click those held-keys would never fire.
                // Del / Backspace are handled via a global Shortcut
                // in Main.qml gated on selection, not Keys.onPressed.

                // Static type headers: ListView pre-sorts by
                // section.property and emits the section.delegate
                // each time the value changes between adjacent
                // items. The C++ flatItems() walks bins in the
                // canonical Videos/Audio/Images/Playlists order so
                // each type appears once, in order, with its
                // items below.
                section.property: "type"
                section.criteria: ViewSection.FullString
                // Type headers sit text-only on the recessed well —
                // the old bgAlt strip fought the card/well tones.
                section.delegate: Rectangle {
                    width: ListView.view.width
                    height: 18
                    color: "transparent"
                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.padding
                        anchors.rightMargin: Theme.padding
                        verticalAlignment: Text.AlignVCenter
                        text: root.sectionLabel(section)
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.5
                    }
                }

                delegate: itemRowComponent

                Text {
                    anchors.centerIn: parent
                    visible: mediaList.count === 0
                    text: qsTr("Drop media here to add to the project")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                    wrapMode: Text.WordWrap
                    width: parent.width - 24
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            // Drop highlight overlay. Drawn ON TOP of mediaList so
            // the accent border isn't covered by section delegates
            // or row backgrounds. Non-interactive — DropArea below
            // still receives the drag events; this is paint-only.
            Rectangle {
                anchors.fill: parent
                radius: Theme.radiusSmall
                color: "transparent"
                border.color: Theme.accent
                border.width: 2
                visible: dropZone.containsDrag
                z: 10
            }
        }
                }

        // ---- Dual Views section ----
        // Saved A/B comparisons (MediaType::DualPair === 5), moved
        // out of the Media bin into their own top-level section that
        // sits ABOVE Playlists. Unlike Playlists this section only
        // exists when at least one saved dual view is present — the
        // header + list are gated on dualViewItems.length. New views
        // are created from the viewport's Save bar, so there's no "+"
        // button here. Double-click a row to load it (handled by the
        // shared itemRowComponent's isDualPair branch).
        Rectangle {
            id: dualViewsCard
            visible: root.dualViewItems.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: Theme.paddingTight
            Layout.rightMargin: Theme.paddingTight
            Layout.topMargin: Theme.paddingTight
            color: Theme.card
            radius: Theme.radiusBase
            clip: true
            // Track the ANIMATED well height (see mediaCard) so the
            // card grows/shrinks with the collapse ease.
            implicitHeight: dualViewsHeader.height
                + dualViewsWell.height
                + (dualViewsWell.height > 0 ? Theme.padding : 0)

            Rectangle {
                id: dualViewsHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: Theme.headerHeight
                radius: Theme.radiusBase
                color: dualViewsHeadMa.containsMouse
                       ? Theme.surfaceHover : "transparent"

                MouseArea {
                    id: dualViewsHeadMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bodyColumn.dualViewsExpanded =
                               !bodyColumn.dualViewsExpanded
                }

                RowLayout {
                    anchors.fill: parent
                    spacing: 0
                    Item {
                        Layout.preferredWidth: Theme.gutterWidth
                        Layout.fillHeight: true
                        Icon {
                            anchors.centerIn: parent
                            name: bodyColumn.dualViewsExpanded
                                  ? "caret-down" : "caret-right"
                            size: Theme.iconSizeSmall
                            color: Theme.textSecondary
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Dual Views")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                    }
                    Text {
                        text: "(" + root.dualViewItems.length + ")"
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        Layout.rightMargin: Theme.padding
                    }
                }
            }

            // Dual Views items list — recessed well, same shared row
            // delegate as Media / Playlists.
            Rectangle {
                id: dualViewsWell
                anchors.top: dualViewsHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.paddingTight
                anchors.rightMargin: Theme.paddingTight
                height: bodyColumn.dualViewsExpanded
                            && root.dualViewItems.length > 0
                        ? dualViewsCol.height + 4 : 0
                Behavior on height {
                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                }
                color: Theme.surfaceRecess
                radius: Theme.radiusSmall
                clip: true

                Column {
                    id: dualViewsCol
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 2
                    Repeater {
                        model: root.dualViewItems
                        delegate: itemRowComponent
                    }
                }
            }
        }

        // ---- Playlists section (Phase 3.H.3) ----
        // Top-level peer to the Media bin. Always present; collapses
        // independently. Items are filtered from flatItems by
        // type === 4 (Playlist) so the same row delegate can render
        // them with all the Media-row affordances (rename, click-
        // to-load, context menu).
        Rectangle {
            id: playlistsCard
            Layout.fillWidth: true
            Layout.leftMargin: Theme.paddingTight
            Layout.rightMargin: Theme.paddingTight
            Layout.topMargin: Theme.paddingTight
            color: Theme.card
            radius: Theme.radiusBase
            clip: true
            // Track the ANIMATED well height (see mediaCard) so the
            // card grows/shrinks with the collapse ease. With no
            // playlists the card is just its header — the + stays
            // the always-available entry point for creating the
            // first one.
            implicitHeight: playlistsHeader.height
                + playlistsWell.height
                + (playlistsWell.height > 0 ? Theme.padding : 0)

            Rectangle {
                id: playlistsHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: Theme.headerHeight
                radius: Theme.radiusBase
                color: playlistsHeadMa.containsMouse
                       ? Theme.surfaceHover : "transparent"

                MouseArea {
                    id: playlistsHeadMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bodyColumn.playlistsExpanded =
                               !bodyColumn.playlistsExpanded
                }

                RowLayout {
                    anchors.fill: parent
                    spacing: 0
                    Item {
                        Layout.preferredWidth: Theme.gutterWidth
                        Layout.fillHeight: true
                        Icon {
                            anchors.centerIn: parent
                            name: bodyColumn.playlistsExpanded
                                  ? "caret-down" : "caret-right"
                            size: Theme.iconSizeSmall
                            color: Theme.textSecondary
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Playlists")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                    }
                    Text {
                        text: "(" + root.playlistItems.length + ")"
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        Layout.rightMargin: Theme.spacing
                    }
                    // Empty-playlist creation, always reachable. Flat
                    // icon-only button, doesn't toggle the section
                    // when clicked (its MouseArea consumes the click
                    // before the header strip's MouseArea sees it).
                    Item {
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 22
                        Layout.rightMargin: Theme.padding
                        z: 2
                        Icon {
                            anchors.centerIn: parent
                            name: "plus"
                            size: Theme.iconSizeSmall
                            color: playlistsPlusMa.containsMouse
                                   ? Theme.accent : Theme.textSecondary
                        }
                        MouseArea {
                            id: playlistsPlusMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.createEmptyPlaylistAndRename()
                            FlatToolTip {
                                visible: playlistsPlusMa.containsMouse
                                text: qsTr("New playlist")
                            }
                        }
                    }
                }
            }

            // Playlists items list — recessed well + the shared row
            // delegate. Collapses to nothing when empty so the empty
            // case doesn't steal vertical space.
            Rectangle {
                id: playlistsWell
                anchors.top: playlistsHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.paddingTight
                anchors.rightMargin: Theme.paddingTight
                height: bodyColumn.playlistsExpanded
                            && root.playlistItems.length > 0
                        ? playlistsCol.height + 4 : 0
                Behavior on height {
                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                }
                color: Theme.surfaceRecess
                radius: Theme.radiusSmall
                clip: true

                Column {
                    id: playlistsCol
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 2
                    Repeater {
                        model: root.playlistItems
                        delegate: itemRowComponent
                    }
                }
            }
        }

                // ---- Safety Guides (moved from RightRail) ------
                // Sits under Playlists. Includes the new inline
                // color picker so users can set an exact safety-
                // overlay color without going through a dialog.
                CollapsibleSection {
                    id: safetyGuidesSection
                    Layout.fillWidth: true
                    title: qsTr("Safety Guides")
                    expanded: false

                    ColumnLayout {
                        x: Theme.paddingTight
                        width: parent ? parent.width - 2 * Theme.paddingTight : 0
                        spacing: Theme.spacingLoose

                        // Shared label column (Theme.labelColWidth)
                        // so the four rows — Guide / Opacity / Width
                        // / Color — align with each other and with
                        // the KV rows everywhere else.
                        component SafetyLabel: Text {
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeTiny
                            Layout.preferredWidth: Theme.labelColWidth
                            elide: Text.ElideRight
                        }
                        // Slider readout — small recessed value chip
                        // (same tone as the inspector wells), fixed
                        // width so both sliders end at the same x.
                        component SliderReadout: Rectangle {
                            property string value: ""
                            Layout.preferredWidth: 36
                            Layout.preferredHeight: 16
                            radius: Theme.radiusSmall
                            color: Theme.surfaceRecess
                            Text {
                                anchors.centerIn: parent
                                text: parent.value
                                color: Theme.textPrimary
                                font.family: Theme.monoFamily
                                font.pixelSize: Theme.fontSizeMono
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.padding
                            spacing: Theme.spacingLoose
                            SafetyLabel { text: qsTr("Guide") }
                            FlatComboBox {
                                id: safetyPicker
                                Layout.fillWidth: true
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSmall
                                model: {
                                    const all = WindowManager.availableSafetySvgs()
                                    return [qsTr("(none)")].concat(all)
                                }
                                currentIndex: 0
                                onActivated: WindowManager.setSafetySvg(
                                    safetyPicker.currentIndex === 0
                                        ? ""
                                        : safetyPicker.textAt(safetyPicker.currentIndex))
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingLoose
                            SafetyLabel { text: qsTr("Opacity") }
                            FlatSlider {
                                id: safetyOpacitySlider
                                Layout.fillWidth: true
                                Layout.preferredHeight: 18
                                from: 0
                                to: 1
                                value: 0.7
                                onValueChanged: WindowManager.setSafetyOpacity(value)
                            }
                            SliderReadout {
                                value: safetyOpacitySlider.value.toFixed(2)
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingLoose
                            SafetyLabel { text: qsTr("Width") }
                            FlatSlider {
                                id: safetyWidthSlider
                                Layout.fillWidth: true
                                Layout.preferredHeight: 18
                                from: 1
                                to: 8
                                value: 2
                                onValueChanged: WindowManager.setSafetyLineWidth(value)
                            }
                            SliderReadout {
                                value: safetyWidthSlider.value.toFixed(0)
                            }
                        }
                        // Inline color picker — same component as
                        // the annotation tools. Sticky preference
                        // for the safety-guide color stored under
                        // safety/colorHex.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingLoose
                            SafetyLabel {
                                text: qsTr("Color")
                                Layout.alignment: Qt.AlignTop
                                topPadding: 2
                            }
                        ColorPickerInline {
                            id: safetyColorPicker
                            Layout.fillWidth: true
                            Layout.bottomMargin: Theme.padding
                            value: Qt.color(safetyColorSettings.colorHex)
                            onValueChanged: {
                                WindowManager.setSafetyColor(
                                    value.r, value.g, value.b);
                                const hh = (n) => {
                                    const v = Math.max(0, Math.min(255, Math.round(n * 255)));
                                    const s = v.toString(16);
                                    return s.length < 2 ? "0" + s : s;
                                };
                                safetyColorSettings.colorHex =
                                    "#" + hh(value.r)
                                        + hh(value.g)
                                        + hh(value.b);
                            }
                            Settings {
                                id: safetyColorSettings
                                category: "safety"
                                property string colorHex: "#ffff00"
                            }
                            Component.onCompleted: {
                                const c = Qt.color(safetyColorSettings.colorHex);
                                if (c.r >= 0) {
                                    WindowManager.setSafetyColor(c.r, c.g, c.b);
                                }
                            }
                        }
                        }
                    }
                }

                // ---- Viewport background --------------------------
                CollapsibleSection {
                    id: backgroundSection
                    Layout.fillWidth: true
                    title: qsTr("Background")
                    expanded: false

                    GridLayout {
                        x: Theme.padding
                        width: parent ? parent.width - 2 * Theme.padding : 0
                        columns: 2
                        rowSpacing: Theme.spacing
                        columnSpacing: Theme.spacing

                        Repeater {
                            model: [
                                // checker:false → solid swatch fill;
                                //  checker:true → 4×4 mini checker
                                //  using the shader's actual tile
                                //  colors for honest preview.
                                { id: 0, label: qsTr("Black"),       checker: false,
                                  c1: "#000000", c2: "#000000" },
                                { id: 1, label: qsTr("Dark Gray"),   checker: false,
                                  // #161616 = Theme.bg; keep matched
                                  // with the compositors' bgMode 1.
                                  c1: "#161616", c2: "#161616" },
                                { id: 2, label: qsTr("Dark Check"),  checker: true,
                                  // 30/255 vs 20/255, matching the
                                  // shader's sRGB output in
                                  // metal_compositor.mm:209-210.
                                  c1: "#1E1E1E", c2: "#141414" },
                                { id: 3, label: qsTr("Light Check"), checker: true,
                                  c1: "#C8C8C8", c2: "#B2B2B2" },
                            ]
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 44
                                radius: 0
                                readonly property int  v: modelData.id
                                readonly property bool isCurrent:
                                    WindowManager.backgroundMode === v
                                color: "transparent"
                                border.color: isCurrent
                                              ? Theme.accent
                                              : (bgMa.containsMouse
                                                  ? Theme.borderStrong
                                                  : Theme.border)
                                border.width: isCurrent ? 2 : 1

                                // Inner preview: solid OR 4×4 mini
                                // checker. Inset by the border.
                                Item {
                                    anchors.fill: parent
                                    anchors.margins: parent.isCurrent ? 2 : 1
                                    clip: true

                                    // Solid background fill (every
                                    // tile, even checker — it's the
                                    // "primary" cell color).
                                    Rectangle {
                                        anchors.fill: parent
                                        color: modelData.c1
                                    }

                                    // Square-cell checker overlay.
                                    // Cell size = tile height / 4
                                    // (so we always get 4 rows);
                                    // columns scale with the tile
                                    // width so cells stay square
                                    // regardless of how wide the
                                    // 2-col grid stretches each
                                    // tile.
                                    Grid {
                                        id: checkerGrid
                                        visible: modelData.checker
                                        anchors.fill: parent
                                        readonly property int cellSize:
                                            Math.max(4, Math.floor(parent.height / 4))
                                        // Capture c2 here at the
                                        // outer Repeater's scope —
                                        // inside the inner Repeater
                                        // below, `modelData` shadows
                                        // to the inner integer index
                                        // and `modelData.c2` would
                                        // resolve to undefined → the
                                        // color binding falls back
                                        // to white.
                                        readonly property color cellColor: modelData.c2
                                        rows:    4
                                        columns: Math.max(
                                            2,
                                            Math.ceil(parent.width / cellSize))
                                        Repeater {
                                            model: checkerGrid.rows * checkerGrid.columns
                                            delegate: Rectangle {
                                                width:  checkerGrid.cellSize
                                                height: checkerGrid.cellSize
                                                color: ((index % checkerGrid.columns)
                                                        + Math.floor(index / checkerGrid.columns))
                                                       % 2 === 1
                                                       ? checkerGrid.cellColor
                                                       : "transparent"
                                            }
                                        }
                                    }
                                }

                                MouseArea {
                                    id: bgMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: WindowManager.backgroundMode = parent.v
                                }
                            }
                        }
                    }
                }


                // ---- Settings (Phase 7.6) ------------------------
                // Performance + UI tunables. Three apply on next
                // load (image-seq threads, image-seq cache window,
                // video decode threads); timelineHoverThumbs is
                // live. Per-row "↺" reverts that one to default;
                // bottom "Reset all preferences" wipes the entire
                // QSettings store and prompts a relaunch.
                CollapsibleSection {
                    id: settingsSection
                    Layout.fillWidth: true
                    title: qsTr("Settings")
                    expanded: false

                    // Defaults — match the constructors / Settings spec.
                    readonly property int  kImageSeqThreadsDefault: 16
                    readonly property int  kImageSeqAheadDefault:   72
                    readonly property int  kFFmpegThreadsDefault:    0
                    readonly property bool kHoverThumbsDefault:   true
                    readonly property bool kWaveformsDefault:     true
                    readonly property bool kHwDecodeEnabledDefault: true
                    readonly property bool kHwScrubInterDefault:  false
                    readonly property int  kScrubCacheMBDefault:    512
                    // Screenshot export format tokens, dropdown-aligned.
                    readonly property var    kScreenshotFormats: ["png", "jpeg", "tiff"]
                    readonly property string kScreenshotFormatDefault: "png"

                    Settings {
                        id: perfSettings
                        category: "performance"
                        property int imageSeqThreads:
                            settingsSection.kImageSeqThreadsDefault
                        property int imageSeqReadAhead:
                            settingsSection.kImageSeqAheadDefault
                        property int ffmpegThreads:
                            settingsSection.kFFmpegThreadsDefault
                        // Windows-only toggle (defaults ON). When OFF,
                        // every VideoDecoder / ScrubDecoder /
                        // DualVideoDecoder skips hwaccel attach entirely
                        // and uses software decode. Escape hatch for
                        // mini-PCs with broken GPU drivers. No effect
                        // on macOS (VideoToolbox always used). Reads
                        // picked up on next file load.
                        property bool hardwareDecodeEnabled:
                            settingsSection.kHwDecodeEnabledDefault
                        // Experimental: lift the intra-only hwaccel gate so
                        // h264/h265 (b-frame) scrub uses GPU decode too. The
                        // GOP cache makes this viable; default OFF until the
                        // VideoToolbox async-reorder path is proven. Reads
                        // picked up on next file load.
                        property bool hwScrubInterCodecs:
                            settingsSection.kHwScrubInterDefault
                        // Per-decoder scrub GOP-cache budget in MB (each dual
                        // side has its own). Applies on next file load.
                        property int scrubCacheMB:
                            settingsSection.kScrubCacheMBDefault
                    }
                    Settings {
                        id: uiSettings
                        category: "ui"
                        property bool timelineHoverThumbs:
                            settingsSection.kHoverThumbsDefault
                    }

                    // Reset confirmation dialog. The danger-zone
                    // button at the bottom triggers this; on accept
                    // we wipe QSettings and quit so the user can
                    // relaunch into a known-clean state.
                    MessageDialog {
                        id: resetAllConfirm
                        title: qsTr("Reset all preferences?")
                        text: qsTr(
                            "Every preference (recents, layout, "
                            + "settings, shortcuts) will be erased "
                            + "and the app will quit. Relaunch to "
                            + "continue.")
                        buttons: MessageDialog.Yes | MessageDialog.Cancel
                        onAccepted: {
                            const s = perfSettings;
                            // Clear via the QSettings primitive on
                            // each known group (Settings element
                            // doesn't expose a "clear all").
                            perfSettings.imageSeqThreads =
                                settingsSection.kImageSeqThreadsDefault;
                            perfSettings.imageSeqReadAhead =
                                settingsSection.kImageSeqAheadDefault;
                            perfSettings.ffmpegThreads =
                                settingsSection.kFFmpegThreadsDefault;
                            perfSettings.hardwareDecodeEnabled =
                                settingsSection.kHwDecodeEnabledDefault;
                            perfSettings.hwScrubInterCodecs =
                                settingsSection.kHwScrubInterDefault;
                            perfSettings.scrubCacheMB =
                                settingsSection.kScrubCacheMBDefault;
                            WindowManager.timelineHoverThumbsEnabled =
                                settingsSection.kHoverThumbsDefault;
                            WindowManager.timelineWaveformsEnabled =
                                settingsSection.kWaveformsDefault;
                            WindowManager.scrubAudioMuted = false;
                            // Drops window/* frame keys AND suppresses
                            // the aboutToQuit geometry flush, which
                            // would otherwise write them right back.
                            WindowManager.clearSavedWindowGeometry();
                            Qt.quit();
                        }
                    }

                    // Inline reusable revert button: small flat
                    // arrow-counter-clockwise icon that goes accent
                    // on hover. Declutter (aesthetics pass 2): only
                    // shown when the row's value differs from its
                    // default (`dirty`) — but the 22 px slot is
                    // always reserved so the control column stays
                    // aligned across clean and dirty rows.
                    component RevertBtn: Item {
                        property bool dirty: true
                        signal clicked
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 22
                        opacity: dirty ? 1 : 0
                        Icon {
                            anchors.centerIn: parent
                            name: "arrow-counter-clockwise"
                            size: Theme.iconSizeSmall
                            color: revertMa.containsMouse
                                   ? Theme.accent : Theme.textMuted
                        }
                        MouseArea {
                            id: revertMa
                            anchors.fill: parent
                            enabled: parent.dirty
                            cursorShape: Qt.PointingHandCursor
                            hoverEnabled: true
                            onClicked: parent.clicked()
                        }
                    }

                    // Group heading — caps voice, no hairline rule
                    // (last holdout of the old divider vocabulary;
                    // spacing does the grouping now, matching the
                    // Keyboard Shortcuts categories).
                    component GroupHeader: Text {
                        property string label: ""
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.padding
                        text: label
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        font.bold: true
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.5
                    }

                    // Fixed-width control slot for switch rows —
                    // right-aligns the switch to the same edge the
                    // spinboxes / combo end at, so the card reads as
                    // three columns: label / control / revert.
                    component SlotSwitch: Item {
                        property alias checked: slotSw.checked
                        signal toggled()
                        Layout.preferredWidth: 110
                        Layout.preferredHeight: 22
                        FlatSwitch {
                            id: slotSw
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            onToggled: parent.toggled()
                        }
                    }

                    // Row label with optional hover help — replaces
                    // the old always-visible HelpText paragraphs
                    // that made the card read as a wall of text.
                    // The explanations still exist, one hover away.
                    component RowLabel: Text {
                        id: rowLabelRoot
                        property string help: ""
                        Layout.fillWidth: true
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSmall
                        elide: Text.ElideRight
                        MouseArea {
                            id: rowLabelMa
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                            FlatToolTip {
                                visible: rowLabelMa.containsMouse
                                         && rowLabelRoot.help.length > 0
                                text: rowLabelRoot.help
                            }
                        }
                    }

                    ColumnLayout {
                        x: Theme.paddingTight
                        width: parent ? parent.width - 2 * Theme.paddingTight : 0
                        spacing: Theme.spacingLoose

                        GroupHeader { label: qsTr("Performance") }

                        // Image-seq decode threads.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Image-seq decode threads")
                                    help: qsTr("Applies on next sequence load.")
                                }
                                FlatSpinBox {
                                    from: 1
                                    to: 64
                                    value: perfSettings.imageSeqThreads
                                    onValueModified: perfSettings.imageSeqThreads = value
                                    Layout.preferredWidth: 110
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSizeMono
                                }
                                RevertBtn {
                                    dirty: perfSettings.imageSeqThreads
                                           !== settingsSection.kImageSeqThreadsDefault
                                    onClicked: perfSettings.imageSeqThreads =
                                                  settingsSection.kImageSeqThreadsDefault
                                }
                            }
                        }

                        // Image-seq cache window.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Image-seq cache window")
                                    help: qsTr("Frames cached ahead of the playhead. "
                                             + "Lower = less RAM.")
                                }
                                FlatSpinBox {
                                    from: 24
                                    to: 240
                                    stepSize: 6
                                    value: perfSettings.imageSeqReadAhead
                                    onValueModified: perfSettings.imageSeqReadAhead = value
                                    Layout.preferredWidth: 110
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSizeMono
                                }
                                RevertBtn {
                                    dirty: perfSettings.imageSeqReadAhead
                                           !== settingsSection.kImageSeqAheadDefault
                                    onClicked: perfSettings.imageSeqReadAhead =
                                                  settingsSection.kImageSeqAheadDefault
                                }
                            }
                        }

                        // Video (FFmpeg) decode threads.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Video decode threads")
                                    help: qsTr("0 = Auto. Single-video playback only; "
                                             + "dual and scrub stay on Auto. Applies "
                                             + "on next video load.")
                                }
                                FlatSpinBox {
                                    from: 0
                                    to: 32
                                    value: perfSettings.ffmpegThreads
                                    onValueModified: perfSettings.ffmpegThreads = value
                                    Layout.preferredWidth: 110
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSizeMono
                                    textFromValue: function(value) {
                                        return value === 0
                                            ? qsTr("Auto")
                                            : value.toString();
                                    }
                                    valueFromText: function(text) {
                                        if (text === qsTr("Auto") || text === "0") return 0;
                                        const n = parseInt(text);
                                        return isNaN(n) ? 0 : n;
                                    }
                                }
                                RevertBtn {
                                    dirty: perfSettings.ffmpegThreads
                                           !== settingsSection.kFFmpegThreadsDefault
                                    onClicked: perfSettings.ffmpegThreads =
                                                  settingsSection.kFFmpegThreadsDefault
                                }
                            }
                        }

                        // Windows-only toggle (defaults ON). Turning
                        // OFF forces every video decoder (single, dual,
                        // scrub, playlist) onto the software path.
                        // macOS uses VideoToolbox unconditionally;
                        // hiding the row entirely there avoids
                        // surfacing an option that does nothing.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            visible: Qt.platform.os === "windows"
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Enable hardware decoding")
                                    help: qsTr("GPU decode (Vulkan/D3D11VA) when "
                                             + "available. Turn off if playback "
                                             + "misbehaves — forces software decode "
                                             + "everywhere. Applies on next file load.")
                                }
                                SlotSwitch {
                                    checked: perfSettings.hardwareDecodeEnabled
                                    onToggled: {
                                        perfSettings.hardwareDecodeEnabled = checked;
                                    }
                                }
                                RevertBtn {
                                    dirty: perfSettings.hardwareDecodeEnabled
                                           !== settingsSection.kHwDecodeEnabledDefault
                                    onClicked: {
                                        perfSettings.hardwareDecodeEnabled =
                                            settingsSection.kHwDecodeEnabledDefault;
                                    }
                                }
                            }
                        }

                        // Scrub GOP-cache budget (MB per decoder).
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Scrub cache size (MB)")
                                    help: qsTr("Decoded frames kept near the playhead "
                                             + "for instant backward scrub. Higher = "
                                             + "smoother, more RAM/VRAM. Per decoder — "
                                             + "dual uses 2x. Applies on next file load.")
                                }
                                FlatSpinBox {
                                    from: 128
                                    to: 4096
                                    stepSize: 128
                                    value: perfSettings.scrubCacheMB
                                    onValueModified: perfSettings.scrubCacheMB = value
                                    Layout.preferredWidth: 110
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSizeMono
                                }
                                RevertBtn {
                                    dirty: perfSettings.scrubCacheMB
                                           !== settingsSection.kScrubCacheMBDefault
                                    onClicked: perfSettings.scrubCacheMB =
                                                  settingsSection.kScrubCacheMBDefault
                                }
                            }
                        }

                        // Experimental: hardware scrub for h264/h265.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Hardware scrub for h264/h265 (experimental)")
                                    help: qsTr("GPU decode while scrubbing h264/h265, "
                                             + "not just all-intra (ProRes). "
                                             + "Experimental — turn off if scrubbed "
                                             + "frames look wrong. Applies on next "
                                             + "file load.")
                                }
                                SlotSwitch {
                                    checked: perfSettings.hwScrubInterCodecs
                                    onToggled: {
                                        perfSettings.hwScrubInterCodecs = checked;
                                    }
                                }
                                RevertBtn {
                                    dirty: perfSettings.hwScrubInterCodecs
                                           !== settingsSection.kHwScrubInterDefault
                                    onClicked: {
                                        perfSettings.hwScrubInterCodecs =
                                            settingsSection.kHwScrubInterDefault;
                                    }
                                }
                            }
                        }

                        GroupHeader { label: qsTr("Interface") }

                        // Always open in minimal mode — overrides the
                        // remembered rail/panel layout on next launch.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Always open in minimal mode")
                                    help: qsTr("Launch with the rails collapsed and "
                                             + "the color panel hidden, ignoring the "
                                             + "last-used layout. Applies on next "
                                             + "launch.")
                                }
                                SlotSwitch {
                                    checked: WindowManager.alwaysOpenMinimal
                                    onToggled: {
                                        WindowManager.alwaysOpenMinimal = checked;
                                    }
                                }
                                RevertBtn {
                                    dirty: WindowManager.alwaysOpenMinimal
                                    onClicked: {
                                        WindowManager.alwaysOpenMinimal = false;
                                    }
                                }
                            }
                        }

                        // Timeline hover thumbnails.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel { text: qsTr("Timeline hover thumbnails") }
                                SlotSwitch {
                                    // Bind to the WM Q_PROPERTY so the
                                    // transport-row toggle and this
                                    // switch stay in sync live.
                                    checked: WindowManager.timelineHoverThumbsEnabled
                                    onToggled: {
                                        WindowManager.timelineHoverThumbsEnabled = checked;
                                    }
                                }
                                RevertBtn {
                                    dirty: WindowManager.timelineHoverThumbsEnabled
                                           !== settingsSection.kHoverThumbsDefault
                                    onClicked: {
                                        WindowManager.timelineHoverThumbsEnabled =
                                            settingsSection.kHoverThumbsDefault;
                                    }
                                }
                            }
                        }

                        // Timeline audio waveforms (2026-07-08
                        // experiment). Gate binds live — off hides
                        // the strips AND stops probing immediately.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Timeline audio waveforms")
                                    help: qsTr("Draw an audio waveform over timeline clips. Peaks decode lazily in the background for the visible range and pause during playback.")
                                }
                                SlotSwitch {
                                    checked: WindowManager.timelineWaveformsEnabled
                                    onToggled: {
                                        WindowManager.timelineWaveformsEnabled = checked;
                                    }
                                }
                                RevertBtn {
                                    dirty: WindowManager.timelineWaveformsEnabled
                                           !== settingsSection.kWaveformsDefault
                                    onClicked: {
                                        WindowManager.timelineWaveformsEnabled =
                                            settingsSection.kWaveformsDefault;
                                    }
                                }
                            }
                        }

                        // Screenshot export format. Affects the
                        // "save screenshot to Desktop" export only;
                        // note thumbnails stay PNG.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Screenshot format")
                                    help: qsTr("Format for Desktop screenshots. JPEG "
                                             + "is lossy; PNG/TIFF lossless. Notes "
                                             + "always use PNG.")
                                }
                                FlatComboBox {
                                    id: screenshotFormatPicker
                                    Layout.preferredWidth: 110
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSmall
                                    model: ["PNG", "JPEG", "TIFF"]
                                    currentIndex: Math.max(0,
                                        settingsSection.kScreenshotFormats.indexOf(
                                            WindowManager.screenshotFormat))
                                    onActivated: WindowManager.screenshotFormat =
                                        settingsSection.kScreenshotFormats[currentIndex]
                                }
                                RevertBtn {
                                    dirty: WindowManager.screenshotFormat
                                           !== settingsSection.kScreenshotFormatDefault
                                    onClicked: WindowManager.screenshotFormat =
                                        settingsSection.kScreenshotFormatDefault
                                }
                            }
                        }

                        // Software updates (macOS / Sparkle only).
                        GroupHeader {
                            label: qsTr("Updates")
                            visible: Qt.platform.os === "osx"
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            visible: Qt.platform.os === "osx"
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Automatically check for updates")
                                    help: qsTr("Checks daily for updates, sending only "
                                             + "your IP and app/OS version. Update "
                                             + "manually anytime from the About menu.")
                                }
                                SlotSwitch {
                                    // Bound directly to Sparkle's own
                                    // persisted setting (single source of
                                    // truth — no separate QSettings copy).
                                    checked: WindowManager.autoUpdateChecks
                                    onToggled: {
                                        WindowManager.autoUpdateChecks = checked;
                                    }
                                }
                                // No revert button on this row — a
                                // filler keeps the control column
                                // aligned with the rows above.
                                Item { Layout.preferredWidth: 22 }
                            }
                        }

                        GroupHeader { label: qsTr("Audio") }

                        // A/V sync offset — separate values for
                        // single-flow and dual-flow because dual's
                        // composite-to-present latency is meaningfully
                        // deeper (per-side decode + dual canvas + OCIO
                        // over the larger intermediate). Each row
                        // persists + applies live to its own path;
                        // changing one doesn't disturb the other.
                        // Per-OS defaults seeded from C++.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("A/V sync (Single)")
                                    help: qsTr("Positive values delay audio to offset "
                                             + "pipeline/display lag. Tune by ear; "
                                             + "applies on the next seek or play.")
                                }
                                FlatSpinBox {
                                    from: -100
                                    to: 100
                                    value: WindowManager.audioSyncOffsetMs
                                    onValueModified:
                                        WindowManager.audioSyncOffsetMs = value
                                    Layout.preferredWidth: 110
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSizeMono
                                    textFromValue: function(value) {
                                        return value + " ms";
                                    }
                                    valueFromText: function(text) {
                                        const n = parseInt(text);
                                        return isNaN(n) ? 0 : n;
                                    }
                                }
                                RevertBtn {
                                    dirty: WindowManager.audioSyncOffsetMs
                                           !== WindowManager.audioSyncOffsetMsDefault
                                    onClicked:
                                        WindowManager.audioSyncOffsetMs =
                                            WindowManager.audioSyncOffsetMsDefault
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("A/V sync (Dual)")
                                    help: qsTr("Positive values delay audio to offset "
                                             + "pipeline/display lag. Dual's latency "
                                             + "runs deeper than single. Tune by ear; "
                                             + "applies on the next seek or play.")
                                }
                                FlatSpinBox {
                                    from: -100
                                    to: 100
                                    value: WindowManager.dualAudioSyncOffsetMs
                                    onValueModified:
                                        WindowManager.dualAudioSyncOffsetMs = value
                                    Layout.preferredWidth: 110
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSizeMono
                                    textFromValue: function(value) {
                                        return value + " ms";
                                    }
                                    valueFromText: function(text) {
                                        const n = parseInt(text);
                                        return isNaN(n) ? 0 : n;
                                    }
                                }
                                RevertBtn {
                                    dirty: WindowManager.dualAudioSyncOffsetMs
                                           !== WindowManager.dualAudioSyncOffsetMsDefault
                                    onClicked:
                                        WindowManager.dualAudioSyncOffsetMs =
                                            WindowManager.dualAudioSyncOffsetMsDefault
                                }
                            }
                        }

                        // Scrub-audio mute. Applies live (mid-gesture
                        // included) — the shuttle engines keep running
                        // and just idle silent.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                RowLabel {
                                    text: qsTr("Mute scrub audio")
                                    help: qsTr("Silence the audio feedback while "
                                             + "dragging the timeline or holding "
                                             + "fast-forward/rewind. Scrubbing "
                                             + "itself is unaffected.")
                                }
                                SlotSwitch {
                                    checked: WindowManager.scrubAudioMuted
                                    onToggled: {
                                        WindowManager.scrubAudioMuted = checked;
                                    }
                                }
                                RevertBtn {
                                    dirty: WindowManager.scrubAudioMuted
                                    onClicked: {
                                        WindowManager.scrubAudioMuted = false;
                                    }
                                }
                            }
                        }

                        // Danger zone — bottom of the section.
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            Layout.topMargin: Theme.paddingLoose
                            color: Theme.error
                            opacity: 0.4
                        }
                        // Sized to content, not full width — a
                        // full-bleed red button was more alarm than
                        // a preferences card needs.
                        FlatButton {
                            Layout.alignment: Qt.AlignLeft
                            Layout.topMargin: Theme.spacing
                            Layout.bottomMargin: Theme.padding
                            iconName: "trash"
                            iconSize: Theme.iconSizeSmall
                            text: qsTr("Reset all preferences…")
                            variant: "danger"
                            Layout.preferredHeight: 24
                            onClicked: resetAllConfirm.open()
                        }
                    }
                }
            }
        }
    }
}
