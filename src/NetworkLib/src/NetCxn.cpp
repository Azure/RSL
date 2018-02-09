#include "NetCxn.h"
#include "PacketUtil.h"
#include "NetPacket.h"
#include "DynString.h"

namespace RSLibImpl
{

    const int NetCxn::CONNECT_RETRY_TIME = HRTIME_MSECONDS(20);

    NetCxn::NetCxn(NetPacketSvc *svc, UInt32 ip, UInt16 port) : EventHandler()
    {
        UpCount();
        LogAssert(NULL != svc);
        m_Mutex = new MSMutex();
        m_Vc = NULL;
        m_Svc = svc;
        m_Event = NULL;
        m_ReenableReceiveEvent = NULL;
        m_ConnectAction = NULL;
        m_TimeoutH = svc->m_TimeoutH;
        m_SendHandler = svc->m_SendHandler;
        m_ReceiveHandler =svc->m_ReceiveHandler;
        m_ConnectHandler = svc->m_ConnectHandler;
        m_RemoteIp = ip;
        m_RemotePort = port;
        m_InWriteCallback = false;
        m_ReceiveSuspended = svc->m_ReceiveSuspended;
        m_ReentrantCount = 0;
        m_Closed = false;
        m_Abort = false;
        m_pListener = svc->m_pListener;
        SetHandler((Handler) &NetCxn::HandlePacketEvent);

    }

    NetCxn::~NetCxn()
    {
        LogAssert(!m_Vc);
        LogAssert(!m_Event);
        LogAssert(!m_ReenableReceiveEvent);
        LogAssert(!m_ConnectAction);
        CloseCleanup();
    }

    void NetCxn::InitializeReadWriteContexts()
    {
        // Initialize read context
        UInt32 size = m_Svc->m_ReadBufferSize;
        LogAssert(size >= PacketHdr::SerialLen);

        m_Read.m_InternalReadBuffer = Netlib::GetBufferPool()->GetBuffer(size);
        m_Read.m_NetBuffer = new NetPoolBuffer(m_Read.m_InternalReadBuffer);

        // Initialize Write Context. Note that we always write
        // directly from the packet's buffer, so we don't have
        // an internal write buffer
        m_Write.m_NetBuffer = new NetPoolBuffer();
        m_Write.m_PacketCtx = NULL;
    }

    TxRxStatus NetCxn::EnqueuePacket(NetPacketCtx *ctx)
    {
        // If this is a client connection and the connection is closed,
        // enqueue the packet only if the client wants to create a new
        // connection. The connection will attempt to reconnect in this
        // case. For Server connections, always return NoConnection if
        // the connection is closed.
        if (m_Closed)
            {
                // If the sendQ is not empty, then we are going to try re-connecting soon
                // In that case, enqueue the new packet even if the connection is
                // currently closed.
                if (m_Svc->m_IsServer || (ctx->m_SendOnExisting && m_Sendq.head == NULL))
                    {
                        return TxNoConnection;
                    }
            }
        ctx->m_Cxn = this;
        m_Sendq.enqueue(ctx);
        m_TimeoutH->Enqueue(ctx);

        // This is to handle the case where a client has called
        // a send after a connect call, but the connect hasn't
        // finished yet.
        // If the user schedules another packet in the send callback, don't
        // call WriteReadyInternal. We'll handle the new packet at the
        // end of ProcessQueuedPacketsForWrite();
        if (m_Vc && !m_InWriteCallback)
            {
                WriteReadyInternal();
            }
        return TxSuccess;
    }

    int NetCxn::WriteReadyInternal()
    {
        // check if there are any packets to be written
        VIO *vio = m_Write.m_Vio;

        ProcessQueuedPacketsForWrite();

        // Reenable only if we are not on the callback stack from 
        // the NetVConnection and we have a packet to write.
        // Reenabling on the callback stack is wasteful since the
        // NetVConnection is going to check if it needs to send any
        // data right after the callback. Also, if vio->ReadAvail()
        // is not 0, then we must have already done a reenable before.

        if (m_Write.m_PacketCtx && !m_InWriteCallback) {

            m_Vc->Reenable(vio);
        }
        return 0;
    }

