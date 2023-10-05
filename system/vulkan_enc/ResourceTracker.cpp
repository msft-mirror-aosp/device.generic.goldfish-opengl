// Copyright (C) 2018 The Android Open Source Project
// Copyright (C) 2018 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ResourceTracker.h"

#include "../OpenglSystemCommon/EmulatorFeatureInfo.h"
#include "../OpenglSystemCommon/HostConnection.h"
#include "CommandBufferStagingStream.h"
#include "DescriptorSetVirtualization.h"
#include "Resources.h"
#include "aemu/base/Optional.h"
#include "aemu/base/Tracing.h"
#include "aemu/base/threads/AndroidWorkPool.h"
#include "goldfish_vk_private_defs.h"
#include "vulkan/vulkan_core.h"

/// Use installed headers or locally defined Fuchsia-specific bits
#ifdef VK_USE_PLATFORM_FUCHSIA

#include <cutils/native_handle.h>
#include <fidl/fuchsia.hardware.goldfish/cpp/wire.h>
#include <fidl/fuchsia.sysmem/cpp/wire.h>
#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>
#include <optional>
#include <zircon/errors.h>
#include <zircon/process.h>
#include <zircon/rights.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>

#include "services/service_connector.h"

#ifndef FUCHSIA_NO_TRACE
#include <lib/trace/event.h>
#endif

#define GET_STATUS_SAFE(result, member) \
    ((result).ok() ? ((result)->member) : ZX_OK)

#else

typedef uint32_t zx_handle_t;
typedef uint64_t zx_koid_t;
#define ZX_HANDLE_INVALID         ((zx_handle_t)0)
#define ZX_KOID_INVALID ((zx_koid_t)0)
void zx_handle_close(zx_handle_t) { }
void zx_event_create(int, zx_handle_t*) { }
#endif // VK_USE_PLATFORM_FUCHSIA

/// Use installed headers or locally defined Android-specific bits
#ifdef VK_USE_PLATFORM_ANDROID_KHR

/// Goldfish sync only used for AEMU -- should replace in virtio-gpu when possibe
#include "../egl/goldfish_sync.h"
#include "AndroidHardwareBuffer.h"

#else

#if defined(__linux__)
#include "../egl/goldfish_sync.h"
#endif

#include <android/hardware_buffer.h>

#endif // VK_USE_PLATFORM_ANDROID_KHR

#include "HostVisibleMemoryVirtualization.h"
#include "Resources.h"
#include "VkEncoder.h"

#include "aemu/base/AlignedBuf.h"
#include "aemu/base/synchronization/AndroidLock.h"
#include "virtgpu_gfxstream_protocol.h"

#include "goldfish_address_space.h"
#include "goldfish_vk_private_defs.h"
#ifdef VK_USE_PLATFORM_ANDROID_KHR
#include "vk_format_info.h"
#endif
#include "vk_struct_id.h"
#include "vk_util.h"

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <vndk/hardware_buffer.h>
#include <log/log.h>
#include <stdlib.h>
#include <sync/sync.h>

#if defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__)

#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifdef HOST_BUILD
#include "android/utils/tempfile.h"
#endif

static inline int
inline_memfd_create(const char *name, unsigned int flags) {
#ifdef HOST_BUILD
    TempFile* tmpFile = tempfile_create();
    return open(tempfile_path(tmpFile), O_RDWR);
    // TODO: Windows is not suppose to support VkSemaphoreGetFdInfoKHR
#else
    return syscall(SYS_memfd_create, name, flags);
#endif
}

#define memfd_create inline_memfd_create
#endif

#define RESOURCE_TRACKER_DEBUG 0

#if RESOURCE_TRACKER_DEBUG
#undef D
#define D(fmt,...) ALOGD("%s: " fmt, __func__, ##__VA_ARGS__);
#else
#ifndef D
#define D(fmt,...)
#endif
#endif

using android::base::Optional;
using android::base::guest::AutoLock;
using android::base::guest::RecursiveLock;
using android::base::guest::Lock;
using android::base::guest::WorkPool;

namespace gfxstream {
namespace vk {

#define MAKE_HANDLE_MAPPING_FOREACH(type_name, map_impl, map_to_u64_impl, map_from_u64_impl) \
    void mapHandles_##type_name(type_name* handles, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_impl; \
        } \
    } \
    void mapHandles_##type_name##_u64(const type_name* handles, uint64_t* handle_u64s, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_to_u64_impl; \
        } \
    } \
    void mapHandles_u64_##type_name(const uint64_t* handle_u64s, type_name* handles, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_from_u64_impl; \
        } \
    } \

#define DEFINE_RESOURCE_TRACKING_CLASS(class_name, impl) \
class class_name : public VulkanHandleMapping { \
public: \
    virtual ~class_name() { } \
    GOLDFISH_VK_LIST_HANDLE_TYPES(impl) \
}; \

#define CREATE_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        handles[i] = new_from_host_##type_name(handles[i]); ResourceTracker::get()->register_##type_name(handles[i]);, \
        handle_u64s[i] = (uint64_t)new_from_host_##type_name(handles[i]), \
        handles[i] = (type_name)new_from_host_u64_##type_name(handle_u64s[i]); ResourceTracker::get()->register_##type_name(handles[i]);)

#define UNWRAP_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        handles[i] = get_host_##type_name(handles[i]), \
        handle_u64s[i] = (uint64_t)get_host_u64_##type_name(handles[i]), \
        handles[i] = (type_name)get_host_##type_name((type_name)handle_u64s[i]))

#define DESTROY_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        ResourceTracker::get()->unregister_##type_name(handles[i]); delete_goldfish_##type_name(handles[i]), \
        (void)handle_u64s[i]; delete_goldfish_##type_name(handles[i]), \
        (void)handles[i]; delete_goldfish_##type_name((type_name)handle_u64s[i]))

DEFINE_RESOURCE_TRACKING_CLASS(CreateMapping, CREATE_MAPPING_IMPL_FOR_TYPE)
DEFINE_RESOURCE_TRACKING_CLASS(UnwrapMapping, UNWRAP_MAPPING_IMPL_FOR_TYPE)
DEFINE_RESOURCE_TRACKING_CLASS(DestroyMapping, DESTROY_MAPPING_IMPL_FOR_TYPE)

static uint32_t* sSeqnoPtr = nullptr;

// static
uint32_t ResourceTracker::streamFeatureBits = 0;
ResourceTracker::ThreadingCallbacks ResourceTracker::threadingCallbacks;

struct StagingInfo {
    Lock mLock;
    std::vector<CommandBufferStagingStream*> streams;
    std::vector<VkEncoder*> encoders;
    /// \brief sets alloc and free callbacks for memory allocation for CommandBufferStagingStream(s)
    /// \param allocFn is the callback to allocate memory
    /// \param freeFn is the callback to free memory
    void setAllocFree(CommandBufferStagingStream::Alloc&& allocFn,
                      CommandBufferStagingStream::Free&& freeFn) {
        mAlloc = allocFn;
        mFree = freeFn;
    }

    ~StagingInfo() {
        for (auto stream : streams) {
            delete stream;
        }

        for (auto encoder : encoders) {
            delete encoder;
        }
    }

    void pushStaging(CommandBufferStagingStream* stream, VkEncoder* encoder) {
        AutoLock<Lock> lock(mLock);
        stream->reset();
        streams.push_back(stream);
        encoders.push_back(encoder);
    }

    void popStaging(CommandBufferStagingStream** streamOut, VkEncoder** encoderOut) {
        AutoLock<Lock> lock(mLock);
        CommandBufferStagingStream* stream;
        VkEncoder* encoder;
        if (streams.empty()) {
            if (mAlloc && mFree) {
                // if custom allocators are provided, forward them to CommandBufferStagingStream
                stream = new CommandBufferStagingStream(mAlloc, mFree);
            } else {
                stream = new CommandBufferStagingStream;
            }
            encoder = new VkEncoder(stream);
        } else {
            stream = streams.back();
            encoder = encoders.back();
            streams.pop_back();
            encoders.pop_back();
        }
        *streamOut = stream;
        *encoderOut = encoder;
    }

   private:
    CommandBufferStagingStream::Alloc mAlloc = nullptr;
    CommandBufferStagingStream::Free mFree = nullptr;
};

static StagingInfo sStaging;

class ResourceTracker::Impl {
public:
    Impl() = default;
    CreateMapping createMapping;
    UnwrapMapping unwrapMapping;
    DestroyMapping destroyMapping;
    DefaultHandleMapping defaultMapping;

#define HANDLE_DEFINE_TRIVIAL_INFO_STRUCT(type) \
    struct type##_Info { \
        uint32_t unused; \
    }; \

    GOLDFISH_VK_LIST_TRIVIAL_HANDLE_TYPES(HANDLE_DEFINE_TRIVIAL_INFO_STRUCT)

    struct VkInstance_Info {
        uint32_t highestApiVersion;
        std::set<std::string> enabledExtensions;
        // Fodder for vkEnumeratePhysicalDevices.
        std::vector<VkPhysicalDevice> physicalDevices;
    };

    struct VkDevice_Info {
        VkPhysicalDevice physdev;
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceMemoryProperties memProps;
        uint32_t apiVersion;
        std::set<std::string> enabledExtensions;
        std::vector<std::pair<PFN_vkDeviceMemoryReportCallbackEXT, void *>> deviceMemoryReportCallbacks;
    };

    struct VkDeviceMemory_Info {
        bool dedicated = false;
        bool imported = false;

#ifdef VK_USE_PLATFORM_ANDROID_KHR
        AHardwareBuffer* ahw = nullptr;
#endif
        zx_handle_t vmoHandle = ZX_HANDLE_INVALID;
        VkDevice device;

        uint8_t* ptr = nullptr;

        uint64_t blobId = 0;
        uint64_t allocationSize = 0;
        uint32_t memoryTypeIndex = 0;
        uint64_t coherentMemorySize = 0;
        uint64_t coherentMemoryOffset = 0;

        GoldfishAddressSpaceBlockPtr goldfishBlock = nullptr;
        CoherentMemoryPtr coherentMemory = nullptr;
    };

    struct VkCommandBuffer_Info {
        uint32_t placeholder;
    };

    struct VkQueue_Info {
        VkDevice device;
    };

    // custom guest-side structs for images/buffers because of AHardwareBuffer :((
    struct VkImage_Info {
        VkDevice device;
        VkImageCreateInfo createInfo;
        bool external = false;
        VkExternalMemoryImageCreateInfo externalCreateInfo;
        VkDeviceMemory currentBacking = VK_NULL_HANDLE;
        VkDeviceSize currentBackingOffset = 0;
        VkDeviceSize currentBackingSize = 0;
        bool baseRequirementsKnown = false;
        VkMemoryRequirements baseRequirements;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        bool hasExternalFormat = false;
        unsigned androidFormat = 0;
        std::vector<int> pendingQsriSyncFds;
#endif
#ifdef VK_USE_PLATFORM_FUCHSIA
        bool isSysmemBackedMemory = false;
#endif
    };

    struct VkBuffer_Info {
        VkDevice device;
        VkBufferCreateInfo createInfo;
        bool external = false;
        VkExternalMemoryBufferCreateInfo externalCreateInfo;
        VkDeviceMemory currentBacking = VK_NULL_HANDLE;
        VkDeviceSize currentBackingOffset = 0;
        VkDeviceSize currentBackingSize = 0;
        bool baseRequirementsKnown = false;
        VkMemoryRequirements baseRequirements;
#ifdef VK_USE_PLATFORM_FUCHSIA
        bool isSysmemBackedMemory = false;
#endif
    };

    struct VkSemaphore_Info {
        VkDevice device;
        zx_handle_t eventHandle = ZX_HANDLE_INVALID;
        zx_koid_t eventKoid = ZX_KOID_INVALID;
        std::optional<int> syncFd = {};
    };

    struct VkDescriptorUpdateTemplate_Info {
        uint32_t templateEntryCount = 0;
        VkDescriptorUpdateTemplateEntry* templateEntries;

        uint32_t imageInfoCount = 0;
        uint32_t bufferInfoCount = 0;
        uint32_t bufferViewCount = 0;
        uint32_t inlineUniformBlockCount = 0;
        uint32_t* imageInfoIndices;
        uint32_t* bufferInfoIndices;
        uint32_t* bufferViewIndices;
        VkDescriptorImageInfo* imageInfos;
        VkDescriptorBufferInfo* bufferInfos;
        VkBufferView* bufferViews;
        std::vector<uint8_t> inlineUniformBlockBuffer;
        std::vector<uint32_t> inlineUniformBlockBytesPerBlocks;  // bytes per uniform block
    };

    struct VkFence_Info {
        VkDevice device;
        bool external = false;
        VkExportFenceCreateInfo exportFenceCreateInfo;
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        int syncFd = -1;
#endif
    };

    struct VkDescriptorPool_Info {
        uint32_t unused;
    };

    struct VkDescriptorSet_Info {
        uint32_t unused;
    };

    struct VkDescriptorSetLayout_Info {
        uint32_t unused;
    };

    struct VkCommandPool_Info {
        uint32_t unused;
    };

    struct VkSampler_Info {
        uint32_t unused;
    };

    struct VkBufferCollectionFUCHSIA_Info {
#ifdef VK_USE_PLATFORM_FUCHSIA
        android::base::Optional<
            fuchsia_sysmem::wire::BufferCollectionConstraints>
            constraints;
        android::base::Optional<VkBufferCollectionPropertiesFUCHSIA> properties;

        // the index of corresponding createInfo for each image format
        // constraints in |constraints|.
        std::vector<uint32_t> createInfoIndex;
#endif  // VK_USE_PLATFORM_FUCHSIA
    };

#define HANDLE_REGISTER_IMPL_IMPL(type) \
    std::unordered_map<type, type##_Info> info_##type; \
    void register_##type(type obj) { \
        AutoLock<RecursiveLock> lock(mLock); \
        info_##type[obj] = type##_Info(); \
    } \

#define HANDLE_UNREGISTER_IMPL_IMPL(type) \
    void unregister_##type(type obj) { \
        AutoLock<RecursiveLock> lock(mLock); \
        info_##type.erase(obj); \
    } \

    GOLDFISH_VK_LIST_HANDLE_TYPES(HANDLE_REGISTER_IMPL_IMPL)
    GOLDFISH_VK_LIST_TRIVIAL_HANDLE_TYPES(HANDLE_UNREGISTER_IMPL_IMPL)

    void unregister_VkInstance(VkInstance instance) {
        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkInstance.find(instance);
        if (it == info_VkInstance.end()) return;
        auto info = it->second;
        info_VkInstance.erase(instance);
        lock.unlock();
    }

    void unregister_VkDevice(VkDevice device) {
        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return;
        auto info = it->second;
        info_VkDevice.erase(device);
        lock.unlock();
    }

    void unregister_VkCommandPool(VkCommandPool pool) {
        if (!pool) return;

        clearCommandPool(pool);

        AutoLock<RecursiveLock> lock(mLock);
        info_VkCommandPool.erase(pool);
    }

    void unregister_VkSampler(VkSampler sampler) {
        if (!sampler) return;

        AutoLock<RecursiveLock> lock(mLock);
        info_VkSampler.erase(sampler);
    }

    void unregister_VkCommandBuffer(VkCommandBuffer commandBuffer) {
        resetCommandBufferStagingInfo(commandBuffer, true /* also reset primaries */, true /* also clear pending descriptor sets */);

        struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
        if (!cb) return;
        if (cb->lastUsedEncoder) { cb->lastUsedEncoder->decRef(); }
        eraseObjects(&cb->subObjects);
        forAllObjects(cb->poolObjects, [cb](void* commandPool) {
            struct goldfish_VkCommandPool* p = as_goldfish_VkCommandPool((VkCommandPool)commandPool);
            eraseObject(&p->subObjects, (void*)cb);
        });
        eraseObjects(&cb->poolObjects);

        if (cb->userPtr) {
            CommandBufferPendingDescriptorSets* pendingSets = (CommandBufferPendingDescriptorSets*)cb->userPtr;
            delete pendingSets;
        }

        AutoLock<RecursiveLock> lock(mLock);
        info_VkCommandBuffer.erase(commandBuffer);
    }

    void unregister_VkQueue(VkQueue queue) {
        struct goldfish_VkQueue* q = as_goldfish_VkQueue(queue);
        if (!q) return;
        if (q->lastUsedEncoder) { q->lastUsedEncoder->decRef(); }

        AutoLock<RecursiveLock> lock(mLock);
        info_VkQueue.erase(queue);
    }

    void unregister_VkDeviceMemory(VkDeviceMemory mem) {
        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkDeviceMemory.find(mem);
        if (it == info_VkDeviceMemory.end()) return;

        auto& memInfo = it->second;

#ifdef VK_USE_PLATFORM_ANDROID_KHR
        if (memInfo.ahw) {
            AHardwareBuffer_release(memInfo.ahw);
        }
#endif

        if (memInfo.vmoHandle != ZX_HANDLE_INVALID) {
            zx_handle_close(memInfo.vmoHandle);
        }

        info_VkDeviceMemory.erase(mem);
    }

    void unregister_VkImage(VkImage img) {
        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkImage.find(img);
        if (it == info_VkImage.end()) return;

        auto& imageInfo = it->second;

        info_VkImage.erase(img);
    }

    void unregister_VkBuffer(VkBuffer buf) {
        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkBuffer.find(buf);
        if (it == info_VkBuffer.end()) return;

        info_VkBuffer.erase(buf);
    }

    void unregister_VkSemaphore(VkSemaphore sem) {
        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkSemaphore.find(sem);
        if (it == info_VkSemaphore.end()) return;

        auto& semInfo = it->second;

        if (semInfo.eventHandle != ZX_HANDLE_INVALID) {
            zx_handle_close(semInfo.eventHandle);
        }

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (semInfo.syncFd.value_or(-1) >= 0) {
            close(semInfo.syncFd.value());
        }
#endif

        info_VkSemaphore.erase(sem);
    }

    void unregister_VkDescriptorUpdateTemplate(VkDescriptorUpdateTemplate templ) {

        AutoLock<RecursiveLock> lock(mLock);
        auto it = info_VkDescriptorUpdateTemplate.find(templ);
        if (it == info_VkDescriptorUpdateTemplate.end())
            return;

        auto& info = it->second;
        if (info.templateEntryCount) delete [] info.templateEntries;
        if (info.imageInfoCount) {
            delete [] info.imageInfoIndices;
            delete [] info.imageInfos;
        }
        if (info.bufferInfoCount) {
            delete [] info.bufferInfoIndices;
            delete [] info.bufferInfos;
        }
        if (info.bufferViewCount) {
            delete [] info.bufferViewIndices;
            delete [] info.bufferViews;
        }
        info_VkDescriptorUpdateTemplate.erase(it);
    }

    void unregister_VkFence(VkFence fence) {
        AutoLock<RecursiveLock> lock(mLock);
        auto it = info_VkFence.find(fence);
        if (it == info_VkFence.end()) return;

        auto& fenceInfo = it->second;
        (void)fenceInfo;

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (fenceInfo.syncFd >= 0) {
            close(fenceInfo.syncFd);
        }
#endif

        info_VkFence.erase(fence);
    }

#ifdef VK_USE_PLATFORM_FUCHSIA
    void unregister_VkBufferCollectionFUCHSIA(
        VkBufferCollectionFUCHSIA collection) {
        AutoLock<RecursiveLock> lock(mLock);
        info_VkBufferCollectionFUCHSIA.erase(collection);
    }
#endif

    void unregister_VkDescriptorSet_locked(VkDescriptorSet set) {
        struct goldfish_VkDescriptorSet* ds = as_goldfish_VkDescriptorSet(set);
        delete ds->reified;
        info_VkDescriptorSet.erase(set);
    }

    void unregister_VkDescriptorSet(VkDescriptorSet set) {
        if (!set) return;

        AutoLock<RecursiveLock> lock(mLock);
        unregister_VkDescriptorSet_locked(set);
    }

    void unregister_VkDescriptorSetLayout(VkDescriptorSetLayout setLayout) {
        if (!setLayout) return;

        AutoLock<RecursiveLock> lock(mLock);
        delete as_goldfish_VkDescriptorSetLayout(setLayout)->layoutInfo;
        info_VkDescriptorSetLayout.erase(setLayout);
    }

    VkResult allocAndInitializeDescriptorSets(
        void* context,
        VkDevice device,
        const VkDescriptorSetAllocateInfo* ci,
        VkDescriptorSet* sets) {

        if (mFeatureInfo->hasVulkanBatchedDescriptorSetUpdate) {
            // Using the pool ID's we collected earlier from the host
            VkResult poolAllocResult = validateAndApplyVirtualDescriptorSetAllocation(ci, sets);

            if (poolAllocResult != VK_SUCCESS) return poolAllocResult;

            for (uint32_t i = 0; i < ci->descriptorSetCount; ++i) {
                register_VkDescriptorSet(sets[i]);
                VkDescriptorSetLayout setLayout = as_goldfish_VkDescriptorSet(sets[i])->reified->setLayout;

                // Need to add ref to the set layout in the virtual case
                // because the set itself might not be realized on host at the
                // same time
                struct goldfish_VkDescriptorSetLayout* dsl = as_goldfish_VkDescriptorSetLayout(setLayout);
                ++dsl->layoutInfo->refcount;
            }
        } else {
            // Pass through and use host allocation
            VkEncoder* enc = (VkEncoder*)context;
            VkResult allocRes = enc->vkAllocateDescriptorSets(device, ci, sets, true /* do lock */);

            if (allocRes != VK_SUCCESS) return allocRes;

            for (uint32_t i = 0; i < ci->descriptorSetCount; ++i) {
                applyDescriptorSetAllocation(ci->descriptorPool, ci->pSetLayouts[i]);
                fillDescriptorSetInfoForPool(ci->descriptorPool, ci->pSetLayouts[i], sets[i]);
            }
        }

        return VK_SUCCESS;
    }

    VkDescriptorImageInfo createImmutableSamplersFilteredImageInfo(
        VkDescriptorType descType,
        VkDescriptorSet descSet,
        uint32_t binding,
        const VkDescriptorImageInfo* pImageInfo) {

        VkDescriptorImageInfo res = *pImageInfo;

        if (descType != VK_DESCRIPTOR_TYPE_SAMPLER &&
            descType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) return res;

        bool immutableSampler = as_goldfish_VkDescriptorSet(descSet)->reified->bindingIsImmutableSampler[binding];

        if (!immutableSampler) return res;

        res.sampler = 0;

        return res;
    }

    bool descriptorBindingIsImmutableSampler(
        VkDescriptorSet dstSet,
        uint32_t dstBinding) {

        return as_goldfish_VkDescriptorSet(dstSet)->reified->bindingIsImmutableSampler[dstBinding];
    }

    VkDescriptorImageInfo
    filterNonexistentSampler(
        const VkDescriptorImageInfo& inputInfo) {

        VkSampler sampler =
            inputInfo.sampler;

        VkDescriptorImageInfo res = inputInfo;

        if (sampler) {
            auto it = info_VkSampler.find(sampler);
            bool samplerExists = it != info_VkSampler.end();
            if (!samplerExists) res.sampler = 0;
        }

        return res;
    }


    void freeDescriptorSetsIfHostAllocated(VkEncoder* enc, VkDevice device, uint32_t descriptorSetCount, const VkDescriptorSet* sets) {
        for (uint32_t i = 0; i < descriptorSetCount; ++i) {
            struct goldfish_VkDescriptorSet* ds = as_goldfish_VkDescriptorSet(sets[i]);
            if (ds->reified->allocationPending) {
                unregister_VkDescriptorSet(sets[i]);
                delete_goldfish_VkDescriptorSet(sets[i]);
            } else {
                enc->vkFreeDescriptorSets(device, ds->reified->pool, 1, &sets[i], false /* no lock */);
            }
        }
    }

    void clearDescriptorPoolAndUnregisterDescriptorSets(void* context, VkDevice device, VkDescriptorPool pool) {

        std::vector<VkDescriptorSet> toClear =
            clearDescriptorPool(pool, mFeatureInfo->hasVulkanBatchedDescriptorSetUpdate);

        for (auto set : toClear) {
            if (mFeatureInfo->hasVulkanBatchedDescriptorSetUpdate) {
                VkDescriptorSetLayout setLayout = as_goldfish_VkDescriptorSet(set)->reified->setLayout;
                decDescriptorSetLayoutRef(context, device, setLayout, nullptr);
            }
            unregister_VkDescriptorSet(set);
            delete_goldfish_VkDescriptorSet(set);
        }
    }

    void unregister_VkDescriptorPool(VkDescriptorPool pool) {
        if (!pool) return;

        AutoLock<RecursiveLock> lock(mLock);

        struct goldfish_VkDescriptorPool* dp = as_goldfish_VkDescriptorPool(pool);
        delete dp->allocInfo;

        info_VkDescriptorPool.erase(pool);
    }

    bool descriptorPoolSupportsIndividualFreeLocked(VkDescriptorPool pool) {
        return as_goldfish_VkDescriptorPool(pool)->allocInfo->createFlags &
            VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    }

    static constexpr uint32_t kDefaultApiVersion = VK_MAKE_VERSION(1, 1, 0);

    void setInstanceInfo(VkInstance instance,
                         uint32_t enabledExtensionCount,
                         const char* const* ppEnabledExtensionNames,
                         uint32_t apiVersion) {
        AutoLock<RecursiveLock> lock(mLock);
        auto& info = info_VkInstance[instance];
        info.highestApiVersion = apiVersion;

        if (!ppEnabledExtensionNames) return;

        for (uint32_t i = 0; i < enabledExtensionCount; ++i) {
            info.enabledExtensions.insert(ppEnabledExtensionNames[i]);
        }
    }

    void setDeviceInfo(VkDevice device,
                       VkPhysicalDevice physdev,
                       VkPhysicalDeviceProperties props,
                       VkPhysicalDeviceMemoryProperties memProps,
                       uint32_t enabledExtensionCount,
                       const char* const* ppEnabledExtensionNames,
                       const void* pNext) {
        AutoLock<RecursiveLock> lock(mLock);
        auto& info = info_VkDevice[device];
        info.physdev = physdev;
        info.props = props;
        info.memProps = memProps;
        info.apiVersion = props.apiVersion;

        const VkBaseInStructure *extensionCreateInfo =
            reinterpret_cast<const VkBaseInStructure *>(pNext);
        while(extensionCreateInfo) {
            if(extensionCreateInfo->sType
                == VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT) {
                auto deviceMemoryReportCreateInfo =
                    reinterpret_cast<const VkDeviceDeviceMemoryReportCreateInfoEXT *>(
                        extensionCreateInfo);
                if(deviceMemoryReportCreateInfo->pfnUserCallback != nullptr) {
                    info.deviceMemoryReportCallbacks.emplace_back(
                        deviceMemoryReportCreateInfo->pfnUserCallback,
                        deviceMemoryReportCreateInfo->pUserData);
                }
            }
            extensionCreateInfo = extensionCreateInfo->pNext;
        }

        if (!ppEnabledExtensionNames) return;

        for (uint32_t i = 0; i < enabledExtensionCount; ++i) {
            info.enabledExtensions.insert(ppEnabledExtensionNames[i]);
        }
    }

    void emitDeviceMemoryReport(VkDevice_Info info,
                                VkDeviceMemoryReportEventTypeEXT type,
                                uint64_t memoryObjectId,
                                VkDeviceSize size,
                                VkObjectType objectType,
                                uint64_t objectHandle,
                                uint32_t heapIndex = 0) {
        if(info.deviceMemoryReportCallbacks.empty()) return;

        const VkDeviceMemoryReportCallbackDataEXT callbackData = {
            VK_STRUCTURE_TYPE_DEVICE_MEMORY_REPORT_CALLBACK_DATA_EXT,  // sType
            nullptr,                                                   // pNext
            0,                                                         // flags
            type,                                                      // type
            memoryObjectId,                                            // memoryObjectId
            size,                                                      // size
            objectType,                                                // objectType
            objectHandle,                                              // objectHandle
            heapIndex,                                                 // heapIndex
        };
        for(const auto &callback : info.deviceMemoryReportCallbacks) {
            callback.first(&callbackData, callback.second);
        }
    }

    void setDeviceMemoryInfo(VkDevice device,
                             VkDeviceMemory memory,
                             VkDeviceSize allocationSize,
                             uint8_t* ptr,
                             uint32_t memoryTypeIndex,
                             AHardwareBuffer* ahw = nullptr,
                             bool imported = false,
                             zx_handle_t vmoHandle = ZX_HANDLE_INVALID) {
        AutoLock<RecursiveLock> lock(mLock);
        auto& info = info_VkDeviceMemory[memory];

        info.device = device;
        info.allocationSize = allocationSize;
        info.ptr = ptr;
        info.memoryTypeIndex = memoryTypeIndex;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        info.ahw = ahw;
#endif
        info.imported = imported;
        info.vmoHandle = vmoHandle;
    }

    void setImageInfo(VkImage image,
                      VkDevice device,
                      const VkImageCreateInfo *pCreateInfo) {
        AutoLock<RecursiveLock> lock(mLock);
        auto& info = info_VkImage[image];

        info.device = device;
        info.createInfo = *pCreateInfo;
    }

    uint8_t* getMappedPointer(VkDeviceMemory memory) {
        AutoLock<RecursiveLock> lock(mLock);
        const auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return nullptr;

        const auto& info = it->second;
        return info.ptr;
    }

    VkDeviceSize getMappedSize(VkDeviceMemory memory) {
        AutoLock<RecursiveLock> lock(mLock);
        const auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return 0;

        const auto& info = it->second;
        return info.allocationSize;
    }

    bool isValidMemoryRange(const VkMappedMemoryRange& range) const {
        AutoLock<RecursiveLock> lock(mLock);
        const auto it = info_VkDeviceMemory.find(range.memory);
        if (it == info_VkDeviceMemory.end()) return false;
        const auto& info = it->second;

        if (!info.ptr) return false;

        VkDeviceSize offset = range.offset;
        VkDeviceSize size = range.size;

        if (size == VK_WHOLE_SIZE) {
            return offset <= info.allocationSize;
        }

        return offset + size <= info.allocationSize;
    }

    void setupCaps(void) {
        VirtGpuDevice& instance = VirtGpuDevice::getInstance((enum VirtGpuCapset)3);
        mCaps = instance.getCaps();

        // Delete once goldfish Linux drivers are gone
        if (mCaps.gfxstreamCapset.protocolVersion == 0) {
            mCaps.gfxstreamCapset.colorBufferMemoryIndex = 0xFFFFFFFF;
        }
    }

    void setupFeatures(const EmulatorFeatureInfo* features) {
        if (!features || mFeatureInfo) return;
        mFeatureInfo.reset(new EmulatorFeatureInfo);
        *mFeatureInfo = *features;

        if (mFeatureInfo->hasDirectMem) {
            mGoldfishAddressSpaceBlockProvider.reset(
                new GoldfishAddressSpaceBlockProvider(
                    GoldfishAddressSpaceSubdeviceType::NoSubdevice));
        }

#ifdef VK_USE_PLATFORM_FUCHSIA
        if (mFeatureInfo->hasVulkan) {
            fidl::ClientEnd<fuchsia_hardware_goldfish::ControlDevice> channel{
                zx::channel(GetConnectToServiceFunction()("/loader-gpu-devices/class/goldfish-control/000"))};
            if (!channel) {
                ALOGE("failed to open control device");
                abort();
            }
            mControlDevice =
                fidl::WireSyncClient<fuchsia_hardware_goldfish::ControlDevice>(
                    std::move(channel));

            fidl::ClientEnd<fuchsia_sysmem::Allocator> sysmem_channel{
                zx::channel(GetConnectToServiceFunction()("/svc/fuchsia.sysmem.Allocator"))};
            if (!sysmem_channel) {
                ALOGE("failed to open sysmem connection");
            }
            mSysmemAllocator =
                fidl::WireSyncClient<fuchsia_sysmem::Allocator>(
                    std::move(sysmem_channel));
            char name[ZX_MAX_NAME_LEN] = {};
            zx_object_get_property(zx_process_self(), ZX_PROP_NAME, name, sizeof(name));
            std::string client_name(name);
            client_name += "-goldfish";
            zx_info_handle_basic_t info;
            zx_object_get_info(zx_process_self(), ZX_INFO_HANDLE_BASIC, &info, sizeof(info),
                               nullptr, nullptr);
            mSysmemAllocator->SetDebugClientInfo(fidl::StringView::FromExternal(client_name),
                                                 info.koid);
        }
#endif

        if (mFeatureInfo->hasVulkanNullOptionalStrings) {
            ResourceTracker::streamFeatureBits |= VULKAN_STREAM_FEATURE_NULL_OPTIONAL_STRINGS_BIT;
        }
        if (mFeatureInfo->hasVulkanIgnoredHandles) {
            ResourceTracker::streamFeatureBits |= VULKAN_STREAM_FEATURE_IGNORED_HANDLES_BIT;
        }
        if (mFeatureInfo->hasVulkanShaderFloat16Int8) {
            ResourceTracker::streamFeatureBits |= VULKAN_STREAM_FEATURE_SHADER_FLOAT16_INT8_BIT;
        }
        if (mFeatureInfo->hasVulkanQueueSubmitWithCommands) {
            ResourceTracker::streamFeatureBits |= VULKAN_STREAM_FEATURE_QUEUE_SUBMIT_WITH_COMMANDS_BIT;
        }
    }

    void setThreadingCallbacks(const ResourceTracker::ThreadingCallbacks& callbacks) {
        ResourceTracker::threadingCallbacks = callbacks;
    }

    bool hostSupportsVulkan() const {
        if (!mFeatureInfo) return false;

        return mFeatureInfo->hasVulkan;
    }

    bool usingDirectMapping() const {
        return true;
    }

    uint32_t getStreamFeatures() const {
        return ResourceTracker::streamFeatureBits;
    }

    bool supportsDeferredCommands() const {
        if (!mFeatureInfo) return false;
        return mFeatureInfo->hasDeferredVulkanCommands;
    }

    bool supportsAsyncQueueSubmit() const {
        if (!mFeatureInfo) return false;
        return mFeatureInfo->hasVulkanAsyncQueueSubmit;
    }

    bool supportsCreateResourcesWithRequirements() const {
        if (!mFeatureInfo) return false;
        return mFeatureInfo->hasVulkanCreateResourcesWithRequirements;
    }

    int getHostInstanceExtensionIndex(const std::string& extName) const {
        int i = 0;
        for (const auto& prop : mHostInstanceExtensions) {
            if (extName == std::string(prop.extensionName)) {
                return i;
            }
            ++i;
        }
        return -1;
    }

    int getHostDeviceExtensionIndex(const std::string& extName) const {
        int i = 0;
        for (const auto& prop : mHostDeviceExtensions) {
            if (extName == std::string(prop.extensionName)) {
                return i;
            }
            ++i;
        }
        return -1;
    }

    void deviceMemoryTransform_tohost(
        VkDeviceMemory* memory, uint32_t memoryCount,
        VkDeviceSize* offset, uint32_t offsetCount,
        VkDeviceSize* size, uint32_t sizeCount,
        uint32_t* typeIndex, uint32_t typeIndexCount,
        uint32_t* typeBits, uint32_t typeBitsCount) {

        (void)memoryCount;
        (void)offsetCount;
        (void)sizeCount;
        (void)typeIndex;
        (void)typeIndexCount;
        (void)typeBits;
        (void)typeBitsCount;

        if (memory) {
            AutoLock<RecursiveLock> lock (mLock);

            for (uint32_t i = 0; i < memoryCount; ++i) {
                VkDeviceMemory mem = memory[i];

                auto it = info_VkDeviceMemory.find(mem);
                if (it == info_VkDeviceMemory.end())
                    return;

                const auto& info = it->second;

                if (!info.coherentMemory)
                    continue;

                memory[i] = info.coherentMemory->getDeviceMemory();

                if (offset) {
                    offset[i] = info.coherentMemoryOffset + offset[i];
                }

                if (size && size[i] == VK_WHOLE_SIZE) {
                    size[i] = info.allocationSize;
                }

                // TODO
                (void)memory;
                (void)offset;
                (void)size;
            }
        }
    }

    void deviceMemoryTransform_fromhost(
        VkDeviceMemory* memory, uint32_t memoryCount,
        VkDeviceSize* offset, uint32_t offsetCount,
        VkDeviceSize* size, uint32_t sizeCount,
        uint32_t* typeIndex, uint32_t typeIndexCount,
        uint32_t* typeBits, uint32_t typeBitsCount) {

        (void)memory;
        (void)memoryCount;
        (void)offset;
        (void)offsetCount;
        (void)size;
        (void)sizeCount;
        (void)typeIndex;
        (void)typeIndexCount;
        (void)typeBits;
        (void)typeBitsCount;
    }

    void transformImpl_VkExternalMemoryProperties_fromhost(
        VkExternalMemoryProperties* pProperties,
        uint32_t) {
        VkExternalMemoryHandleTypeFlags supportedHandleType = 0u;
#ifdef VK_USE_PLATFORM_FUCHSIA
        supportedHandleType |=
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_ZIRCON_VMO_BIT_FUCHSIA;
#endif  // VK_USE_PLATFORM_FUCHSIA
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        supportedHandleType |=
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
#endif  // VK_USE_PLATFORM_ANDROID_KHR
        if (supportedHandleType) {
            pProperties->compatibleHandleTypes &= supportedHandleType;
            pProperties->exportFromImportedHandleTypes &= supportedHandleType;
        }
    }

    VkResult on_vkEnumerateInstanceExtensionProperties(
        void* context,
        VkResult,
        const char*,
        uint32_t* pPropertyCount,
        VkExtensionProperties* pProperties) {
        std::vector<const char*> allowedExtensionNames = {
            "VK_KHR_get_physical_device_properties2",
            "VK_KHR_sampler_ycbcr_conversion",
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
            "VK_KHR_external_semaphore_capabilities",
            "VK_KHR_external_memory_capabilities",
            "VK_KHR_external_fence_capabilities",
#endif
        };

        VkEncoder* enc = (VkEncoder*)context;

        // Only advertise a select set of extensions.
        if (mHostInstanceExtensions.empty()) {
            uint32_t hostPropCount = 0;
            enc->vkEnumerateInstanceExtensionProperties(nullptr, &hostPropCount, nullptr, true /* do lock */);
            mHostInstanceExtensions.resize(hostPropCount);

            VkResult hostRes =
                enc->vkEnumerateInstanceExtensionProperties(
                    nullptr, &hostPropCount, mHostInstanceExtensions.data(), true /* do lock */);

            if (hostRes != VK_SUCCESS) {
                return hostRes;
            }
        }

        std::vector<VkExtensionProperties> filteredExts;

        for (size_t i = 0; i < allowedExtensionNames.size(); ++i) {
            auto extIndex = getHostInstanceExtensionIndex(allowedExtensionNames[i]);
            if (extIndex != -1) {
                filteredExts.push_back(mHostInstanceExtensions[extIndex]);
            }
        }

        VkExtensionProperties anbExtProps[] = {
#ifdef VK_USE_PLATFORM_FUCHSIA
            { "VK_KHR_external_memory_capabilities", 1},
            { "VK_KHR_external_semaphore_capabilities", 1},
#endif
        };

        for (auto& anbExtProp: anbExtProps) {
            filteredExts.push_back(anbExtProp);
        }

        // Spec:
        //
        // https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/vkEnumerateInstanceExtensionProperties.html
        //
        // If pProperties is NULL, then the number of extensions properties
        // available is returned in pPropertyCount. Otherwise, pPropertyCount
        // must point to a variable set by the user to the number of elements
        // in the pProperties array, and on return the variable is overwritten
        // with the number of structures actually written to pProperties. If
        // pPropertyCount is less than the number of extension properties
        // available, at most pPropertyCount structures will be written. If
        // pPropertyCount is smaller than the number of extensions available,
        // VK_INCOMPLETE will be returned instead of VK_SUCCESS, to indicate
        // that not all the available properties were returned.
        //
        // pPropertyCount must be a valid pointer to a uint32_t value
        if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;

        if (!pProperties) {
            *pPropertyCount = (uint32_t)filteredExts.size();
            return VK_SUCCESS;
        } else {
            auto actualExtensionCount = (uint32_t)filteredExts.size();
            if (*pPropertyCount > actualExtensionCount) {
              *pPropertyCount = actualExtensionCount;
            }

            for (uint32_t i = 0; i < *pPropertyCount; ++i) {
                pProperties[i] = filteredExts[i];
            }

            if (actualExtensionCount > *pPropertyCount) {
                return VK_INCOMPLETE;
            }

            return VK_SUCCESS;
        }
    }

    VkResult on_vkEnumerateDeviceExtensionProperties(
        void* context,
        VkResult,
        VkPhysicalDevice physdev,
        const char*,
        uint32_t* pPropertyCount,
        VkExtensionProperties* pProperties) {
        std::vector<const char*> allowedExtensionNames = {
            "VK_KHR_vulkan_memory_model",
            "VK_KHR_buffer_device_address",
            "VK_KHR_maintenance1",
            "VK_KHR_maintenance2",
            "VK_KHR_maintenance3",
            "VK_KHR_bind_memory2",
            "VK_KHR_dedicated_allocation",
            "VK_KHR_get_memory_requirements2",
            "VK_KHR_sampler_ycbcr_conversion",
            "VK_KHR_shader_float16_int8",
        // Timeline semaphores buggy in newer NVIDIA drivers
        // (vkWaitSemaphoresKHR causes further vkCommandBuffer dispatches to deadlock)
#ifndef VK_USE_PLATFORM_ANDROID_KHR
            "VK_KHR_timeline_semaphore",
#endif
            "VK_AMD_gpu_shader_half_float",
            "VK_NV_shader_subgroup_partitioned",
            "VK_KHR_shader_subgroup_extended_types",
            "VK_EXT_subgroup_size_control",
            "VK_EXT_provoking_vertex",
            "VK_EXT_line_rasterization",
            "VK_KHR_shader_terminate_invocation",
            "VK_EXT_transform_feedback",
            "VK_EXT_primitive_topology_list_restart",
            "VK_EXT_index_type_uint8",
            "VK_EXT_load_store_op_none",
            "VK_EXT_swapchain_colorspace",
            "VK_EXT_image_robustness",
            "VK_EXT_custom_border_color",
            "VK_EXT_shader_stencil_export",
            "VK_KHR_image_format_list",
            "VK_KHR_incremental_present",
            "VK_KHR_pipeline_executable_properties",
            "VK_EXT_queue_family_foreign",
            "VK_KHR_descriptor_update_template",
            "VK_KHR_storage_buffer_storage_class",
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
            "VK_KHR_external_semaphore",
            "VK_KHR_external_semaphore_fd",
            // "VK_KHR_external_semaphore_win32", not exposed because it's translated to fd
            "VK_KHR_external_memory",
            "VK_KHR_external_fence",
            "VK_KHR_external_fence_fd",
            "VK_EXT_device_memory_report",
#endif
#if !defined(VK_USE_PLATFORM_ANDROID_KHR) && defined(__linux__)
            "VK_KHR_create_renderpass2",
            "VK_KHR_imageless_framebuffer",
#endif
            // Vulkan 1.3
            "VK_KHR_synchronization2",
            "VK_EXT_private_data",
        };

        VkEncoder* enc = (VkEncoder*)context;

        if (mHostDeviceExtensions.empty()) {
            uint32_t hostPropCount = 0;
            enc->vkEnumerateDeviceExtensionProperties(physdev, nullptr, &hostPropCount, nullptr, true /* do lock */);
            mHostDeviceExtensions.resize(hostPropCount);

            VkResult hostRes =
                enc->vkEnumerateDeviceExtensionProperties(
                    physdev, nullptr, &hostPropCount, mHostDeviceExtensions.data(), true /* do lock */);

            if (hostRes != VK_SUCCESS) {
                return hostRes;
            }
        }

        bool hostHasWin32ExternalSemaphore =
            getHostDeviceExtensionIndex(
                "VK_KHR_external_semaphore_win32") != -1;

        bool hostHasPosixExternalSemaphore =
            getHostDeviceExtensionIndex(
                "VK_KHR_external_semaphore_fd") != -1;

        D("%s: host has ext semaphore? win32 %d posix %d\n", __func__,
          hostHasWin32ExternalSemaphore,
          hostHasPosixExternalSemaphore);

        bool hostSupportsExternalSemaphore =
            hostHasWin32ExternalSemaphore ||
            hostHasPosixExternalSemaphore;

        std::vector<VkExtensionProperties> filteredExts;

        for (size_t i = 0; i < allowedExtensionNames.size(); ++i) {
            auto extIndex = getHostDeviceExtensionIndex(allowedExtensionNames[i]);
            if (extIndex != -1) {
                filteredExts.push_back(mHostDeviceExtensions[extIndex]);
            }
        }

        VkExtensionProperties anbExtProps[] = {
#ifdef VK_USE_PLATFORM_ANDROID_KHR
            { "VK_ANDROID_native_buffer", 7 },
#endif
#ifdef VK_USE_PLATFORM_FUCHSIA
            { "VK_KHR_external_memory", 1 },
            { "VK_KHR_external_semaphore", 1 },
            { "VK_FUCHSIA_external_semaphore", 1 },
#endif
        };

        for (auto& anbExtProp: anbExtProps) {
            filteredExts.push_back(anbExtProp);
        }

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        bool hostSupportsExternalFenceFd =
            getHostDeviceExtensionIndex(
                "VK_KHR_external_fence_fd") != -1;
        if (!hostSupportsExternalFenceFd) {
            filteredExts.push_back(
                VkExtensionProperties { "VK_KHR_external_fence_fd", 1});
        }
#endif

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (hostSupportsExternalSemaphore &&
            !hostHasPosixExternalSemaphore) {
            filteredExts.push_back(
                VkExtensionProperties { "VK_KHR_external_semaphore_fd", 1});
        }
#endif

        bool win32ExtMemAvailable =
            getHostDeviceExtensionIndex(
                "VK_KHR_external_memory_win32") != -1;
        bool posixExtMemAvailable =
            getHostDeviceExtensionIndex(
                "VK_KHR_external_memory_fd") != -1;
        bool moltenVkExtAvailable =
            getHostDeviceExtensionIndex(
                "VK_MVK_moltenvk") != -1;

        bool hostHasExternalMemorySupport =
            win32ExtMemAvailable || posixExtMemAvailable || moltenVkExtAvailable;

        if (hostHasExternalMemorySupport) {
#ifdef VK_USE_PLATFORM_ANDROID_KHR
            filteredExts.push_back(
                VkExtensionProperties {
                   "VK_ANDROID_external_memory_android_hardware_buffer", 7
                });
            filteredExts.push_back(
                VkExtensionProperties { "VK_EXT_queue_family_foreign", 1 });
#endif
#ifdef VK_USE_PLATFORM_FUCHSIA
            filteredExts.push_back(
                VkExtensionProperties { "VK_FUCHSIA_external_memory", 1});
            filteredExts.push_back(
                VkExtensionProperties { "VK_FUCHSIA_buffer_collection", 1 });
#endif
#if !defined(VK_USE_PLATFORM_ANDROID_KHR) && defined(__linux__)
            filteredExts.push_back(
                VkExtensionProperties {
                   "VK_KHR_external_memory_fd", 1
                });
            filteredExts.push_back(
                VkExtensionProperties { "VK_EXT_external_memory_dma_buf", 1 });
#endif
        }

        // Spec:
        //
        // https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/vkEnumerateDeviceExtensionProperties.html
        //
        // pPropertyCount is a pointer to an integer related to the number of
        // extension properties available or queried, and is treated in the
        // same fashion as the
        // vkEnumerateInstanceExtensionProperties::pPropertyCount parameter.
        //
        // https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/vkEnumerateInstanceExtensionProperties.html
        //
        // If pProperties is NULL, then the number of extensions properties
        // available is returned in pPropertyCount. Otherwise, pPropertyCount
        // must point to a variable set by the user to the number of elements
        // in the pProperties array, and on return the variable is overwritten
        // with the number of structures actually written to pProperties. If
        // pPropertyCount is less than the number of extension properties
        // available, at most pPropertyCount structures will be written. If
        // pPropertyCount is smaller than the number of extensions available,
        // VK_INCOMPLETE will be returned instead of VK_SUCCESS, to indicate
        // that not all the available properties were returned.
        //
        // pPropertyCount must be a valid pointer to a uint32_t value

        if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;

        if (!pProperties) {
            *pPropertyCount = (uint32_t)filteredExts.size();
            return VK_SUCCESS;
        } else {
            auto actualExtensionCount = (uint32_t)filteredExts.size();
            if (*pPropertyCount > actualExtensionCount) {
              *pPropertyCount = actualExtensionCount;
            }

            for (uint32_t i = 0; i < *pPropertyCount; ++i) {
                pProperties[i] = filteredExts[i];
            }

            if (actualExtensionCount > *pPropertyCount) {
                return VK_INCOMPLETE;
            }

            return VK_SUCCESS;
        }
    }

    VkResult on_vkEnumeratePhysicalDevices(
        void* context, VkResult,
        VkInstance instance, uint32_t* pPhysicalDeviceCount,
        VkPhysicalDevice* pPhysicalDevices) {

        VkEncoder* enc = (VkEncoder*)context;

        if (!instance) return VK_ERROR_INITIALIZATION_FAILED;

        if (!pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;

        AutoLock<RecursiveLock> lock(mLock);

        // When this function is called, we actually need to do two things:
        // - Get full information about physical devices from the host,
        // even if the guest did not ask for it
        // - Serve the guest query according to the spec:
        //
        // https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/vkEnumeratePhysicalDevices.html

        auto it = info_VkInstance.find(instance);

        if (it == info_VkInstance.end()) return VK_ERROR_INITIALIZATION_FAILED;

        auto& info = it->second;

        // Get the full host information here if it doesn't exist already.
        if (info.physicalDevices.empty()) {
            uint32_t hostPhysicalDeviceCount = 0;

            lock.unlock();
            VkResult countRes = enc->vkEnumeratePhysicalDevices(
                instance, &hostPhysicalDeviceCount, nullptr, false /* no lock */);
            lock.lock();

            if (countRes != VK_SUCCESS) {
                ALOGE("%s: failed: could not count host physical devices. "
                      "Error %d\n", __func__, countRes);
                return countRes;
            }

            info.physicalDevices.resize(hostPhysicalDeviceCount);

            lock.unlock();
            VkResult enumRes = enc->vkEnumeratePhysicalDevices(
                instance, &hostPhysicalDeviceCount, info.physicalDevices.data(), false /* no lock */);
            lock.lock();

            if (enumRes != VK_SUCCESS) {
                ALOGE("%s: failed: could not retrieve host physical devices. "
                      "Error %d\n", __func__, enumRes);
                return enumRes;
            }
        }

        // Serve the guest query according to the spec.
        //
        // https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/vkEnumeratePhysicalDevices.html
        //
        // If pPhysicalDevices is NULL, then the number of physical devices
        // available is returned in pPhysicalDeviceCount. Otherwise,
        // pPhysicalDeviceCount must point to a variable set by the user to the
        // number of elements in the pPhysicalDevices array, and on return the
        // variable is overwritten with the number of handles actually written
        // to pPhysicalDevices. If pPhysicalDeviceCount is less than the number
        // of physical devices available, at most pPhysicalDeviceCount
        // structures will be written.  If pPhysicalDeviceCount is smaller than
        // the number of physical devices available, VK_INCOMPLETE will be
        // returned instead of VK_SUCCESS, to indicate that not all the
        // available physical devices were returned.

        if (!pPhysicalDevices) {
            *pPhysicalDeviceCount = (uint32_t)info.physicalDevices.size();
            return VK_SUCCESS;
        } else {
            uint32_t actualDeviceCount = (uint32_t)info.physicalDevices.size();
            uint32_t toWrite = actualDeviceCount < *pPhysicalDeviceCount ? actualDeviceCount : *pPhysicalDeviceCount;

            for (uint32_t i = 0; i < toWrite; ++i) {
                pPhysicalDevices[i] = info.physicalDevices[i];
            }

            *pPhysicalDeviceCount = toWrite;

            if (actualDeviceCount > *pPhysicalDeviceCount) {
                return VK_INCOMPLETE;
            }

            return VK_SUCCESS;
        }
    }

    void on_vkGetPhysicalDeviceProperties(
        void*,
        VkPhysicalDevice,
        VkPhysicalDeviceProperties*) {
    }

    void on_vkGetPhysicalDeviceFeatures2(
        void*,
        VkPhysicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures) {
        if (pFeatures) {
            VkPhysicalDeviceDeviceMemoryReportFeaturesEXT* memoryReportFeaturesEXT =
                vk_find_struct<VkPhysicalDeviceDeviceMemoryReportFeaturesEXT>(pFeatures);
            if (memoryReportFeaturesEXT) {
                memoryReportFeaturesEXT->deviceMemoryReport = VK_TRUE;
            }
        }
    }

    void on_vkGetPhysicalDeviceProperties2(
        void*,
        VkPhysicalDevice,
        VkPhysicalDeviceProperties2* pProperties) {
        if (pProperties) {
            VkPhysicalDeviceDeviceMemoryReportFeaturesEXT* memoryReportFeaturesEXT =
                vk_find_struct<VkPhysicalDeviceDeviceMemoryReportFeaturesEXT>(pProperties);
            if (memoryReportFeaturesEXT) {
                memoryReportFeaturesEXT->deviceMemoryReport = VK_TRUE;
            }
        }
    }

    void on_vkGetPhysicalDeviceMemoryProperties(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties* out) {
        // gfxstream decides which physical device to expose to the guest on startup.
        // Otherwise, we would need a physical device to properties mapping.
        *out = getPhysicalDeviceMemoryProperties(context, VK_NULL_HANDLE, physicalDevice);
    }

    void on_vkGetPhysicalDeviceMemoryProperties2(
        void*,
        VkPhysicalDevice physdev,
        VkPhysicalDeviceMemoryProperties2* out) {

        on_vkGetPhysicalDeviceMemoryProperties(nullptr, physdev, &out->memoryProperties);
    }

    void on_vkGetDeviceQueue(void*,
                             VkDevice device,
                             uint32_t,
                             uint32_t,
                             VkQueue* pQueue) {
        AutoLock<RecursiveLock> lock(mLock);
        info_VkQueue[*pQueue].device = device;
    }

    void on_vkGetDeviceQueue2(void*,
                              VkDevice device,
                              const VkDeviceQueueInfo2*,
                              VkQueue* pQueue) {
        AutoLock<RecursiveLock> lock(mLock);
        info_VkQueue[*pQueue].device = device;
    }

    VkResult on_vkCreateInstance(
        void* context,
        VkResult input_result,
        const VkInstanceCreateInfo* createInfo,
        const VkAllocationCallbacks*,
        VkInstance* pInstance) {

        if (input_result != VK_SUCCESS) return input_result;

        VkEncoder* enc = (VkEncoder*)context;

        uint32_t apiVersion;
        VkResult enumInstanceVersionRes =
            enc->vkEnumerateInstanceVersion(&apiVersion, false /* no lock */);

        setInstanceInfo(
            *pInstance,
            createInfo->enabledExtensionCount,
            createInfo->ppEnabledExtensionNames,
            apiVersion);

        return input_result;
    }

    VkResult on_vkCreateDevice(
        void* context,
        VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks*,
        VkDevice* pDevice) {

        if (input_result != VK_SUCCESS) return input_result;

        VkEncoder* enc = (VkEncoder*)context;

        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceMemoryProperties memProps;
        enc->vkGetPhysicalDeviceProperties(physicalDevice, &props, false /* no lock */);
        enc->vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps, false /* no lock */);

        setDeviceInfo(
            *pDevice, physicalDevice, props, memProps,
            pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames,
            pCreateInfo->pNext);

        return input_result;
    }

    void on_vkDestroyDevice_pre(
        void* context,
        VkDevice device,
        const VkAllocationCallbacks*) {

        (void)context;
        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return;

        for (auto itr = info_VkDeviceMemory.cbegin() ; itr != info_VkDeviceMemory.cend(); ) {
            auto& memInfo = itr->second;
            if (memInfo.device == device) {
                itr = info_VkDeviceMemory.erase(itr);
            } else {
                itr++;
            }
        }
    }

