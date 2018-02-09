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
    virtual DWORD32 Connect(UInt32 ip, UInt16 port)=0;
    virtual DWORD32 Connect(const char *pszMachineName, const char *pszPort)=0;

    //writes to the stream
    virtual DWORD32 Write(const void *buffer, UInt32 numBytes, UInt32 *bytesSent)=0;
    virtual DWORD32 Write(const void *buffer, UInt32 numBytes)=0;

    //reads from the stream
    virtual DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *received)=0;
    virtual DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *received, bool bShouldReceiveAll)=0;

    //timeouts for reading and sending
    virtual DWORD32 SetTimeouts(int recv, int send)=0;

    //setting flush on speeds up small synchronous packet exchange
    //  sockets - setting flush on sets Naglee off
    virtual DWORD32 SetFlush(bool on)=0;

    //setting fast close speeds up closing sockets
    virtual DWORD32 SetFastClose(bool on)=0;
};

//StreamSocket is the streamized communication class for Sockets
class StreamSocket : public StreamBase
{
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
	virtual DWORD32 Accept(StreamSocket *socket);
	virtual DWORD32 Connect(UInt32 ip, UInt16 port);
	virtual DWORD32 Connect(const char *pszMachineName, const char *pszPort);
	virtual int ReadFromSocket(char *buffer, UInt32 numBytes);
	virtual int WriteToSocket(const char *buffer, UInt32 numBytes);

    DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *received, bool bShouldReceiveAll);
	DWORD32 Write(const void *buffer, UInt32 numBytes, UInt32 *bytesSent);
	DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *received);
	DWORD32 Write(const void *buffer, UInt32 numBytes);

    DWORD32 SetTimeouts(int recv, int send);
    
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

	DWORD32 Connect(SOCKET sock);
public:
    SslSocket();
    ~SslSocket();
	DWORD32 Accept(StreamSocket *socket) override;
	DWORD32 Connect(UInt32 ip, UInt16 port) override;
	DWORD32 Connect(const char *pszMachineName, const char *pszPort) override;

	int ReadFromSocket(char *buffer, UInt32 numBytes) override;
	int WriteToSocket(const char *buffer, UInt32 numBytes) override;
};

// StreamFile implements StreamBase to communicate with two files, one for read and one for write
// filename.in will be opened for read, and filename.out will be opened for write
// Main usage for testing telnet

class StreamFile : public StreamBase
{
    FILE* m_fIncoming;
    FILE* m_fOutgoing;

    bool Disconnect();

 public:
    StreamFile();
    ~StreamFile();

    //connects to the stream, returns an error code on error
    //port is not used
    DWORD32 Connect(UInt32 fileNumber, UInt16 notUsed);
    DWORD32 Connect(const char *pszFileName, const char *pszNotUsed);

    //writes to the stream
    DWORD32 Write(const void *buffer, UInt32 numBytes, UInt32 *bytesSent);
    DWORD32 Write(const void *buffer, UInt32 numBytes);

    //reads from the stream
    DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *received);
    DWORD32 Read(void *buffer, UInt32 numBytes, UInt32 *received, bool bShouldReceiveAll);

    //timeouts for reading and sending
    DWORD32 SetTimeouts(int recv, int send);

    DWORD32 SetFlush(bool /*on*/)
    {
        return (NO_ERROR);
    }

    DWORD32 SetFastClose(bool /*on*/)
    {
        return (NO_ERROR);
    }

};


} // namespace RSLibImpl