    // We'll only write one packet at a time.
    void NetCxn::ProcessQueuedPacketsForWrite()
    {
        // Can't do much until the current packet has been fully written
        // out or there wasn't a packet scheduled at all
        if (!m_Write.m_NetBuffer->IsEmpty())
            {
                return;
            }

        if (!m_Write.m_PacketCtx)
            {
                // Either there is no outstanding write, or the packet
                // timed out while we were waiting for it to finish and 
                // the timeout handler already sent an abort and set the 
                // connection's pointer to NULL. We just need to delete 
                // the NetBuffer's buffer, and proceed
        
                Buffer *buffer = m_Write.m_NetBuffer->GetPoolBuffer();
                Netlib::GetBufferPool()->ReleaseBuffer(buffer);
                m_Write.m_NetBuffer->Clear();
            }
        else
            {
                // Packet's I/O completed. Call back the user with success
                // Note that we can never get here in the user's call stack
                LogAssert(m_InWriteCallback == true);
                NetPacketCtx *ctx = m_Write.m_PacketCtx;
                m_TimeoutH->Remove(ctx);
                m_Sendq.dequeue();
                m_Write.m_PacketCtx = NULL;
                m_Write.m_NetBuffer->Clear();
                //Netlib::m_Counters.g_ctrNumPacketsSent->Increment();

                // Debug listening
                if (NULL != m_pListener)
                    {
                        m_pListener->ProcessSend(ctx->m_Packet, TxSuccess);
                    }

                m_SendHandler->ProcessSend(ctx->m_Packet, TxSuccess);

                // If the connection was closed in the ProcessSend() callback,
                // it is safest to just return.
                if (m_Closed)
                    {
                        return;
                    }
            }

        // Ready to send another packet
        if (m_Sendq.head)
            {
                NetPacketCtx *ctx = m_Sendq.head;
                ctx->m_Packet->Serialize();
                m_Write.m_NetBuffer->SetPoolBuffer(ctx->m_Packet->GetPoolBuffer());
                m_Write.m_NetBuffer->Fill(ctx->m_Packet->GetPacketLength());
                m_Write.m_PacketCtx = m_Sendq.head;
                //Netlib::m_Counters.g_ctrSendPacketSize->Set(ctx->m_Packet->GetPacketLength());
            }
    }

