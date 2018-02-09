#pragma warning(disable:4512)
#include "StreamIO.h"
#include <ws2tcpip.h>
#include <string>
using namespace std;
using namespace RSLibImpl;

//*******************************************************
//StreamSocket
//*******************************************************

// Break the IO into batches of this size. recv/send fails
// if we try to recv or send huge amount of data in a single
// call (the call fails with WSAENOBUFS). 
static const int IO_BATCH_SIZE = 1024 * 1024 * 4;

StreamSocket::~StreamSocket()
{
    Cancel();
}

DWORD32
StreamSocket::SetTimeouts(int recv, int send)
{
    LogAssert(m_sock != INVALID_SOCKET);
    if (setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv, sizeof(int)) != 0 ||
        setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&send, sizeof(int)) != 0)
    {
        return GetLastError();
    }
    return NO_ERROR;
}

StreamSocket*
StreamSocket::CreateStreamSocket()
{
    if (!SSLAuth::HasAnyThumbprint())
    {
        return new StreamSocket();
    }
    else
    {
        return new SslSocket();
    }
}

SslSocket::SslSocket() : StreamSocket(), m_sslsocket(NULL)
{
}

SslSocket::~SslSocket()
{
    if (m_sslsocket != NULL)
    {
        delete m_sslsocket;
        m_sslsocket = NULL;
    }
}

DWORD32 
SslSocket::Accept(StreamSocket *socket)
{
    SslSocket *pSslSocket = dynamic_cast<SslSocket*>(socket);
    if (NULL == pSslSocket)
    {
        return ERROR_INVALID_PARAMETER;
    }

    DWORD32 status = StreamSocket::Accept(socket);
    if (NO_ERROR == status)
    {
        pSslSocket->m_sslsocket = new SslPlumbing::SChannelSocket(socket->m_sock, NULL, true /* isServer */);
        status = pSslSocket->m_sslsocket->Accept();
        if (NO_ERROR != status)
        {
            delete pSslSocket->m_sslsocket;
            pSslSocket->m_sslsocket = NULL;
        }
    }

    return status;
}

DWORD32
SslSocket::Connect(SOCKET sock)
{
    m_sslsocket = new SslPlumbing::SChannelSocket(sock, NULL, false /* isServer */);
    
    DWORD32 status = m_sslsocket->Connect();
    if (NO_ERROR != status)
    {
        delete m_sslsocket;
        m_sslsocket = NULL;
    }

    return status;
}

DWORD32 
SslSocket::Connect(UInt32 ip, UInt16 port)
{
    DWORD32 ret = InitInternalSock(ip, port);
    if (ret != NO_ERROR)
    {
        return ret;
    }

    if (connect(m_sock, (sockaddr *)&m_addr, sizeof(m_addr)) == SOCKET_ERROR)
    {
        return WSAGetLastError();
    }

    return Connect(m_sock);
}

DWORD32 
SslSocket::Connect(const char *pszMachineName, const char *pszPort)
{
    DWORD32 ret = InitInternalSock(NULL, NULL); //ip and port info comes from getaddrinfo downstairs
    if (ret != NO_ERROR)
    {
        return ret;
    }

    addrinfo aiFlags;
    memset(&aiFlags, 0x00, sizeof(aiFlags));
    aiFlags.ai_socktype = SOCK_STREAM;
    aiFlags.ai_protocol = IPPROTO_TCP;
    aiFlags.ai_family = PF_UNSPEC;
    addrinfo *pRetVal;
    int nRet = getaddrinfo(pszMachineName, pszPort, &aiFlags, &pRetVal);

    if (nRet != 0 || !pRetVal)
    {
        if (nRet != 0)
        {
            return nRet;
        }
        else
        {
            return WSAHOST_NOT_FOUND;
        }
    }

    sockaddr_in *pValue = (sockaddr_in *)pRetVal->ai_addr;

    if (connect(m_sock, (sockaddr *)pValue, sizeof(m_addr)) == SOCKET_ERROR)
    {
        freeaddrinfo(pRetVal);
        return WSAGetLastError();
    }
    
    return Connect(m_sock);
}

