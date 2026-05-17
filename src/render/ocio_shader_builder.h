// OcioShaderBuilder — Phase 2.3.
//
// Runtime pipeline builder for the OCIO display pass. Given an
// OCIOConfigManager and a QRhi, produces:
//   - A QShader (cross-compiled from OCIO's GLSL via QShaderBaker)
//   - A set of QRhiTextures + samplers for the 1D and 3D LUTs OCIO
//     embedded in the shader
//   - The metadata the renderer needs to populate its SRB
//
// The builder owns the textures + samplers it produces. The
// renderer holds an OcioShaderBuilder::Built struct and references
// the builder's resources from its SRB. When the active OCIO chain
// changes, the renderer rebuilds and the previous Built is discarded
// (textures freed via QRhi).
//
// Binding scheme (matches Phase 2.2's probe rewrite):
//   binding 0  — UBO with OCIO uniforms (only if numUniforms > 0;
//                Phase 2.3 ships with this slot empty, since the
//                default config and ACES SDR view emit no uniforms)
//   binding 1  — source texture (compositeRaw)
//   binding 2+ — OCIO LUT samplers, declaration order

#pragma once

#include "color/ocio_config_manager.h"

#include <QString>
#include <memory>
#include <rhi/qshader.h>
#include <vector>

class QRhi;
class QRhiSampler;
class QRhiTexture;
class QRhiResourceUpdateBatch;

namespace qcv {

class OcioShaderBuilder
{
public:
    struct Built {
        QShader fragment;
        std::vector<std::unique_ptr<QRhiTexture>> luts;
        std::vector<std::unique_ptr<QRhiSampler>> samplers;
        bool valid = false;
        QString errorMessage;
    };

    explicit OcioShaderBuilder(QRhi *rhi);
    ~OcioShaderBuilder();

    OcioShaderBuilder(const OcioShaderBuilder &) = delete;
    OcioShaderBuilder &operator=(const OcioShaderBuilder &) = delete;

    // Build the full shader + LUT set for the current active chain on
    // `ocio`. LUT data is uploaded via the caller's batch (so the
    // renderer can commit it alongside its other uploads). Returns a
    // Built whose `valid` flag is true on success; on failure, errorMessage
    // is set and the Built holds whatever it managed to create.
    Built build(OCIOConfigManager *ocio, QRhiResourceUpdateBatch *uploadBatch);

private:
    QRhi *m_rhi = nullptr;
};

} // namespace qcv
