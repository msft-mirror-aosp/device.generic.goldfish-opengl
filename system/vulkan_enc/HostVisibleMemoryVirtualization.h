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
#pragma once

#include <vulkan/vulkan.h>

constexpr uint64_t kMegaBtye = 1048576;
// This needs to be a power of 2 that is at least the min alignment needed in HostVisibleMemoryVirtualization.cpp.
constexpr uint64_t kLargestPageSize = 65536;
constexpr uint64_t kDefaultHostMemBlockSize = 16 * kMegaBtye; // 16 mb
constexpr uint64_t kHostVisibleHeapSize = 512 * kMegaBtye;    // 512 mb

struct EmulatorFeatureInfo;

namespace android {
namespace base {
namespace guest {

class SubAllocator;

} // namespace guest
} // namespace base
} // namespace android

namespace goldfish_vk {

class VkEncoder;

bool isHostVisible(const VkPhysicalDeviceMemoryProperties *memoryProps, uint32_t index);

struct HostMemAlloc {
    bool initialized = false;
    VkResult initResult = VK_SUCCESS;
    VkDevice device = nullptr;
    uint32_t memoryTypeIndex = 0;
    VkDeviceSize nonCoherentAtomSize = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize mappedSize = 0;
    uint8_t* mappedPtr = nullptr;
    android::base::guest::SubAllocator* subAlloc = nullptr;
    int rendernodeFd = -1;
    bool boCreated = false;
    uint32_t boHandle = 0;
    uint64_t memoryAddr = 0;
    size_t memorySize = 0;
    bool isDedicated = false;
};

VkResult finishHostMemAllocInit(
    VkEncoder* enc,
    VkDevice device,
    uint32_t memoryTypeIndex,
    VkDeviceSize nonCoherentAtomSize,
    VkDeviceSize mappedSize,
    uint8_t* mappedPtr,
    HostMemAlloc* out);

void destroyHostMemAlloc(
    bool freeMemorySyncSupported,
    VkEncoder* enc,
    VkDevice device,
    HostMemAlloc* toDestroy,
    bool doLock);

struct SubAlloc {
    uint8_t* mappedPtr = nullptr;
    VkDeviceSize subAllocSize = 0;
    VkDeviceSize subMappedSize = 0;

    VkDeviceMemory baseMemory = VK_NULL_HANDLE;
    VkDeviceSize baseOffset = 0;
    android::base::guest::SubAllocator* subAlloc = nullptr;
    VkDeviceMemory subMemory = VK_NULL_HANDLE;
};

void subAllocHostMemory(
    HostMemAlloc* alloc,
    const VkMemoryAllocateInfo* pAllocateInfo,
    SubAlloc* out);

// Returns true if the block would have been emptied.
// In that case, we can then go back and tear down the block itself.
bool subFreeHostMemory(SubAlloc* toFree);

bool canSubAlloc(android::base::guest::SubAllocator* subAlloc, VkDeviceSize size);

} // namespace goldfish_vk
