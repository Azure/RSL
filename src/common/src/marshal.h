#pragma once

#include "basic_types.h"
#include "logging.h"
#include "netpacket.h"
#include <string>
// using namespace std;

namespace RSLibImpl
{

class MarshalMemoryManager : public IMarshalMemoryManager
{
};

class StandardMarshalMemoryManager : public MarshalMemoryManager
{
public:
    /* create a new StandardMarshalMemoryManager with a default buffer
       size. The read pointer and valid length are initialized to
       zero. */
    StandardMarshalMemoryManager();

    /* create a new StandardMarshalMemoryManager with an initial
       buffer of length initialBufferSize. The read pointer and valid
       length are initialized to zero. */
    StandardMarshalMemoryManager(UInt32 initialBufferSize);

    ~StandardMarshalMemoryManager();

    /* return the current size of the allocated buffer */
    UInt32 GetBufferLength();

    /* return a pointer to the allocated buffer; this becomes invalid
       if the buffer is resized. */
    void* GetBuffer();

    /* this call is for the use of clients which are growing the
       buffer: if the space remaining in the current buffer is smaller
       than writePtr+lengthDelta, it will reallocate the buffer to be
       at least of length writePtr+lengthDelta, and potentially larger
       (the standard implementation resizes to the next larger power
       of 2). */
    void EnsureBuffer(UInt32 writePtr, UInt32 lengthDelta);

    /* this call is for the use of clients which know what total
       buffer size they want: if GetBufferLength() is smaller than
       length, it will typically reallocate the buffer to the smallest
       power of 2 >= length. This call will not necessarily shrink the
       buffer if length is smaller than GetBufferLength(). */
    void ResizeBuffer(UInt32 length);

    /* the memory manager also keeps track of two pointers within the
       buffer which are typically used for reading and writing. The
       memory manager will assert fail if the valid length is set to
       be larger than the buffer size, or if the read pointer is set
       to be larger than the valid length. If the valid length is
       shrunk to be smaller than the read pointer, the read pointer is
       set to zero. */
    UInt32 GetReadPointer();
    void SetReadPointer(UInt32 readPointer);
    UInt32 GetValidLength();
    void SetValidLength(UInt32 validLength);

private:
    void*      m_buffer;
    UInt32     m_bufferLength;
    UInt32     m_readPointer;
    UInt32     m_validLength;
};

class FixedMarshalMemoryManager : public MarshalMemoryManager
{
public:
    /* attach the IMarshalMemoryManager to bufferPtr with length
       bufferLength. There will be an assert failure if a client calls
       EnsureBuffer or ResizeBuffer attempting to reserve more than
       bufferLength bytes for the buffer. The read pointer and valid
       length are initialized to zero. */
    FixedMarshalMemoryManager(void* bufferPtr, UInt32 bufferLength);

    /* return the current size of the allocated buffer */
    UInt32 GetBufferLength();

    /* return a pointer to the buffer. */
    void* GetBuffer();

    /* This will generate an assert failure if writePtr+lengthDelta is
       greater than the bufferLength passed in on creation. */
    void EnsureBuffer(UInt32 writePtr, UInt32 lengthDelta);

    /* This will generate an assert failure if length is greater than
       the bufferLength passed in on creation. */
    void ResizeBuffer(UInt32 length);

    /* the memory manager also keeps track of two pointers within the
       buffer which are typically used for reading and writing. The
       memory manager will assert fail if the valid length is set to
       be larger than the buffer size, or if the read pointer is set
       to be larger than the valid length. If the valid length is
       shrunk to be smaller than the read pointer, the read pointer is
       set to zero. */
    UInt32 GetReadPointer();
    void SetReadPointer(UInt32 readPointer);
    UInt32 GetValidLength();
    void SetValidLength(UInt32 validLength);

private:
    void*      m_buffer;
    UInt32     m_bufferLength;
    UInt32     m_readPointer;
    UInt32     m_validLength;
};

/* this class is used when writing a container property. */
class MarshalStartPlaceHolder
{
public:
    MarshalStartPlaceHolder(UInt32 lengthOffset, UInt32 dataStartOffset,
                            bool shortLength);

    UInt32 GetLengthOffset();
    UInt32 GetDataStartOffset();
    bool GetShort();

private:
    /* the offset in the PropertyBag where the length is stored */
    UInt32    m_lengthOffset;
    /* the offset in the PropertyBag which is the start of the
       property value data */
    UInt32    m_dataStartOffset;
    /* m_shortLength is true if the property has a 1-byte length,
       false if it has a 4-byte length */
    bool      m_shortLength;
};

class MarshalData
{
public:
    /* creates a MarshalData object with a
       StandardMarshalMemoryManager object to manage its storage. This
       storage is released when the MarshalData object is deleted */
    MarshalData();

