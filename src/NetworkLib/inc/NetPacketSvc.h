#pragma once

// NetPacketSvc.h
// This file declares the classes needed to write a packet client 
// and server. The two main classes are:
//      PacketHandler - base class of (to-be) user defined handler
//                      class that receives callbacks when packets
//                      are sent or received
//      NetPacketSvc  - provides functinality to run as a server
//                      or a client and to send/receive packets
//


#include "basic_types.h"
#include "logging.h"
#include "NetProcessor.h"
#include "Ptr.h"
#include "HiResTime.h"

namespace RSLibImpl
{

class Packet;
class NetServerAcceptor;
class NetCxn;
class NetCxnTable;
class TimeoutHandler;
class NetPacketCtx;
class NetSendRetry;

// Possible return values for sends (receives are void) and callbacks
// Send Callbacks return TxSuccess when a packet is sent successfully
// within the specified timeout. If the packet couldn't be sent fully
// within the given timeout interval, ProcessSend is called back with
// TxTimedOut. In this case it is possible that the packet was already
// scheduled for I/O (but didn't finish) when timeout happened, so it
// will still eventually get delivered.
enum TxRxStatus
{
    TxSuccess,
    TxTimedOut,
    TxNoConnection,
    TxAbort
};

// SendHandler
// An object of this type must be passed to NetPacketSvc (see
// below) when sending packets. The user must override the 
// ProcessSend virtual methods below which is called for all 
// sent packets with a status indicating one of the conditions 
// above
class SendHandler
{
 public:
    SendHandler() {}
    virtual ~SendHandler() {}
    virtual void ProcessSend(Packet *packet, TxRxStatus status) = 0;
};

// ReceiveHandler
// An object of this type must be passed to NetPacketSvc (see
// below) at startup time. The user must override the 
// ProcessReceive virtual methods below which is called for all 
// received packets with a status indicating one of the conditions 
// above
class ReceiveHandler
{
 public:
    ReceiveHandler() {}
    virtual ~ReceiveHandler() {}
    virtual void ProcessReceive(Packet *packet) = 0;
};

// ConnectHandler
// An object of this must might optionally be pass to NetPacketSvc at
// startup. ProcessConnect is called whenever a connection is established
// or torn. Enum ConnectState signals the state of the connection.
class ConnectHandler
{
    public:

    // For client connections, valid callbacks are:
    // Connecting - The client is going to connect to the server.
    // ConnectFailed - The client failed to connect to the server, or the
    // user closed the connection while the client was trying to connect.
    // Connected - The client is connected to the server
    // DisConnected - the connection to the server broke, or the user
    // closed the connection

    // For Server Connectinos, valid callbacks are:
    // Connected - The server accepted a new connection from the client
    // DisConnected - The remote client or the application closed the connection 
    enum ConnectState
    {
        Connecting,
        ConnectFailed,
        Connected,
        DisConnected,
    };
    
    virtual ~ConnectHandler() {}

    // Ip and Port are the addresses of the remote machine. ProcessConnect may
    // be called on the same call stack as ProcessSend() or CloseConnection().
    // For example, if the user calls Send() and netlib needs to open a new connection,
    // it will callback this handler with ProcessConnect(Connecting) on the same
    // call thread.
    virtual void ProcessConnect(UInt32 ip, UInt16 port, ConnectState state) = 0;
};

class DebugListener : public SendHandler, public ReceiveHandler, public ConnectHandler
{
    public:
    
    virtual ~DebugListener() {}
    
    virtual void ProcessConnect(UInt32 /*ip*/, UInt16 /*port*/, ConnectState /*state*/) {}
};

// NetPacketSvc:
// This class handles all the packet I/O (both on the client 
// side and on the server side). Call StartServer() to start it
// in the server mode. Once started, it will start accepting 
// connections and data on behalf of any clients. It maintains
// a buffer per connection to buffer incoming requests. Call 
// StartClient() to start it in the client mode. This will then
// let the user send packets to arbitrary servers.
class NetPacketSvc
{
public:
    // This constructor attaches a DebugListener to monitor the
    // communication associated with this NetPacketSvc object.
    // Owner of this NetPacketSvc object should keep track of outstanding sends
    // and make sure not to delete the DebugListener until all the sends have completed.
    NetPacketSvc(UInt32 readBufferSize, DebugListener* pListener = NULL);
    ~NetPacketSvc();

    // StartAsServer
    // This method should be called before any other method to 
    // start the NetPacketSvc in the server mode. It starts 
    // istening and accepting connections on the specified port.
    // The handlers should be valid until the object is deleted.
    //
    // RETURN VALUE
    // Returns 0 on success, a non-zero value otherwise
    int     StartAsServer(unsigned short port,
                          SendHandler *sHandler,
                          ReceiveHandler *rHandler,
                          PacketFactory *factory);

    // Same as the above method, with additional ConnectHandler parameter
    int     StartAsServer(unsigned short port,
                          SendHandler *sHandler,
                          ReceiveHandler *rHandler,
                          ConnectHandler *cHandler,
                          PacketFactory *factory);

    // Same API to start server, with IP to bind to for listen socket
    int     StartAsServer(unsigned short port,
                          SendHandler *sHandler,
                          ReceiveHandler *rHandler,
                          ConnectHandler *cHandler,
                          PacketFactory *factory,
                          unsigned int bindIp);

