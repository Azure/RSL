#pragma once

#include <windows.h>
#include <Winsock2.h>
#include "BufferedData.h"

#define READ_EXTRA_DATA_CHUNK_SIZE (4*1024)

class BufferedSocketReader
{
private:

    SOCKET m_rawSocket;
    BufferedData m_bufferedData;

public:
    BufferedSocketReader(SOCKET socket);
    
    ~BufferedSocketReader();

    int GetReadBufferDataLength();

    LPBYTE PeekReadBufferData(int *pbDataLength);

    bool SkipBytes(int length);

    HRESULT Recv();

    HRESULT Send(LPBYTE data, int cbLength);
};
