#pragma once

//
// internal\BufferPool.h
//
// This file defines a buffer and a buffer pool. A buffer simply
// consist of a size and a void *. The buffer pool API allows you
// to request buffers of arbitrary sizes and release them. The 
// buffer pool will internally round the size to the next power of
// 2 and just call system’s allocation functions. Once we have some
// profile of the packet sizes and performance is becoming an issue, 
// we will revisit this and either change the implementation of the
// buffer pool to use a custom memory allocator or implement our own
// heap manager.

#include "basic_types.h"
#include "logging.h"

namespace RSLibImpl
{
    class Buffer
    {
    public:
        UInt32      m_Size;
        void *      m_Memory;
    };

    class BufferPool
    {
    public:
        BufferPool();
        virtual Buffer *GetBuffer(UInt32 size);
        virtual void ReleaseBuffer(Buffer *buffer);
        virtual ~BufferPool();

    private:
        UInt32 NextPowerOfTwo(UInt32 size);
    };

} // namespace RSLibImpl
