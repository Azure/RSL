#pragma once

//
// Packet.h
//      This file declares the following classes:
//
//          PacketHdr
//                  This consists of a size, version, a transaction id
//                  and checksum
//
//          PacketMarshalMemoryManager
//                  This class extends the MemoryManager interface to
//                  reserve space in the beginning for injecting the
//                  packet header
//          Packet
//                  This consists of a packet header and opaque data.
//                  The packet class uses PacketMarshalMemoryManager
//                  to store both the header and the data
//

#include "basic_types.h"
#include "logging.h"
#include "NetBuffer.h"
#include "PacketUtil.h"
#include "BufferPool.h"

namespace RSLibImpl
{

class NetPacketCtx;
class NetPacketSvc;

const UInt32 MaxNetPacketSize = 100 * 1024 * 1024;       // 100MB - max packet size

const UInt32 MaxNetPacketAlertSize = 0;                  // set default size to 0 means no alert                

class PacketHdr
{
public:

    // Public member variables. 
    // IMPORTANT: Update the SerialLen and SetChecksum method below if you 
    // add or remove fields in the header
    UInt32  m_Size;
    UInt32  m_ProtoVersion;
    UInt32  m_Xid;
    UInt64  m_Checksum;

    static const UInt32 SerialLen = sizeof(UInt32) * 3 + sizeof(UInt64);

    PacketHdr();
    PacketHdr(UInt32 size, UInt32 protoVersion, UInt32 xid);

    // Serialize()
    // The following two methods dump the contents of the packet header
    // into the given buffer. The methods return true if the packet
    // was successfully serialized into the buffer, false otherwise.
    bool Serialize(void *buffer, UInt32 bufferLength);
    bool Serialize(NetBuffer *netBuf);

    // SetChecksum
    // The following method writes the given checksum values into
    // its correct position in the serialized header
    static void SetChecksum(UInt64 checksum, void *hdrBuffer, 
                    UInt32 bufferLength);

    // DeSerialize()
    // The following two methods initialize the packet header from the
    // given buffer. The methods return true if the packet
    // was successfully de-serialized from the buffer, false otherwise.
    bool DeSerialize(void *buffer, UInt32 bufferLength);
};

class IMarshalMemoryManager
{
public:
    /* return the current size of the allocated buffer */
    virtual UInt32 GetBufferLength() = 0;

    /* return a pointer to the allocated buffer; this becomes invalid
       if the buffer is resized. */
    virtual void* GetBuffer() = 0;

    /* this call is for the use of clients which are growing the
       buffer: if the space remaining in the current buffer is smaller
       than writePtr+lengthDelta, it will reallocate the buffer to be
       at least of length writePtr+lengthDelta, and potentially larger
       (the standard implementation resizes to the next larger power
       of 2). */
    virtual void EnsureBuffer(UInt32 writePtr, UInt32 lengthDelta) = 0;

    /* this call is for the use of clients which know what total
       buffer size they want: if GetBufferLength() is smaller than
       length, it will typically reallocate the buffer to the smallest
       power of 2 >= length. This call will not necessarily shrink the
       buffer if length is smaller than GetBufferLength(). */
    virtual void ResizeBuffer(UInt32 length) = 0;

    /* the memory manager also keeps track of two pointers within the
       buffer which are typically used for reading and writing. The
       memory manager will assert fail if the valid length is set to
       be larger than the buffer size, or if the read pointer is set
       to be larger than the valid length. If the valid length is
       shrunk to be smaller than the read pointer, the read pointer is
       set to zero. When the buffer is shrunk the new state of both
       pointers is undefined. */
    virtual UInt32 GetReadPointer() = 0;
    virtual void SetReadPointer(UInt32 readPointer) = 0;
    virtual UInt32 GetValidLength() = 0;
    virtual void SetValidLength(UInt32 validLength) = 0;
    virtual ~IMarshalMemoryManager() {};
};

class PacketMarshalMemoryManager : public IMarshalMemoryManager
{
public:

