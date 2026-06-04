// ProjectIo — `.qcvproj` v2 read / write. See header for the
// design notes. This file is intentionally read-from-start linear:
// helpers, then save(), then load(). The MediaItem JSON shape lives
// here (not on the struct) so the in-memory model doesn't grow a
// JSON dependency.

#include "project_io.h"

#include "media_item.h"
#include "project_manager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QtLogging>

namespace qcv::ProjectIo {

namespace {

constexpr const char *kProjectDirToken = "${PROJECT_DIR}";

// ---- Type-name (de)serialization -------------------------------

QString mediaTypeToString(MediaType t)
{
    switch (t) {
    case MediaType::Video:         return QStringLiteral("Video");
    case MediaType::Audio:         return QStringLiteral("Audio");
    case MediaType::Image:         return QStringLiteral("Image");
    case MediaType::ImageSequence: return QStringLiteral("ImageSequence");
    case MediaType::Playlist:      return QStringLiteral("Playlist");
    case MediaType::DualPair:      return QStringLiteral("DualPair");
    }
    return QStringLiteral("Video");
}

MediaType mediaTypeFromString(const QString &s)
{
    if (s == QStringLiteral("Video"))         return MediaType::Video;
    if (s == QStringLiteral("Audio"))         return MediaType::Audio;
    if (s == QStringLiteral("Image"))         return MediaType::Image;
    if (s == QStringLiteral("ImageSequence")) return MediaType::ImageSequence;
    if (s == QStringLiteral("Playlist"))      return MediaType::Playlist;
    if (s == QStringLiteral("DualPair"))      return MediaType::DualPair;
    qWarning("ProjectIo: unknown MediaType '%s' — defaulting to Video",
             qPrintable(s));
    return MediaType::Video;
}

// ---- ${PROJECT_DIR} path rewriting -----------------------------

// Writes paths under projectDir as "${PROJECT_DIR}/relative".
// Anything outside (or empty) returns unchanged.
QString collapseProjectDir(const QString &absPath, const QString &projectDir)
{
    if (absPath.isEmpty() || projectDir.isEmpty()) return absPath;
    const QString normPath = QDir::cleanPath(absPath);
    const QString normDir  = QDir::cleanPath(projectDir);
    // Direct prefix match plus separator — guards against
    // "/foo/barbaz" colliding with project dir "/foo/bar".
    if (normPath == normDir) {
        return QString::fromLatin1(kProjectDirToken);
    }
    if (normPath.startsWith(normDir + QLatin1Char('/'))) {
        return QString::fromLatin1(kProjectDirToken)
             + normPath.mid(normDir.size());
    }
    return absPath;
}

// On load, replace the leading ${PROJECT_DIR} token (if present)
// with the on-disk project directory.
QString expandProjectDir(const QString &storedPath, const QString &projectDir)
{
    if (storedPath.isEmpty()) return storedPath;
    const QString token = QString::fromLatin1(kProjectDirToken);
    if (!storedPath.startsWith(token)) return storedPath;
    const QString remainder = storedPath.mid(token.size());
    if (remainder.isEmpty()) return projectDir;
    // Strip a leading '/' so QDir::filePath doesn't double up.
    QString rel = remainder;
    if (rel.startsWith(QLatin1Char('/'))) rel = rel.mid(1);
    return QDir(projectDir).filePath(rel);
}

// ---- Sub-struct serialization ----------------------------------

QJsonObject videoMetadataToJson(const VideoMetadata &v)
{
    QJsonObject o;
    o[QStringLiteral("loaded")]              = v.loaded;
    o[QStringLiteral("fileSize")]            = static_cast<qint64>(v.fileSize);
    o[QStringLiteral("width")]               = v.width;
    o[QStringLiteral("height")]              = v.height;
    o[QStringLiteral("frameRate")]           = v.frameRate;
    o[QStringLiteral("totalFrames")]         = v.totalFrames;
    o[QStringLiteral("duration")]            = v.duration;
    o[QStringLiteral("videoCodec")]          = v.videoCodec;
    o[QStringLiteral("pixelFormat")]         = v.pixelFormat;
    o[QStringLiteral("bitDepth")]            = v.bitDepth;
    o[QStringLiteral("hasAlpha")]            = v.hasAlpha;
    o[QStringLiteral("sarNum")]              = v.sarNum;
    o[QStringLiteral("sarDen")]              = v.sarDen;
    o[QStringLiteral("isAnamorphic")]        = v.isAnamorphic;
    o[QStringLiteral("unsupportedCodec")]    = v.unsupportedCodec;
    o[QStringLiteral("cameraVendor")]        = v.cameraVendor;
    o[QStringLiteral("cameraModel")]         = v.cameraModel;
    o[QStringLiteral("colorspace")]          = v.colorspace;
    o[QStringLiteral("colorPrimaries")]      = v.colorPrimaries;
    o[QStringLiteral("colorTransfer")]       = v.colorTransfer;
    o[QStringLiteral("colorRange")]          = v.colorRange;
    o[QStringLiteral("nclcTag")]             = v.nclcTag;
    o[QStringLiteral("isHdrContent")]        = v.isHdrContent;
    o[QStringLiteral("audioCodec")]          = v.audioCodec;
    o[QStringLiteral("audioSampleRate")]     = v.audioSampleRate;
    o[QStringLiteral("audioChannels")]       = v.audioChannels;
    o[QStringLiteral("hasEmbeddedTimecode")] = v.hasEmbeddedTimecode;
    o[QStringLiteral("embeddedTimecode")]    = v.embeddedTimecode;
    o[QStringLiteral("audioChannelLayoutName")] = v.audioChannelLayoutName;
    return o;
}

VideoMetadata videoMetadataFromJson(const QJsonObject &o)
{
    VideoMetadata v;
    v.loaded              = o.value(QStringLiteral("loaded")).toBool();
    v.fileSize            = static_cast<qint64>(
        o.value(QStringLiteral("fileSize")).toDouble());
    v.width               = o.value(QStringLiteral("width")).toInt();
    v.height              = o.value(QStringLiteral("height")).toInt();
    v.frameRate           = o.value(QStringLiteral("frameRate")).toDouble();
    v.totalFrames         = o.value(QStringLiteral("totalFrames")).toInt();
    v.duration            = o.value(QStringLiteral("duration")).toDouble();
    v.videoCodec          = o.value(QStringLiteral("videoCodec")).toString();
    v.pixelFormat         = o.value(QStringLiteral("pixelFormat")).toString();
    v.bitDepth            = o.value(QStringLiteral("bitDepth")).toInt(8);
    v.hasAlpha            = o.value(QStringLiteral("hasAlpha")).toBool();
    // Default 1/1 (square) when missing — old projects and the struct
    // default agree; isAnamorphic recomputed from the ratio so a stale
    // false can't desync.
    v.sarNum              = o.value(QStringLiteral("sarNum")).toInt(1);
    v.sarDen              = o.value(QStringLiteral("sarDen")).toInt(1);
    if (v.sarNum <= 0 || v.sarDen <= 0) { v.sarNum = 1; v.sarDen = 1; }
    v.isAnamorphic        = (v.sarNum != v.sarDen);
    v.unsupportedCodec    = o.value(QStringLiteral("unsupportedCodec")).toBool();
    v.cameraVendor        = o.value(QStringLiteral("cameraVendor")).toString();
    v.cameraModel         = o.value(QStringLiteral("cameraModel")).toString();
    v.colorspace          = o.value(QStringLiteral("colorspace")).toString();
    v.colorPrimaries      = o.value(QStringLiteral("colorPrimaries")).toString();
    v.colorTransfer       = o.value(QStringLiteral("colorTransfer")).toString();
    v.colorRange          = o.value(QStringLiteral("colorRange")).toString();
    v.nclcTag             = o.value(QStringLiteral("nclcTag")).toString();
    v.isHdrContent        = o.value(QStringLiteral("isHdrContent")).toBool();
    v.audioCodec          = o.value(QStringLiteral("audioCodec")).toString();
    v.audioSampleRate     = o.value(QStringLiteral("audioSampleRate")).toInt();
    v.audioChannels       = o.value(QStringLiteral("audioChannels")).toInt();
    v.hasEmbeddedTimecode = o.value(QStringLiteral("hasEmbeddedTimecode")).toBool();
    v.embeddedTimecode    = o.value(QStringLiteral("embeddedTimecode")).toString();
    // Empty string when missing — matches the struct's default; old
    // projects without this field load with a blank layout name and
    // the inspector falls back to "<n> channels" derived from count.
    v.audioChannelLayoutName =
        o.value(QStringLiteral("audioChannelLayoutName")).toString();
    return v;
}

QJsonObject adobeMetadataToJson(const AdobeMetadata &a)
{
    QJsonObject o;
    o[QStringLiteral("loaded")]             = a.loaded;
    o[QStringLiteral("aeProjectPath")]      = a.aeProjectPath;
    o[QStringLiteral("premiereWinPath")]    = a.premiereWinPath;
    o[QStringLiteral("premiereMacPath")]    = a.premiereMacPath;
    o[QStringLiteral("qtStartTimecode")]    = a.qtStartTimecode;
    o[QStringLiteral("qtTimecode")]         = a.qtTimecode;
    o[QStringLiteral("qtCreationDate")]     = a.qtCreationDate;
    o[QStringLiteral("qtMediaCreateDate")]  = a.qtMediaCreateDate;
    o[QStringLiteral("mxfStartTimecode")]   = a.mxfStartTimecode;
    o[QStringLiteral("mxfTimecodeAtStart")] = a.mxfTimecodeAtStart;
    o[QStringLiteral("xmpStartTimecode")]   = a.xmpStartTimecode;
    o[QStringLiteral("xmpAltTimecode")]     = a.xmpAltTimecode;
    o[QStringLiteral("userdataTimecode")]   = a.userdataTimecode;
    return o;
}

AdobeMetadata adobeMetadataFromJson(const QJsonObject &o)
{
    AdobeMetadata a;
    a.loaded             = o.value(QStringLiteral("loaded")).toBool();
    a.aeProjectPath      = o.value(QStringLiteral("aeProjectPath")).toString();
    a.premiereWinPath    = o.value(QStringLiteral("premiereWinPath")).toString();
    a.premiereMacPath    = o.value(QStringLiteral("premiereMacPath")).toString();
    a.qtStartTimecode    = o.value(QStringLiteral("qtStartTimecode")).toString();
    a.qtTimecode         = o.value(QStringLiteral("qtTimecode")).toString();
    a.qtCreationDate     = o.value(QStringLiteral("qtCreationDate")).toString();
    a.qtMediaCreateDate  = o.value(QStringLiteral("qtMediaCreateDate")).toString();
    a.mxfStartTimecode   = o.value(QStringLiteral("mxfStartTimecode")).toString();
    a.mxfTimecodeAtStart = o.value(QStringLiteral("mxfTimecodeAtStart")).toString();
    a.xmpStartTimecode   = o.value(QStringLiteral("xmpStartTimecode")).toString();
    a.xmpAltTimecode     = o.value(QStringLiteral("xmpAltTimecode")).toString();
    a.userdataTimecode   = o.value(QStringLiteral("userdataTimecode")).toString();
    return a;
}

QJsonObject rangeToJson(const InOutRange &r)
{
    QJsonObject o;
    o[QStringLiteral("inPoint")]  = r.inPoint;
    o[QStringLiteral("outPoint")] = r.outPoint;
    return o;
}

InOutRange rangeFromJson(const QJsonObject &o)
{
    InOutRange r;
    r.inPoint  = o.value(QStringLiteral("inPoint")).toDouble(-1.0);
    r.outPoint = o.value(QStringLiteral("outPoint")).toDouble(-1.0);
    return r;
}

QJsonObject imageSeqToJson(const ImageSequenceData &s, const QString &projectDir)
{
    QJsonObject o;
    o[QStringLiteral("pattern")]       = s.pattern;
    o[QStringLiteral("ffmpegPattern")] = collapseProjectDir(s.ffmpegPattern, projectDir);
    o[QStringLiteral("directory")]     = collapseProjectDir(s.directory, projectDir);
    o[QStringLiteral("frameCount")]    = s.frameCount;
    o[QStringLiteral("startFrame")]    = s.startFrame;
    o[QStringLiteral("endFrame")]      = s.endFrame;
    o[QStringLiteral("frameRate")]     = s.frameRate;
    o[QStringLiteral("duration")]      = s.duration;
    o[QStringLiteral("width")]         = s.width;
    o[QStringLiteral("height")]        = s.height;
    o[QStringLiteral("format")]        = s.format;
    o[QStringLiteral("layer")]         = s.layer;
    QJsonArray layers;
    for (const QString &name : s.availableLayers) layers.append(name);
    o[QStringLiteral("availableLayers")] = layers;
    o[QStringLiteral("compression")]   = s.compression;
    o[QStringLiteral("audioFile")]     = collapseProjectDir(s.audioFile, projectDir);
    // Per-clip cache stride preference. Field has been on the struct
    // since the per-item-stride feature shipped (in-memory only at
    // first); now persisted so the user's "downshift this 4K seq to
    // 3x" choice survives save/reopen instead of resetting to 1.
    o[QStringLiteral("cacheStride")]   = s.cacheStride;
    return o;
}

ImageSequenceData imageSeqFromJson(const QJsonObject &o, const QString &projectDir)
{
    ImageSequenceData s;
    s.pattern       = o.value(QStringLiteral("pattern")).toString();
    s.ffmpegPattern = expandProjectDir(
        o.value(QStringLiteral("ffmpegPattern")).toString(), projectDir);
    s.directory     = expandProjectDir(
        o.value(QStringLiteral("directory")).toString(), projectDir);
    s.frameCount    = o.value(QStringLiteral("frameCount")).toInt();
    s.startFrame    = o.value(QStringLiteral("startFrame")).toInt(1);
    s.endFrame      = o.value(QStringLiteral("endFrame")).toInt(1);
    s.frameRate     = o.value(QStringLiteral("frameRate")).toDouble(23.976);
    s.duration      = o.value(QStringLiteral("duration")).toDouble();
    s.width         = o.value(QStringLiteral("width")).toInt();
    s.height        = o.value(QStringLiteral("height")).toInt();
    s.format        = o.value(QStringLiteral("format")).toString();
    s.layer         = o.value(QStringLiteral("layer")).toString();
    const QJsonArray layers = o.value(QStringLiteral("availableLayers")).toArray();
    for (const QJsonValue &v : layers) s.availableLayers.append(v.toString());
    s.compression   = o.value(QStringLiteral("compression")).toString();
    s.audioFile     = expandProjectDir(
        o.value(QStringLiteral("audioFile")).toString(), projectDir);
    // Default 1 (Every Frame) when missing — matches the struct's
    // in-memory default, so old projects without this field load
    // identically to before.
    s.cacheStride   = o.value(QStringLiteral("cacheStride")).toInt(1);
    return s;
}

QJsonObject playlistToJson(const PlaylistData &p)
{
    QJsonObject o;
    o[QStringLiteral("masterFps")]        = p.masterFps;
    o[QStringLiteral("canvasWidth")]      = p.canvasWidth;
    o[QStringLiteral("canvasHeight")]     = p.canvasHeight;
    o[QStringLiteral("defaultGapFrames")] = p.defaultGapFrames;
    o[QStringLiteral("loop")]             = p.loop;
    QJsonArray items;
    for (const PlaylistEntry &e : p.items) {
        QJsonObject em;
        em[QStringLiteral("mediaId")] = e.mediaId;
        em[QStringLiteral("range")]   = rangeToJson(e.range);
        items.append(em);
    }
    o[QStringLiteral("items")] = items;
    return o;
}

PlaylistData playlistFromJson(const QJsonObject &o)
{
    PlaylistData p;
    p.masterFps        = o.value(QStringLiteral("masterFps")).toDouble(24.0);
    p.canvasWidth      = o.value(QStringLiteral("canvasWidth")).toInt(1920);
    p.canvasHeight     = o.value(QStringLiteral("canvasHeight")).toInt(1080);
    p.defaultGapFrames = o.value(QStringLiteral("defaultGapFrames")).toInt(10);
    p.loop             = o.value(QStringLiteral("loop")).toBool();
    const QJsonArray items = o.value(QStringLiteral("items")).toArray();
    for (const QJsonValue &v : items) {
        const QJsonObject em = v.toObject();
        PlaylistEntry e;
        e.mediaId = em.value(QStringLiteral("mediaId")).toString();
        e.range   = rangeFromJson(em.value(QStringLiteral("range")).toObject());
        p.items.append(e);
    }
    return p;
}

QJsonObject dualPairToJson(const DualPairData &d)
{
    QJsonObject o;
    o[QStringLiteral("mediaIdA")]       = d.mediaIdA;
    o[QStringLiteral("mediaIdB")]       = d.mediaIdB;
    o[QStringLiteral("masterFps")]      = d.masterFps;
    o[QStringLiteral("masterDuration")] = d.masterDuration;
    o[QStringLiteral("clipsA")] = QJsonArray::fromVariantList(d.clipsA);
    o[QStringLiteral("clipsB")] = QJsonArray::fromVariantList(d.clipsB);
    o[QStringLiteral("inPoint")]  = d.inPoint;
    o[QStringLiteral("outPoint")] = d.outPoint;
    return o;
}

DualPairData dualPairFromJson(const QJsonObject &o)
{
    DualPairData d;
    d.mediaIdA       = o.value(QStringLiteral("mediaIdA")).toString();
    d.mediaIdB       = o.value(QStringLiteral("mediaIdB")).toString();
    d.masterFps      = o.value(QStringLiteral("masterFps")).toDouble(24.0);
    d.masterDuration = o.value(QStringLiteral("masterDuration")).toDouble();
    d.clipsA         = o.value(QStringLiteral("clipsA")).toArray().toVariantList();
    d.clipsB         = o.value(QStringLiteral("clipsB")).toArray().toVariantList();
    d.inPoint        = o.value(QStringLiteral("inPoint")).toInt(-1);
    d.outPoint       = o.value(QStringLiteral("outPoint")).toInt(-1);
    return d;
}

// ---- MediaItem (de)serialization -------------------------------

QJsonObject mediaItemToJson(const MediaItem &it, const QString &projectDir)
{
    QJsonObject o;
    o[QStringLiteral("id")]            = it.id;
    o[QStringLiteral("name")]          = it.name;
    o[QStringLiteral("path")]          = collapseProjectDir(it.path, projectDir);
    o[QStringLiteral("type")]          = mediaTypeToString(it.type);
    o[QStringLiteral("duration")]      = it.duration;
    o[QStringLiteral("hasAudio")]      = it.hasAudio;
    o[QStringLiteral("thumbnailPath")] = collapseProjectDir(it.thumbnailPath, projectDir);
    o[QStringLiteral("range")]         = rangeToJson(it.range);
    o[QStringLiteral("imageSeq")]      = imageSeqToJson(it.imageSeq, projectDir);
    o[QStringLiteral("playlist")]      = playlistToJson(it.playlist);
    o[QStringLiteral("video")]         = videoMetadataToJson(it.video);
    o[QStringLiteral("adobe")]         = adobeMetadataToJson(it.adobe);
    o[QStringLiteral("videoRangeOverride")] =
        videoRangeToString(it.videoRangeOverride);
    o[QStringLiteral("pixelAspectMode")] =
        static_cast<int>(it.pixelAspectMode);
    o[QStringLiteral("customParNum")]  = it.customParNum;
    o[QStringLiteral("customParDen")]  = it.customParDen;
    o[QStringLiteral("audioRoutingMode")] =
        static_cast<int>(it.audioRoutingMode);
    o[QStringLiteral("dualPair")]      = dualPairToJson(it.dualPair);
    return o;
}

MediaItem mediaItemFromJson(const QJsonObject &o, const QString &projectDir)
{
    MediaItem it;
    it.id            = o.value(QStringLiteral("id")).toString();
    it.name          = o.value(QStringLiteral("name")).toString();
    it.path          = expandProjectDir(
        o.value(QStringLiteral("path")).toString(), projectDir);
    it.type          = mediaTypeFromString(o.value(QStringLiteral("type")).toString());
    it.duration      = o.value(QStringLiteral("duration")).toDouble();
    it.hasAudio      = o.value(QStringLiteral("hasAudio")).toBool();
    it.thumbnailPath = expandProjectDir(
        o.value(QStringLiteral("thumbnailPath")).toString(), projectDir);
    it.range         = rangeFromJson(o.value(QStringLiteral("range")).toObject());
    it.imageSeq      = imageSeqFromJson(o.value(QStringLiteral("imageSeq")).toObject(),
                                        projectDir);
    it.playlist      = playlistFromJson(o.value(QStringLiteral("playlist")).toObject());
    it.video         = videoMetadataFromJson(o.value(QStringLiteral("video")).toObject());
    it.adobe         = adobeMetadataFromJson(o.value(QStringLiteral("adobe")).toObject());
    it.videoRangeOverride = videoRangeFromString(
        o.value(QStringLiteral("videoRangeOverride")).toString());
    // Default Square (0) when missing — matches the struct default;
    // custom PAR defaults 1/1 and is only honored when mode == Custom.
    it.pixelAspectMode = static_cast<PixelAspectMode>(
        o.value(QStringLiteral("pixelAspectMode")).toInt(0));
    it.customParNum = o.value(QStringLiteral("customParNum")).toInt(1);
    it.customParDen = o.value(QStringLiteral("customParDen")).toInt(1);
    if (it.customParNum <= 0 || it.customParDen <= 0) {
        it.customParNum = 1; it.customParDen = 1;
    }
    // Default Auto (0) when missing — matches the struct's default
    // and what the FFmpeg-extractor would have set on a new add.
    it.audioRoutingMode = static_cast<AudioRoutingMode>(
        o.value(QStringLiteral("audioRoutingMode")).toInt(0));
    it.dualPair      = dualPairFromJson(o.value(QStringLiteral("dualPair")).toObject());
    return it;
}

// ---- Bin (de)serialization -------------------------------------

QString binTypeToString(MediaType t)
{
    // Bins persist their accept-type alongside their name so a v3
    // schema that adds bins doesn't desync existing files.
    return mediaTypeToString(t);
}

QJsonObject binToJson(const ProjectBin &b)
{
    QJsonObject o;
    o[QStringLiteral("name")] = b.name;
    o[QStringLiteral("type")] = binTypeToString(b.accepts);
    o[QStringLiteral("open")] = b.isOpen;
    QJsonArray ids;
    for (const QString &id : b.itemIds) ids.append(id);
    o[QStringLiteral("item_ids")] = ids;
    return o;
}

} // namespace

// =================================================================
// save
// =================================================================

bool save(ProjectManager &pm, const QString &filePath, QString *errorOut)
{
    if (filePath.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Save: empty path.");
        return false;
    }

    const QFileInfo fi(filePath);
    const QString projectDir = fi.absolutePath();

    QJsonObject root;
    root[QStringLiteral("schema")]  = QString::fromLatin1(kSchemaTag);
    root[QStringLiteral("version")] = kSchemaVersion;

    QJsonObject project;
    project[QStringLiteral("uuid")]        = pm.projectUuid();
    project[QStringLiteral("name")]        = pm.projectName();
    project[QStringLiteral("created_at")]  = pm.projectCreatedAt();
    // modified_at is what's-on-disk-as-of-this-write. ProjectManager
    // refreshes its in-memory value after save() returns; the file
    // gets the freshly-stamped value here.
    project[QStringLiteral("modified_at")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root[QStringLiteral("project")] = project;

    QJsonArray media;
    for (const MediaItem &it : pm.mediaPool()) {
        media.append(mediaItemToJson(it, projectDir));
    }
    root[QStringLiteral("media")] = media;

    QJsonArray bins;
    for (const ProjectBin &b : pm.binsRaw()) {
        bins.append(binToJson(b));
    }
    root[QStringLiteral("bins")] = bins;

    QJsonObject ui;
    ui[QStringLiteral("active_item_id")]    = pm.activeItemId();
    ui[QStringLiteral("b_source_media_id")] = pm.bSourceMediaId();
    root[QStringLiteral("ui_state")] = ui;

    const QJsonDocument doc(root);
    const QByteArray bytes = doc.toJson(QJsonDocument::Indented);

    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Save: cannot open '%1' for write: %2")
                            .arg(filePath, f.errorString());
        }
        return false;
    }
    if (f.write(bytes) != bytes.size()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Save: write to '%1' truncated: %2")
                            .arg(filePath, f.errorString());
        }
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Save: commit to '%1' failed: %2")
                            .arg(filePath, f.errorString());
        }
        return false;
    }
    return true;
}

