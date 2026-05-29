#include "sparkle_updater_macos.h"

#ifdef Q_OS_MACOS

#import <Sparkle/Sparkle.h>

#include <QtGlobal>

namespace qcv {
namespace {

// App-lifetime singleton. Retained for the process lifetime by design
// (a deliberate, harmless "leak"); the updater must outlive every check.
SPUStandardUpdaterController *g_updaterController = nil;

} // namespace

void startSparkleUpdater()
{
    if (g_updaterController) return;
    // startingUpdater:YES starts the updater immediately, honoring the
    // Info.plist SU* keys (feed URL, EdDSA public key, automatic-check
    // policy + interval). nil delegates → Sparkle's standard behavior.
    g_updaterController =
        [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                      updaterDelegate:nil
                                                   userDriverDelegate:nil];
    qInfo("Sparkle: updater started (auto-checks=%d, interval=%.0fs)",
          g_updaterController.updater.automaticallyChecksForUpdates ? 1 : 0,
          g_updaterController.updater.updateCheckInterval);
}

void checkForUpdatesNow()
{
    if (!g_updaterController) startSparkleUpdater();
    [g_updaterController checkForUpdates:nil];
}

void setAutomaticUpdateChecks(bool enabled)
{
    if (!g_updaterController) startSparkleUpdater();
    g_updaterController.updater.automaticallyChecksForUpdates =
        enabled ? YES : NO;
}

bool automaticUpdateChecks()
{
    if (!g_updaterController) return false;
    return g_updaterController.updater.automaticallyChecksForUpdates;
}

} // namespace qcv

#endif // Q_OS_MACOS
