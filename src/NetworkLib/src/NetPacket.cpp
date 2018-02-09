//
// Packet.cpp
//      This file implements the PacketHdr, PacketMarshalMemoryManager
//      and Packet classes defined in Packet.h
//

#include "NetPacket.h"
#include "marshal.h"
#include "utils.h"
#include "DynString.h"

namespace RSLibImpl
{

static const UInt32 s_DefaultBufferLength = 2;

//
// PacketHdr
//
PacketHdr::PacketHdr()
{
    m_Size = 0;
    m_ProtoVersion = 0;
    m_Xid = 0;
}

PacketHdr::PacketHdr(UInt32 size, UInt32 protoVersion, UInt32 xid)
{
    m_Size = size;
    m_ProtoVersion = protoVersion;
    m_Xid = xid;
}

bool PacketHdr::Serialize(void *buffer, UInt32 bufferLength)
{
    LogAssert(bufferLength >= PacketHdr::SerialLen ||
              !"Insufficient Buffer Length");

    // Wrap MarshalData around the buffer so we can use the
    // serialization methods
    MarshalData marshalData(buffer, bufferLength, false);
    marshalData.SetMarshaledLength(0);

    // Dump the fields
    marshalData.WriteUInt32(m_Size);
    marshalData.WriteUInt32(m_ProtoVersion);
    marshalData.WriteUInt32(m_Xid);
    marshalData.WriteUInt64(m_Checksum);
    return true;
}

bool PacketHdr::Serialize(NetBuffer *netBuf)
{
    LogAssert(netBuf->WriteAvail() >= PacketHdr::SerialLen ||
              !"Insufficient space in NetBuffer");

    char temp[PacketHdr::SerialLen];
    char *buffer = netBuf->GetEnd();
    bool copyBack = false;
    if (netBuf->ContiguousWriteAvail() < PacketHdr::SerialLen)
    {
        buffer = temp;
        copyBack = true;
    }

    if (!Serialize(buffer, PacketHdr::SerialLen))
    {
        return false;
    }

    if (copyBack)
    {
        netBuf->CopyFrom(buffer, PacketHdr::SerialLen);
    }

    return true;
}

void PacketHdr::SetChecksum(UInt64 checksum, void *hdrBuffer, 
                    UInt32 bufferLength)
{
    LogAssert(bufferLength >= PacketHdr::SerialLen);
    UInt32 offset = sizeof(UInt32) * 3;

    MarshalData marshalData(hdrBuffer, bufferLength, false);
    marshalData.SetMarshaledLength(offset);
    marshalData.WriteUInt64(checksum);
}

bool PacketHdr::DeSerialize(void *buffer, UInt32 bufferLength)
{
    LogAssert(bufferLength >= PacketHdr::SerialLen ||
              !"Insufficient Buffer Length");

    // Wrap MarshalData around the buffer so we can use the
    // serialization methods
    
    MarshalData marshalData(buffer, bufferLength, false);
    marshalData.SetMarshaledLength(bufferLength);
    
    // Extract individual fields
    if (!marshalData.ReadUInt32(&m_Size))
    {
        LogAssert(!"Error Reading Size");
    }

    if (!marshalData.ReadUInt32(&m_ProtoVersion))
    {
        LogAssert(!"Error Reading Protocol Version");
    }

    if (!marshalData.ReadUInt32(&m_Xid))
    {
        LogAssert(!"Error Reading Xid");
    }

    if (!marshalData.ReadUInt64(&m_Checksum))
    {
        LogAssert(!"Error Reading Checksum");
    }

    return true;
}

//
// PacketMarshalMemoryManager
// 


const UInt32 PacketMarshalMemoryManager::m_packetHdrLength = 
                                                    PacketHdr::SerialLen;

PacketMarshalMemoryManager::PacketMarshalMemoryManager()
{
    
    m_Buffer = Netlib::GetBufferPool()->GetBuffer(m_packetHdrLength + 
                                        s_DefaultBufferLength);
    LogAssert(m_Buffer != NULL);
    m_dataBufferLength = m_Buffer->m_Size - m_packetHdrLength;
    m_packetHdrBuffer = m_Buffer->m_Memory;
    m_dataBuffer = (char *) m_packetHdrBuffer + m_packetHdrLength;
    m_readPointer = 0;
    m_validLength = 0;
}

PacketMarshalMemoryManager::PacketMarshalMemoryManager(UInt32 
                                                       initialBufferLength)
{
    m_Buffer = Netlib::GetBufferPool()->GetBuffer(m_packetHdrLength + 
                                        initialBufferLength);
    LogAssert(m_Buffer != NULL);
    m_dataBufferLength = m_Buffer->m_Size - m_packetHdrLength;
    m_packetHdrBuffer = m_Buffer->m_Memory;
    m_dataBuffer = (char *) m_packetHdrBuffer + m_packetHdrLength;
    m_readPointer = 0;
    m_validLength = 0;
}

PacketMarshalMemoryManager::PacketMarshalMemoryManager(Buffer *buffer)
{
    LogAssert(buffer != NULL);
    LogAssert(buffer->m_Size >= m_packetHdrLength);
    m_Buffer = buffer;
    m_dataBufferLength = m_Buffer->m_Size - m_packetHdrLength;
    m_packetHdrBuffer = m_Buffer->m_Memory;
    m_dataBuffer = (char *) m_packetHdrBuffer + m_packetHdrLength;
    m_readPointer = 0;
    m_validLength = 0;
}

PacketMarshalMemoryManager::~PacketMarshalMemoryManager()
{
    Netlib::GetBufferPool()->ReleaseBuffer(m_Buffer);
}

UInt32 PacketMarshalMemoryManager::GetBufferLength()
{
    return m_dataBufferLength;
}

void* PacketMarshalMemoryManager::GetBuffer()
{
    return m_dataBuffer;
}

void PacketMarshalMemoryManager::EnsureBuffer(UInt32 writePtr,
                                                UInt32 lengthDelta)
{
    UInt32 newLen = writePtr + lengthDelta;
    if (newLen > this->GetBufferLength())
    {
        this->ResizeBuffer(newLen);
    }
}

void PacketMarshalMemoryManager::ResizeBuffer(UInt32 length)
{
    if (length > m_dataBufferLength)
    {
        Buffer *newBuffer;
        newBuffer = Netlib::GetBufferPool()->GetBuffer(m_packetHdrLength + length);
        if (newBuffer == NULL)
        {
            LogAssert(!"Allocating a bigger buffer failed");
        }

        // Copy the data from the old buffer to the new buffer
        memcpy(newBuffer->m_Memory, m_Buffer->m_Memory, 
               m_packetHdrLength + m_validLength);
        Netlib::GetBufferPool()->ReleaseBuffer(m_Buffer);
        m_Buffer = newBuffer;

        m_packetHdrBuffer = m_Buffer->m_Memory;
        m_dataBufferLength = m_Buffer->m_Size - m_packetHdrLength;
        m_dataBuffer = (char *) m_packetHdrBuffer + m_packetHdrLength;
    }
}

UInt32 PacketMarshalMemoryManager::GetReadPointer()
{
    return m_readPointer;
}

void PacketMarshalMemoryManager::SetReadPointer(UInt32 readPointer)
{
    LogAssert(readPointer <= this->GetValidLength());
    m_readPointer = readPointer;
}

UInt32 PacketMarshalMemoryManager::GetValidLength()
{
    return this->m_validLength;
}

void PacketMarshalMemoryManager::SetValidLength(UInt32 validLength)
{
    LogAssert(validLength <= m_dataBufferLength);
    m_validLength = validLength;
    if (m_readPointer > validLength)
    {
        m_readPointer = 0;
    }
}

UInt32 PacketMarshalMemoryManager::GetPacketHdrLength()
{
    return m_packetHdrLength;
}

void *PacketMarshalMemoryManager::GetPacketHdrBuffer()
{
    return m_packetHdrBuffer;
}

Buffer *PacketMarshalMemoryManager::GetPoolBuffer()
{
    return m_Buffer;
}

Buffer* PacketMarshalMemoryManager::SetPoolBuffer(Buffer *buffer)
{
    Buffer *oldBuffer = m_Buffer;
    LogAssert(buffer->m_Size >= m_packetHdrLength);
    m_Buffer = buffer;

    m_packetHdrBuffer = m_Buffer->m_Memory;
    m_dataBufferLength = m_Buffer->m_Size - m_packetHdrLength;
    m_dataBuffer = (char *) m_packetHdrBuffer + m_packetHdrLength;
    return oldBuffer;
}

Buffer* PacketMarshalMemoryManager::ReleasePoolBuffer()
{
    Buffer *buffer = Netlib::GetBufferPool()->GetBuffer(m_packetHdrLength + 
                                        s_DefaultBufferLength);
    return this->SetPoolBuffer(buffer);
}

Buffer* PacketMarshalMemoryManager::Duplicate()
{
    Buffer *oldBuffer = m_Buffer;
    Buffer *newBuffer;
    newBuffer = Netlib::GetBufferPool()->GetBuffer(m_Buffer->m_Size);
    if (newBuffer == NULL)
    {
        LogAssert(!"Allocating a buffer failed");
    }

    // Copy the data from the old buffer to the new buffer
    memcpy(newBuffer->m_Memory, m_Buffer->m_Memory, 
               m_packetHdrLength + m_validLength);
    m_Buffer = newBuffer;
    m_packetHdrBuffer = m_Buffer->m_Memory;
    m_dataBufferLength = m_Buffer->m_Size - m_packetHdrLength;
    m_dataBuffer = (char *) m_packetHdrBuffer + m_packetHdrLength;
    return oldBuffer;
}

//
// Packet
//
Packet::Packet(UInt32 maxSize, UInt32 maxAlertSize) :
    m_MaxPacketSize((maxSize) ? maxSize : MaxNetPacketSize), 
    m_MaxPacketAlertSize((maxAlertSize) ? maxAlertSize : MaxNetPacketAlertSize)
{
}

Packet::Packet(Buffer *buffer, UInt32 maxSize, UInt32 maxAlertSize) :
    m_MaxPacketSize((maxSize) ? maxSize : MaxNetPacketSize),
    m_MaxPacketAlertSize((maxAlertSize) ? maxAlertSize : MaxNetPacketAlertSize),
    m_MemoryManager(buffer)
{
}

Packet::~Packet()
{
}

void Packet::SetServerAddr(UInt32 ip, UInt16 port)
{
    m_ServerAddr.sin_addr.s_addr = ip;
    m_ServerAddr.sin_port = port;
}

void Packet::SetClientAddr(UInt32 ip, UInt16 port)
{
    m_ClientAddr.sin_addr.s_addr = ip;
    m_ClientAddr.sin_port = port;
}

UInt32 Packet::GetServerIp()
{
    return m_ServerAddr.sin_addr.s_addr;
}

UInt16 Packet::GetServerPort()
{
    return m_ServerAddr.sin_port;
}

UInt32 Packet::GetClientIp()
{
    return m_ClientAddr.sin_addr.s_addr;
}

UInt16 Packet::GetClientPort()
{
    return m_ClientAddr.sin_port;
}

void *Packet::GetPacketBuffer()
{
    return this->m_MemoryManager.GetPacketHdrBuffer();
}

UInt32 Packet::GetBufferLength()
{
    return this->m_MemoryManager.GetPacketHdrLength() +
        this->m_MemoryManager.GetBufferLength();
}

Buffer* Packet::GetPoolBuffer()
{
    return this->m_MemoryManager.GetPoolBuffer();
}

Buffer* Packet::SetPoolBuffer(Buffer *buffer)
{
    return this->m_MemoryManager.SetPoolBuffer(buffer);
}

Buffer* Packet::ReleasePoolBuffer()
{
    return this->m_MemoryManager.ReleasePoolBuffer();
}

UInt32 Packet::GetPacketLength()
{
    return this->m_MemoryManager.GetPacketHdrLength() +
        this->m_MemoryManager.GetValidLength();
}

void Packet::ResizeBuffer(UInt32 size)
{
    LogAssert(size >= m_MemoryManager.GetPacketHdrLength());
    this->m_MemoryManager.ResizeBuffer(size - PacketHdr::SerialLen);
}

void *Packet::Serialize()
{
    // Before serializing the header, compute and fill in the header
    // fields
    this->m_Hdr.m_Size = PacketHdr::SerialLen + 
                                m_MemoryManager.GetValidLength();
    this->m_Hdr.m_Checksum = 0;

    // Serialize the header into the beginning of the memory manager's
    // buffer
    this->m_Hdr.Serialize(this->m_MemoryManager.GetPacketHdrBuffer(),
        this->m_MemoryManager.GetPacketHdrLength());

    // Compute the packet checksum
    UInt64 checksum = Utils::CalculateChecksum(
                    this->m_MemoryManager.GetPacketHdrBuffer(),
                    (size_t) this->GetPacketLength());

    // Write the checksum
    PacketHdr::SetChecksum(checksum,
                    this->m_MemoryManager.GetPacketHdrBuffer(),
                    this->m_MemoryManager.GetPacketHdrLength());

    return this->m_MemoryManager.GetPacketHdrBuffer();
}

bool Packet::DeSerialize(NetPoolBuffer *netBuf, bool ownBuffer)
{
    if (!DeSerializeHeader(netBuf))
    {
        return false;
    }
    
    if (ownBuffer)
    {
        Buffer *buf = SetPoolBuffer(netBuf->GetPoolBuffer());
        Netlib::GetBufferPool()->ReleaseBuffer(buf);
    }
    else
    {
        m_MemoryManager.ResizeBuffer(m_Hdr.m_Size);
        
        // Copy the packet data in the netBuffer to packet's internal
        // buffer
        netBuf->CopyTo((char *) this->m_MemoryManager.GetPacketHdrBuffer(),
                       m_Hdr.m_Size);
    }
    
    m_MemoryManager.SetValidLength(m_Hdr.m_Size - PacketHdr::SerialLen);
    if (!this->VerifyChecksum())
    {
        // if we took ownership of the Buffer, release it back
        if (ownBuffer)
        {
            ReleasePoolBuffer();
        }
        return false;
    }
    return true;
}

bool Packet::DeSerializeHeader(void *buffer, UInt32 bufferLength)
{
    // Deserialize the header
    if (!m_Hdr.DeSerialize(buffer, bufferLength))
    {
        return false;
    }
    
    if (m_MaxPacketAlertSize > 0 && m_Hdr.m_Size > m_MaxPacketAlertSize)
    {
        Log(LogID_NetlibCorruptPacket, LogLevel_Alert, "Malformed Packet",
            "Packet Size (%u) larger than max message alert size (%u)", 
            m_Hdr.m_Size, m_MaxPacketAlertSize);
    }
    if (m_Hdr.m_Size < PacketHdr::SerialLen || m_Hdr.m_Size > m_MaxPacketSize)
    {
        DynString str;
        for (UInt32 i = 0; i < PacketHdr::SerialLen; i++)
        {
            UInt8 v = *((char*) buffer + i);
            str.AppendF("%02x", (int) v);
        }
        Log(LogID_NetlibCorruptPacket, LogLevel_Warning, "Malformed Packet",
            "Invalid Packet Size (%u) (data: %s)", m_Hdr.m_Size, str.Str());
        return false;
    }
    return true;
}

bool Packet::DeSerializeHeader(NetBuffer *netBuf)
{
    LogAssert(netBuf->ReadAvail() >= PacketHdr::SerialLen ||
              !"Insufficient data in netbuf");

    char temp[PacketHdr::SerialLen];
    char *buffer = netBuf->GetStart();
    if (netBuf->ContiguousReadAvail() < PacketHdr::SerialLen)
    {
        netBuf->CopyTo(temp, PacketHdr::SerialLen);
        buffer = temp;
    }
    return DeSerializeHeader(buffer, PacketHdr::SerialLen);
}

Buffer* Packet::Duplicate()
{
    return this->m_MemoryManager.Duplicate();
}

// VerifyChecksum
// Returns true if packet is valid, false otherwise. This method
// assumes that the m_Hdr is initialized
bool Packet::VerifyChecksum()
{
    // Write a 0 in the checksum field and recompute the checksum
    // to ensure that we received the packet without errors
    UInt64 rcvdChecksum = m_Hdr.m_Checksum;
    PacketHdr::SetChecksum(0, this->m_MemoryManager.GetPacketHdrBuffer(),
                    this->m_MemoryManager.GetPacketHdrLength());
    UInt64 newChecksum = Utils::CalculateChecksum(
                    this->m_MemoryManager.GetPacketHdrBuffer(),
                    (size_t) this->GetPacketLength());
    if (rcvdChecksum != newChecksum)
    {
        return false;
    }
    else
    {
        return true;
    }
}

//
// PacketFactory
//
PacketFactory::PacketFactory(UInt32 maxPacketSize, UInt32 maxPacketAlertSize) :
    m_MaxPacketSize(maxPacketSize), 
    m_MaxPacketAlertSize(maxPacketAlertSize)
{
}

Packet* PacketFactory::CreatePacket()
{
    return new Packet(m_MaxPacketSize, m_MaxPacketAlertSize);
}

Packet *PacketFactory::CreatePacket(Buffer *buffer)
{
    return new Packet(buffer, m_MaxPacketSize, m_MaxPacketAlertSize);
}

void PacketFactory::DestroyPacket(Packet *p)
{
    delete p;
}

void PacketFactory::SetMaxPacketSize(UInt32 maxPacketSize)
{
    m_MaxPacketSize = maxPacketSize;
}

void PacketFactory::SetMaxPacketAlertSize(UInt32 maxPacketAlertSize)
{
    m_MaxPacketAlertSize = maxPacketAlertSize;
}

} // namespace RSLibImpl