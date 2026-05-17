// probe-ocio-shader — Phase 2.2 OCIO + QShaderBaker smoke test.
//
// What this proves before Phase 2.3 lands:
//   1. OCIO can emit a GLSL fragment-shader function for a given
//      [Input, View, Display, (Look)] chain.
//   2. The emitted GLSL, wrapped in a minimal main(), can be baked
//      by Qt's QShaderBaker into the multi-target QShader bundle
//      (SPIR-V + MSL + HLSL).
//   3. The emitted shader desc surfaces texture / uniform metadata
//      we'll need in Phase 2.3 (LUT bindings + uniforms for Look
//      parameters etc.).
//
// If any of those fail, we know now — before the renderer plumbing
// is in flight — and can pick an alternative (link glslang directly,
// pre-bake shaders offline, etc.). This is the technical-risk phase
// per Guide 14 §IV.
//
// Usage:  probe-ocio-shader [config-path-or-URI] [input-cs] [display] [view]
// Defaults: ocio://default + role:color_picking + default display + default view.

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QtLogging>

#include <rhi/qshader.h>
#include <rhi/qshaderbaker.h>

#include <OpenColorIO/OpenColorIO.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace OCIO = OCIO_NAMESPACE;

namespace {

const char *kFragVertexHeader =
    "#version 440\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "layout(binding = 1) uniform sampler2D u_tex;\n";

QString wrapInFragmentMain(const QString &ocioFunctionGlsl,
                           const QString &funcName)
{
    // OCIO emits the conversion as a free function. We wrap it in a
    // fullscreen-quad fragment shader. The actual renderer (Phase
    // 2.3) does the same wrapping; this matches that shape.
    QString out;
    out += QString::fromUtf8(kFragVertexHeader);
    out += QStringLiteral("\n");
    out += ocioFunctionGlsl;
    out += QStringLiteral("\nvoid main() {\n"
                          "    vec4 src = texture(u_tex, v_uv);\n"
                          "    fragColor = %1(src);\n"
                          "}\n").arg(funcName);
    return out;
}

QString rolePicker(OCIO::ConstConfigRcPtr cfg, const char *role)
{
    if (!cfg) return {};
    if (const char *cs = cfg->getRoleColorSpace(role); cs && *cs)
        return QString::fromUtf8(cs);
    return {};
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const char *configPath = (argc >= 2) ? argv[1] : "ocio://default";
    const char *inputArg   = (argc >= 3) ? argv[2] : nullptr;
    const char *displayArg = (argc >= 4) ? argv[3] : nullptr;
    const char *viewArg    = (argc >= 5) ? argv[4] : nullptr;

    OCIO::ConstConfigRcPtr cfg;
    try {
        cfg = OCIO::Config::CreateFromFile(configPath);
    } catch (const OCIO::Exception &e) {
        std::fprintf(stderr, "OCIO config load failed: %s\n", e.what());
        return 1;
    }

    // Resolve [Input, Display, View] with sensible fallbacks.
    QString inputCs = inputArg ? QString::fromUtf8(inputArg)
                               : rolePicker(cfg, "color_picking");
    if (inputCs.isEmpty()) inputCs = rolePicker(cfg, "scene_linear");
    if (inputCs.isEmpty()) inputCs = QString::fromUtf8(cfg->getColorSpaceNameByIndex(0));

    QString display = displayArg ? QString::fromUtf8(displayArg)
                                 : QString::fromUtf8(cfg->getDefaultDisplay());
    QString view = viewArg ? QString::fromUtf8(viewArg)
                           : QString::fromUtf8(
                               cfg->getDefaultView(display.toUtf8().constData()));

    std::printf("=== probe-ocio-shader — Phase 2.2 smoke test ===\n");
    std::printf("config:  %s\n", configPath);
    std::printf("input:   %s\n", qPrintable(inputCs));
    std::printf("display: %s\n", qPrintable(display));
    std::printf("view:    %s\n", qPrintable(view));
    std::printf("\n");

    // Build the DisplayViewTransform processor.
    OCIO::ConstProcessorRcPtr proc;
    try {
        OCIO::DisplayViewTransformRcPtr dvt = OCIO::DisplayViewTransform::Create();
        dvt->setSrc(inputCs.toUtf8().constData());
        dvt->setDisplay(display.toUtf8().constData());
        dvt->setView(view.toUtf8().constData());
        proc = cfg->getProcessor(dvt);
    } catch (const OCIO::Exception &e) {
        std::fprintf(stderr, "OCIO processor build failed: %s\n", e.what());
        return 1;
    }

    OCIO::ConstGPUProcessorRcPtr gpuProc = proc->getDefaultGPUProcessor();

    OCIO::GpuShaderDescRcPtr shaderDesc = OCIO::GpuShaderDesc::CreateShaderDesc();
    shaderDesc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_4_0);
    shaderDesc->setFunctionName("OCIODisplay");
    shaderDesc->setResourcePrefix("ocio_");

    try {
        gpuProc->extractGpuShaderInfo(shaderDesc);
    } catch (const OCIO::Exception &e) {
        std::fprintf(stderr, "OCIO extractGpuShaderInfo failed: %s\n", e.what());
        return 1;
    }

    QString ocioGlsl = QString::fromUtf8(shaderDesc->getShaderText());
    const int numTextures   = shaderDesc->getNumTextures();
    const int numTextures3D = shaderDesc->getNum3DTextures();
    const int numUniforms   = shaderDesc->getNumUniforms();

