#include "ocio_shader_builder.h"

#include "color/ocio_chain_builder.h"

#include <QtLogging>
#include <rhi/qrhi.h>
#include <rhi/qshaderbaker.h>

#include <OpenColorIO/OpenColorIO.h>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

namespace {

const char *kFragHeader =
    "#version 440\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "layout(binding = 1) uniform sampler2D u_tex;\n"
    "\n";

QString wrapInFragmentMain(const QString &ocioFunctionGlsl)
{
    QString out;
    out += QString::fromUtf8(kFragHeader);
    out += ocioFunctionGlsl;
    out += QStringLiteral("\nvoid main() {\n"
                          "    vec4 src = texture(u_tex, v_uv);\n"
                          "    fragColor = OCIODisplay(src);\n"
                          "}\n");
    return out;
}

// Wrap every `texture(<sampler>, <expr>)` call where the sampler is
// in `names1D` so that <expr> becomes `vec2(<expr>, 0.5)`. We promote
// 1D samplers to 2D (because QRhi has no 1D textures) and need their
// float args to become vec2 to match the new sampler type. Uses a
// balanced-paren walker to handle expressions like
// `texture(s, (i_lo + 0.5) / 363)` correctly — a regex with `[^)]+`
// would stop at the first inner `)`.
QString wrap1DTextureCalls(QString glsl, const QStringList &names1D)
{
    for (const QString &name : names1D) {
        const QString needle = QStringLiteral("texture(%1,").arg(name);
        int pos = 0;
        while ((pos = glsl.indexOf(needle, pos)) != -1) {
            int p = pos + needle.length();
            while (p < glsl.length() && glsl[p].isSpace()) ++p;
            int q = p;
            int depth = 1;
            while (q < glsl.length() && depth > 0) {
                const QChar c = glsl[q];
                if (c == QLatin1Char('(')) ++depth;
                else if (c == QLatin1Char(')')) --depth;
                if (depth > 0) ++q;
            }
            if (depth != 0) break;
            const QString arg = glsl.mid(p, q - p);
            const QString replacement = QStringLiteral("texture(%1, vec2(%2, 0.5))")
                                        .arg(name, arg);
            glsl.replace(pos, q - pos + 1, replacement);
            pos += replacement.length();
        }
    }
    return glsl;
}

// Inject layout(binding=N) qualifiers into OCIO's bare sampler
// declarations and promote 1D samplers to 2D. QRhi has no 1D texture
// type, so we declare samplers as sampler2D and bind row-1 2D
// textures (Nx1). The corresponding texture() call sites get their
// float args rewritten to vec2(arg, 0.5) by wrap1DTextureCalls.
QString rewriteBindings(QString glsl,
                        OCIO::ConstGpuShaderDescRcPtr desc,
                        int firstBinding)
{
    int next = firstBinding;
    QStringList names1D;

    for (int i = 0; i < desc->getNumTextures(); ++i) {
        const char *texName = nullptr, *samplerName = nullptr;
        unsigned w = 0, h = 0;
        OCIO::GpuShaderDesc::TextureType chan = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderDesc::TextureDimensions dim = OCIO::GpuShaderDesc::TEXTURE_1D;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->getTexture(i, texName, samplerName, w, h, chan, dim, interp);
        if (!samplerName || !*samplerName) continue;
        const QString name = QString::fromUtf8(samplerName);

        if (dim == OCIO::GpuShaderDesc::TEXTURE_1D) {
            const QString needle = QStringLiteral("uniform sampler1D %1;").arg(name);
            const QString repl = QStringLiteral("layout(binding=%1) uniform sampler2D %2;")
                                 .arg(next).arg(name);
            if (glsl.contains(needle)) {
                glsl.replace(needle, repl);
                names1D << name;
                ++next;
            } else {
                qWarning("OCIO rewrite: 1D declaration not found for '%s'",
                         qPrintable(name));
            }
        } else {
            // dim == TEXTURE_2D — already sampler2D; just add binding.
            const QString needle = QStringLiteral("uniform sampler2D %1;").arg(name);
            const QString repl = QStringLiteral("layout(binding=%1) uniform sampler2D %2;")
                                 .arg(next).arg(name);
            if (glsl.contains(needle)) {
                glsl.replace(needle, repl);
                ++next;
            } else {
                qWarning("OCIO rewrite: 2D declaration not found for '%s'",
                         qPrintable(name));
            }
        }
    }

    for (int i = 0; i < desc->getNum3DTextures(); ++i) {
        const char *texName = nullptr, *samplerName = nullptr;
        unsigned edgelen = 0;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->get3DTexture(i, texName, samplerName, edgelen, interp);
        if (!samplerName || !*samplerName) continue;
        const QString name = QString::fromUtf8(samplerName);
        const QString needle = QStringLiteral("uniform sampler3D %1;").arg(name);
        const QString repl = QStringLiteral("layout(binding=%1) uniform sampler3D %2;")
                             .arg(next).arg(name);
        if (glsl.contains(needle)) {
            glsl.replace(needle, repl);
            ++next;
        }
    }

    glsl = wrap1DTextureCalls(std::move(glsl), names1D);
    return glsl;
}

} // namespace