    void NetCxn::ReadReadyInternal()
    {    
        if (m_ReceiveSuspended)
            {
                return;
            }

        bool bUsingInternalBuffer = (m_Read.m_NetBuffer->GetPoolBuffer() ==
                                     m_Read.m_InternalReadBuffer);
    
        while (m_Read.m_NetBuffer->ReadAvail() >= (int) PacketHdr::SerialLen)
            {
                if (m_Read.m_Packet == NULL)
                    {
                        m_Read.m_Packet = m_Svc->m_PacketFactory->CreatePacket();

                        if (!m_Read.m_Packet->DeSerializeHeader(m_Read.m_NetBuffer))
                            {
                                DynString remoteAddr(inet_ntoa(m_Vc->GetRemoteAddr().sin_addr));
                                DynString localAddr(inet_ntoa(m_Vc->GetRemoteAddr().sin_addr));
                
                                Log(LogID_NetlibCorruptPacket, LogLevel_Error, "Invalid packet",
                                    "Remote Address (%s:%hu). Local Address (%s:%hu)",
                                    remoteAddr.Str(), m_Vc->GetRemotePort(),
                                    localAddr.Str(), m_Vc->GetLocalPort());
                
                                //Netlib::m_Counters.g_ctrNumPacketsDropped->Increment();
                                CloseConnection();
                                return;
                            }
                    }

                UInt32 packetLength = m_Read.m_Packet->m_Hdr.m_Size;

                if ((int) packetLength <= m_Read.m_NetBuffer->ReadAvail())
                    {
                        // Got full packet
                        Packet *pkt = m_Read.m_Packet;
                        bool pktValid = pkt->DeSerialize(m_Read.m_NetBuffer, !bUsingInternalBuffer);
            
                        if (pktValid == false)
                            {
                                DynString remoteAddr(inet_ntoa(m_Vc->GetRemoteAddr().sin_addr));
                                DynString localAddr(inet_ntoa(m_Vc->GetRemoteAddr().sin_addr));
                
                                Log(LogID_NetlibCorruptPacket, LogLevel_Error, "Invalid packet",
                                    "Remote Address (%s:%hu). Local Address (%s:%hu)",
                                    remoteAddr.Str(), m_Vc->GetRemotePort(),
                                    localAddr.Str(), m_Vc->GetLocalPort());
                
                                //Netlib::m_Counters.g_ctrNumPacketsDropped->Increment();
                                CloseConnection();
                                return;
                            }
            
                        m_Read.m_NetBuffer->Consume(packetLength);
                        m_Read.m_Packet = NULL;
                        //Netlib::m_Counters.g_ctrNumPacketsReceived->Increment();
                        //Netlib::m_Counters.g_ctrReceivePacketSize->Set(packetLength);

                        if (m_Svc->m_IsServer)
                            {
                                pkt->SetClientAddr(m_Vc->GetRemoteIp(), m_Vc->GetRemotePort());
                                pkt->SetServerAddr(m_Vc->GetLocalIp(), m_Vc->GetLocalPort());
                            }
                        else
                            {
                                pkt->SetServerAddr(m_Vc->GetRemoteIp(), m_Vc->GetRemotePort());
                                pkt->SetClientAddr(m_Vc->GetLocalIp(), m_Vc->GetLocalPort());
                            }

                        if (!bUsingInternalBuffer)
                            {
                                // Switch back to the internal buffer
                                m_Read.m_NetBuffer->SetPoolBuffer(m_Read.m_InternalReadBuffer);
                                m_Read.m_Vio->SetNBytes(VIO::MAX_NBYTES);
                            }

                        // Debug listening
                        if (NULL != m_pListener)
                            {
                                m_pListener->ProcessReceive(pkt);
                            }

                        HiResTime startTime = GetHiResTime();

                        m_ReceiveHandler->ProcessReceive(pkt);

                        HiResTime delay = GetHiResTime() - startTime;
                        if (delay > NetProcessor::MAX_CALLBACK_DELAY)
                            {
                                Log(LogID_Netlib, LogLevel_Warning,
                                    "long delay in ProcessReceive (us, isServer)",
                                    LogTag_Int64_1, delay,
                                    LogTag_UInt1, (int) m_Svc->m_IsServer, 
                                    LogTag_End);

                                Logger::CallNotificationsIfDefined(LogLevel_Warning, LogID_Netlib, "long delay in ProcessReceive (us, isServer)");
                            }

                        // If the connection was closed in the ProcessReceive() callback,
                        // it is safest to just return.
                        if (m_Closed)
                            {
                                return;
                            }
            
                        if (!bUsingInternalBuffer)
                            {
                                break;
                            }
                    }
                else if ((int) packetLength > m_Read.m_NetBuffer->GetBufSize())
                    {
                        // Big packet.

                        LogAssert(bUsingInternalBuffer == true);
                        ReadMoreData(packetLength);
                        break;
                    }
                else
                    {
                        // The packet will fill in the current buffer. Wait for
                        // more I/O

                        break;
                    }
            }   // end of while

        return;
    }


    void NetCxn::ReadMoreData(int totalLength)
    {
        Buffer *buffer = Netlib::GetBufferPool()->GetBuffer(totalLength);
        int bytesRead = m_Read.m_NetBuffer->ReadAvail();
        m_Read.m_NetBuffer->CopyTo((char *)buffer->m_Memory, bytesRead, 0);
        m_Read.m_NetBuffer->Consume(bytesRead);
        m_Read.m_NetBuffer->SetPoolBuffer(buffer);
        m_Read.m_NetBuffer->Fill(bytesRead);
        m_Read.m_Vio->SetNBytes(m_Read.m_Vio->GetNDone() + 
                                totalLength - bytesRead);

    }