int
SslSocket::ReadFromSocket(char *buffer, UInt32 numBytes)
{
    return m_sslsocket->Read(buffer, numBytes);
}

int
SslSocket::WriteToSocket(const char *buffer, UInt32 numBytes)
{
    return m_sslsocket->Write((const char*)buffer, numBytes);
}

UInt32
StreamSocket::GetRemoteIp()
{
    return m_addr.sin_addr.s_addr;
}

UInt16
StreamSocket::GetRemotePort()
{
    return ntohs(m_addr.sin_port);
}

DWORD32
StreamSocket::InitInternalSock(UInt32 ip, UInt16 port)
{
    LogAssert(m_sock == INVALID_SOCKET);
    // NOTE: even though we use this socket for blocking I/O only,
    // WSA_FLAG_OVERLAPPED must be set to use the SO_RCVTIMEO and
    // SO_SNDTIMEO option. Otherwise, these options don't work.

    WORD wVersionRequested;
    WSADATA wsaData;

    wVersionRequested = MAKEWORD(2, 2);

    int lerror = WSAStartup(wVersionRequested, &wsaData);
    if (lerror != 0)
    {
        Log(LogID_Common, LogLevel_Error, "A usable Winsock DLL was not found. WSAStartup Error Code: ", LogTag_LastError, lerror, LogTag_End);
        return lerror;
    }

    m_sock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);
    if (m_sock == INVALID_SOCKET)
    {
        lerror = WSAGetLastError();
        Log(LogID_Common, LogLevel_Error, "Create Socket Failed", LogTag_LastError, lerror, LogTag_End);
        return lerror;
    }

    // It's not a good idea to allow socket handles to be inherited. In particular closesocket() will no longer work to cancel blocking operations such as
    // accept().
    if (!SetHandleInformation((HANDLE)m_sock, HANDLE_FLAG_INHERIT, 0))
    {
        Log(LogID_Common, LogLevel_Info, "SetHandleInformation", LogTag_LastError, GetLastError(), LogTag_End);
    }

    memset(&m_addr, 0, sizeof(sockaddr_in));
    m_addr.sin_family = AF_INET;
    m_addr.sin_addr.s_addr = ip;
    m_addr.sin_port = htons(port);
    return NO_ERROR;
}

DWORD32
StreamSocket::Connect(const char *pszMachineName, const char* pszPort)
{
    DWORD32 ret = InitInternalSock(NULL, NULL); //ip and port info comes from getaddrinfo downstairs
    if (ret != NO_ERROR)
    {
        return ret;
    }

    addrinfo aiFlags;
    memset(&aiFlags, 0x00, sizeof(aiFlags));
    aiFlags.ai_socktype = SOCK_STREAM;
    aiFlags.ai_protocol = IPPROTO_TCP;
    aiFlags.ai_family = PF_UNSPEC;
    addrinfo *pRetVal;
    int nRet = getaddrinfo(pszMachineName, pszPort, &aiFlags, &pRetVal);

    if (nRet != 0 || !pRetVal)
    {
        if (nRet != 0)
        {
            return nRet;
        }
        else
        {
            return WSAHOST_NOT_FOUND;
        }
    }

    sockaddr_in *pValue = (sockaddr_in *)pRetVal->ai_addr;

    if (connect(m_sock, (sockaddr *)pValue, sizeof(m_addr)) == SOCKET_ERROR)
    {
        freeaddrinfo(pRetVal);
        return WSAGetLastError();
    }

    freeaddrinfo(pRetVal);
    return NO_ERROR;
}

DWORD32
StreamSocket::Connect(UInt32 ip, UInt16 port)
{
    DWORD32 ret = InitInternalSock(ip, port);
    if (ret != NO_ERROR)
    {
        return ret;
    }

    if (connect(m_sock, (sockaddr *)&m_addr, sizeof(m_addr)) == SOCKET_ERROR)
    {
        return WSAGetLastError();
    }

    return NO_ERROR;
}

