/*
* Copyright (C) 2021 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "CommandBufferStagingStream.h"

#if PLATFORM_SDK_VERSION < 26
#include <cutils/log.h>
#else
#include <log/log.h>
#endif
#include <cutils/properties.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <vector>

static const size_t kReadSize = 512 * 1024;
static const size_t kWriteOffset = kReadSize;

CommandBufferStagingStream::CommandBufferStagingStream(Alloc&& allocFn, Free&& freeFn)
    : IOStream(1048576),
      m_buf(nullptr),
      m_size(0),
      m_writePos(0),
      m_customAlloc(allocFn),
      m_customFree(freeFn) {
    // custom allocator/free
    if (allocFn && freeFn) {
        m_usingCustomAlloc = true;
        // for custom allocation, allocate metadata memory at the beginning.
        // m_alloc, m_free and m_realloc wraps sync data logic

        // \param size to allocate
        // \return ptr starting at data
        m_alloc = [this](size_t size) -> void* {
            // allocation requested size + sync data size

            // <---sync bytes--><----Data--->
            // |———————————————|————————————|
            // |0|1|2|3|4|5|6|7|............|
            // |———————————————|————————————|
            // ꜛ               ꜛ
            // allocated ptr   ptr to data [dataPtr]
            const size_t totalSize = size + kSyncDataSize;

            unsigned char* dataPtr = static_cast<unsigned char*>(m_customAlloc(totalSize));
            if (!dataPtr) {
                ALOGE("Custom allocation (%zu bytes) failed\n", size);
                return nullptr;
            }

            // set DWORD sync data to 0
            *(reinterpret_cast<uint32_t*>(dataPtr)) = kSyncDataReadComplete;

            // pointer for data starts after sync data
            dataPtr += kSyncDataSize;

            return dataPtr;
        };

        // Free freeMemory(freeFn);
        //  \param dataPtr to free
        m_free = [this](void* dataPtr) {
            // for custom allocation/free, memory holding metadata must be freed
            // <---sync byte---><----Data--->
            // |———————————————|————————————|
            // |0|1|2|3|4|5|6|7|............|
            // |———————————————|————————————|
            // ꜛ               ꜛ
            // ptr to free     ptr to data [dataPtr]
            unsigned char* toFreePtr = static_cast<unsigned char*>(dataPtr);
            toFreePtr -= kSyncDataSize;
            m_customFree(toFreePtr);
        };

        // \param ptr is the data pointer currently allocated
        // \return dataPtr starting at data
        m_realloc = [this](void* ptr, size_t size) -> void* {
            // realloc requires freeing previously allocated memory
            // read sync DWORD to ensure host is done reading this memory
            // before releasing it.

            size_t hostWaits = 0;
            unsigned char* syncDataStart = static_cast<unsigned char*>(ptr) - kSyncDataSize;
            uint32_t* syncDWordPtr = reinterpret_cast<uint32_t*>(syncDataStart);

            while (__atomic_load_n(syncDWordPtr, __ATOMIC_ACQUIRE) != kSyncDataReadComplete) {
                hostWaits++;
                usleep(10);
                if (hostWaits > 1000) {
                    ALOGD("%s: warning, stalled on host decoding on this command buffer stream\n",
                          __func__);
                }
            }

            // for custom allocation/free, memory holding metadata must be copied
            // along with stream data
            // <---sync byte---><----Data--->
            // |———————————————|————————————|
            // |0|1|2|3|4|5|6|7|............|
            // |———————————————|————————————|
            // ꜛ               ꜛ
            // [copyLocation]  ptr to data [ptr]

            const size_t toCopySize = m_writePos + kSyncDataSize;
            unsigned char* copyLocation = static_cast<unsigned char*>(ptr) - kSyncDataSize;
            std::vector<uint8_t> tmp(copyLocation, copyLocation + toCopySize);
            m_free(ptr);

            // get new buffer and copy previous stream data to it
            unsigned char* newBuf = static_cast<unsigned char*>(m_alloc(size));
            if (!newBuf) {
                ALOGE("Custom allocation (%zu bytes) failed\n", size);
                return nullptr;
            }
            // custom allocator will allocate space for metadata too
            // copy previous metadata too
            memcpy(newBuf - kSyncDataSize, tmp.data(), toCopySize);

            return newBuf;
        };
    } else {
        // use default allocators
        m_alloc = [](size_t size) { return malloc(size); };
        m_free = [](void* ptr) { free(ptr); };
        m_realloc = [](void* ptr, size_t size) { return realloc(ptr, size); };
    }
}

CommandBufferStagingStream::~CommandBufferStagingStream() {
    flush();
    if (m_buf) m_free(m_buf);
}

void CommandBufferStagingStream::markFlushing() {
    if (!m_usingCustomAlloc) {
        return;
    }
    // mark read of stream buffer as pending
    uint32_t* syncDWordPtr = reinterpret_cast<uint32_t*>(m_buf - kSyncDataSize);
    __atomic_store_n(syncDWordPtr, kSyncDataReadPending, __ATOMIC_RELEASE);
}

size_t CommandBufferStagingStream::idealAllocSize(size_t len) {
    if (len > 1048576) return len;
    return 1048576;
}

void* CommandBufferStagingStream::allocBuffer(size_t minSize) {
    size_t allocSize = (1048576 < minSize ? minSize : 1048576);
    // Initial case: blank
    if (!m_buf) {
        m_buf = (unsigned char*)m_alloc(allocSize);
        m_size = allocSize;
        return (void*)m_buf;
    }

    // Calculate remaining
    size_t remaining = m_size - m_writePos;
    if (remaining < allocSize) {
        size_t newAllocSize = m_size * 2 + allocSize;
        m_buf = (unsigned char*)m_realloc(m_buf, newAllocSize);
        m_size = newAllocSize;

        return (void*)(m_buf + m_writePos);
    }

    return (void*)(m_buf + m_writePos);
}

int CommandBufferStagingStream::commitBuffer(size_t size)
{
    m_writePos += size;
    return 0;
}

const unsigned char *CommandBufferStagingStream::readFully(void*, size_t) {
    // Not supported
    ALOGE("CommandBufferStagingStream::%s: Fatal: not supported\n", __func__);
    abort();
    return nullptr;
}

const unsigned char *CommandBufferStagingStream::read(void*, size_t*) {
    // Not supported
    ALOGE("CommandBufferStagingStream::%s: Fatal: not supported\n", __func__);
    abort();
    return nullptr;
}

int CommandBufferStagingStream::writeFully(const void*, size_t)
{
    // Not supported
    ALOGE("CommandBufferStagingStream::%s: Fatal: not supported\n", __func__);
    abort();
    return 0;
}

const unsigned char *CommandBufferStagingStream::commitBufferAndReadFully(
    size_t, void *, size_t) {

    // Not supported
    ALOGE("CommandBufferStagingStream::%s: Fatal: not supported\n", __func__);
    abort();
    return nullptr;
}

void CommandBufferStagingStream::getWritten(unsigned char** bufOut, size_t* sizeOut) {
    *bufOut = m_buf;
    *sizeOut = m_writePos;
}

void CommandBufferStagingStream::reset() {
    m_writePos = 0;
    IOStream::rewind();
}