// =================================================================
// load
// =================================================================

bool load(ProjectManager &pm, const QString &filePath, QString *errorOut)
{
    auto fail = [&](const QString &msg) -> bool {
        // Per the contract, leave pm in a clean newProject state on
        // failure so the UI doesn't show a half-loaded project.
        pm.newProject();
        if (errorOut) *errorOut = msg;
        return false;
    };

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("Open: cannot read '%1': %2")
                       .arg(filePath, f.errorString()));
    }
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (doc.isNull() || !doc.isObject()) {
        return fail(QStringLiteral("Open: parse error in '%1': %2")
                       .arg(filePath, pe.errorString()));
    }
    const QJsonObject root = doc.object();
    const QString schema = root.value(QStringLiteral("schema")).toString();
    if (schema != QString::fromLatin1(kSchemaTag)) {
        return fail(QStringLiteral(
            "Open: '%1' is not a v2 .qcvproj (schema='%2')")
            .arg(filePath, schema));
    }

    const QFileInfo fi(filePath);
    const QString projectDir = fi.absolutePath();

    const QJsonObject project = root.value(QStringLiteral("project")).toObject();
    QString uuid       = project.value(QStringLiteral("uuid")).toString();
    QString name       = project.value(QStringLiteral("name")).toString();
    QString createdAt  = project.value(QStringLiteral("created_at")).toString();
    QString modifiedAt = project.value(QStringLiteral("modified_at")).toString();

    QList<MediaItem> pool;
    const QJsonArray mediaArr = root.value(QStringLiteral("media")).toArray();
    pool.reserve(mediaArr.size());
    for (const QJsonValue &v : mediaArr) {
        if (!v.isObject()) continue;
        pool.append(mediaItemFromJson(v.toObject(), projectDir));
    }

    QList<ProjectBin> bins;
    const QJsonArray binsArr = root.value(QStringLiteral("bins")).toArray();
    bins.reserve(binsArr.size());
    for (const QJsonValue &v : binsArr) {
        if (!v.isObject()) continue;
        const QJsonObject bo = v.toObject();
        ProjectBin b;
        b.name    = bo.value(QStringLiteral("name")).toString();
        b.accepts = mediaTypeFromString(bo.value(QStringLiteral("type")).toString());
        b.isOpen  = bo.value(QStringLiteral("open")).toBool();
        const QJsonArray ids = bo.value(QStringLiteral("item_ids")).toArray();
        for (const QJsonValue &iv : ids) {
            const QString id = iv.toString();
            if (!id.isEmpty()) b.itemIds.append(id);
        }
        bins.append(b);
    }

    // Defensive default: if the file has no bins block, fall back
    // to the canonical seeded layout so the QML LeftRail still renders.
    if (bins.isEmpty()) {
        qWarning("ProjectIo::load: no bins in file; seeding canonical defaults");
        auto seed = [&](const QString &n, MediaType t, bool open) {
            ProjectBin b; b.name = n; b.accepts = t; b.isOpen = open;
            bins.append(b);
        };
        seed(QStringLiteral("Videos"),     MediaType::Video,    true);
        seed(QStringLiteral("Audio"),      MediaType::Audio,    false);
        seed(QStringLiteral("Images"),     MediaType::Image,    false);
        seed(QStringLiteral("Playlists"),  MediaType::Playlist, false);
        seed(QStringLiteral("Dual Views"), MediaType::DualPair, false);
    }

    const QJsonObject ui = root.value(QStringLiteral("ui_state")).toObject();
    QString activeId  = ui.value(QStringLiteral("active_item_id")).toString();
    QString bSourceId = ui.value(QStringLiteral("b_source_media_id")).toString();

    // Drop selection refs that don't resolve into the loaded pool —
    // setActiveItem would bail otherwise and leave the UI confused.
    auto inPool = [&](const QString &id) {
        if (id.isEmpty()) return false;
        for (const MediaItem &m : pool) if (m.id == id) return true;
        return false;
    };
    if (!inPool(activeId))  activeId.clear();
    if (!inPool(bSourceId)) bSourceId.clear();

    pm.applyLoadedState(std::move(pool),
                        std::move(bins),
                        std::move(activeId),
                        std::move(bSourceId),
                        std::move(uuid),
                        std::move(name),
                        std::move(createdAt),
                        std::move(modifiedAt));
    return true;
}

} // namespace qcv::ProjectIo
