// Copyright (C) 2021 The Android Open Source Project
// Copyright (C) 2021 Google Inc.
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

#include "aemu/base/containers/EntityManager.h"

#include <vulkan/vulkan.h>

#include <unordered_set>
#include <vector>

namespace goldfish_vk {

enum DescriptorWriteType {
    Empty = 0,
    ImageInfo = 1,
    BufferInfo = 2,
    BufferView = 3,
    InlineUniformBlock = 4,
    AccelerationStructure = 5,
};

struct DescriptorWrite {
    DescriptorWriteType type;
    VkDescriptorType descriptorType;

    uint32_t dstArrayElement; // Only used for inlineUniformBlock and accelerationStructure.

    union {
        VkDescriptorImageInfo imageInfo;
        VkDescriptorBufferInfo bufferInfo;
        VkBufferView bufferView;
        VkWriteDescriptorSetInlineUniformBlockEXT inlineUniformBlock;
        VkWriteDescriptorSetAccelerationStructureKHR accelerationStructure;
    };

    std::vector<uint8_t> inlineUniformBlockBuffer;
};

using DescriptorWriteTable = std::vector<std::vector<DescriptorWrite>>;

struct DescriptorWriteArrayRange {
    uint32_t begin;
    uint32_t count;
};

using DescriptorWriteDstArrayRangeTable = std::vector<std::vector<DescriptorWriteArrayRange>>;

struct ReifiedDescriptorSet {
    VkDescriptorPool pool;
    VkDescriptorSetLayout setLayout;
    uint64_t poolId;
    bool allocationPending;

    // Indexed first by binding number
    DescriptorWriteTable allWrites;

    // Indexed first by binding number
    DescriptorWriteDstArrayRangeTable pendingWriteArrayRanges;

    // Indexed by binding number
    std::vector<bool> bindingIsImmutableSampler;

    // Copied from the descriptor set layout
    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

struct DescriptorPoolAllocationInfo {
    VkDevice device;
    VkDescriptorPoolCreateFlags createFlags;

    // TODO: This should be in a single fancy data structure of some kind.
    std::vector<uint64_t> freePoolIds;
    std::unordered_set<uint32_t> allocedPoolIds;
    std::unordered_set<VkDescriptorSet> allocedSets;
    uint32_t maxSets;
    uint32_t usedSets;

    // Fine-grained tracking of descriptor counts in individual pools
    struct DescriptorCountInfo {
        VkDescriptorType type;
        uint32_t descriptorCount;
        uint32_t used;
    };
    std::vector<DescriptorCountInfo> descriptorCountInfo;
};

struct DescriptorSetLayoutInfo {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    uint32_t refcount;
};

void clearReifiedDescriptorSet(ReifiedDescriptorSet* set);

void initDescriptorWriteTable(const std::vector<VkDescriptorSetLayoutBinding>& layoutBindings, DescriptorWriteTable& table);

bool isDescriptorTypeImageInfo(VkDescriptorType descType);
bool isDescriptorTypeBufferInfo(VkDescriptorType descType);
bool isDescriptorTypeBufferView(VkDescriptorType descType);
bool isDescriptorTypeInlineUniformBlock(VkDescriptorType descType);
bool isDescriptorTypeAccelerationStructure(VkDescriptorType descType);

void doEmulatedDescriptorWrite(const VkWriteDescriptorSet* write, ReifiedDescriptorSet* toWrite);
void doEmulatedDescriptorCopy(const VkCopyDescriptorSet* copy, const ReifiedDescriptorSet* src, ReifiedDescriptorSet* dst);

void doEmulatedDescriptorImageInfoWriteFromTemplate(
    VkDescriptorType descType,
    uint32_t binding,
    uint32_t dstArrayElement,
    uint32_t count,
    const VkDescriptorImageInfo* imageInfos,
    ReifiedDescriptorSet* set);

void doEmulatedDescriptorBufferInfoWriteFromTemplate(
    VkDescriptorType descType,
    uint32_t binding,
    uint32_t dstArrayElement,
    uint32_t count,
    const VkDescriptorBufferInfo* bufferInfos,
    ReifiedDescriptorSet* set);

void doEmulatedDescriptorBufferViewWriteFromTemplate(
    VkDescriptorType descType,
    uint32_t binding,
    uint32_t dstArrayElement,
    uint32_t count,
    const VkBufferView* bufferViews,
    ReifiedDescriptorSet* set);

void applyDescriptorSetAllocation(VkDescriptorPool pool, VkDescriptorSetLayout setLayout);
void fillDescriptorSetInfoForPool(VkDescriptorPool pool, VkDescriptorSetLayout setLayout, VkDescriptorSet set);
VkResult validateAndApplyVirtualDescriptorSetAllocation(const VkDescriptorSetAllocateInfo* pAllocateInfo, VkDescriptorSet* pSets);

// Returns false if set wasn't found in its pool.
bool removeDescriptorSetFromPool(VkDescriptorSet set, bool usePoolIds);

std::vector<VkDescriptorSet> clearDescriptorPool(VkDescriptorPool pool, bool usePoolIds);

} // namespace goldfish_vk
