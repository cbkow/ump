// Sparkle auto-updater shim — C++-only interface over Sparkle 2's
// SPUStandardUpdaterController (which provides the standard update UI:
// check / download progress / release notes / install + relaunch).
//
// Mirrors native_fullscreen_macos.h: the macOS implementation lives in
// sparkle_updater_macos.mm (compiled only on Apple); on other platforms
// the calls compile to inline no-ops so WindowManager can invoke them
// unconditionally.
#pragma once

#include <QtGlobal>

namespace qcv {

#ifdef Q_OS_MACOS

// Construct + start the shared updater. Call once at launch after the UI
// is up. Honors the Info.plist SU* keys (SUFeedURL, SUEnableAutomaticChecks,
// SUScheduledCheckInterval, SUPublicEDKey). Idempotent.
void startSparkleUpdater();

// User-initiated "Check for Updates…" — shows Sparkle's standard UI.
void checkForUpdatesNow();

// Toggle / read automatic background checks (Sparkle persists this).
void setAutomaticUpdateChecks(bool enabled);
bool automaticUpdateChecks();

#else  // non-Apple — no updater (Windows uses the Microsoft Store).

inline void startSparkleUpdater() {}
inline void checkForUpdatesNow() {}
inline void setAutomaticUpdateChecks(bool) {}
inline bool automaticUpdateChecks() { return false; }

#endif // Q_OS_MACOS

} // namespace qcv
