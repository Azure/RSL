#include "BufferedData.h"

BufferedData::BufferedData() : m_readBufferDataEnd(-1), m_readBufferDataStart(0)
{

}

int BufferedData::GetReadBufferDataLength()
{
    return m_readBufferDataEnd - m_readBufferDataStart + 1;
}

LPBYTE BufferedData::GetData(int *pbDataLength)
{
    LPBYTE result = NULL;
    int length = GetReadBufferDataLength();
    if (0 != length)
    {
        result = m_readBuffer + m_readBufferDataStart;

        if (length > *pbDataLength)
        {
            m_readBufferDataStart += *pbDataLength;
        }
        else
        {
            *pbDataLength = length;
            m_readBufferDataEnd = -1;
            m_readBufferDataStart = 0;
        }
    }

    return result;
}

HRESULT BufferedData::ReserveExtraBytesForNewData(int bLen)
{
    int length = GetReadBufferDataLength();
    if (length + bLen > sizeof(m_readBuffer))
    {
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    }

    int readBufferSize = (int)sizeof(m_readBuffer);
    if (readBufferSize - (m_readBufferDataEnd + 1) < bLen)
    {
        // not enough data at the end. We have space at the start
        //ASSERT(m_readBufferDataStart != 0);
        memmove(m_readBuffer, m_readBuffer + m_readBufferDataStart, length);
        m_readBufferDataStart = 0;
        m_readBufferDataEnd = length - 1;
    }

    return S_OK;
}

LPBYTE BufferedData::GetNewDataInsertionAddress()
{
    return m_readBuffer + m_readBufferDataEnd + 1;
}

void BufferedData::AdjustForNewDataLength(int bLen)
{
    m_readBufferDataEnd += bLen;
}


HRESULT BufferedData::AddData(LPBYTE pBuff, int bLen)
{
    HRESULT hr = ReserveExtraBytesForNewData(bLen);
    if (SUCCEEDED(hr))
    {
        memcpy((LPVOID)GetNewDataInsertionAddress(), (LPVOID)pBuff, bLen);
        AdjustForNewDataLength(bLen);
    }
    return hr;
}

LPBYTE BufferedData::PeekReadBufferData(int *pbDataLength)
{
    LPBYTE result = NULL;
    int length = GetReadBufferDataLength();
    if (0 != length)
    {
        result = m_readBuffer + m_readBufferDataStart;
    }

    if (NULL != pbDataLength)
    {
        *pbDataLength = length;
    }

    return result;
}

bool BufferedData::SkipBytes(int length)
{
    if (GetReadBufferDataLength() >= length)
    {
        m_readBufferDataStart += length;
        if (GetReadBufferDataLength() == 0)
        {
            m_readBufferDataEnd = -1;
            m_readBufferDataStart = 0;
        }

        return true;
    }
    else
    {
        return false;
    }
}

int BufferedData::GetMaxSize()
{
    return (int)sizeof(m_readBuffer);
}
