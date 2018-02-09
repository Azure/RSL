
#include "NetSSLCxn.h"
#include "NetPacketSvc.h"
#include "PacketUtil.h"

namespace RSLibImpl
{

    NetPacketSvc::NetPacketSvc(UInt32 readBufferSize, DebugListener* pListener) :
        m_IsServer(false),
        m_Started(false),
        m_Stopped(true),
        m_ReceiveSuspended(false),
        m_FailOnDisconnect(false),
        m_ReadBufferSize(readBufferSize),
        m_OutstandingRetries(0),
        m_CxnTable(NULL),
        m_Acceptor(NULL),
        m_TimeoutH(NULL),
        m_SendHandler(NULL),
        m_ReceiveHandler(NULL),
        m_ConnectHandler(NULL),
        m_pListener(pListener),
        m_BindIp(INADDR_ANY)
    {
        // We require the internal read buffer to be at least
        // big enough to hold the packet header
        LogAssert(readBufferSize >= PacketHdr::SerialLen);
        m_Mutex = new MSMutex();
        m_QueueMutex = new MSMutex();
    }

    NetPacketSvc::~NetPacketSvc()
    {
        LogAssert(m_Stopped);
        while (m_OutstandingRetries > 0)
            {
                Sleep(15);
            }
        if (!m_Acceptor)
            {
                delete  m_CxnTable;
            }
        delete  m_Acceptor;
        delete  m_TimeoutH;
        m_Mutex = NULL;
        m_pListener = NULL;
    }

    int NetPacketSvc::StartAsServer(unsigned short port,
                                    SendHandler *sHandler,
                                    ReceiveHandler *rHandler,
                                    PacketFactory *factory)
    {
        return StartAsServer(port, sHandler, rHandler, NULL, factory);
    }

    int NetPacketSvc::StartAsServer(unsigned short port,
                                    SendHandler *sHandler,
                                    ReceiveHandler *rHandler,
                                    ConnectHandler *cHandler,
                                    PacketFactory *factory)
    {
        return StartAsServer(port, sHandler, rHandler, cHandler, factory, INADDR_ANY);
    }

    int NetPacketSvc::StartAsServer(unsigned short port,
                                    SendHandler *sHandler,
                                    ReceiveHandler *rHandler,
                                    ConnectHandler *cHandler,
                                    PacketFactory *factory,
                                    unsigned int bindIp)
    {

        LogAssert(sHandler != NULL);
        LogAssert(rHandler != NULL);
        LogAssert(factory != NULL);
        LogAssert(m_Started == false);

        m_IsServer = true;
        m_Started = true;
        m_Stopped = false;
        m_Acceptor = new NetServerAcceptor(this);
        m_CxnTable = &m_Acceptor->m_CxnTable;
        m_Mutex = m_Acceptor->m_Mutex;
        m_TimeoutH = new TimeoutHandler(this);
        m_SendHandler = sHandler;
        m_ReceiveHandler = rHandler;
        m_ConnectHandler = cHandler;
        LogAssert(factory != NULL);
        m_PacketFactory = factory;
        m_BindIp = bindIp;

        MUTEX_LOCK(lock, m_Mutex);
        Action *action = Netlib::GetNetProcessor()->Accept(m_Acceptor, m_BindIp, port, INVALID_SOCKET);
        if (action == NULL) {
            // accept failed
            return 1;
        }
        m_Acceptor->m_AcceptAction = action;
        MUTEX_RELEASE(lock);
        Log(LogID_Netlib, LogLevel_Info, "Server Started",
            LogTag_Port, port,
            LogTag_End);
        return 0;
    }

    int
    NetPacketSvc::StartAsClient(SendHandler *sHandler,
                                ReceiveHandler *rHandler,
                                PacketFactory *factory)
    {
        return StartAsClient(sHandler, rHandler, NULL, factory);
    }
  
    int
    NetPacketSvc::StartAsClient(SendHandler *sHandler,
                                ReceiveHandler *rHandler,
                                ConnectHandler *cHandler,
                                PacketFactory *factory)
    {
        return StartAsClient(sHandler, rHandler, cHandler, factory, INADDR_ANY);
    }

    int
    NetPacketSvc::StartAsClient(SendHandler *sHandler,
                                ReceiveHandler *rHandler,
                                ConnectHandler *cHandler,
                                PacketFactory *factory,
                                unsigned int bindIp)
    {
        LogAssert(sHandler != NULL);
        LogAssert(rHandler != NULL);
        LogAssert(factory != NULL);
        LogAssert(m_Started == false);

        m_IsServer = false;
        m_Started = true;
        m_Stopped = false;
        m_CxnTable = new NetCxnTable(s_CxnTableSize);
        m_TimeoutH = new TimeoutHandler(this);
        m_SendHandler = sHandler;
        m_ReceiveHandler = rHandler;
        m_ConnectHandler = cHandler;
        LogAssert(factory != NULL);
        m_PacketFactory = factory;
        m_BindIp = bindIp;
        Log(LogID_Netlib, LogLevel_Info, "Client Started");
        return 0; 
    }

