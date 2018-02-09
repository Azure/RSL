#pragma once

// NetCxn.h
// This file declares a connection (to be used by the GNetPacketSvc)
// It provides methods to primarily send and receive packets. The
// classes NetClientCxn and NetServerCxn represent a client/server
// connection respectively. The key read/write functionality is
// implemented in the base class though

#include "NetProcessor.h"
#include "NetVConnection.h"
#include "NetPacket.h"
#include "NetPacketSvc.h"
#include <memory>
#include "SSLImpl.h"


#define LogDebugCxnMacro(msg)                   \
    Log(LogID_Netlib, LogLevel_Debug, msg,      \
        LogTag_NumericIP, m_RemoteIp,           \
        LogTag_Port, m_RemotePort,              \
        LogTag_End);



namespace RSLibImpl
{

    class NetServerAcceptor;

    // NetCxn is refcounted. The constructor increments the refcount.
    // When the connection closes, the ref count
    // is decremented. This object won't be deleted until the refcount is
    // 0.
    class NetCxn : public EventHandler, public RefCount
    {  
        friend class TimeoutHandler;
    
    public:

        virtual ~NetCxn();

        // Write Context for this connection
        struct WriteCtx
        {
            VIO *           m_Vio;
            NetPoolBuffer * m_NetBuffer;
            NetPacketCtx *  m_PacketCtx;

            WriteCtx()
            {
                Reset();
            }

            void Reset()
            {
                m_Vio = NULL;
                m_NetBuffer = NULL;
                m_PacketCtx = NULL;
            }
        };

        // Read Context for this connection
        struct ReadCtx
        {
            VIO *           m_Vio;
            NetPoolBuffer * m_NetBuffer;
            Buffer *        m_InternalReadBuffer;
            Packet        * m_Packet; // this is the packet we are current reading

            ReadCtx()
            {
                Reset();
            }

            void Reset()
            {
                m_Vio = NULL;
                m_NetBuffer = NULL;
                m_InternalReadBuffer = NULL;
                m_Packet = NULL;
            }
        };

        virtual int CloseConnection(bool abort = false);
        void Connect();
        virtual void Start(NetVConnection *vc);
        void ReConnect();
        void SetService(NetPacketSvc *svc) { m_Svc = svc; }
        void        SuspendReceive();
        void        ResumeReceive();
        virtual TxRxStatus    EnqueuePacket(NetPacketCtx *ctx);


        unsigned int GetRemoteIp() { return m_RemoteIp; }
        void SetRemoteIp(unsigned int ip) { m_RemoteIp = ip; }

        unsigned short GetRemotePort() { return m_RemotePort; }
        void SetRemotePort(unsigned short port) { m_RemotePort = port; }

        Link<NetCxn>& HashLink() { return hash_link; }

        static NetCxn* CreateNetCxn(NetPacketSvc *svc, UInt32 ip, UInt16 port) { return new NetCxn(svc, ip, port); }

    protected:
        NetCxn(NetPacketSvc *svc, UInt32 ip, UInt16 port);


        NetVConnection  *m_Vc;          // NetVConncetion used by this cxn
        NetPacketSvc    *m_Svc;         // The NetPacketSvc that created this cnx
        CompletionEvent *m_Event;
        CompletionEvent *m_ReenableReceiveEvent;
        Action          *m_ConnectAction;   // Client only 
        TimeoutHandler  *m_TimeoutH;
        SendHandler     *m_SendHandler;
        ReceiveHandler  *m_ReceiveHandler;
        ConnectHandler  *m_ConnectHandler;

        DebugListener   *m_pListener;

        Link<NetCxn>    link;
        Link<NetCxn>    hash_link;

        Queue<NetPacketCtx>     m_Sendq;
        Queue<NetPacketCtx>     m_DisConnectq;
    
        WriteCtx        m_Write;
        ReadCtx         m_Read;
        unsigned int    m_RemoteIp;
        unsigned short  m_RemotePort;
        bool            m_InWriteCallback;
        bool            m_ReceiveSuspended;
        int             m_ReentrantCount;
        bool            m_Closed;
        bool            m_Abort;

        void        InitializeReadWriteContexts();
        virtual int         WriteReadyInternal();
        void        ProcessQueuedPacketsForWrite();
        virtual void        ReadReadyInternal();

        void CallConnectHandler(ConnectHandler::ConnectState state);
        virtual int CloseCleanup();
        int HandlePacketEvent(Event event, void *data);
        void ReadMoreData(int totalLength);

        static const int CONNECT_RETRY_TIME;
    };



} // namespace RSLibImpl
