#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h> //streamsocket
#include "basic_types.h"
#include "logging.h"
#include "sslplumbing.h"

namespace RSLibImpl
{

// StreamBase is an abstract base class containing the virtual members which defines access to streams 
// Streams could be from file I/O, sockets, etc.
class StreamBase
{
 protected:
    StreamBase() {};

 public:
    virtual ~StreamBase() {};
    
    //connects to the stream, returns an error code on error
    virtual DWORD32 Connect(UInt32 ip, UInt16 port, DWORD recvTimeoutMs, DWORD sendTimeoutMs)=0;
    virtual DWORD32 Connect(const char *pszMachineName, const char *pszPort, DWORD recvTimeoutMs, DWORD sendTimeoutMs)=0;

    //writes to the stream
    virtual DWORD32 Write(const void *buffer, UInt32 numBytes, UInt32 *bytesSent)=0;
    virtual DWORD32 Write(const void *buffer, UInt32 numBytes)=0;

    //reads from the stream
    virtual DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *received)=0;
    virtual DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *received, bool bShouldReceiveAll)=0;

    //setting flush on speeds up small synchronous packet exchange
    //  sockets - setting flush on sets Naglee off
    virtual DWORD32 SetFlush(bool on)=0;

    //setting fast close speeds up closing sockets
    virtual DWORD32 SetFastClose(bool on)=0;
};

//StreamSocket is the streamized communication class for Sockets
class StreamSocket : public StreamBase
{
private:
    DWORD32 SetTimeouts(DWORD recvTimeoutMs, DWORD sendTimeoutMs);

protected:
	typedef StreamSocket*(__cdecl *CreateStreamSocketCallBack)();
	StreamSocket() : m_sock(INVALID_SOCKET) {}

public:
    SOCKET m_sock;
    sockaddr_in m_addr;
    
    ~StreamSocket();
    DWORD32 BindAndListen(UInt16 port, UInt32 queueSize, UInt32 nRetries, UInt32 delay);
    DWORD32 BindAndListen(UInt32 bindIp, UInt16 port, UInt32 queueSize, UInt32 nRetries, UInt32 delay);
	void Cancel();
	virtual DWORD32 Accept(StreamSocket *socket, DWORD recvTimeoutMs, DWORD sendTimeoutMs);
	virtual DWORD32 Connect(UInt32 ip, UInt16 port, DWORD recvTimeoutMs, DWORD sendTimeoutMs);
	virtual DWORD32 Connect(const char *pszMachineName, const char *pszPort, DWORD recvTimeoutMs, DWORD sendTimeoutMs);
	virtual int ReadFromSocket(char *buffer, UInt32 numBytes);
	virtual int WriteToSocket(const char *buffer, UInt32 numBytes);

    DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *received, bool bShouldReceiveAll);
	DWORD32 Write(const void *buffer, UInt32 numBytes, UInt32 *bytesSent);
	DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *received);
	DWORD32 Write(const void *buffer, UInt32 numBytes);

    DWORD32 InitInternalSock(UInt32 ip, UInt16 port);

    UInt32 GetRemoteIp();
    UInt16 GetRemotePort();

    DWORD32 SetFlush(bool on);

    DWORD32 SetFastClose(bool on);
    
	static StreamSocket* CreateStreamSocket();
};


class SslSocket : public StreamSocket
{
private:
	SslPlumbing::SChannelSocket *m_sslsocket;

	DWORD32 Connect(SOCKET sock, DWORD recvTimeoutMs, DWORD sendTimeoutMs);
public:
    SslSocket();
    ~SslSocket();
	DWORD32 Accept(StreamSocket *socket, DWORD recvTimeoutMs, DWORD sendTimeoutMs) override;
	DWORD32 Connect(UInt32 ip, UInt16 port, DWORD recvTimeoutMs, DWORD sendTimeoutMs) override;
	DWORD32 Connect(const char *pszMachineName, const char *pszPort, DWORD recvTimeoutMs, DWORD sendTimeoutMs) override;

	int ReadFromSocket(char *buffer, UInt32 numBytes) override;
	int WriteToSocket(const char *buffer, UInt32 numBytes) override;
};

} // namespace RSLibImpl
