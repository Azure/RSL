#include <marshal.h>
#include <stdio.h>
#include <stdlib.h>

using namespace RSLibImpl;

static const UInt32 s_DefaultBufferSize = 32;

MarshalStartPlaceHolder::MarshalStartPlaceHolder(UInt32 lengthOffset,
                                                 UInt32 dataStartOffset,
                                                 bool shortLength)
{
    this->m_lengthOffset = lengthOffset;
    this->m_dataStartOffset = dataStartOffset;
    this->m_shortLength = shortLength;
}

UInt32 MarshalStartPlaceHolder::GetLengthOffset()
{
    return this->m_lengthOffset;
}

UInt32 MarshalStartPlaceHolder::GetDataStartOffset()
{
    return this->m_dataStartOffset;
}

bool MarshalStartPlaceHolder::GetShort()
{
    return this->m_shortLength;
}

StandardMarshalMemoryManager::StandardMarshalMemoryManager()
{
    this->m_bufferLength = s_DefaultBufferSize;
    this->m_buffer = malloc(s_DefaultBufferSize);
    this->m_readPointer = 0;
    this->m_validLength = 0;
}

StandardMarshalMemoryManager::
  StandardMarshalMemoryManager(UInt32 initialBufferSize)
{
    this->m_bufferLength = initialBufferSize;
    this->m_buffer = malloc(initialBufferSize);
    this->m_readPointer = 0;
    this->m_validLength = 0;
}

StandardMarshalMemoryManager::~StandardMarshalMemoryManager()
{
    free(this->m_buffer);
}

UInt32 StandardMarshalMemoryManager::GetBufferLength()
{
    return this->m_bufferLength;
}

void* StandardMarshalMemoryManager::GetBuffer()
{
    return this->m_buffer;
}

void StandardMarshalMemoryManager::EnsureBuffer(UInt32 writePtr,
                                                UInt32 lengthDelta)
{
    UInt32 newLen = writePtr + lengthDelta;

    if (newLen > this->GetBufferLength())
    {
        this->ResizeBuffer(newLen);
    }
}

void StandardMarshalMemoryManager::ResizeBuffer(UInt32 length)
{
    if (length > this->m_bufferLength)
    {
        UInt32 roundedLength = 1;
        void* newPtr;

        /* always allocate a power of 2 */
        while (roundedLength != 0 && roundedLength < length)
        {
            roundedLength = roundedLength << 1;
        }
        if (roundedLength == 0)
        {
            roundedLength = length;
        }

        newPtr = realloc(this->m_buffer, roundedLength);
        if (newPtr == NULL)
        {
            LogAssert(false);
        }
        else
        {
            this->m_buffer = newPtr;
        }
        this->m_bufferLength = roundedLength;
    }
}

UInt32 StandardMarshalMemoryManager::GetReadPointer()
{
    return this->m_readPointer;
}

void StandardMarshalMemoryManager::SetReadPointer(UInt32 readPointer)
{
    LogAssert(readPointer <= this->GetValidLength());
    this->m_readPointer = readPointer;
}

UInt32 StandardMarshalMemoryManager::GetValidLength()
{
    return this->m_validLength;
}

void StandardMarshalMemoryManager::SetValidLength(UInt32 validLength)
{
    LogAssert(validLength <= this->m_bufferLength);
    this->m_validLength = validLength;
    if (this->m_readPointer > validLength)
    {
        this->m_readPointer = 0;
    }
}

FixedMarshalMemoryManager::
  FixedMarshalMemoryManager(void* buffer, UInt32 bufferLength)
{
    this->m_bufferLength = bufferLength;
    this->m_buffer = buffer;
    this->m_readPointer = 0;
    this->m_validLength = 0;
}

UInt32 FixedMarshalMemoryManager::GetBufferLength()
{
    return this->m_bufferLength;
}

void* FixedMarshalMemoryManager::GetBuffer()
{
    return this->m_buffer;
}

