#include "ocio_pipeline.h"
#include "ocio_config_manager.h"
#include <glad/gl.h>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include "../gpu/d3d11_device_manager.h"
#include <d3dcompiler.h>
#endif

extern std::unique_ptr<OCIOConfigManager> ocio_manager;

OCIOPipeline::OCIOPipeline()
    : shader_program(0)
    , vertex_shader(0)
    , fragment_shader(0)
    , is_valid(false)
    , is_passthrough(false)
    , needs_lut(false) {
}

OCIOPipeline::~OCIOPipeline() {
    CleanupShaders();
    if (!lut_texture_ids.empty()) {
        glDeleteTextures(lut_texture_ids.size(), lut_texture_ids.data());
        lut_texture_ids.clear();
    }
#ifdef _WIN32
    CleanupD3D11Shaders();
#endif
}

bool OCIOPipeline::BuildFromDescription(const std::string& src_colorspace,
    const std::string& display,
    const std::string& view,
    const std::string& looks,
    const std::vector<std::string>& scene_lut_files,
    const std::vector<std::string>& display_lut_files) {
    // Check if OCIO manager has a config
    if (!ocio_manager || !ocio_manager->IsConfigLoaded()) {
        Debug::Log("ERROR: No OCIO config loaded");
        return false;
    }

    try {
        // Get the actual config from the manager
        config = ocio_manager->GetConfig();

        if (!config) {
            Debug::Log("ERROR: Could not get OCIO config from manager");
            return false;
        }

        // Verify the colorspace exists in the config
        try {
            config->getColorSpace(src_colorspace.c_str());
        }
        catch (OCIO::Exception& e) {
            Debug::Log("WARNING: Colorspace '" + src_colorspace + "' not found in config");
            Debug::Log("Creating passthrough pipeline for testing");
            return CreatePassthroughPipeline();  // Fallback to simple pipeline
        }

        // Verify display exists
        bool display_found = false;
        for (int i = 0; i < config->getNumDisplays(); ++i) {
            if (std::string(config->getDisplay(i)) == display) {
                display_found = true;
                break;
            }
        }

        if (!display_found) {
            Debug::Log("WARNING: Display '" + display + "' not found in config");
            Debug::Log("Available displays:");
            for (int i = 0; i < config->getNumDisplays(); ++i) {
                Debug::Log("  - " + std::string(config->getDisplay(i)));
            }
            Debug::Log("Creating passthrough pipeline for testing");
            return CreatePassthroughPipeline();  // Fallback
        }

        // Verify view exists for this display
        bool view_found = false;
        try {
            int num_views = config->getNumViews(display.c_str());
            for (int i = 0; i < num_views; ++i) {
                const char* view_name = config->getView(display.c_str(), i);
                if (view_name && std::string(view_name) == view) {
                    view_found = true;
                    break;
                }
            }
        } catch (OCIO::Exception& e) {
            Debug::Log("ERROR: Exception checking views for display '" + display + "': " + std::string(e.what()));
        }

        if (!view_found) {
            Debug::Log("WARNING: View '" + view + "' not found for display '" + display + "'");
            Debug::Log("Available views for display '" + display + "':");
            try {
                int num_views = config->getNumViews(display.c_str());
                for (int i = 0; i < num_views; ++i) {
                    const char* view_name = config->getView(display.c_str(), i);
                    if (view_name) {
                        Debug::Log("  - " + std::string(view_name));
                    }
                }
            } catch (OCIO::Exception& e) {
                Debug::Log("ERROR: Exception listing views: " + std::string(e.what()));
            }
            Debug::Log("Creating passthrough pipeline for testing");
            return CreatePassthroughPipeline();  // Fallback
        }

        //Debug::Log("Display and view validated: " + display + " - " + view);

        // Create processor with or without looks/LUT files
        if (!looks.empty() || !scene_lut_files.empty() || !display_lut_files.empty()) {
            //Debug::Log("Applying LOOKS: " + looks);

            // Verify the looks exist in the config
            std::vector<std::string> look_names;
            std::string current_look;
            for (char c : looks) {
                if (c == ',') {
                    // Trim whitespace from current_look
                    size_t start = current_look.find_first_not_of(" \t\r\n");
                    size_t end = current_look.find_last_not_of(" \t\r\n");
                    if (start != std::string::npos && end != std::string::npos) {
                        current_look = current_look.substr(start, end - start + 1);
                    }
                    if (!current_look.empty()) {
                        look_names.push_back(current_look);
                        current_look.clear();
                    }
                } else {
                    current_look += c;
                }
            }
            // Handle the last look name
            if (!current_look.empty()) {
                size_t start = current_look.find_first_not_of(" \t\r\n");
                size_t end = current_look.find_last_not_of(" \t\r\n");
                if (start != std::string::npos && end != std::string::npos) {
                    current_look = current_look.substr(start, end - start + 1);
                }
                if (!current_look.empty()) {
                    look_names.push_back(current_look);
                }
            }

            // Verify looks exist and build validated looks string
            std::string looks_validated;
            for (size_t i = 0; i < look_names.size(); ++i) {
                try {
                    config->getLook(look_names[i].c_str());
                    //Debug::Log("  Look found in config: " + look_names[i]);
                    if (!looks_validated.empty()) looks_validated += ", ";
                    looks_validated += look_names[i];
                } catch (OCIO::Exception& e) {
                    Debug::Log("WARNING: Look '" + look_names[i] + "' not found in config: " + std::string(e.what()));
                }
            }

            if (!looks_validated.empty()) {
                try {
                    /*Debug::Log("=== CREATING LOOK TRANSFORM CHAIN ===");
                    Debug::Log("Source colorspace: " + src_colorspace);
                    Debug::Log("Display: " + display + ", View: " + view);
                    Debug::Log("Looks: " + looks_validated);*/

                    // Create a grouped transform: LookTransform -> DisplayViewTransform
                    OCIO::GroupTransformRcPtr groupTransform = OCIO::GroupTransform::Create();
                    //Debug::Log("Created GroupTransform");

                    // First: Apply look transform
                    OCIO::LookTransformRcPtr lookTransform = OCIO::LookTransform::Create();
                    lookTransform->setSrc(src_colorspace.c_str());
                    lookTransform->setLooks(looks_validated.c_str());
                    //Debug::Log("Created LookTransform with source: " + src_colorspace);

                    // Get the result colorspace after applying looks
                    const char* lookResultColorSpace = OCIO::LookTransform::GetLooksResultColorSpace(
                        config, config->getCurrentContext(), looks_validated.c_str());

                    if (!lookResultColorSpace) {
                        Debug::Log("ERROR: GetLooksResultColorSpace returned NULL");
                        throw OCIO::Exception("Look result colorspace is NULL");
                    }

                    lookTransform->setDst(lookResultColorSpace);
                    //Debug::Log("Look result colorspace: " + std::string(lookResultColorSpace));

                    // Try to create a processor for just the look transform first
                    try {
                        OCIO::ConstProcessorRcPtr lookProcessor = config->getProcessor(lookTransform);
                        //Debug::Log("Look transform processor created successfully");
                    } catch (OCIO::Exception& e) {
                        Debug::Log("ERROR creating look processor: " + std::string(e.what()));
                        throw;
                    }

                    groupTransform->appendTransform(lookTransform);
                    //Debug::Log("Appended LookTransform to GroupTransform");

                    // Insert Scene-Referred FileTransforms after looks (applied before display transform)
                    for (const auto& lut_path : scene_lut_files) {
                        try {
                            OCIO::FileTransformRcPtr fileTransform = OCIO::FileTransform::Create();
                            fileTransform->setSrc(lut_path.c_str());
                            fileTransform->setInterpolation(OCIO::INTERP_BEST);  // Let OCIO choose best method
                            fileTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                            groupTransform->appendTransform(fileTransform);
                            Debug::Log("Added Scene-Referred FileTransform: " + lut_path);
                        } catch (OCIO::Exception& e) {
                            Debug::Log("ERROR loading scene LUT file '" + lut_path + "': " + std::string(e.what()));
                        }
                    }

                    // Second: Apply display-view transform from look result colorspace
                    OCIO::DisplayViewTransformRcPtr displayTransform = OCIO::DisplayViewTransform::Create();
                    displayTransform->setSrc(lookResultColorSpace);
                    displayTransform->setDisplay(display.c_str());
                    displayTransform->setView(view.c_str());
                    displayTransform->setLooksBypass(false);
                    //Debug::Log("Created DisplayViewTransform: " + display + " - " + view + " from " + std::string(lookResultColorSpace));

                    // Try to create a processor for just the display transform
                    try {
                        OCIO::ConstProcessorRcPtr displayProcessor = config->getProcessor(displayTransform);
                        //Debug::Log("Display transform processor created successfully");
                    } catch (OCIO::Exception& e) {
                        Debug::Log("ERROR creating display processor: " + std::string(e.what()));
                        throw;
                    }

                    groupTransform->appendTransform(displayTransform);
                    //Debug::Log("Appended DisplayViewTransform to GroupTransform");

                    // Third: Apply Display-Referred FileTransforms (applied after display transform)
                    for (const auto& lut_path : display_lut_files) {
                        try {
                            OCIO::FileTransformRcPtr fileTransform = OCIO::FileTransform::Create();
                            fileTransform->setSrc(lut_path.c_str());
                            fileTransform->setInterpolation(OCIO::INTERP_BEST);  // Let OCIO choose best method
                            fileTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                            groupTransform->appendTransform(fileTransform);
                            Debug::Log("Added Display-Referred FileTransform: " + lut_path);
                        } catch (OCIO::Exception& e) {
                            Debug::Log("ERROR loading display LUT file '" + lut_path + "': " + std::string(e.what()));
                        }
                    }

                    // Create processor from grouped transform
                    processor = config->getProcessor(groupTransform);
                    //Debug::Log("SUCCESS: Applied looks with GroupTransform: " + looks_validated);

                } catch (OCIO::Exception& e) {
                    Debug::Log("OCIO Exception in look transform creation: " + std::string(e.what()));
                    Debug::Log("Falling back to display-view only");

                    // Create DisplayViewTransform without additional looks as fallback
                    OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
                    transform->setSrc(src_colorspace.c_str());
                    transform->setDisplay(display.c_str());
                    transform->setView(view.c_str());
                    processor = config->getProcessor(transform);
                }
            } else if (!scene_lut_files.empty() || !display_lut_files.empty()) {
                // No looks, but we have LUT files - create GroupTransform
                int total_luts = scene_lut_files.size() + display_lut_files.size();
                Debug::Log("No looks found, but applying " + std::to_string(total_luts) + " LUT file(s)");
                try {
                    OCIO::GroupTransformRcPtr groupTransform = OCIO::GroupTransform::Create();

                    // Add ColorSpace transform from source to working space (scene_linear)
                    OCIO::ColorSpaceTransformRcPtr csTransform = OCIO::ColorSpaceTransform::Create();
                    csTransform->setSrc(src_colorspace.c_str());
                    csTransform->setDst("scene_linear");  // Standard working space
                    groupTransform->appendTransform(csTransform);

                    // Add Scene-Referred FileTransforms (before display transform)
                    for (const auto& lut_path : scene_lut_files) {
                        try {
                            OCIO::FileTransformRcPtr fileTransform = OCIO::FileTransform::Create();
                            fileTransform->setSrc(lut_path.c_str());
                            fileTransform->setInterpolation(OCIO::INTERP_BEST);  // Let OCIO choose best method
                            fileTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                            groupTransform->appendTransform(fileTransform);
                            Debug::Log("Added Scene-Referred FileTransform: " + lut_path);
                        } catch (OCIO::Exception& e) {
                            Debug::Log("ERROR loading scene LUT file '" + lut_path + "': " + std::string(e.what()));
                        }
                    }

                    // Add DisplayViewTransform
                    OCIO::DisplayViewTransformRcPtr displayTransform = OCIO::DisplayViewTransform::Create();
                    displayTransform->setSrc("scene_linear");
                    displayTransform->setDisplay(display.c_str());
                    displayTransform->setView(view.c_str());
                    groupTransform->appendTransform(displayTransform);

                    // Add Display-Referred FileTransforms (after display transform)
                    for (const auto& lut_path : display_lut_files) {
                        try {
                            OCIO::FileTransformRcPtr fileTransform = OCIO::FileTransform::Create();
                            fileTransform->setSrc(lut_path.c_str());
                            fileTransform->setInterpolation(OCIO::INTERP_BEST);  // Let OCIO choose best method
                            fileTransform->setDirection(OCIO::TRANSFORM_DIR_FORWARD);
                            groupTransform->appendTransform(fileTransform);
                            Debug::Log("Added Display-Referred FileTransform: " + lut_path);
                        } catch (OCIO::Exception& e) {
                            Debug::Log("ERROR loading display LUT file '" + lut_path + "': " + std::string(e.what()));
                        }
                    }

                    processor = config->getProcessor(groupTransform);
                } catch (OCIO::Exception& e) {
                    Debug::Log("ERROR creating LUT-only pipeline: " + std::string(e.what()));
                    // Fallback to simple display transform
                    OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
                    transform->setSrc(src_colorspace.c_str());
                    transform->setDisplay(display.c_str());
                    transform->setView(view.c_str());
                    processor = config->getProcessor(transform);
                }
            } else {
                Debug::Log("WARNING: No valid looks or LUT files found, using display-view only");
                // Create DisplayViewTransform without additional looks or LUTs
                OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
                transform->setSrc(src_colorspace.c_str());
                transform->setDisplay(display.c_str());
                transform->setView(view.c_str());
                processor = config->getProcessor(transform);
            }
        } else {
            // Create DisplayViewTransform without additional looks
            Debug::Log("Creating simple DisplayViewTransform:");
            Debug::Log("  Src: " + src_colorspace);
            Debug::Log("  Display: " + display);
            Debug::Log("  View: " + view);

            OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
            transform->setSrc(src_colorspace.c_str());
            transform->setDisplay(display.c_str());
            transform->setView(view.c_str());

            // Create processor
            processor = config->getProcessor(transform);
            Debug::Log("Processor created successfully");
        }

        if (!processor) {
            Debug::Log("ERROR: Failed to create OCIO processor");
            return CreatePassthroughPipeline();  // Fallback
        }

        Debug::Log("OCIO processor created successfully");
        Debug::Log("  Source: " + src_colorspace);
        Debug::Log("  Display: " + display + " - " + view);
        if (!looks.empty()) {
            Debug::Log("  Looks: " + looks);
        }

        return GenerateAndCompileShader();

    }
    catch (OCIO::Exception& e) {
        Debug::Log("OCIO Exception: " + std::string(e.what()));
        Debug::Log("Falling back to passthrough pipeline");
        return CreatePassthroughPipeline();  // Fallback on any error
    }
}


