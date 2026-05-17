// OCIOConfigManager — Phase 2.1.
//
// Owns the application-wide OpenColorIO config + the active
// [Input, View, Look, Display, Bypass] selection. All renderers and
// the slot-machine UI consume this singleton.
//
// Design contract per Guide 14:
// - Config defaults to OCIO 2.5's built-in ACES 2.0 CG config
//   (`ocio://default`). No bundled config files needed for v1.
// - Active-chain setters validate against the loaded config; an
//   invalid name is rejected with a warning, leaving the previous
//   value in place.
// - Bypass toggle short-circuits the entire chain via OCIO's
//   identity transform (Guide 15 §16 takeaway #9).
// - PIMPL: OCIO C++ headers do NOT leak to consumers. video
//   decoder, renderer, UI all see only Q_PROPERTY-friendly types.
//
// Phase 2.1 ships the data plumbing only — no shader generation,
// no render integration. Those land in 2.2 / 2.3.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>

namespace qcv {

class OCIOConfigManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString configDescription READ configDescription NOTIFY configChanged)
    Q_PROPERTY(QString configIdentifier READ configIdentifier NOTIFY configChanged)
    Q_PROPERTY(QStringList availableConfigs READ availableConfigs NOTIFY availableConfigsChanged)
    Q_PROPERTY(QString activeConfigName READ activeConfigName NOTIFY configChanged)
    Q_PROPERTY(QStringList colorspaces READ colorspaces NOTIFY configChanged)
    Q_PROPERTY(QStringList displays READ displays NOTIFY configChanged)
    Q_PROPERTY(QStringList looks READ looks NOTIFY configChanged)
    Q_PROPERTY(QString activeInput READ activeInput WRITE setActiveInput NOTIFY activeChainChanged)
    Q_PROPERTY(QString activeDisplay READ activeDisplay WRITE setActiveDisplay NOTIFY activeChainChanged)
    Q_PROPERTY(QString activeView READ activeView WRITE setActiveView NOTIFY activeChainChanged)
    Q_PROPERTY(QString activeLook READ activeLook WRITE setActiveLook NOTIFY activeChainChanged)
    Q_PROPERTY(QString activeSceneLutPath READ activeSceneLutPath WRITE setActiveSceneLutPath NOTIFY activeChainChanged)
    Q_PROPERTY(QString activeDisplayLutPath READ activeDisplayLutPath WRITE setActiveDisplayLutPath NOTIFY activeChainChanged)
    // OCIO is something the user deliberately *engages* — default
    // off, so the app boots showing the raw image. Slot-machine
    // selections persist on the manager regardless of engagement;
    // flipping engaged ON is the explicit "apply this chain" action,
    // and once engaged subsequent slot edits live-update.
    Q_PROPERTY(bool engaged READ engaged WRITE setEngaged NOTIFY activeChainChanged)

public:
    explicit OCIOConfigManager(QObject *parent = nullptr);
    ~OCIOConfigManager() override;

    // QML-callable.
    Q_INVOKABLE bool loadBuiltInDefault();
    Q_INVOKABLE bool loadConfigFile(const QString &path);
    // Switch to one of the enumerated configs. `name` must be a
    // member of availableConfigs(). Resets the active chain to the
    // new config's defaults (a chain valid for one config is rarely
    // valid for another). No-op if name is unknown.
    Q_INVOKABLE bool setActiveConfig(const QString &name);

    // Read-only views of the loaded config.
    QString configDescription() const;
    QString configIdentifier() const;     // "ocio://default" or file path
    QStringList availableConfigs() const;
    QString activeConfigName() const   { return m_activeConfigName; }
    QStringList colorspaces() const;
    QStringList displays() const;
    QStringList looks() const;
    Q_INVOKABLE QStringList viewsForDisplay(const QString &display) const;

    // Bake the active OCIO chain (Look → Scene LUT → DisplayView →
    // Display LUT) to a .cube 3D LUT file at `outPath`. cubeSize is
    // the per-axis grid resolution (65 = 65³ samples ≈ 274K points,
    // sub-100 ms on M-series). Returns empty string on success;
    // human-readable error message on failure.
    //
    // Independent of `engaged` — the chain just needs to be
    // *configured* (input + display + view all set). Synchronous;
    // safe to call from the QML thread at the default 65³ size.
    Q_INVOKABLE QString exportLut(const QString &outPath, int cubeSize = 65);

    // Active chain (drives shader cache key in Phase 2.2+).
    QString activeInput()           const { return m_activeInput; }
    QString activeDisplay()         const { return m_activeDisplay; }
    QString activeView()            const { return m_activeView; }
    QString activeLook()            const { return m_activeLook; }
    QString activeSceneLutPath()    const { return m_activeSceneLutPath; }
    QString activeDisplayLutPath()  const { return m_activeDisplayLutPath; }
    bool    engaged()               const { return m_engaged; }

    void setActiveInput(const QString &name);
    void setActiveDisplay(const QString &name);
    void setActiveView(const QString &name);
    void setActiveLook(const QString &name);
    // LUT-slot path setters (Phase 2.5). Empty path clears the slot.
    // Non-empty path is validated for existence + supported extension
    // (.cube / .3dl / .csp); rejection leaves previous value.
    void setActiveSceneLutPath(const QString &path);
    void setActiveDisplayLutPath(const QString &path);
    void setEngaged(bool b);

    // Monotonically-increasing counter incremented each time the
    // active chain changes. The render thread polls this in
    // synchronize() and rebuilds the OCIO pipeline when it bumps.
    // Atomic for cross-thread reads.
    int activeChainGeneration() const {
        return m_activeChainGeneration.load(std::memory_order_acquire);
    }

signals:
    void configChanged();
    void activeChainChanged();
    void availableConfigsChanged();

private:
    void resetActiveDefaults();
    bool isValidColorSpace(const QString &name) const;
    bool isValidDisplay(const QString &name) const;
    bool isValidViewForDisplay(const QString &display, const QString &view) const;
    bool isValidLook(const QString &name) const;

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    // Phase 2.5c: enumerated set of configs the user can switch
    // between. Built once at startup from $OCIO env var + scan of
    // assets/OCIO/. Display name → absolute config path. Order
    // matters (first entry is the priority pick on startup) so
    // QHash isn't right; QList of pairs.
    struct ConfigSlot {
        QString displayName;     // "Blender 5.1", "$OCIO", etc.
        QString path;            // absolute path to config.ocio
    };
    QList<ConfigSlot> m_configSlots;
    QString m_activeConfigName;

    void enumerateConfigs();

    QString m_configIdentifier;
    QString m_activeInput;
    QString m_activeDisplay;
    QString m_activeView;
    QString m_activeLook;
    QString m_activeSceneLutPath;
    QString m_activeDisplayLutPath;
    bool    m_engaged = false;     // default disengaged — see Q_PROPERTY note
    std::atomic<int> m_activeChainGeneration{0};
};

} // namespace qcv