void FixedMarshalMemoryManager::EnsureBuffer(UInt32 writePtr,
                                             UInt32 lengthDelta)
{
    LogAssert(writePtr + lengthDelta <= this->m_bufferLength);
}

void FixedMarshalMemoryManager::ResizeBuffer(UInt32 length)
{
    LogAssert(length <= this->m_bufferLength);
}

UInt32 FixedMarshalMemoryManager::GetReadPointer()
{
    return this->m_readPointer;
}

void FixedMarshalMemoryManager::SetReadPointer(UInt32 readPointer)
{
    LogAssert(readPointer <= this->GetValidLength());
    this->m_readPointer = readPointer;
}

UInt32 FixedMarshalMemoryManager::GetValidLength()
{
    return this->m_validLength;
}

void FixedMarshalMemoryManager::SetValidLength(UInt32 validLength)
{
    LogAssert(validLength <= this->m_bufferLength);
    this->m_validLength = validLength;
    if (this->m_readPointer > validLength)
    {
        this->m_readPointer = 0;
    }
}

MarshalData::MarshalData()
{
    this->m_memoryManager = new StandardMarshalMemoryManager();
    this->m_selfOwnsMemoryManager = true;
    this->m_overshoot = false;
}

MarshalData::MarshalData(UInt32 initialBufferSize)
{
    this->m_memoryManager =
        new StandardMarshalMemoryManager(initialBufferSize);
    this->m_selfOwnsMemoryManager = true;
    this->m_overshoot = false;
}

MarshalData::MarshalData(void* buffer, UInt32 bufferSize,
                         bool copyData)
{
    this->m_selfOwnsMemoryManager = false;

    this->Attach(buffer, bufferSize, copyData);
}

MarshalData::MarshalData(IMarshalMemoryManager* memoryManager)
{
    this->m_memoryManager = memoryManager;
    this->m_selfOwnsMemoryManager = false;
    this->m_overshoot = false;
}

MarshalData::~MarshalData()
{
    if (m_selfOwnsMemoryManager)
    {
        delete this->m_memoryManager;
    }
}

void MarshalData::Attach(void* buffer, UInt32 bufferSize, bool copyData)
{
    if (m_selfOwnsMemoryManager)
    {
        delete this->m_memoryManager;
    }

    if (copyData)
    {
        this->m_memoryManager =
            new StandardMarshalMemoryManager(bufferSize);
        ::memcpy(this->m_memoryManager->GetBuffer(), buffer, bufferSize);
    }
    else
    {
        this->m_memoryManager =
            new FixedMarshalMemoryManager(buffer, bufferSize);
    }
    this->m_selfOwnsMemoryManager = true;
    this->SetMarshaledLength(bufferSize);
    this->m_overshoot = false;
}

void* MarshalData::GetMarshaled()
{
    return this->m_memoryManager->GetBuffer();
}

UInt32 MarshalData::GetMarshaledLength()
{
    return this->m_memoryManager->GetValidLength();
}

void MarshalData::SetMarshaledLength(UInt32 length)
{
    this->m_memoryManager->ResizeBuffer(length);
    this->m_memoryManager->SetValidLength(length);
}

bool MarshalData::GetOvershoot()
{
    return this->m_overshoot;
}

void MarshalData::Clear(bool clearMemory)
{
    this->SetMarshaledLength(0);
    this->SetReadPointer(0);
    m_overshoot = false;
    if (clearMemory)
    {
        memset(this->m_memoryManager->GetBuffer(), 0,
               this->m_memoryManager->GetBufferLength());
    }
}

void MarshalData::ResetOvershoot()
{
    this->m_overshoot = false;
}

UInt32 MarshalData::GetReadPointer()
{
    return this->m_memoryManager->GetReadPointer();
}

