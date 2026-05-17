// probe-ocio — Phase 2.0 smoke test for OpenColorIO integration.
//
// Loads a config from disk (or via OCIO env var) and prints:
//   - config name, version, search path
//   - roles (scene_linear, color_picking, etc.)
//   - colorspaces (Input column candidates)
//   - looks
//   - displays + each display's views (Output column candidates)
//
// Usage:  probe-ocio <path-to-config.ocio>
//   (or)  OCIO=<path>  probe-ocio
//
// We deliberately don't ship a config in this commit — Phase 2.7
// vendors the four shipped configs (Blender 5.1, Blender 4.5,
// ACES 2.0, ACES 1.3). For now point at any OCIO config you have
// (the OCIO source repo's `tests/data` has minimal configs; ACES
// configs are at https://opencolorio.org/downloads.html).

#include <OpenColorIO/OpenColorIO.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace OCIO = OCIO_NAMESPACE;

int main(int argc, char **argv)
{
    const char *configPath = nullptr;
    if (argc >= 2) {
        configPath = argv[1];
    }

    OCIO::ConstConfigRcPtr config;
    try {
        if (configPath) {
            config = OCIO::Config::CreateFromFile(configPath);
        } else {
            // Falls back to the OCIO env var.
            config = OCIO::Config::CreateFromEnv();
        }
    } catch (const OCIO::Exception &e) {
        std::fprintf(stderr, "OCIO error: %s\n", e.what());
        std::fprintf(stderr,
                     "usage: probe-ocio <path-to-config.ocio>\n"
                     "  (or set the OCIO env var to point at a config)\n");
        return 1;
    }

    if (!config) {
        std::fprintf(stderr, "OCIO::Config came back null\n");
        return 1;
    }

    std::printf("=== probe-ocio — Phase 2.0 smoke test ===\n");
    std::printf("OCIO version (lib):  %s\n", OCIO::GetVersion());
    std::printf("Config description:  %s\n", config->getDescription());
    std::printf("Config working dir:  %s\n", config->getWorkingDir());
    std::printf("Search path entries: %d\n",  config->getNumSearchPaths());
    for (int i = 0; i < config->getNumSearchPaths(); ++i) {
        std::printf("  [%d] %s\n", i, config->getSearchPath(i));
    }
    std::printf("\n");

    // ---- Roles
    std::printf("Roles (%d):\n", config->getNumRoles());
    for (int i = 0; i < config->getNumRoles(); ++i) {
        const char *role = config->getRoleName(i);
        const char *cs   = config->getRoleColorSpace(i);
        std::printf("  %-24s  →  %s\n", role, cs ? cs : "(unset)");
    }
    std::printf("\n");

    // ---- Colorspaces (the Input column candidates)
    const int csCount = config->getNumColorSpaces();
    std::printf("ColorSpaces (%d):\n", csCount);
    for (int i = 0; i < csCount; ++i) {
        const char *name = config->getColorSpaceNameByIndex(i);
        OCIO::ConstColorSpaceRcPtr cs = config->getColorSpace(name);
        const char *family = cs ? cs->getFamily() : "";
        std::printf("  %-40s  family: %s\n",
                    name ? name : "(?)",
                    family && *family ? family : "(none)");
    }
    std::printf("\n");

    // ---- Looks
    const int lookCount = config->getNumLooks();
    std::printf("Looks (%d):\n", lookCount);
    for (int i = 0; i < lookCount; ++i) {
        const char *name = config->getLookNameByIndex(i);
        std::printf("  %s\n", name ? name : "(?)");
    }
    if (lookCount == 0) std::printf("  (none)\n");
    std::printf("\n");

    // ---- Displays + Views (the Output column candidates)
    const int dispCount = config->getNumDisplays();
    std::printf("Displays (%d):\n", dispCount);
    for (int i = 0; i < dispCount; ++i) {
        const char *display = config->getDisplay(i);
        std::printf("  %s\n", display);
        const int viewCount = config->getNumViews(display);
        for (int v = 0; v < viewCount; ++v) {
            const char *view = config->getView(display, v);
            const char *vcs  = config->getDisplayViewColorSpaceName(display, view);
            const char *look = config->getDisplayViewLooks(display, view);
            std::printf("      └─ %-30s  cs: %-30s  look: %s\n",
                        view ? view : "(?)",
                        vcs && *vcs ? vcs : "(default)",
                        look && *look ? look : "(none)");
        }
    }
    std::printf("\n");

    // ---- Active displays / views (the recommended subset)
    if (const char *active = config->getActiveDisplays(); active && *active) {
        std::printf("Active displays: %s\n", active);
    }
    if (const char *activeViews = config->getActiveViews(); activeViews && *activeViews) {
        std::printf("Active views:    %s\n", activeViews);
    }

    std::printf("\nresult: PASS — OpenColorIO link is healthy.\n");
    return 0;
}