bool OCIOPipeline::CreatePassthroughPipeline() {
    Debug::Log("Creating passthrough pipeline (identity color transform)");

    const char* vertex_src = R"(
        #version 330 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aTexCoord;
        out vec2 TexCoord;

        void main() {
            gl_Position = vec4(aPos, 0.0, 1.0);
            TexCoord = aTexCoord;
        }
    )";

    // Passthrough shader - just sample the texture without any color transformation
    // This provides the buffering behavior (video_texture -> color_texture) without color changes
    const char* fragment_src = R"(
        #version 330 core
        in vec2 TexCoord;
        out vec4 FragColor;
        uniform sampler2D videoTexture;

        void main() {
            // Identity passthrough - sample texture directly
            FragColor = texture(videoTexture, TexCoord);
        }
    )";

    // Compile shaders
    vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    if (!CompileShader(vertex_shader, vertex_src, GL_VERTEX_SHADER)) {
        Debug::Log("Failed to compile fallback vertex shader");
        return false;
    }

    fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    if (!CompileShader(fragment_shader, fragment_src, GL_FRAGMENT_SHADER)) {
        Debug::Log("Failed to compile fallback fragment shader");
        return false;
    }

    // Link program
    if (!LinkProgram()) {
        Debug::Log("Failed to link fallback shader program");
        return false;
    }

    is_valid = true;
    is_passthrough = true;
    Debug::Log("Fallback tint pipeline created successfully");
    return true;
}