    void
    NetPacketSvc::Stop()
    {
        LogAssert(!m_Stopped);
        // Server
        m_Mutex->Acquire();
        {
        
            MUTEX_LOCK(lock2, this->m_TimeoutH->m_Mutex);
            m_Stopped = true;
    
            m_TimeoutH->Abort();

            if (m_Acceptor)
                {
                    m_Acceptor->Abort();
                }
        }

        // Once NetPacketSvc and NetAcceptor are stopped, nobody creates
        // any new connections. The only operations after this point are
        // Find() and Remove(). So, it is safe to linearly go through
        // the table and close any existing connections.
        if (m_CxnTable->m_NumCxns)
            {
                for (int i = 0; i < m_CxnTable->m_Size; i++)
                    {
                        while (m_CxnTable->m_Table[i].head)
                            {
                                // We have to release m_Mutex before acquiring cxn->m_Mutex to avoid
                                // deadlocks. cxn->m_Mutex should always be acquiring before acquiring
                                // NetPacketSvc's mutex. This is because netlib always takes the connection's
                                // mutex before calling back the application. The application can
                                // call any of the netlib functions on the same callstack (for example:
                                // CloseConnection()), which then takes NetPacketSvc's lock. Hence, we
                                // must always preserve this order of taking locks (Bug #: 28484)

                                // Increment the ref. count on the connection so that it does not
                                // delete itself when we release all the locks.
                                Ptr<NetCxn> cxn = m_CxnTable->m_Table[i].head;
                                m_Mutex->Release();
                
                                cxn->m_Mutex->Acquire();
                                m_Mutex->Acquire();

                                // The connection could have removed itself from the table between
                                // the time we release NetPacketSvc's lock and acquired cxn's mutex.
                                // If it hasn't then...
                                if (m_CxnTable->m_Table[i].head == cxn)
                                    {
                                        m_CxnTable->Remove(cxn);
                                        cxn->CloseConnection(true);
                                        cxn->SetService(NULL);
                                    }
                                cxn->m_Mutex->Release();
                
                            }
                    }
            }

        m_Mutex->Release();
        // Log outside the MUTEX_LOCK
        if (m_IsServer)
            {
                Log(LogID_Netlib, LogLevel_Info, "Server Stopped");
            }
        else
            {
                Log(LogID_Netlib, LogLevel_Info, "Client Stopped");
            }
    }

    void NetPacketSvc::CloseConnection(UInt32 ip, UInt16 port)
    {
        m_Mutex->Acquire();
        Ptr<NetCxn> cxn  = m_CxnTable->Find(ip, port);
        m_Mutex->Release();
        if (cxn != NULL)
            {
                MUTEX_LOCK(lock, cxn->m_Mutex);
                cxn->CloseConnection(true);
            }
    }

    TxRxStatus
    NetPacketSvc::Send(Packet *packet, DWORD timeout)
    {
        return Send(packet, timeout, false);
    }

    TxRxStatus
    NetPacketSvc::Send(Packet *packet, DWORD timeout, bool sendOnExisting) {
  
        LogAssert(m_Started);
        NetPacketCtx &ctx = packet->m_Ctx;
        ctx.Reset();
        ctx.m_Packet = packet;
        ctx.m_Timeout = m_TimeoutH->m_Time + HRTIME_MSECONDS(timeout);
        ctx.m_SendOnExisting = sendOnExisting;

        UInt32 ip = (m_IsServer) ? packet->GetClientIp() : packet->GetServerIp();
        UInt16 port = (m_IsServer) ? packet->GetClientPort() : packet->GetServerPort();
    
        return TrySend(ip, port, &ctx);
    }