    // SuspendReceive
    // This method must be called under connection's lock
    void NetCxn::SuspendReceive()
    {
        this->m_ReceiveSuspended = true;
        if (m_ReenableReceiveEvent)
            {
                m_ReenableReceiveEvent->Cancel();
                m_ReenableReceiveEvent = NULL;
            }
        LogDebugCxnMacro("In SuspendReceive");
    }

    // ResumeReceive
    // This method must be called under connection's lock
    void NetCxn::ResumeReceive()
    {
        this->m_ReceiveSuspended = false;
        if (!m_ReenableReceiveEvent)
            {
                m_ReenableReceiveEvent = new CompletionEvent(this, PACKET_REENABLE_RECEIVE);
                Netlib::GetNetProcessor()->ScheduleCompletion(m_ReenableReceiveEvent, 0);
            }
        LogDebugCxnMacro("In ResumeReceive");
    }


    // CloseConnection
    // May be called on the user's call stack. It doesn't call back
    // the user on the same stack but schedules an Event to invoke
    // CloseCleanup() (that calls back the user with aborts)
    int NetCxn::CloseConnection(bool abort)
    {
        LogDebugCxnMacro("In CloseConnection");

        // If the client already attempted a connect, cancel that.
        // The user is closing the connection while we were trying
        // to connect
        if (m_ConnectAction) {
            m_ConnectAction->Cancel();
            m_ConnectAction = NULL;
        }

        if (m_ReenableReceiveEvent)
            {
                m_ReenableReceiveEvent->Cancel();
                m_ReenableReceiveEvent = NULL;
            }

        // Note that if we were aborted, m_Svc could now be NULL,
        // otherwise it is guaranteed to be non-null
        if (abort)
            {
                m_Abort = true;
            }
        else
            {
                LogAssert(this->m_Svc != NULL);
            }

        if (m_Closed)
            {
                return 1;
            }
        m_Closed = true;

        // clear the timeout handlers and move all the packets from the
        // send queue to disconnect Q. These are the packets that need to
        // be called back. Any new packets enqueued after this point will
        // be put in the sendQ. For Client connections, if the user
        // enqueues more packets after this, they will go into the sendQ
        // and CloseCleanup() will attemp to reconnect.
        // NOTE: This has to be done before we call the connect
        // handlers, in case the app sends a packet on the callback. We
        // don't want to fail that packet
        if (m_Abort || m_Svc->m_IsServer || m_Svc->m_FailOnDisconnect)
            {
                m_TimeoutH->Clear(m_Sendq);

                m_DisConnectq.append(m_Sendq);
                m_Sendq.clear();
            }
    
        if (m_Vc)
            {
                // Free up the internal buffer if we are currently reading
                // using another buffer from the buffer pool. The current buffer
                // is always released by the NetVConnection
                if (m_Read.m_NetBuffer->GetPoolBuffer() != m_Read.m_InternalReadBuffer)
                    {
                        Netlib::GetBufferPool()->ReleaseBuffer(m_Read.m_InternalReadBuffer);
                        m_Read.m_InternalReadBuffer = NULL;
                    }
        
                if (m_Read.m_Packet != NULL)
                    {
                        m_Svc->m_PacketFactory->DestroyPacket(m_Read.m_Packet);
                        m_Read.m_Packet = NULL;
                    }
        
                // If we have a packet outstanding for write duplicate it. 
                // The original buffer will be deleted by the NetVConnection
                // on write completion
                if (m_Write.m_PacketCtx != NULL)
                    {
                        m_Write.m_PacketCtx->m_Packet->Duplicate();
                        m_Write.m_PacketCtx = NULL;
                    }

                m_Vc->IOClose();
                m_Vc = NULL;

                CallConnectHandler(ConnectHandler::DisConnected);
            }
        else
            {
                // if the Vc is null, this means that the has not been established
                // yet.
                CallConnectHandler(ConnectHandler::ConnectFailed);
            }
    
        // If the user closed the connection, schdule an event to call back CloseCleanup()
        if (!m_Event && !m_ReentrantCount)
            {
                m_Event = new CompletionEvent(this, EVENT_IMMEDIATE);
                Netlib::GetNetProcessor()->ScheduleCompletion(m_Event, 0);
            }

        return 0;
    }

