#include "ocio_config_manager.h"

#include "ocio_chain_builder.h"
#include "ocio_lut_baker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QtLogging>

#include <cstdlib>

#include <OpenColorIO/OpenColorIO.h>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

namespace {

// Phase 2.5c: friendly names for shipped config directories.
// Anything not in the table falls back to the dir name as-is.
QString friendlyNameForDir(const QString &dirName)
{
    static const QHash<QString, QString> table{
        { QStringLiteral("Blender5.1"), QStringLiteral("Blender 5.1") },
        { QStringLiteral("Blender"),    QStringLiteral("Blender (legacy)") },
        { QStringLiteral("ACES_2.0"),   QStringLiteral("ACES 2.0") },
        { QStringLiteral("ACES_1.3"),   QStringLiteral("ACES 1.3") },
    };
    return table.value(dirName, dirName);
}

// Version gate per Guide 05 D10. A config that declares a higher
// OCIO profile than the linked library can't be safely loaded —
// reject before we let OCIO throw mid-build.
bool isVersionAcceptable(const OCIO::ConstConfigRcPtr &cfg, QString *outReason)
{
    if (!cfg) {
        if (outReason) *outReason = QStringLiteral("null config");
        return false;
    }
    const int cfgMajor = cfg->getMajorVersion();
    const int cfgMinor = cfg->getMinorVersion();
    const int libMajor = OCIO::GetVersionHex() >> 24;
    const int libMinor = (OCIO::GetVersionHex() >> 16) & 0xFF;
    if (cfgMajor > libMajor
        || (cfgMajor == libMajor && cfgMinor > libMinor)) {
        if (outReason) {
            *outReason = QStringLiteral(
                "config declares OCIO %1.%2; linked library supports up to %3.%4")
                .arg(cfgMajor).arg(cfgMinor).arg(libMajor).arg(libMinor);
        }
        return false;
    }
    return true;
}

// Phase 2.5: LUT slot extensions OCIO's FileTransform can load.
// .cube is by far the most common interchange format; .3dl is older
// (Discreet) but still used by some grading workflows; .csp is OCIO's
// shaper-paired format.
bool isSupportedLutExtension(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("cube")
        || suffix == QStringLiteral("3dl")
        || suffix == QStringLiteral("csp");
}

// Resolve the bundled OCIO assets directory.
//
// macOS .app bundle:  applicationDirPath() = qcview.app/Contents/MacOS
//                     assets at:            qcview.app/Contents/Resources/assets/OCIO
//                     → ../Resources/assets/OCIO
//
// Dev build (cmake)    is the same — `target_sources` with
//                      MACOSX_PACKAGE_LOCATION puts the assets in the
//                      built .app's Contents/Resources, so the same
//                      relative-path resolution works whether the
//                      .app is run from the build directory or from
//                      a deployed bundle.
//
// Returns an empty QString if the directory doesn't exist (caller
// falls back to ocio://default with a warning).
QString resolveOcioAssetsDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/../Resources/assets/OCIO"),    // macOS bundle
        appDir + QStringLiteral("/assets/OCIO"),                  // future Linux/Win
        appDir + QStringLiteral("/../../assets/OCIO"),            // out-of-tree build
    };
    for (const QString &path : candidates) {
        const QString canonical = QDir(path).canonicalPath();
        if (!canonical.isEmpty() && QFileInfo(canonical).isDir()) {
            return canonical;
        }
    }
    return {};
}

} // namespace

struct OCIOConfigManager::Impl {
    OCIO::ConstConfigRcPtr config;
};

