//
// NetBuffer.cpp
//      This file provides the implementation for NetBuffer
//

#include "NetBuffer.h"
#include "stdafx.h"
#include "logging.h"

namespace RSLibImpl
{

NetBuffer::NetBuffer()
{
}

NetBuffer::NetBuffer(int bufferSize)
{
    m_BufSize = bufferSize;
    m_Buffer = new char[m_BufSize];
    m_IsFull = false;  
    m_Start = m_End = m_Buffer;
}

NetBuffer::NetBuffer(const NetBuffer &buf)
{
    m_BufSize = buf.m_BufSize;
    m_Buffer = new char[m_BufSize];
    m_Start = m_End = m_Buffer;
    m_IsFull = false;
    
    // Copy valid data from the input NetBuffer
    int avail = buf.ReadAvail();
    buf.CopyTo(m_Buffer, avail);
    Fill(avail);
}

NetBuffer::NetBuffer(char *buffer, int bufferSize)
{
    m_Buffer = buffer;
    m_BufSize = bufferSize;
    m_IsFull = false;  
    m_Start = m_End = m_Buffer;
}
  
NetBuffer::~NetBuffer()
{
    delete[] m_Buffer;
}

void NetBuffer::SetBuffer(char* buf, int bufferSize)
{
    // Make sure there is no un-read data in the buffer
    LogAssert(this->IsEmpty());
    m_Buffer = buf;
    m_BufSize = bufferSize;
    m_IsFull = false;  
    m_Start = m_End = m_Buffer;
}

void NetBuffer::Clear()
{
    m_Start = m_End = m_Buffer = NULL;
    m_BufSize = 0;
    m_IsFull = false;
}

char* NetBuffer::GetBuffer() const
{
    return m_Buffer;
}

int NetBuffer::GetBufSize() const
{
    return m_BufSize;
}

char* NetBuffer::GetBufEnd() const
{
    return m_Buffer + m_BufSize;
}

char* NetBuffer::GetStart() const
{
    return m_Start;
}

char* NetBuffer::GetEnd() const
{
    return m_End;
}

bool NetBuffer::IsFull() const
{
    return m_IsFull;
}

bool NetBuffer::IsEmpty() const
{
    return (m_Start == m_End && !m_IsFull);
}

void NetBuffer::Consume(int bytes)
{
    LogAssert(bytes >= 0 && bytes <= this->GetBufSize());
    m_Start += bytes;
    if (m_Start >= this->GetBufEnd())
        m_Start = m_Start - m_BufSize;
    if (bytes)
    {
        m_IsFull = false;
    }
}
void NetBuffer::Fill(int bytes)
{
    LogAssert(bytes >= 0 && bytes <= this->GetBufSize());
    m_End += bytes; 
    if (m_End >= this->GetBufEnd())
        m_End = m_End - m_BufSize;
    if (bytes && (m_End == m_Start)) 
    { 
        m_IsFull = true;
    }
}

int NetBuffer::ReadAvail() const
{ 
    if (m_IsFull) 
    {
        return m_BufSize;
    }
    int avail = (int)(m_End - m_Start);
    if (avail < 0)
        avail = m_BufSize + avail;
    return avail;
}

int NetBuffer::WriteAvail() const
{
    return (m_BufSize - ReadAvail());
}

int NetBuffer::ContiguousReadAvail() const
{ 
    if (m_IsFull) {
        LogAssert(m_Start == m_End);
        return ((int)(this->GetBufEnd() - m_Start));
    }
    if (m_End >= m_Start) 
    {
        return ((int)(m_End - m_Start));
    }
    else {
        return ((int)(this->GetBufEnd() - m_Start));
    }
}

int NetBuffer::ContiguousWriteAvail() const
{
    if (m_IsFull)
    {
        LogAssert(m_Start == m_End);
        return 0;
    }

    if (m_End >= m_Start)
    {
        return ((int)(this->GetBufEnd() - m_End));
    }
    else
    {
        return ((int)(m_Start - m_End));
    }
}

void NetBuffer::CopyFrom(const char* src, int size, int offset)
{
    LogAssert(WriteAvail() >= (size + offset));
    char* dest = m_End + offset;
    if (dest >= this->GetBufEnd())
    {
        dest = dest - m_BufSize;
    }
    int avail = (int) (this->GetBufEnd() - dest);
    if (avail >= size) {
        memcpy(dest, src, size);
    }
    else {
        memcpy(dest, src, avail);
        memcpy(m_Buffer, src + avail, size - avail);
    }
}

void NetBuffer::CopyTo(char* dest, int size, int offset) const
{
    LogAssert(ReadAvail() >= (size + offset));
    char* src = m_Start + offset;
    if (src >= this->GetBufEnd()) {
        src = src - m_BufSize;
    }
    int avail = (int) (this->GetBufEnd() - src);
    if (avail >= size) {
        memcpy(dest, src, size);
    }
    else {
        memcpy(dest, src, avail);
        memcpy(dest + avail, m_Buffer, size - avail);
    }
}

} // namespace RSLibImpl