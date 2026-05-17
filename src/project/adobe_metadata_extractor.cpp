// AdobeMetadataExtractor — see header.

#include "adobe_metadata_extractor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QtGlobal>     // Q_OS_WIN for the .exe-first probe order
#include <QtLogging>

namespace qcv {

namespace {

// Exiftool tags we care about. -s emits "Tag: Value" per line.
const QStringList kExifTags = {
    QStringLiteral("-XMP:AeProjectLinkFullPath"),
    QStringLiteral("-XMP:WindowsAtomUncProjectPath"),
    QStringLiteral("-XMP:MacAtomPosixProjectPath"),
    QStringLiteral("-QuickTime:StartTimecode"),
    QStringLiteral("-QuickTime:TimeCode"),
    QStringLiteral("-QuickTime:CreationDate"),
    QStringLiteral("-QuickTime:MediaCreateDate"),
    QStringLiteral("-MXF:StartTimecode"),
    QStringLiteral("-MXF:TimecodeAtStart"),
    QStringLiteral("-XMP:StartTimecode"),
    QStringLiteral("-XMP:AltTimecode"),
    QStringLiteral("-UserData:TimeCode"),
};

// Parse exiftool -s output: "Tag : Value" lines, one per tag.
// Returns map of trimmed Tag → Value.
QHash<QString, QString> parseExifOutput(const QString &output)
{
    QHash<QString, QString> fields;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) continue;
        const QString key = line.left(colon).trimmed();
        const QString val = line.mid(colon + 1).trimmed();
        if (!key.isEmpty()) fields.insert(key, val);
    }
    return fields;
}

} // namespace

QString AdobeMetadataExtractor::resolveExiftoolPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();

    // 1. Bundled — qcview.app/Contents/Resources/assets/exiftool/exiftool
    //    On Windows the bundle is the self-contained exiftool.exe (the
    //    Strawberry-Perl-embedded build from exiftool.org). On macOS
    //    it's the Perl-script wrapper that defers to the system's
    //    Perl. The bare-name "exiftool" file IS also present in the
    //    Windows bundle (it's the same Perl script), but it's not
    //    directly executable — try .exe first on Windows so the
    //    script doesn't get picked up first.
    QStringList bundleCandidates;
#if defined(Q_OS_WIN)
    bundleCandidates
        << appDir + QStringLiteral("/assets/exiftool/exiftool.exe")
        << appDir + QStringLiteral("/../../assets/exiftool/exiftool.exe");
#endif
    bundleCandidates
        << appDir + QStringLiteral("/../Resources/assets/exiftool/exiftool")
        << appDir + QStringLiteral("/assets/exiftool/exiftool")
        << appDir + QStringLiteral("/../../assets/exiftool/exiftool");
    for (const QString &c : bundleCandidates) {
        const QString canonical = QFileInfo(c).canonicalFilePath();
        if (!canonical.isEmpty() && QFileInfo(canonical).isExecutable()) {
            return canonical;
        }
    }

    // 2. PATH — `which exiftool`. Useful when running tests outside
    //    the bundle layout.
    const QString fromPath = QStandardPaths::findExecutable(
        QStringLiteral("exiftool"));
    if (!fromPath.isEmpty()) return fromPath;

    return {};
}

AdobeMetadata AdobeMetadataExtractor::extract(const QString &filePath)
{
    AdobeMetadata m;
    if (filePath.isEmpty() || !QFileInfo::exists(filePath)) {
        m.loaded = true;
        return m;
    }

    // TRACE_EXIFTOOL — start. exiftool is a Perl subprocess; we
    // log entry / exit so the timing window in the parent process
    // is visible in the trace (the actual heavy lifting is in the
    // child, but our waitForFinished blocks a QThreadPool worker).
    const QString trimmedName = QFileInfo(filePath).fileName();
    qInfo("AdobeMetadataExtractor: begin '%s'", qPrintable(trimmedName));

    const QString exiftool = resolveExiftoolPath();
    if (exiftool.isEmpty()) {
        qWarning("AdobeMetadataExtractor: exiftool not found "
                 "(checked bundled + PATH); skipping for %s",
                 qPrintable(filePath));
        m.loaded = true;
        return m;
    }

    QStringList args;
    args << QStringLiteral("-s");        // short tag form, suppress group prefixes
    args << kExifTags;
    args << filePath;

    QProcess proc;
    proc.start(exiftool, args);
    if (!proc.waitForStarted(2000)) {
        qWarning("AdobeMetadataExtractor: failed to start exiftool: %s",
                 qPrintable(proc.errorString()));
        m.loaded = true;
        return m;
    }
    if (!proc.waitForFinished(15000)) {
        qWarning("AdobeMetadataExtractor: exiftool timeout after 15s for %s",
                 qPrintable(filePath));
        proc.kill();
        proc.waitForFinished(1000);
        m.loaded = true;
        return m;
    }
    if (proc.exitStatus() != QProcess::NormalExit) {
        m.loaded = true;
        return m;
    }

    const QString output = QString::fromUtf8(proc.readAllStandardOutput());
    const auto fields = parseExifOutput(output);

    auto get = [&](const char *k) -> QString {
        return fields.value(QString::fromLatin1(k));
    };

    // Adobe project links — exiftool's -s strips the group prefix,
    // so we look up by the bare tag name.
    m.aeProjectPath    = get("AeProjectLinkFullPath");
    m.premiereWinPath  = get("WindowsAtomUncProjectPath");
    m.premiereMacPath  = get("MacAtomPosixProjectPath");

    // Timecodes. QuickTime / XMP / MXF / UserData all share the
    // same `-s` output namespace; the same key can appear from
    // different groups. We've requested narrow tag lists so this
    // is unambiguous in practice.
    m.qtStartTimecode  = get("StartTimecode");
    m.qtTimecode       = get("TimeCode");
    m.qtCreationDate   = get("CreationDate");
    m.qtMediaCreateDate= get("MediaCreateDate");
    m.mxfStartTimecode = get("StartTimecode");   // MXF version overrides if present
    m.mxfTimecodeAtStart = get("TimecodeAtStart");
    m.xmpStartTimecode = m.qtStartTimecode;       // alias when XMP-sourced
    m.xmpAltTimecode   = get("AltTimecode");
    m.userdataTimecode = get("TimeCode");

    m.loaded = true;
    qInfo("AdobeMetadataExtractor: end '%s' (ae=%d prMac=%d prWin=%d)",
          qPrintable(trimmedName),
          !m.aeProjectPath.isEmpty(),
          !m.premiereMacPath.isEmpty(),
          !m.premiereWinPath.isEmpty());
    return m;
}

} // namespace qcv