#ifdef VK_USE_PLATFORM_ANDROID_KHR
    uint32_t getColorBufferMemoryIndex(void* context, VkDevice device) {
        // Create test image to get the memory requirements
        VkEncoder* enc = (VkEncoder*)context;
        VkImageCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {64, 64, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_MAX_ENUM,
        };
        VkImage image = VK_NULL_HANDLE;
        VkResult res = enc->vkCreateImage(device, &createInfo, nullptr, &image, true /* do lock */);

        if (res != VK_SUCCESS) {
            return 0;
        }

        VkMemoryRequirements memReqs;
        enc->vkGetImageMemoryRequirements(
            device, image, &memReqs, true /* do lock */);
        enc->vkDestroyImage(device, image, nullptr, true /* do lock */);

        const VkPhysicalDeviceMemoryProperties& memProps =
                getPhysicalDeviceMemoryProperties(context, device, VK_NULL_HANDLE);

        // Currently, host looks for the last index that has with memory
        // property type VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        VkMemoryPropertyFlags memoryProperty = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        for (int i = VK_MAX_MEMORY_TYPES - 1; i >= 0; --i) {
            if ((memReqs.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & memoryProperty)) {
                return i;
            }
        }

        return 0;
    }

    VkResult on_vkGetAndroidHardwareBufferPropertiesANDROID(
            void* context, VkResult,
            VkDevice device,
            const AHardwareBuffer* buffer,
            VkAndroidHardwareBufferPropertiesANDROID* pProperties) {
        auto grallocHelper =
            ResourceTracker::threadingCallbacks.hostConnectionGetFunc()->grallocHelper();

        // Delete once goldfish Linux drivers are gone
        if (mCaps.gfxstreamCapset.colorBufferMemoryIndex == 0xFFFFFFFF) {
            mCaps.gfxstreamCapset.colorBufferMemoryIndex =
                    getColorBufferMemoryIndex(context, device);
        }

        updateMemoryTypeBits(&pProperties->memoryTypeBits,
                             mCaps.gfxstreamCapset.colorBufferMemoryIndex);

        return getAndroidHardwareBufferPropertiesANDROID(
            grallocHelper, buffer, pProperties);
    }

    VkResult on_vkGetMemoryAndroidHardwareBufferANDROID(
        void*, VkResult,
        VkDevice device,
        const VkMemoryGetAndroidHardwareBufferInfoANDROID *pInfo,
        struct AHardwareBuffer** pBuffer) {

        if (!pInfo) return VK_ERROR_INITIALIZATION_FAILED;
        if (!pInfo->memory) return VK_ERROR_INITIALIZATION_FAILED;

        AutoLock<RecursiveLock> lock(mLock);

        auto deviceIt = info_VkDevice.find(device);

        if (deviceIt == info_VkDevice.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto memoryIt = info_VkDeviceMemory.find(pInfo->memory);

        if (memoryIt == info_VkDeviceMemory.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto& info = memoryIt->second;

        VkResult queryRes =
            getMemoryAndroidHardwareBufferANDROID(&info.ahw);

        if (queryRes != VK_SUCCESS) return queryRes;

        *pBuffer = info.ahw;

        return queryRes;
    }
#endif

#ifdef VK_USE_PLATFORM_FUCHSIA
    VkResult on_vkGetMemoryZirconHandleFUCHSIA(
        void*, VkResult,
        VkDevice device,
        const VkMemoryGetZirconHandleInfoFUCHSIA* pInfo,
        uint32_t* pHandle) {

        if (!pInfo) return VK_ERROR_INITIALIZATION_FAILED;
        if (!pInfo->memory) return VK_ERROR_INITIALIZATION_FAILED;

        AutoLock<RecursiveLock> lock(mLock);

        auto deviceIt = info_VkDevice.find(device);

        if (deviceIt == info_VkDevice.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto memoryIt = info_VkDeviceMemory.find(pInfo->memory);

        if (memoryIt == info_VkDeviceMemory.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto& info = memoryIt->second;

        if (info.vmoHandle == ZX_HANDLE_INVALID) {
            ALOGE("%s: memory cannot be exported", __func__);
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        *pHandle = ZX_HANDLE_INVALID;
        zx_handle_duplicate(info.vmoHandle, ZX_RIGHT_SAME_RIGHTS, pHandle);
        return VK_SUCCESS;
    }

    VkResult on_vkGetMemoryZirconHandlePropertiesFUCHSIA(
        void*, VkResult,
        VkDevice device,
        VkExternalMemoryHandleTypeFlagBits handleType,
        uint32_t handle,
        VkMemoryZirconHandlePropertiesFUCHSIA* pProperties) {
        using fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal;
        using fuchsia_hardware_goldfish::wire::kMemoryPropertyHostVisible;

        if (handleType !=
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ZIRCON_VMO_BIT_FUCHSIA) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        zx_info_handle_basic_t handleInfo;
        zx_status_t status = zx::unowned_vmo(handle)->get_info(
            ZX_INFO_HANDLE_BASIC, &handleInfo, sizeof(handleInfo), nullptr,
            nullptr);
        if (status != ZX_OK || handleInfo.type != ZX_OBJ_TYPE_VMO) {
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }

        AutoLock<RecursiveLock> lock(mLock);

        auto deviceIt = info_VkDevice.find(device);

        if (deviceIt == info_VkDevice.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto& info = deviceIt->second;

        zx::vmo vmo_dup;
        status =
            zx::unowned_vmo(handle)->duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo_dup);
        if (status != ZX_OK) {
            ALOGE("zx_handle_duplicate() error: %d", status);
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        uint32_t memoryProperty = 0u;

        auto result = mControlDevice->GetBufferHandleInfo(std::move(vmo_dup));
        if (!result.ok()) {
            ALOGE(
                "mControlDevice->GetBufferHandleInfo fatal error: epitaph: %d",
                result.status());
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (result.value().is_ok()) {
            memoryProperty = result.value().value()->info.memory_property();
        } else if (result.value().error_value() == ZX_ERR_NOT_FOUND) {
            // If an VMO is allocated while ColorBuffer/Buffer is not created,
            // it must be a device-local buffer, since for host-visible buffers,
            // ColorBuffer/Buffer is created at sysmem allocation time.
            memoryProperty = kMemoryPropertyDeviceLocal;
        } else {
            // Importing read-only host memory into the Vulkan driver should not
            // work, but it is not an error to try to do so. Returning a
            // VkMemoryZirconHandlePropertiesFUCHSIA with no available
            // memoryType bits should be enough for clients. See fxbug.dev/24225
            // for other issues this this flow.
            ALOGW("GetBufferHandleInfo failed: %d", result.value().error_value());
            pProperties->memoryTypeBits = 0;
            return VK_SUCCESS;
        }

        pProperties->memoryTypeBits = 0;
        for (uint32_t i = 0; i < info.memProps.memoryTypeCount; ++i) {
            if (((memoryProperty & kMemoryPropertyDeviceLocal) &&
                 (info.memProps.memoryTypes[i].propertyFlags &
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ||
                ((memoryProperty & kMemoryPropertyHostVisible) &&
                 (info.memProps.memoryTypes[i].propertyFlags &
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) {
                pProperties->memoryTypeBits |= 1ull << i;
            }
        }
        return VK_SUCCESS;
    }

    zx_koid_t getEventKoid(zx_handle_t eventHandle) {
        if (eventHandle == ZX_HANDLE_INVALID) {
            return ZX_KOID_INVALID;
        }

        zx_info_handle_basic_t info;
        zx_status_t status =
            zx_object_get_info(eventHandle, ZX_INFO_HANDLE_BASIC, &info,
                               sizeof(info), nullptr, nullptr);
        if (status != ZX_OK) {
            ALOGE("Cannot get object info of handle %u: %d", eventHandle,
                  status);
            return ZX_KOID_INVALID;
        }
        return info.koid;
    }

    VkResult on_vkImportSemaphoreZirconHandleFUCHSIA(
        void*, VkResult,
        VkDevice device,
        const VkImportSemaphoreZirconHandleInfoFUCHSIA* pInfo) {

        if (!pInfo) return VK_ERROR_INITIALIZATION_FAILED;
        if (!pInfo->semaphore) return VK_ERROR_INITIALIZATION_FAILED;

        AutoLock<RecursiveLock> lock(mLock);

        auto deviceIt = info_VkDevice.find(device);

        if (deviceIt == info_VkDevice.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto semaphoreIt = info_VkSemaphore.find(pInfo->semaphore);

        if (semaphoreIt == info_VkSemaphore.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto& info = semaphoreIt->second;

        if (info.eventHandle != ZX_HANDLE_INVALID) {
            zx_handle_close(info.eventHandle);
        }
#if VK_HEADER_VERSION < 174
        info.eventHandle = pInfo->handle;
#else // VK_HEADER_VERSION >= 174
        info.eventHandle = pInfo->zirconHandle;
#endif // VK_HEADER_VERSION < 174
        if (info.eventHandle != ZX_HANDLE_INVALID) {
            info.eventKoid = getEventKoid(info.eventHandle);
        }

        return VK_SUCCESS;
    }

    VkResult on_vkGetSemaphoreZirconHandleFUCHSIA(
        void*, VkResult,
        VkDevice device,
        const VkSemaphoreGetZirconHandleInfoFUCHSIA* pInfo,
        uint32_t* pHandle) {

        if (!pInfo) return VK_ERROR_INITIALIZATION_FAILED;
        if (!pInfo->semaphore) return VK_ERROR_INITIALIZATION_FAILED;

        AutoLock<RecursiveLock> lock(mLock);

        auto deviceIt = info_VkDevice.find(device);

        if (deviceIt == info_VkDevice.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto semaphoreIt = info_VkSemaphore.find(pInfo->semaphore);

        if (semaphoreIt == info_VkSemaphore.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto& info = semaphoreIt->second;

        if (info.eventHandle == ZX_HANDLE_INVALID) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        *pHandle = ZX_HANDLE_INVALID;
        zx_handle_duplicate(info.eventHandle, ZX_RIGHT_SAME_RIGHTS, pHandle);
        return VK_SUCCESS;
    }

    VkResult on_vkCreateBufferCollectionFUCHSIA(
        void*,
        VkResult,
        VkDevice,
        const VkBufferCollectionCreateInfoFUCHSIA* pInfo,
        const VkAllocationCallbacks*,
        VkBufferCollectionFUCHSIA* pCollection) {
        fidl::ClientEnd<::fuchsia_sysmem::BufferCollectionToken> token_client;

        if (pInfo->collectionToken) {
            token_client =
                fidl::ClientEnd<::fuchsia_sysmem::BufferCollectionToken>(
                    zx::channel(pInfo->collectionToken));
        } else {
            auto endpoints = fidl::CreateEndpoints<
                ::fuchsia_sysmem::BufferCollectionToken>();
            if (!endpoints.is_ok()) {
                ALOGE("zx_channel_create failed: %d", endpoints.status_value());
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            auto result = mSysmemAllocator->AllocateSharedCollection(
                std::move(endpoints->server));
            if (!result.ok()) {
                ALOGE("AllocateSharedCollection failed: %d", result.status());
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            token_client = std::move(endpoints->client);
        }

        auto endpoints =
            fidl::CreateEndpoints<::fuchsia_sysmem::BufferCollection>();
        if (!endpoints.is_ok()) {
            ALOGE("zx_channel_create failed: %d", endpoints.status_value());
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto [collection_client, collection_server] =
            std::move(endpoints.value());

        auto result = mSysmemAllocator->BindSharedCollection(
            std::move(token_client), std::move(collection_server));
        if (!result.ok()) {
            ALOGE("BindSharedCollection failed: %d", result.status());
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto* sysmem_collection =
            new fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>(
                std::move(collection_client));
        *pCollection =
            reinterpret_cast<VkBufferCollectionFUCHSIA>(sysmem_collection);

        register_VkBufferCollectionFUCHSIA(*pCollection);
        return VK_SUCCESS;
    }

    void on_vkDestroyBufferCollectionFUCHSIA(
        void*,
        VkResult,
        VkDevice,
        VkBufferCollectionFUCHSIA collection,
        const VkAllocationCallbacks*) {
        auto sysmem_collection = reinterpret_cast<
            fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>*>(
            collection);
        if (sysmem_collection) {
            (*sysmem_collection)->Close();
        }
        delete sysmem_collection;

        unregister_VkBufferCollectionFUCHSIA(collection);
    }

    inline fuchsia_sysmem::wire::BufferCollectionConstraints
    defaultBufferCollectionConstraints(
        size_t minSizeBytes,
        size_t minBufferCount,
        size_t maxBufferCount = 0u,
        size_t minBufferCountForCamping = 0u,
        size_t minBufferCountForDedicatedSlack = 0u,
        size_t minBufferCountForSharedSlack = 0u) {
        fuchsia_sysmem::wire::BufferCollectionConstraints constraints = {};
        constraints.min_buffer_count = minBufferCount;
        if (maxBufferCount > 0) {
            constraints.max_buffer_count = maxBufferCount;
        }
        if (minBufferCountForCamping) {
            constraints.min_buffer_count_for_camping = minBufferCountForCamping;
        }
        if (minBufferCountForSharedSlack) {
            constraints.min_buffer_count_for_shared_slack =
                minBufferCountForSharedSlack;
        }
        constraints.has_buffer_memory_constraints = true;
        fuchsia_sysmem::wire::BufferMemoryConstraints& buffer_constraints =
            constraints.buffer_memory_constraints;

        buffer_constraints.min_size_bytes = minSizeBytes;
        buffer_constraints.max_size_bytes = 0xffffffff;
        buffer_constraints.physically_contiguous_required = false;
        buffer_constraints.secure_required = false;

        // No restrictions on coherency domain or Heaps.
        buffer_constraints.ram_domain_supported = true;
        buffer_constraints.cpu_domain_supported = true;
        buffer_constraints.inaccessible_domain_supported = true;
        buffer_constraints.heap_permitted_count = 2;
        buffer_constraints.heap_permitted[0] =
            fuchsia_sysmem::wire::HeapType::kGoldfishDeviceLocal;
        buffer_constraints.heap_permitted[1] =
            fuchsia_sysmem::wire::HeapType::kGoldfishHostVisible;

        return constraints;
    }

    uint32_t getBufferCollectionConstraintsVulkanImageUsage(
        const VkImageCreateInfo* pImageInfo) {
        uint32_t usage = 0u;
        VkImageUsageFlags imageUsage = pImageInfo->usage;

#define SetUsageBit(BIT, VALUE)                                           \
    if (imageUsage & VK_IMAGE_USAGE_##BIT##_BIT) {                 \
        usage |= fuchsia_sysmem::wire::kVulkanImageUsage##VALUE; \
    }

        SetUsageBit(COLOR_ATTACHMENT, ColorAttachment);
        SetUsageBit(TRANSFER_SRC, TransferSrc);
        SetUsageBit(TRANSFER_DST, TransferDst);
        SetUsageBit(SAMPLED, Sampled);

#undef SetUsageBit
        return usage;
    }

    uint32_t getBufferCollectionConstraintsVulkanBufferUsage(
        VkBufferUsageFlags bufferUsage) {
        uint32_t usage = 0u;

#define SetUsageBit(BIT, VALUE)                                            \
    if (bufferUsage & VK_BUFFER_USAGE_##BIT##_BIT) {                \
        usage |= fuchsia_sysmem::wire::kVulkanBufferUsage##VALUE; \
    }

        SetUsageBit(TRANSFER_SRC, TransferSrc);
        SetUsageBit(TRANSFER_DST, TransferDst);
        SetUsageBit(UNIFORM_TEXEL_BUFFER, UniformTexelBuffer);
        SetUsageBit(STORAGE_TEXEL_BUFFER, StorageTexelBuffer);
        SetUsageBit(UNIFORM_BUFFER, UniformBuffer);
        SetUsageBit(STORAGE_BUFFER, StorageBuffer);
        SetUsageBit(INDEX_BUFFER, IndexBuffer);
        SetUsageBit(VERTEX_BUFFER, VertexBuffer);
        SetUsageBit(INDIRECT_BUFFER, IndirectBuffer);

#undef SetUsageBit
        return usage;
    }

    uint32_t getBufferCollectionConstraintsVulkanBufferUsage(
        const VkBufferConstraintsInfoFUCHSIA* pBufferConstraintsInfo) {
        VkBufferUsageFlags bufferUsage =
            pBufferConstraintsInfo->createInfo.usage;
        return getBufferCollectionConstraintsVulkanBufferUsage(bufferUsage);
    }

    static fuchsia_sysmem::wire::PixelFormatType vkFormatTypeToSysmem(
        VkFormat format) {
        switch (format) {
            case VK_FORMAT_B8G8R8A8_SINT:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_SNORM:
            case VK_FORMAT_B8G8R8A8_SSCALED:
            case VK_FORMAT_B8G8R8A8_USCALED:
                return fuchsia_sysmem::wire::PixelFormatType::kBgra32;
            case VK_FORMAT_R8G8B8A8_SINT:
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_R8G8B8A8_SNORM:
            case VK_FORMAT_R8G8B8A8_SSCALED:
            case VK_FORMAT_R8G8B8A8_USCALED:
                return fuchsia_sysmem::wire::PixelFormatType::kR8G8B8A8;
            case VK_FORMAT_R8_UNORM:
            case VK_FORMAT_R8_UINT:
            case VK_FORMAT_R8_USCALED:
            case VK_FORMAT_R8_SNORM:
            case VK_FORMAT_R8_SINT:
            case VK_FORMAT_R8_SSCALED:
            case VK_FORMAT_R8_SRGB:
                return fuchsia_sysmem::wire::PixelFormatType::kR8;
            case VK_FORMAT_R8G8_UNORM:
            case VK_FORMAT_R8G8_UINT:
            case VK_FORMAT_R8G8_USCALED:
            case VK_FORMAT_R8G8_SNORM:
            case VK_FORMAT_R8G8_SINT:
            case VK_FORMAT_R8G8_SSCALED:
            case VK_FORMAT_R8G8_SRGB:
                return fuchsia_sysmem::wire::PixelFormatType::kR8G8;
            default:
                return fuchsia_sysmem::wire::PixelFormatType::kInvalid;
        }
    }

    static bool vkFormatMatchesSysmemFormat(
        VkFormat vkFormat,
        fuchsia_sysmem::wire::PixelFormatType sysmemFormat) {
        switch (vkFormat) {
            case VK_FORMAT_B8G8R8A8_SINT:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_SNORM:
            case VK_FORMAT_B8G8R8A8_SSCALED:
            case VK_FORMAT_B8G8R8A8_USCALED:
                return sysmemFormat ==
                       fuchsia_sysmem::wire::PixelFormatType::kBgra32;
            case VK_FORMAT_R8G8B8A8_SINT:
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_R8G8B8A8_SNORM:
            case VK_FORMAT_R8G8B8A8_SSCALED:
            case VK_FORMAT_R8G8B8A8_USCALED:
                return sysmemFormat ==
                       fuchsia_sysmem::wire::PixelFormatType::kR8G8B8A8;
            case VK_FORMAT_R8_UNORM:
            case VK_FORMAT_R8_UINT:
            case VK_FORMAT_R8_USCALED:
            case VK_FORMAT_R8_SNORM:
            case VK_FORMAT_R8_SINT:
            case VK_FORMAT_R8_SSCALED:
            case VK_FORMAT_R8_SRGB:
                return sysmemFormat ==
                           fuchsia_sysmem::wire::PixelFormatType::kR8 ||
                       sysmemFormat ==
                           fuchsia_sysmem::wire::PixelFormatType::kL8;
            case VK_FORMAT_R8G8_UNORM:
            case VK_FORMAT_R8G8_UINT:
            case VK_FORMAT_R8G8_USCALED:
            case VK_FORMAT_R8G8_SNORM:
            case VK_FORMAT_R8G8_SINT:
            case VK_FORMAT_R8G8_SSCALED:
            case VK_FORMAT_R8G8_SRGB:
                return sysmemFormat ==
                       fuchsia_sysmem::wire::PixelFormatType::kR8G8;
            default:
                return false;
        }
    }

    static VkFormat sysmemPixelFormatTypeToVk(
        fuchsia_sysmem::wire::PixelFormatType format) {
        switch (format) {
            case fuchsia_sysmem::wire::PixelFormatType::kBgra32:
                return VK_FORMAT_B8G8R8A8_SRGB;
            case fuchsia_sysmem::wire::PixelFormatType::kR8G8B8A8:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case fuchsia_sysmem::wire::PixelFormatType::kL8:
            case fuchsia_sysmem::wire::PixelFormatType::kR8:
                return VK_FORMAT_R8_UNORM;
            case fuchsia_sysmem::wire::PixelFormatType::kR8G8:
                return VK_FORMAT_R8G8_UNORM;
            default:
                return VK_FORMAT_UNDEFINED;
        }
    }

    // TODO(fxbug.dev/90856): This is currently only used for allocating
    // memory for dedicated external images. It should be migrated to use
    // SetBufferCollectionImageConstraintsFUCHSIA.
    VkResult setBufferCollectionConstraintsFUCHSIA(
        VkEncoder* enc,
        VkDevice device,
        fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>* collection,
        const VkImageCreateInfo* pImageInfo) {
        if (pImageInfo == nullptr) {
            ALOGE("setBufferCollectionConstraints: pImageInfo cannot be null.");
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }

        const VkSysmemColorSpaceFUCHSIA kDefaultColorSpace = {
            .sType = VK_STRUCTURE_TYPE_SYSMEM_COLOR_SPACE_FUCHSIA,
            .pNext = nullptr,
            .colorSpace = static_cast<uint32_t>(
                fuchsia_sysmem::wire::ColorSpaceType::kSrgb),
        };

        std::vector<VkImageFormatConstraintsInfoFUCHSIA> formatInfos;
        if (pImageInfo->format == VK_FORMAT_UNDEFINED) {
            const auto kFormats = {
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_R8G8B8A8_SRGB,
            };
            for (auto format : kFormats) {
                // shallow copy, using pNext from pImageInfo directly.
                auto createInfo = *pImageInfo;
                createInfo.format = format;
                formatInfos.push_back(VkImageFormatConstraintsInfoFUCHSIA{
                    .sType =
                        VK_STRUCTURE_TYPE_IMAGE_FORMAT_CONSTRAINTS_INFO_FUCHSIA,
                    .pNext = nullptr,
                    .imageCreateInfo = createInfo,
                    .colorSpaceCount = 1,
                    .pColorSpaces = &kDefaultColorSpace,
                });
            }
        } else {
            formatInfos.push_back(VkImageFormatConstraintsInfoFUCHSIA{
                .sType =
                    VK_STRUCTURE_TYPE_IMAGE_FORMAT_CONSTRAINTS_INFO_FUCHSIA,
                .pNext = nullptr,
                .imageCreateInfo = *pImageInfo,
                .colorSpaceCount = 1,
                .pColorSpaces = &kDefaultColorSpace,
            });
        }

        VkImageConstraintsInfoFUCHSIA imageConstraints = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CONSTRAINTS_INFO_FUCHSIA,
            .pNext = nullptr,
            .formatConstraintsCount = static_cast<uint32_t>(formatInfos.size()),
            .pFormatConstraints = formatInfos.data(),
            .bufferCollectionConstraints =
                VkBufferCollectionConstraintsInfoFUCHSIA{
                    .sType =
                        VK_STRUCTURE_TYPE_BUFFER_COLLECTION_CONSTRAINTS_INFO_FUCHSIA,
                    .pNext = nullptr,
                    .minBufferCount = 1,
                    .maxBufferCount = 0,
                    .minBufferCountForCamping = 0,
                    .minBufferCountForDedicatedSlack = 0,
                    .minBufferCountForSharedSlack = 0,
                },
            .flags = 0u,
        };

        return setBufferCollectionImageConstraintsFUCHSIA(
            enc, device, collection, &imageConstraints);
    }

    VkResult addImageBufferCollectionConstraintsFUCHSIA(
        VkEncoder* enc,
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        const VkImageFormatConstraintsInfoFUCHSIA*
            formatConstraints,  // always non-zero
        VkImageTiling tiling,
        fuchsia_sysmem::wire::BufferCollectionConstraints* constraints) {
        // First check if the format, tiling and usage is supported on host.
        VkImageFormatProperties imageFormatProperties;
        auto createInfo = &formatConstraints->imageCreateInfo;
        auto result = enc->vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice, createInfo->format, createInfo->imageType, tiling,
            createInfo->usage, createInfo->flags, &imageFormatProperties,
            true /* do lock */);
        if (result != VK_SUCCESS) {
            ALOGD(
                "%s: Image format (%u) type (%u) tiling (%u) "
                "usage (%u) flags (%u) not supported by physical "
                "device",
                __func__, static_cast<uint32_t>(createInfo->format),
                static_cast<uint32_t>(createInfo->imageType),
                static_cast<uint32_t>(tiling),
                static_cast<uint32_t>(createInfo->usage),
                static_cast<uint32_t>(createInfo->flags));
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }

        // Check if format constraints contains unsupported format features.
        {
            VkFormatProperties formatProperties;
            enc->vkGetPhysicalDeviceFormatProperties(
                physicalDevice, createInfo->format, &formatProperties,
                true /* do lock */);

            auto supportedFeatures =
                (tiling == VK_IMAGE_TILING_LINEAR)
                    ? formatProperties.linearTilingFeatures
                    : formatProperties.optimalTilingFeatures;
            auto requiredFeatures = formatConstraints->requiredFormatFeatures;
            if ((~supportedFeatures) & requiredFeatures) {
                ALOGD(
                    "%s: Host device support features for %s tiling: %08x, "
                    "required features: %08x, feature bits %08x missing",
                    __func__,
                    tiling == VK_IMAGE_TILING_LINEAR ? "LINEAR" : "OPTIMAL",
                    static_cast<uint32_t>(requiredFeatures),
                    static_cast<uint32_t>(supportedFeatures),
                    static_cast<uint32_t>((~supportedFeatures) &
                                          requiredFeatures));
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
        }

        fuchsia_sysmem::wire::ImageFormatConstraints imageConstraints;
        if (formatConstraints->sysmemPixelFormat != 0) {
            auto pixelFormat =
                static_cast<fuchsia_sysmem::wire::PixelFormatType>(
                    formatConstraints->sysmemPixelFormat);
            if (createInfo->format != VK_FORMAT_UNDEFINED &&
                !vkFormatMatchesSysmemFormat(createInfo->format, pixelFormat)) {
                ALOGD("%s: VkFormat %u doesn't match sysmem pixelFormat %lu",
                      __func__, static_cast<uint32_t>(createInfo->format),
                      formatConstraints->sysmemPixelFormat);
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            imageConstraints.pixel_format.type = pixelFormat;
        } else {
            auto pixel_format = vkFormatTypeToSysmem(createInfo->format);
            if (pixel_format ==
                fuchsia_sysmem::wire::PixelFormatType::kInvalid) {
                ALOGD("%s: Unsupported VkFormat %u", __func__,
                      static_cast<uint32_t>(createInfo->format));
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            imageConstraints.pixel_format.type = pixel_format;
        }

        imageConstraints.color_spaces_count =
            formatConstraints->colorSpaceCount;
        for (size_t i = 0; i < formatConstraints->colorSpaceCount; i++) {
            imageConstraints.color_space[0].type =
                static_cast<fuchsia_sysmem::wire::ColorSpaceType>(
                    formatConstraints->pColorSpaces[i].colorSpace);
        }

        // Get row alignment from host GPU.
        VkDeviceSize offset = 0;
        VkDeviceSize rowPitchAlignment = 1u;

        if (tiling == VK_IMAGE_TILING_LINEAR) {
            VkImageCreateInfo createInfoDup = *createInfo;
            createInfoDup.pNext = nullptr;
            enc->vkGetLinearImageLayout2GOOGLE(device, &createInfoDup, &offset,
                                            &rowPitchAlignment,
                                            true /* do lock */);
            D("vkGetLinearImageLayout2GOOGLE: format %d offset %lu "
              "rowPitchAlignment = %lu",
              (int)createInfo->format, offset, rowPitchAlignment);
        }

        imageConstraints.min_coded_width = createInfo->extent.width;
        imageConstraints.max_coded_width = 0xfffffff;
        imageConstraints.min_coded_height = createInfo->extent.height;
        imageConstraints.max_coded_height = 0xffffffff;
        // The min_bytes_per_row can be calculated by sysmem using
        // |min_coded_width|, |bytes_per_row_divisor| and color format.
        imageConstraints.min_bytes_per_row = 0;
        imageConstraints.max_bytes_per_row = 0xffffffff;
        imageConstraints.max_coded_width_times_coded_height = 0xffffffff;

        imageConstraints.layers = 1;
        imageConstraints.coded_width_divisor = 1;
        imageConstraints.coded_height_divisor = 1;
        imageConstraints.bytes_per_row_divisor = rowPitchAlignment;
        imageConstraints.start_offset_divisor = 1;
        imageConstraints.display_width_divisor = 1;
        imageConstraints.display_height_divisor = 1;
        imageConstraints.pixel_format.has_format_modifier = true;
        imageConstraints.pixel_format.format_modifier.value =
            (tiling == VK_IMAGE_TILING_LINEAR)
                ? fuchsia_sysmem::wire::kFormatModifierLinear
                : fuchsia_sysmem::wire::kFormatModifierGoogleGoldfishOptimal;

        constraints->image_format_constraints
            [constraints->image_format_constraints_count++] = imageConstraints;
        return VK_SUCCESS;
    }

    struct SetBufferCollectionImageConstraintsResult {
        VkResult result;
        fuchsia_sysmem::wire::BufferCollectionConstraints constraints;
        std::vector<uint32_t> createInfoIndex;
    };

    SetBufferCollectionImageConstraintsResult
    setBufferCollectionImageConstraintsImpl(
        VkEncoder* enc,
        VkDevice device,
        fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>* pCollection,
        const VkImageConstraintsInfoFUCHSIA* pImageConstraintsInfo) {
        const auto& collection = *pCollection;
        if (!pImageConstraintsInfo ||
             pImageConstraintsInfo->sType !=
                 VK_STRUCTURE_TYPE_IMAGE_CONSTRAINTS_INFO_FUCHSIA) {
            ALOGE("%s: invalid pImageConstraintsInfo", __func__);
            return {VK_ERROR_INITIALIZATION_FAILED};
        }

        if (pImageConstraintsInfo->formatConstraintsCount == 0) {
            ALOGE("%s: formatConstraintsCount must be greater than 0",
                  __func__);
            abort();
        }

        fuchsia_sysmem::wire::BufferCollectionConstraints constraints =
            defaultBufferCollectionConstraints(
                /* min_size_bytes */ 0,
                pImageConstraintsInfo->bufferCollectionConstraints
                    .minBufferCount,
                pImageConstraintsInfo->bufferCollectionConstraints
                    .maxBufferCount,
                pImageConstraintsInfo->bufferCollectionConstraints
                    .minBufferCountForCamping,
                pImageConstraintsInfo->bufferCollectionConstraints
                    .minBufferCountForDedicatedSlack,
                pImageConstraintsInfo->bufferCollectionConstraints
                    .minBufferCountForSharedSlack);

        std::vector<fuchsia_sysmem::wire::ImageFormatConstraints>
            format_constraints;

        VkPhysicalDevice physicalDevice;
        {
            AutoLock<RecursiveLock> lock(mLock);
            auto deviceIt = info_VkDevice.find(device);
            if (deviceIt == info_VkDevice.end()) {
                return {VK_ERROR_INITIALIZATION_FAILED};
            }
            physicalDevice = deviceIt->second.physdev;
        }

        std::vector<uint32_t> createInfoIndex;

        bool hasOptimalTiling = false;
        for (uint32_t i = 0; i < pImageConstraintsInfo->formatConstraintsCount;
             i++) {
            const VkImageCreateInfo* createInfo =
                &pImageConstraintsInfo->pFormatConstraints[i].imageCreateInfo;
            const VkImageFormatConstraintsInfoFUCHSIA* formatConstraints =
                &pImageConstraintsInfo->pFormatConstraints[i];

            // add ImageFormatConstraints for *optimal* tiling
            VkResult optimalResult = VK_ERROR_FORMAT_NOT_SUPPORTED;
            if (createInfo->tiling == VK_IMAGE_TILING_OPTIMAL) {
                optimalResult = addImageBufferCollectionConstraintsFUCHSIA(
                    enc, device, physicalDevice, formatConstraints,
                    VK_IMAGE_TILING_OPTIMAL, &constraints);
                if (optimalResult == VK_SUCCESS) {
                    createInfoIndex.push_back(i);
                    hasOptimalTiling = true;
                }
            }

            // Add ImageFormatConstraints for *linear* tiling
            VkResult linearResult = addImageBufferCollectionConstraintsFUCHSIA(
                enc, device, physicalDevice, formatConstraints,
                VK_IMAGE_TILING_LINEAR, &constraints);
            if (linearResult == VK_SUCCESS) {
                createInfoIndex.push_back(i);
            }

            // Update usage and BufferMemoryConstraints
            if (linearResult == VK_SUCCESS || optimalResult == VK_SUCCESS) {
                constraints.usage.vulkan |=
                    getBufferCollectionConstraintsVulkanImageUsage(createInfo);

                if (formatConstraints && formatConstraints->flags) {
                    ALOGW(
                        "%s: Non-zero flags (%08x) in image format "
                        "constraints; this is currently not supported, see "
                        "fxbug.dev/68833.",
                        __func__, formatConstraints->flags);
                }
            }
        }

        // Set buffer memory constraints based on optimal/linear tiling support
        // and flags.
        VkImageConstraintsInfoFlagsFUCHSIA flags = pImageConstraintsInfo->flags;
        if (flags & VK_IMAGE_CONSTRAINTS_INFO_CPU_READ_RARELY_FUCHSIA)
            constraints.usage.cpu |= fuchsia_sysmem::wire::kCpuUsageRead;
        if (flags & VK_IMAGE_CONSTRAINTS_INFO_CPU_READ_OFTEN_FUCHSIA)
            constraints.usage.cpu |= fuchsia_sysmem::wire::kCpuUsageReadOften;
        if (flags & VK_IMAGE_CONSTRAINTS_INFO_CPU_WRITE_RARELY_FUCHSIA)
            constraints.usage.cpu |= fuchsia_sysmem::wire::kCpuUsageWrite;
        if (flags & VK_IMAGE_CONSTRAINTS_INFO_CPU_WRITE_OFTEN_FUCHSIA)
            constraints.usage.cpu |= fuchsia_sysmem::wire::kCpuUsageWriteOften;

        constraints.has_buffer_memory_constraints = true;
        auto& memory_constraints = constraints.buffer_memory_constraints;
        memory_constraints.cpu_domain_supported = true;
        memory_constraints.ram_domain_supported = true;
        memory_constraints.inaccessible_domain_supported =
            hasOptimalTiling &&
            !(flags & (VK_IMAGE_CONSTRAINTS_INFO_CPU_READ_RARELY_FUCHSIA |
                       VK_IMAGE_CONSTRAINTS_INFO_CPU_READ_OFTEN_FUCHSIA |
                       VK_IMAGE_CONSTRAINTS_INFO_CPU_WRITE_RARELY_FUCHSIA |
                       VK_IMAGE_CONSTRAINTS_INFO_CPU_WRITE_OFTEN_FUCHSIA));

        if (memory_constraints.inaccessible_domain_supported) {
            memory_constraints.heap_permitted_count = 2;
            memory_constraints.heap_permitted[0] =
                fuchsia_sysmem::wire::HeapType::kGoldfishDeviceLocal;
            memory_constraints.heap_permitted[1] =
                fuchsia_sysmem::wire::HeapType::kGoldfishHostVisible;
        } else {
            memory_constraints.heap_permitted_count = 1;
            memory_constraints.heap_permitted[0] =
                fuchsia_sysmem::wire::HeapType::kGoldfishHostVisible;
        }

        if (constraints.image_format_constraints_count == 0) {
            ALOGE("%s: none of the specified formats is supported by device",
                  __func__);
            return {VK_ERROR_FORMAT_NOT_SUPPORTED};
        }

        constexpr uint32_t kVulkanPriority = 5;
        const char kName[] = "GoldfishSysmemShared";
        collection->SetName(kVulkanPriority, fidl::StringView(kName));

        auto result = collection->SetConstraints(true, constraints);
        if (!result.ok()) {
            ALOGE("setBufferCollectionConstraints: SetConstraints failed: %d",
                  result.status());
            return {VK_ERROR_INITIALIZATION_FAILED};
        }

        return {VK_SUCCESS, constraints, std::move(createInfoIndex)};
    }

    VkResult setBufferCollectionImageConstraintsFUCHSIA(
        VkEncoder* enc,
        VkDevice device,
        fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>* pCollection,
        const VkImageConstraintsInfoFUCHSIA* pImageConstraintsInfo) {
        const auto& collection = *pCollection;

        auto setConstraintsResult = setBufferCollectionImageConstraintsImpl(
            enc, device, pCollection, pImageConstraintsInfo);
        if (setConstraintsResult.result != VK_SUCCESS) {
            return setConstraintsResult.result;
        }

        // copy constraints to info_VkBufferCollectionFUCHSIA if
        // |collection| is a valid VkBufferCollectionFUCHSIA handle.
        AutoLock<RecursiveLock> lock(mLock);
        VkBufferCollectionFUCHSIA buffer_collection =
            reinterpret_cast<VkBufferCollectionFUCHSIA>(pCollection);
        if (info_VkBufferCollectionFUCHSIA.find(buffer_collection) !=
            info_VkBufferCollectionFUCHSIA.end()) {
            info_VkBufferCollectionFUCHSIA[buffer_collection].constraints =
                android::base::makeOptional(
                    std::move(setConstraintsResult.constraints));
            info_VkBufferCollectionFUCHSIA[buffer_collection].createInfoIndex =
                std::move(setConstraintsResult.createInfoIndex);
        }

        return VK_SUCCESS;
    }

    struct SetBufferCollectionBufferConstraintsResult {
        VkResult result;
        fuchsia_sysmem::wire::BufferCollectionConstraints constraints;
    };

    SetBufferCollectionBufferConstraintsResult
    setBufferCollectionBufferConstraintsImpl(
        fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>* pCollection,
        const VkBufferConstraintsInfoFUCHSIA* pBufferConstraintsInfo) {
        const auto& collection = *pCollection;
        if (pBufferConstraintsInfo == nullptr) {
            ALOGE(
                "setBufferCollectionBufferConstraints: "
                "pBufferConstraintsInfo cannot be null.");
            return {VK_ERROR_OUT_OF_DEVICE_MEMORY};
        }

        fuchsia_sysmem::wire::BufferCollectionConstraints constraints =
            defaultBufferCollectionConstraints(
                /* min_size_bytes */ pBufferConstraintsInfo->createInfo.size,
                /* buffer_count */ pBufferConstraintsInfo
                    ->bufferCollectionConstraints.minBufferCount);
        constraints.usage.vulkan =
            getBufferCollectionConstraintsVulkanBufferUsage(
                pBufferConstraintsInfo);

        constexpr uint32_t kVulkanPriority = 5;
        const char kName[] = "GoldfishBufferSysmemShared";
        collection->SetName(kVulkanPriority, fidl::StringView(kName));

        auto result = collection->SetConstraints(true, constraints);
        if (!result.ok()) {
            ALOGE("setBufferCollectionConstraints: SetConstraints failed: %d",
                  result.status());
            return {VK_ERROR_OUT_OF_DEVICE_MEMORY};
        }

        return {VK_SUCCESS, constraints};
    }

    VkResult setBufferCollectionBufferConstraintsFUCHSIA(
        fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>* pCollection,
        const VkBufferConstraintsInfoFUCHSIA* pBufferConstraintsInfo) {
        auto setConstraintsResult = setBufferCollectionBufferConstraintsImpl(
            pCollection, pBufferConstraintsInfo);
        if (setConstraintsResult.result != VK_SUCCESS) {
            return setConstraintsResult.result;
        }

        // copy constraints to info_VkBufferCollectionFUCHSIA if
        // |collection| is a valid VkBufferCollectionFUCHSIA handle.
        AutoLock<RecursiveLock> lock(mLock);
        VkBufferCollectionFUCHSIA buffer_collection =
            reinterpret_cast<VkBufferCollectionFUCHSIA>(pCollection);
        if (info_VkBufferCollectionFUCHSIA.find(buffer_collection) !=
            info_VkBufferCollectionFUCHSIA.end()) {
            info_VkBufferCollectionFUCHSIA[buffer_collection].constraints =
                android::base::makeOptional(setConstraintsResult.constraints);
        }

        return VK_SUCCESS;
    }

    VkResult on_vkSetBufferCollectionImageConstraintsFUCHSIA(
        void* context,
        VkResult,
        VkDevice device,
        VkBufferCollectionFUCHSIA collection,
        const VkImageConstraintsInfoFUCHSIA* pImageConstraintsInfo) {
        VkEncoder* enc = (VkEncoder*)context;
        auto sysmem_collection = reinterpret_cast<
            fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>*>(
            collection);
        return setBufferCollectionImageConstraintsFUCHSIA(
            enc, device, sysmem_collection, pImageConstraintsInfo);
    }

    VkResult on_vkSetBufferCollectionBufferConstraintsFUCHSIA(
        void*,
        VkResult,
        VkDevice,
        VkBufferCollectionFUCHSIA collection,
        const VkBufferConstraintsInfoFUCHSIA* pBufferConstraintsInfo) {
        auto sysmem_collection = reinterpret_cast<
            fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>*>(
            collection);
        return setBufferCollectionBufferConstraintsFUCHSIA(
            sysmem_collection, pBufferConstraintsInfo);
    }

    VkResult getBufferCollectionImageCreateInfoIndexLocked(
        VkBufferCollectionFUCHSIA collection,
        fuchsia_sysmem::wire::BufferCollectionInfo2& info,
        uint32_t* outCreateInfoIndex) {
        if (!info_VkBufferCollectionFUCHSIA[collection]
                 .constraints.hasValue()) {
            ALOGE("%s: constraints not set", __func__);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }

        if (!info.settings.has_image_format_constraints) {
            // no image format constraints, skip getting createInfoIndex.
            return VK_SUCCESS;
        }

        const auto& constraints =
            *info_VkBufferCollectionFUCHSIA[collection].constraints;
        const auto& createInfoIndices =
            info_VkBufferCollectionFUCHSIA[collection].createInfoIndex;
        const auto& out = info.settings.image_format_constraints;
        bool foundCreateInfo = false;

        for (size_t imageFormatIndex = 0;
             imageFormatIndex < constraints.image_format_constraints_count;
             imageFormatIndex++) {
            const auto& in =
                constraints.image_format_constraints[imageFormatIndex];
            // These checks are sorted in order of how often they're expected to
            // mismatch, from most likely to least likely. They aren't always
            // equality comparisons, since sysmem may change some values in
            // compatible ways on behalf of the other participants.
            if ((out.pixel_format.type != in.pixel_format.type) ||
                (out.pixel_format.has_format_modifier !=
                 in.pixel_format.has_format_modifier) ||
                (out.pixel_format.format_modifier.value !=
                 in.pixel_format.format_modifier.value) ||
                (out.min_bytes_per_row < in.min_bytes_per_row) ||
                (out.required_max_coded_width < in.required_max_coded_width) ||
                (out.required_max_coded_height <
                 in.required_max_coded_height) ||
                (in.bytes_per_row_divisor != 0 &&
                 out.bytes_per_row_divisor % in.bytes_per_row_divisor != 0)) {
                continue;
            }
            // Check if the out colorspaces are a subset of the in color spaces.
            bool all_color_spaces_found = true;
            for (uint32_t j = 0; j < out.color_spaces_count; j++) {
                bool found_matching_color_space = false;
                for (uint32_t k = 0; k < in.color_spaces_count; k++) {
                    if (out.color_space[j].type == in.color_space[k].type) {
                        found_matching_color_space = true;
                        break;
                    }
                }
                if (!found_matching_color_space) {
                    all_color_spaces_found = false;
                    break;
                }
            }
            if (!all_color_spaces_found) {
                continue;
            }

            // Choose the first valid format for now.
            *outCreateInfoIndex = createInfoIndices[imageFormatIndex];
            return VK_SUCCESS;
        }

        ALOGE("%s: cannot find a valid image format in constraints", __func__);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    VkResult on_vkGetBufferCollectionPropertiesFUCHSIA(
        void* context,
        VkResult,
        VkDevice device,
        VkBufferCollectionFUCHSIA collection,
        VkBufferCollectionPropertiesFUCHSIA* pProperties) {
        VkEncoder* enc = (VkEncoder*)context;
        const auto& sysmem_collection = *reinterpret_cast<
            fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>*>(
            collection);

        auto result = sysmem_collection->WaitForBuffersAllocated();
        if (!result.ok() || result->status != ZX_OK) {
            ALOGE("Failed wait for allocation: %d %d", result.status(),
                  GET_STATUS_SAFE(result, status));
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        fuchsia_sysmem::wire::BufferCollectionInfo2 info =
            std::move(result->buffer_collection_info);

        bool is_host_visible =
            info.settings.buffer_settings.heap ==
            fuchsia_sysmem::wire::HeapType::kGoldfishHostVisible;
        bool is_device_local =
            info.settings.buffer_settings.heap ==
            fuchsia_sysmem::wire::HeapType::kGoldfishDeviceLocal;
        if (!is_host_visible && !is_device_local) {
            ALOGE("buffer collection uses a non-goldfish heap (type 0x%lu)",
                  static_cast<uint64_t>(info.settings.buffer_settings.heap));
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // memoryTypeBits
        // ====================================================================
        {
            AutoLock<RecursiveLock> lock(mLock);
            auto deviceIt = info_VkDevice.find(device);
            if (deviceIt == info_VkDevice.end()) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            auto& deviceInfo = deviceIt->second;

            // Device local memory type supported.
            pProperties->memoryTypeBits = 0;
            for (uint32_t i = 0; i < deviceInfo.memProps.memoryTypeCount; ++i) {
                if ((is_device_local &&
                     (deviceInfo.memProps.memoryTypes[i].propertyFlags &
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ||
                    (is_host_visible &&
                     (deviceInfo.memProps.memoryTypes[i].propertyFlags &
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) {
                    pProperties->memoryTypeBits |= 1ull << i;
                }
            }
        }

        // bufferCount
        // ====================================================================
        pProperties->bufferCount = info.buffer_count;

        auto storeProperties = [this, collection, pProperties]() -> VkResult {
            // store properties to storage
            AutoLock<RecursiveLock> lock(mLock);
            if (info_VkBufferCollectionFUCHSIA.find(collection) ==
                info_VkBufferCollectionFUCHSIA.end()) {
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }

            info_VkBufferCollectionFUCHSIA[collection].properties =
                android::base::makeOptional(*pProperties);

            // We only do a shallow copy so we should remove all pNext pointers.
            info_VkBufferCollectionFUCHSIA[collection].properties->pNext =
                nullptr;
            info_VkBufferCollectionFUCHSIA[collection]
                .properties->sysmemColorSpaceIndex.pNext = nullptr;
            return VK_SUCCESS;
        };

        // The fields below only apply to buffer collections with image formats.
        if (!info.settings.has_image_format_constraints) {
            ALOGD("%s: buffer collection doesn't have image format constraints",
                  __func__);
            return storeProperties();
        }

        // sysmemFormat
        // ====================================================================

        pProperties->sysmemPixelFormat = static_cast<uint64_t>(
            info.settings.image_format_constraints.pixel_format.type);

        // colorSpace
        // ====================================================================
        if (info.settings.image_format_constraints.color_spaces_count == 0) {
            ALOGE(
                "%s: color space missing from allocated buffer collection "
                "constraints",
                __func__);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        // Only report first colorspace for now.
        pProperties->sysmemColorSpaceIndex.colorSpace = static_cast<uint32_t>(
            info.settings.image_format_constraints.color_space[0].type);

        // createInfoIndex
        // ====================================================================
        {
            AutoLock<RecursiveLock> lock(mLock);
            auto getIndexResult = getBufferCollectionImageCreateInfoIndexLocked(
                collection, info, &pProperties->createInfoIndex);
            if (getIndexResult != VK_SUCCESS) {
                return getIndexResult;
            }
        }

        // formatFeatures
        // ====================================================================
        VkPhysicalDevice physicalDevice;
        {
            AutoLock<RecursiveLock> lock(mLock);
            auto deviceIt = info_VkDevice.find(device);
            if (deviceIt == info_VkDevice.end()) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            physicalDevice = deviceIt->second.physdev;
        }

        VkFormat vkFormat = sysmemPixelFormatTypeToVk(
            info.settings.image_format_constraints.pixel_format.type);
        VkFormatProperties formatProperties;
        enc->vkGetPhysicalDeviceFormatProperties(
            physicalDevice, vkFormat, &formatProperties, true /* do lock */);
        if (is_device_local) {
            pProperties->formatFeatures =
                formatProperties.optimalTilingFeatures;
        }
        if (is_host_visible) {
            pProperties->formatFeatures = formatProperties.linearTilingFeatures;
        }

        // YCbCr properties
        // ====================================================================
        // TODO(59804): Implement this correctly when we support YUV pixel
        // formats in goldfish ICD.
        pProperties->samplerYcbcrConversionComponents.r =
            VK_COMPONENT_SWIZZLE_IDENTITY;
        pProperties->samplerYcbcrConversionComponents.g =
            VK_COMPONENT_SWIZZLE_IDENTITY;
        pProperties->samplerYcbcrConversionComponents.b =
            VK_COMPONENT_SWIZZLE_IDENTITY;
        pProperties->samplerYcbcrConversionComponents.a =
            VK_COMPONENT_SWIZZLE_IDENTITY;
        pProperties->suggestedYcbcrModel =
            VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
        pProperties->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
        pProperties->suggestedXChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        pProperties->suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;

        return storeProperties();
    }
#endif

    CoherentMemoryPtr createCoherentMemory(VkDevice device,
                                           VkDeviceMemory mem,
                                           const VkMemoryAllocateInfo& hostAllocationInfo,
                                           VkEncoder* enc,
                                           VkResult& res)
    {
        CoherentMemoryPtr coherentMemory = nullptr;
        if (mFeatureInfo->hasDirectMem) {
            uint64_t gpuAddr = 0;
            GoldfishAddressSpaceBlockPtr block = nullptr;
            res = enc->vkMapMemoryIntoAddressSpaceGOOGLE(device, mem, &gpuAddr, true);
            if (res != VK_SUCCESS) {
                return coherentMemory;
            }
            {
                AutoLock<RecursiveLock> lock(mLock);
                auto it = info_VkDeviceMemory.find(mem);
                if (it == info_VkDeviceMemory.end()) {
                     res = VK_ERROR_OUT_OF_HOST_MEMORY;
                     return coherentMemory;
                }
                auto& info = it->second;
                block = info.goldfishBlock;
                info.goldfishBlock = nullptr;

                coherentMemory =
                    std::make_shared<CoherentMemory>(block, gpuAddr, hostAllocationInfo.allocationSize, device, mem);
            }
        } else if (mFeatureInfo->hasVirtioGpuNext) {
            struct VirtGpuCreateBlob createBlob = { 0 };
            uint64_t hvaSizeId[3];
            res = enc->vkGetMemoryHostAddressInfoGOOGLE(device, mem,
                    &hvaSizeId[0], &hvaSizeId[1], &hvaSizeId[2], true /* do lock */);
            if(res != VK_SUCCESS) {
                return coherentMemory;
            }
            {
                AutoLock<RecursiveLock> lock(mLock);
                VirtGpuDevice& instance = VirtGpuDevice::getInstance((enum VirtGpuCapset)3);
                createBlob.blobMem = kBlobMemHost3d;
                createBlob.flags = kBlobFlagMappable;
                createBlob.blobId = hvaSizeId[2];
                createBlob.size = hostAllocationInfo.allocationSize;

                auto blob = instance.createBlob(createBlob);
                if (!blob) {
                    res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                    return coherentMemory;
                }

                VirtGpuBlobMappingPtr mapping = blob->createMapping();
                if (!mapping) {
                    res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                    return coherentMemory;
                }

                coherentMemory =
                    std::make_shared<CoherentMemory>(mapping, createBlob.size, device, mem);
            }
        } else {
            ALOGE("FATAL: Unsupported virtual memory feature");
            abort();
        }
        return coherentMemory;
    }

    VkResult allocateCoherentMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
                                    VkEncoder* enc, VkDeviceMemory* pMemory) {
        uint64_t blobId = 0;
        uint64_t offset = 0;
        uint8_t *ptr = nullptr;
        VkMemoryAllocateFlagsInfo allocFlagsInfo;
        VkMemoryOpaqueCaptureAddressAllocateInfo opaqueCaptureAddressAllocInfo;
        VkCreateBlobGOOGLE createBlobInfo;
        VirtGpuBlobPtr guestBlob = nullptr;

        memset(&createBlobInfo, 0, sizeof(struct VkCreateBlobGOOGLE));
        createBlobInfo.sType = VK_STRUCTURE_TYPE_CREATE_BLOB_GOOGLE;

        const VkMemoryAllocateFlagsInfo* allocFlagsInfoPtr =
            vk_find_struct<VkMemoryAllocateFlagsInfo>(pAllocateInfo);
        const VkMemoryOpaqueCaptureAddressAllocateInfo* opaqueCaptureAddressAllocInfoPtr =
            vk_find_struct<VkMemoryOpaqueCaptureAddressAllocateInfo>(pAllocateInfo);

        bool deviceAddressMemoryAllocation =
            allocFlagsInfoPtr &&
            ((allocFlagsInfoPtr->flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) ||
             (allocFlagsInfoPtr->flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT));

        bool dedicated = deviceAddressMemoryAllocation;

        if (mCaps.gfxstreamCapset.deferredMapping || mCaps.params[kParamCreateGuestHandle])
            dedicated = true;

        VkMemoryAllocateInfo hostAllocationInfo = vk_make_orphan_copy(*pAllocateInfo);
        vk_struct_chain_iterator structChainIter = vk_make_chain_iterator(&hostAllocationInfo);

        if (mCaps.gfxstreamCapset.deferredMapping || mCaps.params[kParamCreateGuestHandle]) {
            hostAllocationInfo.allocationSize = ALIGN(pAllocateInfo->allocationSize, 4096);
        } else if (dedicated) {
            // Over-aligning to kLargestSize to some Windows drivers (b:152769369).  Can likely
            // have host report the desired alignment.
            hostAllocationInfo.allocationSize =
                ALIGN(pAllocateInfo->allocationSize, kLargestPageSize);
        } else {
            VkDeviceSize roundedUpAllocSize = ALIGN(pAllocateInfo->allocationSize, kMegaByte);
            hostAllocationInfo.allocationSize = std::max(roundedUpAllocSize,
                                                         kDefaultHostMemBlockSize);
        }

        // Support device address capture/replay allocations
        if (deviceAddressMemoryAllocation) {
            if (allocFlagsInfoPtr) {
                ALOGV("%s: has alloc flags\n", __func__);
                allocFlagsInfo = *allocFlagsInfoPtr;
                vk_append_struct(&structChainIter, &allocFlagsInfo);
            }

            if (opaqueCaptureAddressAllocInfoPtr) {
                ALOGV("%s: has opaque capture address\n", __func__);
                opaqueCaptureAddressAllocInfo = *opaqueCaptureAddressAllocInfoPtr;
                vk_append_struct(&structChainIter, &opaqueCaptureAddressAllocInfo);
            }
        }

        if (mCaps.params[kParamCreateGuestHandle]) {
            struct VirtGpuCreateBlob createBlob = {0};
            struct VirtGpuExecBuffer exec = {};
            VirtGpuDevice& instance = VirtGpuDevice::getInstance();
            struct gfxstreamPlaceholderCommandVk placeholderCmd = {};

            createBlobInfo.blobId = ++mBlobId;
            createBlobInfo.blobMem = kBlobMemGuest;
            createBlobInfo.blobFlags = kBlobFlagCreateGuestHandle;
            vk_append_struct(&structChainIter, &createBlobInfo);

            createBlob.blobMem = kBlobMemGuest;
            createBlob.flags = kBlobFlagCreateGuestHandle;
            createBlob.blobId = createBlobInfo.blobId;
            createBlob.size = hostAllocationInfo.allocationSize;

            guestBlob = instance.createBlob(createBlob);
            if (!guestBlob) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

            placeholderCmd.hdr.opCode = GFXSTREAM_PLACEHOLDER_COMMAND_VK;
            exec.command = static_cast<void*>(&placeholderCmd);
            exec.command_size = sizeof(placeholderCmd);
            exec.flags = kRingIdx;
            exec.ring_idx = 1;
            if (instance.execBuffer(exec, guestBlob)) return VK_ERROR_OUT_OF_HOST_MEMORY;

            guestBlob->wait();
        } else if (mCaps.gfxstreamCapset.deferredMapping) {
            createBlobInfo.blobId = ++mBlobId;
            createBlobInfo.blobMem = kBlobMemHost3d;
            vk_append_struct(&structChainIter, &createBlobInfo);
        }

        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkResult host_res =
        enc->vkAllocateMemory(device, &hostAllocationInfo, nullptr,
                              &mem, true /* do lock */);
        if(host_res != VK_SUCCESS) {
            return host_res;
        }

        struct VkDeviceMemory_Info info;
        if (mCaps.gfxstreamCapset.deferredMapping || mCaps.params[kParamCreateGuestHandle]) {
            info.allocationSize = pAllocateInfo->allocationSize;
            info.blobId = createBlobInfo.blobId;
        }

        if (guestBlob) {
            auto mapping = guestBlob->createMapping();
            if (!mapping) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

            auto coherentMemory = std::make_shared<CoherentMemory>(
                mapping, hostAllocationInfo.allocationSize, device, mem);

            coherentMemory->subAllocate(pAllocateInfo->allocationSize, &ptr, offset);
            info.coherentMemoryOffset = offset;
            info.coherentMemory = coherentMemory;
            info.ptr = ptr;
        }

        info.coherentMemorySize = hostAllocationInfo.allocationSize;
        info.memoryTypeIndex = hostAllocationInfo.memoryTypeIndex;
        info.device = device;
        info.dedicated = dedicated;
        {
            // createCoherentMemory inside need to access info_VkDeviceMemory
            // information. set it before use.
            AutoLock<RecursiveLock> lock(mLock);
            info_VkDeviceMemory[mem] = info;
        }

        if (mCaps.gfxstreamCapset.deferredMapping || mCaps.params[kParamCreateGuestHandle]) {
            *pMemory = mem;
            return host_res;
        }

        auto coherentMemory = createCoherentMemory(device, mem, hostAllocationInfo, enc, host_res);
        if(coherentMemory) {
            AutoLock<RecursiveLock> lock(mLock);
            coherentMemory->subAllocate(pAllocateInfo->allocationSize, &ptr, offset);
            info.allocationSize = pAllocateInfo->allocationSize;
            info.coherentMemoryOffset = offset;
            info.coherentMemory = coherentMemory;
            info.ptr = ptr;
            info_VkDeviceMemory[mem] = info;
            *pMemory = mem;
        }
        else {
            enc->vkFreeMemory(device, mem, nullptr, true);
            AutoLock<RecursiveLock> lock(mLock);
            info_VkDeviceMemory.erase(mem);
        }
        return host_res;
    }

    VkResult getCoherentMemory(const VkMemoryAllocateInfo* pAllocateInfo, VkEncoder* enc,
                               VkDevice device, VkDeviceMemory* pMemory) {
        VkMemoryAllocateFlagsInfo allocFlagsInfo;
        VkMemoryOpaqueCaptureAddressAllocateInfo opaqueCaptureAddressAllocInfo;

        // Add buffer device address capture structs
        const VkMemoryAllocateFlagsInfo* allocFlagsInfoPtr =
            vk_find_struct<VkMemoryAllocateFlagsInfo>(pAllocateInfo);

        bool dedicated = allocFlagsInfoPtr &&
                         ((allocFlagsInfoPtr->flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) ||
                          (allocFlagsInfoPtr->flags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT));

        if (mCaps.gfxstreamCapset.deferredMapping || mCaps.params[kParamCreateGuestHandle])
            dedicated = true;

        CoherentMemoryPtr coherentMemory = nullptr;
        uint8_t *ptr = nullptr;
        uint64_t offset = 0;
        {
            AutoLock<RecursiveLock> lock(mLock);
            for (const auto &[memory, info] : info_VkDeviceMemory) {
                if (info.memoryTypeIndex != pAllocateInfo->memoryTypeIndex)
                    continue;

                if (info.dedicated || dedicated)
                    continue;

                if (!info.coherentMemory)
                    continue;

                if (!info.coherentMemory->subAllocate(pAllocateInfo->allocationSize, &ptr, offset))
                    continue;

                coherentMemory = info.coherentMemory;
                break;
            }
            if (coherentMemory) {
                struct VkDeviceMemory_Info info;
                info.coherentMemoryOffset = offset;
                info.ptr = ptr;
                info.memoryTypeIndex = pAllocateInfo->memoryTypeIndex;
                info.allocationSize = pAllocateInfo->allocationSize;
                info.coherentMemory = coherentMemory;
                info.device = device;

                // for suballocated memory, create an alias VkDeviceMemory handle for application
                // memory used for suballocations will still be VkDeviceMemory associated with
                // CoherentMemory
                auto mem = new_from_host_VkDeviceMemory(VK_NULL_HANDLE);
                info_VkDeviceMemory[mem] = info;
                *pMemory = mem;
                return VK_SUCCESS;
            }
        }
        return allocateCoherentMemory(device, pAllocateInfo, enc, pMemory);
    }

    uint64_t getAHardwareBufferId(AHardwareBuffer* ahw) {
        uint64_t id = 0;
#if defined(PLATFORM_SDK_VERSION) && PLATFORM_SDK_VERSION >= 31
        AHardwareBuffer_getId(ahw, &id);
#else
        (void)ahw;
#endif
        return id;
    }

    VkResult on_vkAllocateMemory(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkMemoryAllocateInfo* pAllocateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDeviceMemory* pMemory) {

#define _RETURN_FAILURE_WITH_DEVICE_MEMORY_REPORT(result) \
        { \
            auto it = info_VkDevice.find(device); \
            if (it == info_VkDevice.end()) return result; \
            emitDeviceMemoryReport( \
                it->second, \
                VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATION_FAILED_EXT, \
                0, \
                pAllocateInfo->allocationSize, \
                VK_OBJECT_TYPE_DEVICE_MEMORY, \
                0, \
                pAllocateInfo->memoryTypeIndex); \
            return result; \
        }

#define _RETURN_SCUCCESS_WITH_DEVICE_MEMORY_REPORT \
        { \
            uint64_t memoryObjectId = (uint64_t)(void*)*pMemory; \
            if (ahw) { \
                memoryObjectId = getAHardwareBufferId(ahw); \
            } \
            emitDeviceMemoryReport( \
                info_VkDevice[device], \
                isImport ? VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT : VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT, \
                memoryObjectId, \
                pAllocateInfo->allocationSize, \
                VK_OBJECT_TYPE_DEVICE_MEMORY, \
                (uint64_t)(void*)*pMemory, \
                pAllocateInfo->memoryTypeIndex); \
            return VK_SUCCESS; \
        }


        if (input_result != VK_SUCCESS) _RETURN_FAILURE_WITH_DEVICE_MEMORY_REPORT(input_result);

        VkEncoder* enc = (VkEncoder*)context;

        VkMemoryAllocateInfo finalAllocInfo = vk_make_orphan_copy(*pAllocateInfo);
        vk_struct_chain_iterator structChainIter = vk_make_chain_iterator(&finalAllocInfo);

        VkMemoryAllocateFlagsInfo allocFlagsInfo;
        VkMemoryOpaqueCaptureAddressAllocateInfo opaqueCaptureAddressAllocInfo;

        // Add buffer device address capture structs
        const VkMemoryAllocateFlagsInfo* allocFlagsInfoPtr =
            vk_find_struct<VkMemoryAllocateFlagsInfo>(pAllocateInfo);
        const VkMemoryOpaqueCaptureAddressAllocateInfo* opaqueCaptureAddressAllocInfoPtr =
            vk_find_struct<VkMemoryOpaqueCaptureAddressAllocateInfo>(pAllocateInfo);

        if (allocFlagsInfoPtr) {
            ALOGV("%s: has alloc flags\n", __func__);
            allocFlagsInfo = *allocFlagsInfoPtr;
            vk_append_struct(&structChainIter, &allocFlagsInfo);
        }

        if (opaqueCaptureAddressAllocInfoPtr) {
            ALOGV("%s: has opaque capture address\n", __func__);
            opaqueCaptureAddressAllocInfo = *opaqueCaptureAddressAllocInfoPtr;
            vk_append_struct(&structChainIter, &opaqueCaptureAddressAllocInfo);
        }

        VkMemoryDedicatedAllocateInfo dedicatedAllocInfo;
        VkImportColorBufferGOOGLE importCbInfo = {
            VK_STRUCTURE_TYPE_IMPORT_COLOR_BUFFER_GOOGLE, 0,
        };
        VkImportBufferGOOGLE importBufferInfo = {
                VK_STRUCTURE_TYPE_IMPORT_BUFFER_GOOGLE,
                0,
        };
        // VkImportPhysicalAddressGOOGLE importPhysAddrInfo = {
        //     VK_STRUCTURE_TYPE_IMPORT_PHYSICAL_ADDRESS_GOOGLE, 0,
        // };

        const VkExportMemoryAllocateInfo* exportAllocateInfoPtr =
            vk_find_struct<VkExportMemoryAllocateInfo>(pAllocateInfo);

#ifdef VK_USE_PLATFORM_ANDROID_KHR
        const VkImportAndroidHardwareBufferInfoANDROID* importAhbInfoPtr =
            vk_find_struct<VkImportAndroidHardwareBufferInfoANDROID>(pAllocateInfo);
#else
        const void* importAhbInfoPtr = nullptr;
#endif

#ifdef VK_USE_PLATFORM_FUCHSIA
        const VkImportMemoryBufferCollectionFUCHSIA*
            importBufferCollectionInfoPtr =
                vk_find_struct<VkImportMemoryBufferCollectionFUCHSIA>(
                    pAllocateInfo);

        const VkImportMemoryZirconHandleInfoFUCHSIA* importVmoInfoPtr =
                vk_find_struct<VkImportMemoryZirconHandleInfoFUCHSIA>(
                        pAllocateInfo);
#else
        const void* importBufferCollectionInfoPtr = nullptr;
        const void* importVmoInfoPtr = nullptr;
#endif  // VK_USE_PLATFORM_FUCHSIA

        const VkMemoryDedicatedAllocateInfo* dedicatedAllocInfoPtr =
            vk_find_struct<VkMemoryDedicatedAllocateInfo>(pAllocateInfo);

        // Note for AHardwareBuffers, the Vulkan spec states:
        //
        //     Android hardware buffers have intrinsic width, height, format, and usage
        //     properties, so Vulkan images bound to memory imported from an Android
        //     hardware buffer must use dedicated allocations
        //
        // so any allocation requests with a VkImportAndroidHardwareBufferInfoANDROID
        // will necessarily have a VkMemoryDedicatedAllocateInfo. However, the host
        // may or may not actually use a dedicated allocation to emulate
        // AHardwareBuffers. As such, the VkMemoryDedicatedAllocateInfo is passed to the
        // host and the host will decide whether or not to use it.

        bool shouldPassThroughDedicatedAllocInfo =
            !exportAllocateInfoPtr &&
            !importBufferCollectionInfoPtr &&
            !importVmoInfoPtr;

        const VkPhysicalDeviceMemoryProperties& physicalDeviceMemoryProps
            = getPhysicalDeviceMemoryProperties(context, device, VK_NULL_HANDLE);

        const bool requestedMemoryIsHostVisible =
            isHostVisible(&physicalDeviceMemoryProps, pAllocateInfo->memoryTypeIndex);

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        shouldPassThroughDedicatedAllocInfo &= !requestedMemoryIsHostVisible;
#endif  // VK_USE_PLATFORM_FUCHSIA

        if (shouldPassThroughDedicatedAllocInfo &&
            dedicatedAllocInfoPtr) {
            dedicatedAllocInfo = vk_make_orphan_copy(*dedicatedAllocInfoPtr);
            vk_append_struct(&structChainIter, &dedicatedAllocInfo);
        }

        // State needed for import/export.
        bool exportAhb = false;
        bool exportVmo = false;
        bool importAhb = false;
        bool importBufferCollection = false;
        bool importVmo = false;
        (void)exportVmo;

        // Even if we export allocate, the underlying operation
        // for the host is always going to be an import operation.
        // This is also how Intel's implementation works,
        // and is generally simpler;
        // even in an export allocation,
        // we perform AHardwareBuffer allocation
        // on the guest side, at this layer,
        // and then we attach a new VkDeviceMemory
        // to the AHardwareBuffer on the host via an "import" operation.
        AHardwareBuffer* ahw = nullptr;

        if (exportAllocateInfoPtr) {
            exportAhb =
                exportAllocateInfoPtr->handleTypes &
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
#ifdef VK_USE_PLATFORM_FUCHSIA
            exportVmo = exportAllocateInfoPtr->handleTypes &
                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ZIRCON_VMO_BIT_FUCHSIA;
#endif  // VK_USE_PLATFORM_FUCHSIA
        } else if (importAhbInfoPtr) {
            importAhb = true;
        } else if (importBufferCollectionInfoPtr) {
            importBufferCollection = true;
        } else if (importVmoInfoPtr) {
            importVmo = true;
        }
        bool isImport = importAhb || importBufferCollection ||
                        importVmo;

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        if (exportAhb) {
            bool hasDedicatedImage = dedicatedAllocInfoPtr &&
                (dedicatedAllocInfoPtr->image != VK_NULL_HANDLE);
            bool hasDedicatedBuffer = dedicatedAllocInfoPtr &&
                (dedicatedAllocInfoPtr->buffer != VK_NULL_HANDLE);
            VkExtent3D imageExtent = { 0, 0, 0 };
            uint32_t imageLayers = 0;
            VkFormat imageFormat = VK_FORMAT_UNDEFINED;
            VkImageUsageFlags imageUsage = 0;
            VkImageCreateFlags imageCreateFlags = 0;
            VkDeviceSize bufferSize = 0;
            VkDeviceSize allocationInfoAllocSize =
                finalAllocInfo.allocationSize;

            if (hasDedicatedImage) {
                AutoLock<RecursiveLock> lock(mLock);

                auto it = info_VkImage.find(
                    dedicatedAllocInfoPtr->image);
                if (it == info_VkImage.end()) _RETURN_FAILURE_WITH_DEVICE_MEMORY_REPORT(VK_ERROR_INITIALIZATION_FAILED);
                const auto& info = it->second;
                const auto& imgCi = info.createInfo;

                imageExtent = imgCi.extent;
                imageLayers = imgCi.arrayLayers;
                imageFormat = imgCi.format;
                imageUsage = imgCi.usage;
                imageCreateFlags = imgCi.flags;
            }

            if (hasDedicatedBuffer) {
                AutoLock<RecursiveLock> lock(mLock);

                auto it = info_VkBuffer.find(
                    dedicatedAllocInfoPtr->buffer);
                if (it == info_VkBuffer.end()) _RETURN_FAILURE_WITH_DEVICE_MEMORY_REPORT(VK_ERROR_INITIALIZATION_FAILED);
                const auto& info = it->second;
                const auto& bufCi = info.createInfo;

                bufferSize = bufCi.size;
            }

            VkResult ahbCreateRes =
                createAndroidHardwareBuffer(
                    hasDedicatedImage,
                    hasDedicatedBuffer,
                    imageExtent,
                    imageLayers,
                    imageFormat,
                    imageUsage,
                    imageCreateFlags,
                    bufferSize,
                    allocationInfoAllocSize,
                    &ahw);

            if (ahbCreateRes != VK_SUCCESS) {
                _RETURN_FAILURE_WITH_DEVICE_MEMORY_REPORT(ahbCreateRes);
            }
        }

        if (importAhb) {
            ahw = importAhbInfoPtr->buffer;
            // We still need to acquire the AHardwareBuffer.
            importAndroidHardwareBuffer(
                ResourceTracker::threadingCallbacks.hostConnectionGetFunc()->grallocHelper(),
                importAhbInfoPtr, nullptr);
        }

        if (ahw) {
            D("%s: Import AHardwareBuffer", __func__);
            const uint32_t hostHandle =
                ResourceTracker::threadingCallbacks.hostConnectionGetFunc()->grallocHelper()
                    ->getHostHandle(AHardwareBuffer_getNativeHandle(ahw));

            AHardwareBuffer_Desc ahbDesc = {};
            AHardwareBuffer_describe(ahw, &ahbDesc);
            if (ahbDesc.format == AHARDWAREBUFFER_FORMAT_BLOB
                && !ResourceTracker::threadingCallbacks.hostConnectionGetFunc()->grallocHelper()->treatBlobAsImage()) {
                importBufferInfo.buffer = hostHandle;
                vk_append_struct(&structChainIter, &importBufferInfo);
            } else {
                importCbInfo.colorBuffer = hostHandle;
                vk_append_struct(&structChainIter, &importCbInfo);
            }
        }
#endif
        zx_handle_t vmo_handle = ZX_HANDLE_INVALID;

#ifdef VK_USE_PLATFORM_FUCHSIA
        if (importBufferCollection) {
            const auto& collection = *reinterpret_cast<
                fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>*>(
                importBufferCollectionInfoPtr->collection);
            auto result = collection->WaitForBuffersAllocated();
            if (!result.ok() || result->status != ZX_OK) {
                ALOGE("WaitForBuffersAllocated failed: %d %d", result.status(),
                      GET_STATUS_SAFE(result, status));
                _RETURN_FAILURE_WITH_DEVICE_MEMORY_REPORT(VK_ERROR_INITIALIZATION_FAILED);
            }
            fuchsia_sysmem::wire::BufferCollectionInfo2& info =
                result->buffer_collection_info;
            uint32_t index = importBufferCollectionInfoPtr->index;
            if (info.buffer_count < index) {
                ALOGE("Invalid buffer index: %d %d", index);
                _RETURN_FAILURE_WITH_DEVICE_MEMORY_REPORT(VK_ERROR_INITIALIZATION_FAILED);
            }
            vmo_handle = info.buffers[index].vmo.release();
        }

        if (importVmo) {
            vmo_handle = importVmoInfoPtr->handle;
        }

        if (exportVmo) {
            bool hasDedicatedImage = dedicatedAllocInfoPtr &&
                (dedicatedAllocInfoPtr->image != VK_NULL_HANDLE);
            bool hasDedicatedBuffer =
                dedicatedAllocInfoPtr &&
                (dedicatedAllocInfoPtr->buffer != VK_NULL_HANDLE);

            if (hasDedicatedImage && hasDedicatedBuffer) {
                ALOGE(
                    "Invalid VkMemoryDedicatedAllocationInfo: At least one "
                    "of image and buffer must be VK_NULL_HANDLE.");
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }

            const VkImageCreateInfo* pImageCreateInfo = nullptr;

            VkBufferConstraintsInfoFUCHSIA bufferConstraintsInfo = {
                .sType =
                    VK_STRUCTURE_TYPE_BUFFER_COLLECTION_CREATE_INFO_FUCHSIA,
                .pNext = nullptr,
                .createInfo = {},
                .requiredFormatFeatures = 0,
                .bufferCollectionConstraints =
                    VkBufferCollectionConstraintsInfoFUCHSIA{
                        .sType =
                            VK_STRUCTURE_TYPE_BUFFER_COLLECTION_CONSTRAINTS_INFO_FUCHSIA,
                        .pNext = nullptr,
                        .minBufferCount = 1,
                        .maxBufferCount = 0,
                        .minBufferCountForCamping = 0,
                        .minBufferCountForDedicatedSlack = 0,
                        .minBufferCountForSharedSlack = 0,
                    },
            };
            const VkBufferConstraintsInfoFUCHSIA* pBufferConstraintsInfo =
                nullptr;

            if (hasDedicatedImage) {
                AutoLock<RecursiveLock> lock(mLock);

                auto it = info_VkImage.find(dedicatedAllocInfoPtr->image);
                if (it == info_VkImage.end()) return VK_ERROR_INITIALIZATION_FAILED;
                const auto& imageInfo = it->second;

                pImageCreateInfo = &imageInfo.createInfo;
            }

            if (hasDedicatedBuffer) {
                AutoLock<RecursiveLock> lock(mLock);

                auto it = info_VkBuffer.find(dedicatedAllocInfoPtr->buffer);
                if (it == info_VkBuffer.end())
                    return VK_ERROR_INITIALIZATION_FAILED;
                const auto& bufferInfo = it->second;

                bufferConstraintsInfo.createInfo = bufferInfo.createInfo;
                pBufferConstraintsInfo = &bufferConstraintsInfo;
            }

            hasDedicatedImage = hasDedicatedImage &&
                                getBufferCollectionConstraintsVulkanImageUsage(
                                    pImageCreateInfo);
            hasDedicatedBuffer =
                hasDedicatedBuffer &&
                getBufferCollectionConstraintsVulkanBufferUsage(
                    pBufferConstraintsInfo);

            if (hasDedicatedImage || hasDedicatedBuffer) {
                auto token_ends =
                    fidl::CreateEndpoints<::fuchsia_sysmem::BufferCollectionToken>();
                if (!token_ends.is_ok()) {
                    ALOGE("zx_channel_create failed: %d", token_ends.status_value());
                    abort();
                }

                {
                    auto result = mSysmemAllocator->AllocateSharedCollection(
                        std::move(token_ends->server));
                    if (!result.ok()) {
                        ALOGE("AllocateSharedCollection failed: %d",
                              result.status());
                        abort();
                    }
                }

                auto collection_ends =
                    fidl::CreateEndpoints<::fuchsia_sysmem::BufferCollection>();
                if (!collection_ends.is_ok()) {
                    ALOGE("zx_channel_create failed: %d", collection_ends.status_value());
                    abort();
                }

                {
                    auto result = mSysmemAllocator->BindSharedCollection(
                        std::move(token_ends->client), std::move(collection_ends->server));
                    if (!result.ok()) {
                        ALOGE("BindSharedCollection failed: %d",
                              result.status());
                        abort();
                    }
                }

                fidl::WireSyncClient<fuchsia_sysmem::BufferCollection> collection(
                    std::move(collection_ends->client));
                if (hasDedicatedImage) {
                    // TODO(fxbug.dev/90856): Use setBufferCollectionImageConstraintsFUCHSIA.
                    VkResult res = setBufferCollectionConstraintsFUCHSIA(
                        enc, device, &collection, pImageCreateInfo);
                    if (res == VK_ERROR_FORMAT_NOT_SUPPORTED) {
                      ALOGE("setBufferCollectionConstraints failed: format %u is not supported",
                            pImageCreateInfo->format);
                      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                    }
                    if (res != VK_SUCCESS) {
                        ALOGE("setBufferCollectionConstraints failed: %d", res);
                        abort();
                    }
                }

                if (hasDedicatedBuffer) {
                    VkResult res = setBufferCollectionBufferConstraintsFUCHSIA(
                        &collection, pBufferConstraintsInfo);
                    if (res != VK_SUCCESS) {
                        ALOGE("setBufferCollectionBufferConstraints failed: %d",
                              res);
                        abort();
                    }
                }

                {
                    auto result = collection->WaitForBuffersAllocated();
                    if (result.ok() && result->status == ZX_OK) {
                        fuchsia_sysmem::wire::BufferCollectionInfo2& info =
                            result->buffer_collection_info;
                        if (!info.buffer_count) {
                            ALOGE(
                                "WaitForBuffersAllocated returned "
                                "invalid count: %d",
                                info.buffer_count);
                            abort();
                        }
                        vmo_handle = info.buffers[0].vmo.release();
                    } else {
                        ALOGE("WaitForBuffersAllocated failed: %d %d",
                              result.status(), GET_STATUS_SAFE(result, status));
                        abort();
                    }
                }

                collection->Close();

                zx::vmo vmo_copy;
                zx_status_t status = zx_handle_duplicate(vmo_handle, ZX_RIGHT_SAME_RIGHTS,
                                                         vmo_copy.reset_and_get_address());
                if (status != ZX_OK) {
                    ALOGE("Failed to duplicate VMO: %d", status);
                    abort();
                }

                if (pImageCreateInfo) {
                    // Only device-local images need to create color buffer; for
                    // host-visible images, the color buffer is already created
                    // when sysmem allocates memory. Here we use the |tiling|
                    // field of image creation info to determine if it uses
                    // host-visible memory.
                    bool isLinear = pImageCreateInfo->tiling == VK_IMAGE_TILING_LINEAR;
                    if (!isLinear) {
                        fuchsia_hardware_goldfish::wire::ColorBufferFormatType format;
                        switch (pImageCreateInfo->format) {
                            case VK_FORMAT_B8G8R8A8_SINT:
                            case VK_FORMAT_B8G8R8A8_UNORM:
                            case VK_FORMAT_B8G8R8A8_SRGB:
                            case VK_FORMAT_B8G8R8A8_SNORM:
                            case VK_FORMAT_B8G8R8A8_SSCALED:
                            case VK_FORMAT_B8G8R8A8_USCALED:
                                format = fuchsia_hardware_goldfish::wire::ColorBufferFormatType::
                                        kBgra;
                                break;
                            case VK_FORMAT_R8G8B8A8_SINT:
                            case VK_FORMAT_R8G8B8A8_UNORM:
                            case VK_FORMAT_R8G8B8A8_SRGB:
                            case VK_FORMAT_R8G8B8A8_SNORM:
                            case VK_FORMAT_R8G8B8A8_SSCALED:
                            case VK_FORMAT_R8G8B8A8_USCALED:
                                format = fuchsia_hardware_goldfish::wire::ColorBufferFormatType::
                                        kRgba;
                                break;
                            case VK_FORMAT_R8_UNORM:
                            case VK_FORMAT_R8_UINT:
                            case VK_FORMAT_R8_USCALED:
                            case VK_FORMAT_R8_SNORM:
                            case VK_FORMAT_R8_SINT:
                            case VK_FORMAT_R8_SSCALED:
                            case VK_FORMAT_R8_SRGB:
                                format = fuchsia_hardware_goldfish::wire::ColorBufferFormatType::
                                        kLuminance;
                                break;
                            case VK_FORMAT_R8G8_UNORM:
                            case VK_FORMAT_R8G8_UINT:
                            case VK_FORMAT_R8G8_USCALED:
                            case VK_FORMAT_R8G8_SNORM:
                            case VK_FORMAT_R8G8_SINT:
                            case VK_FORMAT_R8G8_SSCALED:
                            case VK_FORMAT_R8G8_SRGB:
                                format =
                                        fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kRg;
                                break;
                            default:
                                ALOGE("Unsupported format: %d",
                                      pImageCreateInfo->format);
                                abort();
                        }

                        fidl::Arena arena;
                        fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params createParams(
                                arena);
                        createParams.set_width(pImageCreateInfo->extent.width)
                                .set_height(pImageCreateInfo->extent.height)
                                .set_format(format)
                                .set_memory_property(fuchsia_hardware_goldfish::wire::
                                                             kMemoryPropertyDeviceLocal);

                        auto result = mControlDevice->CreateColorBuffer2(std::move(vmo_copy),
                                                                         std::move(createParams));
                        if (!result.ok() || result->res != ZX_OK) {
                            if (result.ok() &&
                                result->res == ZX_ERR_ALREADY_EXISTS) {
                                ALOGD("CreateColorBuffer: color buffer already "
                                      "exists\n");
                            } else {
                                ALOGE("CreateColorBuffer failed: %d:%d",
                                      result.status(),
                                      GET_STATUS_SAFE(result, res));
                                abort();
                            }
                        }
                    }
                }

                if (pBufferConstraintsInfo) {
                    fidl::Arena arena;
                    fuchsia_hardware_goldfish::wire::CreateBuffer2Params createParams(arena);
                    createParams
                        .set_size(arena,
                                  pBufferConstraintsInfo->createInfo.size)
                        .set_memory_property(fuchsia_hardware_goldfish::wire::
                                                 kMemoryPropertyDeviceLocal);

                    auto result =
                        mControlDevice->CreateBuffer2(std::move(vmo_copy), std::move(createParams));
                    if (!result.ok() || result->is_error()) {
                        ALOGE("CreateBuffer2 failed: %d:%d", result.status(),
                              GET_STATUS_SAFE(result, error_value()));
                        abort();
                    }
                }
            } else {
                ALOGW("Dedicated image / buffer not available. Cannot create "
                      "BufferCollection to export VMOs.");
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
        }

        if (vmo_handle != ZX_HANDLE_INVALID) {
            zx::vmo vmo_copy;
            zx_status_t status = zx_handle_duplicate(vmo_handle,
                                                     ZX_RIGHT_SAME_RIGHTS,
                                                     vmo_copy.reset_and_get_address());
            if (status != ZX_OK) {
                ALOGE("Failed to duplicate VMO: %d", status);
                abort();
            }
            zx_status_t status2 = ZX_OK;

            auto result = mControlDevice->GetBufferHandle(std::move(vmo_copy));
            if (!result.ok() || result->res != ZX_OK) {
                ALOGE("GetBufferHandle failed: %d:%d", result.status(),
                      GET_STATUS_SAFE(result, res));
            } else {
                fuchsia_hardware_goldfish::wire::BufferHandleType
                    handle_type = result->type;
                uint32_t buffer_handle = result->id;

                if (handle_type == fuchsia_hardware_goldfish::wire::
                                       BufferHandleType::kBuffer) {
                    importBufferInfo.buffer = buffer_handle;
                    vk_append_struct(&structChainIter, &importBufferInfo);
                } else {
                    importCbInfo.colorBuffer = buffer_handle;
                    vk_append_struct(&structChainIter, &importCbInfo);
                }
            }
        }
#endif

        if (ahw || !requestedMemoryIsHostVisible) {
            input_result =
                enc->vkAllocateMemory(
                    device, &finalAllocInfo, pAllocator, pMemory, true /* do lock */);

            if (input_result != VK_SUCCESS) {
                _RETURN_FAILURE_WITH_DEVICE_MEMORY_REPORT(input_result);
            }

            VkDeviceSize allocationSize = finalAllocInfo.allocationSize;
            setDeviceMemoryInfo(
                device, *pMemory,
                0, nullptr,
                finalAllocInfo.memoryTypeIndex,
                ahw,
                isImport,
                vmo_handle);

            _RETURN_SCUCCESS_WITH_DEVICE_MEMORY_REPORT;
        }

#ifdef VK_USE_PLATFORM_FUCHSIA
        if (vmo_handle != ZX_HANDLE_INVALID) {
            input_result = enc->vkAllocateMemory(device, &finalAllocInfo, pAllocator, pMemory, true /* do lock */);

            // Get VMO handle rights, and only use allowed rights to map the
            // host memory.
            zx_info_handle_basic handle_info;
            zx_status_t status = zx_object_get_info(vmo_handle, ZX_INFO_HANDLE_BASIC, &handle_info,
                                        sizeof(handle_info), nullptr, nullptr);
            if (status != ZX_OK) {
                ALOGE("%s: cannot get vmo object info: vmo = %u status: %d.", __func__, vmo_handle,
                      status);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }

            zx_vm_option_t vm_permission = 0u;
            vm_permission |= (handle_info.rights & ZX_RIGHT_READ) ? ZX_VM_PERM_READ : 0;
            vm_permission |= (handle_info.rights & ZX_RIGHT_WRITE) ? ZX_VM_PERM_WRITE : 0;

            zx_paddr_t addr;
            status = zx_vmar_map(zx_vmar_root_self(), vm_permission, 0, vmo_handle, 0,
                finalAllocInfo.allocationSize, &addr);
            if (status != ZX_OK) {
                ALOGE("%s: cannot map vmar: status %d.", __func__, status);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }

            setDeviceMemoryInfo(device, *pMemory,
                finalAllocInfo.allocationSize,
                reinterpret_cast<uint8_t*>(addr), finalAllocInfo.memoryTypeIndex,
                /*ahw=*/nullptr, isImport, vmo_handle);
            return VK_SUCCESS;
        }
#endif

        // Host visible memory with direct mapping
        VkResult result = getCoherentMemory(&finalAllocInfo, enc, device, pMemory);
        if (result != VK_SUCCESS) {
            return result;
        }

        _RETURN_SCUCCESS_WITH_DEVICE_MEMORY_REPORT;
    }

    CoherentMemoryPtr freeCoherentMemoryLocked(VkDeviceMemory memory, VkDeviceMemory_Info& info) {
        if (info.coherentMemory && info.ptr) {
            if (info.coherentMemory->getDeviceMemory() != memory) {
                delete_goldfish_VkDeviceMemory(memory);
            }

            if (info.ptr) {
                info.coherentMemory->release(info.ptr);
                info.ptr = nullptr;
            }

            return std::move(info.coherentMemory);
        }

        return nullptr;
    }

    void on_vkFreeMemory(
        void* context,
        VkDevice device,
        VkDeviceMemory memory,
        const VkAllocationCallbacks* pAllocateInfo) {

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return;
        auto& info = it->second;
        uint64_t memoryObjectId = (uint64_t)(void*)memory;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        if (info.ahw) {
            memoryObjectId = getAHardwareBufferId(info.ahw);
        }
#endif

        emitDeviceMemoryReport(info_VkDevice[device],
                               info.imported ? VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_UNIMPORT_EXT
                                             : VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT,
                               memoryObjectId, 0 /* size */, VK_OBJECT_TYPE_DEVICE_MEMORY,
                               (uint64_t)(void*)memory);

#ifdef VK_USE_PLATFORM_FUCHSIA
        if (info.vmoHandle && info.ptr) {
            zx_status_t status = zx_vmar_unmap(
                zx_vmar_root_self(), reinterpret_cast<zx_paddr_t>(info.ptr), info.allocationSize);
            if (status != ZX_OK) {
                ALOGE("%s: Cannot unmap ptr: status %d", status);
            }
            info.ptr = nullptr;
        }
#endif

        if (!info.coherentMemory) {
            lock.unlock();
            VkEncoder* enc = (VkEncoder*)context;
            enc->vkFreeMemory(device, memory, pAllocateInfo, true /* do lock */);
            return;
        }

        auto coherentMemory = freeCoherentMemoryLocked(memory, info);

        // We have to release the lock before we could possibly free a
        // CoherentMemory, because that will call into VkEncoder, which
        // shouldn't be called when the lock is held.
        lock.unlock();
        coherentMemory = nullptr;
    }

    VkResult on_vkMapMemory(void* context, VkResult host_result, VkDevice device,
                            VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                            VkMemoryMapFlags, void** ppData) {
        if (host_result != VK_SUCCESS) {
            ALOGE("%s: Host failed to map\n", __func__);
            return host_result;
        }

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) {
            ALOGE("%s: Could not find this device memory\n", __func__);
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        auto& info = it->second;

        if (info.blobId && !info.coherentMemory && !mCaps.params[kParamCreateGuestHandle]) {
            VkEncoder* enc = (VkEncoder*)context;
            VirtGpuBlobMappingPtr mapping;
            VirtGpuDevice& instance = VirtGpuDevice::getInstance();

            uint64_t offset;
            uint8_t* ptr;

            VkResult vkResult = enc->vkGetBlobGOOGLE(device, memory, false);
            if (vkResult != VK_SUCCESS) return vkResult;

            struct VirtGpuCreateBlob createBlob = {};
            createBlob.blobMem = kBlobMemHost3d;
            createBlob.flags = kBlobFlagMappable;
            createBlob.blobId = info.blobId;
            createBlob.size = info.coherentMemorySize;

            auto blob = instance.createBlob(createBlob);
            if (!blob) {
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }

            mapping = blob->createMapping();
            if (!mapping) {
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }

            auto coherentMemory =
                std::make_shared<CoherentMemory>(mapping, createBlob.size, device, memory);

            coherentMemory->subAllocate(info.allocationSize, &ptr, offset);

            info.coherentMemoryOffset = offset;
            info.coherentMemory = coherentMemory;
            info.ptr = ptr;
        }

        if (!info.ptr) {
            ALOGE("%s: ptr null\n", __func__);
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        if (size != VK_WHOLE_SIZE &&
            (info.ptr + offset + size > info.ptr + info.allocationSize)) {
            ALOGE("%s: size is too big. alloc size 0x%llx while we wanted offset 0x%llx size 0x%llx total 0x%llx\n", __func__,
                    (unsigned long long)info.allocationSize,
                    (unsigned long long)offset,
                    (unsigned long long)size,
                    (unsigned long long)offset);
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        *ppData = info.ptr + offset;

        return host_result;
    }

    void on_vkUnmapMemory(
        void*,
        VkDevice,
        VkDeviceMemory) {
        // no-op
    }

    void transformExternalResourceMemoryDedicatedRequirementsForGuest(
        VkMemoryDedicatedRequirements* dedicatedReqs) {
        dedicatedReqs->prefersDedicatedAllocation = VK_TRUE;
        dedicatedReqs->requiresDedicatedAllocation = VK_TRUE;
    }

    void transformImageMemoryRequirementsForGuestLocked(
        VkImage image,
        VkMemoryRequirements* reqs) {

        setMemoryRequirementsForSysmemBackedImage(image, reqs);
    }

    void transformImageMemoryRequirements2ForGuest(
        VkImage image,
        VkMemoryRequirements2* reqs2) {

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkImage.find(image);
        if (it == info_VkImage.end()) return;

        auto& info = it->second;

        if (!info.external ||
            !info.externalCreateInfo.handleTypes) {
            setMemoryRequirementsForSysmemBackedImage(image, &reqs2->memoryRequirements);
            return;
        }

        setMemoryRequirementsForSysmemBackedImage(image, &reqs2->memoryRequirements);

        VkMemoryDedicatedRequirements* dedicatedReqs =
            vk_find_struct<VkMemoryDedicatedRequirements>(reqs2);

        if (!dedicatedReqs) return;

        transformExternalResourceMemoryDedicatedRequirementsForGuest(
            dedicatedReqs);
    }

    void transformBufferMemoryRequirements2ForGuest(
        VkBuffer buffer,
        VkMemoryRequirements2* reqs2) {

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkBuffer.find(buffer);
        if (it == info_VkBuffer.end()) return;

        auto& info = it->second;

        if (!info.external ||
            !info.externalCreateInfo.handleTypes) {
            return;
        }

        VkMemoryDedicatedRequirements* dedicatedReqs =
            vk_find_struct<VkMemoryDedicatedRequirements>(reqs2);

        if (!dedicatedReqs) return;

        transformExternalResourceMemoryDedicatedRequirementsForGuest(
            dedicatedReqs);
    }

    VkResult on_vkCreateImage(
        void* context, VkResult,
        VkDevice device, const VkImageCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkImage *pImage) {
        VkEncoder* enc = (VkEncoder*)context;

        VkImageCreateInfo localCreateInfo = vk_make_orphan_copy(*pCreateInfo);
        vk_struct_chain_iterator structChainIter = vk_make_chain_iterator(&localCreateInfo);
        VkExternalMemoryImageCreateInfo localExtImgCi;

        const VkExternalMemoryImageCreateInfo* extImgCiPtr =
            vk_find_struct<VkExternalMemoryImageCreateInfo>(pCreateInfo);
        if (extImgCiPtr) {
            localExtImgCi = vk_make_orphan_copy(*extImgCiPtr);
            vk_append_struct(&structChainIter, &localExtImgCi);
        }

#ifdef VK_USE_PLATFORM_ANDROID_KHR
        VkNativeBufferANDROID localAnb;
        const VkNativeBufferANDROID* anbInfoPtr =
            vk_find_struct<VkNativeBufferANDROID>(pCreateInfo);
        if (anbInfoPtr) {
            localAnb = vk_make_orphan_copy(*anbInfoPtr);
            vk_append_struct(&structChainIter, &localAnb);
        }

        VkExternalFormatANDROID localExtFormatAndroid;
        const VkExternalFormatANDROID* extFormatAndroidPtr =
            vk_find_struct<VkExternalFormatANDROID>(pCreateInfo);
        if (extFormatAndroidPtr) {
            localExtFormatAndroid = vk_make_orphan_copy(*extFormatAndroidPtr);

            // Do not append external format android;
            // instead, replace the local image localCreateInfo format
            // with the corresponding Vulkan format
            if (extFormatAndroidPtr->externalFormat) {
                localCreateInfo.format =
                    vk_format_from_android(extFormatAndroidPtr->externalFormat);
                if (localCreateInfo.format == VK_FORMAT_UNDEFINED)
                    return VK_ERROR_VALIDATION_FAILED_EXT;
            }
        }
#endif

#ifdef VK_USE_PLATFORM_FUCHSIA
        const VkBufferCollectionImageCreateInfoFUCHSIA* extBufferCollectionPtr =
            vk_find_struct<VkBufferCollectionImageCreateInfoFUCHSIA>(
                pCreateInfo);

        bool isSysmemBackedMemory = false;

        if (extImgCiPtr &&
            (extImgCiPtr->handleTypes &
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_ZIRCON_VMO_BIT_FUCHSIA)) {
            isSysmemBackedMemory = true;
        }

        if (extBufferCollectionPtr) {
            const auto& collection = *reinterpret_cast<
                fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>*>(
                extBufferCollectionPtr->collection);
            uint32_t index = extBufferCollectionPtr->index;
            zx::vmo vmo;

            fuchsia_sysmem::wire::BufferCollectionInfo2 info;

            auto result = collection->WaitForBuffersAllocated();
            if (result.ok() && result->status == ZX_OK) {
                info = std::move(result->buffer_collection_info);
                if (index < info.buffer_count && info.settings.has_image_format_constraints) {
                    vmo = std::move(info.buffers[index].vmo);
                }
            } else {
                ALOGE("WaitForBuffersAllocated failed: %d %d", result.status(),
                      GET_STATUS_SAFE(result, status));
            }

            if (vmo.is_valid()) {
                zx::vmo vmo_dup;
                if (zx_status_t status = vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo_dup);
                    status != ZX_OK) {
                    ALOGE("%s: zx_vmo_duplicate failed: %d", __func__, status);
                    abort();
                }

                auto buffer_handle_result = mControlDevice->GetBufferHandle(std::move(vmo_dup));
                if (!buffer_handle_result.ok()) {
                    ALOGE("%s: GetBufferHandle FIDL error: %d", __func__,
                          buffer_handle_result.status());
                    abort();
                }
                if (buffer_handle_result.value().res == ZX_OK) {
                    // Buffer handle already exists.
                    // If it is a ColorBuffer, no-op; Otherwise return error.
                    if (buffer_handle_result.value().type !=
                        fuchsia_hardware_goldfish::wire::BufferHandleType::kColorBuffer) {
                        ALOGE("%s: BufferHandle %u is not a ColorBuffer", __func__,
                              buffer_handle_result.value().id);
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    }
                } else if (buffer_handle_result.value().res == ZX_ERR_NOT_FOUND) {
                    // Buffer handle not found. Create ColorBuffer based on buffer settings.
                    auto format =
                        info.settings.image_format_constraints.pixel_format.type ==
                                fuchsia_sysmem::wire::PixelFormatType::kR8G8B8A8
                            ? fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kRgba
                            : fuchsia_hardware_goldfish::wire::ColorBufferFormatType::kBgra;

                    uint32_t memory_property =
                        info.settings.buffer_settings.heap ==
                                fuchsia_sysmem::wire::HeapType::kGoldfishDeviceLocal
                            ? fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal
                            : fuchsia_hardware_goldfish::wire::kMemoryPropertyHostVisible;

                    fidl::Arena arena;
                    fuchsia_hardware_goldfish::wire::CreateColorBuffer2Params createParams(
                        arena);
                    createParams.set_width(
                            info.settings.image_format_constraints.min_coded_width)
                        .set_height(
                            info.settings.image_format_constraints.min_coded_height)
                        .set_format(format)
                        .set_memory_property(memory_property);

                    auto result =
                        mControlDevice->CreateColorBuffer2(std::move(vmo), std::move(createParams));
                    if (result.ok() && result->res == ZX_ERR_ALREADY_EXISTS) {
                        ALOGD(
                            "CreateColorBuffer: color buffer already exists\n");
                    } else if (!result.ok() || result->res != ZX_OK) {
                        ALOGE("CreateColorBuffer failed: %d:%d", result.status(),
                            GET_STATUS_SAFE(result, res));
                    }
                }

                if (info.settings.buffer_settings.heap ==
                    fuchsia_sysmem::wire::HeapType::kGoldfishHostVisible) {
                    ALOGD(
                        "%s: Image uses host visible memory heap; set tiling "
                        "to linear to match host ImageCreateInfo",
                        __func__);
                    localCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;
                }
            }
            isSysmemBackedMemory = true;
        }

        if (isSysmemBackedMemory) {
            localCreateInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        }
#endif

        VkResult res;
        VkMemoryRequirements memReqs;

        if (supportsCreateResourcesWithRequirements()) {
            res = enc->vkCreateImageWithRequirementsGOOGLE(device, &localCreateInfo, pAllocator, pImage, &memReqs, true /* do lock */);
        } else {
            res = enc->vkCreateImage(device, &localCreateInfo, pAllocator, pImage, true /* do lock */);
        }

        if (res != VK_SUCCESS) return res;

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkImage.find(*pImage);
        if (it == info_VkImage.end()) return VK_ERROR_INITIALIZATION_FAILED;

        auto& info = it->second;

        info.device = device;
        info.createInfo = *pCreateInfo;
        info.createInfo.pNext = nullptr;

#ifdef VK_USE_PLATFORM_ANDROID_KHR
        if (extFormatAndroidPtr && extFormatAndroidPtr->externalFormat) {
            info.hasExternalFormat = true;
            info.androidFormat = extFormatAndroidPtr->externalFormat;
        }
#endif  // VK_USE_PLATFORM_ANDROID_KHR

        if (supportsCreateResourcesWithRequirements()) {
            info.baseRequirementsKnown = true;
        }

        if (extImgCiPtr) {
            info.external = true;
            info.externalCreateInfo = *extImgCiPtr;
        }

#ifdef VK_USE_PLATFORM_FUCHSIA
        if (isSysmemBackedMemory) {
            info.isSysmemBackedMemory = true;
        }
#endif

// Delete `protocolVersion` check goldfish drivers are gone.
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        if (mCaps.gfxstreamCapset.colorBufferMemoryIndex == 0xFFFFFFFF) {
            mCaps.gfxstreamCapset.colorBufferMemoryIndex =
                    getColorBufferMemoryIndex(context, device);
        }
        if (extImgCiPtr &&
            (extImgCiPtr->handleTypes &
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID)) {
            updateMemoryTypeBits(&memReqs.memoryTypeBits,
                                 mCaps.gfxstreamCapset.colorBufferMemoryIndex);
        }
#endif

        if (info.baseRequirementsKnown) {
            transformImageMemoryRequirementsForGuestLocked(*pImage, &memReqs);
            info.baseRequirements = memReqs;
        }
        return res;
    }

    VkResult on_vkCreateSamplerYcbcrConversion(
        void* context, VkResult,
        VkDevice device,
        const VkSamplerYcbcrConversionCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSamplerYcbcrConversion* pYcbcrConversion) {

        VkSamplerYcbcrConversionCreateInfo localCreateInfo = vk_make_orphan_copy(*pCreateInfo);

#ifdef VK_USE_PLATFORM_ANDROID_KHR
        const VkExternalFormatANDROID* extFormatAndroidPtr =
            vk_find_struct<VkExternalFormatANDROID>(pCreateInfo);
        if (extFormatAndroidPtr) {
            if (extFormatAndroidPtr->externalFormat == AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM) {
                // We don't support external formats on host and it causes RGB565
                // to fail in CtsGraphicsTestCases android.graphics.cts.BasicVulkanGpuTest
                // when passed as an external format.
                // We may consider doing this for all external formats.
                // See b/134771579.
                *pYcbcrConversion = VK_YCBCR_CONVERSION_DO_NOTHING;
                return VK_SUCCESS;
            } else if (extFormatAndroidPtr->externalFormat) {
                localCreateInfo.format =
                    vk_format_from_android(extFormatAndroidPtr->externalFormat);
            }
        }
#endif

        VkEncoder* enc = (VkEncoder*)context;
        VkResult res = enc->vkCreateSamplerYcbcrConversion(
            device, &localCreateInfo, pAllocator, pYcbcrConversion, true /* do lock */);

        if (*pYcbcrConversion == VK_YCBCR_CONVERSION_DO_NOTHING) {
            ALOGE("FATAL: vkCreateSamplerYcbcrConversion returned a reserved value (VK_YCBCR_CONVERSION_DO_NOTHING)");
            abort();
        }
        return res;
    }

    void on_vkDestroySamplerYcbcrConversion(
        void* context,
        VkDevice device,
        VkSamplerYcbcrConversion ycbcrConversion,
        const VkAllocationCallbacks* pAllocator) {
        VkEncoder* enc = (VkEncoder*)context;
        if (ycbcrConversion != VK_YCBCR_CONVERSION_DO_NOTHING) {
            enc->vkDestroySamplerYcbcrConversion(device, ycbcrConversion, pAllocator, true /* do lock */);
        }
    }

    VkResult on_vkCreateSamplerYcbcrConversionKHR(
        void* context, VkResult,
        VkDevice device,
        const VkSamplerYcbcrConversionCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSamplerYcbcrConversion* pYcbcrConversion) {

        VkSamplerYcbcrConversionCreateInfo localCreateInfo = vk_make_orphan_copy(*pCreateInfo);

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        const VkExternalFormatANDROID* extFormatAndroidPtr =
            vk_find_struct<VkExternalFormatANDROID>(pCreateInfo);
        if (extFormatAndroidPtr) {
            if (extFormatAndroidPtr->externalFormat == AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM) {
                // We don't support external formats on host and it causes RGB565
                // to fail in CtsGraphicsTestCases android.graphics.cts.BasicVulkanGpuTest
                // when passed as an external format.
                // We may consider doing this for all external formats.
                // See b/134771579.
                *pYcbcrConversion = VK_YCBCR_CONVERSION_DO_NOTHING;
                return VK_SUCCESS;
            } else if (extFormatAndroidPtr->externalFormat) {
                localCreateInfo.format =
                    vk_format_from_android(extFormatAndroidPtr->externalFormat);
            }
        }
#endif

        VkEncoder* enc = (VkEncoder*)context;
        VkResult res = enc->vkCreateSamplerYcbcrConversionKHR(
            device, &localCreateInfo, pAllocator, pYcbcrConversion, true /* do lock */);

        if (*pYcbcrConversion == VK_YCBCR_CONVERSION_DO_NOTHING) {
            ALOGE("FATAL: vkCreateSamplerYcbcrConversionKHR returned a reserved value (VK_YCBCR_CONVERSION_DO_NOTHING)");
            abort();
        }
        return res;
    }

    void on_vkDestroySamplerYcbcrConversionKHR(
        void* context,
        VkDevice device,
        VkSamplerYcbcrConversion ycbcrConversion,
        const VkAllocationCallbacks* pAllocator) {
        VkEncoder* enc = (VkEncoder*)context;
        if (ycbcrConversion != VK_YCBCR_CONVERSION_DO_NOTHING) {
            enc->vkDestroySamplerYcbcrConversionKHR(device, ycbcrConversion, pAllocator, true /* do lock */);
        }
    }

    VkResult on_vkCreateSampler(
        void* context, VkResult,
        VkDevice device,
        const VkSamplerCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSampler* pSampler) {
        VkSamplerCreateInfo localCreateInfo = vk_make_orphan_copy(*pCreateInfo);
        vk_struct_chain_iterator structChainIter = vk_make_chain_iterator(&localCreateInfo);

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_USE_PLATFORM_FUCHSIA)
        VkSamplerYcbcrConversionInfo localVkSamplerYcbcrConversionInfo;
        const VkSamplerYcbcrConversionInfo* samplerYcbcrConversionInfo =
            vk_find_struct<VkSamplerYcbcrConversionInfo>(pCreateInfo);
        if (samplerYcbcrConversionInfo) {
            if (samplerYcbcrConversionInfo->conversion != VK_YCBCR_CONVERSION_DO_NOTHING) {
                localVkSamplerYcbcrConversionInfo =
                    vk_make_orphan_copy(*samplerYcbcrConversionInfo);
                vk_append_struct(&structChainIter, &localVkSamplerYcbcrConversionInfo);
            }
        }

        VkSamplerCustomBorderColorCreateInfoEXT localVkSamplerCustomBorderColorCreateInfo;
        const VkSamplerCustomBorderColorCreateInfoEXT* samplerCustomBorderColorCreateInfo =
            vk_find_struct<VkSamplerCustomBorderColorCreateInfoEXT>(pCreateInfo);
        if (samplerCustomBorderColorCreateInfo) {
            localVkSamplerCustomBorderColorCreateInfo =
                vk_make_orphan_copy(*samplerCustomBorderColorCreateInfo);
            vk_append_struct(&structChainIter, &localVkSamplerCustomBorderColorCreateInfo);
        }
#endif

        VkEncoder* enc = (VkEncoder*)context;
        return enc->vkCreateSampler(device, &localCreateInfo, pAllocator, pSampler, true /* do lock */);
    }

    void on_vkGetPhysicalDeviceExternalBufferProperties(
        void* context,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo,
        VkExternalBufferProperties* pExternalBufferProperties) {
        VkEncoder* enc = (VkEncoder*)context;
        // b/299520213
        // We declared blob formar not supported.
        if (ResourceTracker::threadingCallbacks.hostConnectionGetFunc()->grallocHelper()->treatBlobAsImage()
            && pExternalBufferInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) {
            pExternalBufferProperties->externalMemoryProperties.externalMemoryFeatures = 0;
            pExternalBufferProperties->externalMemoryProperties.exportFromImportedHandleTypes = 0;
            pExternalBufferProperties->externalMemoryProperties.compatibleHandleTypes = 0;
            return;
        }
        enc->vkGetPhysicalDeviceExternalBufferProperties(physicalDevice, pExternalBufferInfo, pExternalBufferProperties, true /* do lock */);
    }

    void on_vkGetPhysicalDeviceExternalFenceProperties(
        void* context,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo,
        VkExternalFenceProperties* pExternalFenceProperties) {

        (void)context;
        (void)physicalDevice;

        pExternalFenceProperties->exportFromImportedHandleTypes = 0;
        pExternalFenceProperties->compatibleHandleTypes = 0;
        pExternalFenceProperties->externalFenceFeatures = 0;

        bool syncFd =
            pExternalFenceInfo->handleType &
            VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

        if (!syncFd) {
            return;
        }

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        pExternalFenceProperties->exportFromImportedHandleTypes =
            VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
        pExternalFenceProperties->compatibleHandleTypes =
            VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
        pExternalFenceProperties->externalFenceFeatures =
            VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT |
            VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT;

        D("%s: asked for sync fd, set the features\n", __func__);
#endif
    }

    VkResult on_vkCreateFence(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkFenceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator, VkFence* pFence) {

        VkEncoder* enc = (VkEncoder*)context;
        VkFenceCreateInfo finalCreateInfo = *pCreateInfo;

        const VkExportFenceCreateInfo* exportFenceInfoPtr =
            vk_find_struct<VkExportFenceCreateInfo>(pCreateInfo);

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        bool exportSyncFd =
            exportFenceInfoPtr &&
            (exportFenceInfoPtr->handleTypes &
             VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT);
#endif

        input_result = enc->vkCreateFence(
            device, &finalCreateInfo, pAllocator, pFence, true /* do lock */);

        if (input_result != VK_SUCCESS) return input_result;

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (exportSyncFd) {
            if (!mFeatureInfo->hasVirtioGpuNativeSync) {
                ALOGV("%s: ensure sync device\n", __func__);
                ensureSyncDeviceFd();
            }

            ALOGV("%s: getting fence info\n", __func__);
            AutoLock<RecursiveLock> lock(mLock);
            auto it = info_VkFence.find(*pFence);

            if (it == info_VkFence.end())
                return VK_ERROR_INITIALIZATION_FAILED;

            auto& info = it->second;

            info.external = true;
            info.exportFenceCreateInfo = *exportFenceInfoPtr;
            ALOGV("%s: info set (fence still -1). fence: %p\n", __func__, (void*)(*pFence));
            // syncFd is still -1 because we expect user to explicitly
            // export it via vkGetFenceFdKHR
        }
#endif

        return input_result;
    }

    void on_vkDestroyFence(
        void* context,
        VkDevice device,
        VkFence fence,
        const VkAllocationCallbacks* pAllocator) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkDestroyFence(device, fence, pAllocator, true /* do lock */);
    }

    VkResult on_vkResetFences(
        void* context,
        VkResult,
        VkDevice device,
        uint32_t fenceCount,
        const VkFence* pFences) {

        VkEncoder* enc = (VkEncoder*)context;
        VkResult res = enc->vkResetFences(device, fenceCount, pFences, true /* do lock */);

        if (res != VK_SUCCESS) return res;

        if (!fenceCount) return res;

        // Permanence: temporary
        // on fence reset, close the fence fd
        // and act like we need to GetFenceFdKHR/ImportFenceFdKHR again
        AutoLock<RecursiveLock> lock(mLock);
        for (uint32_t i = 0; i < fenceCount; ++i) {
            VkFence fence = pFences[i];
            auto it = info_VkFence.find(fence);
            auto& info = it->second;
            if (!info.external) continue;

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
            if (info.syncFd >= 0) {
                ALOGV("%s: resetting fence. make fd -1\n", __func__);
                goldfish_sync_signal(info.syncFd);
                close(info.syncFd);
                info.syncFd = -1;
            }
#endif
        }

        return res;
    }

    VkResult on_vkImportFenceFdKHR(
        void* context,
        VkResult,
        VkDevice device,
        const VkImportFenceFdInfoKHR* pImportFenceFdInfo) {

        (void)context;
        (void)device;
        (void)pImportFenceFdInfo;

        // Transference: copy
        // meaning dup() the incoming fd

        VkEncoder* enc = (VkEncoder*)context;

        bool hasFence = pImportFenceFdInfo->fence != VK_NULL_HANDLE;

        if (!hasFence) return VK_ERROR_OUT_OF_HOST_MEMORY;

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)

        bool syncFdImport =
            pImportFenceFdInfo->handleType & VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

        if (!syncFdImport) {
            ALOGV("%s: VK_ERROR_OUT_OF_HOST_MEMORY: no sync fd import\n", __func__);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        AutoLock<RecursiveLock> lock(mLock);
        auto it = info_VkFence.find(pImportFenceFdInfo->fence);
        if (it == info_VkFence.end()) {
            ALOGV("%s: VK_ERROR_OUT_OF_HOST_MEMORY: no fence info\n", __func__);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        auto& info = it->second;

        if (info.syncFd >= 0) {
            ALOGV("%s: previous sync fd exists, close it\n", __func__);
            goldfish_sync_signal(info.syncFd);
            close(info.syncFd);
        }

        if (pImportFenceFdInfo->fd < 0) {
            ALOGV("%s: import -1, set to -1 and exit\n", __func__);
            info.syncFd = -1;
        } else {
            ALOGV("%s: import actual fd, dup and close()\n", __func__);
            info.syncFd = dup(pImportFenceFdInfo->fd);
            close(pImportFenceFdInfo->fd);
        }
        return VK_SUCCESS;
#else
        return VK_ERROR_OUT_OF_HOST_MEMORY;
#endif
    }

    VkResult createFence(VkDevice device, uint64_t hostFenceHandle, int64_t& osHandle) {
        struct VirtGpuExecBuffer exec = { };
        struct gfxstreamCreateExportSyncVK exportSync = { };
        VirtGpuDevice& instance = VirtGpuDevice::getInstance();

        uint64_t hostDeviceHandle = get_host_u64_VkDevice(device);

        exportSync.hdr.opCode = GFXSTREAM_CREATE_EXPORT_SYNC_VK;
        exportSync.deviceHandleLo = (uint32_t)hostDeviceHandle;
        exportSync.deviceHandleHi = (uint32_t)(hostDeviceHandle >> 32);
        exportSync.fenceHandleLo = (uint32_t)hostFenceHandle;
        exportSync.fenceHandleHi = (uint32_t)(hostFenceHandle >> 32);

        exec.command = static_cast<void*>(&exportSync);
        exec.command_size = sizeof(exportSync);
        exec.flags = kFenceOut | kRingIdx;
        if (instance.execBuffer(exec, nullptr))
            return VK_ERROR_OUT_OF_HOST_MEMORY;

        osHandle = exec.handle.osHandle;
        return VK_SUCCESS;
    }

    VkResult on_vkGetFenceFdKHR(
        void* context,
        VkResult,
        VkDevice device,
        const VkFenceGetFdInfoKHR* pGetFdInfo,
        int* pFd) {

        // export operation.
        // first check if fence is signaled
        // then if so, return -1
        // else, queue work

        VkEncoder* enc = (VkEncoder*)context;

        bool hasFence = pGetFdInfo->fence != VK_NULL_HANDLE;

        if (!hasFence) {
            ALOGV("%s: VK_ERROR_OUT_OF_HOST_MEMORY: no fence\n", __func__);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        bool syncFdExport =
            pGetFdInfo->handleType & VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

        if (!syncFdExport) {
            ALOGV("%s: VK_ERROR_OUT_OF_HOST_MEMORY: no sync fd fence\n", __func__);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        VkResult currentFenceStatus = enc->vkGetFenceStatus(device, pGetFdInfo->fence, true /* do lock */);

        if (VK_ERROR_DEVICE_LOST == currentFenceStatus) { // Other error
            ALOGV("%s: VK_ERROR_DEVICE_LOST: Other error\n", __func__);
            *pFd = -1;
            return VK_ERROR_DEVICE_LOST;
        }

        if (VK_NOT_READY == currentFenceStatus || VK_SUCCESS == currentFenceStatus) {
            // Fence is valid. We also create a new sync fd for a signaled
            // fence, because ANGLE will use the returned fd directly to
            // implement eglDupNativeFenceFDANDROID, where -1 is only returned
            // when error occurs.
            AutoLock<RecursiveLock> lock(mLock);

            auto it = info_VkFence.find(pGetFdInfo->fence);
            if (it == info_VkFence.end()) {
                ALOGV("%s: VK_ERROR_OUT_OF_HOST_MEMORY: no fence info\n", __func__);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }

            auto& info = it->second;

            bool syncFdCreated =
                info.external &&
                (info.exportFenceCreateInfo.handleTypes &
                 VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT);

            if (!syncFdCreated) {
                ALOGV("%s: VK_ERROR_OUT_OF_HOST_MEMORY: no sync fd created\n", __func__);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }

            if (mFeatureInfo->hasVirtioGpuNativeSync) {
                VkResult result;
                int64_t osHandle;
                uint64_t hostFenceHandle = get_host_u64_VkFence(pGetFdInfo->fence);

                result = createFence(device, hostFenceHandle, osHandle);
                if (result != VK_SUCCESS)
                    return result;

                *pFd = osHandle;
            } else {
                goldfish_sync_queue_work(
                    mSyncDeviceFd,
                    get_host_u64_VkFence(pGetFdInfo->fence) /* the handle */,
                    GOLDFISH_SYNC_VULKAN_SEMAPHORE_SYNC /* thread handle (doubling as type field) */,
                    pFd);
            }

            // relinquish ownership
            info.syncFd = -1;
            ALOGV("%s: got fd: %d\n", __func__, *pFd);
            return VK_SUCCESS;
        }
        return VK_ERROR_DEVICE_LOST;
#else
        return VK_ERROR_OUT_OF_HOST_MEMORY;
#endif
    }

    VkResult on_vkWaitForFences(
        void* context,
        VkResult,
        VkDevice device,
        uint32_t fenceCount,
        const VkFence* pFences,
        VkBool32 waitAll,
        uint64_t timeout) {

        VkEncoder* enc = (VkEncoder*)context;

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        std::vector<VkFence> fencesExternal;
        std::vector<int> fencesExternalWaitFds;
        std::vector<VkFence> fencesNonExternal;

        AutoLock<RecursiveLock> lock(mLock);

        for (uint32_t i = 0; i < fenceCount; ++i) {
            auto it = info_VkFence.find(pFences[i]);
            if (it == info_VkFence.end()) continue;
            const auto& info = it->second;
            if (info.syncFd >= 0) {
                fencesExternal.push_back(pFences[i]);
                fencesExternalWaitFds.push_back(info.syncFd);
            } else {
                fencesNonExternal.push_back(pFences[i]);
            }
        }

        lock.unlock();

        if (fencesExternal.empty()) {
            // No need for work pool, just wait with host driver.
            return enc->vkWaitForFences(
                device, fenceCount, pFences, waitAll, timeout, true /* do lock */);
        } else {
            // Depending on wait any or wait all,
            // schedule a wait group with waitAny/waitAll
            std::vector<WorkPool::Task> tasks;

            ALOGV("%s: scheduling ext waits\n", __func__);

            for (auto fd : fencesExternalWaitFds) {
                ALOGV("%s: wait on %d\n", __func__, fd);
                tasks.push_back([fd] {
                    sync_wait(fd, 3000);
                    ALOGV("done waiting on fd %d\n", fd);
                });
            }

            if (!fencesNonExternal.empty()) {
                tasks.push_back([this,
                                 fencesNonExternal /* copy of vector */,
                                 device, waitAll, timeout] {
                    auto hostConn = ResourceTracker::threadingCallbacks.hostConnectionGetFunc();
                    auto vkEncoder = ResourceTracker::threadingCallbacks.vkEncoderGetFunc(hostConn);
                    ALOGV("%s: vkWaitForFences to host\n", __func__);
                    vkEncoder->vkWaitForFences(device, fencesNonExternal.size(), fencesNonExternal.data(), waitAll, timeout, true /* do lock */);
                });
            }

            auto waitGroupHandle = mWorkPool.schedule(tasks);

            // Convert timeout to microseconds from nanoseconds
            bool waitRes = false;
            if (waitAll) {
                waitRes = mWorkPool.waitAll(waitGroupHandle, timeout / 1000);
            } else {
                waitRes = mWorkPool.waitAny(waitGroupHandle, timeout / 1000);
            }

            if (waitRes) {
                ALOGV("%s: VK_SUCCESS\n", __func__);
                return VK_SUCCESS;
            } else {
                ALOGV("%s: VK_TIMEOUT\n", __func__);
                return VK_TIMEOUT;
            }
        }
#else
        return enc->vkWaitForFences(
            device, fenceCount, pFences, waitAll, timeout, true /* do lock */);
#endif
    }

    VkResult on_vkCreateDescriptorPool(
        void* context,
        VkResult,
        VkDevice device,
        const VkDescriptorPoolCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorPool* pDescriptorPool) {

        VkEncoder* enc = (VkEncoder*)context;

        VkResult res = enc->vkCreateDescriptorPool(
            device, pCreateInfo, pAllocator, pDescriptorPool, true /* do lock */);

        if (res != VK_SUCCESS) return res;

        VkDescriptorPool pool = *pDescriptorPool;

        struct goldfish_VkDescriptorPool* dp = as_goldfish_VkDescriptorPool(pool);
        dp->allocInfo = new DescriptorPoolAllocationInfo;
        dp->allocInfo->device = device;
        dp->allocInfo->createFlags = pCreateInfo->flags;
        dp->allocInfo->maxSets = pCreateInfo->maxSets;
        dp->allocInfo->usedSets = 0;

        for (uint32_t i = 0; i < pCreateInfo->poolSizeCount; ++i) {
            dp->allocInfo->descriptorCountInfo.push_back({
                pCreateInfo->pPoolSizes[i].type,
                pCreateInfo->pPoolSizes[i].descriptorCount,
                0, /* used */
            });
        }

        if (mFeatureInfo->hasVulkanBatchedDescriptorSetUpdate) {
            std::vector<uint64_t> poolIds(pCreateInfo->maxSets);

            uint32_t count = pCreateInfo->maxSets;
            enc->vkCollectDescriptorPoolIdsGOOGLE(
                device, pool, &count, poolIds.data(), true /* do lock */);

            dp->allocInfo->freePoolIds = poolIds;
        }

        return res;
    }

    void on_vkDestroyDescriptorPool(
        void* context,
        VkDevice device,
        VkDescriptorPool descriptorPool,
        const VkAllocationCallbacks* pAllocator) {

        if (!descriptorPool) return;

        VkEncoder* enc = (VkEncoder*)context;

        clearDescriptorPoolAndUnregisterDescriptorSets(context, device, descriptorPool);

        enc->vkDestroyDescriptorPool(device, descriptorPool, pAllocator, true /* do lock */);
    }

    VkResult on_vkResetDescriptorPool(
        void* context,
        VkResult,
        VkDevice device,
        VkDescriptorPool descriptorPool,
        VkDescriptorPoolResetFlags flags) {

        if (!descriptorPool) return VK_ERROR_INITIALIZATION_FAILED;

        VkEncoder* enc = (VkEncoder*)context;

        VkResult res = enc->vkResetDescriptorPool(device, descriptorPool, flags, true /* do lock */);

        if (res != VK_SUCCESS) return res;

        clearDescriptorPoolAndUnregisterDescriptorSets(context, device, descriptorPool);
        return res;
    }

    VkResult on_vkAllocateDescriptorSets(
        void* context,
        VkResult,
        VkDevice device,
        const VkDescriptorSetAllocateInfo*          pAllocateInfo,
        VkDescriptorSet*                            pDescriptorSets) {

        VkEncoder* enc = (VkEncoder*)context;

        return allocAndInitializeDescriptorSets(context, device, pAllocateInfo, pDescriptorSets);
    }

    VkResult on_vkFreeDescriptorSets(
        void* context,
        VkResult,
        VkDevice                                    device,
        VkDescriptorPool                            descriptorPool,
        uint32_t                                    descriptorSetCount,
        const VkDescriptorSet*                      pDescriptorSets) {

        VkEncoder* enc = (VkEncoder*)context;

        // Bit of robustness so that we can double free descriptor sets
        // and do other invalid usages
        // https://github.com/KhronosGroup/Vulkan-Docs/issues/1070
        // (people expect VK_SUCCESS to always be returned by vkFreeDescriptorSets)
        std::vector<VkDescriptorSet> toActuallyFree;
        {
            AutoLock<RecursiveLock> lock(mLock);

            // Pool was destroyed
            if (info_VkDescriptorPool.find(descriptorPool) == info_VkDescriptorPool.end()) {
                return VK_SUCCESS;
            }

            if (!descriptorPoolSupportsIndividualFreeLocked(descriptorPool))
                return VK_SUCCESS;

            std::vector<VkDescriptorSet> existingDescriptorSets;;

            // Check if this descriptor set was in the pool's set of allocated descriptor sets,
            // to guard against double free (Double free is allowed by the client)
            {
                auto allocedSets = as_goldfish_VkDescriptorPool(descriptorPool)->allocInfo->allocedSets;

                for (uint32_t i = 0; i < descriptorSetCount; ++i) {

                    if (allocedSets.end() == allocedSets.find(pDescriptorSets[i])) {
                        ALOGV("%s: Warning: descriptor set %p not found in pool. Was this double-freed?\n", __func__,
                              (void*)pDescriptorSets[i]);
                        continue;
                    }

                    auto it = info_VkDescriptorSet.find(pDescriptorSets[i]);
                    if (it == info_VkDescriptorSet.end())
                        continue;

                    existingDescriptorSets.push_back(pDescriptorSets[i]);
                }
            }

            for (auto set : existingDescriptorSets) {
                if (removeDescriptorSetFromPool(set, mFeatureInfo->hasVulkanBatchedDescriptorSetUpdate)) {
                    toActuallyFree.push_back(set);
                }
            }

            if (toActuallyFree.empty()) return VK_SUCCESS;
        }

        if (mFeatureInfo->hasVulkanBatchedDescriptorSetUpdate) {
            // In the batched set update case, decrement refcount on the set layout
            // and only free on host if we satisfied a pending allocation on the
            // host.
            for (uint32_t i = 0; i < toActuallyFree.size(); ++i) {
                VkDescriptorSetLayout setLayout = as_goldfish_VkDescriptorSet(toActuallyFree[i])->reified->setLayout;
                decDescriptorSetLayoutRef(context, device, setLayout, nullptr);
            }
            freeDescriptorSetsIfHostAllocated(
                enc, device, (uint32_t)toActuallyFree.size(), toActuallyFree.data());
        } else {
            // In the non-batched set update case, just free them directly.
            enc->vkFreeDescriptorSets(device, descriptorPool, (uint32_t)toActuallyFree.size(), toActuallyFree.data(), true /* do lock */);
        }
        return VK_SUCCESS;
    }

    VkResult on_vkCreateDescriptorSetLayout(
        void* context,
        VkResult,
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorSetLayout* pSetLayout) {

        VkEncoder* enc = (VkEncoder*)context;

        VkResult res = enc->vkCreateDescriptorSetLayout(
            device, pCreateInfo, pAllocator, pSetLayout, true /* do lock */);

        if (res != VK_SUCCESS) return res;

        struct goldfish_VkDescriptorSetLayout* dsl =
            as_goldfish_VkDescriptorSetLayout(*pSetLayout);
        dsl->layoutInfo = new DescriptorSetLayoutInfo;
        for (uint32_t i = 0; i < pCreateInfo->bindingCount; ++i) {
            dsl->layoutInfo->bindings.push_back(pCreateInfo->pBindings[i]);
        }
        dsl->layoutInfo->refcount = 1;

        return res;
    }

    void on_vkUpdateDescriptorSets(
        void* context,
        VkDevice device,
        uint32_t descriptorWriteCount,
        const VkWriteDescriptorSet* pDescriptorWrites,
        uint32_t descriptorCopyCount,
        const VkCopyDescriptorSet* pDescriptorCopies) {

        VkEncoder* enc = (VkEncoder*)context;

        std::vector<VkDescriptorImageInfo> transformedImageInfos;
        std::vector<VkWriteDescriptorSet> transformedWrites(descriptorWriteCount);

        memcpy(transformedWrites.data(), pDescriptorWrites, sizeof(VkWriteDescriptorSet) * descriptorWriteCount);

        size_t imageInfosNeeded = 0;
        for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
            if (!isDescriptorTypeImageInfo(transformedWrites[i].descriptorType)) continue;
            if (!transformedWrites[i].pImageInfo) continue;

            imageInfosNeeded += transformedWrites[i].descriptorCount;
        }

        transformedImageInfos.resize(imageInfosNeeded);

        size_t imageInfoIndex = 0;
        for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
            if (!isDescriptorTypeImageInfo(transformedWrites[i].descriptorType)) continue;
            if (!transformedWrites[i].pImageInfo) continue;

            for (uint32_t j = 0; j < transformedWrites[i].descriptorCount; ++j) {
                transformedImageInfos[imageInfoIndex] = transformedWrites[i].pImageInfo[j];
                ++imageInfoIndex;
            }
            transformedWrites[i].pImageInfo = &transformedImageInfos[imageInfoIndex - transformedWrites[i].descriptorCount];
        }

        {
            // Validate and filter samplers
            AutoLock<RecursiveLock> lock(mLock);
            size_t imageInfoIndex = 0;
            for (uint32_t i = 0; i < descriptorWriteCount; ++i) {

                if (!isDescriptorTypeImageInfo(transformedWrites[i].descriptorType)) continue;
                if (!transformedWrites[i].pImageInfo) continue;

                bool isImmutableSampler =
                    descriptorBindingIsImmutableSampler(
                        transformedWrites[i].dstSet,
                        transformedWrites[i].dstBinding);

                for (uint32_t j = 0; j < transformedWrites[i].descriptorCount; ++j) {
                    if (isImmutableSampler) {
                        transformedImageInfos[imageInfoIndex].sampler = 0;
                    }
                    transformedImageInfos[imageInfoIndex] =
                        filterNonexistentSampler(transformedImageInfos[imageInfoIndex]);
                    ++imageInfoIndex;
                }
            }
        }

        if (mFeatureInfo->hasVulkanBatchedDescriptorSetUpdate) {
            for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
                VkDescriptorSet set = transformedWrites[i].dstSet;
                doEmulatedDescriptorWrite(&transformedWrites[i],
                        as_goldfish_VkDescriptorSet(set)->reified);
            }

            for (uint32_t i = 0; i < descriptorCopyCount; ++i) {
                doEmulatedDescriptorCopy(&pDescriptorCopies[i],
                        as_goldfish_VkDescriptorSet(pDescriptorCopies[i].srcSet)->reified,
                        as_goldfish_VkDescriptorSet(pDescriptorCopies[i].dstSet)->reified);
            }
        } else {
            enc->vkUpdateDescriptorSets(
                    device, descriptorWriteCount, transformedWrites.data(),
                    descriptorCopyCount, pDescriptorCopies, true /* do lock */);
        }
    }

    void on_vkDestroyImage(
        void* context,
        VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {

#ifdef VK_USE_PLATFORM_ANDROID_KHR
        {
          AutoLock<RecursiveLock> lock(mLock); // do not guard encoder may cause
                                               // deadlock b/243339973

          // Wait for any pending QSRIs to prevent a race between the Gfxstream host
          // potentially processing the below `vkDestroyImage()` from the VK encoder
          // command stream before processing a previously submitted
          // `VIRTIO_GPU_NATIVE_SYNC_VULKAN_QSRI_EXPORT` from the virtio-gpu command
          // stream which relies on the image existing.
          auto imageInfoIt = info_VkImage.find(image);
          if (imageInfoIt != info_VkImage.end()) {
            auto& imageInfo = imageInfoIt->second;
            for (int syncFd : imageInfo.pendingQsriSyncFds) {
                int syncWaitRet = sync_wait(syncFd, 3000);
                if (syncWaitRet < 0) {
                    ALOGE("%s: Failed to wait for pending QSRI sync: sterror: %s errno: %d",
                          __func__, strerror(errno), errno);
                }
                close(syncFd);
            }
            imageInfo.pendingQsriSyncFds.clear();
          }
        }
#endif
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkDestroyImage(device, image, pAllocator, true /* do lock */);
    }

    void setMemoryRequirementsForSysmemBackedImage(
        VkImage image, VkMemoryRequirements *pMemoryRequirements) {
#ifdef VK_USE_PLATFORM_FUCHSIA
        auto it = info_VkImage.find(image);
        if (it == info_VkImage.end()) return;
        auto& info = it->second;
        if (info.isSysmemBackedMemory) {
            auto width = info.createInfo.extent.width;
            auto height = info.createInfo.extent.height;
            pMemoryRequirements->size = width * height * 4;
        }
#else
        // Bypass "unused parameter" checks.
        (void)image;
        (void)pMemoryRequirements;
#endif
    }

    void on_vkGetImageMemoryRequirements(
        void *context, VkDevice device, VkImage image,
        VkMemoryRequirements *pMemoryRequirements) {

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkImage.find(image);
        if (it == info_VkImage.end()) return;

        auto& info = it->second;

        if (info.baseRequirementsKnown) {
            *pMemoryRequirements = info.baseRequirements;
            return;
        }

        lock.unlock();

        VkEncoder* enc = (VkEncoder*)context;

        enc->vkGetImageMemoryRequirements(
            device, image, pMemoryRequirements, true /* do lock */);

        lock.lock();

        transformImageMemoryRequirementsForGuestLocked(
            image, pMemoryRequirements);

        info.baseRequirementsKnown = true;
        info.baseRequirements = *pMemoryRequirements;
    }

    void on_vkGetImageMemoryRequirements2(
        void *context, VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
        VkMemoryRequirements2 *pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetImageMemoryRequirements2(
            device, pInfo, pMemoryRequirements, true /* do lock */);
        transformImageMemoryRequirements2ForGuest(
            pInfo->image, pMemoryRequirements);
    }

    void on_vkGetImageMemoryRequirements2KHR(
        void *context, VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
        VkMemoryRequirements2 *pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetImageMemoryRequirements2KHR(
            device, pInfo, pMemoryRequirements, true /* do lock */);
        transformImageMemoryRequirements2ForGuest(
            pInfo->image, pMemoryRequirements);
    }

    VkResult on_vkBindImageMemory(
        void* context, VkResult,
        VkDevice device, VkImage image, VkDeviceMemory memory,
        VkDeviceSize memoryOffset) {
        VkEncoder* enc = (VkEncoder*)context;
        // Do not forward calls with invalid handles to host.
        if (info_VkDeviceMemory.find(memory) == info_VkDeviceMemory.end() ||
            info_VkImage.find(image) == info_VkImage.end()) {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return enc->vkBindImageMemory(device, image, memory, memoryOffset, true /* do lock */);
    }

    VkResult on_vkBindImageMemory2(
        void* context, VkResult,
        VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
        VkEncoder* enc = (VkEncoder*)context;

        if (bindingCount < 1 || !pBindInfos) {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }

        for (uint32_t i = 0; i < bindingCount; i++) {
            const VkBindImageMemoryInfo& bimi = pBindInfos[i];

            auto imageIt = info_VkImage.find(bimi.image);
            if (imageIt == info_VkImage.end()) {
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }

            if (bimi.memory != VK_NULL_HANDLE) {
                auto memoryIt = info_VkDeviceMemory.find(bimi.memory);
                if (memoryIt == info_VkDeviceMemory.end()) {
                    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                }
            }
        }

        return enc->vkBindImageMemory2(device, bindingCount, pBindInfos, true /* do lock */);
    }

    VkResult on_vkBindImageMemory2KHR(
        void* context, VkResult result,
        VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
        return on_vkBindImageMemory2(context, result, device, bindingCount, pBindInfos);
    }

    VkResult on_vkCreateBuffer(
        void* context, VkResult,
        VkDevice device, const VkBufferCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkBuffer *pBuffer) {
        VkEncoder* enc = (VkEncoder*)context;

        VkBufferCreateInfo localCreateInfo = vk_make_orphan_copy(*pCreateInfo);
        vk_struct_chain_iterator structChainIter =
            vk_make_chain_iterator(&localCreateInfo);
        VkExternalMemoryBufferCreateInfo localExtBufCi;

        const VkExternalMemoryBufferCreateInfo* extBufCiPtr =
            vk_find_struct<VkExternalMemoryBufferCreateInfo>(pCreateInfo);
        if (extBufCiPtr) {
            localExtBufCi = vk_make_orphan_copy(*extBufCiPtr);
            vk_append_struct(&structChainIter, &localExtBufCi);
        }

#ifdef VK_USE_PLATFORM_FUCHSIA
        Optional<zx::vmo> vmo;
        bool isSysmemBackedMemory = false;

        if (extBufCiPtr &&
            (extBufCiPtr->handleTypes &
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_ZIRCON_VMO_BIT_FUCHSIA)) {
            isSysmemBackedMemory = true;
        }

        const auto* extBufferCollectionPtr =
            vk_find_struct<VkBufferCollectionBufferCreateInfoFUCHSIA>(
                pCreateInfo);

        if (extBufferCollectionPtr) {
            const auto& collection = *reinterpret_cast<
                fidl::WireSyncClient<fuchsia_sysmem::BufferCollection>*>(
                extBufferCollectionPtr->collection);
            uint32_t index = extBufferCollectionPtr->index;

            auto result = collection->WaitForBuffersAllocated();
            if (result.ok() && result->status == ZX_OK) {
                auto& info = result->buffer_collection_info;
                if (index < info.buffer_count) {
                    vmo = android::base::makeOptional(
                            std::move(info.buffers[index].vmo));
                }
            } else {
                ALOGE("WaitForBuffersAllocated failed: %d %d", result.status(),
                      GET_STATUS_SAFE(result, status));
            }

            if (vmo && vmo->is_valid()) {
                fidl::Arena arena;
                fuchsia_hardware_goldfish::wire::CreateBuffer2Params createParams(arena);
                createParams.set_size(arena, pCreateInfo->size)
                    .set_memory_property(
                        fuchsia_hardware_goldfish::wire::kMemoryPropertyDeviceLocal);

                auto result =
                    mControlDevice->CreateBuffer2(std::move(*vmo), createParams);
                if (!result.ok() ||
                    (result->is_error() != ZX_OK &&
                     result->error_value() != ZX_ERR_ALREADY_EXISTS)) {
                    ALOGE("CreateBuffer2 failed: %d:%d", result.status(),
                          GET_STATUS_SAFE(result, error_value()));
                }
                isSysmemBackedMemory = true;
            }
        }
#endif  // VK_USE_PLATFORM_FUCHSIA

        VkResult res;
        VkMemoryRequirements memReqs;

        if (supportsCreateResourcesWithRequirements()) {
            res = enc->vkCreateBufferWithRequirementsGOOGLE(
                device, &localCreateInfo, pAllocator, pBuffer, &memReqs,
                true /* do lock */);
        } else {
            res = enc->vkCreateBuffer(device, &localCreateInfo, pAllocator,
                                      pBuffer, true /* do lock */);
        }

        if (res != VK_SUCCESS) return res;

// Delete `protocolVersion` check goldfish drivers are gone.
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        if (mCaps.gfxstreamCapset.colorBufferMemoryIndex == 0xFFFFFFFF) {
            mCaps.gfxstreamCapset.colorBufferMemoryIndex =
                    getColorBufferMemoryIndex(context, device);
        }
        if (extBufCiPtr &&
            (extBufCiPtr->handleTypes &
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID)) {
            updateMemoryTypeBits(&memReqs.memoryTypeBits,
                                 mCaps.gfxstreamCapset.colorBufferMemoryIndex);
        }
#endif

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkBuffer.find(*pBuffer);
        if (it == info_VkBuffer.end()) return VK_ERROR_INITIALIZATION_FAILED;

        auto& info = it->second;

        info.createInfo = localCreateInfo;
        info.createInfo.pNext = nullptr;

        if (supportsCreateResourcesWithRequirements()) {
            info.baseRequirementsKnown = true;
            info.baseRequirements = memReqs;
        }

        if (extBufCiPtr) {
            info.external = true;
            info.externalCreateInfo = *extBufCiPtr;
        }

#ifdef VK_USE_PLATFORM_FUCHSIA
        if (isSysmemBackedMemory) {
            info.isSysmemBackedMemory = true;
        }
#endif

        return res;
    }

    void on_vkDestroyBuffer(
        void* context,
        VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkDestroyBuffer(device, buffer, pAllocator, true /* do lock */);
    }

    void on_vkGetBufferMemoryRequirements(
        void* context, VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements) {

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkBuffer.find(buffer);
        if (it == info_VkBuffer.end()) return;

        auto& info = it->second;

        if (info.baseRequirementsKnown) {
            *pMemoryRequirements = info.baseRequirements;
            return;
        }

        lock.unlock();

        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetBufferMemoryRequirements(
            device, buffer, pMemoryRequirements, true /* do lock */);

        lock.lock();

        info.baseRequirementsKnown = true;
        info.baseRequirements = *pMemoryRequirements;
    }

    void on_vkGetBufferMemoryRequirements2(
        void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements, true /* do lock */);
        transformBufferMemoryRequirements2ForGuest(
            pInfo->buffer, pMemoryRequirements);
    }

    void on_vkGetBufferMemoryRequirements2KHR(
        void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetBufferMemoryRequirements2KHR(device, pInfo, pMemoryRequirements, true /* do lock */);
        transformBufferMemoryRequirements2ForGuest(
            pInfo->buffer, pMemoryRequirements);
    }

    VkResult on_vkBindBufferMemory(
        void *context, VkResult,
        VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
        VkEncoder *enc = (VkEncoder *)context;
        return enc->vkBindBufferMemory(
            device, buffer, memory, memoryOffset, true /* do lock */);
    }

    VkResult on_vkBindBufferMemory2(
        void *context, VkResult,
        VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
        VkEncoder *enc = (VkEncoder *)context;
        return enc->vkBindBufferMemory2(
            device, bindInfoCount, pBindInfos, true /* do lock */);
    }

    VkResult on_vkBindBufferMemory2KHR(
        void *context, VkResult,
        VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
        VkEncoder *enc = (VkEncoder *)context;
        return enc->vkBindBufferMemory2KHR(
            device, bindInfoCount, pBindInfos, true /* do lock */);
    }

    void ensureSyncDeviceFd() {
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (mSyncDeviceFd >= 0)
            return;
        mSyncDeviceFd = goldfish_sync_open();
        if (mSyncDeviceFd >= 0) {
            ALOGD("%s: created sync device for current Vulkan process: %d\n", __func__, mSyncDeviceFd);
        } else {
            ALOGD("%s: failed to create sync device for current Vulkan process\n", __func__);
        }
#endif
    }

    VkResult on_vkCreateSemaphore(
        void* context, VkResult input_result,
        VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSemaphore* pSemaphore) {

        VkEncoder* enc = (VkEncoder*)context;

        VkSemaphoreCreateInfo finalCreateInfo = *pCreateInfo;

        const VkExportSemaphoreCreateInfoKHR* exportSemaphoreInfoPtr =
            vk_find_struct<VkExportSemaphoreCreateInfoKHR>(pCreateInfo);

#ifdef VK_USE_PLATFORM_FUCHSIA
        bool exportEvent =
                exportSemaphoreInfoPtr &&
                (exportSemaphoreInfoPtr->handleTypes &
                 VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_ZIRCON_EVENT_BIT_FUCHSIA);

        if (exportEvent) {
            finalCreateInfo.pNext = nullptr;
            // If we have timeline semaphores externally, leave it there.
            const VkSemaphoreTypeCreateInfo* typeCi =
                vk_find_struct<VkSemaphoreTypeCreateInfo>(pCreateInfo);
            if (typeCi) finalCreateInfo.pNext = typeCi;
        }
#endif

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        bool exportSyncFd = exportSemaphoreInfoPtr &&
            (exportSemaphoreInfoPtr->handleTypes &
             VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);

        if (exportSyncFd) {
            finalCreateInfo.pNext = nullptr;
            // If we have timeline semaphores externally, leave it there.
            const VkSemaphoreTypeCreateInfo* typeCi =
                vk_find_struct<VkSemaphoreTypeCreateInfo>(pCreateInfo);
            if (typeCi) finalCreateInfo.pNext = typeCi;
        }
#endif
        input_result = enc->vkCreateSemaphore(
            device, &finalCreateInfo, pAllocator, pSemaphore, true /* do lock */);

        zx_handle_t event_handle = ZX_HANDLE_INVALID;

#ifdef VK_USE_PLATFORM_FUCHSIA
        if (exportEvent) {
            zx_event_create(0, &event_handle);
        }
#endif

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkSemaphore.find(*pSemaphore);
        if (it == info_VkSemaphore.end()) return VK_ERROR_INITIALIZATION_FAILED;

        auto& info = it->second;

        info.device = device;
        info.eventHandle = event_handle;
#ifdef VK_USE_PLATFORM_FUCHSIA
        info.eventKoid = getEventKoid(info.eventHandle);
#endif

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (exportSyncFd) {
            if (mFeatureInfo->hasVirtioGpuNativeSync) {
                VkResult result;
                int64_t osHandle;
                uint64_t hostFenceHandle = get_host_u64_VkSemaphore(*pSemaphore);

                result = createFence(device, hostFenceHandle, osHandle);
                if (result != VK_SUCCESS)
                    return result;

                info.syncFd.emplace(osHandle);
            } else {
                ensureSyncDeviceFd();

                if (exportSyncFd) {
                    int syncFd = -1;
                    goldfish_sync_queue_work(
                            mSyncDeviceFd,
                            get_host_u64_VkSemaphore(*pSemaphore) /* the handle */,
                            GOLDFISH_SYNC_VULKAN_SEMAPHORE_SYNC /* thread handle (doubling as type field) */,
                            &syncFd);
                    info.syncFd.emplace(syncFd);
                }
            }
        }
#endif

        return VK_SUCCESS;
    }

    void on_vkDestroySemaphore(
        void* context,
        VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkDestroySemaphore(device, semaphore, pAllocator, true /* do lock */);
    }

    // https://www.khronos.org/registry/vulkan/specs/1.0-extensions/html/vkspec.html#vkGetSemaphoreFdKHR
    // Each call to vkGetSemaphoreFdKHR must create a new file descriptor and transfer ownership
    // of it to the application. To avoid leaking resources, the application must release ownership
    // of the file descriptor when it is no longer needed.
    VkResult on_vkGetSemaphoreFdKHR(
        void* context, VkResult,
        VkDevice device, const VkSemaphoreGetFdInfoKHR* pGetFdInfo,
        int* pFd) {
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        VkEncoder* enc = (VkEncoder*)context;
        bool getSyncFd =
            pGetFdInfo->handleType & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

        if (getSyncFd) {
            AutoLock<RecursiveLock> lock(mLock);
            auto it = info_VkSemaphore.find(pGetFdInfo->semaphore);
            if (it == info_VkSemaphore.end()) return VK_ERROR_OUT_OF_HOST_MEMORY;
            auto& semInfo = it->second;
            // syncFd is supposed to have value.
            *pFd = dup(semInfo.syncFd.value_or(-1));
            return VK_SUCCESS;
        } else {
            // opaque fd
            int hostFd = 0;
            VkResult result = enc->vkGetSemaphoreFdKHR(device, pGetFdInfo, &hostFd, true /* do lock */);
            if (result != VK_SUCCESS) {
                return result;
            }
            *pFd = memfd_create("vk_opaque_fd", 0);
            write(*pFd, &hostFd, sizeof(hostFd));
            return VK_SUCCESS;
        }
#else
        (void)context;
        (void)device;
        (void)pGetFdInfo;
        (void)pFd;
        return VK_ERROR_INCOMPATIBLE_DRIVER;
#endif
    }

    VkResult on_vkImportSemaphoreFdKHR(
        void* context, VkResult input_result,
        VkDevice device,
        const VkImportSemaphoreFdInfoKHR* pImportSemaphoreFdInfo) {
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        VkEncoder* enc = (VkEncoder*)context;
        if (input_result != VK_SUCCESS) {
            return input_result;
        }

        if (pImportSemaphoreFdInfo->handleType &
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) {
            VkImportSemaphoreFdInfoKHR tmpInfo = *pImportSemaphoreFdInfo;

            AutoLock<RecursiveLock> lock(mLock);

            auto semaphoreIt = info_VkSemaphore.find(pImportSemaphoreFdInfo->semaphore);
            auto& info = semaphoreIt->second;

            if (info.syncFd.value_or(-1) >= 0) {
                close(info.syncFd.value());
            }

            info.syncFd.emplace(pImportSemaphoreFdInfo->fd);

            return VK_SUCCESS;
        } else {
            int fd = pImportSemaphoreFdInfo->fd;
            int err = lseek(fd, 0, SEEK_SET);
            if (err == -1) {
                ALOGE("lseek fail on import semaphore");
            }
            int hostFd = 0;
            read(fd, &hostFd, sizeof(hostFd));
            VkImportSemaphoreFdInfoKHR tmpInfo = *pImportSemaphoreFdInfo;
            tmpInfo.fd = hostFd;
            VkResult result = enc->vkImportSemaphoreFdKHR(device, &tmpInfo, true /* do lock */);
            close(fd);
            return result;
        }
#else
        (void)context;
        (void)input_result;
        (void)device;
        (void)pImportSemaphoreFdInfo;
        return VK_ERROR_INCOMPATIBLE_DRIVER;
#endif
    }

    struct CommandBufferPendingDescriptorSets {
        std::unordered_set<VkDescriptorSet> sets;
    };

    void collectAllPendingDescriptorSetsBottomUp(const std::vector<VkCommandBuffer>& workingSet, std::unordered_set<VkDescriptorSet>& allDs) {
        if (workingSet.empty()) return;

        std::vector<VkCommandBuffer> nextLevel;
        for (auto commandBuffer : workingSet) {
            struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
            forAllObjects(cb->subObjects, [&nextLevel](void* secondary) {
                    nextLevel.push_back((VkCommandBuffer)secondary);
                    });
        }

        collectAllPendingDescriptorSetsBottomUp(nextLevel, allDs);

        for (auto cmdbuf : workingSet) {
            struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(cmdbuf);

            if (!cb->userPtr) {
                continue; // No descriptors to update.
            }

            CommandBufferPendingDescriptorSets* pendingDescriptorSets =
                (CommandBufferPendingDescriptorSets*)(cb->userPtr);

            if (pendingDescriptorSets->sets.empty()) {
                continue; // No descriptors to update.
            }

            allDs.insert(pendingDescriptorSets->sets.begin(), pendingDescriptorSets->sets.end());
        }
    }

    void commitDescriptorSetUpdates(void* context, VkQueue queue, const std::unordered_set<VkDescriptorSet>& sets) {
        VkEncoder* enc = (VkEncoder*)context;

        std::unordered_map<VkDescriptorPool, uint32_t> poolSet;
        std::vector<VkDescriptorPool> pools;
        std::vector<VkDescriptorSetLayout> setLayouts;
        std::vector<uint64_t> poolIds;
        std::vector<uint32_t> descriptorSetWhichPool;
        std::vector<uint32_t> pendingAllocations;
        std::vector<uint32_t> writeStartingIndices;
        std::vector<VkWriteDescriptorSet> writesForHost;

        uint32_t poolIndex = 0;
        uint32_t currentWriteIndex = 0;
        for (auto set : sets) {
            ReifiedDescriptorSet* reified = as_goldfish_VkDescriptorSet(set)->reified;
            VkDescriptorPool pool = reified->pool;
            VkDescriptorSetLayout setLayout = reified->setLayout;

            auto it = poolSet.find(pool);
            if (it == poolSet.end()) {
                poolSet[pool] = poolIndex;
                descriptorSetWhichPool.push_back(poolIndex);
                pools.push_back(pool);
                ++poolIndex;
            } else {
                uint32_t savedPoolIndex = it->second;
                descriptorSetWhichPool.push_back(savedPoolIndex);
            }

            poolIds.push_back(reified->poolId);
            setLayouts.push_back(setLayout);
            pendingAllocations.push_back(reified->allocationPending ? 1 : 0);
            writeStartingIndices.push_back(currentWriteIndex);

            auto& writes = reified->allWrites;

            for (size_t i = 0; i < writes.size(); ++i) {
                uint32_t binding = i;

                for (size_t j = 0; j < writes[i].size(); ++j) {
                    auto& write = writes[i][j];

                    if (write.type == DescriptorWriteType::Empty) continue;

                    uint32_t dstArrayElement = 0;

                    VkDescriptorImageInfo* imageInfo = nullptr;
                    VkDescriptorBufferInfo* bufferInfo = nullptr;
                    VkBufferView* bufferView = nullptr;

                    switch (write.type) {
                        case DescriptorWriteType::Empty:
                            break;
                        case DescriptorWriteType::ImageInfo:
                            dstArrayElement = j;
                            imageInfo = &write.imageInfo;
                            break;
                        case DescriptorWriteType::BufferInfo:
                            dstArrayElement = j;
                            bufferInfo = &write.bufferInfo;
                            break;
                        case DescriptorWriteType::BufferView:
                            dstArrayElement = j;
                            bufferView = &write.bufferView;
                            break;
                        case DescriptorWriteType::InlineUniformBlock:
                        case DescriptorWriteType::AccelerationStructure:
                            // TODO
                            ALOGE("Encountered pending inline uniform block or acceleration structure desc write, abort (NYI)\n");
                            abort();
                        default:
                            break;

                    }

                    // TODO: Combine multiple writes into one VkWriteDescriptorSet.
                    VkWriteDescriptorSet forHost = {
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0 /* TODO: inline uniform block */,
                        set,
                        binding,
                        dstArrayElement,
                        1,
                        write.descriptorType,
                        imageInfo,
                        bufferInfo,
                        bufferView,
                    };

                    writesForHost.push_back(forHost);
                    ++currentWriteIndex;

                    // Set it back to empty.
                    write.type = DescriptorWriteType::Empty;
                }
            }
        }

        // Skip out if there's nothing to VkWriteDescriptorSet home about.
        if (writesForHost.empty()) {
            return;
        }

        enc->vkQueueCommitDescriptorSetUpdatesGOOGLE(
            queue,
            (uint32_t)pools.size(), pools.data(),
            (uint32_t)sets.size(),
            setLayouts.data(),
            poolIds.data(),
            descriptorSetWhichPool.data(),
            pendingAllocations.data(),
            writeStartingIndices.data(),
            (uint32_t)writesForHost.size(),
            writesForHost.data(),
            false /* no lock */);

        // If we got here, then we definitely serviced the allocations.
        for (auto set : sets) {
            ReifiedDescriptorSet* reified = as_goldfish_VkDescriptorSet(set)->reified;
            reified->allocationPending = false;
        }
    }

    void flushCommandBufferPendingCommandsBottomUp(void* context, VkQueue queue, const std::vector<VkCommandBuffer>& workingSet) {
        if (workingSet.empty()) return;

        std::vector<VkCommandBuffer> nextLevel;
        for (auto commandBuffer : workingSet) {
            struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
            forAllObjects(cb->subObjects, [&nextLevel](void* secondary) {
                nextLevel.push_back((VkCommandBuffer)secondary);
            });
        }

        flushCommandBufferPendingCommandsBottomUp(context, queue, nextLevel);

        // After this point, everyone at the previous level has been flushed
        for (auto cmdbuf : workingSet) {
            struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(cmdbuf);

            // There's no pending commands here, skip. (case 1)
            if (!cb->privateStream) continue;

            unsigned char* writtenPtr = 0;
            size_t written = 0;
            CommandBufferStagingStream* cmdBufStream =
                static_cast<CommandBufferStagingStream*>(cb->privateStream);
            cmdBufStream->getWritten(&writtenPtr, &written);

            // There's no pending commands here, skip. (case 2, stream created but no new recordings)
            if (!written) continue;

            // There are pending commands to flush.
            VkEncoder* enc = (VkEncoder*)context;
            VkDeviceMemory deviceMemory = cmdBufStream->getDeviceMemory();
            VkDeviceSize dataOffset = 0;
            if (mFeatureInfo->hasVulkanAuxCommandMemory) {
                // for suballocations, deviceMemory is an alias VkDeviceMemory
                // get underling VkDeviceMemory for given alias
                deviceMemoryTransform_tohost(&deviceMemory, 1 /*memoryCount*/, &dataOffset,
                                             1 /*offsetCount*/, nullptr /*size*/, 0 /*sizeCount*/,
                                             nullptr /*typeIndex*/, 0 /*typeIndexCount*/,
                                             nullptr /*typeBits*/, 0 /*typeBitCounts*/);

                // mark stream as flushing before flushing commands
                cmdBufStream->markFlushing();
                enc->vkQueueFlushCommandsFromAuxMemoryGOOGLE(queue, cmdbuf, deviceMemory,
                                                             dataOffset, written, true /*do lock*/);
            } else {
                enc->vkQueueFlushCommandsGOOGLE(queue, cmdbuf, written, (const void*)writtenPtr,
                                                true /* do lock */);
            }
            // Reset this stream.
            // flushing happens on vkQueueSubmit
            // vulkan api states that on queue submit,
            // applications MUST not attempt to modify the command buffer in any way
            // -as the device may be processing the commands recorded to it.
            // It is safe to call reset() here for this reason.
            // Command Buffer associated with this stream will only leave pending state
            // after queue submit is complete and host has read the data
            cmdBufStream->reset();
        }
    }

    // Unlike resetCommandBufferStagingInfo, this does not always erase its
    // superObjects pointers because the command buffer has merely been
    // submitted, not reset.  However, if the command buffer was recorded with
    // ONE_TIME_SUBMIT_BIT, then it will also reset its primaries.
    //
    // Also, we save the set of descriptor sets referenced by this command
    // buffer because we only submitted the command buffer and it's possible to
    // update the descriptor set again and re-submit the same command without
    // recording it (Update-after-bind descriptor sets)
    void resetCommandBufferPendingTopology(VkCommandBuffer commandBuffer) {
        struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
        if (cb->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) {
            resetCommandBufferStagingInfo(commandBuffer,
                true /* reset primaries */,
                true /* clear pending descriptor sets */);
        } else {
            resetCommandBufferStagingInfo(commandBuffer,
                false /* Don't reset primaries */,
                false /* Don't clear pending descriptor sets */);
        }
    }

    uint32_t getWaitSemaphoreCount(const VkSubmitInfo& pSubmit) {
        return pSubmit.waitSemaphoreCount;
    }

    uint32_t getWaitSemaphoreCount(const VkSubmitInfo2& pSubmit) {
        return pSubmit.waitSemaphoreInfoCount;
    }

    uint32_t getCommandBufferCount(const VkSubmitInfo& pSubmit) {
        return pSubmit.commandBufferCount;
    }

    uint32_t getCommandBufferCount(const VkSubmitInfo2& pSubmit) {
        return pSubmit.commandBufferInfoCount;
    }

    uint32_t getSignalSemaphoreCount(const VkSubmitInfo& pSubmit) {
        return pSubmit.signalSemaphoreCount;
    }

    uint32_t getSignalSemaphoreCount(const VkSubmitInfo2& pSubmit) {
        return pSubmit.signalSemaphoreInfoCount;
    }

    VkSemaphore getWaitSemaphore(const VkSubmitInfo& pSubmit, int i) {
        return pSubmit.pWaitSemaphores[i];
    }

    VkSemaphore getWaitSemaphore(const VkSubmitInfo2& pSubmit, int i) {
        return pSubmit.pWaitSemaphoreInfos[i].semaphore;
    }

    VkSemaphore getSignalSemaphore(const VkSubmitInfo& pSubmit, int i) {
        return pSubmit.pSignalSemaphores[i];
    }

    VkSemaphore getSignalSemaphore(const VkSubmitInfo2& pSubmit, int i) {
        return pSubmit.pSignalSemaphoreInfos[i].semaphore;
    }

    VkCommandBuffer getCommandBuffer(const VkSubmitInfo& pSubmit, int i) {
        return pSubmit.pCommandBuffers[i];
    }

    VkCommandBuffer getCommandBuffer(const VkSubmitInfo2& pSubmit, int i) {
        return pSubmit.pCommandBufferInfos[i].commandBuffer;
    }

    template <class VkSubmitInfoType>
    void flushStagingStreams(void* context, VkQueue queue, uint32_t submitCount,
                             const VkSubmitInfoType* pSubmits) {
        std::vector<VkCommandBuffer> toFlush;
        for (uint32_t i = 0; i < submitCount; ++i) {
            for (uint32_t j = 0; j < getCommandBufferCount(pSubmits[i]); ++j) {
                toFlush.push_back(getCommandBuffer(pSubmits[i], j));
            }
        }

        std::unordered_set<VkDescriptorSet> pendingSets;
        collectAllPendingDescriptorSetsBottomUp(toFlush, pendingSets);
        commitDescriptorSetUpdates(context, queue, pendingSets);

        flushCommandBufferPendingCommandsBottomUp(context, queue, toFlush);

        for (auto cb : toFlush) {
            resetCommandBufferPendingTopology(cb);
        }
    }

    VkResult on_vkQueueSubmit(
        void* context, VkResult input_result,
        VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
        AEMU_SCOPED_TRACE("on_vkQueueSubmit");
        return on_vkQueueSubmitTemplate<VkSubmitInfo>(context, input_result, queue, submitCount,
                                                      pSubmits, fence);
    }

    VkResult on_vkQueueSubmit2(void* context, VkResult input_result, VkQueue queue,
                               uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
        AEMU_SCOPED_TRACE("on_vkQueueSubmit2");
        return on_vkQueueSubmitTemplate<VkSubmitInfo2>(context, input_result, queue, submitCount,
                                                       pSubmits, fence);
    }

    VkResult vkQueueSubmitEnc(VkEncoder* enc, VkQueue queue, uint32_t submitCount,
                              const VkSubmitInfo* pSubmits, VkFence fence) {
        if (supportsAsyncQueueSubmit()) {
            enc->vkQueueSubmitAsyncGOOGLE(queue, submitCount, pSubmits, fence, true /* do lock */);
            return VK_SUCCESS;
        } else {
            return enc->vkQueueSubmit(queue, submitCount, pSubmits, fence, true /* do lock */);
        }
    }

    VkResult vkQueueSubmitEnc(VkEncoder* enc, VkQueue queue, uint32_t submitCount,
                              const VkSubmitInfo2* pSubmits, VkFence fence) {
        if (supportsAsyncQueueSubmit()) {
            enc->vkQueueSubmitAsync2GOOGLE(queue, submitCount, pSubmits, fence, true /* do lock */);
            return VK_SUCCESS;
        } else {
            return enc->vkQueueSubmit2(queue, submitCount, pSubmits, fence, true /* do lock */);
        }
    }

    template <typename VkSubmitInfoType>
    VkResult on_vkQueueSubmitTemplate(void* context, VkResult input_result, VkQueue queue,
                                      uint32_t submitCount, const VkSubmitInfoType* pSubmits,
                                      VkFence fence) {
        flushStagingStreams(context, queue, submitCount, pSubmits);

        std::vector<VkSemaphore> pre_signal_semaphores;
        std::vector<zx_handle_t> pre_signal_events;
        std::vector<int> pre_signal_sync_fds;
        std::vector<std::pair<zx_handle_t, zx_koid_t>> post_wait_events;
        std::vector<int> post_wait_sync_fds;

        VkEncoder* enc = (VkEncoder*)context;

        AutoLock<RecursiveLock> lock(mLock);

        for (uint32_t i = 0; i < submitCount; ++i) {
            for (uint32_t j = 0; j < getWaitSemaphoreCount(pSubmits[i]); ++j) {
                VkSemaphore semaphore = getWaitSemaphore(pSubmits[i], j);
                auto it = info_VkSemaphore.find(semaphore);
                if (it != info_VkSemaphore.end()) {
                    auto& semInfo = it->second;
#ifdef VK_USE_PLATFORM_FUCHSIA
                    if (semInfo.eventHandle) {
                        pre_signal_events.push_back(semInfo.eventHandle);
                        pre_signal_semaphores.push_back(semaphore);
                    }
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
                    if (semInfo.syncFd.has_value()) {
                        pre_signal_sync_fds.push_back(semInfo.syncFd.value());
                        pre_signal_semaphores.push_back(semaphore);
                    }
#endif
                }
            }
            for (uint32_t j = 0; j < getSignalSemaphoreCount(pSubmits[i]); ++j) {
                auto it = info_VkSemaphore.find(getSignalSemaphore(pSubmits[i], j));
                if (it != info_VkSemaphore.end()) {
                    auto& semInfo = it->second;
#ifdef VK_USE_PLATFORM_FUCHSIA
                    if (semInfo.eventHandle) {
                        post_wait_events.push_back(
                            {semInfo.eventHandle, semInfo.eventKoid});
#ifndef FUCHSIA_NO_TRACE
                        if (semInfo.eventKoid != ZX_KOID_INVALID) {
                            // TODO(fxbug.dev/66098): Remove the "semaphore"
                            // FLOW_END events once it is removed from clients
                            // (for example, gfx Engine).
                            TRACE_FLOW_END("gfx", "semaphore",
                                           semInfo.eventKoid);
                            TRACE_FLOW_BEGIN("gfx", "goldfish_post_wait_event",
                                             semInfo.eventKoid);
                        }
#endif
                    }
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
                    if (semInfo.syncFd.value_or(-1) >= 0) {
                        post_wait_sync_fds.push_back(semInfo.syncFd.value());
                    }
#endif
                }
            }
        }
        lock.unlock();

        if (pre_signal_semaphores.empty()) {
            input_result = vkQueueSubmitEnc(enc, queue, submitCount, pSubmits, fence);
            if (input_result != VK_SUCCESS) return input_result;
        } else {
            // Schedule waits on the OS external objects and
            // signal the wait semaphores
            // in a separate thread.
            std::vector<WorkPool::Task> preSignalTasks;
            std::vector<WorkPool::Task> preSignalQueueSubmitTasks;;
#ifdef VK_USE_PLATFORM_FUCHSIA
            for (auto event : pre_signal_events) {
                preSignalTasks.push_back([event] {
                    zx_object_wait_one(
                        event,
                        ZX_EVENT_SIGNALED,
                        ZX_TIME_INFINITE,
                        nullptr);
                });
            }
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
            for (auto fd : pre_signal_sync_fds) {
                // https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkImportSemaphoreFdInfoKHR.html
                // fd == -1 is treated as already signaled
                if (fd != -1) {
                    preSignalTasks.push_back([fd] {
                        sync_wait(fd, 3000);
                    });
                }
            }
#endif
            if (!preSignalTasks.empty()) {
                auto waitGroupHandle = mWorkPool.schedule(preSignalTasks);
                mWorkPool.waitAll(waitGroupHandle);
            }

            // Use the old version of VkSubmitInfo
            VkSubmitInfo submit_info = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = 0,
                .pWaitSemaphores = nullptr,
                .pWaitDstStageMask = nullptr,
                .signalSemaphoreCount =
                    static_cast<uint32_t>(pre_signal_semaphores.size()),
                .pSignalSemaphores = pre_signal_semaphores.data()};
            vkQueueSubmitEnc(enc, queue, 1, &submit_info, VK_NULL_HANDLE);
            input_result = vkQueueSubmitEnc(enc, queue, submitCount, pSubmits, fence);
            if (input_result != VK_SUCCESS) return input_result;
        }
        lock.lock();
        int externalFenceFdToSignal = -1;

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (fence != VK_NULL_HANDLE) {
            auto it = info_VkFence.find(fence);
            if (it != info_VkFence.end()) {
                const auto& info = it->second;
                if (info.syncFd >= 0) {
                    externalFenceFdToSignal = info.syncFd;
                }
            }
        }
#endif
        if (externalFenceFdToSignal >= 0 ||
            !post_wait_events.empty() ||
            !post_wait_sync_fds.empty()) {

            std::vector<WorkPool::Task> tasks;

            tasks.push_back([queue, externalFenceFdToSignal,
                             post_wait_events /* copy of zx handles */,
                             post_wait_sync_fds /* copy of sync fds */] {
                auto hostConn = ResourceTracker::threadingCallbacks.hostConnectionGetFunc();
                auto vkEncoder = ResourceTracker::threadingCallbacks.vkEncoderGetFunc(hostConn);
                auto waitIdleRes = vkEncoder->vkQueueWaitIdle(queue, true /* do lock */);
#ifdef VK_USE_PLATFORM_FUCHSIA
                AEMU_SCOPED_TRACE("on_vkQueueSubmit::SignalSemaphores");
                (void)externalFenceFdToSignal;
                for (auto& [event, koid] : post_wait_events) {
#ifndef FUCHSIA_NO_TRACE
                    if (koid != ZX_KOID_INVALID) {
                        TRACE_FLOW_END("gfx", "goldfish_post_wait_event", koid);
                        TRACE_FLOW_BEGIN("gfx", "event_signal", koid);
                    }
#endif
                    zx_object_signal(event, 0, ZX_EVENT_SIGNALED);
                }
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
                for (auto& fd : post_wait_sync_fds) {
                    goldfish_sync_signal(fd);
                }

                if (externalFenceFdToSignal >= 0) {
                    ALOGV("%s: external fence real signal: %d\n", __func__, externalFenceFdToSignal);
                    goldfish_sync_signal(externalFenceFdToSignal);
                }
#endif
            });
            auto queueAsyncWaitHandle = mWorkPool.schedule(tasks);
            auto& queueWorkItems = mQueueSensitiveWorkPoolItems[queue];
            queueWorkItems.push_back(queueAsyncWaitHandle);
        }
        return VK_SUCCESS;
    }

    VkResult on_vkQueueWaitIdle(
        void* context, VkResult,
        VkQueue queue) {

        VkEncoder* enc = (VkEncoder*)context;

        AutoLock<RecursiveLock> lock(mLock);
        std::vector<WorkPool::WaitGroupHandle> toWait =
            mQueueSensitiveWorkPoolItems[queue];
        mQueueSensitiveWorkPoolItems[queue].clear();
        lock.unlock();

        if (toWait.empty()) {
            ALOGV("%s: No queue-specific work pool items\n", __func__);
            return enc->vkQueueWaitIdle(queue, true /* do lock */);
        }

        for (auto handle : toWait) {
            ALOGV("%s: waiting on work group item: %llu\n", __func__,
                  (unsigned long long)handle);
            mWorkPool.waitAll(handle);
        }

        // now done waiting, get the host's opinion
        return enc->vkQueueWaitIdle(queue, true /* do lock */);
    }

#ifdef VK_USE_PLATFORM_ANDROID_KHR
    void unwrap_VkNativeBufferANDROID(
        const VkNativeBufferANDROID* inputNativeInfo,
        VkNativeBufferANDROID* outputNativeInfo) {

        if (!inputNativeInfo || !inputNativeInfo->handle) {
            return;
        }

        if (!outputNativeInfo || !outputNativeInfo) {
            ALOGE("FATAL: Local native buffer info not properly allocated!");
            abort();
        }

        auto* gralloc = ResourceTracker::threadingCallbacks.hostConnectionGetFunc()->grallocHelper();

        *(uint32_t*)(outputNativeInfo->handle) =
            gralloc->getHostHandle((const native_handle_t*)inputNativeInfo->handle);
    }

    void unwrap_vkCreateImage_pCreateInfo(
        const VkImageCreateInfo* pCreateInfo,
        VkImageCreateInfo* local_pCreateInfo) {

        const VkNativeBufferANDROID* inputNativeInfo =
            vk_find_struct<VkNativeBufferANDROID>(pCreateInfo);

        VkNativeBufferANDROID* outputNativeInfo =
            const_cast<VkNativeBufferANDROID*>(
                vk_find_struct<VkNativeBufferANDROID>(local_pCreateInfo));

        unwrap_VkNativeBufferANDROID(inputNativeInfo, outputNativeInfo);
    }

    void unwrap_vkAcquireImageANDROID_nativeFenceFd(int fd, int*) {
        if (fd != -1) {
            AEMU_SCOPED_TRACE("waitNativeFenceInAcquire");
            // Implicit Synchronization
            sync_wait(fd, 3000);
            // From libvulkan's swapchain.cpp:
            // """
            // NOTE: we're relying on AcquireImageANDROID to close fence_clone,
            // even if the call fails. We could close it ourselves on failure, but
            // that would create a race condition if the driver closes it on a
            // failure path: some other thread might create an fd with the same
            // number between the time the driver closes it and the time we close
            // it. We must assume one of: the driver *always* closes it even on
            // failure, or *never* closes it on failure.
            // """
            // Therefore, assume contract where we need to close fd in this driver
            close(fd);
        }
    }

    void unwrap_VkBindImageMemorySwapchainInfoKHR(
        const VkBindImageMemorySwapchainInfoKHR* inputBimsi,
        VkBindImageMemorySwapchainInfoKHR* outputBimsi) {
        if (!inputBimsi || !inputBimsi->swapchain) {
            return;
        }

        if (!outputBimsi || !outputBimsi->swapchain) {
            ALOGE("FATAL: Local VkBindImageMemorySwapchainInfoKHR not properly allocated!");
            abort();
        }

        // Android based swapchains are implemented by the Android framework's
        // libvulkan. The only exist within the guest and should not be sent to
        // the host.
        outputBimsi->swapchain = VK_NULL_HANDLE;
    }

    void unwrap_VkBindImageMemory2_pBindInfos(
            uint32_t bindInfoCount,
            const VkBindImageMemoryInfo* inputBindInfos,
            VkBindImageMemoryInfo* outputBindInfos) {
        for (uint32_t i = 0; i < bindInfoCount; ++i) {
            const VkBindImageMemoryInfo* inputBindInfo = &inputBindInfos[i];
            VkBindImageMemoryInfo* outputBindInfo = &outputBindInfos[i];

            const VkNativeBufferANDROID* inputNativeInfo =
                vk_find_struct<VkNativeBufferANDROID>(inputBindInfo);

            VkNativeBufferANDROID* outputNativeInfo =
                const_cast<VkNativeBufferANDROID*>(
                    vk_find_struct<VkNativeBufferANDROID>(outputBindInfo));

            unwrap_VkNativeBufferANDROID(inputNativeInfo, outputNativeInfo);

            const VkBindImageMemorySwapchainInfoKHR* inputBimsi =
                vk_find_struct<VkBindImageMemorySwapchainInfoKHR>(inputBindInfo);

            VkBindImageMemorySwapchainInfoKHR* outputBimsi =
                const_cast<VkBindImageMemorySwapchainInfoKHR*>(
                    vk_find_struct<VkBindImageMemorySwapchainInfoKHR>(outputBindInfo));

            unwrap_VkBindImageMemorySwapchainInfoKHR(inputBimsi, outputBimsi);
        }
    }
#endif

    // Action of vkMapMemoryIntoAddressSpaceGOOGLE:
    // 1. preprocess (on_vkMapMemoryIntoAddressSpaceGOOGLE_pre):
    //    uses address space device to reserve the right size of
    //    memory.
    // 2. the reservation results in a physical address. the physical
    //    address is set as |*pAddress|.
    // 3. after pre, the API call is encoded to the host, where the
    //    value of pAddress is also sent (the physical address).
    // 4. the host will obtain the actual gpu pointer and send it
    //    back out in |*pAddress|.
    // 5. postprocess (on_vkMapMemoryIntoAddressSpaceGOOGLE) will run,
    //    using the mmap() method of GoldfishAddressSpaceBlock to obtain
    //    a pointer in guest userspace corresponding to the host pointer.
    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
        void*,
        VkResult,
        VkDevice,
        VkDeviceMemory memory,
        uint64_t* pAddress) {

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        auto& memInfo = it->second;

        GoldfishAddressSpaceBlockPtr block = std::make_shared<GoldfishAddressSpaceBlock>();
        block->allocate(mGoldfishAddressSpaceBlockProvider.get(), memInfo.coherentMemorySize);

        memInfo.goldfishBlock = block;
        *pAddress = block->physAddr();

        return VK_SUCCESS;
    }

    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE(
        void*,
        VkResult input_result,
        VkDevice,
        VkDeviceMemory memory,
        uint64_t* pAddress) {
        (void)memory;
	(void)pAddress;

        if (input_result != VK_SUCCESS) {
            return input_result;
        }

        return input_result;
    }

    VkResult initDescriptorUpdateTemplateBuffers(
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate) {

        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkDescriptorUpdateTemplate.find(descriptorUpdateTemplate);
        if (it == info_VkDescriptorUpdateTemplate.end()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto& info = it->second;
        uint32_t inlineUniformBlockBufferSize = 0;

        for (uint32_t i = 0; i < pCreateInfo->descriptorUpdateEntryCount; ++i) {
            const auto& entry = pCreateInfo->pDescriptorUpdateEntries[i];
            uint32_t descCount = entry.descriptorCount;
            VkDescriptorType descType = entry.descriptorType;
            ++info.templateEntryCount;
            if (isDescriptorTypeInlineUniformBlock(descType)) {
                inlineUniformBlockBufferSize += descCount;
                ++info.inlineUniformBlockCount;
            } else {
                for (uint32_t j = 0; j < descCount; ++j) {
                    if (isDescriptorTypeImageInfo(descType)) {
                        ++info.imageInfoCount;
                    } else if (isDescriptorTypeBufferInfo(descType)) {
                        ++info.bufferInfoCount;
                    } else if (isDescriptorTypeBufferView(descType)) {
                        ++info.bufferViewCount;
                    } else {
                        ALOGE("%s: FATAL: Unknown descriptor type %d\n", __func__, descType);
                        abort();
                    }
                }
            }
        }

        if (info.templateEntryCount)
            info.templateEntries = new VkDescriptorUpdateTemplateEntry[info.templateEntryCount];

        if (info.imageInfoCount) {
            info.imageInfoIndices = new uint32_t[info.imageInfoCount];
            info.imageInfos = new VkDescriptorImageInfo[info.imageInfoCount];
        }

        if (info.bufferInfoCount) {
            info.bufferInfoIndices = new uint32_t[info.bufferInfoCount];
            info.bufferInfos = new VkDescriptorBufferInfo[info.bufferInfoCount];
        }

        if (info.bufferViewCount) {
            info.bufferViewIndices = new uint32_t[info.bufferViewCount];
            info.bufferViews = new VkBufferView[info.bufferViewCount];
        }

        if (info.inlineUniformBlockCount) {
            info.inlineUniformBlockBuffer.resize(inlineUniformBlockBufferSize);
            info.inlineUniformBlockBytesPerBlocks.resize(info.inlineUniformBlockCount);
        }

        uint32_t imageInfoIndex = 0;
        uint32_t bufferInfoIndex = 0;
        uint32_t bufferViewIndex = 0;
        uint32_t inlineUniformBlockIndex = 0;

        for (uint32_t i = 0; i < pCreateInfo->descriptorUpdateEntryCount; ++i) {
            const auto& entry = pCreateInfo->pDescriptorUpdateEntries[i];
            uint32_t descCount = entry.descriptorCount;
            VkDescriptorType descType = entry.descriptorType;

            info.templateEntries[i] = entry;

            if (isDescriptorTypeInlineUniformBlock(descType)) {
                info.inlineUniformBlockBytesPerBlocks[inlineUniformBlockIndex] = descCount;
                ++inlineUniformBlockIndex;
            } else {
                for (uint32_t j = 0; j < descCount; ++j) {
                    if (isDescriptorTypeImageInfo(descType)) {
                        info.imageInfoIndices[imageInfoIndex] = i;
                        ++imageInfoIndex;
                    } else if (isDescriptorTypeBufferInfo(descType)) {
                        info.bufferInfoIndices[bufferInfoIndex] = i;
                        ++bufferInfoIndex;
                    } else if (isDescriptorTypeBufferView(descType)) {
                        info.bufferViewIndices[bufferViewIndex] = i;
                        ++bufferViewIndex;
                    } else {
                        ALOGE("%s: FATAL: Unknown descriptor type %d\n", __func__, descType);
                        // abort();
                    }
                }
            }
        }

        return VK_SUCCESS;
    }

    VkResult on_vkCreateDescriptorUpdateTemplate(
        void* context, VkResult input_result,
        VkDevice device,
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {

        (void)context;
        (void)device;
        (void)pAllocator;

        if (input_result != VK_SUCCESS) return input_result;

        return initDescriptorUpdateTemplateBuffers(pCreateInfo, *pDescriptorUpdateTemplate);
    }

    VkResult on_vkCreateDescriptorUpdateTemplateKHR(
        void* context, VkResult input_result,
        VkDevice device,
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {

        (void)context;
        (void)device;
        (void)pAllocator;

        if (input_result != VK_SUCCESS) return input_result;

        return initDescriptorUpdateTemplateBuffers(pCreateInfo, *pDescriptorUpdateTemplate);
    }

    void on_vkUpdateDescriptorSetWithTemplate(
        void* context,
        VkDevice device,
        VkDescriptorSet descriptorSet,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const void* pData) {

        VkEncoder* enc = (VkEncoder*)context;

        uint8_t* userBuffer = (uint8_t*)pData;
        if (!userBuffer) return;

        // TODO: Make this thread safe
        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkDescriptorUpdateTemplate.find(descriptorUpdateTemplate);
        if (it == info_VkDescriptorUpdateTemplate.end()) {
            return;
        }

        auto& info = it->second;

        uint32_t templateEntryCount = info.templateEntryCount;
        VkDescriptorUpdateTemplateEntry* templateEntries = info.templateEntries;

        uint32_t imageInfoCount = info.imageInfoCount;
        uint32_t bufferInfoCount = info.bufferInfoCount;
        uint32_t bufferViewCount = info.bufferViewCount;
        uint32_t inlineUniformBlockCount = info.inlineUniformBlockCount;
        uint32_t* imageInfoIndices = info.imageInfoIndices;
        uint32_t* bufferInfoIndices = info.bufferInfoIndices;
        uint32_t* bufferViewIndices = info.bufferViewIndices;
        VkDescriptorImageInfo* imageInfos = info.imageInfos;
        VkDescriptorBufferInfo* bufferInfos = info.bufferInfos;
        VkBufferView* bufferViews = info.bufferViews;
        uint8_t* inlineUniformBlockBuffer = info.inlineUniformBlockBuffer.data();
        uint32_t* inlineUniformBlockBytesPerBlocks = info.inlineUniformBlockBytesPerBlocks.data();

        lock.unlock();

        size_t currImageInfoOffset = 0;
        size_t currBufferInfoOffset = 0;
        size_t currBufferViewOffset = 0;
        size_t inlineUniformBlockOffset = 0;
        size_t inlineUniformBlockIdx = 0;

        struct goldfish_VkDescriptorSet* ds = as_goldfish_VkDescriptorSet(descriptorSet);
        ReifiedDescriptorSet* reified = ds->reified;

        bool batched = mFeatureInfo->hasVulkanBatchedDescriptorSetUpdate;

        for (uint32_t i = 0; i < templateEntryCount; ++i) {
            const auto& entry = templateEntries[i];
            VkDescriptorType descType = entry.descriptorType;
            uint32_t dstBinding = entry.dstBinding;

            auto offset = entry.offset;
            auto stride = entry.stride;
            auto dstArrayElement = entry.dstArrayElement;

            uint32_t descCount = entry.descriptorCount;

            if (isDescriptorTypeImageInfo(descType)) {

                if (!stride) stride = sizeof(VkDescriptorImageInfo);

                const VkDescriptorImageInfo* currImageInfoBegin =
                    (const VkDescriptorImageInfo*)((uint8_t*)imageInfos + currImageInfoOffset);

                for (uint32_t j = 0; j < descCount; ++j) {
                    const VkDescriptorImageInfo* user =
                        (const VkDescriptorImageInfo*)(userBuffer + offset + j * stride);

                    memcpy(((uint8_t*)imageInfos) + currImageInfoOffset,
                           user, sizeof(VkDescriptorImageInfo));
                    currImageInfoOffset += sizeof(VkDescriptorImageInfo);
                }

                if (batched) {
                  doEmulatedDescriptorImageInfoWriteFromTemplate(
                        descType,
                        dstBinding,
                        dstArrayElement,
                        descCount,
                        currImageInfoBegin,
                        reified);
                }
            } else if (isDescriptorTypeBufferInfo(descType)) {


                if (!stride) stride = sizeof(VkDescriptorBufferInfo);

                const VkDescriptorBufferInfo* currBufferInfoBegin =
                    (const VkDescriptorBufferInfo*)((uint8_t*)bufferInfos + currBufferInfoOffset);

                for (uint32_t j = 0; j < descCount; ++j) {
                    const VkDescriptorBufferInfo* user =
                        (const VkDescriptorBufferInfo*)(userBuffer + offset + j * stride);

                    memcpy(((uint8_t*)bufferInfos) + currBufferInfoOffset,
                           user, sizeof(VkDescriptorBufferInfo));
                    currBufferInfoOffset += sizeof(VkDescriptorBufferInfo);
                }

                if (batched) {
                  doEmulatedDescriptorBufferInfoWriteFromTemplate(
                        descType,
                        dstBinding,
                        dstArrayElement,
                        descCount,
                        currBufferInfoBegin,
                        reified);
                }

            } else if (isDescriptorTypeBufferView(descType)) {
                if (!stride) stride = sizeof(VkBufferView);

                const VkBufferView* currBufferViewBegin =
                    (const VkBufferView*)((uint8_t*)bufferViews + currBufferViewOffset);

                for (uint32_t j = 0; j < descCount; ++j) {
                  const VkBufferView* user =
                      (const VkBufferView*)(userBuffer + offset + j * stride);

                  memcpy(((uint8_t*)bufferViews) + currBufferViewOffset, user,
                         sizeof(VkBufferView));
                  currBufferViewOffset += sizeof(VkBufferView);
                }

                if (batched) {
                  doEmulatedDescriptorBufferViewWriteFromTemplate(descType, dstBinding,
                                                                  dstArrayElement, descCount,
                                                                  currBufferViewBegin, reified);
                }
            } else if (isDescriptorTypeInlineUniformBlock(descType)) {
                uint32_t inlineUniformBlockBytesPerBlock =
                    inlineUniformBlockBytesPerBlocks[inlineUniformBlockIdx];
                uint8_t* currInlineUniformBlockBufferBegin =
                    inlineUniformBlockBuffer + inlineUniformBlockOffset;
                memcpy(currInlineUniformBlockBufferBegin, userBuffer + offset,
                       inlineUniformBlockBytesPerBlock);
                inlineUniformBlockIdx++;
                inlineUniformBlockOffset += inlineUniformBlockBytesPerBlock;

                if (batched) {
                  doEmulatedDescriptorInlineUniformBlockFromTemplate(
                      descType, dstBinding, dstArrayElement, descCount,
                      currInlineUniformBlockBufferBegin, reified);
                }
            } else {
                ALOGE("%s: FATAL: Unknown descriptor type %d\n", __func__, descType);
                abort();
            }
        }

        if (batched) return;

        enc->vkUpdateDescriptorSetWithTemplateSized2GOOGLE(
            device, descriptorSet, descriptorUpdateTemplate, imageInfoCount, bufferInfoCount,
            bufferViewCount, static_cast<uint32_t>(info.inlineUniformBlockBuffer.size()),
            imageInfoIndices, bufferInfoIndices, bufferViewIndices, imageInfos, bufferInfos,
            bufferViews, inlineUniformBlockBuffer, true /* do lock */);
    }

    VkResult on_vkGetPhysicalDeviceImageFormatProperties2_common(
        bool isKhr,
        void* context, VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
        VkImageFormatProperties2* pImageFormatProperties) {

        VkEncoder* enc = (VkEncoder*)context;
        (void)input_result;

#ifdef VK_USE_PLATFORM_FUCHSIA

        constexpr VkFormat kExternalImageSupportedFormats[] = {
            VK_FORMAT_B8G8R8A8_SINT,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_B8G8R8A8_SNORM,
            VK_FORMAT_B8G8R8A8_SSCALED,
            VK_FORMAT_B8G8R8A8_USCALED,
            VK_FORMAT_R8G8B8A8_SINT,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_FORMAT_R8G8B8A8_SNORM,
            VK_FORMAT_R8G8B8A8_SSCALED,
            VK_FORMAT_R8G8B8A8_USCALED,
            VK_FORMAT_R8_UNORM,
            VK_FORMAT_R8_UINT,
            VK_FORMAT_R8_USCALED,
            VK_FORMAT_R8_SNORM,
            VK_FORMAT_R8_SINT,
            VK_FORMAT_R8_SSCALED,
            VK_FORMAT_R8_SRGB,
            VK_FORMAT_R8G8_UNORM,
            VK_FORMAT_R8G8_UINT,
            VK_FORMAT_R8G8_USCALED,
            VK_FORMAT_R8G8_SNORM,
            VK_FORMAT_R8G8_SINT,
            VK_FORMAT_R8G8_SSCALED,
            VK_FORMAT_R8G8_SRGB,
        };

        VkExternalImageFormatProperties* ext_img_properties =
            vk_find_struct<VkExternalImageFormatProperties>(pImageFormatProperties);

        if (ext_img_properties) {
          if (std::find(std::begin(kExternalImageSupportedFormats),
                        std::end(kExternalImageSupportedFormats),
                        pImageFormatInfo->format) == std::end(kExternalImageSupportedFormats)) {
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
          }
        }
#endif

#ifdef VK_USE_PLATFORM_ANDROID_KHR
        VkAndroidHardwareBufferUsageANDROID* output_ahw_usage =
            vk_find_struct<VkAndroidHardwareBufferUsageANDROID>(pImageFormatProperties);
#endif

        VkResult hostRes;

        if (isKhr) {
            hostRes = enc->vkGetPhysicalDeviceImageFormatProperties2KHR(
                physicalDevice, pImageFormatInfo,
                pImageFormatProperties, true /* do lock */);
        } else {
            hostRes = enc->vkGetPhysicalDeviceImageFormatProperties2(
                physicalDevice, pImageFormatInfo,
                pImageFormatProperties, true /* do lock */);
        }

        if (hostRes != VK_SUCCESS) return hostRes;

#ifdef VK_USE_PLATFORM_FUCHSIA
        if (ext_img_properties) {
            const VkPhysicalDeviceExternalImageFormatInfo* ext_img_info =
                vk_find_struct<VkPhysicalDeviceExternalImageFormatInfo>(pImageFormatInfo);
            if (ext_img_info) {
                if (static_cast<uint32_t>(ext_img_info->handleType) ==
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ZIRCON_VMO_BIT_FUCHSIA) {
                    ext_img_properties->externalMemoryProperties = {
                            .externalMemoryFeatures =
                                    VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                                    VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
                            .exportFromImportedHandleTypes =
                                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ZIRCON_VMO_BIT_FUCHSIA,
                            .compatibleHandleTypes =
                                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_ZIRCON_VMO_BIT_FUCHSIA,
                    };
                }
            }
        }
#endif

#ifdef VK_USE_PLATFORM_ANDROID_KHR
        if (output_ahw_usage) {
            output_ahw_usage->androidHardwareBufferUsage =
                getAndroidHardwareBufferUsageFromVkUsage(
                    pImageFormatInfo->flags,
                    pImageFormatInfo->usage);
        }
#endif

        return hostRes;
    }

    VkResult on_vkGetPhysicalDeviceImageFormatProperties2(
        void* context, VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
        VkImageFormatProperties2* pImageFormatProperties) {
        return on_vkGetPhysicalDeviceImageFormatProperties2_common(
            false /* not KHR */, context, input_result,
            physicalDevice, pImageFormatInfo, pImageFormatProperties);
    }

    VkResult on_vkGetPhysicalDeviceImageFormatProperties2KHR(
        void* context, VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
        VkImageFormatProperties2* pImageFormatProperties) {
        return on_vkGetPhysicalDeviceImageFormatProperties2_common(
            true /* is KHR */, context, input_result,
            physicalDevice, pImageFormatInfo, pImageFormatProperties);
    }

    void on_vkGetPhysicalDeviceExternalSemaphoreProperties(
        void*,
        VkPhysicalDevice,
        const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
        VkExternalSemaphoreProperties* pExternalSemaphoreProperties) {
        (void)pExternalSemaphoreInfo;
        (void)pExternalSemaphoreProperties;
#ifdef VK_USE_PLATFORM_FUCHSIA
        if (pExternalSemaphoreInfo->handleType ==
            static_cast<uint32_t>(VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_ZIRCON_EVENT_BIT_FUCHSIA)) {
            pExternalSemaphoreProperties->compatibleHandleTypes |=
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_ZIRCON_EVENT_BIT_FUCHSIA;
            pExternalSemaphoreProperties->exportFromImportedHandleTypes |=
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_ZIRCON_EVENT_BIT_FUCHSIA;
            pExternalSemaphoreProperties->externalSemaphoreFeatures |=
                VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
                VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
        }
#else
        if (pExternalSemaphoreInfo->handleType ==
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) {
            pExternalSemaphoreProperties->compatibleHandleTypes |=
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            pExternalSemaphoreProperties->exportFromImportedHandleTypes |=
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            pExternalSemaphoreProperties->externalSemaphoreFeatures |=
                VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
                VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
        }
#endif  // VK_USE_PLATFORM_FUCHSIA
    }

    void registerEncoderCleanupCallback(const VkEncoder* encoder, void* object, CleanupCallback callback) {
        AutoLock<RecursiveLock> lock(mLock);
        auto& callbacks = mEncoderCleanupCallbacks[encoder];
        callbacks[object] = callback;
    }

    void unregisterEncoderCleanupCallback(const VkEncoder* encoder, void* object) {
        AutoLock<RecursiveLock> lock(mLock);
        mEncoderCleanupCallbacks[encoder].erase(object);
    }

    void onEncoderDeleted(const VkEncoder* encoder) {
        AutoLock<RecursiveLock> lock(mLock);
        if (mEncoderCleanupCallbacks.find(encoder) == mEncoderCleanupCallbacks.end()) return;

        std::unordered_map<void*, CleanupCallback> callbackCopies = mEncoderCleanupCallbacks[encoder];

        mEncoderCleanupCallbacks.erase(encoder);
        lock.unlock();

        for (auto it : callbackCopies) {
            it.second();
        }
    }

    uint32_t syncEncodersForCommandBuffer(VkCommandBuffer commandBuffer, VkEncoder* currentEncoder) {
        struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
        if (!cb) return 0;

        auto lastEncoder = cb->lastUsedEncoder;

        if (lastEncoder == currentEncoder) return 0;

        currentEncoder->incRef();

        cb->lastUsedEncoder = currentEncoder;

        if (!lastEncoder) return 0;

        auto oldSeq = cb->sequenceNumber;
        cb->sequenceNumber += 2;
        lastEncoder->vkCommandBufferHostSyncGOOGLE(commandBuffer, false, oldSeq + 1, true /* do lock */);
        lastEncoder->flush();
        currentEncoder->vkCommandBufferHostSyncGOOGLE(commandBuffer, true, oldSeq + 2, true /* do lock */);

        if (lastEncoder->decRef()) {
            cb->lastUsedEncoder = nullptr;
        }
        return 0;
    }

    uint32_t syncEncodersForQueue(VkQueue queue, VkEncoder* currentEncoder) {
        if (!supportsAsyncQueueSubmit()) {
            return 0;
        }

        struct goldfish_VkQueue* q = as_goldfish_VkQueue(queue);
        if (!q) return 0;

        auto lastEncoder = q->lastUsedEncoder;

        if (lastEncoder == currentEncoder) return 0;

        currentEncoder->incRef();

        q->lastUsedEncoder = currentEncoder;

        if (!lastEncoder) return 0;

        auto oldSeq = q->sequenceNumber;
        q->sequenceNumber += 2;
        lastEncoder->vkQueueHostSyncGOOGLE(queue, false, oldSeq + 1, true /* do lock */);
        lastEncoder->flush();
        currentEncoder->vkQueueHostSyncGOOGLE(queue, true, oldSeq + 2, true /* do lock */);

        if (lastEncoder->decRef()) {
            q->lastUsedEncoder = nullptr;
        }

        return 0;
    }

    CommandBufferStagingStream::Alloc getAlloc() {
        if (mFeatureInfo->hasVulkanAuxCommandMemory) {
            return [this](size_t size) -> CommandBufferStagingStream::Memory {
                VkMemoryAllocateInfo info{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .pNext = nullptr,
                    .allocationSize = size,
                    .memoryTypeIndex = VK_MAX_MEMORY_TYPES  // indicates auxiliary memory
                };

                auto enc = ResourceTracker::getThreadLocalEncoder();
                VkDevice device = VK_NULL_HANDLE;
                VkDeviceMemory vkDeviceMem = VK_NULL_HANDLE;
                VkResult result = getCoherentMemory(&info, enc, device, &vkDeviceMem);
                if (result != VK_SUCCESS) {
                    ALOGE("Failed to get coherent memory %u", result);
                    return {.deviceMemory = VK_NULL_HANDLE, .ptr = nullptr};
                }

                // getCoherentMemory() uses suballocations.
                // To retrieve the suballocated memory address, look up
                // VkDeviceMemory filled in by getCoherentMemory()
                // scope of mLock
                {
                    AutoLock<RecursiveLock> lock(mLock);
                    const auto it = info_VkDeviceMemory.find(vkDeviceMem);
                    if (it == info_VkDeviceMemory.end()) {
                        ALOGE("Coherent memory allocated %u not found", result);
                        return {.deviceMemory = VK_NULL_HANDLE, .ptr = nullptr};
                    };

                    const auto& info = it->second;
                    return {.deviceMemory = vkDeviceMem, .ptr = info.ptr};
                }
            };
        }
        return nullptr;
    }

    CommandBufferStagingStream::Free getFree() {
        if (mFeatureInfo->hasVulkanAuxCommandMemory) {
            return [this](const CommandBufferStagingStream::Memory& memory) {
                // deviceMemory may not be the actual backing auxiliary VkDeviceMemory
                // for suballocations, deviceMemory is a alias VkDeviceMemory hand;
                // freeCoherentMemoryLocked maps the alias to the backing VkDeviceMemory
                VkDeviceMemory deviceMemory = memory.deviceMemory;
                AutoLock<RecursiveLock> lock(mLock);
                auto it = info_VkDeviceMemory.find(deviceMemory);
                if (it == info_VkDeviceMemory.end()) {
                    ALOGE("Device memory to free not found");
                    return;
                }
                auto coherentMemory = freeCoherentMemoryLocked(deviceMemory, it->second);
                // We have to release the lock before we could possibly free a
                // CoherentMemory, because that will call into VkEncoder, which
                // shouldn't be called when the lock is held.
                lock.unlock();
                coherentMemory = nullptr;
            };
        }
        return nullptr;
    }

    VkResult on_vkBeginCommandBuffer(
        void* context, VkResult input_result,
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo) {

        (void)context;

        resetCommandBufferStagingInfo(commandBuffer, true /* also reset primaries */, true /* also clear pending descriptor sets */);

        VkEncoder* enc = ResourceTracker::getCommandBufferEncoder(commandBuffer);
        (void)input_result;

        struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
        cb->flags = pBeginInfo->flags;

        VkCommandBufferBeginInfo modifiedBeginInfo;

        if (pBeginInfo->pInheritanceInfo && !cb->isSecondary) {
            modifiedBeginInfo = *pBeginInfo;
            modifiedBeginInfo.pInheritanceInfo = nullptr;
            pBeginInfo = &modifiedBeginInfo;
        }

        if (!supportsDeferredCommands()) {
            return enc->vkBeginCommandBuffer(commandBuffer, pBeginInfo, true /* do lock */);
        }

        enc->vkBeginCommandBufferAsyncGOOGLE(commandBuffer, pBeginInfo, true /* do lock */);

        return VK_SUCCESS;
    }

    VkResult on_vkEndCommandBuffer(
        void* context, VkResult input_result,
        VkCommandBuffer commandBuffer) {

        VkEncoder* enc = (VkEncoder*)context;
        (void)input_result;

        if (!supportsDeferredCommands()) {
            return enc->vkEndCommandBuffer(commandBuffer, true /* do lock */);
        }

        enc->vkEndCommandBufferAsyncGOOGLE(commandBuffer, true /* do lock */);

        return VK_SUCCESS;
    }

    VkResult on_vkResetCommandBuffer(
        void* context, VkResult input_result,
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags) {

        resetCommandBufferStagingInfo(commandBuffer, true /* also reset primaries */, true /* also clear pending descriptor sets */);

        VkEncoder* enc = (VkEncoder*)context;
        (void)input_result;

        if (!supportsDeferredCommands()) {
            return enc->vkResetCommandBuffer(commandBuffer, flags, true /* do lock */);
        }

        enc->vkResetCommandBufferAsyncGOOGLE(commandBuffer, flags, true /* do lock */);
        return VK_SUCCESS;
    }

    VkResult on_vkCreateImageView(
        void* context, VkResult input_result,
        VkDevice device,
        const VkImageViewCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkImageView* pView) {

        VkEncoder* enc = (VkEncoder*)context;
        (void)input_result;

        VkImageViewCreateInfo localCreateInfo = vk_make_orphan_copy(*pCreateInfo);
        vk_struct_chain_iterator structChainIter = vk_make_chain_iterator(&localCreateInfo);

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        if (pCreateInfo->format == VK_FORMAT_UNDEFINED) {
            AutoLock<RecursiveLock> lock(mLock);

            auto it = info_VkImage.find(pCreateInfo->image);
            if (it != info_VkImage.end() && it->second.hasExternalFormat) {
                localCreateInfo.format = vk_format_from_android(it->second.androidFormat);
            }
        }
        VkSamplerYcbcrConversionInfo localVkSamplerYcbcrConversionInfo;
        const VkSamplerYcbcrConversionInfo* samplerYcbcrConversionInfo =
            vk_find_struct<VkSamplerYcbcrConversionInfo>(pCreateInfo);
        if (samplerYcbcrConversionInfo) {
            if (samplerYcbcrConversionInfo->conversion != VK_YCBCR_CONVERSION_DO_NOTHING) {
                localVkSamplerYcbcrConversionInfo = vk_make_orphan_copy(*samplerYcbcrConversionInfo);
                vk_append_struct(&structChainIter, &localVkSamplerYcbcrConversionInfo);
            }
        }
#endif

        return enc->vkCreateImageView(device, &localCreateInfo, pAllocator, pView, true /* do lock */);
    }

    void on_vkCmdExecuteCommands(
        void* context,
        VkCommandBuffer commandBuffer,
        uint32_t commandBufferCount,
        const VkCommandBuffer* pCommandBuffers) {

        VkEncoder* enc = (VkEncoder*)context;

        if (!mFeatureInfo->hasVulkanQueueSubmitWithCommands) {
            enc->vkCmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers, true /* do lock */);
            return;
        }

        struct goldfish_VkCommandBuffer* primary = as_goldfish_VkCommandBuffer(commandBuffer);
        for (uint32_t i = 0; i < commandBufferCount; ++i) {
            struct goldfish_VkCommandBuffer* secondary = as_goldfish_VkCommandBuffer(pCommandBuffers[i]);
            appendObject(&secondary->superObjects, primary);
            appendObject(&primary->subObjects, secondary);
        }

        enc->vkCmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers, true /* do lock */);
    }

    void addPendingDescriptorSets(VkCommandBuffer commandBuffer, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets) {
        struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);

        if (!cb->userPtr) {
            CommandBufferPendingDescriptorSets* newPendingSets =
                new CommandBufferPendingDescriptorSets;
            cb->userPtr = newPendingSets;
        }

        CommandBufferPendingDescriptorSets* pendingSets =
            (CommandBufferPendingDescriptorSets*)cb->userPtr;

        for (uint32_t i = 0; i < descriptorSetCount; ++i) {
            pendingSets->sets.insert(pDescriptorSets[i]);
        }
    }

    void on_vkCmdBindDescriptorSets(
        void* context,
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipelineLayout layout,
        uint32_t firstSet,
        uint32_t descriptorSetCount,
        const VkDescriptorSet* pDescriptorSets,
        uint32_t dynamicOffsetCount,
        const uint32_t* pDynamicOffsets) {

        VkEncoder* enc = (VkEncoder*)context;

        if (mFeatureInfo->hasVulkanBatchedDescriptorSetUpdate)
            addPendingDescriptorSets(commandBuffer, descriptorSetCount, pDescriptorSets);

        enc->vkCmdBindDescriptorSets(
            commandBuffer,
            pipelineBindPoint,
            layout,
            firstSet,
            descriptorSetCount,
            pDescriptorSets,
            dynamicOffsetCount,
            pDynamicOffsets,
            true /* do lock */);
    }

    void on_vkCmdPipelineBarrier(
        void* context,
        VkCommandBuffer commandBuffer,
        VkPipelineStageFlags srcStageMask,
        VkPipelineStageFlags dstStageMask,
        VkDependencyFlags dependencyFlags,
        uint32_t memoryBarrierCount,
        const VkMemoryBarrier* pMemoryBarriers,
        uint32_t bufferMemoryBarrierCount,
        const VkBufferMemoryBarrier* pBufferMemoryBarriers,
        uint32_t imageMemoryBarrierCount,
        const VkImageMemoryBarrier* pImageMemoryBarriers) {

        VkEncoder* enc = (VkEncoder*)context;

        std::vector<VkImageMemoryBarrier> updatedImageMemoryBarriers;
        updatedImageMemoryBarriers.reserve(imageMemoryBarrierCount);
        for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
            VkImageMemoryBarrier barrier = pImageMemoryBarriers[i];

#ifdef VK_USE_PLATFORM_ANDROID_KHR
            // Unfortunetly, Android does not yet have a mechanism for sharing the expected
            // VkImageLayout when passing around AHardwareBuffer-s so many existing users
            // that import AHardwareBuffer-s into VkImage-s/VkDeviceMemory-s simply use
            // VK_IMAGE_LAYOUT_UNDEFINED. However, the Vulkan spec's image layout transition
            // sections says "If the old layout is VK_IMAGE_LAYOUT_UNDEFINED, the contents of
            // that range may be discarded." Some Vulkan drivers have been observed to actually
            // perform the discard which leads to AHardwareBuffer-s being unintentionally
            // cleared. See go/ahb-vkimagelayout for more information.
            if (barrier.srcQueueFamilyIndex != barrier.dstQueueFamilyIndex &&
                (barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL ||
                 barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT) &&
                barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                // This is not a complete solution as the Vulkan spec does not require that
                // Vulkan drivers perform a no-op in the case when oldLayout equals newLayout
                // but this has been observed to be enough to work for now to avoid clearing
                // out images.
                // TODO(b/236179843): figure out long term solution.
                barrier.oldLayout = barrier.newLayout;
            }
#endif

            updatedImageMemoryBarriers.push_back(barrier);
        }

        enc->vkCmdPipelineBarrier(
            commandBuffer,
            srcStageMask,
            dstStageMask,
            dependencyFlags,
            memoryBarrierCount,
            pMemoryBarriers,
            bufferMemoryBarrierCount,
            pBufferMemoryBarriers,
            updatedImageMemoryBarriers.size(),
            updatedImageMemoryBarriers.data(),
            true /* do lock */);
    }

    void decDescriptorSetLayoutRef(
        void* context,
        VkDevice device,
        VkDescriptorSetLayout descriptorSetLayout,
        const VkAllocationCallbacks* pAllocator) {

        if (!descriptorSetLayout) return;

        struct goldfish_VkDescriptorSetLayout* setLayout = as_goldfish_VkDescriptorSetLayout(descriptorSetLayout);

        if (0 == --setLayout->layoutInfo->refcount) {
            VkEncoder* enc = (VkEncoder*)context;
            enc->vkDestroyDescriptorSetLayout(device, descriptorSetLayout, pAllocator, true /* do lock */);
        }
    }

    void on_vkDestroyDescriptorSetLayout(
        void* context,
        VkDevice device,
        VkDescriptorSetLayout descriptorSetLayout,
        const VkAllocationCallbacks* pAllocator) {
        decDescriptorSetLayoutRef(context, device, descriptorSetLayout, pAllocator);
    }

    VkResult on_vkAllocateCommandBuffers(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkCommandBufferAllocateInfo* pAllocateInfo,
        VkCommandBuffer* pCommandBuffers) {

        (void)input_result;

        VkEncoder* enc = (VkEncoder*)context;
        VkResult res = enc->vkAllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers, true /* do lock */);
        if (VK_SUCCESS != res) return res;

        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(pCommandBuffers[i]);
            cb->isSecondary = pAllocateInfo->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            cb->device = device;
        }

        return res;
    }

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    VkResult exportSyncFdForQSRILocked(VkImage image, int *fd) {

        ALOGV("%s: call for image %p hos timage handle 0x%llx\n", __func__, (void*)image,
              (unsigned long long)get_host_u64_VkImage(image));

        if (mFeatureInfo->hasVirtioGpuNativeSync) {
            struct VirtGpuExecBuffer exec = { };
            struct gfxstreamCreateQSRIExportVK exportQSRI = { };
            VirtGpuDevice& instance = VirtGpuDevice::getInstance();

            uint64_t hostImageHandle = get_host_u64_VkImage(image);

            exportQSRI.hdr.opCode = GFXSTREAM_CREATE_QSRI_EXPORT_VK;
            exportQSRI.imageHandleLo = (uint32_t)hostImageHandle;
            exportQSRI.imageHandleHi = (uint32_t)(hostImageHandle >> 32);

            exec.command = static_cast<void*>(&exportQSRI);
            exec.command_size = sizeof(exportQSRI);
            exec.flags = kFenceOut | kRingIdx;
            if (instance.execBuffer(exec, nullptr))
                return VK_ERROR_OUT_OF_HOST_MEMORY;

            *fd = exec.handle.osHandle;
        } else {
            ensureSyncDeviceFd();
            goldfish_sync_queue_work(
                    mSyncDeviceFd,
                    get_host_u64_VkImage(image) /* the handle */,
                    GOLDFISH_SYNC_VULKAN_QSRI /* thread handle (doubling as type field) */,
                    fd);
        }

        ALOGV("%s: got fd: %d\n", __func__, *fd);
        auto imageInfoIt = info_VkImage.find(image);
        if (imageInfoIt != info_VkImage.end()) {
            auto& imageInfo = imageInfoIt->second;

            // Remove any pending QSRI sync fds that are already signaled.
            auto syncFdIt = imageInfo.pendingQsriSyncFds.begin();
            while (syncFdIt != imageInfo.pendingQsriSyncFds.end()) {
                int syncFd = *syncFdIt;
                int syncWaitRet = sync_wait(syncFd, /*timeout msecs*/0);
                if (syncWaitRet == 0) {
                    // Sync fd is signaled.
                    syncFdIt = imageInfo.pendingQsriSyncFds.erase(syncFdIt);
                    close(syncFd);
                } else {
                    if (errno != ETIME) {
                        ALOGE("%s: Failed to wait for pending QSRI sync: sterror: %s errno: %d",
                              __func__, strerror(errno), errno);
                    }
                    break;
                }
            }

            int syncFdDup = dup(*fd);
            if (syncFdDup < 0) {
                ALOGE("%s: Failed to dup() QSRI sync fd : sterror: %s errno: %d",
                      __func__, strerror(errno), errno);
            } else {
                imageInfo.pendingQsriSyncFds.push_back(syncFdDup);
            }
        }

        return VK_SUCCESS;
    }

    VkResult on_vkQueueSignalReleaseImageANDROID(
        void* context,
        VkResult input_result,
        VkQueue queue,
        uint32_t waitSemaphoreCount,
        const VkSemaphore* pWaitSemaphores,
        VkImage image,
        int* pNativeFenceFd) {

        (void)input_result;

        VkEncoder* enc = (VkEncoder*)context;

        if (!mFeatureInfo->hasVulkanAsyncQsri) {
            return enc->vkQueueSignalReleaseImageANDROID(queue, waitSemaphoreCount, pWaitSemaphores, image, pNativeFenceFd, true /* lock */);
        }

        {
            AutoLock<RecursiveLock> lock(mLock);
            auto it = info_VkImage.find(image);
            if (it == info_VkImage.end()) {
                if (pNativeFenceFd) *pNativeFenceFd = -1;
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        enc->vkQueueSignalReleaseImageANDROIDAsyncGOOGLE(queue, waitSemaphoreCount, pWaitSemaphores, image, true /* lock */);

        AutoLock<RecursiveLock> lock(mLock);
        VkResult result;
        if (pNativeFenceFd) {
            result =
                exportSyncFdForQSRILocked(image, pNativeFenceFd);
        } else {
            int syncFd;
            result = exportSyncFdForQSRILocked(image, &syncFd);

            if (syncFd >= 0)
                close(syncFd);
        }

        return result;
    }
#endif

    VkResult on_vkCreateGraphicsPipelines(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkPipelineCache pipelineCache,
        uint32_t createInfoCount,
        const VkGraphicsPipelineCreateInfo* pCreateInfos,
        const VkAllocationCallbacks* pAllocator,
        VkPipeline* pPipelines) {
        (void)input_result;
        VkEncoder* enc = (VkEncoder*)context;
        std::vector<VkGraphicsPipelineCreateInfo> localCreateInfos(
                pCreateInfos, pCreateInfos + createInfoCount);
        for (VkGraphicsPipelineCreateInfo& graphicsPipelineCreateInfo : localCreateInfos) {
            // dEQP-VK.api.pipeline.pipeline_invalid_pointers_unused_structs#graphics
            bool requireViewportState = false;
            // VUID-VkGraphicsPipelineCreateInfo-rasterizerDiscardEnable-00750
            requireViewportState |= graphicsPipelineCreateInfo.pRasterizationState != nullptr &&
                    graphicsPipelineCreateInfo.pRasterizationState->rasterizerDiscardEnable
                        == VK_FALSE;
            // VUID-VkGraphicsPipelineCreateInfo-pViewportState-04892
#ifdef VK_EXT_extended_dynamic_state2
            if (!requireViewportState && graphicsPipelineCreateInfo.pDynamicState) {
                for (uint32_t i = 0; i <
                            graphicsPipelineCreateInfo.pDynamicState->dynamicStateCount; i++) {
                    if (VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT ==
                                graphicsPipelineCreateInfo.pDynamicState->pDynamicStates[i]) {
                        requireViewportState = true;
                        break;
                    }
                }
            }
#endif // VK_EXT_extended_dynamic_state2
            if (!requireViewportState) {
                graphicsPipelineCreateInfo.pViewportState = nullptr;
            }

            // It has the same requirement as for pViewportState.
            bool shouldIncludeFragmentShaderState = requireViewportState;

            // VUID-VkGraphicsPipelineCreateInfo-rasterizerDiscardEnable-00751
            if (!shouldIncludeFragmentShaderState) {
                graphicsPipelineCreateInfo.pMultisampleState = nullptr;
            }

            // VUID-VkGraphicsPipelineCreateInfo-renderPass-06043
            // VUID-VkGraphicsPipelineCreateInfo-renderPass-06044
            if (graphicsPipelineCreateInfo.renderPass == VK_NULL_HANDLE
                    || !shouldIncludeFragmentShaderState) {
                graphicsPipelineCreateInfo.pDepthStencilState = nullptr;
                graphicsPipelineCreateInfo.pColorBlendState = nullptr;
            }
        }
        return enc->vkCreateGraphicsPipelines(device, pipelineCache, localCreateInfos.size(),
                localCreateInfos.data(), pAllocator, pPipelines, true /* do lock */);
    }

    uint32_t getApiVersionFromInstance(VkInstance instance) const {
        AutoLock<RecursiveLock> lock(mLock);
        uint32_t api = kDefaultApiVersion;

        auto it = info_VkInstance.find(instance);
        if (it == info_VkInstance.end()) return api;

        api = it->second.highestApiVersion;

        return api;
    }

    uint32_t getApiVersionFromDevice(VkDevice device) const {
        AutoLock<RecursiveLock> lock(mLock);

        uint32_t api = kDefaultApiVersion;

        auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return api;

        api = it->second.apiVersion;

        return api;
    }

    bool hasInstanceExtension(VkInstance instance, const std::string& name) const {
        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkInstance.find(instance);
        if (it == info_VkInstance.end()) return false;

        return it->second.enabledExtensions.find(name) !=
               it->second.enabledExtensions.end();
    }

    bool hasDeviceExtension(VkDevice device, const std::string& name) const {
        AutoLock<RecursiveLock> lock(mLock);

        auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return false;

        return it->second.enabledExtensions.find(name) !=
               it->second.enabledExtensions.end();
    }

    VkDevice getDevice(VkCommandBuffer commandBuffer) const {
        struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
        if (!cb) {
            return nullptr;
        }
        return cb->device;
    }

    // Resets staging stream for this command buffer and primary command buffers
    // where this command buffer has been recorded. If requested, also clears the pending
    // descriptor sets.
    void resetCommandBufferStagingInfo(VkCommandBuffer commandBuffer, bool alsoResetPrimaries,
                                       bool alsoClearPendingDescriptorSets) {
        struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
        if (!cb) {
            return;
        }
        if (cb->privateEncoder) {
            sStaging.pushStaging((CommandBufferStagingStream*)cb->privateStream, cb->privateEncoder);
            cb->privateEncoder = nullptr;
            cb->privateStream = nullptr;
        }

        if (alsoClearPendingDescriptorSets && cb->userPtr) {
            CommandBufferPendingDescriptorSets* pendingSets = (CommandBufferPendingDescriptorSets*)cb->userPtr;
            pendingSets->sets.clear();
        }

        if (alsoResetPrimaries) {
            forAllObjects(cb->superObjects, [this, alsoResetPrimaries, alsoClearPendingDescriptorSets](void* obj) {
                VkCommandBuffer superCommandBuffer = (VkCommandBuffer)obj;
                struct goldfish_VkCommandBuffer* superCb = as_goldfish_VkCommandBuffer(superCommandBuffer);
                this->resetCommandBufferStagingInfo(superCommandBuffer, alsoResetPrimaries, alsoClearPendingDescriptorSets);
            });
            eraseObjects(&cb->superObjects);
        }

        forAllObjects(cb->subObjects, [cb](void* obj) {
            VkCommandBuffer subCommandBuffer = (VkCommandBuffer)obj;
            struct goldfish_VkCommandBuffer* subCb = as_goldfish_VkCommandBuffer(subCommandBuffer);
            // We don't do resetCommandBufferStagingInfo(subCommandBuffer)
            // since the user still might have submittable stuff pending there.
            eraseObject(&subCb->superObjects, (void*)cb);
        });

        eraseObjects(&cb->subObjects);
    }

    void resetCommandPoolStagingInfo(VkCommandPool commandPool) {
        struct goldfish_VkCommandPool* p = as_goldfish_VkCommandPool(commandPool);

        if (!p) return;

        forAllObjects(p->subObjects, [this](void* commandBuffer) {
            this->resetCommandBufferStagingInfo((VkCommandBuffer)commandBuffer, true /* also reset primaries */, true /* also clear pending descriptor sets */);
        });
    }

    void addToCommandPool(VkCommandPool commandPool,
                          uint32_t commandBufferCount,
                          VkCommandBuffer* pCommandBuffers) {
        for (uint32_t i = 0; i < commandBufferCount; ++i) {
            struct goldfish_VkCommandPool* p = as_goldfish_VkCommandPool(commandPool);
            struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(pCommandBuffers[i]);
            appendObject(&p->subObjects, (void*)(pCommandBuffers[i]));
            appendObject(&cb->poolObjects, (void*)commandPool);
        }
    }

    void clearCommandPool(VkCommandPool commandPool) {
        resetCommandPoolStagingInfo(commandPool);
        struct goldfish_VkCommandPool* p = as_goldfish_VkCommandPool(commandPool);
        forAllObjects(p->subObjects, [this](void* commandBuffer) {
            this->unregister_VkCommandBuffer((VkCommandBuffer)commandBuffer);
        });
        eraseObjects(&p->subObjects);
    }

private:
    mutable RecursiveLock mLock;

    const VkPhysicalDeviceMemoryProperties& getPhysicalDeviceMemoryProperties(
            void* context,
            VkDevice device = VK_NULL_HANDLE,
            VkPhysicalDevice physicalDevice = VK_NULL_HANDLE) {
        if (!mCachedPhysicalDeviceMemoryProps) {
            if (physicalDevice == VK_NULL_HANDLE) {
                AutoLock<RecursiveLock> lock(mLock);

                auto deviceInfoIt = info_VkDevice.find(device);
                if (deviceInfoIt == info_VkDevice.end()) {
                    ALOGE("Failed to pass device or physical device.");
                    abort();
                }
                const auto& deviceInfo = deviceInfoIt->second;
                physicalDevice = deviceInfo.physdev;
            }

            VkEncoder* enc = (VkEncoder*)context;

            VkPhysicalDeviceMemoryProperties properties;
            enc->vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties, true /* no lock */);

            mCachedPhysicalDeviceMemoryProps.emplace(std::move(properties));
        }
        return *mCachedPhysicalDeviceMemoryProps;
    }

    std::optional<const VkPhysicalDeviceMemoryProperties> mCachedPhysicalDeviceMemoryProps;
    std::unique_ptr<EmulatorFeatureInfo> mFeatureInfo;
    std::unique_ptr<GoldfishAddressSpaceBlockProvider> mGoldfishAddressSpaceBlockProvider;

    struct VirtGpuCaps mCaps;
    std::vector<VkExtensionProperties> mHostInstanceExtensions;
    std::vector<VkExtensionProperties> mHostDeviceExtensions;

    // 32 bits only for now, upper bits may be used later.
    std::atomic<uint32_t> mBlobId = 0;
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
    int mSyncDeviceFd = -1;
#endif

#ifdef VK_USE_PLATFORM_FUCHSIA
    fidl::WireSyncClient<fuchsia_hardware_goldfish::ControlDevice>
        mControlDevice;
    fidl::WireSyncClient<fuchsia_sysmem::Allocator>
        mSysmemAllocator;
#endif

    WorkPool mWorkPool { 4 };
    std::unordered_map<VkQueue, std::vector<WorkPool::WaitGroupHandle>>
        mQueueSensitiveWorkPoolItems;

    std::unordered_map<const VkEncoder*, std::unordered_map<void*, CleanupCallback>> mEncoderCleanupCallbacks;

};

ResourceTracker::ResourceTracker() : mImpl(new ResourceTracker::Impl()) { }
ResourceTracker::~ResourceTracker() { }
VulkanHandleMapping* ResourceTracker::createMapping() {
    return &mImpl->createMapping;
}
VulkanHandleMapping* ResourceTracker::unwrapMapping() {
    return &mImpl->unwrapMapping;
}
VulkanHandleMapping* ResourceTracker::destroyMapping() {
    return &mImpl->destroyMapping;
}
VulkanHandleMapping* ResourceTracker::defaultMapping() {
    return &mImpl->defaultMapping;
}
static ResourceTracker* sTracker = nullptr;
// static
ResourceTracker* ResourceTracker::get() {
    if (!sTracker) {
        // To be initialized once on vulkan device open.
        sTracker = new ResourceTracker;
    }
    return sTracker;
}

#define HANDLE_REGISTER_IMPL(type) \
    void ResourceTracker::register_##type(type obj) { \
        mImpl->register_##type(obj); \
    } \
    void ResourceTracker::unregister_##type(type obj) { \
        mImpl->unregister_##type(obj); \
    } \

GOLDFISH_VK_LIST_HANDLE_TYPES(HANDLE_REGISTER_IMPL)

uint8_t* ResourceTracker::getMappedPointer(VkDeviceMemory memory) {
    return mImpl->getMappedPointer(memory);
}

VkDeviceSize ResourceTracker::getMappedSize(VkDeviceMemory memory) {
    return mImpl->getMappedSize(memory);
}

bool ResourceTracker::isValidMemoryRange(const VkMappedMemoryRange& range) const {
    return mImpl->isValidMemoryRange(range);
}

void ResourceTracker::setupFeatures(const EmulatorFeatureInfo* features) {
    mImpl->setupFeatures(features);
}

void ResourceTracker::setupCaps(void) { mImpl->setupCaps(); }

void ResourceTracker::setThreadingCallbacks(const ResourceTracker::ThreadingCallbacks& callbacks) {
    mImpl->setThreadingCallbacks(callbacks);
}

bool ResourceTracker::hostSupportsVulkan() const {
    return mImpl->hostSupportsVulkan();
}

bool ResourceTracker::usingDirectMapping() const {
    return mImpl->usingDirectMapping();
}

uint32_t ResourceTracker::getStreamFeatures() const {
    return mImpl->getStreamFeatures();
}

uint32_t ResourceTracker::getApiVersionFromInstance(VkInstance instance) const {
    return mImpl->getApiVersionFromInstance(instance);
}

uint32_t ResourceTracker::getApiVersionFromDevice(VkDevice device) const {
    return mImpl->getApiVersionFromDevice(device);
}
bool ResourceTracker::hasInstanceExtension(VkInstance instance, const std::string &name) const {
    return mImpl->hasInstanceExtension(instance, name);
}
bool ResourceTracker::hasDeviceExtension(VkDevice device, const std::string &name) const {
    return mImpl->hasDeviceExtension(device, name);
}
VkDevice ResourceTracker::getDevice(VkCommandBuffer commandBuffer) const {
    return mImpl->getDevice(commandBuffer);
}
void ResourceTracker::addToCommandPool(VkCommandPool commandPool,
                      uint32_t commandBufferCount,
                      VkCommandBuffer* pCommandBuffers) {
    mImpl->addToCommandPool(commandPool, commandBufferCount, pCommandBuffers);
}
void ResourceTracker::resetCommandPoolStagingInfo(VkCommandPool commandPool) {
    mImpl->resetCommandPoolStagingInfo(commandPool);
}


// static
ALWAYS_INLINE VkEncoder* ResourceTracker::getCommandBufferEncoder(VkCommandBuffer commandBuffer) {
    if (!(ResourceTracker::streamFeatureBits & VULKAN_STREAM_FEATURE_QUEUE_SUBMIT_WITH_COMMANDS_BIT)) {
        auto enc = ResourceTracker::getThreadLocalEncoder();
        ResourceTracker::get()->syncEncodersForCommandBuffer(commandBuffer, enc);
        return enc;
    }

    struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
    if (!cb->privateEncoder) {
        sStaging.setAllocFree(ResourceTracker::get()->getAlloc(),
                              ResourceTracker::get()->getFree());
        sStaging.popStaging((CommandBufferStagingStream**)&cb->privateStream, &cb->privateEncoder);
    }
    uint8_t* writtenPtr; size_t written;
    ((CommandBufferStagingStream*)cb->privateStream)->getWritten(&writtenPtr, &written);
    return cb->privateEncoder;
}

// static
ALWAYS_INLINE VkEncoder* ResourceTracker::getQueueEncoder(VkQueue queue) {
    auto enc = ResourceTracker::getThreadLocalEncoder();
    if (!(ResourceTracker::streamFeatureBits & VULKAN_STREAM_FEATURE_QUEUE_SUBMIT_WITH_COMMANDS_BIT)) {
        ResourceTracker::get()->syncEncodersForQueue(queue, enc);
    }
    return enc;
}

// static
ALWAYS_INLINE VkEncoder* ResourceTracker::getThreadLocalEncoder() {
    auto hostConn = ResourceTracker::threadingCallbacks.hostConnectionGetFunc();
    auto vkEncoder = ResourceTracker::threadingCallbacks.vkEncoderGetFunc(hostConn);
    return vkEncoder;
}

// static
void ResourceTracker::setSeqnoPtr(uint32_t* seqnoptr) {
    sSeqnoPtr = seqnoptr;
}

// static
ALWAYS_INLINE uint32_t ResourceTracker::nextSeqno() {
    uint32_t res = __atomic_add_fetch(sSeqnoPtr, 1, __ATOMIC_SEQ_CST);
    return res;
}

// static
ALWAYS_INLINE uint32_t ResourceTracker::getSeqno() {
    uint32_t res = __atomic_load_n(sSeqnoPtr, __ATOMIC_SEQ_CST);
    return res;
}

VkResult ResourceTracker::on_vkEnumerateInstanceExtensionProperties(
    void* context,
    VkResult input_result,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    return mImpl->on_vkEnumerateInstanceExtensionProperties(
        context, input_result, pLayerName, pPropertyCount, pProperties);
}

VkResult ResourceTracker::on_vkEnumerateDeviceExtensionProperties(
    void* context,
    VkResult input_result,
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    return mImpl->on_vkEnumerateDeviceExtensionProperties(
        context, input_result, physicalDevice, pLayerName, pPropertyCount, pProperties);
}

VkResult ResourceTracker::on_vkEnumeratePhysicalDevices(
    void* context, VkResult input_result,
    VkInstance instance, uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices) {
    return mImpl->on_vkEnumeratePhysicalDevices(
        context, input_result, instance, pPhysicalDeviceCount,
        pPhysicalDevices);
}

void ResourceTracker::on_vkGetPhysicalDeviceProperties(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties) {
    mImpl->on_vkGetPhysicalDeviceProperties(context, physicalDevice,
        pProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceFeatures2(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures) {
    mImpl->on_vkGetPhysicalDeviceFeatures2(context, physicalDevice,
        pFeatures);
}

void ResourceTracker::on_vkGetPhysicalDeviceFeatures2KHR(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures) {
    mImpl->on_vkGetPhysicalDeviceFeatures2(context, physicalDevice,
        pFeatures);
}

void ResourceTracker::on_vkGetPhysicalDeviceProperties2(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties) {
    mImpl->on_vkGetPhysicalDeviceProperties2(context, physicalDevice,
        pProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceProperties2KHR(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties) {
    mImpl->on_vkGetPhysicalDeviceProperties2(context, physicalDevice,
        pProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceMemoryProperties(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties(
        context, physicalDevice, pMemoryProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceMemoryProperties2(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties2(
        context, physicalDevice, pMemoryProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceMemoryProperties2KHR(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties2(
        context, physicalDevice, pMemoryProperties);
}

void ResourceTracker::on_vkGetDeviceQueue(void* context,
                                          VkDevice device,
                                          uint32_t queueFamilyIndex,
                                          uint32_t queueIndex,
                                          VkQueue* pQueue) {
    mImpl->on_vkGetDeviceQueue(context, device, queueFamilyIndex, queueIndex,
                               pQueue);
}

void ResourceTracker::on_vkGetDeviceQueue2(void* context,
                                           VkDevice device,
                                           const VkDeviceQueueInfo2* pQueueInfo,
                                           VkQueue* pQueue) {
    mImpl->on_vkGetDeviceQueue2(context, device, pQueueInfo, pQueue);
}

VkResult ResourceTracker::on_vkCreateInstance(
    void* context,
    VkResult input_result,
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    return mImpl->on_vkCreateInstance(
        context, input_result, pCreateInfo, pAllocator, pInstance);
}

VkResult ResourceTracker::on_vkCreateDevice(
    void* context,
    VkResult input_result,
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
    return mImpl->on_vkCreateDevice(
        context, input_result, physicalDevice, pCreateInfo, pAllocator, pDevice);
}

void ResourceTracker::on_vkDestroyDevice_pre(
    void* context,
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyDevice_pre(context, device, pAllocator);
}

VkResult ResourceTracker::on_vkAllocateMemory(
    void* context,
    VkResult input_result,
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory) {
    return mImpl->on_vkAllocateMemory(
        context, input_result, device, pAllocateInfo, pAllocator, pMemory);
}

void ResourceTracker::on_vkFreeMemory(
    void* context,
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator) {
    return mImpl->on_vkFreeMemory(
        context, device, memory, pAllocator);
}

VkResult ResourceTracker::on_vkMapMemory(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void** ppData) {
    return mImpl->on_vkMapMemory(
        context, input_result, device, memory, offset, size, flags, ppData);
}

void ResourceTracker::on_vkUnmapMemory(
    void* context,
    VkDevice device,
    VkDeviceMemory memory) {
    mImpl->on_vkUnmapMemory(context, device, memory);
}

VkResult ResourceTracker::on_vkCreateImage(
    void* context, VkResult input_result,
    VkDevice device, const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkImage *pImage) {
    return mImpl->on_vkCreateImage(
        context, input_result,
        device, pCreateInfo, pAllocator, pImage);
}

void ResourceTracker::on_vkDestroyImage(
    void* context,
    VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {
    mImpl->on_vkDestroyImage(context,
        device, image, pAllocator);
}

void ResourceTracker::on_vkGetImageMemoryRequirements(
    void *context, VkDevice device, VkImage image,
    VkMemoryRequirements *pMemoryRequirements) {
    mImpl->on_vkGetImageMemoryRequirements(
        context, device, image, pMemoryRequirements);
}

void ResourceTracker::on_vkGetImageMemoryRequirements2(
    void *context, VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements) {
    mImpl->on_vkGetImageMemoryRequirements2(
        context, device, pInfo, pMemoryRequirements);
}

void ResourceTracker::on_vkGetImageMemoryRequirements2KHR(
    void *context, VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements) {
    mImpl->on_vkGetImageMemoryRequirements2KHR(
        context, device, pInfo, pMemoryRequirements);
}

VkResult ResourceTracker::on_vkBindImageMemory(
    void* context, VkResult input_result,
    VkDevice device, VkImage image, VkDeviceMemory memory,
    VkDeviceSize memoryOffset) {
    return mImpl->on_vkBindImageMemory(
        context, input_result, device, image, memory, memoryOffset);
}

VkResult ResourceTracker::on_vkBindImageMemory2(
    void* context, VkResult input_result,
    VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
    return mImpl->on_vkBindImageMemory2(
        context, input_result, device, bindingCount, pBindInfos);
}

VkResult ResourceTracker::on_vkBindImageMemory2KHR(
    void* context, VkResult input_result,
    VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
    return mImpl->on_vkBindImageMemory2KHR(
        context, input_result, device, bindingCount, pBindInfos);
}

VkResult ResourceTracker::on_vkCreateBuffer(
    void* context, VkResult input_result,
    VkDevice device, const VkBufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkBuffer *pBuffer) {
    return mImpl->on_vkCreateBuffer(
        context, input_result,
        device, pCreateInfo, pAllocator, pBuffer);
}

void ResourceTracker::on_vkDestroyBuffer(
    void* context,
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
    mImpl->on_vkDestroyBuffer(context, device, buffer, pAllocator);
}

void ResourceTracker::on_vkGetBufferMemoryRequirements(
    void* context, VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements) {
    mImpl->on_vkGetBufferMemoryRequirements(context, device, buffer, pMemoryRequirements);
}

void ResourceTracker::on_vkGetBufferMemoryRequirements2(
    void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements) {
    mImpl->on_vkGetBufferMemoryRequirements2(
        context, device, pInfo, pMemoryRequirements);
}

void ResourceTracker::on_vkGetBufferMemoryRequirements2KHR(
    void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemoryRequirements) {
    mImpl->on_vkGetBufferMemoryRequirements2KHR(
        context, device, pInfo, pMemoryRequirements);
}

VkResult ResourceTracker::on_vkBindBufferMemory(
    void* context, VkResult input_result,
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    return mImpl->on_vkBindBufferMemory(
        context, input_result,
        device, buffer, memory, memoryOffset);
}

VkResult ResourceTracker::on_vkBindBufferMemory2(
    void* context, VkResult input_result,
    VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
    return mImpl->on_vkBindBufferMemory2(
        context, input_result,
        device, bindInfoCount, pBindInfos);
}

VkResult ResourceTracker::on_vkBindBufferMemory2KHR(
    void* context, VkResult input_result,
    VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
    return mImpl->on_vkBindBufferMemory2KHR(
        context, input_result,
        device, bindInfoCount, pBindInfos);
}

VkResult ResourceTracker::on_vkCreateSemaphore(
    void* context, VkResult input_result,
    VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSemaphore *pSemaphore) {
    return mImpl->on_vkCreateSemaphore(
        context, input_result,
        device, pCreateInfo, pAllocator, pSemaphore);
}

void ResourceTracker::on_vkDestroySemaphore(
    void* context,
    VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator) {
    mImpl->on_vkDestroySemaphore(context, device, semaphore, pAllocator);
}

VkResult ResourceTracker::on_vkQueueSubmit(
    void* context, VkResult input_result,
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    return mImpl->on_vkQueueSubmit(
        context, input_result, queue, submitCount, pSubmits, fence);
}

VkResult ResourceTracker::on_vkQueueSubmit2(void* context, VkResult input_result, VkQueue queue,
                                            uint32_t submitCount, const VkSubmitInfo2* pSubmits,
                                            VkFence fence) {
    return mImpl->on_vkQueueSubmit2(context, input_result, queue, submitCount, pSubmits, fence);
}

VkResult ResourceTracker::on_vkQueueWaitIdle(
    void* context, VkResult input_result,
    VkQueue queue) {
    return mImpl->on_vkQueueWaitIdle(context, input_result, queue);
}

VkResult ResourceTracker::on_vkGetSemaphoreFdKHR(
    void* context, VkResult input_result,
    VkDevice device,
    const VkSemaphoreGetFdInfoKHR* pGetFdInfo,
    int* pFd) {
    return mImpl->on_vkGetSemaphoreFdKHR(context, input_result, device, pGetFdInfo, pFd);
}

VkResult ResourceTracker::on_vkImportSemaphoreFdKHR(
    void* context, VkResult input_result,
    VkDevice device,
    const VkImportSemaphoreFdInfoKHR* pImportSemaphoreFdInfo) {
    return mImpl->on_vkImportSemaphoreFdKHR(context, input_result, device, pImportSemaphoreFdInfo);
}

void ResourceTracker::unwrap_vkCreateImage_pCreateInfo(
    const VkImageCreateInfo* pCreateInfo,
    VkImageCreateInfo* local_pCreateInfo) {
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    mImpl->unwrap_vkCreateImage_pCreateInfo(pCreateInfo, local_pCreateInfo);
#endif
}

void ResourceTracker::unwrap_vkAcquireImageANDROID_nativeFenceFd(int fd, int* fd_out) {
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    mImpl->unwrap_vkAcquireImageANDROID_nativeFenceFd(fd, fd_out);
#endif
}

void ResourceTracker::unwrap_VkBindImageMemory2_pBindInfos(
        uint32_t bindInfoCount,
        const VkBindImageMemoryInfo* inputBindInfos,
        VkBindImageMemoryInfo* outputBindInfos) {
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    mImpl->unwrap_VkBindImageMemory2_pBindInfos(bindInfoCount, inputBindInfos, outputBindInfos);
#endif
}

#ifdef VK_USE_PLATFORM_FUCHSIA
VkResult ResourceTracker::on_vkGetMemoryZirconHandleFUCHSIA(
    void* context, VkResult input_result,
    VkDevice device,
    const VkMemoryGetZirconHandleInfoFUCHSIA* pInfo,
    uint32_t* pHandle) {
    return mImpl->on_vkGetMemoryZirconHandleFUCHSIA(
        context, input_result, device, pInfo, pHandle);
}

VkResult ResourceTracker::on_vkGetMemoryZirconHandlePropertiesFUCHSIA(
    void* context, VkResult input_result,
    VkDevice device,
    VkExternalMemoryHandleTypeFlagBits handleType,
    uint32_t handle,
    VkMemoryZirconHandlePropertiesFUCHSIA* pProperties) {
    return mImpl->on_vkGetMemoryZirconHandlePropertiesFUCHSIA(
        context, input_result, device, handleType, handle, pProperties);
}

VkResult ResourceTracker::on_vkGetSemaphoreZirconHandleFUCHSIA(
    void* context, VkResult input_result,
    VkDevice device,
    const VkSemaphoreGetZirconHandleInfoFUCHSIA* pInfo,
    uint32_t* pHandle) {
    return mImpl->on_vkGetSemaphoreZirconHandleFUCHSIA(
        context, input_result, device, pInfo, pHandle);
}

VkResult ResourceTracker::on_vkImportSemaphoreZirconHandleFUCHSIA(
    void* context, VkResult input_result,
    VkDevice device,
    const VkImportSemaphoreZirconHandleInfoFUCHSIA* pInfo) {
    return mImpl->on_vkImportSemaphoreZirconHandleFUCHSIA(
        context, input_result, device, pInfo);
}

VkResult ResourceTracker::on_vkCreateBufferCollectionFUCHSIA(
    void* context,
    VkResult input_result,
    VkDevice device,
    const VkBufferCollectionCreateInfoFUCHSIA* pInfo,
    const VkAllocationCallbacks* pAllocator,
    VkBufferCollectionFUCHSIA* pCollection) {
    return mImpl->on_vkCreateBufferCollectionFUCHSIA(
        context, input_result, device, pInfo, pAllocator, pCollection);
}

void ResourceTracker::on_vkDestroyBufferCollectionFUCHSIA(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkBufferCollectionFUCHSIA collection,
    const VkAllocationCallbacks* pAllocator) {
    return mImpl->on_vkDestroyBufferCollectionFUCHSIA(
        context, input_result, device, collection, pAllocator);
}

VkResult ResourceTracker::on_vkSetBufferCollectionBufferConstraintsFUCHSIA(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkBufferCollectionFUCHSIA collection,
    const VkBufferConstraintsInfoFUCHSIA* pBufferDConstraintsInfo) {
    return mImpl->on_vkSetBufferCollectionBufferConstraintsFUCHSIA(
        context, input_result, device, collection, pBufferDConstraintsInfo);
}

VkResult ResourceTracker::on_vkSetBufferCollectionImageConstraintsFUCHSIA(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkBufferCollectionFUCHSIA collection,
    const VkImageConstraintsInfoFUCHSIA* pImageConstraintsInfo) {
    return mImpl->on_vkSetBufferCollectionImageConstraintsFUCHSIA(
        context, input_result, device, collection, pImageConstraintsInfo);
}

VkResult ResourceTracker::on_vkGetBufferCollectionPropertiesFUCHSIA(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkBufferCollectionFUCHSIA collection,
    VkBufferCollectionPropertiesFUCHSIA* pProperties) {
    return mImpl->on_vkGetBufferCollectionPropertiesFUCHSIA(
        context, input_result, device, collection, pProperties);
}
#endif

#ifdef VK_USE_PLATFORM_ANDROID_KHR
VkResult ResourceTracker::on_vkGetAndroidHardwareBufferPropertiesANDROID(
    void* context, VkResult input_result,
    VkDevice device,
    const AHardwareBuffer* buffer,
    VkAndroidHardwareBufferPropertiesANDROID* pProperties) {
    return mImpl->on_vkGetAndroidHardwareBufferPropertiesANDROID(
        context, input_result, device, buffer, pProperties);
}
VkResult ResourceTracker::on_vkGetMemoryAndroidHardwareBufferANDROID(
    void* context, VkResult input_result,
    VkDevice device,
    const VkMemoryGetAndroidHardwareBufferInfoANDROID *pInfo,
    struct AHardwareBuffer** pBuffer) {
    return mImpl->on_vkGetMemoryAndroidHardwareBufferANDROID(
        context, input_result,
        device, pInfo, pBuffer);
}
#endif

VkResult ResourceTracker::on_vkCreateSamplerYcbcrConversion(
    void* context, VkResult input_result,
    VkDevice device,
    const VkSamplerYcbcrConversionCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSamplerYcbcrConversion* pYcbcrConversion) {
    return mImpl->on_vkCreateSamplerYcbcrConversion(
        context, input_result, device, pCreateInfo, pAllocator, pYcbcrConversion);
}

void ResourceTracker::on_vkDestroySamplerYcbcrConversion(
    void* context,
    VkDevice device,
    VkSamplerYcbcrConversion ycbcrConversion,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroySamplerYcbcrConversion(
        context, device, ycbcrConversion, pAllocator);
}

VkResult ResourceTracker::on_vkCreateSamplerYcbcrConversionKHR(
    void* context, VkResult input_result,
    VkDevice device,
    const VkSamplerYcbcrConversionCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSamplerYcbcrConversion* pYcbcrConversion) {
    return mImpl->on_vkCreateSamplerYcbcrConversionKHR(
        context, input_result, device, pCreateInfo, pAllocator, pYcbcrConversion);
}

void ResourceTracker::on_vkDestroySamplerYcbcrConversionKHR(
    void* context,
    VkDevice device,
    VkSamplerYcbcrConversion ycbcrConversion,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroySamplerYcbcrConversionKHR(
        context, device, ycbcrConversion, pAllocator);
}

VkResult ResourceTracker::on_vkCreateSampler(
    void* context, VkResult input_result,
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler) {
    return mImpl->on_vkCreateSampler(
        context, input_result, device, pCreateInfo, pAllocator, pSampler);
}

void ResourceTracker::on_vkGetPhysicalDeviceExternalBufferProperties(
    void* context,
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalBufferInfo* pExternalBufferInfo,
    VkExternalBufferProperties* pExternalBufferProperties) {
    mImpl->on_vkGetPhysicalDeviceExternalBufferProperties(
        context, physicalDevice, pExternalBufferInfo, pExternalBufferProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceExternalFenceProperties(
    void* context,
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo,
    VkExternalFenceProperties* pExternalFenceProperties) {
    mImpl->on_vkGetPhysicalDeviceExternalFenceProperties(
        context, physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceExternalFencePropertiesKHR(
    void* context,
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo,
    VkExternalFenceProperties* pExternalFenceProperties) {
    mImpl->on_vkGetPhysicalDeviceExternalFenceProperties(
        context, physicalDevice, pExternalFenceInfo, pExternalFenceProperties);
}

VkResult ResourceTracker::on_vkCreateFence(
    void* context,
    VkResult input_result,
    VkDevice device,
    const VkFenceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkFence* pFence) {
    return mImpl->on_vkCreateFence(
        context, input_result, device, pCreateInfo, pAllocator, pFence);
}

void ResourceTracker::on_vkDestroyFence(
    void* context,
    VkDevice device,
    VkFence fence,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyFence(
        context, device, fence, pAllocator);
}

VkResult ResourceTracker::on_vkResetFences(
    void* context,
    VkResult input_result,
    VkDevice device,
    uint32_t fenceCount,
    const VkFence* pFences) {
    return mImpl->on_vkResetFences(
        context, input_result, device, fenceCount, pFences);
}

VkResult ResourceTracker::on_vkImportFenceFdKHR(
    void* context,
    VkResult input_result,
    VkDevice device,
    const VkImportFenceFdInfoKHR* pImportFenceFdInfo) {
    return mImpl->on_vkImportFenceFdKHR(
        context, input_result, device, pImportFenceFdInfo);
}

VkResult ResourceTracker::on_vkGetFenceFdKHR(
    void* context,
    VkResult input_result,
    VkDevice device,
    const VkFenceGetFdInfoKHR* pGetFdInfo,
    int* pFd) {
    return mImpl->on_vkGetFenceFdKHR(
        context, input_result, device, pGetFdInfo, pFd);
}

VkResult ResourceTracker::on_vkWaitForFences(
    void* context,
    VkResult input_result,
    VkDevice device,
    uint32_t fenceCount,
    const VkFence* pFences,
    VkBool32 waitAll,
    uint64_t timeout) {
    return mImpl->on_vkWaitForFences(
        context, input_result, device, fenceCount, pFences, waitAll, timeout);
}

VkResult ResourceTracker::on_vkCreateDescriptorPool(
    void* context,
    VkResult input_result,
    VkDevice device,
    const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorPool* pDescriptorPool) {
    return mImpl->on_vkCreateDescriptorPool(
        context, input_result, device, pCreateInfo, pAllocator, pDescriptorPool);
}

void ResourceTracker::on_vkDestroyDescriptorPool(
    void* context,
    VkDevice device,
    VkDescriptorPool descriptorPool,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyDescriptorPool(context, device, descriptorPool, pAllocator);
}

VkResult ResourceTracker::on_vkResetDescriptorPool(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkDescriptorPool descriptorPool,
    VkDescriptorPoolResetFlags flags) {
    return mImpl->on_vkResetDescriptorPool(
        context, input_result, device, descriptorPool, flags);
}

VkResult ResourceTracker::on_vkAllocateDescriptorSets(
    void* context,
    VkResult input_result,
    VkDevice                                    device,
    const VkDescriptorSetAllocateInfo*          pAllocateInfo,
    VkDescriptorSet*                            pDescriptorSets) {
    return mImpl->on_vkAllocateDescriptorSets(
        context, input_result, device, pAllocateInfo, pDescriptorSets);
}

VkResult ResourceTracker::on_vkFreeDescriptorSets(
    void* context,
    VkResult input_result,
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool,
    uint32_t                                    descriptorSetCount,
    const VkDescriptorSet*                      pDescriptorSets) {
    return mImpl->on_vkFreeDescriptorSets(
        context, input_result, device, descriptorPool, descriptorSetCount, pDescriptorSets);
}

VkResult ResourceTracker::on_vkCreateDescriptorSetLayout(
    void* context,
    VkResult input_result,
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout) {
    return mImpl->on_vkCreateDescriptorSetLayout(
        context, input_result, device, pCreateInfo, pAllocator, pSetLayout);
}

void ResourceTracker::on_vkUpdateDescriptorSets(
    void* context,
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies) {
    return mImpl->on_vkUpdateDescriptorSets(
        context, device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
}

VkResult ResourceTracker::on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkDeviceMemory memory,
    uint64_t* pAddress) {
    return mImpl->on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
        context, input_result, device, memory, pAddress);
}

VkResult ResourceTracker::on_vkMapMemoryIntoAddressSpaceGOOGLE(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkDeviceMemory memory,
    uint64_t* pAddress) {
    return mImpl->on_vkMapMemoryIntoAddressSpaceGOOGLE(
        context, input_result, device, memory, pAddress);
}

VkResult ResourceTracker::on_vkCreateDescriptorUpdateTemplate(
    void* context, VkResult input_result,
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    return mImpl->on_vkCreateDescriptorUpdateTemplate(
        context, input_result,
        device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
}

VkResult ResourceTracker::on_vkCreateDescriptorUpdateTemplateKHR(
    void* context, VkResult input_result,
    VkDevice device,
    const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    return mImpl->on_vkCreateDescriptorUpdateTemplateKHR(
        context, input_result,
        device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
}

void ResourceTracker::on_vkUpdateDescriptorSetWithTemplate(
    void* context,
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const void* pData) {
    mImpl->on_vkUpdateDescriptorSetWithTemplate(
        context, device, descriptorSet,
        descriptorUpdateTemplate, pData);
}

VkResult ResourceTracker::on_vkGetPhysicalDeviceImageFormatProperties2(
    void* context, VkResult input_result,
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
    VkImageFormatProperties2* pImageFormatProperties) {
    return mImpl->on_vkGetPhysicalDeviceImageFormatProperties2(
        context, input_result, physicalDevice, pImageFormatInfo,
        pImageFormatProperties);
}

VkResult ResourceTracker::on_vkGetPhysicalDeviceImageFormatProperties2KHR(
    void* context, VkResult input_result,
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
    VkImageFormatProperties2* pImageFormatProperties) {
    return mImpl->on_vkGetPhysicalDeviceImageFormatProperties2KHR(
        context, input_result, physicalDevice, pImageFormatInfo,
        pImageFormatProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceExternalSemaphoreProperties(
    void* context,
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
    VkExternalSemaphoreProperties* pExternalSemaphoreProperties) {
    mImpl->on_vkGetPhysicalDeviceExternalSemaphoreProperties(
        context, physicalDevice, pExternalSemaphoreInfo,
        pExternalSemaphoreProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(
    void* context,
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
    VkExternalSemaphoreProperties* pExternalSemaphoreProperties) {
    mImpl->on_vkGetPhysicalDeviceExternalSemaphoreProperties(
        context, physicalDevice, pExternalSemaphoreInfo,
        pExternalSemaphoreProperties);
}

void ResourceTracker::registerEncoderCleanupCallback(const VkEncoder* encoder, void* handle, ResourceTracker::CleanupCallback callback) {
    mImpl->registerEncoderCleanupCallback(encoder, handle, callback);
}

void ResourceTracker::unregisterEncoderCleanupCallback(const VkEncoder* encoder, void* handle) {
    mImpl->unregisterEncoderCleanupCallback(encoder, handle);
}

void ResourceTracker::onEncoderDeleted(const VkEncoder* encoder) {
    mImpl->onEncoderDeleted(encoder);
}

uint32_t ResourceTracker::syncEncodersForCommandBuffer(VkCommandBuffer commandBuffer, VkEncoder* current) {
    return mImpl->syncEncodersForCommandBuffer(commandBuffer, current);
}

uint32_t ResourceTracker::syncEncodersForQueue(VkQueue queue, VkEncoder* current) {
    return mImpl->syncEncodersForQueue(queue, current);
}

CommandBufferStagingStream::Alloc ResourceTracker::getAlloc() { return mImpl->getAlloc(); }

CommandBufferStagingStream::Free ResourceTracker::getFree() { return mImpl->getFree(); }

VkResult ResourceTracker::on_vkBeginCommandBuffer(
    void* context, VkResult input_result,
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo) {
    return mImpl->on_vkBeginCommandBuffer(
        context, input_result, commandBuffer, pBeginInfo);
}

VkResult ResourceTracker::on_vkEndCommandBuffer(
    void* context, VkResult input_result,
    VkCommandBuffer commandBuffer) {
    return mImpl->on_vkEndCommandBuffer(
        context, input_result, commandBuffer);
}

VkResult ResourceTracker::on_vkResetCommandBuffer(
    void* context, VkResult input_result,
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags) {
    return mImpl->on_vkResetCommandBuffer(
        context, input_result, commandBuffer, flags);
}

VkResult ResourceTracker::on_vkCreateImageView(
    void* context, VkResult input_result,
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView) {
    return mImpl->on_vkCreateImageView(
        context, input_result, device, pCreateInfo, pAllocator, pView);
}

void ResourceTracker::on_vkCmdExecuteCommands(
    void* context,
    VkCommandBuffer commandBuffer,
    uint32_t commandBufferCount,
    const VkCommandBuffer* pCommandBuffers) {
    mImpl->on_vkCmdExecuteCommands(
        context, commandBuffer, commandBufferCount, pCommandBuffers);
}

void ResourceTracker::on_vkCmdBindDescriptorSets(
    void* context,
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t* pDynamicOffsets) {
    mImpl->on_vkCmdBindDescriptorSets(
        context,
        commandBuffer,
        pipelineBindPoint,
        layout,
        firstSet,
        descriptorSetCount,
        pDescriptorSets,
        dynamicOffsetCount,
        pDynamicOffsets);
}

void ResourceTracker::on_vkCmdPipelineBarrier(
    void* context,
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount,
    const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers) {
    mImpl->on_vkCmdPipelineBarrier(
        context,
        commandBuffer,
        srcStageMask,
        dstStageMask,
        dependencyFlags,
        memoryBarrierCount,
        pMemoryBarriers,
        bufferMemoryBarrierCount,
        pBufferMemoryBarriers,
        imageMemoryBarrierCount,
        pImageMemoryBarriers);
}

void ResourceTracker::on_vkDestroyDescriptorSetLayout(
    void* context,
    VkDevice device,
    VkDescriptorSetLayout descriptorSetLayout,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyDescriptorSetLayout(context, device, descriptorSetLayout, pAllocator);
}

VkResult ResourceTracker::on_vkAllocateCommandBuffers(
    void* context,
    VkResult input_result,
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers) {
    return mImpl->on_vkAllocateCommandBuffers(context, input_result, device, pAllocateInfo, pCommandBuffers);
}

#ifdef VK_USE_PLATFORM_ANDROID_KHR
VkResult ResourceTracker::on_vkQueueSignalReleaseImageANDROID(
    void* context,
    VkResult input_result,
    VkQueue queue,
    uint32_t waitSemaphoreCount,
    const VkSemaphore* pWaitSemaphores,
    VkImage image,
    int* pNativeFenceFd) {
    return mImpl->on_vkQueueSignalReleaseImageANDROID(context, input_result, queue, waitSemaphoreCount, pWaitSemaphores, image, pNativeFenceFd);
}
#endif

VkResult ResourceTracker::on_vkCreateGraphicsPipelines(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines) {
    return mImpl->on_vkCreateGraphicsPipelines(context, input_result, device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

void ResourceTracker::deviceMemoryTransform_tohost(
    VkDeviceMemory* memory, uint32_t memoryCount,
    VkDeviceSize* offset, uint32_t offsetCount,
    VkDeviceSize* size, uint32_t sizeCount,
    uint32_t* typeIndex, uint32_t typeIndexCount,
    uint32_t* typeBits, uint32_t typeBitsCount) {
    mImpl->deviceMemoryTransform_tohost(
        memory, memoryCount,
        offset, offsetCount,
        size, sizeCount,
        typeIndex, typeIndexCount,
        typeBits, typeBitsCount);
}

void ResourceTracker::deviceMemoryTransform_fromhost(
    VkDeviceMemory* memory, uint32_t memoryCount,
    VkDeviceSize* offset, uint32_t offsetCount,
    VkDeviceSize* size, uint32_t sizeCount,
    uint32_t* typeIndex, uint32_t typeIndexCount,
    uint32_t* typeBits, uint32_t typeBitsCount) {
    mImpl->deviceMemoryTransform_fromhost(
        memory, memoryCount,
        offset, offsetCount,
        size, sizeCount,
        typeIndex, typeIndexCount,
        typeBits, typeBitsCount);
}

void ResourceTracker::transformImpl_VkExternalMemoryProperties_fromhost(
    VkExternalMemoryProperties* pProperties,
    uint32_t lenAccess) {
    mImpl->transformImpl_VkExternalMemoryProperties_fromhost(pProperties,
                                                             lenAccess);
}

void ResourceTracker::transformImpl_VkExternalMemoryProperties_tohost(
    VkExternalMemoryProperties*, uint32_t) {}

void ResourceTracker::transformImpl_VkImageCreateInfo_fromhost(const VkImageCreateInfo*,
                                                               uint32_t) {}
void ResourceTracker::transformImpl_VkImageCreateInfo_tohost(const VkImageCreateInfo*,
                                                             uint32_t) {}

#define DEFINE_TRANSFORMED_TYPE_IMPL(type)                                  \
    void ResourceTracker::transformImpl_##type##_tohost(type*, uint32_t) {} \
    void ResourceTracker::transformImpl_##type##_fromhost(type*, uint32_t) {}

LIST_TRIVIAL_TRANSFORMED_TYPES(DEFINE_TRANSFORMED_TYPE_IMPL)

}  // namespace vk
}  // namespace gfxstream