bool OCIOPipeline::GenerateAndCompileShader() {
    if (!processor) return false;

    try {
        // Create GPU processor
        OCIO::GpuShaderDescRcPtr shaderDesc = OCIO::GpuShaderDesc::CreateShaderDesc();
        shaderDesc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_4_0);
        shaderDesc->setFunctionName("OCIODisplay");
        shaderDesc->setResourcePrefix("ocio_");

        // Extract GPU shader information
        OCIO::ConstGPUProcessorRcPtr gpuProc = processor->getDefaultGPUProcessor();
        gpuProc->extractGpuShaderInfo(shaderDesc);

        // Get the shader source
        const char* shader_src = shaderDesc->getShaderText();
        std::string shader_text(shader_src ? shader_src : "");
        Debug::Log("OCIO shader function length: " + std::to_string(shader_text.length()) + " characters");

        // Log first 500 chars of shader to see what's in it
        if (!shader_text.empty()) {
            std::string preview = shader_text.substr(0, std::min(size_t(500), shader_text.length()));
            Debug::Log("OCIO shader preview: " + preview);
        }

        // Check if we need LUTs
        int num_3d_luts = shaderDesc->getNum3DTextures();
        int num_1d_luts = shaderDesc->getNumTextures();  // 1D LUTs
        Debug::Log("Shader generation: " + std::to_string(num_3d_luts) + " 3D LUT(s), " +
                   std::to_string(num_1d_luts) + " 1D LUT(s) required");

        // Clear existing LUTs
        if (!lut_texture_ids.empty()) {
            glDeleteTextures(lut_texture_ids.size(), lut_texture_ids.data());
        }
        lut_texture_ids.clear();
        lut_texture_dimensions.clear();
        lut_sampler_names.clear();

        // Create 1D LUTs (ACES 2.0 gamut mapping tables)
        if (num_1d_luts > 0) {
            needs_lut = true;
            Debug::Log("Creating " + std::to_string(num_1d_luts) + " 1D LUT texture(s)");

            for (int lut_index = 0; lut_index < num_1d_luts; ++lut_index) {
                const char* textureName = nullptr;
                const char* samplerName = nullptr;
                unsigned width = 0;
                unsigned height = 0;
                OCIO::GpuShaderDesc::TextureType channel;
                OCIO::GpuShaderDesc::TextureDimensions dimensions;
                OCIO::Interpolation interp = OCIO::INTERP_LINEAR;

                shaderDesc->getTexture(lut_index, textureName, samplerName, width, height,
                                      channel, dimensions, interp);

                std::string current_sampler_name = samplerName ? samplerName : ("ocio_lut1d_" + std::to_string(lut_index));
                lut_sampler_names.push_back(current_sampler_name);

                // Debug: Log channel type and dimensions
                std::string channel_type = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL) ? "RED" : "RGB";
                std::string dim_type = (dimensions == OCIO::GpuShaderDesc::TEXTURE_1D) ? "1D" : "2D";
                Debug::Log("LUT " + std::to_string(lut_index) + " sampler: " + current_sampler_name +
                          ", width: " + std::to_string(width) + ", height: " + std::to_string(height) +
                          ", channel: " + channel_type + ", dimensions: " + dim_type);

                if (width > 0) {
                    unsigned int lut_texture_id;
                    glGenTextures(1, &lut_texture_id);
                    lut_texture_ids.push_back(lut_texture_id);

                    // Determine texture type based on dimensions
                    bool is_1d = (dimensions == OCIO::GpuShaderDesc::TEXTURE_1D);
                    bool is_red_channel = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL);

                    lut_texture_dimensions.push_back(is_1d ? 1 : 2);

                    if (is_1d) {
                        glBindTexture(GL_TEXTURE_1D, lut_texture_id);

                        // Allocate data based on channel type
                        unsigned int num_components = is_red_channel ? 1 : 3;
                        std::vector<float> lut_data(width * num_components);

                        const float* lut_ptr = nullptr;
                        shaderDesc->getTextureValues(lut_index, lut_ptr);
                        if (lut_ptr) {
                            std::memcpy(lut_data.data(), lut_ptr, lut_data.size() * sizeof(float));
                            Debug::Log("1D LUT " + std::to_string(lut_index) + " data loaded (" +
                                      std::to_string(lut_data.size()) + " floats)");
                        } else {
                            Debug::Log("WARNING: No 1D LUT " + std::to_string(lut_index) + " data, using identity");
                            for (unsigned i = 0; i < width; ++i) {
                                if (is_red_channel) {
                                    lut_data[i] = float(i) / float(width - 1);
                                } else {
                                    float val = float(i) / float(width - 1);
                                    lut_data[i * 3 + 0] = val;
                                    lut_data[i * 3 + 1] = val;
                                    lut_data[i * 3 + 2] = val;
                                }
                            }
                        }

                        // Create texture with correct format
                        if (is_red_channel) {
                            glTexImage1D(GL_TEXTURE_1D, 0, GL_R32F, width, 0, GL_RED, GL_FLOAT, lut_data.data());
                        } else {
                            glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB32F, width, 0, GL_RGB, GL_FLOAT, lut_data.data());
                        }

                        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    } else {
                        // Handle 2D texture case
                        glBindTexture(GL_TEXTURE_2D, lut_texture_id);

                        unsigned int num_components = is_red_channel ? 1 : 3;
                        std::vector<float> lut_data(width * height * num_components);

                        const float* lut_ptr = nullptr;
                        shaderDesc->getTextureValues(lut_index, lut_ptr);
                        if (lut_ptr) {
                            std::memcpy(lut_data.data(), lut_ptr, lut_data.size() * sizeof(float));
                            Debug::Log("2D LUT " + std::to_string(lut_index) + " data loaded (" +
                                      std::to_string(lut_data.size()) + " floats)");
                        }

                        if (is_red_channel) {
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width, height, 0, GL_RED, GL_FLOAT, lut_data.data());
                        } else {
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, width, height, 0, GL_RGB, GL_FLOAT, lut_data.data());
                        }

                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    }
                }
            }
        }

        // Create 3D LUTs (traditional color transforms)
        if (num_3d_luts > 0) {
            needs_lut = true;
            Debug::Log("Creating " + std::to_string(num_3d_luts) + " 3D LUT texture(s)");

            for (int lut_index = 0; lut_index < num_3d_luts; ++lut_index) {
                // Get LUT information
                const char* textureName = nullptr;
                const char* samplerName = nullptr;
                unsigned edgelen = 0;
                OCIO::Interpolation interp = OCIO::INTERP_LINEAR;

                shaderDesc->get3DTexture(lut_index, textureName, samplerName, edgelen, interp);

                // Store the sampler name
                std::string current_sampler_name = samplerName ? samplerName : ("ocio_lut3d_" + std::to_string(lut_index));
                lut_sampler_names.push_back(current_sampler_name);
                //Debug::Log("LUT " + std::to_string(lut_index) + " sampler name: " + current_sampler_name);

                if (edgelen > 0) {
                    // Generate 3D LUT texture
                    unsigned int lut_texture_id;
                    glGenTextures(1, &lut_texture_id);
                    lut_texture_ids.push_back(lut_texture_id);
                    lut_texture_dimensions.push_back(3);  // Mark as 3D LUT

                    glBindTexture(GL_TEXTURE_3D, lut_texture_id);

                    // Allocate and fill the 3D texture
                    std::vector<float> lut_data(edgelen * edgelen * edgelen * 3);

                    // Get LUT values
                    const float* lut_ptr = nullptr;
                    shaderDesc->get3DTextureValues(lut_index, lut_ptr);
                    if (lut_ptr) {
                        std::memcpy(lut_data.data(), lut_ptr, lut_data.size() * sizeof(float));
                        //Debug::Log("LUT " + std::to_string(lut_index) + " data received from OCIO and copied successfully");
                    }
                    else {
                        Debug::Log("WARNING: No LUT " + std::to_string(lut_index) + " data provided, using identity");
                        // Fill with identity LUT as fallback
                        for (unsigned z = 0; z < edgelen; ++z) {
                            for (unsigned y = 0; y < edgelen; ++y) {
                                for (unsigned x = 0; x < edgelen; ++x) {
                                    unsigned idx = 3 * (x + edgelen * (y + edgelen * z));
                                    lut_data[idx + 0] = float(x) / float(edgelen - 1);
                                    lut_data[idx + 1] = float(y) / float(edgelen - 1);
                                    lut_data[idx + 2] = float(z) / float(edgelen - 1);
                                }
                            }
                        }
                    }

                    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F,
                        edgelen, edgelen, edgelen,
                        0, GL_RGB, GL_FLOAT, lut_data.data());

                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

                   /* Debug::Log("Created 3D LUT texture " + std::to_string(lut_index) + ": " +
                              std::to_string(edgelen) + "x" + std::to_string(edgelen) + "x" + std::to_string(edgelen) +
                              " (ID: " + std::to_string(lut_texture_id) + ")");*/
                }
            }
        }

        // Create vertex shader (pass-through)
        const char* vertex_src = R"(
            #version 330 core
            layout(location = 0) in vec2 aPos;
            layout(location = 1) in vec2 aTexCoord;
            out vec2 TexCoord;
            
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
                TexCoord = aTexCoord;
            }
        )";

        // Create fragment shader with OCIO code
        std::stringstream frag_src;
        frag_src << "#version 330 core\n";
        frag_src << "in vec2 TexCoord;\n";
        frag_src << "out vec4 FragColor;\n";
        frag_src << "uniform sampler2D videoTexture;\n";

        // ADD DEBUG MODE
        frag_src << "uniform int debugMode;\n";  // 0=normal, 1=show input, 2=show UV

        // Add OCIO shader code (includes its own sampler declarations)
        frag_src << shader_src << "\n";

        frag_src << "void main() {\n";
        frag_src << "    vec4 col = texture(videoTexture, TexCoord);\n";

        // Debug modes
        frag_src << "    if (debugMode == 0) {\n";
        frag_src << "        FragColor = col;\n";  // Show input without processing
        frag_src << "        return;\n";
        frag_src << "    }\n";
        frag_src << "    if (debugMode == 2) {\n";
        frag_src << "        FragColor = vec4(TexCoord.x, TexCoord.y, 0.5, 1.0);\n";  // Show UVs
        frag_src << "        return;\n";
        frag_src << "    }\n";
        frag_src << "    if (debugMode == 3) {\n";
        frag_src << "        // Test if input texture is working\n";
        frag_src << "        FragColor = vec4(col.rgb * 0.5 + 0.25, 1.0);\n";  // Dimmed input + offset
        frag_src << "        return;\n";
        frag_src << "    }\n";

        // Normal OCIO processing
        frag_src << "    vec4 ocio_result = OCIODisplay(col);\n";
        frag_src << "    \n";
        frag_src << "    // Debug: Check for invalid OCIO results\n";
        frag_src << "    if (any(isnan(ocio_result.rgb)) || any(isinf(ocio_result.rgb))) {\n";
        frag_src << "        FragColor = vec4(1.0, 0.0, 1.0, 1.0);  // Magenta for invalid\n";
        frag_src << "        return;\n";
        frag_src << "    }\n";
        frag_src << "    \n";
        frag_src << "    FragColor = ocio_result;\n";
        frag_src << "}\n";

        // Debug: Output the generated shader code
        std::string frag_str = frag_src.str();
       /* Debug::Log("=== GENERATED FRAGMENT SHADER ===");
        Debug::Log(frag_str.substr(0, 500) + "...(truncated)");
        Debug::Log("=== END SHADER ===");*/

        // Compile shaders
        vertex_shader = glCreateShader(GL_VERTEX_SHADER);
        if (!CompileShader(vertex_shader, vertex_src, GL_VERTEX_SHADER)) {
            return false;
        }

        fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
        if (!CompileShader(fragment_shader, frag_str.c_str(), GL_FRAGMENT_SHADER)) {
            return false;
        }

        // Link program
        if (!LinkProgram()) {
            return false;
        }

        is_valid = true;
        is_passthrough = false;  // Real OCIO transform, not passthrough
        //Debug::Log("OCIO shader compiled and linked successfully");

        return true;

    }
    catch (OCIO::Exception& e) {
        Debug::Log("OCIO Shader Generation Exception: " + std::string(e.what()));
        return false;
    }
}

