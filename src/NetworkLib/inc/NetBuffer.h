#pragma once

//
// internal\NetBuffer.h
//      This file defines NetBuffer, which is a circular buffer used
//      to read and write data from/to connections
//

namespace RSLibImpl
{

class NetBuffer {

public:
    NetBuffer();

    // NetBuffer
    // This constructor creates a new buffer of the specified size
    NetBuffer(int bufferSize);

    // NetBuffer
    // Copy constructor for NetBuffer. Creates an internal buffer
    // and copies all the data available (to read) from the input
    // buffer
    NetBuffer(const NetBuffer &buf);
    NetBuffer(char *buffer, int bufferSize);

    // ~NetBuffer
    // Deletes the internal buffer if allocated. No deletes will
    // attempted if the internal buffer was allocated by the caller
    // and set using SetBuffer() (see below).
    virtual ~NetBuffer();

    // SetBuffer
    // Deletes the internal buffer if allocated. Sets the internal
    // buffer to point to the given input buffer and clears internal
    // state (this is equivalent to creating a fresh NetBuffer that
    // uses some pre-allocated memory)
    void    SetBuffer(char * buf, int bufferSize);

    void    Clear();

    // GetBuffer/GetBufSize
    // Return the internal buffer and its size
    char *  GetBuffer() const;
    int     GetBufSize() const;
    char *  GetBufEnd() const;
      
    // GetStart/GetEnd
    // Return the start and end pointer for the data contained in
    // the buffer. Note that since this is a circular buffer, 
    // GetEnd() may be <= GetStart(). Also if GetEnd() == GetStart(),
    // the caller should call IsFull() to check if the NetBuffer is
    // empty or full
    char *  GetStart() const;
    char *  GetEnd() const;

    // IsFull
    // Returns true if the buffer is full, false otherwise
    bool    IsFull() const;

    // IsEmpty
    // Returns true if the buffer is empty, false otherwise
    bool    IsEmpty() const;
    
    // Consume/Fill
    // These methods update the internal state and should be used
    // by the caller when they are done consuming or filling data
    void    Consume(int bytes);
    void    Fill(int bytes);
      
    // ReadAvail
    // Returns the number of bytes available in the buffer that
    // are ready to be read
    int     ReadAvail() const;
    
    // WriteAvail
    // Returns the amount of space available in the buffer
    int     WriteAvail() const;

    // ContiguousReadAvail
    // Returns the number of contiguous bytes of data available 
    // to be read (without having to wrap around the end of the
    // buffer)
    int     ContiguousReadAvail() const;

    // ContiguousWriteAvail
    // Returns the amount of contiguous space available in the 
    // buffer (without having to wrap around the end of the buffer)
    int     ContiguousWriteAvail() const;

    // CopyTo/CopyFrom
    // The methods copy the data to/from a given input buffer to
    // the NetBuffer. They automatically take care of wrapping
    // around if the data/space is not available contiguously
    // Note that these methods do not update the internal state
    // and the caller *must* call Consume() and Fill() methods
    // to update that.
    void    CopyTo(char *buf, int size, int offset = 0) const;
    void    CopyFrom(const char *buf, int size, int offset = 0);

protected:
    char *  m_Buffer;
    int     m_BufSize;
    char *  m_Start;
    char *  m_End;
    bool    m_IsFull;
};

} // namespace RSLibImpl