    /* creates a MarshalData object with a
       StandardMarshalMemoryManager object to manage its storage,
       initialized to length initialBufferSize. This storage is
       released when the MarshalData object is deleted */
    MarshalData(UInt32 initialBufferSize);

    /* if copyData is true, this creates a MarshalData object with a
       StandardMarshalMemoryManager object to manage its storage,
       initialized with a copy of buffer and length bufferSize; the
       buffer can be grown to an arbitrary length and will be released
       when the MarshalData object is deleted. If copyData is false,
       this creates a MarshalData object with a
       FixedMarshalMemoryManager pointing at buffer; the buffer cannot
       be grown, and this storage is not released when the MarshalData
       object is deleted. If copyData is false, buffer must not be
       freed while the MarshalData object is in
       scope. GetMarshaledLength() is set to bufferSize by this
       call. */
    MarshalData(void* buffer, UInt32 bufferSize, bool copyData);

    /* creates a MarshalData object with storage managed by
       memoryManager. It is the responsibility of the caller to delete
       memoryManager if necessary after the MarshalData object has
       gone out of scope. You must not delete the memoryManager object
       before the MarshalData object goes out of scope. */
    MarshalData(IMarshalMemoryManager* memoryManager);

    ~MarshalData();

    /* discard the current storage and attach to a buffer of length
       bufferSize. If copyData is false this creates a
       FixedMarshalMemoryManager pointing at buffer; the buffer cannot
       be grown, and buffer is not released when the MarshalData
       object is deleted. If copyData is false, buffer must not be
       freed while the MarshalData object is in
       scope. GetMarshaledLength() is set to bufferSize by this call
       and overshoot is set to false. */
    void Attach(void* buffer, UInt32 bufferSize, bool copyData);

    /* Get a pointer to the buffer holding the marshaled data. This
       pointer is only valid as long as there are no calls to the
       IMarshalMemoryManager which might change the buffer length,
       e.g. EnsureBuffer or ResizeBuffer. */
    void* GetMarshaled();
    /* Get the length of the valid data in the buffer. */
    UInt32 GetMarshaledLength();

    /* Set the length of the valid data in the buffer. If this is
       greater than the current buffer length, the buffer will be
       resized to (at least) length. In practice it will usually be
       resized to exactly length. */
    void SetMarshaledLength(UInt32 length);

    /* returns true if the overshoot flag is set, false otherwise. The
       overshoot flag is set if a read is attempted which would
       overflow the end of the buffer. If GetOvershoot() returns true
       then all subsequent reads will return false until the overshoot
       flag is reset using ResetOvershoot(), Clear(),
       RewindReadPointer() or ResetReadPointer(). */
    bool GetOvershoot();

    /* Reset the object. After this call, SetMarshaledLength is zero,
       the read pointer is reset to the start of the buffer, and the
       overshoot flag is set to false. If clearMemory is true, the
       buffer contents are zeroed. */
    void Clear(bool clearMemory);

    /* reset the overshoot flag to false. */
    void ResetOvershoot();

    /* Return the current read pointer offset */
    UInt32 GetReadPointer();

    /* Sets the current read pointer to offset and returns true if
       offset is <= GetMarshaledLength(), otherwise sets the read
       pointer to zero and returns false. This call resets the
       overshoot pointer to false. */
    bool SetReadPointer(UInt32 offset);

    /* Back the read ptr up length bytes. If length is greater than
       the current value of the read pointer, the read pointer is set
       to the start of the buffer and the call returns false,
       otherwise it returns true. This call resets the overshoot
       pointer to false. */
    bool RewindReadPointer(UInt32 length);

    /* Advance the read ptr length bytes. If length plus the current
       value of the read pointer is greater than the
       GetMarshaledLength(), the read pointer is set to
       GetMarshaledLength() and the call returns false, otherwise it
       returns true. This call resets the overshoot pointer to
       false. */
    bool ForwardReadPointer(UInt32 length);

    /* Back the read ptr up to the start of the buffer.  This call
       resets the overshoot pointer to false. */
    void ResetReadPointer();

