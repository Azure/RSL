#include "PacketUtil.h"
#include "NetPacket.h"
#include "NetPacketSvc.h"
#include "NetSSLCxn.h"

namespace RSLibImpl
{

    //
    // NetPoolBuffer
    //

    NetPoolBuffer::NetPoolBuffer()
    {
        Clear();
    }

    NetPoolBuffer::NetPoolBuffer(int bufferSize)
    {    
        NetBuffer::Clear();
        SetPoolBuffer(Netlib::GetBufferPool()->GetBuffer(bufferSize));
    }

    NetPoolBuffer::NetPoolBuffer(const NetPoolBuffer &buf)
    {
        NetBuffer::Clear();
        SetPoolBuffer(Netlib::GetBufferPool()->GetBuffer(buf.m_BufSize));

        // Copy valid data from the input NetBuffer
        int avail = buf.ReadAvail();
        buf.CopyTo(m_Buffer, avail);
        Fill(avail);
    }

    NetPoolBuffer::NetPoolBuffer(Buffer *buffer)
    {
        NetBuffer::Clear();
        SetPoolBuffer(buffer);
    }

    NetPoolBuffer::~NetPoolBuffer()
    {
        Netlib::GetBufferPool()->ReleaseBuffer(m_PoolBuffer);
        m_Buffer = NULL;
    }

    void NetPoolBuffer::SetPoolBuffer(Buffer *buffer)
    {
        m_PoolBuffer = buffer;
        NetBuffer::SetBuffer((char *) m_PoolBuffer->m_Memory, 
                             m_PoolBuffer->m_Size);
    }

    Buffer* NetPoolBuffer::GetPoolBuffer()
    {
        return m_PoolBuffer;
    }

    void NetPoolBuffer::Clear()
    {
        m_PoolBuffer = NULL;
        NetBuffer::Clear();
    }

    //
    // NetPacketCtx
    //
    NetPacketCtx::NetPacketCtx()
    {
        Reset();
    }

    void NetPacketCtx::Reset() {
        m_Cxn = NULL;
        m_Packet = NULL;
        link.next = link.prev = NULL;
        m_TimeoutLink.next = m_TimeoutLink.prev = NULL;
        m_SendOnExisting = false;
    }

    NetPacketCtx::~NetPacketCtx() {}

    NetSendRetry::NetSendRetry(UInt32 ip, UInt16 port, NetPacketSvc *svc) :
        EventHandler(), m_Ip(ip), m_Port(port), m_Svc(svc)
    {
        m_Mutex = new MSMutex();
        InterlockedIncrement(&m_Svc->m_OutstandingRetries);
        SetHandler((Handler) &NetSendRetry::HandleEvent);
    }

    NetSendRetry::~NetSendRetry()
    {
        InterlockedDecrement(&m_Svc->m_OutstandingRetries);
        m_Mutex = NULL;
    }

    int
    NetSendRetry::HandleEvent(Event /*event*/, void * /*data*/)
    {
        m_Svc->TrySend(m_Ip, m_Port, NULL);
        delete this;
        return 0;
    }

    //
    // TimeoutHandler
    //
    TimeoutHandler::TimeoutHandler(NetPacketSvc *svc) :
        EventHandler(new MSMutex()),
        m_Event(NULL), m_svc(svc)
    {
        m_Time = GetHiResTime();
        SetHandler((Handler) &TimeoutHandler::EventTimeout);
        m_Event = new CompletionEvent(this, EVENT_INTERVAL);
        Netlib::GetNetProcessor()->ScheduleCompletion(m_Event, HRTIME_MSECONDS(10));
    }

