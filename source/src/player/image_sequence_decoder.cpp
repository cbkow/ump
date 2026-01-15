#include "image_sequence_decoder.h"
#include "image_loaders.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace ump {

//=============================================================================
// Helper: Create appropriate loader for file extension
//=============================================================================

static std::unique_ptr<IImageLoader> CreateLoaderForExtension(const std::string& extension) {
    std::string ext = extension;
    // Lowercase for comparison
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".exr") {
        return std::make_unique<EXRImageLoader>();
    } else if (ext == ".tif" || ext == ".tiff") {
        return std::make_unique<TIFFImageLoader>();
    } else if (ext == ".png") {
        return std::make_unique<PNGImageLoader>();
    } else if (ext == ".jpg" || ext == ".jpeg") {
        return std::make_unique<JPEGImageLoader>();
    }

    // Default to EXR loader (will fail gracefully if wrong format)
    return std::make_unique<EXRImageLoader>();
}

static std::string GetExtensionFromPattern(const std::string& pattern) {
    auto pos = pattern.rfind('.');
    if (pos != std::string::npos) {
        return pattern.substr(pos);
    }
    return ".exr";  // Default
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

ImageSequenceDecoder::ImageSequenceDecoder(const std::string& directory, const std::string& pattern)
    : directory_(directory)
    , pattern_(pattern)
{
}

ImageSequenceDecoder::~ImageSequenceDecoder() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool ImageSequenceDecoder::Initialize(int start_frame, int end_frame, double fps,
                                       PipelineMode mode, const std::string& exr_layer) {
    if (initialized_) {
        return true;
    }

    start_frame_ = start_frame;
    end_frame_ = end_frame;
    frame_count_ = end_frame - start_frame + 1;
    fps_ = fps;
    duration_ = (fps > 0) ? static_cast<double>(frame_count_) / fps : 0.0;
    pipeline_mode_ = mode;
    exr_layer_ = exr_layer;

    // Create appropriate loader based on file extension
    std::string extension = GetExtensionFromPattern(pattern_);
    loader_ = CreateLoaderForExtension(extension);

    if (!loader_) {
        return false;
    }

    // Set EXR layer if applicable
    if (auto* exr_loader = dynamic_cast<EXRImageLoader*>(loader_.get())) {
        exr_loader->SetLayer(exr_layer_);
    }

    // Probe first frame for dimensions
    std::string first_frame_path = GetFramePath(0);  // Frame 0 = start_frame_ file
    if (!loader_->GetDimensions(first_frame_path, width_, height_)) {
        // Try loading a frame to get dimensions
        auto pixels = loader_->LoadFrame(first_frame_path, exr_layer_, pipeline_mode_);
        if (pixels) {
            width_ = pixels->width;
            height_ = pixels->height;
        } else {
            // Can't determine dimensions - fail initialization
            return false;
        }
    }

    // Initialize playhead to start
    playhead_frame_ = 0;
    io_frame_ = 0;

    // Start I/O thread
    running_ = true;
    io_thread_ = std::thread(&ImageSequenceDecoder::IOThread, this);

    initialized_ = true;
    return true;
}

void ImageSequenceDecoder::Shutdown() {
    if (!initialized_) {
        return;
    }

    // Stop I/O thread
    running_ = false;
    io_cv_.notify_all();

    if (io_thread_.joinable()) {
        io_thread_.join();
    }

    // Clear buffer
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        ring_buffer_.clear();
        buffer_frame_set_.clear();
        buffer_ahead_count_ = 0;
        buffer_size_ = 0;
    }

    loader_.reset();
    initialized_ = false;
}

void ImageSequenceDecoder::HardReset(int target_frame) {
    // Clear buffer and restart from target frame
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        ring_buffer_.clear();
        buffer_frame_set_.clear();
        buffer_ahead_count_ = 0;
        buffer_size_ = 0;
    }

    {
        std::lock_guard<std::mutex> lock(in_flight_mutex_);
        frames_in_flight_.clear();
    }

    // Update playhead and trigger I/O
    playhead_frame_ = target_frame;
    io_frame_ = target_frame;
    seek_requested_ = false;

    io_cv_.notify_one();
}

//=============================================================================
// Frame Access
//=============================================================================

std::shared_ptr<PixelData> ImageSequenceDecoder::GetFrame(int frame_number) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return FindInBuffer(frame_number);
}

std::shared_ptr<PixelData> ImageSequenceDecoder::GetClosestFrame(int frame_number, int* actual_frame) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (ring_buffer_.empty()) {
        if (actual_frame) *actual_frame = -1;
        return nullptr;
    }

    // First try exact match
    auto exact = FindInBuffer(frame_number);
    if (exact) {
        if (actual_frame) *actual_frame = frame_number;
        return exact;
    }

    // Find closest frame in buffer
    int closest_frame = -1;
    int min_distance = INT_MAX;
    std::shared_ptr<PixelData> closest_pixels;

    for (const auto& bf : ring_buffer_) {
        int distance = std::abs(bf.frame_number - frame_number);
        if (distance < min_distance) {
            min_distance = distance;
            closest_frame = bf.frame_number;
            closest_pixels = bf.pixels;
        }
    }

    if (actual_frame) *actual_frame = closest_frame;
    return closest_pixels;
}

