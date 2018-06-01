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
StreamSocket::SetTimeouts(DWORD recvTimeoutMs, DWORD sendTimeoutMs)
{
    LogAssert(m_sock != INVALID_SOCKET);

    if (setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&recvTimeoutMs, sizeof(DWORD)) != 0 ||
        setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&sendTimeoutMs, sizeof(DWORD)) != 0)
    {
        return WSAGetLastError();
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
SslSocket::Accept(StreamSocket *socket, DWORD recvTimeoutMs, DWORD sendTimeoutMs)
{
    SslSocket *pSslSocket = dynamic_cast<SslSocket*>(socket);
    if (NULL == pSslSocket)
    {
        return ERROR_INVALID_PARAMETER;
    }

    DWORD32 status = StreamSocket::Accept(socket, recvTimeoutMs, sendTimeoutMs);
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
SslSocket::Connect(SOCKET sock, DWORD recvTimeoutMs, DWORD sendTimeoutMs)
{
    if (setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&recvTimeoutMs, sizeof(DWORD)) != 0 ||
        setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&sendTimeoutMs, sizeof(DWORD)) != 0)
    {
        return WSAGetLastError();
    }

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
SslSocket::Connect(UInt32 ip, UInt16 port, DWORD recvTimeoutMs, DWORD sendTimeoutMs)
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

    return Connect(m_sock, recvTimeoutMs, sendTimeoutMs);
}

DWORD32 
SslSocket::Connect(const char *pszMachineName, const char *pszPort, DWORD recvTimeoutMs, DWORD sendTimeoutMs)
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
    
    return Connect(m_sock, recvTimeoutMs, sendTimeoutMs);
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
StreamSocket::Connect(const char *pszMachineName, const char* pszPort, DWORD recvTimeoutMs, DWORD sendTimeoutMs)
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

    ret = SetTimeouts(recvTimeoutMs, sendTimeoutMs);
    if (ret != NO_ERROR)
    {
        return ret;
    }
    
    return NO_ERROR;
}

DWORD32
StreamSocket::Connect(UInt32 ip, UInt16 port, DWORD recvTimeoutMs, DWORD sendTimeoutMs)
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

    ret = SetTimeouts(recvTimeoutMs, sendTimeoutMs);
    if (ret != NO_ERROR)
    {
        return ret;
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
StreamSocket::Accept(StreamSocket *socket, DWORD recvTimeoutMs, DWORD sendTimeoutMs)
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

    DWORD32 ret = socket->SetTimeouts(recvTimeoutMs, sendTimeoutMs);
    if (ret != NO_ERROR)
    {
        Log(LogID_Common, LogLevel_Error, "Failed to set socket timeouts after accept", LogTag_LastError, ret, LogTag_End);
        return ret;
    }

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