DWORD32
StreamSocket::BindAndListen(UInt16 port, UInt32 queueSize, UInt32 nRetries, UInt32 delay)
{
    return BindAndListen(INADDR_ANY, port, queueSize, nRetries, delay);
}

DWORD32
StreamSocket::BindAndListen(UInt32 bindIp, UInt16 port, UInt32 queueSize, UInt32 nRetries, UInt32 delay)
{
    DWORD32 ret = InitInternalSock(bindIp, port);
    if (ret != NO_ERROR)
    {
        return ret;
    }

    int lerror = NO_ERROR;
    UInt32 i = 0;

    for (; i < nRetries; i++)
    {
        if (bind(m_sock, (struct sockaddr *) &m_addr, sizeof(m_addr)) == SOCKET_ERROR)
        {
            lerror = WSAGetLastError();
            Log(LogID_Common, LogLevel_Error, "Failed to bind, Retrying", LogTag_Port, port, LogTag_LastError, lerror, LogTag_End);
            Sleep(delay * 1000);
        }
        else
        {
            break;
        }
    }

    if (i == nRetries)
    {
        return lerror;
    }

    if (listen(m_sock, queueSize) == SOCKET_ERROR)
    {
        lerror = WSAGetLastError();
        Log(LogID_Common, LogLevel_Error, "Failed to listen", LogTag_Port, port, LogTag_LastError, lerror, LogTag_End);
        return lerror;
    }
    return NO_ERROR;
}

void
StreamSocket::Cancel()
{
    if (m_sock != INVALID_SOCKET && m_sock != NULL)
    {
        closesocket(m_sock);
    }
    m_sock = INVALID_SOCKET;
}

DWORD32
StreamSocket::Accept(StreamSocket *socket)
{
    LogAssert(m_sock != INVALID_SOCKET);
    SOCKET s;
    int addrLen = sizeof(socket->m_addr);

    s = accept(m_sock, (struct sockaddr *)&socket->m_addr, &addrLen);
    if (s == INVALID_SOCKET)
    {
        int lerror = WSAGetLastError();
        Log(LogID_Common, LogLevel_Error, "Failed to accept", LogTag_LastError, lerror, LogTag_End);
        return lerror;
    }

    socket->m_sock = s;
    return NO_ERROR;
}
   
int
StreamSocket::ReadFromSocket(char *buffer, UInt32 numBytes)
{
    return recv(m_sock, buffer, numBytes, 0);
}

int
StreamSocket::WriteToSocket(const char *buffer, UInt32 numBytes)
{
    return send(m_sock, buffer, numBytes, 0);
}

DWORD32
StreamSocket::Write(const void *buffer, UInt32 numBytes, UInt32 *bytesSent)
{
    const char *buf = (const char *)buffer;
    // write the bytes
    const char *bufEnd = buf + numBytes;
    *bytesSent = 0;
    while (buf < bufEnd)
    {
        int todo = min((int)(bufEnd - buf), IO_BATCH_SIZE);
        int bytes = WriteToSocket(buf, todo);
        if (bytes <= 0)
        {
            return WSAGetLastError();
        }
        buf += bytes;
        *bytesSent += bytes;
    }
    return NO_ERROR;
}

DWORD32
StreamSocket::Write(const void *buffer, UInt32 numBytes)
{
    UInt32 written;
    return Write(buffer, numBytes, &written);
}

DWORD32
StreamSocket::Read(void *buffer, UInt32 numBytes, UInt32 *received)
{
    return Read(buffer, numBytes, received, true);
}