    // StartAsClient
    // This method should be the first one called to start 
    // NetPacketSvc in client mode. Once started, it is ready to
    // send and receive packets to/from servers
    // The handlers should be valid until the object is deleted.
    //
    // RETURN VALUE
    // Returns 0 on success, a non-zero value otherwise
    int     StartAsClient(SendHandler *sHandler,
                          ReceiveHandler *rHandler,
                          PacketFactory *factory);

    // Same as the above method, with additional ConnectHandler parameter
    int     StartAsClient(SendHandler *sHandler,
                          ReceiveHandler *rHandler,
                          ConnectHandler *cHandler,
                          PacketFactory *factory);

    // Same as the above method, with additional ConnectHandler parameter
    int     StartAsClient(SendHandler *sHandler,
                          ReceiveHandler *rHandler,
                          ConnectHandler *cHandler,
                          PacketFactory *factory,
                          unsigned int bindIp);

    // Send
    // Send a packet. On this is running in the server mode, we 
    // try to check if a connection with the client already exists
    // (using the client ip address and port number values in the
    // packet. If so, the packet is sent on that connection, 
    // othwerwise the method returns <TxNoConnection>. If this is 
    // running in the client mode, the connection to the server is
    // fetched (created if it doesn't exist already) and the packet
    // is sent to the server.
    //
    // RETURN VALUE
    // TxSuccess - sendHandler->ProcessSend() will be called later.
    // TxAbort - If the service is stopped
    // TxNoConnection (Server Only) - If the connection to the client
    //          doesn't exist
    //
    // NOTES
    // The handler is never called on the caller's call stack
    TxRxStatus  Send(Packet *packet, DWORD timeout);

    // Same as the above send. For Client, if sendOnExisting is true
    // and the connection to the server is closed or does not exist,
    // Send will fail.
    TxRxStatus  Send(Packet *packet, DWORD timeout, bool sendOnExisting);

    // SuspendReceive
    // Suspends any further calls to PacketHandler::ProcessReceive
    // until a call to ResumeReceive(). This call waits to acquire
    // the locks for all the connections
    void    SuspendReceive();

    // ResumeReceive
    // Resumes an already suspended service. This call enable all
    // the connections but makes sure that no ProcessReceive()s are
    // called on the user's call stack
    void    ResumeReceive();

    // Stop
    // This stops the client/server. All connections are closed and
    // all outstanding sends and receives are called back with a
    // TxAbort
    //
    // NOTES
    // The caller is not called back on the same call stack
    void    Stop();


    // CloseConnection
    // This method looks up the connection specified by the given
    // IP addr and port number and if found, closes it. This is a
    // blocking call. The user is called back with a status of
    // TxAbort for all outstanding sends (although on a different
    // call stack).
    //
    // The user is NOT guaranteed to receive all the TxAbort 
    // call-backs before the call to CloseConnection completes. 
    // CloseConnection() will remove the connection from the 
    // connection table and schedule a clean up call on the removed
    // connection so that the aborts can be issued on a different 
    // call stack.
    //
    // Both send() and closeconnection() try to acquire the svc and 
    // connection lock. If closeconnection() succeeds before send, 
    // it will remove the connection from the table and user will be 
    // scheduled to receive aborts for all the packets that were 
    // queued on the connection at that time. Once closeconnection()
    // returns and send() gets the svc lock, it will find that the 
    // connection does not exist and will create a new connection and
    // enqueue the packet.
    //
    // If send() succeeds in getting the lock before closeconnection(),
    // it will enqueue the packet. Then when closeconnection() gets the
    // lock, it will delete the connection and schedule clean up (which
    // will result in TxAborts for all packets in the connection queue 
    // at the time closeconnection() got the lock, which may or may not 
    // include the packet enqueued by the preceeding send() call).
    void    CloseConnection(UInt32 ip, UInt16 port);

    // For client netPacketSvc only. If the connection to the server
    // can not be established or breaks for any reason, the default
    // behavior is to create another connection and trying sending all
    // the outstanding packets on the new connection, until the
    // packets timeout. If this method is called with flag=true, all
    // outstanding packets are immediately failed, if the connection
    // cannot be established or breaks.
    void    SetFailPacketsOnDisconnect(bool flag);

    // TODO: Make these fields private and expose functions to
    // access public member variables
    bool                    m_IsServer;
    bool                    m_Started;
    bool                    m_Stopped;
    bool                    m_ReceiveSuspended;
    bool                    m_FailOnDisconnect;
    UInt32                  m_ReadBufferSize;
    volatile long           m_OutstandingRetries;
    Ptr<MSMutex>            m_Mutex;
    Ptr<MSMutex>            m_QueueMutex;       // mutex for m_SendRetryQ
    Queue<NetPacketCtx>     m_SendRetryQ;       // retries for missed locks
    NetCxnTable *           m_CxnTable;         // Client use only
    NetServerAcceptor *     m_Acceptor;         // Server use only
    TimeoutHandler *        m_TimeoutH;
    SendHandler *           m_SendHandler;
    ReceiveHandler *        m_ReceiveHandler;
    ConnectHandler *        m_ConnectHandler;
    PacketFactory *         m_PacketFactory;
    unsigned int            m_BindIp;

    DebugListener*          m_pListener;

private:
    void SuspendResumeReceive(bool suspend);
    TxRxStatus TrySend(UInt32 ip, UInt16 port, NetPacketCtx *ctx);
    TxRxStatus ProcessSendRetryQueue(UInt32 ip, UInt16 port, NetCxn *cxn,
                                     NetPacketCtx *ctx);
    void ScheduleSendRetry(UInt32 ip, UInt16 port, NetPacketCtx *ctx);
    friend class NetSendRetry;
};

} // namespace RSLibImpl