    int NetCxn::CloseCleanup()
    {
        // If we come here after cleaning up the last reference 
        // (i.e. DownCount destroyed the object) then we dont do anything else.
        if (m_Crefs == 0)
            {
                return 0;
            }

        LogDebugCxnMacro("In CloseCleanup");

        LogAssert(m_Closed);

        if (!m_Abort)
            {
                LogAssert(this->m_Svc != NULL);
            }

        if (m_Event)
            {
                m_Event->Cancel();
                m_Event = NULL;
            }

        if (m_ReenableReceiveEvent)
            {
                m_ReenableReceiveEvent->Cancel();
                m_ReenableReceiveEvent = NULL;
            }

        TxRxStatus status = (m_Abort) ? TxAbort : TxNoConnection;

        NetPacketCtx *ctx;
        while ((ctx = m_DisConnectq.dequeue()) != NULL)
            {
                // Debug listening
                if (NULL != m_pListener)
                    {
                        m_pListener->ProcessSend(ctx->m_Packet, status);
                    }

                m_SendHandler->ProcessSend(ctx->m_Packet, status);
            }
    
        if (m_Sendq.head)
            {
                // If the sendq contains any packets, try to re-connect.
                // If reconnect fails, it will call CloseConnection() reentrantly
                ReConnect();
                return 0;
            }
        // At this point, if the service was aborted, we are already
        // removed from the appropriate queue. Otherwise, do so now
        if (m_Svc)
            {
                MUTEX_TRY_LOCK(lock, m_Svc->m_Mutex);
                if (!lock)
                    {
                        // Re-schedule CloseCleanup()
                        m_Event = new CompletionEvent(this, EVENT_IMMEDIATE);
                        Netlib::GetNetProcessor()->ScheduleCompletion(m_Event, MUTEX_RETRY_DELAY);
                        return 0;
                    }
                m_Svc->m_CxnTable->Remove(this);
            }

        LogDebugCxnMacro("Deleting Connection");
        DownCount();
        return 0;
    }

    void NetCxn::CallConnectHandler(ConnectHandler::ConnectState state)
    {
        if (m_pListener)
            {
                m_pListener->ProcessConnect(m_RemoteIp, m_RemotePort, state);
            }
           
        if (m_ConnectHandler)
            {
                m_ConnectHandler->ProcessConnect(m_RemoteIp, m_RemotePort, state);
            }
    }

    void NetCxn::Start(NetVConnection *vc)
    {
        MUTEX_LOCK(lock, m_Mutex);
        m_Vc = vc;
        InitializeReadWriteContexts();

        // Its important to initialize everything before calling the user. The user
        // might close the connnection on the callback. This would end up calling
        // CloseConnection. The members of this object need to be properly initialized
        // when CloseConnection is called.
        CallConnectHandler(ConnectHandler::Connected);
        if (m_Closed)
            {
                return;
            }

        ProcessQueuedPacketsForWrite();

        m_Read.m_Vio = m_Vc->IORead(this, VIO::MAX_NBYTES, m_Read.m_NetBuffer);
        m_Write.m_Vio = m_Vc->IOWrite(this, VIO::MAX_NBYTES, m_Write.m_NetBuffer);

        return;
    }

