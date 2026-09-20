#pragma once
#include <Includes.h>
#include "../VkIncludes.h"

#if MOBILEGL_BUILD_DISAGGREGATED
namespace MobileGL::MG_Backend::DirectVulkan {
    // The wire draw pass has one graphics subpass, no input attachments,
    // multiview or extensions. Its Vulkan pipeline compatibility is formats,
    // sample counts and the ordered attachment references, not native handles,
    // image identity, dimensions, load/store operations or image layouts.
    struct WireRenderPassCompatibilityKey {
        Vector<Uint64> attachmentFormatsAndSamples;
        Vector<Uint32> colorReferences;
        Uint32 depthReference = VK_ATTACHMENT_UNUSED;
        Bool operator==(const WireRenderPassCompatibilityKey&) const = default;
    };

    struct WireRenderPassCompatibilityHash {
        SizeT operator()(const WireRenderPassCompatibilityKey& key) const {
            SizeT hash = 0;
            const auto mix = [&](Uint64 value) {
                hash ^= static_cast<SizeT>(value) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            };
            mix(key.attachmentFormatsAndSamples.size());
            for (const auto value : key.attachmentFormatsAndSamples) mix(value);
            mix(key.colorReferences.size());
            for (const auto value : key.colorReferences) mix(value);
            mix(key.depthReference);
            return hash;
        }
    };

    class WireRenderPassCompatibilityTable {
    public:
        Uint64 Intern(const WireRenderPassCompatibilityKey& key) {
            if (const auto found = m_ids.find(key); found != m_ids.end()) return found->second;
            // No reset/eviction: an ID cannot be reused while a pipeline factory
            // or a renderer memo can still hold its previous meaning.
            MOBILEGL_ASSERT(m_nextId != 0, "wire render-pass compatibility ID exhausted");
            const Uint64 id = m_nextId++;
            m_ids.emplace(key, id);
            return id;
        }
    private:
        UnorderedMap<WireRenderPassCompatibilityKey, Uint64, WireRenderPassCompatibilityHash> m_ids;
        Uint64 m_nextId = 1; // zero selects the native-handle pipeline domain
    };
}
#endif