    // Tries to get all the required locks and send packets from the SendRetryQueue
    // and then sends the ctx packet. If it can't grab all the locks, it schedules
    // an event.
    // Returns the status of the last packet sent.
    TxRxStatus NetPacketSvc::TrySend(UInt32 ip, UInt16 port, NetPacketCtx *ctx)
    {
        MUTEX_TRY_LOCK(lock, m_Mutex);
        if (!lock)
            {
                ScheduleSendRetry(ip, port, ctx);
                return TxSuccess;
            }

        NetCxn *cxn = m_CxnTable->Find(ip, port);
        if (cxn == NULL)
            {
                return ProcessSendRetryQueue(ip, port, NULL, ctx);
            }
    
        // Get the connections lock
        MUTEX_TRY_LOCK(lock2, cxn->m_Mutex);
        if (!lock2)
            {
                ScheduleSendRetry(ip, port, ctx);
                return TxSuccess;
            }

        MUTEX_RELEASE(lock);

        // send all the packets in the retry queue and the new packet
        return ProcessSendRetryQueue(ip, port, cxn, ctx);
    }

    TxRxStatus
    NetPacketSvc::ProcessSendRetryQueue(UInt32 ip, UInt16 port, NetCxn *cxn,
                                        NetPacketCtx *newCtx)
    {
        Queue<NetPacketCtx> ctxQ;
        NetPacketCtx *ctx;
        {
            MUTEX_LOCK(lock, m_QueueMutex);
            for (ctx = m_SendRetryQ.head; ctx;)
                {
                    UInt32 ctxIp = ctx->m_Packet->GetServerIp();
                    UInt16 ctxPort = ctx->m_Packet->GetServerPort();
                    if (m_IsServer)
                        {
                            ctxIp = ctx->m_Packet->GetClientIp();
                            ctxPort = ctx->m_Packet->GetClientPort();
                        }
    
                    if (ctxIp == ip && ctxPort == port)
                        {
                            // If newCtx is not null, then this is being called from
                            // NetPacketSvc::Send(). In this case, just enqueue this
                            // new packets and return TxSuccess. When the event for
                            // the existing packet fires, we'll remove all the packets
                            // and try to process them.
                            if (newCtx)
                                {
                                    m_SendRetryQ.enqueue(newCtx);
                                    return TxSuccess;
                                }
                            NetPacketCtx *next = ctx->link.next;
                            m_SendRetryQ.remove(ctx);
                            ctxQ.enqueue(ctx);
                            ctx = next;
                        }
                    else
                        {
                            ctx = ctx->link.next;
                        }
                }
        }

        TxRxStatus r = TxSuccess;
        if (newCtx)
            {
                ctxQ.enqueue(newCtx);
            }
        TxRxStatus status = (m_Stopped) ? TxAbort : TxNoConnection;
        bool newCxn = false;

        while ((ctx = ctxQ.dequeue()) != NULL)
            {
                // if this is a client and the connection to the server doesn't
                // exist, create a new connection
                if (cxn == NULL && !m_IsServer && !m_Stopped && ctx->m_SendOnExisting == false)
                    {
                        cxn = NetSslCxn::CreateNetCxn(this, ip, port);
                        LogAssert(cxn);
                        newCxn = true;
                        cxn->m_Mutex->Acquire();
                        m_CxnTable->Insert(cxn);
                    }
        
                r = (cxn) ? cxn->EnqueuePacket(ctx) : status;
                if (r != TxSuccess && ctx != newCtx)
                    {
                        if (NULL != m_pListener)
                            {
                                m_pListener->ProcessSend(ctx->m_Packet, r);
                            }
                        m_SendHandler->ProcessSend(ctx->m_Packet, r);
                    }
            }

        if (newCxn)
            {
                cxn->Connect();
                cxn->m_Mutex->Release();
            }
        return r;
    }

    void NetPacketSvc::ScheduleSendRetry(UInt32 ip, UInt16 port, NetPacketCtx *ctx)
    {
        UInt64 delay = MUTEX_RETRY_DELAY;
        if (ctx)
            {
                delay = 0;
                MUTEX_LOCK(queueLock, m_QueueMutex);
                m_SendRetryQ.enqueue(ctx);
            }
        NetSendRetry *entry = new NetSendRetry(ip, port, this);
        CompletionEvent *e;
        e = new CompletionEvent(entry, EventHandler::EVENT_IMMEDIATE);
        Netlib::GetNetProcessor()->ScheduleCompletion(e, delay);
    }

    void NetPacketSvc::SuspendReceive()
    {
        SuspendResumeReceive(true);
    }

    void NetPacketSvc::ResumeReceive()
    {
        SuspendResumeReceive(false);
    }

    void
    NetPacketSvc::SuspendResumeReceive(bool suspend)
    {
        // Go through all the connections and and suspend them
        MUTEX_LOCK(lock, m_Mutex);
        m_ReceiveSuspended = suspend;
        m_CxnTable->SuspendResumeReceive(suspend);
    }

    void
    NetPacketSvc::SetFailPacketsOnDisconnect(bool flag)
    {
        m_FailOnDisconnect = flag;
    }

} // namespace RSLibImpl
