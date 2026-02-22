#pragma once

#include "frame_source_interface.h"
#include "direct_exr_cache.h"
#include <memory>

namespace qcview {

//=============================================================================
// DirectEXRCacheSource
// Thin wrapper around DirectEXRCache implementing IFrameSource interface
// DirectEXRCache remains UNCHANGED - this adapter just exposes its interface
//=============================================================================

class DirectEXRCacheSource : public IFrameSource {
public:
    DirectEXRCacheSource();
    ~DirectEXRCacheSource() override;

    // Non-copyable, movable
    DirectEXRCacheSource(const DirectEXRCacheSource&) = delete;
    DirectEXRCacheSource& operator=(const DirectEXRCacheSource&) = delete;
    DirectEXRCacheSource(DirectEXRCacheSource&&) = default;
    DirectEXRCacheSource& operator=(DirectEXRCacheSource&&) = default;

    //=========================================================================
    // IFrameSource Implementation
    //=========================================================================

    bool Initialize(const FrameSourceConfig& config) override;
    void Shutdown() override;
    bool IsInitialized() const override;

    void UpdatePosition(double timestamp) override;
    void SetPlaying(bool playing) override;

    void SetLoopRange(int in_frame, int out_frame) override;
    void ClearLoopRange() override;
    void SetLooping(bool enabled) override;

    GLuint GetFrame(int frame, int& width, int& height, bool& exact) override;
    bool IsFrameCached(int frame) const override;
    void ProcessPendingOperations() override;

    BufferHealth GetBufferHealth() const override;
    void ResetPlaybackState() override;

    int GetTotalFrames() const override;
    double GetFPS() const override;
    bool GetDimensions(int& width, int& height) const override;
    std::string GetSourceType() const override;

    //=========================================================================
    // Direct Access (for debugging/statistics)
    //=========================================================================

    DirectEXRCache* GetCache() { return cache_.get(); }
    const DirectEXRCache* GetCache() const { return cache_.get(); }

    // Get cache statistics
    DirectEXRCache::Stats GetStats() const;

    // Get cache segments for UI visualization
    std::vector<CacheSegment> GetCacheSegments() const;

private:
    std::unique_ptr<DirectEXRCache> cache_;
    FrameSourceConfig config_;
    double fps_ = 24.0;
    std::string source_type_;
};

} // namespace qcview
