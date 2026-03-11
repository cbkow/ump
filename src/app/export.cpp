// export.cpp — QueueFrameCapture, ProcessExportStateMachine, FinalizeExport, CaptureRenderedFrame, StartExport

#include "app/application.h"
#include "annotations/annotation_serializer.h"
#include "annotations/annotation_exporter.h"
#include "utils/debug_utils.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include "../external/glfw/deps/stb_image_write.h"

#ifdef QCVIEW_USE_VULKAN
#include "gpu/vulkan_device.h"
#include "gpu/vulkan_texture_pool.h"
#include "annotations/vulkan_annotation_renderer.h"
#include "color/vulkan_ocio_renderer.h"
#endif

#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

    // Queue a frame capture (called from export flow)
    void Application::QueueFrameCapture(const std::string& output_path,
                          const std::vector<qcview::Annotations::ActiveStroke>& strokes,
                          int export_width,
                          int export_height) {
        pending_capture.output_path = output_path;
        pending_capture.strokes = strokes;
        pending_capture.pending = true;
        pending_capture.completed = false;
        pending_capture.just_queued = true;  // Delay capture by one frame
        pending_capture.display_pos = ImVec2(0, 0);  // Reset
        pending_capture.display_size = ImVec2(0, 0);  // Reset

        // If export dimensions provided, use native resolution rendering
        if (export_width > 0 && export_height > 0) {
            pending_capture.use_native_resolution = true;

            // Scale export dimensions to fit within window framebuffer
            // This works around OpenGL context limitations that can clip offscreen FBOs
            int fb_width, fb_height;
            glfwGetFramebufferSize(window, &fb_width, &fb_height);

            if (export_width > fb_width || export_height > fb_height) {
                float scale_x = static_cast<float>(fb_width) / export_width;
                float scale_y = static_cast<float>(fb_height) / export_height;
                float scale = std::min(scale_x, scale_y);

                int scaled_width = static_cast<int>(export_width * scale);
                int scaled_height = static_cast<int>(export_height * scale);

                Debug::Log("Scaling export from " + std::to_string(export_width) + "x" + std::to_string(export_height) +
                          " to " + std::to_string(scaled_width) + "x" + std::to_string(scaled_height) +
                          " (window: " + std::to_string(fb_width) + "x" + std::to_string(fb_height) + ")");

                export_width = scaled_width;
                export_height = scaled_height;
            }

            pending_capture.export_width = export_width;
            pending_capture.export_height = export_height;
        } else {
            pending_capture.use_native_resolution = false;
        }
    }

    // Wait for capture to complete (yields to render loop)
    // Process export state machine (called once per frame)
    void Application::ProcessExportStateMachine() {
        if (!export_state.active) {
            return;
        }

        // If waiting for window resize, decrement counter
        if (export_state.frames_to_wait_for_resize > 0) {
            export_state.frames_to_wait_for_resize--;
            return;
        }

        // If waiting for frames after seek, decrement counter
        if (export_state.frames_to_wait_after_seek > 0) {
            export_state.frames_to_wait_after_seek--;
            return;
        }

        // If we just finished waiting for seek, queue the capture now
        if (export_state.waiting_for_seek) {
            Debug::Log("Seek complete, queueing capture at native resolution");
            QueueFrameCapture(export_state.pending_output_path, export_state.pending_strokes,
                            export_state.options.width, export_state.options.height);
            export_state.waiting_for_capture = true;
            export_state.waiting_for_seek = false;
            // Return and let next frame render with the queued capture (so display area gets set)
            return;
        }

        // If waiting for a capture to complete, check if it's done
        if (export_state.waiting_for_capture) {
            if (!pending_capture.pending) {
                // Capture completed (or failed)
                export_state.waiting_for_capture = false;

                if (pending_capture.success) {
                    export_state.captured_images.push_back(pending_capture.output_path);
                    export_state.current_note_index++;
                } else {
                    // Capture failed - abort export
                    Debug::Log("Export error: Failed to capture image for note at index " + std::to_string(export_state.current_note_index));
                    FinalizeExport(false);
                    return;
                }
            } else {
                // Still waiting for capture
                return;
            }
        }

        // Check if we've processed all notes
        if (export_state.current_note_index >= export_state.notes.size()) {
            // All captures done - finalize export
            FinalizeExport(true);
            return;
        }

        // Process next note
        const auto& note = export_state.notes[export_state.current_note_index];

        Debug::Log("Processing note " + std::to_string(export_state.current_note_index) +
                   " at timestamp " + std::to_string(note.timestamp_seconds) +
                   " (timecode: " + note.timecode + ")");

        // Parse annotation strokes for this note
        std::vector<qcview::Annotations::ActiveStroke> strokes;
        if (!note.annotation_data.empty()) {
            strokes = qcview::Annotations::AnnotationSerializer::JsonStringToStrokes(note.annotation_data);
        }

        // Generate output path (use filesystem::path to ensure correct separators)
        // Include note index to handle multiple notes at same timecode (e.g., Frame.io imports)
        std::filesystem::path output_path = std::filesystem::path(export_state.temp_dir) /
            ("note_" + std::to_string(export_state.current_note_index) + "_" +
             qcview::Annotations::AnnotationExporter::FormatTimecode(note.timestamp_seconds, export_state.options.frame_rate) + ".png");

        // Save capture data for after seek completes
        export_state.pending_output_path = output_path.string();
        export_state.pending_strokes = strokes;

        // Seek to timestamp and wait for it to complete before queueing capture
        if (video_player) {
            double current_pos = video_player->GetPosition();
            // Use small epsilon for floating point comparison (1ms tolerance)
            const double epsilon = 0.001;

            if (std::abs(current_pos - note.timestamp_seconds) > epsilon) {
                // Need to seek to different position
                Debug::Log("Seeking from " + std::to_string(current_pos) + " to " + std::to_string(note.timestamp_seconds));
                video_player->Seek(note.timestamp_seconds);
                // Wait 5 frames for seek to complete and frame to render (increased for EXR/slower codecs)
                export_state.frames_to_wait_after_seek = 5;
                export_state.waiting_for_seek = true;
            } else {
                // Already at correct position, just wait for frame to be ready
                Debug::Log("Already at position " + std::to_string(current_pos) + ", skipping seek");
                // Wait 2 frames to ensure current frame is fully rendered
                export_state.frames_to_wait_after_seek = 2;
                export_state.waiting_for_seek = true;
            }
        }
    }

    void Application::FinalizeExport(bool success) {
        // Note: No UI restoration needed - offscreen rendering doesn't change UI state

        if (!success) {
            export_state.active = false;
            Debug::Log("Export aborted");
            return;
        }

        // Create a mapping from note timestamps to captured image paths
        std::map<double, std::string> timestamp_to_image;
        for (size_t i = 0; i < export_state.notes.size() && i < export_state.captured_images.size(); i++) {
            timestamp_to_image[export_state.notes[i].timestamp_seconds] = export_state.captured_images[i];
        }

        // Set up capture callback to return pre-captured images
        annotation_exporter->SetCaptureCallback([timestamp_to_image](
            double timestamp_seconds,
            const std::string& annotation_data,
            const std::string& output_path
        ) -> bool {
            // Find the pre-captured image for this timestamp
            auto it = timestamp_to_image.find(timestamp_seconds);
            if (it != timestamp_to_image.end()) {
                std::filesystem::path source_path = it->second;
                std::filesystem::path dest_path = output_path;

                // Check if source file exists
                if (!std::filesystem::exists(source_path)) {
                    Debug::Log("Source image not found: " + source_path.string());
                    return false;
                }

                // Compare normalized paths to check if they're the same
                try {
                    if (std::filesystem::absolute(source_path) == std::filesystem::absolute(dest_path)) {
                        return true;  // Same file, no need to copy
                    }
                } catch (...) {
                    // Path comparison failed, continue with copy
                }

                // Copy the captured image to the requested output path
                try {
                    std::filesystem::copy_file(source_path, dest_path,
                        std::filesystem::copy_options::overwrite_existing);
                    return true;
                } catch (const std::exception& e) {
                    Debug::Log("Failed to copy image: " + std::string(e.what()));
                    return false;
                }
            }
            return false;
        });

        // Now call the exporter to create the final document (it will use pre-captured images)
        std::string result_path = annotation_exporter->ExportNotes(export_state.notes, export_state.options);

        if (!result_path.empty()) {
            Debug::Log("Export completed successfully: " + result_path);
        } else {
            Debug::Log("Export failed during document generation");
        }

        export_state.active = false;
    }

    // Capture the currently rendered frame (called during render loop)
    bool Application::CaptureRenderedFrame() {
        if (!pending_capture.pending) {
            return false;
        }

#ifdef QCVIEW_USE_VULKAN
        // Skip capture on first frame after queueing
        if (pending_capture.just_queued) {
            pending_capture.just_queued = false;
            return false;
        }

        // Vulkan export path: render video + annotations to offscreen image, readback to CPU
        {
            auto& dev = qcview::VulkanDeviceManager::Instance();
            auto& pool = qcview::VulkanTexturePool::Instance();
            VkDevice device = dev.GetDevice();

            if (!video_player) {
                Debug::Log("CaptureRenderedFrame (Vulkan): No video player");
                pending_capture.pending = false;
                return false;
            }

            int video_width = video_player->GetVideoWidth();
            int video_height = video_player->GetVideoHeight();
            int capture_width = pending_capture.use_native_resolution ? pending_capture.export_width : video_width;
            int capture_height = pending_capture.use_native_resolution ? pending_capture.export_height : video_height;

            if (capture_width <= 0 || capture_height <= 0) {
                Debug::Log("CaptureRenderedFrame (Vulkan): Invalid dimensions");
                pending_capture.pending = false;
                return false;
            }

            // Get the video texture from the pool
            GLuint video_tex_id = video_player->GetCurrentVideoTexture();
            const auto* src_tex = pool.GetTexture(static_cast<uint64_t>(video_tex_id));

            if (!src_tex || !src_tex->is_valid) {
                Debug::Log("CaptureRenderedFrame (Vulkan): No valid video texture in pool");
                pending_capture.pending = false;
                return false;
            }

            Debug::Log("CaptureRenderedFrame (Vulkan): Capturing " +
                       std::to_string(capture_width) + "x" + std::to_string(capture_height));

            // Create offscreen RGBA8 image at export resolution
            VkImage export_image = VK_NULL_HANDLE;
            VmaAllocation export_alloc = VK_NULL_HANDLE;
            VkImageView export_view = VK_NULL_HANDLE;

            VkImageCreateInfo img_info{};
            img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            img_info.imageType = VK_IMAGE_TYPE_2D;
            img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
            img_info.extent = {static_cast<uint32_t>(capture_width),
                               static_cast<uint32_t>(capture_height), 1};
            img_info.mipLevels = 1;
            img_info.arrayLayers = 1;
            img_info.samples = VK_SAMPLE_COUNT_1_BIT;
            img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            img_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VmaAllocationCreateInfo alloc_ci{};
            alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

            if (vmaCreateImage(dev.GetAllocator(), &img_info, &alloc_ci,
                               &export_image, &export_alloc, nullptr) != VK_SUCCESS) {
                Debug::Log("CaptureRenderedFrame (Vulkan): Failed to create export image");
                pending_capture.pending = false;
                return false;
            }

            VkImageViewCreateInfo view_ci{};
            view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_ci.image = export_image;
            view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
            view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_ci.subresourceRange.levelCount = 1;
            view_ci.subresourceRange.layerCount = 1;

            if (vkCreateImageView(device, &view_ci, nullptr, &export_view) != VK_SUCCESS) {
                vmaDestroyImage(dev.GetAllocator(), export_image, export_alloc);
                pending_capture.pending = false;
                return false;
            }

            // Blit video texture onto export image
            {
                VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

                // Transition export image to TRANSFER_DST
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = export_image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &barrier);

                // Transition source to TRANSFER_SRC
                VkImageMemoryBarrier src_barrier{};
                src_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                src_barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                src_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                src_barrier.image = src_tex->image;
                src_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                src_barrier.subresourceRange.levelCount = 1;
                src_barrier.subresourceRange.layerCount = 1;
                src_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &src_barrier);

                // Blit (handles format conversion and scaling)
                VkImageBlit blit{};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.layerCount = 1;
                blit.srcOffsets[1] = {src_tex->width, src_tex->height, 1};
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.layerCount = 1;
                blit.dstOffsets[1] = {capture_width, capture_height, 1};

                vkCmdBlitImage(cmd, src_tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               export_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);

                // Transition source back to SHADER_READ_ONLY
                src_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                src_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                src_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                src_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &src_barrier);

                // Transition export image to COLOR_ATTACHMENT_OPTIMAL for annotation rendering
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &barrier);

                dev.EndSingleTimeCommands(cmd);
            }

            // Render annotations on top
            if (!pending_capture.strokes.empty() && vulkan_annotation_renderer_ &&
                vulkan_annotation_renderer_->IsInitialized()) {
                float viewport_width = pending_capture.viewport_width_at_creation;
                float line_width_scale = (float)capture_width / viewport_width;

                Debug::Log("Rendering " + std::to_string(pending_capture.strokes.size()) +
                          " annotation strokes (Vulkan), lws=" + std::to_string(line_width_scale));

                vulkan_annotation_renderer_->RenderAnnotationsToImage(
                    pending_capture.strokes,
                    export_image, export_view,
                    capture_width, capture_height,
                    0.0f, 0.0f, (float)capture_width, (float)capture_height,
                    line_width_scale, true);
            }

            // Readback: copy image to staging buffer
            size_t pixel_count = static_cast<size_t>(capture_width) * capture_height;
            size_t buffer_size = pixel_count * 4;

            VkBuffer staging_buffer = VK_NULL_HANDLE;
            VmaAllocation staging_alloc = VK_NULL_HANDLE;

            VkBufferCreateInfo buf_ci{};
            buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buf_ci.size = buffer_size;
            buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

            VmaAllocationCreateInfo staging_ai{};
            staging_ai.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
            staging_ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo staging_info{};
            if (vmaCreateBuffer(dev.GetAllocator(), &buf_ci, &staging_ai,
                                &staging_buffer, &staging_alloc, &staging_info) != VK_SUCCESS) {
                vkDestroyImageView(device, export_view, nullptr);
                vmaDestroyImage(dev.GetAllocator(), export_image, export_alloc);
                pending_capture.pending = false;
                return false;
            }

            {
                VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

                // Transition export image to TRANSFER_SRC
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = export_image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
                barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &barrier);

                VkBufferImageCopy region{};
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = {static_cast<uint32_t>(capture_width),
                                      static_cast<uint32_t>(capture_height), 1};

                vkCmdCopyImageToBuffer(cmd, export_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       staging_buffer, 1, &region);

                dev.EndSingleTimeCommands(cmd);
            }

            // Write PNG
            bool write_ok = false;
            if (staging_info.pMappedData) {
                write_ok = stbi_write_png(
                    pending_capture.output_path.c_str(),
                    capture_width, capture_height, 4,
                    staging_info.pMappedData,
                    capture_width * 4) != 0;

                if (write_ok) {
                    Debug::Log("CaptureRenderedFrame (Vulkan): Saved " + pending_capture.output_path);
                } else {
                    Debug::Log("CaptureRenderedFrame (Vulkan): stbi_write_png failed");
                }
            }

            // Cleanup
            vmaDestroyBuffer(dev.GetAllocator(), staging_buffer, staging_alloc);
            vkDestroyImageView(device, export_view, nullptr);
            vmaDestroyImage(dev.GetAllocator(), export_image, export_alloc);

            pending_capture.success = write_ok;
            pending_capture.completed = true;
            pending_capture.pending = false;
            return write_ok;
        }
