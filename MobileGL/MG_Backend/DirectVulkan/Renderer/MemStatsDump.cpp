// MobileGL - Diagnostic memory accounting (LOCAL ONLY, not for upstream).
//
// Exposes MobileGL_DumpMemoryStats() for the launcher's footprint sampler.
// Logs VMA totals plus live buffer/texture/program/pipeline counts to stderr
// so Jetsam triage can tell GPU allocations apart from shader-cache growth.
// Logs at most one dump per call; the caller (launcher) rate-limits to ~30s.

#include "VulkanRenderer.h"
#include "VkBufferManager.h"
#include "VkTextureManager.h"
#include "ProgramFactory.h"
#include "PipelineFactory.h"
#include "BufferArena.h"
#include "../DirectVulkan.h"
#include <vk_mem_alloc.h>
#include <malloc/malloc.h>
#include <cstdio>

namespace MobileGL::MG_Backend::DirectVulkan {

void VulkanRenderer::DumpMemoryStats() const {
    if (m_allocator == nullptr) {
        std::fprintf(stderr, "[MobileGL-MemStats] no VMA allocator (renderer not initialized)\n");
        std::fflush(stderr);
        return;
    }
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(m_allocator, &stats);
    const VmaStatistics& total = stats.total.statistics;
    const SizeT liveBuffers = m_bufferManager.GetLiveResourceCount();
    const SizeT liveTextures = (m_textureManager != nullptr) ? m_textureManager->GetAliveTextureCount() : 0;
    const SizeT livePrograms = (m_programFactory != nullptr) ? m_programFactory->GetCacheEntryCount() : 0;
    const SizeT livePipelines =
        (m_pipelineFactory != nullptr ? m_pipelineFactory->GetCacheEntryCount() : 0) + m_computePipelines.size();
    const Uint64 arenaUploaded = BufferArena::GetCumulativeUploadedBytes();
    SizeT tex2D = 0, texCube = 0, texArray = 0, tex3D = 0, texOther = 0;
    if (m_textureManager != nullptr) {
        m_textureManager->CensusLiveTargets(tex2D, texCube, texArray, tex3D, texOther);
    }
    malloc_statistics_t mst{};
    malloc_zone_statistics(malloc_default_zone(), &mst);
    std::fprintf(stderr,
                 "[MobileGL-MemStats] vma: blocks=%u allocs=%u blockBytes=%lluMB allocBytes=%lluMB | "
                 "buffers=%llu textures=%llu programs=%llu pipelines=%llu | arenaUploaded=%lluMB\n",
                 total.blockCount, total.allocationCount,
                 (unsigned long long)(total.blockBytes / 1048576),
                 (unsigned long long)(total.allocationBytes / 1048576),
                 (unsigned long long)liveBuffers, (unsigned long long)liveTextures,
                 (unsigned long long)livePrograms, (unsigned long long)livePipelines,
                 (unsigned long long)(arenaUploaded / 1048576));
    std::fprintf(stderr,
                 "[MobileGL-MemStats] texTargets: 2D=%llu cube=%llu array=%llu 3D=%llu other=%llu | "
                 "malloc_in_use=%lluMB\n",
                 (unsigned long long)tex2D, (unsigned long long)texCube, (unsigned long long)texArray,
                 (unsigned long long)tex3D, (unsigned long long)texOther,
                 (unsigned long long)(mst.size_in_use / 1048576));
    std::fflush(stderr);
}

} // namespace MobileGL::MG_Backend::DirectVulkan

extern "C" __attribute__((visibility("default"))) void MobileGL_DumpMemoryStats(void) {
    auto& renderer = MobileGL::MG_Backend::DirectVulkan::pVulkanRenderer;
    if (!renderer) {
        std::fprintf(stderr, "[MobileGL-MemStats] no live VulkanRenderer\n");
        std::fflush(stderr);
        return;
    }
    renderer->DumpMemoryStats();
}