bool OCIOPipeline::CompileShader(unsigned int& shader, const char* source, unsigned int type) {
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        Debug::Log("ERROR: Shader compilation failed: " + std::string(infoLog));
        return false;
    }

    return true;
}

bool OCIOPipeline::LinkProgram() {
    shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    int success;
    glGetProgramiv(shader_program, GL_LINK_STATUS, &success);

    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(shader_program, 512, nullptr, infoLog);
        Debug::Log("ERROR: Shader linking failed: " + std::string(infoLog));
        return false;
    }

    // Set uniform locations
    glUseProgram(shader_program);
    glUniform1i(glGetUniformLocation(shader_program, "videoTexture"), 0);

    if (needs_lut && !lut_sampler_names.empty()) {
        // Bind all LUT samplers to consecutive texture units starting from unit 1
        for (size_t i = 0; i < lut_sampler_names.size(); ++i) {
            const char* sampler_name = lut_sampler_names[i].c_str();
            GLint lut_loc = glGetUniformLocation(shader_program, sampler_name);
            int texture_unit = 1 + i; // Start from texture unit 1

            if (lut_loc >= 0) {
                glUniform1i(lut_loc, texture_unit);
                //Debug::Log("LinkProgram: Set " + std::string(sampler_name) + " to unit " + std::to_string(texture_unit));
            } else {
                Debug::Log("LinkProgram: WARNING - LUT uniform '" + std::string(sampler_name) + "' not found!");
            }
        }
    }

    return true;
}