    TimeoutHandler::~TimeoutHandler()
    {
        LogAssert(!m_Timeoutq.head);
        if (m_Event)
            {
                m_Event->Cancel();
            }
        m_Mutex = NULL;
    }
    void
    TimeoutHandler::Enqueue(NetPacketCtx *ctx) {

        m_QueueMutex.Acquire();
        NetPacketCtx *cb = m_Timeoutq.tail;

        // insert the timeouts in the sorted order. The assumption is
        // that most users will specify the same timeout for every packet.
        // So, for majority of the cases, ctx will be inserted at the
        // end of the m_Timeoutq. If it turns out that we have to do a lot
        // of work to keep this list sorted, then maybe we should split up
        // the list by timeouts.
        for (;cb; cb = cb->m_TimeoutLink.prev)
            {
                if (cb->m_Timeout <= ctx->m_Timeout)
                    {
                        m_Timeoutq.insert(ctx, ctx->m_TimeoutLink, cb);
                        break;
                    }
            }

        if (!cb)
            {
                m_Timeoutq.push(ctx, ctx->m_TimeoutLink);
            }
        m_QueueMutex.Release();
    }

    void
    TimeoutHandler::Remove(NetPacketCtx *ctx) {
        m_QueueMutex.Acquire();
        m_Timeoutq.remove(ctx, ctx->m_TimeoutLink);
        m_QueueMutex.Release();
    }

    void
    TimeoutHandler::Clear(Queue<NetPacketCtx> &q) {

        NetPacketCtx *ctx = q.head;
        while (ctx)
            {
                Remove(ctx);
                ctx = ctx->link.next;
            }
    }

    // We must have both m_QueueMutex and the connetions's mutex when
    // removing the ctx from the connection queue. Otherwise, there is
    // a race condition where the connection could attempt to remove
    // the context while this method is removing the context from the
    // connection's queue.
    int
    TimeoutHandler::EventTimeout(Event, void *) {

        NetPacketCtx *ctx;
        m_Time = GetHiResTime();
        m_QueueMutex.Acquire();
        ctx = m_Timeoutq.head;

        while (ctx && ctx->m_Timeout <= m_Time)
            {
                MUTEX_TRY_LOCK(lock, ctx->m_Cxn->m_Mutex);
                if (!lock)
                    {
                        ctx = ctx->m_TimeoutLink.next;
                    }
                else
                    {
                        m_Timeoutq.remove(ctx, ctx->m_TimeoutLink);
                        ctx->m_Cxn->m_Sendq.remove(ctx);

                        if (ctx->m_Cxn->m_Write.m_PacketCtx != NULL &&
                            ctx->m_Cxn->m_Write.m_PacketCtx == ctx)
                            {
                                // Packet buffer is busy
                                ctx->m_Packet->Duplicate();
                                ctx->m_Cxn->m_Write.m_PacketCtx = NULL;
                            }
                        m_QueueMutex.Release();
                        //Netlib::m_Counters.g_ctrNumPacketsTimedOut->Increment();
                        // Debug listening
                        if (NULL != m_svc->m_pListener)
                            {
                                m_svc->m_pListener->ProcessSend(ctx->m_Packet, TxTimedOut);
                            }
                        m_svc->m_SendHandler->ProcessSend(ctx->m_Packet, TxTimedOut);
                        m_QueueMutex.Acquire();
                        ctx = m_Timeoutq.head;
                    }
            }

        m_QueueMutex.Release();
        m_Event = new CompletionEvent(this, EventHandler::EVENT_INTERVAL);
        Netlib::GetNetProcessor()->ScheduleCompletion(m_Event, HRTIME_MSECONDS(20));
        return 0;
    }

    void TimeoutHandler::Abort()
    {
        if (m_Event)
            {
                m_Event->Cancel();
                m_Event = NULL;
            }
    }

    //
    // NetCxnTable
    //
    NetCxnTable::NetCxnTable(int size) :
        m_Size(size),
        m_NumCxns(0)
    {
        m_Table = new Queue<NetCxn>[m_Size];
    }

    NetCxnTable::~NetCxnTable() {
        delete [] m_Table;
    }