#endif

        // Skip capture on first frame after queueing - wait for video texture to update
        if (pending_capture.just_queued) {
            Debug::Log("Skipping capture on first frame, waiting for video texture to update after seek");
            pending_capture.just_queued = false;
            return false;  // Wait one frame for video player to render the seeked frame
        }

        int capture_x, capture_y, capture_width, capture_height;
        std::vector<unsigned char> screen_data;
        bool used_offscreen_rendering = false;

        // Check if we're exporting at native resolution (offscreen)
        if (pending_capture.use_native_resolution) {
            // Use the full video resolution for export
            capture_width = pending_capture.export_width;
            capture_height = pending_capture.export_height;
            capture_x = 0;
            capture_y = 0;

            Debug::Log("Using native resolution for export: " + std::to_string(capture_width) + "x" + std::to_string(capture_height));
            Debug::Log("Viewport size at creation: " + std::to_string(pending_capture.viewport_width_at_creation) + "x" + std::to_string(pending_capture.display_size.y));

            // Render to offscreen framebuffer at native resolution
            GLuint fbo = 0;
            GLuint render_texture = 0;

            // Create framebuffer
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);

            // Create texture for rendering
            glGenTextures(1, &render_texture);
            glBindTexture(GL_TEXTURE_2D, render_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, capture_width, capture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            // Attach texture to framebuffer
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, render_texture, 0);

            // Create depth-stencil renderbuffer for NanoVG anti-aliased strokes
            GLuint depth_stencil_rb = 0;
            glGenRenderbuffers(1, &depth_stencil_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, depth_stencil_rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, capture_width, capture_height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_stencil_rb);

            // Check framebuffer status
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                Debug::Log("Framebuffer creation failed with status: " + std::to_string(status));
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteRenderbuffers(1, &depth_stencil_rb);
                glDeleteTextures(1, &render_texture);
                glDeleteFramebuffers(1, &fbo);
            } else {
                // Set viewport to native resolution
                glViewport(0, 0, capture_width, capture_height);

                // CRITICAL: Disable scissor test - ImGui enables it with window-based rect
                // which would clip our offscreen rendering for tall portrait videos
                glDisable(GL_SCISSOR_TEST);

                // Clear framebuffer (including stencil for NanoVG)
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClearStencil(0);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

                // Get video texture from video player
                if (video_player) {
                    GLuint video_texture = video_player->GetCurrentVideoTexture();
                    int video_width = video_player->GetVideoWidth();
                    int video_height = video_player->GetVideoHeight();

                    if (video_texture != 0 && video_width > 0 && video_height > 0) {
                        Debug::Log("Rendering video texture " + std::to_string(video_texture) +
                                  " (" + std::to_string(video_width) + "x" + std::to_string(video_height) + ") to framebuffer");

                        // Apply OCIO color correction if available
                        GLuint display_texture = video_texture;
                        if (video_player->HasColorPipeline()) {
                            GLuint color_corrected = video_player->CreateColorCorrectedTexture(
                                video_texture, video_width, video_height,
                                capture_width, capture_height
                            );
                            if (color_corrected != 0) {
                                display_texture = color_corrected;
                                Debug::Log("Applied OCIO color correction");
                            }
                        }

                        // Render video texture to framebuffer using simple quad
                        glEnable(GL_BLEND);
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                        // Simple passthrough shader
                        const char* vertex_shader = R"(
                            #version 330 core
                            layout(location = 0) in vec2 aPos;
                            layout(location = 1) in vec2 aTexCoord;
                            out vec2 TexCoord;
                            void main() {
                                gl_Position = vec4(aPos, 0.0, 1.0);
                                TexCoord = aTexCoord;
                            }
                        )";

                        const char* fragment_shader = R"(
                            #version 330 core
                            in vec2 TexCoord;
                            out vec4 FragColor;
                            uniform sampler2D uTexture;
                            void main() {
                                vec4 texColor = texture(uTexture, TexCoord);
                                // Composite over dark gray (#191919) for images with alpha channels
                                vec3 bgColor = vec3(0.098, 0.098, 0.098);
                                FragColor = vec4(mix(bgColor, texColor.rgb, texColor.a), 1.0);
                            }
                        )";

                        // Compile shaders
                        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
                        glShaderSource(vs, 1, &vertex_shader, nullptr);
                        glCompileShader(vs);

                        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
                        glShaderSource(fs, 1, &fragment_shader, nullptr);
                        glCompileShader(fs);

                        GLuint shader_program = glCreateProgram();
                        glAttachShader(shader_program, vs);
                        glAttachShader(shader_program, fs);
                        glLinkProgram(shader_program);

                        // Fullscreen quad (flipped V coordinates because video texture is stored upside-down in OpenGL)
                        float quad_vertices[] = {
                            -1.0f, -1.0f,  0.0f, 1.0f,  // bottom-left: U=0, V=1
                             1.0f, -1.0f,  1.0f, 1.0f,  // bottom-right: U=1, V=1
                             1.0f,  1.0f,  1.0f, 0.0f,  // top-right: U=1, V=0
                            -1.0f, -1.0f,  0.0f, 1.0f,  // bottom-left: U=0, V=1
                             1.0f,  1.0f,  1.0f, 0.0f,  // top-right: U=1, V=0
                            -1.0f,  1.0f,  0.0f, 0.0f   // top-left: U=0, V=0
                        };

                        GLuint vao, vbo;
                        glGenVertexArrays(1, &vao);
                        glGenBuffers(1, &vbo);
                        glBindVertexArray(vao);
                        glBindBuffer(GL_ARRAY_BUFFER, vbo);
                        glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
                        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
                        glEnableVertexAttribArray(0);
                        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
                        glEnableVertexAttribArray(1);

                        // Render video texture
                        glUseProgram(shader_program);
                        glBindTexture(GL_TEXTURE_2D, display_texture);
                        glDrawArrays(GL_TRIANGLES, 0, 6);

                        // Cleanup video shader resources
                        glDeleteVertexArrays(1, &vao);
                        glDeleteBuffers(1, &vbo);
                        glDeleteProgram(shader_program);
                        glDeleteShader(vs);
                        glDeleteShader(fs);

                        // Clean up color corrected texture if we created one
                        if (display_texture != video_texture) {
                            glDeleteTextures(1, &display_texture);
                        }

                        // Render annotations on top using NanoVG (same renderer as display)
                        if (!pending_capture.strokes.empty()) {
                            Debug::Log("Rendering " + std::to_string(pending_capture.strokes.size()) + " annotation strokes with NanoVG");

                            // Calculate line width scale factor based on viewport vs export resolution
                            float viewport_width = pending_capture.viewport_width_at_creation;
                            float line_width_scale = (float)capture_width / viewport_width;
                            Debug::Log("Line width scale: viewport=" + std::to_string(viewport_width) +
                                      "px, export=" + std::to_string(capture_width) +
                                      "px, scale=" + std::to_string(line_width_scale) + "x");

                            // Use NanoVG for consistent rendering between display and export
                            annotation_renderer->BeginFrame((float)capture_width, (float)capture_height, 1.0f);

                            for (const auto& stroke : pending_capture.strokes) {
                                if (stroke.points.empty()) continue;

                                // Create a scaled copy of the stroke for export resolution
                                qcview::Annotations::ActiveStroke scaled_stroke = stroke;
                                scaled_stroke.stroke_width = stroke.stroke_width * line_width_scale;

                                // Render with smoothing enabled (handles is_modeled flag internally)
                                annotation_renderer->RenderActiveStroke(
                                    scaled_stroke, 0.0f, 0.0f, (float)capture_width, (float)capture_height, true
                                );
                            }

                            annotation_renderer->EndFrame();
                            Debug::Log("NanoVG annotation rendering complete");
                        }

                        // Read pixels from framebuffer
                        size_t buffer_size = static_cast<size_t>(capture_width) * static_cast<size_t>(capture_height) * 4;
                        screen_data.resize(buffer_size);
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glReadPixels(0, 0, capture_width, capture_height, GL_RGBA, GL_UNSIGNED_BYTE, screen_data.data());

                        used_offscreen_rendering = true;
                        Debug::Log("Successfully rendered to offscreen framebuffer");
                    } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                        Debug::Log("No valid video texture available for offscreen rendering");
                    }
                }

                // Re-enable scissor test for ImGui
                glEnable(GL_SCISSOR_TEST);

                // Cleanup framebuffer
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteRenderbuffers(1, &depth_stencil_rb);
                glDeleteTextures(1, &render_texture);
                glDeleteFramebuffers(1, &fbo);

                // Restore viewport
                GLint viewport[4];
                glfwGetFramebufferSize(window, &viewport[2], &viewport[3]);
                glViewport(0, 0, viewport[2], viewport[3]);
            }

        } else {
            // Get viewport dimensions (framebuffer size in physical pixels)
            GLint viewport[4];
            glGetIntegerv(GL_VIEWPORT, viewport);

            // Calculate DPI scale factor (ImGui uses logical pixels, OpenGL uses physical pixels)
            int win_w, win_h;
            glfwGetWindowSize(window, &win_w, &win_h);
            float scale_x = (win_w > 0) ? static_cast<float>(viewport[2]) / win_w : 1.0f;
            float scale_y = (win_h > 0) ? static_cast<float>(viewport[3]) / win_h : 1.0f;

            // Get the video display area and convert from ImGui logical coords to physical framebuffer coords
            capture_x = static_cast<int>(pending_capture.display_pos.x * scale_x);
            capture_width = static_cast<int>(pending_capture.display_size.x * scale_x);
            capture_height = static_cast<int>(pending_capture.display_size.y * scale_y);

            // Fallback: if display area not set yet (viewport not calculated), use full framebuffer
            if (capture_width <= 0 || capture_height <= 0) {
                capture_x = viewport[0];
                capture_y = viewport[1];
                capture_width = viewport[2];
                capture_height = viewport[3];
                Debug::Log("Display area not set, using full viewport: " + std::to_string(capture_width) + "x" + std::to_string(capture_height));
            } else {
                // Convert ImGui Y coordinate (top-down, logical) to OpenGL Y coordinate (bottom-up, physical)
                // OpenGL Y = framebuffer_height - (imgui_y * scale) - capture_height
                int imgui_y_physical = static_cast<int>(pending_capture.display_pos.y * scale_y);
                capture_y = viewport[3] - imgui_y_physical - capture_height;

                Debug::Log("Capture area: x=" + std::to_string(capture_x) +
                          " y=" + std::to_string(capture_y) +
                          " width=" + std::to_string(capture_width) + " height=" + std::to_string(capture_height) +
                          " (scale: " + std::to_string(scale_x) + "x" + std::to_string(scale_y) + ")");
            }

            // Only do screen capture if we didn't use offscreen rendering
            if (!used_offscreen_rendering) {
                // Calculate buffer size with overflow check
                size_t buffer_size = static_cast<size_t>(capture_width) * static_cast<size_t>(capture_height) * 4;
                Debug::Log("Allocating buffer: " + std::to_string(buffer_size) + " bytes for " +
                           std::to_string(capture_width) + "x" + std::to_string(capture_height));

                // Set pixel store alignment for proper reading (especially important for non-multiple-of-4 widths)
                glPixelStorei(GL_PACK_ALIGNMENT, 1);

                // Read pixels from the video display area only
                screen_data.resize(buffer_size);
                glReadPixels(capture_x, capture_y, capture_width, capture_height, GL_RGBA, GL_UNSIGNED_BYTE, screen_data.data());

                // Check for OpenGL errors
                GLenum gl_error = glGetError();
                if (gl_error != GL_NO_ERROR) {
                    Debug::Log("OpenGL error during glReadPixels: " + std::to_string(gl_error));
                }
            }
        }

        // Validate dimensions
        if (capture_width <= 0 || capture_height <= 0) {
            Debug::Log("Invalid capture dimensions: " + std::to_string(capture_width) + "x" + std::to_string(capture_height));
            pending_capture.completed = true;
            pending_capture.success = false;
            pending_capture.pending = false;
            return false;
        }

        // Validate we have data
        if (screen_data.empty()) {
            Debug::Log("No screen data captured");
            pending_capture.completed = true;
            pending_capture.success = false;
            pending_capture.pending = false;
            return false;
        }

        // Flip vertically (OpenGL glReadPixels always returns bottom-up data)
        size_t buffer_size = static_cast<size_t>(capture_width) * static_cast<size_t>(capture_height) * 4;
        std::vector<unsigned char> flipped(buffer_size);
        int row_size = capture_width * 4;
        for (int y = 0; y < capture_height; y++) {
            memcpy(&flipped[y * row_size],
                   &screen_data[(capture_height - 1 - y) * row_size],
                   row_size);
        }

        // Save the captured frame
        bool success = stbi_write_png(pending_capture.output_path.c_str(), capture_width, capture_height, 4, flipped.data(), row_size) != 0;

        // Reset pixel store alignment to default
        glPixelStorei(GL_PACK_ALIGNMENT, 4);

        if (success) {
            Debug::Log("Successfully captured frame: " + pending_capture.output_path);
        } else {
            Debug::Log("Failed to save frame: " + pending_capture.output_path);
        }

        // Mark capture as completed
        pending_capture.completed = true;
        pending_capture.success = success;
        pending_capture.pending = false;

        return success;
    }

    // Capture video frame with annotations (synchronous)
    // Start export process (initiates state machine)
    void Application::StartExport(
        qcview::Annotations::AnnotationExporter::ExportFormat format,
        const qcview::Annotations::AnnotationExporter::ExportOptions& options,
        const std::vector<qcview::AnnotationNote>& notes,
        const std::string& temp_dir
    ) {
        export_state.active = true;
        export_state.format = format;
        export_state.options = options;
        export_state.notes = notes;
        export_state.current_note_index = 0;
        export_state.temp_dir = temp_dir;
        export_state.captured_images.clear();
        export_state.waiting_for_capture = false;
        export_state.frames_to_wait_after_seek = 0;
        export_state.frames_to_wait_for_resize = 2;  // Wait 2 frames for video sync before starting

        // Note: No need to hide UI or resize window - offscreen FBO rendering is independent of display

        Debug::Log("Export state machine started with " + std::to_string(notes.size()) + " notes");
        Debug::Log("Will render offscreen at video resolution: " + std::to_string(options.width) + "x" + std::to_string(options.height));
    }
