// sslplumbing.h : Defines the entry point for the console application.
//
#include "basic_types.h"

#include <iostream> 
#include <sstream>
#include <string>

#define SECURITY_WIN32
#include <security.h>
#include <Schannel.h>
#include <memory>
#include <assert.h>

#include "Wincrypt.h"
#include "sslimpl.h"

#include "scopeguard.h"
#include "BufferedData.h"
#include "BufferedSocketReader.h"

//#pragma comment(lib, "RSLib_Network.lib")

//#pragma comment(lib, "ws2_32.lib")
//#pragma comment(lib, "Crypt32.lib")

using namespace RSLibImpl;

namespace SslPlumbing
{
    class SChannelSocket : public SSLAuth
    {
    private:
        // Prohibit copy
        SChannelSocket(const SChannelSocket&) : m_bufferedSocket(INVALID_SOCKET) {}

        // Prohibit assignment
        SChannelSocket& operator=(const SChannelSocket&) {}

        std::string m_remoteEndpointName;
        bool m_isServer;
        
        BYTE m_encryptDataBuffer[ENCRYPT_BUFFER_SIZE];
        int m_maxDataSizeEncryptable;

        CRITSEC m_csRecv;
        CRITSEC m_csSend;
        BufferedSocketReader m_bufferedSocket;
        BufferedData m_bufferedDecryptedMessage;

        HRESULT AuthenticateChannel(bool authServer);
        HRESULT ConnectInternal();
        HRESULT RecvAndDecrypt();
        HRESULT WriteChunk(const char *buffer, int len);

    protected:
        virtual HRESULT RunSSPILoop(int *cbInDataLen, LPVOID inData, int *cbOutDataLen, LPVOID outData, OutputType *outType);

    public:
        SChannelSocket(SOCKET socket, LPCSTR remoteEndpointName, bool isServer);
        ~SChannelSocket();

        virtual DWORD Close();

        DWORD Connect();
        DWORD Accept();

        int Write(const char *buffer, int len);
        int Read(char *buf, int maxLen);
    };
}
