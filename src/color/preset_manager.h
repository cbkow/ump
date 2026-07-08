// PresetManager — Phase 2.5e.
//
// Owns the application's color-preset list (built-in + user) and
// applies a preset to OCIOConfigManager.
//
// Per Guide 05 D15: presets are stored as **slot-state JSON**, not
// the old app's node-graph JSON. A preset captures the [config,
// input, look, scene_lut, output, view, display_lut] tuple — same
// six slot states the slot-machine UI presents — plus a name and
// timestamps. Built-ins ship hardcoded; user presets persist to
// QStandardPaths::AppDataLocation/color-presets.json.
//
// Phase 2.5e.1 ships built-ins + apply only. Save / Save as,
// migration from the old .qcvnode format, and the Manage modal
// land in 2.5e.2 / 2.5e.3 / 2.5e.4.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace qcv {

class OCIOConfigManager;

class PresetManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList availablePresets READ availablePresets NOTIFY presetsChanged)
    // QVariantList of { name, section } pairs for the Preset reel.
    // ListView's section.property uses "section" to insert sticky
    // group headers ("Blender 5.1" etc.) — keeps preset names short
    // and readable in the narrow column. See Phase 2.5e.1.5 notes
    // in preset_manager.cpp.
    Q_PROPERTY(QVariantList availablePresetEntries READ availablePresetEntries NOTIFY presetsChanged)
    Q_PROPERTY(QString activePresetName READ activePresetName NOTIFY activePresetChanged)
    // True when the user's current slot tuple no longer matches the
    // active preset's stored slots (any reel edit after applying a
    // preset). Drives the italicized "(modified)" label on the
    // preset bar — see Guide 05 §2 "Editing state".
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)

public:
    // A single preset's slot tuple. Empty strings = slot unset
    // (passthrough for that stage per Guide 05 D6).
    //
    // Kind drives the badge + platform icon + enabled state in the
    // preset reel — see Phase 2.5 polish:
    //   Universal  — works in any HDR mode / any platform (passthrough).
    //   SdrSrgb    — sRGB / Rec.709 / BT.1886 SDR outputs. Enabled
    //                only in SDR sRGB swapchain mode (both OSes).
    //   SdrP3      — Display P3 SDR outputs. Enabled only in SDR
    //                Display P3 swapchain mode (both OSes).
    //   HdrEdrSrgb — linear-light EDR with sRGB / Rec.709 primaries.
    //                Cross-platform: usable in macOS EDR modes AND
    //                in Windows scRGB (extended-linear sRGB) mode,
    //                since both swapchains accept linear sRGB values.
    //   HdrEdrP3   — linear-light EDR with Display P3 primaries.
    //                macOS-only (Windows has no P3-tagged extended
    //                linear swapchain).
    //   HdrPq      — Rec.2100-PQ / HDR10 outputs. Enabled in HDR10
    //                PQ mode on Windows / Linux.
    struct Preset {
        enum Kind {
            Universal  = 0,
            SdrSrgb    = 1,
            HdrEdrSrgb = 2,
            HdrPq      = 3,
            HdrEdrP3   = 4,
            SdrP3      = 5,
        };

        QString name;
        QString section;          // grouping header in the Preset reel
        QString configName;       // matches OCIOConfigManager::availableConfigs() entry
        QString input;
        QString look;
        QString sceneLutPath;
        QString output;           // display name in OCIO terminology
        QString view;
        QString displayLutPath;
        bool    builtIn = false;
        Kind    kind    = SdrSrgb;
    };

    // Constructor wires to the OCIOConfigManager that the manager
    // applies presets to. The OCIO manager must outlive this manager;
    // typically both are owned by WindowManager.
    explicit PresetManager(OCIOConfigManager *ocio, QObject *parent = nullptr);
    ~PresetManager() override;

    QStringList   availablePresets() const;
    QVariantList  availablePresetEntries() const;
    QString       activePresetName() const { return m_activePresetName; }
    bool          modified() const { return m_modified; }
    Q_INVOKABLE bool applyPreset(const QString &name);

    // Step ◀/▶ through the available list. Wraps around at ends.
    Q_INVOKABLE void stepPreset(int delta);

    // Phase 2.5e.2 — save + persistence.
    //
    // Built-ins are read-only; saveCurrent() against a built-in
    // returns false (UI should call saveAs instead). saveAs creates
    // a new "Custom" user preset; if the name collides with an
    // existing built-in or user preset the call is rejected (Manage
    // modal in .e.4 will offer rename/delete to resolve).
    Q_INVOKABLE bool activeIsBuiltIn() const;
    Q_INVOKABLE bool saveCurrent();
    Q_INVOKABLE bool saveAs(const QString &name);
    // Delete the active preset. Rejected if there is no active
    // preset or if the active preset is built-in (built-ins are
    // read-only). On success, clears the active selection and
    // persists the user-presets file. Returns true on success.
    Q_INVOKABLE bool deleteCurrent();
    // Undo the last successful deleteCurrent (undo-toast). Re-inserts
    // the snapshotted preset at its old list position, restores it as
    // the active preset (the delete didn't touch the OCIO chain, so
    // modified-state is recomputed rather than the preset re-applied),
    // and persists. Returns the restored preset's name; empty when
    // there is nothing to restore, the name has been reused since
    // (a re-save under the same name wins), or the disk write fails
    // (snapshot kept — the toast action can be retried).
    Q_INVOKABLE QString undoDeleteLast();

signals:
    void presetsChanged();
    void activePresetChanged();
    void modifiedChanged();

private slots:
    void onActiveChainChanged();
    void onConfigChanged();

private:
    void loadBuiltIns();
    const Preset *findByName(const QString &name) const;
    Preset *findByNameMutable(const QString &name);
    bool currentMatchesPreset(const Preset &p) const;
    Preset captureCurrentAsPreset(const QString &name) const;
    QString userPresetsFilePath() const;
    void loadUserPresetsFromDisk();
    bool writeUserPresetsToDisk() const;

    OCIOConfigManager *m_ocio = nullptr;
    QList<Preset>      m_presets;
    QString            m_activePresetName;
    bool               m_modified = false;
    // One-slot undo buffer for deleteCurrent (each delete overwrites
    // the last). Index is the preset's old position in m_presets.
    Preset             m_lastDeleted;
    int                m_lastDeletedIndex = -1;
    bool               m_hasLastDeleted   = false;
    // Set true while applyPreset runs so the chain-change signals
    // fired by each setActiveX call don't trip the modified-detection
    // logic. Manual reel edits go through the same setters but with
    // m_applying false → modified state updates correctly.
    bool               m_applying = false;
};

} // namespace qcv