bool MarshalData::SetReadPointer(UInt32 offset)
{
    UInt32 dataLen = this->GetMarshaledLength();

    this->m_overshoot = false;

    if (offset > dataLen)
    {
        this->m_memoryManager->SetReadPointer(0);
        return false;
    }
    else
    {
        this->m_memoryManager->SetReadPointer(offset);
        return true;
    }

}

bool MarshalData::RewindReadPointer(UInt32 length)
{
    UInt32 readPointer = this->GetReadPointer();
    this->m_overshoot = false;

    if (length >= readPointer)
    {
        this->SetReadPointer(0);
        return false;
    }
    else
    {
        this->SetReadPointer(readPointer - length);
        return true;
    }
}

bool MarshalData::ForwardReadPointer(UInt32 length)
{
    UInt32 dataLen = this->GetMarshaledLength();
    UInt32 readPointer = this->GetReadPointer();

    this->m_overshoot = false;

    if (readPointer + length > dataLen)
    {
        this->SetReadPointer(dataLen);
        return false;
    }
    else
    {
        this->SetReadPointer(readPointer + length);
        return true;
    }
}

void MarshalData::ResetReadPointer()
{
    this->SetReadPointer(0);
    this->m_overshoot = false;
}

void MarshalData::EnsureBuffer(UInt32 len)
{
    this->m_memoryManager->EnsureBuffer(this->GetMarshaledLength(), len);
}

void MarshalData::ResizeBuffer(UInt32 len)
{
    this->m_memoryManager->ResizeBuffer(len);
}

bool MarshalData::TestReadRemaining(UInt32 length)
{
    UInt32 dataLen = this->GetMarshaledLength();

    if (this->m_overshoot || this->GetReadPointer() + length > dataLen)
    {
        this->m_overshoot = true;
        return false;
    }
    else
    {
        return true;
    }
}

void MarshalData::WriteUInt8(UInt8 val)
{
    int len = 1;

    this->EnsureBuffer(len);

    int writePtr = this->GetMarshaledLength();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    data[writePtr + 0] = val;

    this->SetMarshaledLength(writePtr + len);
}

bool MarshalData::ReadUInt8(UInt8* pVal)
{
    UInt32 len = 1;
    UInt32 dataLen = this->GetMarshaledLength();
    UInt32 readPointer = this->GetReadPointer();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    if (this->m_overshoot || readPointer + len > dataLen)
    {
        this->m_overshoot = true;
        return false;
    }

    *pVal = data[readPointer + 0];

    this->SetReadPointer(readPointer + len);

    return true;
}

void MarshalData::WriteBool(bool val)
{
    WriteUInt8(val?1:0);
}

bool MarshalData::ReadBool(bool *pVal)
{
    UInt8 isBool;
    if (! ReadUInt8(&isBool))
    {
        return false;
    }
    *pVal = (isBool!=0);
    return true;
}

void MarshalData::WriteUInt16(UInt16 val)
{
    UInt32 len = 2;

    this->EnsureBuffer(len);

    int writePtr = this->GetMarshaledLength();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    data[writePtr + 0] = (UInt8) ((val >> 0) & 0xff);
    data[writePtr + 1] = (UInt8) ((val >> 8) & 0xff);

    this->SetMarshaledLength(writePtr + len);
}

bool MarshalData::ReadUInt16(UInt16* pVal)
{
    UInt32 len = 2;
    UInt32 dataLen = this->GetMarshaledLength();
    UInt32 readPointer = this->GetReadPointer();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    if (this->m_overshoot || readPointer + len > dataLen)
    {
        this->m_overshoot = true;
        return false;
    }

    *pVal =
        (((UInt16) data[readPointer + 0]) << 0) |
        (((UInt16) data[readPointer + 1]) << 8);

    this->SetReadPointer(readPointer + len);

    return true;
}