DWORD32
StreamSocket::Read(void *buffer, UInt32 numBytes, UInt32 *received, bool bShouldReceiveAll)
{
    char *buf = (char *)buffer;
    char *bufEnd = buf + numBytes;
    *received = 0;
    while (buf < bufEnd)
    {
        int todo = min((int)(bufEnd - buf), IO_BATCH_SIZE);
        int bytes = ReadFromSocket(buf, todo);
        if (bytes <= 0)
        {
            if (bytes < 0)
            {
                return WSAGetLastError();
            }
            else if (*received == 0)
            {
                return ERROR_HANDLE_EOF;
            }
            else
            {
                return NO_ERROR;
            }
        }

        buf += bytes;
        *received += bytes;

        if (!bShouldReceiveAll)
        {
            break; //break if we dont have to receive everything
        }
    }
    return NO_ERROR;
}

DWORD32
StreamSocket::SetFlush(bool on)
{
    BOOL nodelay = (on) ? TRUE : FALSE;
    if (setsockopt(
                   m_sock,
                   IPPROTO_TCP,
                   TCP_NODELAY,
                   (const char*)&nodelay,
                   sizeof(nodelay)))
    {
        return (WSAGetLastError());
    }

    return NO_ERROR;
}

DWORD32
StreamSocket::SetFastClose(bool on)
{
    linger option = {(u_short)(on ? 1 : 0), (u_short)0};
    if (setsockopt(
                   m_sock,
                   SOL_SOCKET,
                   SO_LINGER,
                   (const char*)&option,
                   sizeof(option)))
    {
        return (WSAGetLastError());
    }
    return NO_ERROR;
}

//*******************************************************
//StreamFile
//*******************************************************

StreamFile::StreamFile()
{
    m_fIncoming = NULL;
    m_fOutgoing = NULL;
}

StreamFile::~StreamFile()
{
    Disconnect();
}

//close all the files
bool
StreamFile::Disconnect()
{

    if (m_fIncoming)
    {
        fclose(m_fIncoming);
    }

    if (m_fOutgoing)
    {
        fclose(m_fOutgoing);
    }

    return (!(m_fIncoming || m_fOutgoing));
}

DWORD32
//required for inheritence reasons, no practical use for streamfile
StreamFile::SetTimeouts(int /*recv*/, int /*send*/)
{
    return NO_ERROR;
}
  
DWORD32
StreamFile::Connect(const char *pszFileName, const char* /*pszNotUsed*/)
{
    string outFile = pszFileName;
    outFile.append(".out");
    string inFile = pszFileName;
    inFile.append(".in");

    if (!this->Disconnect())
    {
        return WSASYSCALLFAILURE;
    }

    m_fIncoming = fopen(inFile.c_str(), "rb");
    m_fOutgoing = fopen(outFile.c_str(), "wb");

    if (!m_fIncoming || !m_fOutgoing)
    {
        return WSASYSCALLFAILURE;
    }

    return NO_ERROR;
}

DWORD32
StreamFile::Connect(UInt32 fileNumber, UInt16 /*notUsed*/)
{
    char filename[65];
    _ui64toa_s( fileNumber, filename, ARRAYSIZE(filename), 10);

    return this->Connect(filename, NULL);
}
    
DWORD32
StreamFile::Write(const void *buffer, UInt32 numBytes, UInt32 *bytesSent)
{
    *bytesSent = (UInt32)fwrite(buffer, sizeof(char), (size_t)numBytes, m_fOutgoing);
    if (*bytesSent != numBytes)
    {
        return WSASYSCALLFAILURE;
    }

    return NO_ERROR;
}

DWORD32
StreamFile::Write(const void *buffer, UInt32 numBytes)
{
    UInt32 written;
    return Write(buffer, numBytes, &written);
}

DWORD32
StreamFile::Read(void *buffer, UInt32 numBytes, UInt32 *received)
{
    return Read(buffer, numBytes, received, true);
}

DWORD32
StreamFile::Read(void *buffer, UInt32 numBytes, UInt32 *received, bool /*bShouldReceiveAll*/)
{
    *received = (UInt32)fread(buffer, sizeof(char), (size_t)numBytes, m_fIncoming);
    if (ferror(m_fIncoming))
    {
        return WSASYSCALLFAILURE;
    }

    if (feof(m_fIncoming))
    {
        return ERROR_HANDLE_EOF;
    }

    return NO_ERROR;
}