bool ImageSequenceDecoder::HasFrame(int frame_number) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return buffer_frame_set_.count(frame_number) > 0;
}

std::shared_ptr<PixelData> ImageSequenceDecoder::GetKeyframe(int target_frame, int* actual_keyframe) {
    // For image sequences, every frame is a keyframe
    if (actual_keyframe) *actual_keyframe = target_frame;
    return GetFrame(target_frame);
}

//=============================================================================
// Playhead Management
//=============================================================================

void ImageSequenceDecoder::UpdatePlayhead(int frame_number, SeekQuality quality, bool force_seek) {
    // Clamp to valid range
    frame_number = std::max(0, std::min(frame_number, frame_count_ - 1));

    int old_playhead = playhead_frame_.load();
    playhead_frame_ = frame_number;

    // Check if we need to reposition I/O
    int buffer_start = old_playhead - config_.readBehindFrames;
    int buffer_end = old_playhead + config_.readAheadFrames;

    bool outside_window = (frame_number < buffer_start || frame_number > buffer_end);
    bool big_jump = std::abs(frame_number - old_playhead) > config_.readAheadFrames / 2;

    if (force_seek || outside_window || big_jump) {
        seek_target_frame_ = frame_number;
        seek_requested_ = true;
    }

    // Always notify I/O thread to ensure it starts/continues loading
    // This is especially important when pre-warming decoders for upcoming clips
    io_cv_.notify_one();

    // Update buffer ahead count
    int ahead_count = 0;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        for (const auto& bf : ring_buffer_) {
            if (bf.frame_number >= frame_number) {
                ahead_count++;
            }
        }
    }
    buffer_ahead_count_ = ahead_count;
}

//=============================================================================
// Buffer Status
//=============================================================================

int ImageSequenceDecoder::GetBufferedAhead() const {
    return buffer_ahead_count_.load();
}

int ImageSequenceDecoder::GetBufferedBehind() const {
    int playhead = playhead_frame_.load();
    int behind = 0;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (const auto& bf : ring_buffer_) {
        if (bf.frame_number < playhead) {
            behind++;
        }
    }
    return behind;
}

int ImageSequenceDecoder::GetBufferSize() const {
    return buffer_size_.load();
}

void ImageSequenceDecoder::GetBufferedRange(int& start_frame, int& end_frame) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (ring_buffer_.empty()) {
        start_frame = -1;
        end_frame = -1;
        return;
    }

    start_frame = INT_MAX;
    end_frame = INT_MIN;

    for (const auto& bf : ring_buffer_) {
        start_frame = std::min(start_frame, bf.frame_number);
        end_frame = std::max(end_frame, bf.frame_number);
    }
}

void ImageSequenceDecoder::ClearBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    ring_buffer_.clear();
    buffer_frame_set_.clear();
    buffer_ahead_count_ = 0;
    buffer_size_ = 0;
}

//=============================================================================
// Configuration
//=============================================================================

void ImageSequenceDecoder::SetConfig(const StreamingDecoderConfig& config) {
    config_ = config;
}

//=============================================================================
// I/O Thread
//=============================================================================

void ImageSequenceDecoder::IOThread() {
    while (running_) {
        // Wait for work
        {
            std::unique_lock<std::mutex> lock(io_mutex_);
            io_cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return !running_ || seek_requested_ || NeedsMoreFrames();
            });
        }

        if (!running_) break;

        // Handle seek request (reposition I/O)
        if (seek_requested_) {
            seek_requested_ = false;

            // Clear frames outside new window
            EvictOutsideWindow();

            // Reset I/O position to seek target
            io_frame_ = seek_target_frame_;

            // Clear in-flight tracking
            {
                std::lock_guard<std::mutex> lock(in_flight_mutex_);
                frames_in_flight_.clear();
            }
        }

        // Load frames if needed
        while (running_ && !seek_requested_ && NeedsMoreFrames()) {
            int next_frame = GetNextFrameToLoad();
            if (next_frame < 0) break;

            // Mark as in-flight
            {
                std::lock_guard<std::mutex> lock(in_flight_mutex_);
                if (frames_in_flight_.count(next_frame)) {
                    continue;  // Already loading this frame
                }
                frames_in_flight_.insert(next_frame);
            }

            // Load the frame (this is the slow I/O operation)
            auto pixels = LoadFrame(next_frame);

            // Remove from in-flight
            {
                std::lock_guard<std::mutex> lock(in_flight_mutex_);
                frames_in_flight_.erase(next_frame);
            }

            if (pixels && running_ && !seek_requested_) {
                AddToBuffer(next_frame, pixels);
            }
        }
    }
}

