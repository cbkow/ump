#include "d3d11_texture_pool.h"
#include "d3d11_device_manager.h"

#ifdef _WIN32

#include "../utils/debug_utils.h"
#include <algorithm>

namespace qcview {

D3D11TexturePool::D3D11TexturePool() = default;

D3D11TexturePool::~D3D11TexturePool() {
    Clear();
}

void D3D11TexturePool::SetMemoryBudget(size_t budget_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    memory_budget_ = budget_bytes;
}

void D3D11TexturePool::SetMaxTextureAge(double age_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_texture_age_ = age_seconds;
}

size_t D3D11TexturePool::CalculateTextureSize(UINT width, UINT height, DXGI_FORMAT format) {
    UINT bytes_per_pixel = 0;

    switch (format) {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_A8_UNORM:
            bytes_per_pixel = 1;
            break;

        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_FLOAT:
            bytes_per_pixel = 2;
            break;

        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
            bytes_per_pixel = 4;
            break;

        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
            bytes_per_pixel = 8;
            break;

        case DXGI_FORMAT_R32G32B32_FLOAT:
            bytes_per_pixel = 12;
            break;

        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            bytes_per_pixel = 16;
            break;

        default:
            bytes_per_pixel = 4;  // Assume 4 bytes for unknown formats
            break;
    }

    return static_cast<size_t>(width) * height * bytes_per_pixel;
}

PooledTexture* D3D11TexturePool::CreateTexture(
    UINT width, UINT height, DXGI_FORMAT format, UINT bind_flags) {

    auto& device_mgr = D3D11DeviceManager::Instance();
    if (!device_mgr.IsInitialized()) {
        Debug::Log("D3D11TexturePool: Device manager not initialized");
        return nullptr;
    }

    auto texture = std::make_unique<PooledTexture>();
    texture->width = width;
    texture->height = height;
    texture->format = format;
    texture->memory_size = CalculateTextureSize(width, height, format);
    texture->last_used = std::chrono::steady_clock::now();
    texture->in_use = true;

    // Create the texture
    texture->texture = device_mgr.CreateTexture2D(
        width, height, format,
        D3D11_USAGE_DEFAULT,
        bind_flags
    );

    if (!texture->texture) {
        Debug::Log("D3D11TexturePool: Failed to create texture");
        return nullptr;
    }

    // Create SRV if requested and texture has shader resource binding
    if (create_srv_ && (bind_flags & D3D11_BIND_SHADER_RESOURCE)) {
        texture->srv = device_mgr.CreateSRV(texture->texture.Get());
    }

    // Create RTV if requested and texture has render target binding
    if (create_rtv_ && (bind_flags & D3D11_BIND_RENDER_TARGET)) {
        texture->rtv = device_mgr.CreateRTV(texture->texture.Get());
    }

    total_memory_used_ += texture->memory_size;
    create_count_++;

    PooledTexture* ptr = texture.get();
    textures_.push_back(std::move(texture));

    return ptr;
}

PooledTexture* D3D11TexturePool::AcquireTexture(
    UINT width, UINT height,
    DXGI_FORMAT format,
    UINT bind_flags) {

    std::lock_guard<std::mutex> lock(mutex_);

    acquire_count_++;

    // Look for an available texture with matching parameters
    TextureKey key{width, height, format, bind_flags};
    auto range = available_index_.equal_range(key);

    for (auto it = range.first; it != range.second; ++it) {
        PooledTexture* tex = it->second;
        if (!tex->in_use) {
            tex->in_use = true;
            tex->last_used = std::chrono::steady_clock::now();
            available_index_.erase(it);
            reuse_count_++;
            return tex;
        }
    }

    // No suitable texture found, create a new one
    return CreateTexture(width, height, format, bind_flags);
}

void D3D11TexturePool::ReleaseTexture(PooledTexture* texture) {
    if (!texture) return;

    std::lock_guard<std::mutex> lock(mutex_);

    texture->in_use = false;
    texture->last_used = std::chrono::steady_clock::now();

    // Get the bind flags from the texture
    D3D11_TEXTURE2D_DESC desc;
    texture->texture->GetDesc(&desc);

    // Add to available index for fast lookup
    TextureKey key{texture->width, texture->height, texture->format, desc.BindFlags};
    available_index_.insert({key, texture});
}

void D3D11TexturePool::ReleaseTexture(ID3D11Texture2D* texture) {
    if (!texture) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // Find the pooled texture by raw pointer
    for (auto& pooled : textures_) {
        if (pooled->texture.Get() == texture) {
            pooled->in_use = false;
            pooled->last_used = std::chrono::steady_clock::now();

            D3D11_TEXTURE2D_DESC desc;
            pooled->texture->GetDesc(&desc);
            TextureKey key{pooled->width, pooled->height, pooled->format, desc.BindFlags};
            available_index_.insert({key, pooled.get()});
            return;
        }
    }
}

void D3D11TexturePool::EvictOldTextures() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> max_age(max_texture_age_);