void OCIOPipeline::UpdateUniforms(int video_texture_unit, int lut_texture_unit) {
    if (!is_valid || !shader_program) return;

    // Get current program to restore later
    GLint current_program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);

    // Use our shader
    glUseProgram(shader_program);

    // Set video texture uniform
    GLint video_loc = glGetUniformLocation(shader_program, "videoTexture");
    if (video_loc >= 0) {
        glUniform1i(video_loc, video_texture_unit);
        //Debug::Log("Set videoTexture to unit " + std::to_string(video_texture_unit));
    }
    else {
        Debug::Log("WARNING: videoTexture uniform not found!");
    }

    // Set all LUT uniforms if needed
    if (needs_lut && !lut_sampler_names.empty()) {
        for (size_t i = 0; i < lut_sampler_names.size(); ++i) {
            const char* sampler_name = lut_sampler_names[i].c_str();
            int texture_unit = lut_texture_unit + i; // Consecutive texture units

            GLint lut_loc = glGetUniformLocation(shader_program, sampler_name);
            if (lut_loc >= 0) {
                glUniform1i(lut_loc, texture_unit);
                //Debug::Log("Set " + std::string(sampler_name) + " to unit " + std::to_string(texture_unit));
            }
            else {
                Debug::Log("WARNING: " + std::string(sampler_name) + " uniform not found!");
            }
        }
    }

    // Don't restore program here - let the caller manage it
}

