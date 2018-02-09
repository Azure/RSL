#pragma once

#include <windows.h>

#define MAX_ENCRYPT_DATA_SIZE (16 * 1024)

// DO NOT MODIFY: following macros directly unless code issue.
// Encryption header size 21
// Encryption trailor size 64
#define ENCRYPT_BUFFER_SIZE (MAX_ENCRYPT_DATA_SIZE + 21 + 64)
#define BUFFERED_DATA_SIZE ENCRYPT_BUFFER_SIZE

class BufferedData
{
private:

    BYTE m_readBuffer[BUFFERED_DATA_SIZE];
    int m_readBufferDataEnd;
    int m_readBufferDataStart;

public:
    BufferedData();

    int GetReadBufferDataLength();

    LPBYTE GetData(int *pbDataLength);

    HRESULT ReserveExtraBytesForNewData(int bLen);

    LPBYTE GetNewDataInsertionAddress();

    void AdjustForNewDataLength(int bLen);

    HRESULT AddData(LPBYTE pBuff, int bLen);

    LPBYTE PeekReadBufferData(int *pbDataLength);

    bool SkipBytes(int length);

    int GetMaxSize();
};