    // PacketMarshalMemoryManager()
    // Create a new PacketMarshalMemoryManager with space for the
    // packet header and default space for data
    PacketMarshalMemoryManager();
    PacketMarshalMemoryManager(UInt32 initialBufferSize);
    PacketMarshalMemoryManager(Buffer *buffer);
    ~PacketMarshalMemoryManager();

    // GetBufferLength
    // Get the length of the buffer available for data (which is 
    // length of the allocated buffer minus the space reserved 
    // for the packet header)
    UInt32 GetBufferLength();


    // GetBuffer
    // Returns a pointer to the start of the data portion of the buffer
    // This becomes invalid if the buffer is resized
    void* GetBuffer();

    // EnsureBuffer
    // This call is for the use of clients which are growing the
    // buffer: if the space remaining in the current buffer for data
    // is smaller than writePtr+lengthDelta, it will potentially
    // reallocate the buffer to be longer than writePtr+lengthDelta
    void EnsureBuffer(UInt32 writePtr, UInt32 lengthDelta);


    // ResizeBuffer
    // this call is for the use of clients which know what total
    // buffer size they want: if GetBufferLength() is smaller than
    // length, it will typically reallocate the buffer to be exactly
    // length bytes long. This call will not necessarily shrink the
    // buffer if length is smaller than GetBufferLength()
    void ResizeBuffer(UInt32 length);

    // Pointer Methods
    // The memory manager also keeps track of two pointers within the
    // buffer which are typically used for reading and writing. The
    // memory manager will assert fail if the valid length is set to
    // be larger than the buffer size, or if the read pointer is set
    // to be larger than the valid length. If the valid length is
    // shrunk to be smaller than the read pointer, the read pointer is
    // set to zero. When the buffer is shrunk the new state of both
    // pointers is undefined
    UInt32 GetReadPointer();
    void SetReadPointer(UInt32 readPointer);
    UInt32 GetValidLength();
    void SetValidLength(UInt32 validLength);

    // GetPacketHdrBufferLength
    // Returns the size of the buffer space reserved for the 
    // packet header
    UInt32 GetPacketHdrLength();

    // GetPacketHdrBuffer()
    // Returns a pointer to the start of the buffer space reserved
    // for the packet header. This pointer becomes invalid if
    // the buffer is resized
    void* GetPacketHdrBuffer();

    // GetPoolBuffer
    // Returns the current pool buffer
    Buffer* GetPoolBuffer();

    // SetPoolBuffer
    // Releases the existing buffer to the buffer pool and replaces
    // it by the given buffer. Returns the old buffer
    Buffer *SetPoolBuffer(Buffer *buffer);

    // ReleasePoolBuffer
    // This method releases the existing buffer and allocates a small
    // new buffer. Returns the buffer just released
    Buffer* ReleasePoolBuffer();

    // Duplicate
    // This method allocates a new buffer, copies the data from the
    // existing buffer into the new buffer, sets the internal buffer
    // to be the new buffer and returns the existing buffer. This 
    // is useful in cases when the packet's buffer is busy (given
    // to the OS for writes, for example) but we need to call the 
    // user back with a Timeout
    Buffer* Duplicate();

private:
    static const UInt32    m_packetHdrLength;
    void*           m_packetHdrBuffer; 
    void*           m_dataBuffer;
    UInt32          m_dataBufferLength;
    UInt32          m_readPointer;
    UInt32          m_validLength;
    Buffer *        m_Buffer;
};

// class Packet
//      A Packet consists of IP addresses for server and/or client hosts,
//      the packet header (as declared above) and data (which is just a
//      IMarshalMemoryManager). A serialized packet consists only of the
//      header and the data (the IP address information can be derived
//      from the connection).
class Packet
{
public:
    PacketHdr                       m_Hdr;
    PacketMarshalMemoryManager      m_MemoryManager;

    Packet(UInt32 maxSize = 0, UInt32 maxAlertSize = 0);
    Packet(Buffer *buffer, UInt32 maxSize = 0, UInt32 maxAlertSize = 0);

    // Setting and Getting server/client's IP address and port number
    // The following methods set and get the server or client side
    // IP addresses and port numbers. The port numbers (in/out) are
    // in host byte order
    void SetServerAddr(UInt32 ip, UInt16 port);
    void SetClientAddr(UInt32 ip, UInt16 port);