OcioShaderBuilder::OcioShaderBuilder(QRhi *rhi)
    : m_rhi(rhi)
{}

OcioShaderBuilder::~OcioShaderBuilder() = default;

OcioShaderBuilder::Built
OcioShaderBuilder::build(OCIOConfigManager *ocio, QRhiResourceUpdateBatch *uploadBatch)
{
    Built result;

    if (!m_rhi || !ocio) {
        result.errorMessage = QStringLiteral("OcioShaderBuilder: missing rhi or ocio");
        return result;
    }

    // Phase 7.5 A.2.7: chain construction lives in OcioChainBuilder so
    // the native Metal / Vulkan renderers can share it. The QRhi-only
    // work (binding rewrites, QShaderBaker, LUT upload) stays here.
    OcioChain chain = OcioChainBuilder::build(ocio);
    if (!chain.ok) {
        result.errorMessage = chain.errorMessage;
        return result;
    }
    OCIO::ConstGpuShaderDescRcPtr desc = chain.desc;

    QString glsl = chain.shaderText;
    glsl = rewriteBindings(std::move(glsl), desc, /*firstBinding=*/2);

    const QString fullShader = wrapInFragmentMain(glsl);

    QShaderBaker baker;
    baker.setSourceString(fullShader.toUtf8(), QShader::FragmentStage);
    baker.setGeneratedShaderVariants({ QShader::StandardShader });
    baker.setGeneratedShaders({
        { QShader::SpirvShader, QShaderVersion(100) },
        { QShader::MslShader,   QShaderVersion(20)  },
    });

    QShader shader = baker.bake();
    if (!shader.isValid()) {
        result.errorMessage = QStringLiteral("QShaderBaker: %1").arg(baker.errorMessage());
        return result;
    }
    result.fragment = shader;

    // Upload LUT textures. Same binding order as the rewrite above:
    // 1D + 2D textures first (in OCIO's order), then 3D textures.
    auto sampler = m_rhi->newSampler(
        QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    sampler->create();
    std::unique_ptr<QRhiSampler> sharedSampler(sampler);

    // 1D / 2D LUTs.
    for (int i = 0; i < desc->getNumTextures(); ++i) {
        const char *texName = nullptr, *samplerName = nullptr;
        unsigned w = 0, h = 0;
        OCIO::GpuShaderDesc::TextureType chan = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderDesc::TextureDimensions dim = OCIO::GpuShaderDesc::TEXTURE_1D;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->getTexture(i, texName, samplerName, w, h, chan, dim, interp);

        const float *values = nullptr;
        desc->getTextureValues(i, values);
        if (!values) {
            qWarning("OCIO LUT[%d] has null values; skipping", i);
            continue;
        }

        // Pixel layout: TEXTURE_RED_CHANNEL = 1 channel, RGB_CHANNEL = 3.
        // QRhi has R32F (single channel) and RGBA32F (no RGB32F variant),
        // so RGB LUTs get padded to RGBA with alpha=1.0.
        const bool isRgb = (chan == OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL);
        const QRhiTexture::Format fmt = isRgb ? QRhiTexture::RGBA32F
                                              : QRhiTexture::R32F;
        const int width = static_cast<int>(w > 0 ? w : 1);
        const int height = static_cast<int>(h > 0 ? h : 1);

        std::unique_ptr<QRhiTexture> tex(m_rhi->newTexture(
            fmt, QSize(width, height), 1));
        tex->create();

        QByteArray bytes;
        if (isRgb) {
            // OCIO returns a tightly-packed RGB float array
            // (3 floats/pixel). QRhi RGBA32F wants 4 floats/pixel.
            bytes.resize(width * height * 4 * sizeof(float));
            float *dst = reinterpret_cast<float *>(bytes.data());
            const float *src = values;
            const int n = width * height;
            for (int p = 0; p < n; ++p) {
                dst[p * 4 + 0] = src[p * 3 + 0];
                dst[p * 4 + 1] = src[p * 3 + 1];
                dst[p * 4 + 2] = src[p * 3 + 2];
                dst[p * 4 + 3] = 1.0f;
            }
        } else {
            bytes.resize(width * height * sizeof(float));
            std::memcpy(bytes.data(), values, bytes.size());
        }
        QRhiTextureSubresourceUploadDescription sub(bytes.constData(), bytes.size());
        QRhiTextureUploadEntry entry(0, 0, sub);
        QRhiTextureUploadDescription up({ entry });
        uploadBatch->uploadTexture(tex.get(), up);

        result.luts.push_back(std::move(tex));
        // Each LUT gets its own sampler clone (identical config but
        // QRhi requires a unique sampler per binding).
        std::unique_ptr<QRhiSampler> s(m_rhi->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        s->create();
        result.samplers.push_back(std::move(s));
    }

    // 3D LUTs.
    for (int i = 0; i < desc->getNum3DTextures(); ++i) {
        const char *texName = nullptr, *samplerName = nullptr;
        unsigned edgelen = 0;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->get3DTexture(i, texName, samplerName, edgelen, interp);

        const float *values = nullptr;
        desc->get3DTextureValues(i, values);
        if (!values || edgelen == 0) {
            qWarning("OCIO 3D LUT[%d] missing or empty; skipping", i);
            continue;
        }

        // QRhi 3D textures: width × height × depth, format RGBA32F
        // (there's no RGB-only float variant). OCIO emits edgelen³
        // RGB triplets contiguously — pad each pixel to RGBA.
        const int E = static_cast<int>(edgelen);
        std::unique_ptr<QRhiTexture> tex(m_rhi->newTexture(
            QRhiTexture::RGBA32F,
            E, E, E,                          // width, height, depth
            1,                                // sampleCount
            QRhiTexture::ThreeDimensional));
        tex->create();

        // QRhi 3D texture uploads need ONE QRhiTextureUploadEntry per
        // z-slice (per qrhi.cpp:4464 docs — `layer` ∈ [0, depth-1]
        // for ThreeDimensional textures). Uploading the whole cube
        // as a single layer=0 entry only populates slice 0; the rest
        // is uninitialized GPU memory, producing visually-corrupt
        // results for any view with a 3D LUT (Filmic, AgX, ACES,
        // etc.). Build per-slice byte arrays and submit E entries.
        const int sliceTexels = E * E;
        const int sliceBytes  = sliceTexels * 4 * sizeof(float);

        // QRhi's QRhiTextureSubresourceUploadDescription(const void*,
        // quint32) deep-copies the bytes into an internal QByteArray
        // (qrhi.cpp:2971), so per-slice buffers can go out of scope
        // safely after uploadTexture() returns.
        for (int z = 0; z < E; ++z) {
            QByteArray slice;
            slice.resize(sliceBytes);
            float *dst = reinterpret_cast<float *>(slice.data());
            const float *srcSlice = values + z * sliceTexels * 3;
            for (int p = 0; p < sliceTexels; ++p) {
                dst[p * 4 + 0] = srcSlice[p * 3 + 0];
                dst[p * 4 + 1] = srcSlice[p * 3 + 1];
                dst[p * 4 + 2] = srcSlice[p * 3 + 2];
                dst[p * 4 + 3] = 1.0f;
            }
            QRhiTextureSubresourceUploadDescription sub(
                slice.constData(), static_cast<quint32>(slice.size()));
            QRhiTextureUploadEntry entry(z, 0, sub);
            uploadBatch->uploadTexture(tex.get(),
                                       QRhiTextureUploadDescription(entry));
        }

        result.luts.push_back(std::move(tex));
        std::unique_ptr<QRhiSampler> s(m_rhi->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        s->create();
        result.samplers.push_back(std::move(s));
    }

    result.valid = true;
    return result;
}

} // namespace qcv