void MarshalData::WriteUInt32(UInt32 val)
{
    UInt32 len = 4;

    this->EnsureBuffer(len);

    int writePtr = this->GetMarshaledLength();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    data[writePtr + 0] = (UInt8) ((val >> 0) & 0xff);
    data[writePtr + 1] = (UInt8) ((val >> 8) & 0xff);
    data[writePtr + 2] = (UInt8) ((val >> 16) & 0xff);
    data[writePtr + 3] = (UInt8) ((val >> 24) & 0xff);

    this->SetMarshaledLength(writePtr + len);
}

bool MarshalData::ReadUInt32(UInt32* pVal)
{
    UInt32 len = 4;
    UInt32 dataLen = this->GetMarshaledLength();
    UInt32 readPointer = this->GetReadPointer();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    if (this->m_overshoot || readPointer + len > dataLen)
    {
        this->m_overshoot = true;
        return false;
    }

    *pVal =
        (((UInt32) data[readPointer + 0]) << 0) |
        (((UInt32) data[readPointer + 1]) << 8) |
        (((UInt32) data[readPointer + 2]) << 16) |
        (((UInt32) data[readPointer + 3]) << 24);

    this->SetReadPointer(readPointer + len);

    return true;
}

void MarshalData::WriteUInt32Array(UInt32 count, UInt32 *vals)
{
    for (UInt32 i = 0; i < count; i++)
    {
        WriteUInt32(vals[i]);
    }
}

bool MarshalData::ReadUInt32Array(UInt32 count, UInt32 *vals)
{
    for (UInt32 i = 0; i < count; i++)
    {
        if (!ReadUInt32(&vals[i]))
        {
            return false;
        }
    }

    return true;
}

void MarshalData::WriteUInt64(UInt64 val)
{
    UInt32 len = 8;

    this->EnsureBuffer(len);

    int writePtr = this->GetMarshaledLength();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    data[writePtr + 0] = (UInt8) ((val >> 0) & 0xff);
    data[writePtr + 1] = (UInt8) ((val >> 8) & 0xff);
    data[writePtr + 2] = (UInt8) ((val >> 16) & 0xff);
    data[writePtr + 3] = (UInt8) ((val >> 24) & 0xff);
    data[writePtr + 4] = (UInt8) ((val >> 32) & 0xff);
    data[writePtr + 5] = (UInt8) ((val >> 40) & 0xff);
    data[writePtr + 6] = (UInt8) ((val >> 48) & 0xff);
    data[writePtr + 7] = (UInt8) ((val >> 56) & 0xff);

    this->SetMarshaledLength(writePtr + len);
}

bool MarshalData::ReadUInt64(UInt64* pVal)
{
    UInt32 len = 8;
    UInt32 dataLen = this->GetMarshaledLength();
    UInt32 readPointer = this->GetReadPointer();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    if (this->m_overshoot || readPointer + len > dataLen)
    {
        this->m_overshoot = true;
        return false;
    }

    *pVal =
        (((UInt64) data[readPointer + 0]) << 0) |
        (((UInt64) data[readPointer + 1]) << 8) |
        (((UInt64) data[readPointer + 2]) << 16) |
        (((UInt64) data[readPointer + 3]) << 24) |
        (((UInt64) data[readPointer + 4]) << 32) |
        (((UInt64) data[readPointer + 5]) << 40) |
        (((UInt64) data[readPointer + 6]) << 48) |
        (((UInt64) data[readPointer + 7]) << 56);

    this->SetReadPointer(readPointer + len);

    return true;
}

void MarshalData::WriteUInt64Array(UInt32 count, UInt64 *vals)
{
    for (UInt32 i = 0; i < count; i++)
    {
        WriteUInt64(vals[i]);
    }
}

bool MarshalData::ReadUInt64Array(UInt32 count, UInt64 *vals)
{
    for (UInt32 i = 0; i < count; i++)
    {
        if (!ReadUInt64(&vals[i]))
        {
            return false;
        }
    }

    return true;
}

void MarshalData::WriteFloat(float val)
{
    UInt32* ival = (UInt32 *) &val;
    this->WriteUInt32(*ival);
}

