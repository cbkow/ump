// probe-metadata — see CMakeLists.txt.

#include "project/ffmpeg_metadata_extractor.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStringList>

#include <cstdio>

using qcv::FFmpegMetadataExtractor;
using qcv::VideoMetadata;

static void dump(const VideoMetadata &m)
{
    // Keep the byte arrays alive for the duration of each printf.
    auto s = [](const QString &v) {
        return v.isEmpty() ? QByteArray("-") : v.toUtf8();
    };
    std::printf("  loaded            %s\n", m.loaded ? "yes" : "NO");
    std::printf("  container         %s\n", s(m.containerFormat).constData());
    std::printf("  mxf OP            %s\n", s(m.mxfOperationalPattern).constData());
    std::printf("  encoder           %s\n", s(m.encoderTool).constData());
    std::printf("  codec / profile   %s / %s\n", s(m.videoCodec).constData(), s(m.codecProfile).constData());
    std::printf("  size / fps        %dx%d @ %.3f  (%d frames, %.3fs)\n",
                m.width, m.height, m.frameRate, m.totalFrames, m.duration);
    std::printf("  pixfmt / depth    %s / %d-bit%s%s\n", s(m.pixelFormat).constData(), m.bitDepth,
                m.hasAlpha ? " +alpha" : "", m.isRgb ? " (RGB)" : "");
    std::printf("  range             container=%s effective=%s\n",
                s(m.containerRangeTag).constData(), s(m.colorRange).constData());
    std::printf("  bitrate video     %.2f Mb/s\n", m.videoBitrate / 1e6);
    std::printf("  bitrate file avg  %.2f Mb/s\n", m.containerBitrate / 1e6);
    std::printf("  audio             %s %d Hz %dch (%d streams) %s\n",
                s(m.audioCodec).constData(), m.audioSampleRate, m.audioChannels,
                m.audioStreamCount, s(m.audioChannelLayoutName).constData());
    std::printf("  timecode          %s\n", s(m.embeddedTimecode).constData());
    std::printf("  vendor / model    %s / %s\n", s(m.cameraVendor).constData(), s(m.cameraModel).constData());
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments();
    args.removeFirst();
    bool probeOnly = false;
    if (!args.isEmpty() && args.first() == QLatin1String("--probe")) {
        probeOnly = true;
        args.removeFirst();
    }
    if (args.isEmpty()) {
        std::fprintf(stderr, "usage: probe-metadata [--probe] <file> [file…]\n");
        return 2;
    }
    for (const QString &path : args) {
        std::printf("== %s\n", qPrintable(QFileInfo(path).fileName()));
        const VideoMetadata m = probeOnly
            ? FFmpegMetadataExtractor::probeContainer(path)
            : FFmpegMetadataExtractor::extract(path);
        dump(m);
    }
    return 0;
}