void OCIOPipeline::CleanupShaders() {
    if (shader_program) {
        glDeleteProgram(shader_program);
        shader_program = 0;
    }
    if (vertex_shader) {
        glDeleteShader(vertex_shader);
        vertex_shader = 0;
    }
    if (fragment_shader) {
        glDeleteShader(fragment_shader);
        fragment_shader = 0;
    }
    is_valid = false;
}

bool OCIOPipeline::BuildTestPipeline() {
    Debug::Log("Building test pipeline (passthrough with tint)");
    return CreatePassthroughPipeline();  // Calls the private method
}

#ifdef _WIN32
//=============================================================================
// D3D11/HLSL Implementation
//=============================================================================

void OCIOPipeline::CleanupD3D11Shaders() {
    d3d_vertex_shader_.Reset();
    d3d_pixel_shader_.Reset();
    d3d_constant_buffer_.Reset();
    d3d_lut_1d_.clear();
    d3d_lut_3d_.clear();
    d3d_lut_srvs_.clear();
    d3d_lut_samplers_.clear();
}

bool OCIOPipeline::CreatePassthroughPipelineD3D11() {
    Debug::Log("Creating D3D11 passthrough pipeline (identity transform)");

    auto& device_mgr = ump::D3D11DeviceManager::Instance();
    if (!device_mgr.IsInitialized()) {
        Debug::Log("ERROR: D3D11DeviceManager not initialized");
        return false;
    }

    CleanupD3D11Shaders();

    // Vertex shader
    const char* vs_hlsl = R"(
        struct VSInput {
            float2 Position : POSITION;
            float2 TexCoord : TEXCOORD0;
        };

        struct VSOutput {
            float4 Position : SV_POSITION;
            float2 TexCoord : TEXCOORD0;
        };

        VSOutput VSMain(VSInput input) {
            VSOutput output;
            output.Position = float4(input.Position, 0.0, 1.0);
            output.TexCoord = input.TexCoord;
            return output;
        }
    )";

    // Passthrough pixel shader
    const char* ps_hlsl = R"(
        Texture2D videoTexture : register(t0);
        SamplerState linearSampler : register(s0);

        struct PSInput {
            float4 Position : SV_POSITION;
            float2 TexCoord : TEXCOORD0;
        };

        float4 PSMain(PSInput input) : SV_TARGET {
            return videoTexture.Sample(linearSampler, input.TexCoord);
        }
    )";

    d3d_vertex_shader_ = device_mgr.CompileVertexShader(vs_hlsl, "VSMain");
    if (!d3d_vertex_shader_) {
        Debug::Log("ERROR: Failed to compile D3D11 passthrough vertex shader");
        return false;
    }

    d3d_pixel_shader_ = device_mgr.CompilePixelShader(ps_hlsl, "PSMain");
    if (!d3d_pixel_shader_) {
        Debug::Log("ERROR: Failed to compile D3D11 passthrough pixel shader");
        return false;
    }

    Debug::Log("D3D11 passthrough pipeline created successfully");
    return true;
}