bool MarshalData::ReadFloat(float *pVal)
{
    UInt32 *iVal = (UInt32 *) pVal;
    return this->ReadUInt32(iVal);
}

void MarshalData::WriteFloatArray(UInt32 count, float *vals)
{
    for (UInt32 i = 0; i < count; i++)
    {
        WriteFloat(vals[i]);
    }
}

bool MarshalData::ReadFloatArray(UInt32 count, float *vals)
{
    for (UInt32 i = 0; i < count; i++)
    {
        if (!ReadFloat(&vals[i]))
        {
            return false;
        }
    }

    return true;
}

void MarshalData::WriteData(UInt32 len, void* src)
{
    this->EnsureBuffer(len);

    int writePtr = this->GetMarshaledLength();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    memcpy(data + writePtr, src, len);

    this->SetMarshaledLength(writePtr + len);
}

bool MarshalData::ReadData(UInt32 length, void* dst)
{
    UInt32 dataLen = this->GetMarshaledLength();
    UInt32 readPointer = this->GetReadPointer();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    if (this->m_overshoot || readPointer + (int) length > dataLen)
    {
        this->m_overshoot = true;
        return false;
    }

    memcpy(dst, data + readPointer, length);

    this->SetReadPointer(readPointer + length);

    return true;
}

void MarshalData::WriteString(char *s)
{
    UInt32 length = 0;
    
    if (s != NULL)
    {
        length = (UInt32) strlen(s);
    }
    
    WriteUInt32(length);
    if (length > 0)
    {
        WriteData(length, s);
    }
}

bool MarshalData::ReadString(char **s)
{
    UInt32 length;
    
    *s = NULL;
    if (!ReadUInt32(&length))
    {
        return false;
    }

    if (length == 0)
    {
        return true;
    }
    
    char *result = new char[length + 1];
    if (ReadData(length, result))
    {
        result[length] = '\0';
        *s = result;
        return true;
    }
    else
    {
        delete [] result;
        return false;
    }
}

void MarshalData::WriteString(std::string str)
{
    WriteString((char*)str.c_str());
}

bool MarshalData::ReadString(std::string &str)
{
    char * valueStr;

    if (!ReadString(&valueStr))
    {
        return false; 
    }
    
    str="";

    if (valueStr)
    {
        str = valueStr;
        delete [] valueStr;
    }
    return true;
}

bool MarshalData::PeekDataPointer(UInt32 length, void** dst)
{
    UInt32 dataLen = this->GetMarshaledLength();
    UInt32 readPointer = this->GetReadPointer();
    UInt8* data = (UInt8 *) this->GetMarshaled();

    if (this->m_overshoot || readPointer + (int) length > dataLen)
    {
        this->m_overshoot = true;
        return false;
    }

    *dst = data + readPointer;

    return true;
}

MarshalStartPlaceHolder* MarshalData::StartContainer(bool shortLength)
{
    UInt32 lengthOffset, dataStartOffset;

    lengthOffset = GetMarshaledLength();
    if (shortLength)
    {
        WriteUInt8(0);
    }
    else
    {
        WriteUInt32(0);
    }
    dataStartOffset = GetMarshaledLength();

    return new MarshalStartPlaceHolder(lengthOffset, dataStartOffset,
                                       shortLength);
}

void MarshalData::CloseContainer(MarshalStartPlaceHolder* placeHolder)
{
    UInt32 currentOffset = GetMarshaledLength();
    UInt32 length;

    LogAssert(currentOffset >= placeHolder->GetDataStartOffset());
    length = currentOffset - placeHolder->GetDataStartOffset();

    SetMarshaledLength(placeHolder->GetLengthOffset());

    if (placeHolder->GetShort())
    {
        LogAssert(length < 256);
        WriteUInt8((UInt8) length);
    }
    else
    {
        WriteUInt32(length);
    }

    SetMarshaledLength(currentOffset);

    delete placeHolder;
}