    // Remove old textures that aren't in use
    auto it = textures_.begin();
    while (it != textures_.end()) {
        auto& tex = *it;
        if (!tex->in_use) {
            auto age = std::chrono::duration_cast<std::chrono::duration<double>>(
                now - tex->last_used);

            if (age > max_age) {
                // Remove from available index
                D3D11_TEXTURE2D_DESC desc;
                tex->texture->GetDesc(&desc);
                TextureKey key{tex->width, tex->height, tex->format, desc.BindFlags};

                auto range = available_index_.equal_range(key);
                for (auto avail_it = range.first; avail_it != range.second; ++avail_it) {
                    if (avail_it->second == tex.get()) {
                        available_index_.erase(avail_it);
                        break;
                    }
                }

                total_memory_used_ -= tex->memory_size;
                evict_count_++;
                it = textures_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void D3D11TexturePool::EvictAllTextures() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Only evict textures not in use
    auto it = textures_.begin();
    while (it != textures_.end()) {
        if (!(*it)->in_use) {
            total_memory_used_ -= (*it)->memory_size;
            evict_count_++;
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }

    available_index_.clear();
}

void D3D11TexturePool::EnforceMemoryBudget() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (total_memory_used_ <= memory_budget_) return;

    // Sort available textures by last used time (oldest first)
    std::vector<PooledTexture*> candidates;
    for (auto& tex : textures_) {
        if (!tex->in_use) {
            candidates.push_back(tex.get());
        }
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const PooledTexture* a, const PooledTexture* b) {
            return a->last_used < b->last_used;
        });

    // Remove oldest textures until under budget
    for (auto* candidate : candidates) {
        if (total_memory_used_ <= memory_budget_) break;

        // Find and remove from textures_
        for (auto it = textures_.begin(); it != textures_.end(); ++it) {
            if (it->get() == candidate) {
                // Remove from available index
                D3D11_TEXTURE2D_DESC desc;
                candidate->texture->GetDesc(&desc);
                TextureKey key{candidate->width, candidate->height, candidate->format, desc.BindFlags};

                auto range = available_index_.equal_range(key);
                for (auto avail_it = range.first; avail_it != range.second; ++avail_it) {
                    if (avail_it->second == candidate) {
                        available_index_.erase(avail_it);
                        break;
                    }
                }

                total_memory_used_ -= candidate->memory_size;
                evict_count_++;
                textures_.erase(it);
                break;
            }
        }
    }
}

void D3D11TexturePool::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    textures_.clear();
    available_index_.clear();
    total_memory_used_ = 0;
}

D3D11TexturePool::Stats D3D11TexturePool::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.total_textures = textures_.size();
    stats.memory_used = total_memory_used_;
    stats.memory_budget = memory_budget_;
    stats.acquire_count = acquire_count_;
    stats.reuse_count = reuse_count_;
    stats.create_count = create_count_;
    stats.evict_count = evict_count_;

    for (const auto& tex : textures_) {
        if (tex->in_use) {
            stats.textures_in_use++;
        } else {
            stats.textures_available++;
        }
    }

    return stats;
}

} // namespace qcview

#endif // _WIN32