OCIOConfigManager::OCIOConfigManager(QObject *parent)
    : QObject(parent), m_impl(std::make_unique<Impl>())
{
    // Phase 2.5c: build the enumerated config slot list once at
    // startup ($OCIO env + scan of bundled assets/OCIO/), then load
    // the highest-priority entry per Guide 05 §3:
    //   1. Explicit user choice in Settings  (not yet — Phase 7+)
    //   2. $OCIO env var (if set + valid + passes version gate)
    //   3. Shipped default (Blender 5.1)
    //   4. OCIO library built-in (last-resort)
    enumerateConfigs();

    bool loaded = false;
    for (const ConfigSlot &slot : m_configSlots) {
        qInfo("OCIOConfigManager: loading '%s' from %s",
              qPrintable(slot.displayName), qPrintable(slot.path));
        if (loadConfigFile(slot.path)) {
            m_activeConfigName = slot.displayName;
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        qWarning("OCIOConfigManager: no enumerated config loaded; "
                 "falling back to ocio://default");
        if (loadBuiltInDefault()) {
            m_activeConfigName = QStringLiteral("OCIO built-in default");
            // Add the built-in to the list so the UI has something
            // to display in the Config reel.
            m_configSlots.prepend({ m_activeConfigName,
                                    QStringLiteral("ocio://default") });
            emit availableConfigsChanged();
        }
    }
}

void OCIOConfigManager::enumerateConfigs()
{
    m_configSlots.clear();

    // $OCIO env var — highest priority of the auto-discovered slots.
    if (const char *envOcio = std::getenv("OCIO"); envOcio && *envOcio) {
        const QString envPath = QString::fromUtf8(envOcio);
        if (QFileInfo::exists(envPath)) {
            // Version gate the env config before listing it. If it's
            // out-of-version we still list it, but loadConfigFile
            // will reject it on attempt — and the next slot wins.
            m_configSlots.append({ QStringLiteral("$OCIO"), envPath });
            qInfo("OCIOConfigManager: $OCIO points to %s", qPrintable(envPath));
        } else {
            qWarning("OCIOConfigManager: $OCIO=%s — file does not exist; ignored",
                     envOcio);
        }
    }

    // Bundled configs — scan assets/OCIO/* for any subdir containing
    // a config.ocio. Order is alphabetical by friendly name except
    // Blender 5.1 is always first (matches old app default).
    const QString assetsDir = resolveOcioAssetsDir();
    if (!assetsDir.isEmpty()) {
        QDir d(assetsDir);
        const QFileInfoList subdirs =
            d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        QList<ConfigSlot> bundled;
        for (const QFileInfo &sub : subdirs) {
            const QString cfg = sub.absoluteFilePath()
                                + QStringLiteral("/config.ocio");
            if (QFileInfo::exists(cfg)) {
                bundled.append({ friendlyNameForDir(sub.fileName()), cfg });
            }
        }
        // Promote Blender 5.1 to the front of the bundled list.
        for (int i = 0; i < bundled.size(); ++i) {
            if (bundled[i].displayName == QStringLiteral("Blender 5.1")) {
                bundled.move(i, 0);
                break;
            }
        }
        m_configSlots.append(bundled);
    }

    emit availableConfigsChanged();
}

OCIOConfigManager::~OCIOConfigManager() = default;

bool OCIOConfigManager::loadBuiltInDefault()
{
    try {
        m_impl->config = OCIO::Config::CreateFromFile("ocio://default");
        m_configIdentifier = QStringLiteral("ocio://default");
    } catch (const OCIO::Exception &e) {
        qWarning("OCIOConfigManager: built-in default load failed: %s", e.what());
        return false;
    }
    resetActiveDefaults();
    emit configChanged();
    return true;
}

bool OCIOConfigManager::loadConfigFile(const QString &path)
{
    OCIO::ConstConfigRcPtr candidate;
    try {
        candidate = OCIO::Config::CreateFromFile(path.toUtf8().constData());
    } catch (const OCIO::Exception &e) {
        qWarning("OCIOConfigManager: load %s failed: %s",
                 qPrintable(path), e.what());
        return false;
    }
    QString reason;
    if (!isVersionAcceptable(candidate, &reason)) {
        qWarning("OCIOConfigManager: rejected %s — %s",
                 qPrintable(path), qPrintable(reason));
        return false;
    }
    m_impl->config = candidate;
    m_configIdentifier = path;
    resetActiveDefaults();
    emit configChanged();
    return true;
}

bool OCIOConfigManager::setActiveConfig(const QString &name)
{
    if (name == m_activeConfigName) return true;
    for (const ConfigSlot &slot : m_configSlots) {
        if (slot.displayName == name) {
            // For the built-in URI entry, route through the URI loader.
            const bool ok = (slot.path == QStringLiteral("ocio://default"))
                ? loadBuiltInDefault()
                : loadConfigFile(slot.path);
            if (ok) {
                m_activeConfigName = name;
                emit configChanged();   // configIdentifier + activeConfigName
            }
            return ok;
        }
    }
    qWarning("OCIOConfigManager: unknown config '%s'", qPrintable(name));
    return false;
}

QStringList OCIOConfigManager::availableConfigs() const
{
    QStringList out;
    out.reserve(m_configSlots.size());
    for (const ConfigSlot &slot : m_configSlots) {
        out << slot.displayName;
    }
    return out;
}

QString OCIOConfigManager::configDescription() const
{
    if (!m_impl->config) return {};
    const char *desc = m_impl->config->getDescription();
    if (!desc) return {};
    const QString full = QString::fromUtf8(desc).trimmed();
    if (full.isEmpty()) return full;

    // The ACES configs ship with multi-line descriptions plus an
    // ASCII rule and trailing version-bracket tags, e.g.
    //   "Academy Color Encoding System - Studio Config (All Views)
    //    [COLORSPACES v4.0.0] [ACES v2.0] [OCIO v2.5]
    //    ----------------------------------------..."
    // The panel header is single-line, so collapse to the first
    // non-rule content line and strip the trailing "[...] [...]"
    // version tags.
    QString line;
    for (const QString &raw : full.split(QChar('\n'))) {
        const QString t = raw.trimmed();
        if (t.isEmpty()) continue;
        bool allDash = (t.size() >= 4);
        for (QChar c : t) {
            if (c != QLatin1Char('-')) { allDash = false; break; }
        }
        if (allDash) continue;
        line = t;
        break;
    }
    if (line.isEmpty()) return full;

    // Strip trailing "  [foo]  [bar]  ..." groups.
    while (line.endsWith(QLatin1Char(']'))) {
        const int open = line.lastIndexOf(QLatin1Char('['));
        if (open <= 0) break;
        line.truncate(open);
        line = line.trimmed();
    }
    return line;
}

QString OCIOConfigManager::configIdentifier() const
{
    return m_configIdentifier;
}

QStringList OCIOConfigManager::colorspaces() const
{
    QStringList out;
    if (!m_impl->config) return out;
    for (int i = 0; i < m_impl->config->getNumColorSpaces(); ++i) {
        out << QString::fromUtf8(m_impl->config->getColorSpaceNameByIndex(i));
    }
    return out;
}

QStringList OCIOConfigManager::displays() const
{
    QStringList out;
    if (!m_impl->config) return out;
    for (int i = 0; i < m_impl->config->getNumDisplays(); ++i) {
        out << QString::fromUtf8(m_impl->config->getDisplay(i));
    }
    return out;
}

QStringList OCIOConfigManager::looks() const
{
    QStringList out;
    if (!m_impl->config) return out;
    for (int i = 0; i < m_impl->config->getNumLooks(); ++i) {
        out << QString::fromUtf8(m_impl->config->getLookNameByIndex(i));
    }
    return out;
}

QStringList OCIOConfigManager::viewsForDisplay(const QString &display) const
{
    QStringList out;
    if (!m_impl->config || display.isEmpty()) return out;
    const QByteArray d = display.toUtf8();
    const int n = m_impl->config->getNumViews(d.constData());
    for (int i = 0; i < n; ++i) {
        out << QString::fromUtf8(m_impl->config->getView(d.constData(), i));
    }
    return out;
}

QString OCIOConfigManager::exportLut(const QString &outPath, int cubeSize)
{
    if (!m_impl->config) {
        return QStringLiteral("No OCIO config loaded");
    }
    if (outPath.isEmpty()) {
        return QStringLiteral("Output path is empty");
    }
    if (cubeSize < 2) {
        return QStringLiteral("Cube size must be ≥ 2 (got %1)").arg(cubeSize);
    }

    QString err;
    OCIO::GroupTransformRcPtr group =
        OcioChainBuilder::buildGroupTransform(this, m_impl->config, &err);
    if (!group) {
        return err.isEmpty() ? QStringLiteral("Chain build failed") : err;
    }

    try {
        OCIO::ConstProcessorRcPtr proc = m_impl->config->getProcessor(group);
        // OPTIMIZATION_DEFAULT enables OCIO's standard fast path
        // (LUT prebake / pixel-format short-circuit). Sub-100 ms at
        // 65³ on M-series silicon.
        OCIO::ConstCPUProcessorRcPtr cpuProc =
            proc->getOptimizedCPUProcessor(OCIO::OPTIMIZATION_DEFAULT);
        return OcioLutBaker::writeCube(cpuProc, outPath, cubeSize);
    } catch (const OCIO::Exception &e) {
        return QStringLiteral("OCIO error: %1").arg(e.what());
    }
}

void OCIOConfigManager::setActiveInput(const QString &name)
{
    if (m_activeInput == name) return;
    // Empty = clear (slot becomes passthrough — Guide 05 D6).
    if (!name.isEmpty() && !isValidColorSpace(name)) {
        qWarning("OCIOConfigManager: rejected unknown Input colorspace '%s'",
                 qPrintable(name));
        return;
    }
    m_activeInput = name;
    m_activeChainGeneration.fetch_add(1, std::memory_order_acq_rel);
    emit activeChainChanged();
}

void OCIOConfigManager::setActiveDisplay(const QString &name)
{
    if (m_activeDisplay == name) return;
    if (!name.isEmpty() && !isValidDisplay(name)) {
        qWarning("OCIOConfigManager: rejected unknown Display '%s'",
                 qPrintable(name));
        return;
    }
    m_activeDisplay = name;
    // If current View is no longer valid for the new Display, snap
    // to the first available view (or clear if there are none — e.g.
    // when Display itself was just cleared).
    if (!m_activeDisplay.isEmpty()
        && !isValidViewForDisplay(m_activeDisplay, m_activeView)) {
        const QStringList views = viewsForDisplay(m_activeDisplay);
        m_activeView = views.isEmpty() ? QString() : views.first();
    } else if (m_activeDisplay.isEmpty()) {
        m_activeView.clear();
    }
    m_activeChainGeneration.fetch_add(1, std::memory_order_acq_rel);
    emit activeChainChanged();
}

void OCIOConfigManager::setActiveView(const QString &name)
{
    if (m_activeView == name) return;
    if (!name.isEmpty()
        && !isValidViewForDisplay(m_activeDisplay, name)) {
        qWarning("OCIOConfigManager: rejected View '%s' for Display '%s'",
                 qPrintable(name), qPrintable(m_activeDisplay));
        return;
    }
    m_activeView = name;
    m_activeChainGeneration.fetch_add(1, std::memory_order_acq_rel);
    emit activeChainChanged();
}

void OCIOConfigManager::setActiveLook(const QString &name)
{
    if (m_activeLook == name) return;
    if (!name.isEmpty() && !isValidLook(name)) {
        qWarning("OCIOConfigManager: rejected unknown Look '%s'",
                 qPrintable(name));
        return;
    }
    m_activeLook = name;
    m_activeChainGeneration.fetch_add(1, std::memory_order_acq_rel);
    emit activeChainChanged();
}

void OCIOConfigManager::setActiveSceneLutPath(const QString &path)
{
    if (m_activeSceneLutPath == path) return;
    if (!path.isEmpty()) {
        if (!QFileInfo::exists(path)) {
            qWarning("OCIOConfigManager: Scene LUT file not found: %s",
                     qPrintable(path));
            return;
        }
        if (!isSupportedLutExtension(path)) {
            qWarning("OCIOConfigManager: unsupported Scene LUT extension '%s' "
                     "(expected .cube / .3dl / .csp)",
                     qPrintable(QFileInfo(path).suffix()));
            return;
        }
    }
    m_activeSceneLutPath = path;
    m_activeChainGeneration.fetch_add(1, std::memory_order_acq_rel);
    emit activeChainChanged();
}

void OCIOConfigManager::setActiveDisplayLutPath(const QString &path)
{
    if (m_activeDisplayLutPath == path) return;
    if (!path.isEmpty()) {
        if (!QFileInfo::exists(path)) {
            qWarning("OCIOConfigManager: Display LUT file not found: %s",
                     qPrintable(path));
            return;
        }
        if (!isSupportedLutExtension(path)) {
            qWarning("OCIOConfigManager: unsupported Display LUT extension '%s' "
                     "(expected .cube / .3dl / .csp)",
                     qPrintable(QFileInfo(path).suffix()));
            return;
        }
    }
    m_activeDisplayLutPath = path;
    m_activeChainGeneration.fetch_add(1, std::memory_order_acq_rel);
    emit activeChainChanged();
}

void OCIOConfigManager::setEngaged(bool b)
{
    if (m_engaged == b) return;
    m_engaged = b;
    m_activeChainGeneration.fetch_add(1, std::memory_order_acq_rel);
    emit activeChainChanged();
}

// -------- private --------

void OCIOConfigManager::resetActiveDefaults()
{
    // Engagement is the user's persistent intent ("I want OCIO
    // active") and is orthogonal to which config / chain is loaded.
    // We don't touch m_engaged here so config switches preserve the
    // user's engage state — they pick a new config and the new
    // chain's defaults render live without having to re-flip the
    // toggle. The two exceptions are (a) the no-config branch below,
    // where there's no chain to engage with, and (b) the constructor's
    // default initializer in the header (engaged = false on launch).
    if (!m_impl->config) {
        m_activeInput.clear();
        m_activeDisplay.clear();
        m_activeView.clear();
        m_activeLook.clear();
        m_activeSceneLutPath.clear();
        m_activeDisplayLutPath.clear();
        m_engaged = false;
        emit activeChainChanged();
        return;
    }

    // Default Input — try 'color_picking' role (sRGB-equivalent in
    // most configs); fall back to 'scene_linear'; then to the first
    // colorspace.
    if (const char *cs = m_impl->config->getRoleColorSpace("color_picking"); cs && *cs) {
        m_activeInput = QString::fromUtf8(cs);
    } else if (const char *cs = m_impl->config->getRoleColorSpace("scene_linear"); cs && *cs) {
        m_activeInput = QString::fromUtf8(cs);
    } else if (m_impl->config->getNumColorSpaces() > 0) {
        m_activeInput = QString::fromUtf8(
            m_impl->config->getColorSpaceNameByIndex(0));
    } else {
        m_activeInput.clear();
    }

    // Default Display — config's default display.
    if (const char *d = m_impl->config->getDefaultDisplay(); d && *d) {
        m_activeDisplay = QString::fromUtf8(d);
    } else if (m_impl->config->getNumDisplays() > 0) {
        m_activeDisplay = QString::fromUtf8(m_impl->config->getDisplay(0));
    } else {
        m_activeDisplay.clear();
    }

    // Default View — prefer "Un-tone-mapped" when available (the
    // OCIO 2.5 default config's identity-ish view for SDR content),
    // else the config's getDefaultView() (often an ACES tone-mapped
    // SDR view, which dims in-range content noticeably). Phase 2.4
    // lets users pick anything else from the slot-machine UI.
    if (!m_activeDisplay.isEmpty()) {
        const QStringList views = viewsForDisplay(m_activeDisplay);
        const QString preferUntonemapped = QStringLiteral("Un-tone-mapped");
        if (views.contains(preferUntonemapped)) {
            m_activeView = preferUntonemapped;
        } else {
            const QByteArray d = m_activeDisplay.toUtf8();
            if (const char *v = m_impl->config->getDefaultView(d.constData());
                v && *v) {
                m_activeView = QString::fromUtf8(v);
            } else {
                m_activeView = views.isEmpty() ? QString() : views.first();
            }
        }
    } else {
        m_activeView.clear();
    }

    m_activeLook.clear();
    m_activeSceneLutPath.clear();
    m_activeDisplayLutPath.clear();
    // m_engaged intentionally preserved — see comment at top of fn.
    m_activeChainGeneration.fetch_add(1, std::memory_order_acq_rel);
    emit activeChainChanged();
}

bool OCIOConfigManager::isValidColorSpace(const QString &name) const
{
    if (!m_impl->config || name.isEmpty()) return false;
    OCIO::ConstColorSpaceRcPtr cs = m_impl->config->getColorSpace(name.toUtf8().constData());
    return cs != nullptr;
}

bool OCIOConfigManager::isValidDisplay(const QString &name) const
{
    if (!m_impl->config || name.isEmpty()) return false;
    for (int i = 0; i < m_impl->config->getNumDisplays(); ++i) {
        if (name == QString::fromUtf8(m_impl->config->getDisplay(i))) return true;
    }
    return false;
}

bool OCIOConfigManager::isValidViewForDisplay(const QString &display,
                                              const QString &view) const
{
    if (!m_impl->config || display.isEmpty() || view.isEmpty()) return false;
    const QByteArray d = display.toUtf8();
    for (int i = 0; i < m_impl->config->getNumViews(d.constData()); ++i) {
        if (view == QString::fromUtf8(m_impl->config->getView(d.constData(), i))) {
            return true;
        }
    }
    return false;
}

bool OCIOConfigManager::isValidLook(const QString &name) const
{
    if (!m_impl->config || name.isEmpty()) return false;
    return m_impl->config->getLook(name.toUtf8().constData()) != nullptr;
}

} // namespace qcv