std::shared_ptr<PixelData> ImageSequenceDecoder::LoadFrame(int frame_number) {
    if (!loader_) return nullptr;

    std::string path = GetFramePath(frame_number);
    return loader_->LoadFrame(path, exr_layer_, pipeline_mode_);
}

std::string ImageSequenceDecoder::GetFramePath(int frame_number) const {
    // frame_number is 0-based index, convert to file frame number
    int file_frame = frame_number + start_frame_;

    // Format the pattern with the frame number
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), pattern_.c_str(), file_frame);

    // Combine directory and filename
    std::filesystem::path dir(directory_);
    std::filesystem::path file(buffer);
    return (dir / file).string();
}

//=============================================================================
// Ring Buffer Management
//=============================================================================

void ImageSequenceDecoder::AddToBuffer(int frame_number, std::shared_ptr<PixelData> pixels) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Check if already in buffer
    if (buffer_frame_set_.count(frame_number)) {
        return;
    }

    // Add to buffer
    BufferedFrame bf;
    bf.frame_number = frame_number;
    bf.pixels = pixels;

    // Insert in sorted order (by frame number)
    auto it = std::lower_bound(ring_buffer_.begin(), ring_buffer_.end(), bf,
        [](const BufferedFrame& a, const BufferedFrame& b) {
            return a.frame_number < b.frame_number;
        });
    ring_buffer_.insert(it, bf);
    buffer_frame_set_.insert(frame_number);

    // Update counts
    buffer_size_ = static_cast<int>(ring_buffer_.size());

    int playhead = playhead_frame_.load();
    int ahead = 0;
    for (const auto& f : ring_buffer_) {
        if (f.frame_number >= playhead) ahead++;
    }
    buffer_ahead_count_ = ahead;
}

std::shared_ptr<PixelData> ImageSequenceDecoder::FindInBuffer(int frame_number) const {
    // Buffer is sorted, could use binary search, but linear is fine for typical buffer sizes
    for (const auto& bf : ring_buffer_) {
        if (bf.frame_number == frame_number) {
            return bf.pixels;
        }
        if (bf.frame_number > frame_number) {
            break;  // Past target in sorted buffer
        }
    }
    return nullptr;
}

bool ImageSequenceDecoder::NeedsMoreFrames() const {
    int playhead = playhead_frame_.load();
    int ahead = buffer_ahead_count_.load();
    int total = buffer_size_.load();

    // Need more if we don't have enough ahead
    if (ahead < config_.readAheadFrames) {
        // But also check total buffer size limit
        int max_buffer = config_.readAheadFrames + config_.readBehindFrames;
        if (total < max_buffer) {
            return true;
        }
    }

    return false;
}

int ImageSequenceDecoder::GetNextFrameToLoad() {
    int playhead = playhead_frame_.load();

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Prioritize frames ahead of playhead
    for (int offset = 0; offset <= config_.readAheadFrames; offset++) {
        int frame = playhead + offset;
        if (frame >= 0 && frame < frame_count_) {
            if (buffer_frame_set_.count(frame) == 0) {
                // Check not in-flight
                std::lock_guard<std::mutex> ifl(in_flight_mutex_);
                if (frames_in_flight_.count(frame) == 0) {
                    return frame;
                }
            }
        }
    }

    // Then fill behind playhead
    for (int offset = 1; offset <= config_.readBehindFrames; offset++) {
        int frame = playhead - offset;
        if (frame >= 0 && frame < frame_count_) {
            if (buffer_frame_set_.count(frame) == 0) {
                // Check not in-flight
                std::lock_guard<std::mutex> ifl(in_flight_mutex_);
                if (frames_in_flight_.count(frame) == 0) {
                    return frame;
                }
            }
        }
    }

    return -1;  // Nothing to load
}

void ImageSequenceDecoder::EvictOutsideWindow() {
    int playhead = playhead_frame_.load();
    int window_start = playhead - config_.readBehindFrames;
    int window_end = playhead + config_.readAheadFrames;

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Remove frames outside window
    auto it = ring_buffer_.begin();
    while (it != ring_buffer_.end()) {
        if (it->frame_number < window_start || it->frame_number > window_end) {
            buffer_frame_set_.erase(it->frame_number);
            it = ring_buffer_.erase(it);
        } else {
            ++it;
        }
    }

    // Update counts
    buffer_size_ = static_cast<int>(ring_buffer_.size());

    int ahead = 0;
    for (const auto& f : ring_buffer_) {
        if (f.frame_number >= playhead) ahead++;
    }
    buffer_ahead_count_ = ahead;
}

} // namespace ump
