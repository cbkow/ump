// QCView Player — application entry
//
// Phase 1.2: forces the QRhi backend, loads the UI window, then
// instantiates WindowManager which creates and tracks the Player
// child window.

#include <QColorSpace>
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QFileInfo>
#include <QSet>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>

#include <mutex>
#include <sstream>
#include <thread>

#include "window/window_manager.h"
#include "window/sparkle_updater_macos.h"

#if defined(Q_OS_WIN)
#  include "decode/vulkan/vulkan_device_manager.h"
#endif

namespace {

// Force the QRhi backend at process start, before any QQuickWindow is
// constructed. Per Guide 01 D3.
void forceQRhiBackend()
{
#if defined(Q_OS_MACOS)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#elif defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
#else
#  error "Unsupported platform for QCView Player"
#endif
}

// Phase 2.4: pin the application-wide default surface colorspace to
// sRGB. The UI window is shown by QML before any C++ runs after
// engine.loadFromModule(), so QWindow::setFormat after the fact is a
// no-op for its swapchain. Setting the default format here applies
// to every QQuickWindow constructed by the QML engine — i.e., the UI
// window. The Player child window then overrides per-window with
// its own setFormat (currently sRGB; switches to extended-linear
// sRGB / P3 in the HDR phase, Guide 06 §1).
void setDefaultUiColorspace()
{
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setColorSpace(QColorSpace::SRgb);
    QSurfaceFormat::setDefaultFormat(fmt);
}

// File-logger plumbing. The app is built /subsystem:windows so
// qInfo/qWarning would otherwise vanish into OutputDebugString —
// invisible without DebugView. This handler tees every log line
// (with timestamp + severity prefix) to `qcview-log.txt` next to the
// executable AND still forwards to the default Qt handler so existing
// debug consumers (DebugView, attached debuggers) keep working.
//
// Truncates on launch so a stale log doesn't grow unbounded across
// sessions; if persistent rolling logs are wanted we can add date-
// stamped rotation later.
QtMessageHandler g_prevQtHandler = nullptr;
std::mutex       g_logFileMutex;
QFile           *g_logFile       = nullptr;

void fileLoggerMessageHandler(QtMsgType type,
                                const QMessageLogContext &ctx,
                                const QString &msg)
{
    const char *sev = "I";
    switch (type) {
        case QtDebugMsg:    sev = "D"; break;
        case QtInfoMsg:     sev = "I"; break;
        case QtWarningMsg:  sev = "W"; break;
        case QtCriticalMsg: sev = "E"; break;
        case QtFatalMsg:    sev = "F"; break;
    }
    // Thread-id tag — hash std::thread::id to its underlying integer
    // via stringstream (the only portable extraction), then take the
    // low 12 bits hex so the log column stays compact. Lets us cross-
    // reference which thread emitted each line — critical for chasing
    // heap-corruption windows that race between the GUI thread, the
    // audio decode thread, and QThreadPool background workers.
    std::ostringstream tidss;
    tidss << std::this_thread::get_id();
    QString tid = QString::fromStdString(tidss.str());
    if (tid.size() > 4) tid = tid.right(4);
    const QString line = QStringLiteral("[%1 t=%2] %3  %4")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz")))
        .arg(tid)
        .arg(QLatin1String(sev))
        .arg(msg);

    {
        std::lock_guard<std::mutex> lock(g_logFileMutex);
        if (g_logFile && g_logFile->isOpen()) {
            QTextStream ts(g_logFile);
            ts << line << '\n';
            g_logFile->flush();
        }
    }
    if (g_prevQtHandler) {
        g_prevQtHandler(type, ctx, msg);
    }
}

void installFileLogger()
{
    const QString logPath = QCoreApplication::applicationDirPath()
                          + QStringLiteral("/qcview-log.txt");
    const QString prevPath = QCoreApplication::applicationDirPath()
                           + QStringLiteral("/qcview-log.prev.txt");
    // Rotate the previous session's log to qcview-log.prev.txt before
    // we truncate. Without this, a crash followed by a re-launch
    // wipes the crashing session's log — exactly when we need it
    // most. Now the prior session always survives one more launch.
    if (QFile::exists(logPath)) {
        QFile::remove(prevPath);
        QFile::rename(logPath, prevPath);
    }
    auto *f = new QFile(logPath);
    if (!f->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        delete f;
        return;
    }
    g_logFile        = f;
    g_prevQtHandler  = qInstallMessageHandler(fileLoggerMessageHandler);
    qInfo("File logger installed → %s", qPrintable(logPath));
}

// Per-user named pipe / socket for single-instance handoff. Including
// the username scopes us to the current login session — a fast-user-
// switched second account on the same Windows box gets its own primary
// instance, and `\\.\pipe\` ACLs aren't relied on for isolation.
QString singleInstanceServerName()
{
    QString user = QString::fromLocal8Bit(qgetenv("USERNAME"));
    if (user.isEmpty()) user = QString::fromLocal8Bit(qgetenv("USER"));
    if (user.isEmpty()) user = QStringLiteral("default");
    return QStringLiteral("qcview-singleinstance-") + user;
}

// Extract the positional file/URI arguments from argv (drop argv[0]
// and the flag-style payloads consumed by --dual-test / --simulate-
// user / --sbs). Shared by the argv path AND the secondary's forward-
// to-primary path so both code paths see the exact same list.
QStringList collectPositionalArgs(const QStringList &args)
{
    QSet<int> consumed;
    consumed.insert(0);   // argv[0] = exe path
    auto consumeFlag = [&](const QString &flag, int payloadCount) {
        const int i = args.indexOf(flag);
        if (i < 0) return;
        consumed.insert(i);
        for (int k = 1; k <= payloadCount && i + k < args.size(); ++k)
            consumed.insert(i + k);
    };
    consumeFlag(QStringLiteral("--dual-test"),     2);
    consumeFlag(QStringLiteral("--simulate-user"), 2);
    consumeFlag(QStringLiteral("--sbs"),           0);

    QStringList out;
    out.reserve(args.size());
    for (int i = 0; i < args.size(); ++i) {
        if (consumed.contains(i)) continue;
        const QString a = args.at(i);
        if (a.isEmpty() || a.startsWith(QLatin1Char('-'))) continue;
        out << a;
    }
    return out;
}

// Dev-mode flags that the user passes when they explicitly want a
// fresh process (dual-test harness, etc.). When any of these are
// present we skip the single-instance handoff entirely — secondary
// keeps running, primary keeps running, two processes side by side.
bool hasDevModeFlag(const QStringList &args)
{
    return args.contains(QStringLiteral("--dual-test"))
        || args.contains(QStringLiteral("--simulate-user"));
}

// Open a list of file paths / qcview:// URIs through the running
// primary's WindowManager. Same logic as the original argv lambda;
// extracted into a helper so the single-instance signal handler can
// reuse it for forwarded paths without duplicating the dispatch.
void dispatchOpenArgsOnPrimary(qcv::WindowManager *wm, const QStringList &paths)
{
    if (!wm || paths.isEmpty()) return;
    QString firstAdded;
    for (const QString &arg : paths) {
        if (arg.startsWith(QStringLiteral("qcview://"), Qt::CaseInsensitive)) {
            wm->openProjectLink(arg);
            continue;
        }
        if (auto *p = wm->project()) {
            const QString id = p->addMediaFile(arg);
            if (!id.isEmpty() && firstAdded.isEmpty()) {
                firstAdded = id;
            }
        }
    }
    if (!firstAdded.isEmpty()) {
        if (auto *p = wm->project()) {
            p->setActiveItem(firstAdded);
        }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    forceQRhiBackend();
    setDefaultUiColorspace();

    // Match MinRender — Fusion gives traditional always-visible
    // scrollbars instead of the macOS overlay style's auto-fade,
    // so users can see where they are in long lists. Our explicit
    // QtQuick.Controls.Basic imports (FlatButton, FlatComboBox,
    // etc.) are unaffected since they pin the style locally.
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    QGuiApplication::setApplicationName(QStringLiteral("QCView"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("QCView"));
    QGuiApplication::setApplicationVersion(QStringLiteral(QCV_VERSION_STR));
    QGuiApplication::setOrganizationName(QStringLiteral("QCView"));
    QGuiApplication::setOrganizationDomain(QStringLiteral("qcview.app"));

    QGuiApplication app(argc, argv);

    // Single-instance handoff (secondary side).
    //
    // If a primary qcview is already listening on our per-user socket,
    // forward this launch's positional args to it and exit. Skips when
    // dev-mode flags are present so --dual-test always gets its own
    // process. Without this, two qcview processes shared the QSettings
    // recent-media list and racing writes from concurrent shutdowns
    // were producing heap-corruption crashes via corrupted QStringList
    // entries in the menu rebuild chain.
    {
        const QStringList allArgs = QGuiApplication::arguments();
        if (!hasDevModeFlag(allArgs)) {
            const QString serverName = singleInstanceServerName();
            QLocalSocket probe;
            probe.connectToServer(serverName);
            if (probe.waitForConnected(500)) {
                const QStringList toForward = collectPositionalArgs(allArgs);
                QByteArray payload;
                {
                    QDataStream ds(&payload, QIODevice::WriteOnly);
                    ds.setVersion(QDataStream::Qt_6_0);
                    ds << toForward;
                }
                probe.write(payload);
                probe.flush();
                probe.waitForBytesWritten(1000);
                probe.disconnectFromServer();
                if (probe.state() != QLocalSocket::UnconnectedState) {
                    probe.waitForDisconnected(500);
                }
                return 0;
            }
        }
    }

    // Phase G.1 — file logger. Tees Qt log output to qcview-log.txt
    // next to the binary so qInfo/qWarning are visible without
    // DebugView. Installed AFTER QGuiApplication so applicationDirPath()
    // is resolved, but BEFORE anything else that logs, so we capture
    // VulkanDeviceManager init etc.
    installFileLogger();

    // Window icon — split title-bar vs taskbar/Alt+Tab visuals.
    //
    // Windows queries each window for ICON_SMALL (title bar) and
    // ICON_BIG (taskbar / Alt+Tab) at different pixel sizes. Qt picks
    // the closest pre-supplied size from the QIcon for each request,
    // so we feed in the cutout variant at small sizes (16/20/24) and
    // the full logo at larger sizes (32/48/64/128/256). Result:
    //   - title bar  → qcviewcutout.png
    //   - taskbar    → qcview.png
    // Pre-scaling with SmoothTransformation keeps the 512² source
    // sharp at small sizes (better than Qt's default addFile scale).
    //
    // The .exe-embedded icon from qcview.rc still drives Explorer /
    // file thumbnails / pinned shortcuts before launch.
    //
    // macOS skipped: title bars don't show an icon, and the Dock pulls
    // from the bundle's .icns (CFBundleIconFile in Info.plist). Calling
    // setWindowIcon here would override the launched-app Dock icon with
    // the Windows-style qcview.png that has its own squircle baked in,
    // which prevents Tahoe's "Dark" appearance mode from drawing the
    // system mortise behind the foreground artwork.
#if !defined(Q_OS_MACOS)
    {
        const QString appDir = QCoreApplication::applicationDirPath();
        auto resolveIcon = [&appDir](const QString &fname) -> QString {
            const QStringList candidates = {
                appDir + QStringLiteral("/../Resources/assets/icons/") + fname,
                appDir + QStringLiteral("/assets/icons/") + fname,
                appDir + QStringLiteral("/../../assets/icons/") + fname,
                appDir + QStringLiteral("/../../../assets/icons/") + fname,
            };
            for (const QString &p : candidates) {
                if (QFileInfo::exists(p)) return p;
            }
            return {};
        };
        const QString cutoutPath = resolveIcon(QStringLiteral("qcviewcutout.png"));
        const QString fullPath   = resolveIcon(QStringLiteral("qcview.png"));

        QIcon winIcon;
        const QImage cutoutImg = cutoutPath.isEmpty() ? QImage() : QImage(cutoutPath);
        const QImage fullImg   = fullPath.isEmpty()   ? QImage() : QImage(fullPath);
        if (!cutoutImg.isNull()) {
            for (int sz : {16, 20, 24}) {
                winIcon.addPixmap(QPixmap::fromImage(cutoutImg.scaled(
                    sz, sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)));
            }
        }
        if (!fullImg.isNull()) {
            for (int sz : {32, 40, 48, 64, 96, 128, 256}) {
                winIcon.addPixmap(QPixmap::fromImage(fullImg.scaled(
                    sz, sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)));
            }
        } else if (!cutoutImg.isNull()) {
            // Full missing — fall back to cutout at larger sizes too so
            // the taskbar isn't blank.
            for (int sz : {32, 48, 64, 128, 256}) {
                winIcon.addPixmap(QPixmap::fromImage(cutoutImg.scaled(
                    sz, sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)));
            }
        }
        if (!winIcon.isNull()) app.setWindowIcon(winIcon);
    }
#endif

#if defined(Q_OS_WIN)
    // Phase F.2.4.1 — stand up the decode-only Vulkan device early so
    // diagnostics log device + extension list at startup. FFmpeg's
    // hwaccel context attach in F.2.4.2 will consume the same device.
    // Soft init: failures are logged but do not abort startup
    // (F.2.4.1 has no consumers yet; F.2.4.2 makes it required).
    if (!qcv::VulkanDeviceManager::instance().initialize()) {
        qWarning("main: VulkanDeviceManager init failed — Vulkan decode "
                 "path will be unavailable; F.2.4.2 will gate on this.");
    }
#endif

    // Polish — bundled fonts. Inter (sans), JetBrainsMono, and
    // Phosphor (icon font). Theme.qml binds to WindowManager's
    // family accessors so it picks up whatever
    // QFontDatabase::addApplicationFont registered.
    auto loadFamily = [](const QString &path) -> QString {
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0) {
            qWarning("main: failed to load font %s", qPrintable(path));
            return {};
        }
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        return families.isEmpty() ? QString() : families.first();
    };
#ifdef Q_OS_MACOS
    QString fontDir = QCoreApplication::applicationDirPath()
                    + QStringLiteral("/../Resources/assets/fonts/");
#else
    QString fontDir = QCoreApplication::applicationDirPath()
                    + QStringLiteral("/assets/fonts/");
#endif
    if (!QDir(fontDir).exists()) {
        fontDir = QCoreApplication::applicationDirPath()
                + QStringLiteral("/assets/fonts/");
    }
    const QString interName = loadFamily(fontDir + "Inter_18pt-Regular.ttf");
    loadFamily(fontDir + "Inter_18pt-Bold.ttf");
    loadFamily(fontDir + "Inter_18pt-Italic.ttf");
    const QString monoName     = loadFamily(fontDir + "JetBrainsMono-Regular.ttf");
    const QString phosphorName = loadFamily(fontDir + "Phosphor.ttf");
    if (!interName.isEmpty()) {
        QGuiApplication::setFont(QFont(interName, 10));
    }

    QQmlApplicationEngine engine;

    // Expose WindowManager to QML before loading the module so the UI
    // window's QML can bind buttons to its slots.
    qcv::WindowManager windowManager(&engine);
    if (!interName.isEmpty())    windowManager.setInterFamily(interName);
    if (!monoName.isEmpty())     windowManager.setMonoFamily(monoName);
    if (!phosphorName.isEmpty()) windowManager.setPhosphorFamily(phosphorName);
    engine.rootContext()->setContextProperty(QStringLiteral("WindowManager"), &windowManager);

    engine.loadFromModule(QStringLiteral("Qcv"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    if (!windowManager.initialize()) {
        return -2;
    }

    // Start the Sparkle auto-updater (macOS). Deferred one tick off the
    // launch path; honors the Info.plist SU* keys (feed, EdDSA key, daily
    // auto-check). No-op on other platforms. The About-menu "Check for
    // Updates…" and the Settings toggle route through WindowManager.
    QTimer::singleShot(0, &windowManager, [] {
        qcv::startSparkleUpdater();
    });

    // Phase 7.7 Stage 3 — developer test entry. If `--dual-test PATH_A
    // PATH_B` is on the command line, enter dual mode after the
    // window comes up. Schedules a one-shot QTimer so the entry runs
    // on the event loop after first expose.
    {
        QStringList args = QGuiApplication::arguments();
        const int idx = args.indexOf(QStringLiteral("--dual-test"));
        if (idx >= 0 && idx + 2 < args.size()) {
            const QString pathA = args.at(idx + 1);
            const QString pathB = args.at(idx + 2);
            const bool   sbs   = args.contains(QStringLiteral("--sbs"));
            QTimer::singleShot(500, &windowManager,
                               [&windowManager, pathA, pathB, sbs] {
                windowManager.enterDualTestMode(pathA, pathB);
                if (sbs) {
                    // Match the user's manual click-SBS flow.
                    QTimer::singleShot(1000, &windowManager,
                                        [&windowManager] {
                        windowManager.setCompositorMode(1);
                    });
                }
            });
        }
        // Positional arguments — file paths and qcview:// URIs.
        // Use cases:
        //   - qcview.exe "C:\path\to\file.mov"           (Explorer
        //     double-click after we register file types in the
        //     installer)
        //   - qcview.exe "qcview://uuid/mediaId"         (URI scheme
        //     registration, also installer-side)
        //   - qcview.exe a.mov b.mov c.exr               (multiple
        //     files — all get added to the project, first one auto-
        //     loads into the viewport)
        //
        // The secondary-handoff path above catches the
        // already-running-instance case; reaching here means we're
        // the primary (or the only instance).
        {
            const QStringList toOpen = collectPositionalArgs(
                QGuiApplication::arguments());
            if (!toOpen.isEmpty()) {
                QTimer::singleShot(500, &windowManager,
                                    [&windowManager, toOpen] {
                    dispatchOpenArgsOnPrimary(&windowManager, toOpen);
                });
            }
        }

        // --simulate-user A B: matches the user's actual interaction —
        // drop A onto the chip (loads via single flow), drop B (sets
        // Project::bSource), then click SBS. Lets us reproduce the
        // bug they reported without UI driver tools.
        const int simIdx = args.indexOf(QStringLiteral("--simulate-user"));
        if (simIdx >= 0 && simIdx + 2 < args.size()) {
            const QString pathA = args.at(simIdx + 1);
            const QString pathB = args.at(simIdx + 2);
            QTimer::singleShot(800, &windowManager,
                               [&windowManager, pathA, pathB] {
                qInfo("--simulate-user: drop A=%s",
                      qPrintable(QFileInfo(pathA).fileName()));
                if (auto *p = windowManager.project()) {
                    const QString id = p->addMediaFile(pathA);
                    if (!id.isEmpty()) p->setActiveItem(id);
                }
                QTimer::singleShot(800, &windowManager,
                                    [&windowManager, pathB] {
                    qInfo("--simulate-user: drop B=%s",
                          qPrintable(QFileInfo(pathB).fileName()));
                    windowManager.setBSource(pathB);
                    QTimer::singleShot(800, &windowManager,
                                        [&windowManager] {
                        qInfo("--simulate-user: click SBS");
                        windowManager.setCompositorMode(1);
                    });
                });
            });
        }
    }

    // Single-instance handoff (primary side).
    //
    // We get here only when the secondary probe above didn't find a
    // listening server — i.e., we ARE the primary. Stand up the
    // server now (after windowManager is live so the newConnection
    // handler can dispatch through it) so subsequent qcview launches
    // forward their file args to us instead of starting a second
    // process. removeServer first to clear any stale POSIX socket
    // file from a previous hard kill; on Windows named pipes self-
    // clean when the last handle closes so the call is a no-op there.
    QLocalServer singleInstanceServer;
    {
        const bool wantSingle = !hasDevModeFlag(QGuiApplication::arguments());
        if (wantSingle) {
            const QString serverName = singleInstanceServerName();
            QLocalServer::removeServer(serverName);
            if (!singleInstanceServer.listen(serverName)) {
                qWarning("single-instance: QLocalServer::listen('%s') failed: %s",
                         qPrintable(serverName),
                         qPrintable(singleInstanceServer.errorString()));
            } else {
                qInfo("single-instance: listening on '%s'",
                      qPrintable(serverName));
                QObject::connect(&singleInstanceServer,
                                 &QLocalServer::newConnection,
                                 &windowManager,
                                 [&windowManager, srv = &singleInstanceServer]() {
                    while (auto *conn = srv->nextPendingConnection()) {
                        QObject::connect(conn, &QLocalSocket::disconnected,
                                         conn, &QObject::deleteLater);
                        // Synchronous read — payload is a single small
                        // QDataStream-serialized QStringList; 2s
                        // covers any reasonable slow handshake.
                        QByteArray payload;
                        while (conn->state() == QLocalSocket::ConnectedState &&
                               conn->bytesAvailable() < int(sizeof(quint32))) {
                            if (!conn->waitForReadyRead(2000)) break;
                        }
                        payload = conn->readAll();
                        // Drain any tail bytes that arrived between
                        // readAll and the disconnect.
                        while (conn->waitForReadyRead(50)) {
                            payload += conn->readAll();
                        }
                        QStringList paths;
                        {
                            QDataStream ds(&payload, QIODevice::ReadOnly);
                            ds.setVersion(QDataStream::Qt_6_0);
                            ds >> paths;
                        }
                        conn->disconnectFromServer();
                        qInfo("single-instance: received %d path(s) "
                              "from secondary launch", int(paths.size()));
                        if (!paths.isEmpty()) {
                            dispatchOpenArgsOnPrimary(&windowManager, paths);
                        }
                        windowManager.raiseUiWindow();
                    }
                });
            }
        }
    }

    return app.exec();
}
