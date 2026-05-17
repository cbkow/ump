#include "ocio_chain_builder.h"

#include "ocio_config_manager.h"

#include <QtLogging>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

OCIO::GroupTransformRcPtr OcioChainBuilder::buildGroupTransform(
    OCIOConfigManager *ocio,
    OCIO::ConstConfigRcPtr cfg,
    QString *errorOut)
{
    auto fail = [&](const QString &msg) -> OCIO::GroupTransformRcPtr {
        if (errorOut) *errorOut = msg;
        return nullptr;
    };

    if (!ocio) return fail(QStringLiteral("OcioChainBuilder: null OCIOConfigManager"));
    if (!cfg)  return fail(QStringLiteral("OcioChainBuilder: null OCIO config"));

    const QString inputCs = ocio->activeInput();
    const QString display = ocio->activeDisplay();
    const QString view    = ocio->activeView();
    const QString look    = ocio->activeLook();
    if (inputCs.isEmpty() || display.isEmpty() || view.isEmpty()) {
        return fail(QStringLiteral("OcioChainBuilder: active chain incomplete"));
    }

    const QString sceneLutPath   = ocio->activeSceneLutPath();
    const QString displayLutPath = ocio->activeDisplayLutPath();

    try {
        // Phase 2.5d / Guide 05 D7 chain shape (preserved verbatim from
        // old app's src/color/ocio_pipeline.cpp:179-262):
        //
        //   inputCs
        //     → [LookTransform inputCs → lookResultCs]   (if Look set)
        //     → [Scene LUT FileTransform]                (if Scene LUT set)
        //     → DisplayViewTransform(srcCs → display+view)
        //         where srcCs = lookResultCs if Look set, else inputCs
        //     → [Display LUT FileTransform]              (if Display LUT set)
        //
        // setLooksBypass on the DVT stays at the default (false) so
        // the view's built-in looks chain still applies. If overlap
        // with the user's look happens, that's the colorist's choice
        // — matches old-app behavior.
        OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();

        QByteArray dvtSrcCs = inputCs.toUtf8();

        if (!look.isEmpty()) {
            const char *lookResultCs =
                OCIO::LookTransform::GetLooksResultColorSpace(
                    cfg, cfg->getCurrentContext(),
                    look.toUtf8().constData());
            if (lookResultCs && *lookResultCs) {
                OCIO::LookTransformRcPtr lt = OCIO::LookTransform::Create();
                lt->setSrc(inputCs.toUtf8().constData());
                lt->setDst(lookResultCs);
                lt->setLooks(look.toUtf8().constData());
                group->appendTransform(lt);
                dvtSrcCs = QByteArray(lookResultCs);
            } else {
                qWarning("OcioChainBuilder: GetLooksResultColorSpace "
                         "returned null for '%s' — skipping Look",
                         qPrintable(look));
            }
        }

        if (!sceneLutPath.isEmpty()) {
            OCIO::FileTransformRcPtr ft = OCIO::FileTransform::Create();
            ft->setSrc(sceneLutPath.toUtf8().constData());
            ft->setInterpolation(OCIO::INTERP_BEST);
            ft->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
            group->appendTransform(ft);
        }

        OCIO::DisplayViewTransformRcPtr dvt = OCIO::DisplayViewTransform::Create();
        dvt->setSrc(dvtSrcCs.constData());
        dvt->setDisplay(display.toUtf8().constData());
        dvt->setView(view.toUtf8().constData());
        group->appendTransform(dvt);

        if (!displayLutPath.isEmpty()) {
            OCIO::FileTransformRcPtr ft = OCIO::FileTransform::Create();
            ft->setSrc(displayLutPath.toUtf8().constData());
            ft->setInterpolation(OCIO::INTERP_BEST);
            ft->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
            group->appendTransform(ft);
        }

        return group;
    } catch (const OCIO::Exception &e) {
        return fail(QStringLiteral("OCIO build: %1").arg(e.what()));
    }
}

OcioChain OcioChainBuilder::build(OCIOConfigManager *ocio, Language language)
{
    OcioChain out;

    if (!ocio) {
        out.errorMessage = QStringLiteral("OcioChainBuilder: null OCIOConfigManager");
        return out;
    }

    OCIO::GpuLanguage gpuLang = OCIO::GPU_LANGUAGE_GLSL_4_0;
    switch (language) {
        case Language::Glsl_4_0:    gpuLang = OCIO::GPU_LANGUAGE_GLSL_4_0;   break;
        case Language::Msl_2_0:     gpuLang = OCIO::GPU_LANGUAGE_MSL_2_0;    break;
        case Language::GlslVk_4_6:  gpuLang = OCIO::GPU_LANGUAGE_GLSL_VK_4_6; break;
        case Language::Hlsl_Sm_5_0: gpuLang = OCIO::GPU_LANGUAGE_HLSL_SM_5_0; break;
    }

    OCIO::ConstConfigRcPtr cfg;
    OCIO::GroupTransformRcPtr group;
    OCIO::ConstProcessorRcPtr proc;
    OCIO::ConstGPUProcessorRcPtr gpuProc;
    OCIO::GpuShaderDescRcPtr desc;
    try {
        cfg = OCIO::Config::CreateFromFile(
            ocio->configIdentifier().toUtf8().constData());

        group = buildGroupTransform(ocio, cfg, &out.errorMessage);
        if (!group) {
            return out;  // errorMessage already populated
        }

        proc    = cfg->getProcessor(group);
        gpuProc = proc->getDefaultGPUProcessor();
        desc    = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc->setLanguage(gpuLang);
        desc->setFunctionName("OCIODisplay");
        desc->setResourcePrefix("ocio_");
        gpuProc->extractGpuShaderInfo(desc);
    } catch (const OCIO::Exception &e) {
        out.errorMessage = QStringLiteral("OCIO build: %1").arg(e.what());
        return out;
    }

    out.desc       = desc;
    out.shaderText = QString::fromUtf8(desc->getShaderText());
    out.ok         = true;
    return out;
}

} // namespace qcv
