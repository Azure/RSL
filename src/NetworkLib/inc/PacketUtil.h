#pragma once

//
// internal\PacketUtil.h
// This file declares the following utility classes:
//          NetPoolBuffer: A subclass of NetBuffer that uses Buffer Pool
//          NetPacketCtx: the context for a packet
//          TimeoutHandler: the class to handle packet timeouts
//          NetCxnTable : The hash table of connections
//          NetServerAcceptor: the class that handles accepting
//                             connections on the server side

#pragma once

#include "EventHandler.h"
#include "List.h"
#include "HiResTime.h"
#include "NetProcessor.h"
#include "NetPacketSvc.h"
#include "NetBuffer.h"
#include "BufferPool.h"

namespace RSLibImpl
{

    class NetCxn;
    class Packet;

    // Default size of the hash table of connections
    static const int s_CxnTableSize = 2053;

    // NetPoolBuffer
    // This class is exactly like the netBuffer with just one difference
    // - it uses the BufferPool to allocate and free memory
    class NetPoolBuffer : public NetBuffer
    {
    public:
        NetPoolBuffer();

        // NetPoolBuffer
        // This constructor creates a new buffer of the specified size
        NetPoolBuffer(int bufferSize);

        // NetBuffer
        // Copy constructor for NetBuffer. Creates an internal buffer
        // and copies all the data available (to read) from the input
        // buffer
        NetPoolBuffer(const NetPoolBuffer &buf);
        NetPoolBuffer(Buffer *buffer);

        // ~NetBuffer
        // Deletes the internal buffer if allocated. No deletes will
        // attempted if the internal buffer was allocated by the caller
        // and set using SetBuffer() (see below).
        virtual ~NetPoolBuffer();

        void SetPoolBuffer(Buffer *buffer);
        Buffer* GetPoolBuffer();
        void Clear();

        Link<NetPoolBuffer> link;

    protected:
        Buffer *m_PoolBuffer;
    };

    // NetPacketCtx
    //  This class is used to create and save the context of the
    //  packet (e.g. PacketSvc, PacketHandler, etc.)
    class NetPacketCtx {
    public:
        NetPacketCtx();
        ~NetPacketCtx();
        void  Reset();

        //    NetPacketSvc    *m_Svc;
        NetCxn          *m_Cxn;
        Packet          *m_Packet;
        HiResTime       m_Timeout;
        Link<NetPacketCtx> link;
        Link<NetPacketCtx> m_TimeoutLink;
        bool            m_SendOnExisting;
    };

    class NetSendRetry : public EventHandler
    {
    public:
        NetSendRetry(UInt32, UInt16 port, NetPacketSvc *svc);
        ~NetSendRetry();

        int HandleEvent(Event event, void *data);

        UInt32 m_Ip;
        UInt16 m_Port;
        NetPacketSvc *m_Svc;
    };
    
    //
    // TimeoutHandler
    //
    class TimeoutHandler : public EventHandler {

    public:

        TimeoutHandler(NetPacketSvc *svc);
        ~TimeoutHandler();
        void Enqueue(NetPacketCtx *ctx);
        void Remove(NetPacketCtx *ctx);
        void Clear(Queue<NetPacketCtx> &q);
        int EventTimeout(Event event, void *data);
        void Abort();

        Queue<NetPacketCtx> m_Timeoutq;
        volatile HiResTime m_Time;
        CompletionEvent *m_Event;
        NetPacketSvc *m_svc;
        MSMutex m_QueueMutex;
    };

    //
    // NetCxnTable
    //
    class NetCxnTable
    {
    public:
        NetCxnTable(int size);
        ~NetCxnTable();

        void Insert(NetCxn *cxn);
        NetCxn *Find(unsigned int ip, unsigned short port);
        void Remove(NetCxn *cxn);
        void SuspendResumeReceive(bool suspend);

        Queue<NetCxn> *m_Table;
        int m_Size;
        int m_NumCxns;
    };

    //
    // NetServerAcceptor
    //
    class NetServerAcceptor : public EventHandler {
    public:

        NetServerAcceptor(NetPacketSvc *svc);
        ~NetServerAcceptor();

        int HandleEvent(Event event, void *data);
        void Abort();

        NetPacketSvc *m_Svc;
        Action *m_AcceptAction;
        NetCxnTable m_CxnTable;

    };

} // namespace RSLibImpl
