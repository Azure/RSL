#include "BufferedSocketReader.h"

BufferedSocketReader::BufferedSocketReader(SOCKET socket) : m_rawSocket(socket)
{
}

BufferedSocketReader::~BufferedSocketReader()
{
    // closesocket(m_rawSocket);          // Do not close. Closed by StreamSocket
    // m_rawSocket = INVALID_SOCKET;
}

int BufferedSocketReader::GetReadBufferDataLength()
{
    return m_bufferedData.GetReadBufferDataLength();
}

LPBYTE BufferedSocketReader::PeekReadBufferData(int *pbDataLength)
{
    return m_bufferedData.PeekReadBufferData(pbDataLength);
}

bool BufferedSocketReader::SkipBytes(int length)
{
    return m_bufferedData.SkipBytes(length);
}

HRESULT BufferedSocketReader::Recv()
{
    int sizeToRead = m_bufferedData.GetMaxSize();
    int length = GetReadBufferDataLength();
    if (length > 0)
    {
        sizeToRead -= length;
        if (sizeToRead > READ_EXTRA_DATA_CHUNK_SIZE)
        {
            // we could in theory read all remaining size. But this will cause the buffered data to be moved around, 
            // it might not be needed all the time
            sizeToRead = READ_EXTRA_DATA_CHUNK_SIZE;
        }

        if (0 == sizeToRead)
        {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }

        HRESULT hr = m_bufferedData.ReserveExtraBytesForNewData(sizeToRead);
        if (FAILED(hr))
        {
            return hr;
        }
    }


    int status = recv(m_rawSocket, (char*)(m_bufferedData.GetNewDataInsertionAddress()), sizeToRead, 0);
    if (SOCKET_ERROR == status)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }
    else if (status == 0)
    {
        return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
    }
    m_bufferedData.AdjustForNewDataLength(status);

    return S_OK;
}

HRESULT BufferedSocketReader::Send(LPBYTE data, int cbLength)
{
    while (0 != cbLength)
    {
        int status = send(m_rawSocket, (char*)data, cbLength, 0);
        if (SOCKET_ERROR == status)
        {
            return HRESULT_FROM_WIN32(WSAGetLastError());
        }

        cbLength -= status;
        data += status;
    }

    return S_OK;
}
