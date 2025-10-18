#include "ocio_pipeline.h"
#include "ocio_config_manager.h"
#include <glad/gl.h>
#include <sstream>
#include <vector>

extern std::unique_ptr<OCIOConfigManager> ocio_manager;

OCIOPipeline::OCIOPipeline()
    : shader_program(0)
    , vertex_shader(0)
    , fragment_shader(0)
    , is_valid(false)
    , needs_lut(false) {
}

OCIOPipeline::~OCIOPipeline() {
    CleanupShaders();
    if (!lut_texture_ids.empty()) {
        glDeleteTextures(lut_texture_ids.size(), lut_texture_ids.data());
        lut_texture_ids.clear();
    }
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
            OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
            transform->setSrc(src_colorspace.c_str());
            transform->setDisplay(display.c_str());
            transform->setView(view.c_str());

            // Create processor
            processor = config->getProcessor(transform);
        }

        if (!processor) {
            Debug::Log("ERROR: Failed to create OCIO processor");
            return CreatePassthroughPipeline();  // Fallback
        }

        /*Debug::Log("OCIO processor created successfully");
        Debug::Log("  Source: " + src_colorspace);
        Debug::Log("  Display: " + display + " - " + view);*/
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
    Debug::Log("Creating passthrough pipeline for testing");

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

    // Test shader - output red to verify rendering
    const char* fragment_src = R"(
        #version 330 core
        in vec2 TexCoord;
        out vec4 FragColor;
        uniform sampler2D videoTexture;
        
        void main() {
            // Test: Output texture coordinates as colors
            FragColor = vec4(TexCoord.x, TexCoord.y, 0.5, 1.0);
            
            // Or test: Just output red
            // FragColor = vec4(1.0, 0.0, 0.0, 1.0);
            
            // Or normal: Sample texture
            // FragColor = texture(videoTexture, TexCoord);
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

        // Check if we need 3D LUTs
        int num_luts = shaderDesc->getNum3DTextures();
        if (num_luts > 0) {
            needs_lut = true;
            //Debug::Log("Shader requires " + std::to_string(num_luts) + " 3D LUT(s)");

            // Clear existing LUTs
            if (!lut_texture_ids.empty()) {
                glDeleteTextures(lut_texture_ids.size(), lut_texture_ids.data());
            }
            lut_texture_ids.clear();
            lut_sampler_names.clear();

            // Create all required LUTs
            for (int lut_index = 0; lut_index < num_luts; ++lut_index) {
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