    /* Ensure there is enough space in the buffer to append len bytes,
       i.e. GetMarshaledLength() + len <= the size of the buffer. If
       not, the buffer is expanded; the StandardMarshalMemoryManager
       expands the buffer to be of size
       2*(GetMarshaledLength()+len). */
    void EnsureBuffer(UInt32 len);

    /* If GetBufferLength() is smaller than length, it will 
       typically reallocate the buffer to be exactly length 
       bytes long. This call will not necessarily shrink the
       buffer if length is smaller than GetBufferLength(). */
    void ResizeBuffer(UInt32 len);

    /* Returns true if there are at least len bytes remaining to be
       read from the buffer. Otherwise returns false and sets the
       overshoot flag to true. */
    bool TestReadRemaining(UInt32 len);

    /* Append values of different types. The appended value begins at
       offset GetMarshaledLength() in the buffer, and
       GetMarshaledLength() is incremented by the size of the
       object. EnsureBuffer() is called internally to each of these
       calls, so it need not be explicitly invoked by the caller. */
    void WriteUInt8(UInt8 val);
    void WriteBool(bool val);    
    void WriteUInt16(UInt16 val);
    void WriteUInt32(UInt32 val);
    void WriteUInt64(UInt64 val);
    void WriteFloat(float val);
    /* copy length bytes verbatim into the buffer. This call does not
       write length into the buffer explicitly; if you want to precede
       the data blob with a length, call e.g. WriteUInt32() first. */
    void WriteData(UInt32 length, void *data);
    /* write a string into the buffer. This writes a UInt32 value for the
       length followed by the buffer. A NULL string is treated as a string of
       length 0 */
    void WriteString(char *s);
    void WriteString(std::string str);
    /* Array versions of writes. Write an array of data elements in to the
       buffer */
    void WriteUInt32Array(UInt32 count, UInt32* vals);
    void WriteUInt64Array(UInt32 count, UInt64* vals);
    void WriteFloatArray(UInt32 count, float* vals);
    /* start writing a property whose length you don't yet know. When
       you have finished appending all the necessary information,
       called ClosePropertyContainer() which will fill in the length
       field. If shortLength is true, the length field is 1-byte,
       otherwise it is 4-byte. */
    MarshalStartPlaceHolder* StartContainer(bool shortLength);
    /* finish writing a property whose length was not known at the
       start. This call fills in the length field of the property
       whose StartContainer created placeHolder, and then deletes
       placeHolder. If a 1-byte length was used but more than 255
       bytes of data have been written, this call will assert fail. */
    void CloseContainer(MarshalStartPlaceHolder* placeHolder);

    /* read values of different types, starting at the current read
       pointer offset in the buffer and updating the read pointer. If
       GetMarshaledLength() < current read ptr + item length, each
       call will return false and not update the read ptr, otherwise
       it will return true. If a Read* method returns false, then all
       subsequent Read* methods will return false unless
       ResetOvershoot(), ResetReadPointer(), RewindReadPointer() or
       Clear() is called. If a read method returns false, it is
       guaranteed not to modify the value pointed to by pVal. */
    bool ReadUInt8(UInt8 *pVal);
    bool ReadBool(bool * pVal);
    bool ReadUInt16(UInt16 *pVal);
    bool ReadUInt32(UInt32 *pVal);
    bool ReadUInt64(UInt64 *pVal);
    bool ReadFloat(float *pVal);
    /* copy length bytes verbatim out of the buffer and advance the
       read ptr by length. */
    bool ReadData(UInt32 length, void *data);
    /* read a string from the buffer. This reads a UInt32 value for the
       length followed by the actual string. A NULL string is returned for 
       length 0 strings. The routine allocates space for the string that is 
       supposed to be freed by the caller. */
    bool ReadString(char **s);
    bool ReadString(std::string &str);
    /* Array versions of reads. Read an array of data elements from the
       buffer. The caller is supposed to allocate the right amount of 
       space for the arrays before calling this routine. */
    bool ReadUInt32Array(UInt32 count, UInt32* vals);
    bool ReadUInt64Array(UInt32 count, UInt64* vals);
    bool ReadFloatArray(UInt32 count, float* vals);
    /* set *dst to point to the current read ptr location without
       modifying the read ptr. length is still used, since the call
       returns true if there are length valid bytes available and
       false otherwise. */
    bool PeekDataPointer(UInt32 length, void** dst);

private:
    IMarshalMemoryManager*    m_memoryManager;
    bool                     m_selfOwnsMemoryManager;
    bool                     m_overshoot;
};

} // namespace RSLibImpl

