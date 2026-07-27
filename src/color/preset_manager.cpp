#include "preset_manager.h"

#include "ocio_config_manager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtLogging>

namespace qcv {

PresetManager::PresetManager(OCIOConfigManager *ocio, QObject *parent)
    : QObject(parent), m_ocio(ocio)
{
    loadBuiltIns();
    loadUserPresetsFromDisk();

    // Track invalidation: any chain edit that didn't come from
    // applyPreset itself toggles us into "modified" state; switching
    // configs clears the active preset entirely (the slots reset to
    // the new config's defaults so the preset name no longer
    // describes what's loaded).
    if (m_ocio) {
        connect(m_ocio, &OCIOConfigManager::activeChainChanged,
                this, &PresetManager::onActiveChainChanged);
        connect(m_ocio, &OCIOConfigManager::configChanged,
                this, &PresetManager::onConfigChanged);
    }
}

PresetManager::~PresetManager() = default;

void PresetManager::loadBuiltIns()
{
    // Built-ins ported from the old app (src/app/color_presets.cpp).
    // Phase 2.5e.1.5 pivot: the [Config] prefix on names was hard
    // to read in the narrow Preset reel. Now the Preset struct
    // carries a `section` field; the QML reel uses ListView's
    // section.property to render group headers ("Blender 5.1",
    // "ACES 2.0", etc.) and the names themselves are short and
    // descriptive.
    //
    // HDR presets (entries with EDR / PQ / HDR in the name) load
    // their chains fine but render clamped until Phase 2.6 ships
    // extended-linear swapchain support.

    auto P = [](const QString &name, const QString &section,
                const QString &cfg, const QString &input,
                const QString &look, const QString &output, const QString &view,
                Preset::Kind kind = Preset::SdrSrgb) {
        return Preset{ name, section, cfg, input, look,
                       /*sceneLutPath*/ {}, output, view,
                       /*displayLutPath*/ {}, /*builtIn*/ true, kind };
    };

    const QString secBuiltIn   = QStringLiteral("Built-in");
    const QString secBlender52 = QStringLiteral("Blender 5.2");
    const QString secBlender   = QStringLiteral("Blender 5.1");
    const QString secAces2     = QStringLiteral("ACES 2.0");
    const QString secAces1     = QStringLiteral("ACES 1.3");

    m_presets = {
        // --- Passthrough ---
        Preset{ QStringLiteral("None (Passthrough)"), secBuiltIn,
                {}, {}, {}, {}, {}, {}, {}, true, Preset::Universal },

        // --- Blender 5.2 SDR ---
        // Same literals as the 5.1 section below — every referenced
        // colorspace/display/view name is unchanged between the two
        // configs. 5.1 sections stay because user presets store raw
        // configName strings with no migration path.
        P(QStringLiteral("Rec.709 → sRGB Standard"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Rec.1886"),
          {}, QStringLiteral("sRGB"), QStringLiteral("Standard")),

        P(QStringLiteral("Linear Rec.709 → sRGB Standard"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("sRGB"), QStringLiteral("Standard")),

        P(QStringLiteral("Linear Rec.709 → sRGB AgX"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("sRGB"), QStringLiteral("AgX")),

        P(QStringLiteral("Linear Rec.709 → Display P3"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Display P3"), QStringLiteral("Standard"),
          Preset::SdrP3),

        // --- Blender 5.2 HDR (linear-light EDR) ---
        // sRGB-primaries variant works on macOS EDR + Windows scRGB.
        P(QStringLiteral("Rec.709 → EDR sRGB 1000 nits"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Linear sRGB EDR"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits"),
          Preset::HdrEdrSrgb),

        // P3-primaries variant — macOS only.
        P(QStringLiteral("Rec.709 → EDR P3 1000 nits"), secBlender52,
          QStringLiteral("Blender 5.2"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Linear P3 EDR"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits"),
          Preset::HdrEdrP3),

        // --- Blender 5.1 SDR ---
        P(QStringLiteral("Rec.709 → sRGB Standard"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Rec.1886"),
          {}, QStringLiteral("sRGB"), QStringLiteral("Standard")),

        P(QStringLiteral("Linear Rec.709 → sRGB Standard"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("sRGB"), QStringLiteral("Standard")),

        P(QStringLiteral("Linear Rec.709 → sRGB AgX"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("sRGB"), QStringLiteral("AgX")),

        P(QStringLiteral("Linear Rec.709 → Display P3"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Display P3"), QStringLiteral("Standard"),
          Preset::SdrP3),

        // --- Blender 5.1 HDR (linear-light EDR) ---
        // sRGB-primaries variant works on macOS EDR + Windows scRGB.
        P(QStringLiteral("Rec.709 → EDR sRGB 1000 nits"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Linear sRGB EDR"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits"),
          Preset::HdrEdrSrgb),

        // P3-primaries variant — macOS only.
        P(QStringLiteral("Rec.709 → EDR P3 1000 nits"), secBlender,
          QStringLiteral("Blender 5.1"), QStringLiteral("Linear Rec.709"),
          {}, QStringLiteral("Linear P3 EDR"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits"),
          Preset::HdrEdrP3),

        // --- ACES 2.0 SDR (colorimetric) ---
        P(QStringLiteral("ACEScg → sRGB"), secAces2,
          QStringLiteral("ACES 2.0"), QStringLiteral("ACEScg"),
          {}, QStringLiteral("sRGB - Display"),
          QStringLiteral("Video (colorimetric)")),

        P(QStringLiteral("ACEScg → Display P3"), secAces2,
          QStringLiteral("ACES 2.0"), QStringLiteral("ACEScg"),
          {}, QStringLiteral("Display P3 - Display"),
          QStringLiteral("Video (colorimetric)"),
          Preset::SdrP3),

        P(QStringLiteral("sRGB → Rec.2100-PQ HDR"), secAces2,
          QStringLiteral("ACES 2.0"), QStringLiteral("sRGB - Display"),
          {}, QStringLiteral("Rec.2100-PQ - Display"),
          QStringLiteral("Video (colorimetric)"),
          Preset::HdrPq),

        P(QStringLiteral("Rec.709 → Rec.2100-PQ HDR"), secAces2,
          QStringLiteral("ACES 2.0"),
          QStringLiteral("Rec.1886 Rec.709 - Display"),
          {}, QStringLiteral("Rec.2100-PQ - Display"),
          QStringLiteral("Video (colorimetric)"),
          Preset::HdrPq),

        // --- ACES 2.0 HDR (linear-light EDR) ---
        // sRGB-primaries variant works on macOS EDR + Windows scRGB.
        P(QStringLiteral("ACEScg → EDR sRGB 1000 nits"), secAces2,
          QStringLiteral("ACES 2.0"), QStringLiteral("ACEScg"),
          {}, QStringLiteral("Linear sRGB EDR - Display"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits (P3 D65)"),
          Preset::HdrEdrSrgb),

        // P3-primaries variant — macOS only.
        P(QStringLiteral("ACEScg → EDR P3 1000 nits"), secAces2,
          QStringLiteral("ACES 2.0"), QStringLiteral("ACEScg"),
          {}, QStringLiteral("Linear P3 EDR - Display"),
          QStringLiteral("ACES 2.0 - HDR 1000 nits (P3 D65)"),
          Preset::HdrEdrP3),

        // --- ACES 1.3 ---
        // ACES 1.3 calls ACEScg "lin_ap1" in its OCIO config — the
        // input slot keeps that name (the chain build references it),
        // but the user-facing preset name uses the more recognizable
        // "ACEScg" label.
        P(QStringLiteral("ACEScg → sRGB"), secAces1,
          QStringLiteral("ACES 1.3"), QStringLiteral("lin_ap1"),
          {}, QStringLiteral("srgb_display"),
          QStringLiteral("ACES 1.0 - SDR Video")),

        P(QStringLiteral("aces2065-1 → sRGB"), secAces1,
          QStringLiteral("ACES 1.3"), QStringLiteral("aces2065_1"),
          {}, QStringLiteral("srgb_display"),
          QStringLiteral("ACES 1.0 - SDR Video")),
    };

    emit presetsChanged();
}

QVariantList PresetManager::availablePresetEntries() const
{
    QVariantList out;
    out.reserve(m_presets.size());
    for (const Preset &p : m_presets) {
        QVariantMap entry;
        entry[QStringLiteral("name")]    = p.name;
        entry[QStringLiteral("section")] = p.section;
        entry[QStringLiteral("kind")]    = static_cast<int>(p.kind);
        out.append(entry);
    }
    return out;
}

QStringList PresetManager::availablePresets() const
{
    QStringList out;
    out.reserve(m_presets.size());
    for (const Preset &p : m_presets) out << p.name;
    return out;
}

const PresetManager::Preset *PresetManager::findByName(const QString &name) const
{
    for (const Preset &p : m_presets) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

bool PresetManager::applyPreset(const QString &name)
{
    if (!m_ocio) return false;
    const Preset *p = findByName(name);
    if (!p) {
        qWarning("PresetManager: unknown preset '%s'", qPrintable(name));
        return false;
    }

    m_applying = true;

    // Switch config first if needed — the input/display/view names
    // in the preset are interpreted in the target config's name
    // space, so the config has to be live before we set them.
    if (!p->configName.isEmpty()
        && p->configName != m_ocio->activeConfigName()) {
        if (!m_ocio->setActiveConfig(p->configName)) {
            qWarning("PresetManager: preset '%s' references config "
                     "'%s' which isn't available — slots not applied",
                     qPrintable(name), qPrintable(p->configName));
            m_applying = false;
            return false;
        }
    }

    // Apply each slot. Setters now accept empty as "clear" (Guide
    // 05 D6 — passthrough on absence) so the "None (Passthrough)"
    // preset wipes all four colorspace-name slots.
    m_ocio->setActiveInput(p->input);
    m_ocio->setActiveLook(p->look);
    m_ocio->setActiveDisplay(p->output);
    m_ocio->setActiveView(p->view);
    m_ocio->setActiveSceneLutPath(p->sceneLutPath);
    m_ocio->setActiveDisplayLutPath(p->displayLutPath);

    // Auto-engage (Phase 2.5e.1.5): the user picking a preset is a
    // deliberate "I want this look" action — it shouldn't require a
    // separate Engage flip. They can still disengage manually if
    // they want to compare to the raw image.
    m_ocio->setEngaged(true);

    m_applying = false;

    bool changed = false;
    if (m_activePresetName != name) {
        m_activePresetName = name;
        changed = true;
    }
    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
    if (changed) emit activePresetChanged();
    return true;
}

bool PresetManager::currentMatchesPreset(const Preset &p) const
{
    if (!m_ocio) return false;
    // configName comparison handles the empty-config case for the
    // "None (Passthrough)" preset (configName is empty; current
    // configName is whatever's loaded — they won't match, so None
    // is "modified" the moment any other config is active. That's
    // correct: None means literally nothing is engaged, and we
    // can't represent that state while a config is loaded with
    // non-default slots).
    return m_ocio->activeConfigName()      == p.configName
        && m_ocio->activeInput()           == p.input
        && m_ocio->activeLook()            == p.look
        && m_ocio->activeDisplay()         == p.output
        && m_ocio->activeView()            == p.view
        && m_ocio->activeSceneLutPath()    == p.sceneLutPath
        && m_ocio->activeDisplayLutPath()  == p.displayLutPath;
}

void PresetManager::onActiveChainChanged()
{
    if (m_applying) return;
    if (m_activePresetName.isEmpty()) return;
    const Preset *p = findByName(m_activePresetName);
    if (!p) return;
    const bool nowModified = !currentMatchesPreset(*p);
    if (nowModified != m_modified) {
        m_modified = nowModified;
        emit modifiedChanged();
    }
}

void PresetManager::onConfigChanged()
{
    if (m_applying) return;
    // Manual config switch: the active preset's slots are written
    // in the *old* config's namespace, so the preset name no longer
    // describes what's loaded. Clear it. (Switching configs as part
    // of a preset apply goes through m_applying=true and is
    // unaffected.)
    if (!m_activePresetName.isEmpty()) {
        m_activePresetName.clear();
        emit activePresetChanged();
    }
    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
}

void PresetManager::stepPreset(int delta)
{
    if (m_presets.isEmpty()) return;
    int idx = -1;
    for (int i = 0; i < m_presets.size(); ++i) {
        if (m_presets[i].name == m_activePresetName) { idx = i; break; }
    }
    if (idx < 0) idx = 0;
    int next = (idx + delta) % m_presets.size();
    if (next < 0) next += m_presets.size();
    applyPreset(m_presets[next].name);
}

// ---- Phase 2.5e.2: save + persistence ----

PresetManager::Preset *PresetManager::findByNameMutable(const QString &name)
{
    for (Preset &p : m_presets) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

bool PresetManager::activeIsBuiltIn() const
{
    if (const Preset *p = findByName(m_activePresetName)) {
        return p->builtIn;
    }
    return false;
}

namespace {

// Derive a Preset::Kind from the OCIO output / display name. Used for
// user-saved presets (which don't ship with a kind in the JSON); the
// built-ins set it explicitly in loadBuiltIns(). Same heuristic shape
// as Guide 06 §3's star-decoration matcher.
PresetManager::Preset::Kind inferKindFromOutput(const QString &output)
{
    if (output.isEmpty()) return PresetManager::Preset::Universal;
    const QString lo = output.toLower();
    const bool hasPq      = lo.contains(QStringLiteral("pq"))
                         || lo.contains(QStringLiteral("st.2084"))
                         || lo.contains(QStringLiteral("st2084"))
                         || lo.contains(QStringLiteral("rec.2100-pq"))
                         || lo.contains(QStringLiteral("rec2100-pq"))
                         || lo.contains(QStringLiteral("hdr10"));
    if (hasPq) return PresetManager::Preset::HdrPq;
    const bool hasEdr     = lo.contains(QStringLiteral("edr"))
                         || lo.contains(QStringLiteral("linear"))
                         || lo.contains(QStringLiteral("extended"));
    if (hasEdr) {
        // Split by primaries — P3-tagged linear EDR is macOS-only;
        // sRGB / Rec.709 linear EDR works on Windows scRGB too.
        const bool hasP3 = lo.contains(QStringLiteral("p3"));
        return hasP3 ? PresetManager::Preset::HdrEdrP3
                     : PresetManager::Preset::HdrEdrSrgb;
    }
    // SDR — split on primaries again so Display P3 SDR Outputs only
    // light up in the SDR Display P3 swapchain mode, and sRGB / 709 /
    // 1886 only light up in the SDR sRGB mode.
    const bool hasP3 = lo.contains(QStringLiteral("p3"))
                    && !lo.contains(QStringLiteral("dci"));
    return hasP3 ? PresetManager::Preset::SdrP3
                 : PresetManager::Preset::SdrSrgb;
}

} // namespace

PresetManager::Preset
PresetManager::captureCurrentAsPreset(const QString &name) const
{
    Preset p;
    p.name = name;
    p.section = QStringLiteral("Custom");
    p.builtIn = false;
    if (m_ocio) {
        p.configName     = m_ocio->activeConfigName();
        p.input          = m_ocio->activeInput();
        p.look           = m_ocio->activeLook();
        p.output         = m_ocio->activeDisplay();
        p.view           = m_ocio->activeView();
        p.sceneLutPath   = m_ocio->activeSceneLutPath();
        p.displayLutPath = m_ocio->activeDisplayLutPath();
    }
    p.kind = inferKindFromOutput(p.output);
    return p;
}

bool PresetManager::saveCurrent()
{
    if (m_activePresetName.isEmpty()) {
        qWarning("PresetManager::saveCurrent: no active preset — call saveAs");
        return false;
    }
    Preset *existing = findByNameMutable(m_activePresetName);
    if (!existing) {
        qWarning("PresetManager::saveCurrent: active preset '%s' is missing",
                 qPrintable(m_activePresetName));
        return false;
    }
    if (existing->builtIn) {
        qWarning("PresetManager::saveCurrent: '%s' is built-in — call saveAs",
                 qPrintable(existing->name));
        return false;
    }
    Preset captured = captureCurrentAsPreset(existing->name);
    captured.section = existing->section;  // keep "Custom"
    *existing = captured;
    if (!writeUserPresetsToDisk()) return false;

    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
    emit presetsChanged();
    return true;
}

bool PresetManager::saveAs(const QString &name)
{
    if (name.trimmed().isEmpty()) {
        qWarning("PresetManager::saveAs: empty name rejected");
        return false;
    }
    if (findByName(name)) {
        qWarning("PresetManager::saveAs: '%s' already exists",
                 qPrintable(name));
        return false;
    }
    Preset p = captureCurrentAsPreset(name);
    m_presets.append(p);
    if (!writeUserPresetsToDisk()) {
        // Roll back the in-memory append so disk + memory stay in sync.
        m_presets.removeLast();
        return false;
    }

    m_activePresetName = name;
    emit presetsChanged();
    emit activePresetChanged();
    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
    return true;
}

bool PresetManager::deleteCurrent()
{
    if (m_activePresetName.isEmpty()) {
        qWarning("PresetManager::deleteCurrent: no active preset");
        return false;
    }
    int idx = -1;
    for (int i = 0; i < m_presets.size(); ++i) {
        if (m_presets[i].name == m_activePresetName) { idx = i; break; }
    }
    if (idx < 0) {
        qWarning("PresetManager::deleteCurrent: active preset '%s' is missing",
                 qPrintable(m_activePresetName));
        return false;
    }
    if (m_presets[idx].builtIn) {
        qWarning("PresetManager::deleteCurrent: '%s' is built-in — cannot delete",
                 qPrintable(m_presets[idx].name));
        return false;
    }

    // Snapshot for rollback on disk-write failure — and, on success,
    // for the undo-toast (undoDeleteLast).
    const Preset removed = m_presets.takeAt(idx);
    if (!writeUserPresetsToDisk()) {
        m_presets.insert(idx, removed);
        return false;
    }
    m_lastDeleted      = removed;
    m_lastDeletedIndex = idx;
    m_hasLastDeleted   = true;

    m_activePresetName.clear();
    emit presetsChanged();
    emit activePresetChanged();
    if (m_modified) {
        m_modified = false;
        emit modifiedChanged();
    }
    return true;
}

QString PresetManager::undoDeleteLast()
{
    if (!m_hasLastDeleted) return {};
    if (findByName(m_lastDeleted.name)) {
        // The name was reused since the delete (re-save / save-as) —
        // restoring would collide. The newer preset wins; drop the
        // snapshot for good.
        qWarning("PresetManager::undoDeleteLast: '%s' exists again — "
                 "not restoring", qPrintable(m_lastDeleted.name));
        m_hasLastDeleted = false;
        return {};
    }

    const int idx = qBound(0, m_lastDeletedIndex,
                           static_cast<int>(m_presets.size()));
    m_presets.insert(idx, m_lastDeleted);
    if (!writeUserPresetsToDisk()) {
        m_presets.removeAt(idx);   // keep the snapshot — retry possible
        return {};
    }
    m_hasLastDeleted = false;

    // Reinstate the pre-delete selection. deleteCurrent never touched
    // the live OCIO chain, so don't re-apply — just recompute whether
    // the chain still matches (the user may have tweaked reels in
    // between, which correctly reads as "modified").
    m_activePresetName = m_lastDeleted.name;
    emit presetsChanged();
    emit activePresetChanged();
    const bool nowModified = !currentMatchesPreset(m_lastDeleted);
    if (m_modified != nowModified) {
        m_modified = nowModified;
        emit modifiedChanged();
    }
    return m_lastDeleted.name;
}

QString PresetManager::userPresetsFilePath() const
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir + QStringLiteral("/color-presets.json");
}

void PresetManager::loadUserPresetsFromDisk()
{
    const QString path = userPresetsFilePath();
    QFile f(path);
    if (!f.exists()) return;          // first launch — no user presets yet
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("PresetManager: cannot open %s: %s",
                 qPrintable(path), qPrintable(f.errorString()));
        return;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning("PresetManager: %s parse error: %s",
                 qPrintable(path), qPrintable(err.errorString()));
        return;
    }
    const QJsonObject root = doc.object();
    const int schema = root.value(QStringLiteral("schema")).toInt(0);
    if (schema != 1) {
        qWarning("PresetManager: %s has unsupported schema %d (expected 1)",
                 qPrintable(path), schema);
        return;
    }
    const QJsonArray arr = root.value(QStringLiteral("user_presets")).toArray();
    int loaded = 0;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const QString n = o.value(QStringLiteral("name")).toString();
        if (n.isEmpty()) continue;
        if (findByName(n)) {
            qWarning("PresetManager: skipping duplicate user preset '%s'",
                     qPrintable(n));
            continue;
        }
        // Local variable name avoids `slots` — Qt MOC defines that
        // as a no-op macro, producing parse errors on bare use.
        const QJsonObject slotsObj = o.value(QStringLiteral("slots")).toObject();
        Preset p;
        p.name           = n;
        p.section        = QStringLiteral("Custom");
        p.builtIn        = false;
        p.configName     = o.value(QStringLiteral("configName")).toString();
        p.input          = slotsObj.value(QStringLiteral("input")).toString();
        p.look           = slotsObj.value(QStringLiteral("look")).toString();
        p.output         = slotsObj.value(QStringLiteral("output")).toString();
        p.view           = slotsObj.value(QStringLiteral("view")).toString();
        p.sceneLutPath   = slotsObj.value(QStringLiteral("scene_lut")).toString();
        p.displayLutPath = slotsObj.value(QStringLiteral("display_lut")).toString();
        p.kind           = inferKindFromOutput(p.output);
        m_presets.append(p);
        ++loaded;
    }
    if (loaded > 0) {
        qInfo("PresetManager: loaded %d user preset(s) from %s",
              loaded, qPrintable(path));
        emit presetsChanged();
    }
}

bool PresetManager::writeUserPresetsToDisk() const
{
    const QString path = userPresetsFilePath();
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) {
        qWarning("PresetManager: cannot create preset dir %s",
                 qPrintable(dir));
        return false;
    }

    QJsonArray arr;
    for (const Preset &p : m_presets) {
        if (p.builtIn) continue;       // only user presets persist
        QJsonObject slotsObj;
        slotsObj[QStringLiteral("input")]        = p.input;
        slotsObj[QStringLiteral("look")]         = p.look;
        slotsObj[QStringLiteral("output")]       = p.output;
        slotsObj[QStringLiteral("view")]         = p.view;
        slotsObj[QStringLiteral("scene_lut")]    = p.sceneLutPath;
        slotsObj[QStringLiteral("display_lut")]  = p.displayLutPath;

        QJsonObject obj;
        obj[QStringLiteral("name")]       = p.name;
        obj[QStringLiteral("configName")] = p.configName;
        obj[QStringLiteral("slots")]      = slotsObj;
        // Per-entry timestamps for the Manage modal (.e.4) — modified
        // refreshes on each write; created sticks to the file's
        // existing value if we can find it, else now.
        const QString iso = QDateTime::currentDateTimeUtc()
                                .toString(Qt::ISODate);
        obj[QStringLiteral("modified_at")] = iso;
        obj[QStringLiteral("created_at")]  = iso;
        arr.append(obj);
    }
    QJsonObject root;
    root[QStringLiteral("schema")]       = 1;
    root[QStringLiteral("user_presets")] = arr;

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("PresetManager: cannot open %s for write: %s",
                 qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        qWarning("PresetManager: commit failed for %s: %s",
                 qPrintable(path), qPrintable(f.errorString()));
        return false;
    }
    return true;
}

} // namespace qcv