    int NetCxn::HandlePacketEvent(Event event, void *data)
    {
        int timeout;
        HiResTime startTime = GetHiResTime();
        m_ReentrantCount++;
        switch(event) 
            {
            case NET_EVENT_CONNECT:
                LogAssert(m_Svc->m_IsServer == false);
                LogDebugCxnMacro("Connect Succeeded");
                m_ConnectAction = NULL;
                Start((NetVConnection *)data);
                break; 

            case NET_EVENT_CONNECT_FAILED:
                LogAssert(m_Svc->m_IsServer == false);
                LogDebugCxnMacro("Connect Failed");
                m_ConnectAction = NULL;

                // don't call CloseConnection right now. If Connect calls back
                // on the same stack, CloseConnection() would end calling CloseCleanup()
                // and we'll delete ourselves! The safer thing is to schedule
                // an event that will call us back right away.
                timeout = (m_Svc->m_FailOnDisconnect) ? 0 : CONNECT_RETRY_TIME;
                m_Event = new CompletionEvent(this, PACKET_CONNECT_FAILED);
                Netlib::GetNetProcessor()->ScheduleCompletion(m_Event, timeout);
                break;

            case PACKET_CONNECT_FAILED:
                m_Event = NULL;
                if (!m_Closed)
                    {
                        CloseConnection();
                    }
                break;

            case VC_EVENT_READ_COMPLETE:
            case VC_EVENT_READ_READY:
                ReadReadyInternal();
                break;

            case PACKET_REENABLE_RECEIVE:
                m_ReenableReceiveEvent = NULL;
                if (m_Vc)
                    {
                        ReadReadyInternal();
                        m_Vc->Reenable(m_Read.m_Vio);
                    }
                break;

            case VC_EVENT_WRITE_READY:
                m_InWriteCallback = true;
                WriteReadyInternal();
                m_InWriteCallback = false;
                break;

            case VC_EVENT_EOS:
                CloseConnection();
                break;

            case VC_EVENT_ERROR:
                CloseConnection();
                break;

            case EVENT_IMMEDIATE:
                m_Event = NULL;
                break;
            default:
                LogAssert(0);
            }

        HiResTime delay = GetHiResTime() - startTime;
        if (delay > NetProcessor::MAX_CALLBACK_DELAY)
            {
                Log(LogID_Netlib, LogLevel_Warning,
                    "long delay in NetCxn::HandleEvent (us, eventId)",
                    LogTag_NumericIP, m_RemoteIp,
                    LogTag_Port, m_RemotePort,  
                    LogTag_Int64_1, delay,
                    LogTag_UInt1, event,
                    LogTag_End);
                Logger::CallNotificationsIfDefined(LogLevel_Warning, LogID_Netlib, "long delay in NetCxn::HandleEvent (us, eventId)");
            }

        m_ReentrantCount--;
        if (m_ReentrantCount == 0 && m_Closed) {
            CloseCleanup();
        }
        // don't access any member variables after CloseCleanup(). The object is
        // deleted in CloseCleanup()
        return 0;
    }

    void NetCxn::Connect()
    {
        MUTEX_LOCK(lock, m_Mutex);
        LogAssert(m_Svc->m_IsServer == false);
        CallConnectHandler(ConnectHandler::Connecting);
        if (m_Closed)
            {
                return;
            }
    
        Action *action = Netlib::GetNetProcessor()->Connect(this, m_RemoteIp, m_RemotePort, m_Svc->m_BindIp);
        if (action == NULL)
            {
                return;
            }
        m_ConnectAction = action;
        LogDebugCxnMacro("Connection Initialized");
        return;
    }

    void NetCxn::ReConnect()
    {
        LogAssert(m_Svc != NULL && m_Svc->m_IsServer == false);
        LogAssert(m_DisConnectq.head == NULL);
    
        if (m_Closed)
            {
                LogDebugCxnMacro("ReConnecting");
                m_Read.Reset();
                m_Write.Reset();
                m_ConnectAction = NULL;
                m_Closed = false;
                m_Abort = false;
                CallConnectHandler(ConnectHandler::Connecting);
                if (m_Closed)
                    {
                        return;
                    }
            }

        Action *action = Netlib::GetNetProcessor()->Connect(this, m_RemoteIp, m_RemotePort, m_Svc->m_BindIp);
        if (action != NULL) {
            m_ConnectAction = action;
        }
        return;
    }

} // namespace RSLibImpl