    void
    NetCxnTable::Insert(NetCxn *cxn) {
        DWORD hash = cxn->GetRemoteIp() + cxn->GetRemotePort();
        hash = hash % m_Size;
        m_Table[hash].enqueue(cxn, cxn->HashLink());
        m_NumCxns++;
        //Netlib::m_Counters.g_ctrNumActiveConnection->Increment();
    }

    NetCxn*
    NetCxnTable::Find(unsigned int ip, unsigned short port) {
        DWORD hash = ip + port;
        hash = hash % m_Size;
        for (NetCxn *c = m_Table[hash].head; c; c = c->HashLink().next) {
            if (c->GetRemoteIp() == ip && c->GetRemotePort() == port)
                return c;
        }
        return NULL;
    }

    void
    NetCxnTable::Remove(NetCxn *cxn) {
        DWORD hash = cxn->GetRemoteIp()+ cxn->GetRemotePort();
        hash = hash % m_Size;
        m_Table[hash].remove(cxn, cxn->HashLink());
        m_NumCxns--;
        //Netlib::m_Counters.g_ctrNumActiveConnection->Decrement();
    }

    void
    NetCxnTable::SuspendResumeReceive(bool suspend)
    {
        // This method must be called under either the acceptor's lock
        // (for a server) or the svc's lock (for a client)
        if (m_NumCxns)
            {
                for (int i = 0; i < m_Size; i++)
                    {
                        NetCxn *cxn = m_Table[i].head;
                        while (cxn)
                            {
                                NetCxn *n = cxn->HashLink().next;
                                MUTEX_LOCK(lock, cxn->m_Mutex);
                                if (suspend)
                                    {
                                        cxn->SuspendReceive();
                                    }
                                else
                                    {
                                        cxn->ResumeReceive();
                                    }
                                cxn = n;
                            }
                    }
            }
    }

    //
    // NetServerAcceptor
    //

    NetServerAcceptor::NetServerAcceptor(NetPacketSvc *svc) : 
        EventHandler(),
        m_Svc(svc),
        m_AcceptAction(NULL),
        m_CxnTable(s_CxnTableSize)
    {
        m_Mutex = new MSMutex();
        SetHandler((Handler) &NetServerAcceptor::HandleEvent);
    }

    NetServerAcceptor::~NetServerAcceptor()
    {
        while (1)
            {
                {
                    MUTEX_LOCK(lock, m_Mutex);
                    if (!m_CxnTable.m_NumCxns)
                        {
                            break;
                        }
                }
                Sleep(10);
            }
        m_Mutex = NULL;
    }

    int
    NetServerAcceptor::HandleEvent(Event event, void *data) {

        switch(event)
            {
            case NET_EVENT_ACCEPT :
                {
                    // The lock has already been acquired by g_NetProcessor

                    NetVConnection *vc = (NetVConnection *)data;
                    unsigned int ip = vc->GetRemoteIp();
                    unsigned short port = vc->GetRemotePort();

                    // check if the client connection EventHandler's is already there
                    NetCxn *cxn = m_CxnTable.Find(ip, port);
                    if (cxn) {
                        Log(LogID_Netlib, LogLevel_Warning, "Duplicate Connection",
                            LogTag_NumericIP, ip, LogTag_Port, port, LogTag_End);
                        vc->IOClose();
                        return 0;
                    }

                    cxn = NetSslCxn::CreateNetCxn(m_Svc, ip, port);
                    LogAssert(cxn);
                    m_CxnTable.Insert(cxn);
                    cxn->Start(vc);
                    return 0;
                }
            case NET_EVENT_ACCEPT_FAILED:
                {
                    LogAssert(!"accept failed\n");
                    return 0;
                }
            default:
                LogAssert(0);
            }
        return 0;
    }

    void NetServerAcceptor::Abort()
    {
        if (m_AcceptAction)
            {
                m_AcceptAction->Cancel();
                m_AcceptAction = NULL;
            }
    }

} // namespace RSLibImpl
