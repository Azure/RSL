#include "BufferPool.h"

namespace RSLibImpl
{

BufferPool::BufferPool()
{
}

BufferPool::~BufferPool()
{
}

Buffer *BufferPool::GetBuffer(UInt32 bufSize)
{
    UInt32 size = NextPowerOfTwo(bufSize);
    LogAssert(size >= bufSize && size < 2 * bufSize);
    Buffer *b = new Buffer();
    b->m_Size = size;
    b->m_Memory = malloc(size);
    if (!b->m_Memory)
    {
        Log(LogID_Netlib, LogLevel_Assert, "Out Of Memory");
        delete b;
        return NULL;
    }
    return b;
}

void BufferPool::ReleaseBuffer(Buffer *buffer)
{
    if (buffer == NULL)
    {
        return;
    }

    if (buffer->m_Size > 0)
    {
        LogAssert(buffer->m_Memory != NULL);
        free(buffer->m_Memory);
    }
    delete buffer;
}

UInt32 BufferPool::NextPowerOfTwo(UInt32 value)
{
    // Check if the value is already a power of 2
    if ((value & (value - 1)) == 0)
    {
        return value;
    }

    // Make sure that the value isn't too big to be rounded
    // up
    LogAssert(value < (1 << 31));

    const UInt32 base[] = {0x2, 0xC, 0xF0, 0xFF00, 0xFFFF0000};
    const UInt32 set[] = {1, 2, 4, 8, 16};

    // This variable will store the highest bit set in the 
    // the value
    register UInt32 highestBitSet = 0;
    for (int i = 4; i >= 0; i--)
    {
        if (value & base[i])
        {
            value >>= set[i];
            highestBitSet |= set[i];
        } 
    }

    return (1 << (highestBitSet + 1));
}

} // namespace RSLibImpl