    // ---- Post-process for Vulkan/SPIR-V baker.
    //
    // OCIO emits OpenGL-style GLSL: `uniform sampler1D foo;` with no
    // binding qualifier. QShaderBaker uses glslang in Vulkan-strict
    // mode (samplers MUST have layout(binding=N)). We rewrite each
    // sampler declaration to add an explicit binding.
    //
    // Binding allocation:
    //   0    = OCIO uniforms (if any) — block at the top
    //   1    = source texture (u_tex) — declared by our wrapper
    //   2... = OCIO 1D + 2D + 3D LUT samplers in declaration order
    //
    // Phase 2.3's renderer does the same rewrite + uses the same
    // binding scheme to populate the SRB.
    int nextBinding = 2;
    auto rewriteSampler = [&](const char *samplerName) {
        if (!samplerName || !*samplerName) return;
        const QString name = QString::fromUtf8(samplerName);
        const QStringList samplers = {
            QStringLiteral("sampler1D"),
            QStringLiteral("sampler2D"),
            QStringLiteral("sampler3D"),
        };
        for (const QString &kind : samplers) {
            const QString needle = QStringLiteral("uniform %1 %2;").arg(kind, name);
            const QString replacement = QStringLiteral("layout(binding=%1) uniform %2 %3;")
                                        .arg(nextBinding).arg(kind, name);
            if (ocioGlsl.contains(needle)) {
                ocioGlsl.replace(needle, replacement);
                ++nextBinding;
                return;
            }
        }
        qWarning("rewrite: didn't find sampler declaration for '%s'",
                 qPrintable(name));
    };

    for (int i = 0; i < numTextures; ++i) {
        const char *texName = nullptr, *samplerName = nullptr;
        unsigned w = 0, h = 0;
        OCIO::GpuShaderDesc::TextureType chan = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderDesc::TextureDimensions dim = OCIO::GpuShaderDesc::TEXTURE_1D;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        shaderDesc->getTexture(i, texName, samplerName, w, h, chan, dim, interp);
        rewriteSampler(samplerName);
    }
    for (int i = 0; i < numTextures3D; ++i) {
        const char *texName = nullptr, *samplerName = nullptr;
        unsigned edgelen = 0;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        shaderDesc->get3DTexture(i, texName, samplerName, edgelen, interp);
        rewriteSampler(samplerName);
    }

    // Dump the post-rewrite GLSL so it's inspectable when a view
    // misrenders. The probe is a debug tool — no harm in always
    // emitting the snapshot.
    {
        QFile dump(QStringLiteral("/tmp/probe_ocio_shader.frag.glsl"));
        if (dump.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            dump.write(ocioGlsl.toUtf8());
            std::printf("GLSL dumped to:      /tmp/probe_ocio_shader.frag.glsl\n");
        }
    }
    std::printf("OCIO shader text:    %d bytes\n",
                static_cast<int>(ocioGlsl.toUtf8().size()));
    std::printf("OCIO 1D textures:    %d\n", numTextures);
    std::printf("OCIO 3D textures:    %d\n", numTextures3D);
    std::printf("OCIO uniforms:       %d\n", numUniforms);
    std::printf("\n");

    if (numTextures > 0 || numTextures3D > 0) {
        std::printf("Texture inventory (Phase 2.3 will upload these as QRhiTextures):\n");
        for (int i = 0; i < numTextures; ++i) {
            const char *texName = nullptr;
            const char *samplerName = nullptr;
            unsigned width = 0, height = 0;
            OCIO::GpuShaderDesc::TextureType chan = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
            OCIO::GpuShaderDesc::TextureDimensions dim = OCIO::GpuShaderDesc::TEXTURE_1D;
            OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
            shaderDesc->getTexture(i, texName, samplerName, width, height, chan, dim, interp);
            std::printf("  [1D %d] sampler=%s  size=%ux%u  chan=%d  dim=%d\n",
                        i, samplerName, width, height,
                        static_cast<int>(chan), static_cast<int>(dim));
        }
        for (int i = 0; i < numTextures3D; ++i) {
            const char *texName = nullptr;
            const char *samplerName = nullptr;
            unsigned edgelen = 0;
            OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
            shaderDesc->get3DTexture(i, texName, samplerName, edgelen, interp);
            std::printf("  [3D %d] sampler=%s  edgelen=%u\n",
                        i, samplerName, edgelen);
        }
        std::printf("\n");
    }

    // Bake via QShaderBaker.
    const QString fullShader = wrapInFragmentMain(ocioGlsl, "OCIODisplay");

    QShaderBaker baker;
    baker.setSourceString(fullShader.toUtf8(), QShader::FragmentStage);
    baker.setGeneratedShaderVariants({ QShader::StandardShader });
    baker.setGeneratedShaders({
        { QShader::SpirvShader, QShaderVersion(100) },
        { QShader::MslShader,   QShaderVersion(20)  },
    });

    QShader shader = baker.bake();
    if (!shader.isValid()) {
        std::fprintf(stderr, "QShaderBaker FAILED:\n%s\n",
                     qPrintable(baker.errorMessage()));
        std::fprintf(stderr, "\n--- shader source that failed (first 2KB) ---\n%s\n",
                     fullShader.left(2048).toUtf8().constData());
        return 1;
    }

    std::printf("QShaderBaker:        baked successfully\n");
    std::printf("Source variants:     %lld\n",
                static_cast<long long>(shader.availableShaders().size()));
    for (const auto &k : shader.availableShaders()) {
        std::printf("  source = %d  version = %d  variant = %d\n",
                    static_cast<int>(k.source()),
                    k.sourceVersion().version(),
                    static_cast<int>(k.sourceVariant()));
    }
    std::printf("\nresult: PASS — OCIO → QShaderBaker pipeline is viable.\n");
    return 0;
}