    UInt32 GetServerIp();
    UInt16 GetServerPort();

    UInt32 GetClientIp();
    UInt16 GetClientPort();

    // The following two methods return the packet buffer and packet
    // buffer length. This is useful when you want to avoid copying
    // and read the data direclty into the packet buffer. Once the
    // data has been read into the packet't internal buffer, the 
    // caller must call DeSerialize() to correctly resurrect the 
    // headers and other members.
    void *GetPacketBuffer();
    UInt32 GetBufferLength();
    
    // GetPoolBuffer
    // Returns the pool buffer
    Buffer* GetPoolBuffer();

    // SetPoolBuffer
    // Releases the existing buffer to the buffer pool and replaces
    // it by the given buffer
    Buffer* SetPoolBuffer(Buffer *buffer);

    // ResetPoolBuffer
    // This method releases the existing buffer and allocates a small
    // new buffer
    Buffer* ReleasePoolBuffer();

    // GetPacketLength
    // The following method returns the packet length
    UInt32 GetPacketLength();

    // ResizeBuffer
    // This method resizes the packet's buffer (which includes
    // the header) be at least <size>. If the internal buffer
    // is already big enough, it isn't shrunk.
    void ResizeBuffer(UInt32 size);

    // Serialize
    // Serializes the header and returns a pointer to the beginning
    // of the packet. The length of the packet can be fetched using
    // GetPacketLength()
    void* Serialize();

    // DeSerialize
    // This method attempts to create the packet from the data
    // in the given netPoolBuffer. The netBuffer must contain the 
    // full packet otherwise this method throws a LogAssert. If
    // the netBuffer contains more than one packet, this method
    // reads the first packet. This method also computes the
    // packet checksum and verifies it against the checksum
    // already sent in the packet header (by the writer)
    // If ownBuffer is true, packet owns netBuffer after the
    // method returns true
    //
    // RETURN VALUES
    // true:    When a full packet was read and the checksum was
    //          verified to be correct
    // false:   When a packet was read but the the checksum was
    //          bad
    bool DeSerialize(NetPoolBuffer *netBuffer, bool ownBuffer);

    // DeSerializeHeader()
    // The following two methods initialize the packet header from the
    // given buffer. The methods return true if the packet
    // was successfully de-serialized from the buffer, false otherwise.
    // Returns false if the length in the header is greater
    // than the maximum packet size
    bool DeSerializeHeader(void *buffer, UInt32 bufferLength);
    bool DeSerializeHeader(NetBuffer *netBuf);
    
    // Duplicate
    // This method allocates a new buffer, copies the data from the
    // existing buffer into the new buffer, sets the internal buffer
    // to be the new buffer and returns the existing buffer. This 
    // is useful in cases when the packet's buffer is busy (given
    // to the OS for writes, for example) but we need to call the 
    // user back (with Timeout for example)
    Buffer* Duplicate();

    virtual ~Packet();

private:

    bool VerifyChecksum();

    struct sockaddr_in              m_ServerAddr;
    struct sockaddr_in              m_ClientAddr;
    NetPacketCtx m_Ctx;         // The packet context
    UInt32                          m_MaxPacketSize;
    UInt32                          m_MaxPacketAlertSize;

    friend class NetPacketSvc;
};

// PacketFactory
// This class basically provides an abstract way to create and destroy
// packets. It is (and is required to be) thread-safe so that the 
// connections don't have acquire a lock just to be able to create
// and destroy packets
class PacketFactory
{
    public:
    PacketFactory(UInt32 maxPacketSize = 0, UInt32 maxPacketAlertSize = 0);
    virtual ~PacketFactory() {}
    
    virtual Packet* CreatePacket();
    virtual Packet* CreatePacket(Buffer *buffer);
    virtual void    DestroyPacket(Packet *p);

    void SetMaxPacketSize(UInt32 maxPacketSize);
    void SetMaxPacketAlertSize(UInt32 maxPacketAlertSize);

    private:
    UInt32 m_MaxPacketSize;
    UInt32 m_MaxPacketAlertSize;

};

} // namespace RSLibImpl