bool OCIOPipeline::GenerateAndCompileShaderD3D11() {
    if (!processor) {
        Debug::Log("ERROR: No OCIO processor available for D3D11 shader generation");
        return CreatePassthroughPipelineD3D11();
    }

    auto& device_mgr = ump::D3D11DeviceManager::Instance();
    if (!device_mgr.IsInitialized()) {
        Debug::Log("ERROR: D3D11DeviceManager not initialized for shader generation");
        return false;
    }

    CleanupD3D11Shaders();

    try {
        // Create GPU processor with HLSL output
        OCIO::GpuShaderDescRcPtr shaderDesc = OCIO::GpuShaderDesc::CreateShaderDesc();
        shaderDesc->setLanguage(OCIO::GPU_LANGUAGE_HLSL_DX11);
        shaderDesc->setFunctionName("OCIODisplay");
        shaderDesc->setResourcePrefix("ocio_");

        // Extract GPU shader information
        OCIO::ConstGPUProcessorRcPtr gpuProc = processor->getDefaultGPUProcessor();
        gpuProc->extractGpuShaderInfo(shaderDesc);

        // Get the shader source
        const char* shader_src = shaderDesc->getShaderText();
        std::string ocio_shader_text(shader_src ? shader_src : "");
        Debug::Log("OCIO HLSL shader function length: " + std::to_string(ocio_shader_text.length()) + " characters");

        // Check for LUTs
        int num_3d_luts = shaderDesc->getNum3DTextures();
        int num_1d_luts = shaderDesc->getNumTextures();
        Debug::Log("D3D11 shader: " + std::to_string(num_3d_luts) + " 3D LUT(s), " +
                   std::to_string(num_1d_luts) + " 1D LUT(s) required");

        auto* device = device_mgr.GetDevice();

        // Create 1D LUT textures
        for (int i = 0; i < num_1d_luts; ++i) {
            const char* textureName = nullptr;
            const char* samplerName = nullptr;
            unsigned width = 0;
            unsigned height = 0;
            OCIO::GpuShaderDesc::TextureType channel;
            OCIO::GpuShaderDesc::TextureDimensions dimensions;
            OCIO::Interpolation interp = OCIO::INTERP_LINEAR;

            shaderDesc->getTexture(i, textureName, samplerName, width, height, channel, dimensions, interp);

            if (width == 0) continue;

            const float* lut_ptr = nullptr;
            shaderDesc->getTextureValues(i, lut_ptr);

            bool is_red_channel = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL);
            DXGI_FORMAT format = is_red_channel ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;

            // Create 1D texture
            D3D11_TEXTURE1D_DESC tex_desc = {};
            tex_desc.Width = width;
            tex_desc.MipLevels = 1;
            tex_desc.ArraySize = 1;
            tex_desc.Format = format;
            tex_desc.Usage = D3D11_USAGE_DEFAULT;
            tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA init_data = {};
            init_data.pSysMem = lut_ptr;

            Microsoft::WRL::ComPtr<ID3D11Texture1D> tex1d;
            HRESULT hr = device->CreateTexture1D(&tex_desc, lut_ptr ? &init_data : nullptr, &tex1d);
            if (FAILED(hr)) {
                Debug::Log("ERROR: Failed to create 1D LUT texture " + std::to_string(i));
                continue;
            }
            d3d_lut_1d_.push_back(tex1d);

            // Create SRV
            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format = format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
            srv_desc.Texture1D.MipLevels = 1;

            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            hr = device->CreateShaderResourceView(tex1d.Get(), &srv_desc, &srv);
            if (SUCCEEDED(hr)) {
                d3d_lut_srvs_.push_back(srv);
            }

            // Create sampler
            auto sampler = device_mgr.CreateSamplerState(
                D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                D3D11_TEXTURE_ADDRESS_CLAMP);
            if (sampler) {
                d3d_lut_samplers_.push_back(sampler);
            }

            Debug::Log("Created D3D11 1D LUT " + std::to_string(i) + ": " + std::to_string(width) + " elements");
        }

        // Create 3D LUT textures
        for (int i = 0; i < num_3d_luts; ++i) {
            const char* textureName = nullptr;
            const char* samplerName = nullptr;
            unsigned edgelen = 0;
            OCIO::Interpolation interp = OCIO::INTERP_LINEAR;

            shaderDesc->get3DTexture(i, textureName, samplerName, edgelen, interp);

            if (edgelen == 0) continue;

            const float* lut_ptr = nullptr;
            shaderDesc->get3DTextureValues(i, lut_ptr);

            // Create 3D texture
            D3D11_TEXTURE3D_DESC tex_desc = {};
            tex_desc.Width = edgelen;
            tex_desc.Height = edgelen;
            tex_desc.Depth = edgelen;
            tex_desc.MipLevels = 1;
            tex_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;  // Use RGBA for alignment
            tex_desc.Usage = D3D11_USAGE_DEFAULT;
            tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            // Convert RGB to RGBA for D3D11
            std::vector<float> rgba_data(edgelen * edgelen * edgelen * 4);
            if (lut_ptr) {
                for (unsigned z = 0; z < edgelen; ++z) {
                    for (unsigned y = 0; y < edgelen; ++y) {
                        for (unsigned x = 0; x < edgelen; ++x) {
                            unsigned src_idx = 3 * (x + edgelen * (y + edgelen * z));
                            unsigned dst_idx = 4 * (x + edgelen * (y + edgelen * z));
                            rgba_data[dst_idx + 0] = lut_ptr[src_idx + 0];
                            rgba_data[dst_idx + 1] = lut_ptr[src_idx + 1];
                            rgba_data[dst_idx + 2] = lut_ptr[src_idx + 2];
                            rgba_data[dst_idx + 3] = 1.0f;
                        }
                    }
                }
            }

            D3D11_SUBRESOURCE_DATA init_data = {};
            init_data.pSysMem = rgba_data.data();
            init_data.SysMemPitch = edgelen * 4 * sizeof(float);
            init_data.SysMemSlicePitch = edgelen * edgelen * 4 * sizeof(float);

            Microsoft::WRL::ComPtr<ID3D11Texture3D> tex3d;
            HRESULT hr = device->CreateTexture3D(&tex_desc, &init_data, &tex3d);
            if (FAILED(hr)) {
                Debug::Log("ERROR: Failed to create 3D LUT texture " + std::to_string(i));
                continue;
            }
            d3d_lut_3d_.push_back(tex3d);

            // Create SRV
            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            srv_desc.Texture3D.MipLevels = 1;

            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            hr = device->CreateShaderResourceView(tex3d.Get(), &srv_desc, &srv);
            if (SUCCEEDED(hr)) {
                d3d_lut_srvs_.push_back(srv);
            }

            // Create sampler
            auto sampler = device_mgr.CreateSamplerState(
                D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                D3D11_TEXTURE_ADDRESS_CLAMP);
            if (sampler) {
                d3d_lut_samplers_.push_back(sampler);
            }

            Debug::Log("Created D3D11 3D LUT " + std::to_string(i) + ": " +
                       std::to_string(edgelen) + "^3");
        }

        // Build vertex shader
        const char* vs_hlsl = R"(
            struct VSInput {
                float2 Position : POSITION;
                float2 TexCoord : TEXCOORD0;
            };

            struct VSOutput {
                float4 Position : SV_POSITION;
                float2 TexCoord : TEXCOORD0;
            };

            VSOutput VSMain(VSInput input) {
                VSOutput output;
                output.Position = float4(input.Position, 0.0, 1.0);
                output.TexCoord = input.TexCoord;
                return output;
            }
        )";

        d3d_vertex_shader_ = device_mgr.CompileVertexShader(vs_hlsl, "VSMain");
        if (!d3d_vertex_shader_) {
            Debug::Log("ERROR: Failed to compile D3D11 vertex shader");
            return CreatePassthroughPipelineD3D11();
        }

        // Build pixel shader with OCIO code
        std::stringstream ps_src;
        ps_src << "Texture2D videoTexture : register(t0);\n";
        ps_src << "SamplerState linearSampler : register(s0);\n";
        ps_src << "\n";
        ps_src << "struct PSInput {\n";
        ps_src << "    float4 Position : SV_POSITION;\n";
        ps_src << "    float2 TexCoord : TEXCOORD0;\n";
        ps_src << "};\n";
        ps_src << "\n";

        // Add OCIO shader code (includes its own texture/sampler declarations)
        ps_src << ocio_shader_text << "\n";
        ps_src << "\n";

        ps_src << "float4 PSMain(PSInput input) : SV_TARGET {\n";
        ps_src << "    float4 col = videoTexture.Sample(linearSampler, input.TexCoord);\n";
        ps_src << "    float4 result = OCIODisplay(col);\n";
        ps_src << "    \n";
        ps_src << "    // Check for invalid values\n";
        ps_src << "    if (any(isnan(result.rgb)) || any(isinf(result.rgb))) {\n";
        ps_src << "        return float4(1.0, 0.0, 1.0, 1.0);  // Magenta for invalid\n";
        ps_src << "    }\n";
        ps_src << "    return result;\n";
        ps_src << "}\n";

        std::string ps_hlsl = ps_src.str();

        d3d_pixel_shader_ = device_mgr.CompilePixelShader(ps_hlsl, "PSMain");
        if (!d3d_pixel_shader_) {
            Debug::Log("ERROR: Failed to compile D3D11 OCIO pixel shader");
            Debug::Log("Shader source preview: " + ps_hlsl.substr(0, 500) + "...");
            return CreatePassthroughPipelineD3D11();
        }

        Debug::Log("D3D11 OCIO shader compiled successfully");
        return true;

    } catch (OCIO::Exception& e) {
        Debug::Log("OCIO D3D11 Shader Generation Exception: " + std::string(e.what()));
        return CreatePassthroughPipelineD3D11();
    }
}

#endif // _WIN32