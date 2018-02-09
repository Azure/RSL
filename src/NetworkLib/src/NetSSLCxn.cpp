#include "NetSSLCxn.h"
#include "PacketUtil.h"
#include "NetPacket.h"
#include "DynString.h"

namespace RSLibImpl
{
    NetCxn* NetSslCxn::CreateNetCxn(NetPacketSvc *svc, UInt32 ip, UInt16 port)
    {
        if( SSLAuth::IsSSLEnabled() )
        {
            SSLInfo("SSL is enabled. Using ssl connection",
                       LogTag_NumericIP, ip,
                       LogTag_Port, port);

            return new NetSslCxn(svc, ip, port);
        }
        else
        {
            SSLWarning("SSL is not enabled. Using non-ssl connection",
                       LogTag_NumericIP, ip,
                       LogTag_Port, port);

            return NetCxn::CreateNetCxn(svc, ip, port);
        }
    }


    void NetSslCxn::Start(NetVConnection *vc)
    {
        MUTEX_LOCK(lock, m_Mutex);

#ifdef _DEBUG
        m_bytesRecv = m_bytesSent = m_pktIssued = m_pktSent = 0; m_pktRecv = 0;
#endif

        int availLen = m_readSocketBuffer.ReadAvail();
        if(availLen != 0)
            {
                m_readSocketBuffer.Consume(availLen); // resetting connection all old data is not required
            }

        availLen = m_decryptedData.ReadAvail();
        if(availLen != 0)
            {
                m_decryptedData.Consume(availLen);
            }

        if(m_pSerializedPktBuffer != NULL)
            {
                Netlib::GetBufferPool()->ReleaseBuffer(m_pSerializedPktBuffer);
                m_pSerializedPktBuffer = NULL;
            }
        m_serializedPktLength = m_chunkSent = 0;    

        m_Vc = vc;
        InitializeReadWriteContexts();

        if (m_Closed)
            {
                return;
            }

        ProcessQueuedPacketsForWrite();

        m_Read.m_Vio = m_Vc->IORead(this, VIO::MAX_NBYTES, m_Read.m_NetBuffer);
        m_Write.m_Vio = m_Vc->IOWrite(this, VIO::MAX_NBYTES, m_Write.m_NetBuffer);

        if(m_Svc->m_IsServer)
            {
                m_pSslAuth.reset(new SSLAuthClient());        
            }
        else
            {
                m_pSslAuth.reset(new SSLAuthServer());

                // Send SSPI request to server
                ProcessEncryptedBuffer(true);
            }

        return;
    }



    TxRxStatus NetSslCxn::EnqueuePacket(NetPacketCtx *ctx)
    {
#ifdef _DEBUG
        m_pktIssued++;
        DisplayDebugInfo ("Packet issued from local service");
#endif

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
        if (m_Vc && !m_InWriteCallback && IsSspiAuthCompleted())
            {
                WriteReadyInternal();
            }
        return TxSuccess;
    }

    TxRxStatus NetSslCxn::EnqueueAuthData(NetPoolBuffer *buffer)
    {
        // If this is a client connection and the connection is closed,
        // enqueue the packet only if the client wants to create a new
        // connection. The connection will attempt to reconnect in this
        // case. For Server connections, always return NoConnection if
        // the connection is closed.
        if (m_Closed)
            {
                return TxNoConnection;
            }
        m_SendAuthDataq.enqueue(buffer);

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

    HRESULT NetSslCxn::ProcessEncryptedBuffer(bool allowEmptyInput)
    {
        // copy data from old to new
        int totalDataAvailable = m_readSocketBuffer.ReadAvail();
        std::unique_ptr<BYTE[]> inDataBuff(new BYTE[totalDataAvailable]);    

        int dataAlreadyProcessed = 0;
        m_readSocketBuffer.CopyTo((char*)inDataBuff.get(), totalDataAvailable);

        std::unique_ptr<BYTE[]> outDataBuff(new BYTE[SSL_ENCRYPT_BUFFER_SIZE]);

        HRESULT err = S_OK;

        while(dataAlreadyProcessed < totalDataAvailable || allowEmptyInput)
            {
                allowEmptyInput = false;
        
                int cbInData = totalDataAvailable - dataAlreadyProcessed;
                int cbOutData = SSL_ENCRYPT_BUFFER_SIZE;
                ISSLAuth::OutputType outType;
#ifdef _DEBUG
                DisplayStrAndInt("ProcessData input", cbInData);
#endif
                err = m_pSslAuth->ProcessData(&cbInData, (LPVOID)((LPBYTE)inDataBuff.get() + dataAlreadyProcessed), &cbOutData, outDataBuff.get(), &outType);

#ifdef _DEBUG
                DisplayOutputFromProcessData(err, cbInData, cbOutData, outType);
#endif
        
                if( S_OK == err)
                    {
                        if(cbInData > 0)
                            {
                                m_readSocketBuffer.Consume(cbInData);
                                dataAlreadyProcessed += cbInData;
                            }

        
                        if( outType == ISSLAuth::OutputType::AuthData || outType == ISSLAuth::OutputType::AuthDataEnd)
                            {
                                if(cbOutData > 0)
                                    {
                                        // send it back to the remote endpoint
                                        NetPoolBuffer *buffer = new NetPoolBuffer( (UInt32) cbOutData);
                                        LogAssert(buffer != NULL);

                                        LogAssert(buffer->WriteAvail() >= cbOutData);
                                        buffer->CopyFrom((char*)outDataBuff.get(), cbOutData);
                                        buffer->Fill(cbOutData);

                                        if( TxSuccess != EnqueueAuthData(buffer))
                                            {
                                                delete buffer;
                                            }
                                    }
                            }
                        else
                            {
                                // use the decrypted data
                                if( m_decryptedData.WriteAvail() < cbOutData)
                                    {
                                        // Not enough space. Allocate more
                                        int bytesAlreadyRead = m_decryptedData.ReadAvail();
                                        int bytesNeeded = bytesAlreadyRead + cbOutData;
                                        Buffer *buffer = Netlib::GetBufferPool()->GetBuffer(bytesNeeded);
                                        m_decryptedData.CopyTo((char *)buffer->m_Memory, bytesAlreadyRead, 0);
                                        m_decryptedData.Consume(bytesAlreadyRead);
                                        m_decryptedData.SetPoolBuffer(buffer);
                                        m_decryptedData.Fill(bytesAlreadyRead);
                                    }

                                m_decryptedData.CopyFrom((char*)outDataBuff.get(), cbOutData);
                                m_decryptedData.Fill(cbOutData);
                            }

                        if(outType == ISSLAuth::OutputType::AuthDataEnd)
                            {
#ifdef _DEBUG
                                DisplayStrAndInt("Auth completed", 0);
#endif
                                if(! m_pSslAuth->IsCertCAValidationPassed() )
                                    {
                                        SSLError("Cert chain validation failed. Skipping",
                                                 LogTag_NumericIP, m_RemoteIp,
                                                 LogTag_Port, m_RemotePort);
                                    }
                                else
                                    {
                                        SSLInfo("SSPI SSL auth completed",
                                                LogTag_NumericIP, m_RemoteIp,
                                                LogTag_Port, m_RemotePort);
                                    }

                                CallConnectHandler(ConnectHandler::Connected);
                                WriteReadyInternal();                           // Invoke any packets that were not sent
                            }
                    }
                else if( SEC_E_INCOMPLETE_MESSAGE == err)
                    {
                        // not enough data. out of the loop. Request more data from the reader
                        err = S_OK;
                        break; 
                    }
                else
                    {
                        return err;
                    }
            }

        return err;
    }


    void NetSslCxn::ReadReadyInternal()
    {    
        if (m_ReceiveSuspended)
            {
                return;
            }



        int newDataLength = m_Read.m_NetBuffer->ReadAvail();
        
#ifdef _DEBUG
        m_bytesRecv += newDataLength;
        DisplayStrAndInt("Bytes received", newDataLength);
#endif
        
        if(m_readSocketBuffer.WriteAvail() < newDataLength)
            {
                // not enough read buffer. Allocate more
                int bytesAlreadyRead = m_readSocketBuffer.ReadAvail();
                int bytesNeeded = bytesAlreadyRead + newDataLength;
                Buffer *buffer = Netlib::GetBufferPool()->GetBuffer(bytesNeeded);
                LogAssert(NULL != buffer);
                m_readSocketBuffer.CopyTo((char *)buffer->m_Memory, bytesAlreadyRead, 0);
                m_readSocketBuffer.Consume(bytesAlreadyRead);
                m_readSocketBuffer.SetPoolBuffer(buffer);
                m_readSocketBuffer.Fill(bytesAlreadyRead);
            }

        // At this stage we have enough space in the buffer (but may not be continuously)
        int writableLength = m_readSocketBuffer.ContiguousWriteAvail();
        if(writableLength < newDataLength)
            {
                m_Read.m_NetBuffer->CopyTo(m_readSocketBuffer.GetEnd(), writableLength);
                m_Read.m_NetBuffer->Consume(writableLength);
                m_readSocketBuffer.Fill(writableLength);
                newDataLength -= writableLength;
                writableLength = m_readSocketBuffer.ContiguousWriteAvail();
            }

        m_Read.m_NetBuffer->CopyTo(m_readSocketBuffer.GetEnd(), newDataLength);
        m_Read.m_NetBuffer->Consume(newDataLength);
        m_readSocketBuffer.Fill(newDataLength);

        HRESULT hr = ProcessEncryptedBuffer();

#ifdef _DEBUG
        DisplayStrAndInt("ProcessEncryptedBuffer result", hr);
#endif
    
        if(S_OK != hr)
            {
                SSLError("SSPI ProcessData failed",
                         LogTag_NumericIP, m_RemoteIp,
                         LogTag_Port, m_RemotePort,
                         LogTag_StatusCode, hr);
                // failure. close connection
                CloseConnection();
                return;
            }

   
        while (m_decryptedData.ReadAvail() >= (int) PacketHdr::SerialLen)
            {
                if (m_Read.m_Packet == NULL)
                    {
                        m_Read.m_Packet = m_Svc->m_PacketFactory->CreatePacket();

                        if (!m_Read.m_Packet->DeSerializeHeader(&m_decryptedData))
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

                if ((int) packetLength <= m_decryptedData.ReadAvail())
                    {
                        // Got full packet
                        Packet *pkt = m_Read.m_Packet;
                        bool pktValid = pkt->DeSerialize(&m_decryptedData, false);
            
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
            
                        m_decryptedData.Consume(packetLength);
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

                        // Debug listening
                        if (NULL != m_pListener)
                            {
                                m_pListener->ProcessReceive(pkt);
                            }

                        HiResTime startTime = GetHiResTime();

                        m_ReceiveHandler->ProcessReceive(pkt);

#ifdef _DEBUG                        
                        m_pktRecv++;
                        DisplayDebugInfo ("Packet recv from remote service");
#endif

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

    int NetSslCxn::WriteReadyInternal()
    {
        // check if there are any packets to be written
        VIO *vio = m_Write.m_Vio;

        bool authPacketPresent = ProcessQueuedPacketsForWrite();

        // Reenable only if we are not on the callback stack from 
        // the NetVConnection and we have a packet to write.
        // Reenabling on the callback stack is wasteful since the
        // NetVConnection is going to check if it needs to send any
        // data right after the callback. Also, if vio->ReadAvail()
        // is not 0, then we must have already done a reenable before.

        if ((m_Write.m_PacketCtx || authPacketPresent) && !m_InWriteCallback) {

            m_Vc->Reenable(vio);
        }
        return 0;
    }


    DWORD NetSslCxn::EncryptData(Buffer *pDataToSend, int startIndex, int dataLength, NetPoolBuffer **ppEncryptedData)
    {
        int maxSizeRequired = dataLength + SSL_ENCRYPT_HEADER_SIZE + SSL_ENCRYPT_TRAILER_SIZE_MAX;
        *ppEncryptedData = new NetPoolBuffer(maxSizeRequired);

        char * pEncBuffDataStart =  (*ppEncryptedData)->GetStart();

        if(m_pSslAuth.get() == NULL)
            {
                return ERROR_INVALID_HANDLE;
            }
    
        DWORD err = m_pSslAuth->EncryptData(dataLength, (LPVOID)((LPBYTE)pDataToSend->m_Memory + startIndex), &maxSizeRequired, pEncBuffDataStart);

        if(ERROR_SUCCESS == err)
            {
                (*ppEncryptedData)->Fill(maxSizeRequired);
            }
        else
            {
                SSLError("SSPI EncryptData failed",
                         LogTag_NumericIP, m_RemoteIp,
                         LogTag_Port, m_RemotePort,
                         LogTag_StatusCode, err);
            }

        return err;
    }


    // We'll only write one packet at a time.
    bool NetSslCxn::ProcessQueuedPacketsForWrite()
    {

        Log(LogID_Netlib, LogLevel_Debug, "In ProcessQueuedPacketsForWrite"
            "(buffer, size, start, end, isFull, ctx, callback)",
            LogTag_U64X1, (UInt64) m_Write.m_NetBuffer->GetBuffer(),
            LogTag_Int1, m_Write.m_NetBuffer->GetBufSize(),
            LogTag_Ptr1, m_Write.m_NetBuffer->GetStart(),
            LogTag_Ptr2, m_Write.m_NetBuffer->GetEnd(),
            LogTag_Int2, m_Write.m_NetBuffer->IsFull(),
            LogTag_Ptr1, m_Write.m_PacketCtx,
            LogTag_Int1, m_InWriteCallback,
            LogTag_NumericIP, m_RemoteIp,
            LogTag_Port, m_RemotePort,
            LogTag_End);

        // Can't do much until the current packet has been fully written
        // out or there wasn't a packet scheduled at all
        if (!m_Write.m_NetBuffer->IsEmpty())
            {
                return false;
            }

        Buffer *bufferWritten = m_Write.m_NetBuffer->GetPoolBuffer();
        NetPoolBuffer *authPoolBuffer = NULL;
        if(m_SendAuthDataq.head && bufferWritten == m_SendAuthDataq.head->GetPoolBuffer())
            {
                authPoolBuffer = m_SendAuthDataq.dequeue();
            }
    
        m_Write.m_NetBuffer->Clear();

        if(authPoolBuffer)
            {
                m_Write.m_PacketCtx = NULL;
        
                delete authPoolBuffer;
            }
        else if(bufferWritten)
            {
                LogAssert(m_InWriteCallback == true);
                Netlib::GetBufferPool()->ReleaseBuffer(bufferWritten);
            }

        if(m_chunkSent == m_serializedPktLength)
            {
                if (m_Write.m_PacketCtx)
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

#ifdef _DEBUG
                        m_pktSent++;
                        DisplayDebugInfo ("Packet sent to remote service");
#endif
                    }

                if(m_pSerializedPktBuffer != NULL)
                    {
                        Netlib::GetBufferPool()->ReleaseBuffer(m_pSerializedPktBuffer);
                        m_pSerializedPktBuffer = NULL;
                    }

                m_serializedPktLength = m_chunkSent = 0;
            }

    
        m_bIsSendingAuthData = false;        

        // If the connection was closed in the ProcessSend() callback,
        // it is safest to just return.
        if (m_Closed)
            {
                return false;
            }

        // Ready to send another packet
        if(m_SendAuthDataq.head)
            {
                NetPoolBuffer *buffer = m_SendAuthDataq.head;
                m_Write.m_NetBuffer->SetPoolBuffer(buffer->GetPoolBuffer());
                m_Write.m_NetBuffer->Fill(buffer->ReadAvail());
                m_Write.m_PacketCtx = NULL;

#ifdef _DEBUG
                DisplayStrAndInt("Sending Auth Data size", buffer->ReadAvail());
#endif

                if(m_pSerializedPktBuffer != NULL)
                    {
                        Netlib::GetBufferPool()->ReleaseBuffer(m_pSerializedPktBuffer);
                        m_pSerializedPktBuffer = NULL;
                    }
                m_serializedPktLength = m_chunkSent = 0;


                m_bIsSendingAuthData = true;
                return true;
            }
        else if(IsSspiAuthCompleted())
            {    
                int startIndex = 0;
                int dataLength = 0;
                if(m_pSerializedPktBuffer != NULL)
                    {
                        startIndex = m_chunkSent;
                        dataLength = m_serializedPktLength - m_chunkSent;

#ifdef _DEBUG
                        DisplayStrAndInt("Sending remaining chunk from", m_chunkSent);
#endif
                    }
                else if (m_Sendq.head)
                    {
                        NetPacketCtx *ctx = m_Sendq.head;
                        ctx->m_Packet->Serialize();

                        dataLength = m_serializedPktLength = ctx->m_Packet->GetPacketLength();
                        m_pSerializedPktBuffer = Netlib::GetBufferPool()->GetBuffer(m_serializedPktLength);
                        LogAssert(NULL != m_pSerializedPktBuffer);                

                        errno_t err = memcpy_s(m_pSerializedPktBuffer->m_Memory, m_serializedPktLength, ctx->m_Packet->GetPoolBuffer()->m_Memory, m_serializedPktLength);
                        if(err != ERROR_SUCCESS)
                            {
                                return false;
                            }

#ifdef _DEBUG
                        DisplayStrAndInt("Sending new packet", m_serializedPktLength);
#endif

                        startIndex = m_chunkSent = 0;
                        m_Write.m_PacketCtx = m_Sendq.head;
                    }
                else
                    {
                        return false;
                    }
        
                NetPoolBuffer *pEncryptedNetBuffer = NULL;
                if(dataLength > SSL_MAX_ENCRYPT_DATA_SIZE)
                    {
                        dataLength = SSL_MAX_ENCRYPT_DATA_SIZE;
                    }

                m_chunkSent += dataLength;
#ifdef _DEBUG
                DisplayStrAndInt("Sending chunk size", dataLength);
                DisplayStrAndInt("Chunks sent so far", m_chunkSent);
#endif
        
                DWORD err = EncryptData(m_pSerializedPktBuffer, startIndex, dataLength, &pEncryptedNetBuffer);
                if(ERROR_SUCCESS == err)
                    {
                        int encryptedDataSize = pEncryptedNetBuffer->ReadAvail();
                        m_Write.m_NetBuffer->SetPoolBuffer(pEncryptedNetBuffer->GetPoolBuffer());
                        m_Write.m_NetBuffer->Fill(encryptedDataSize);
                        pEncryptedNetBuffer->Clear();
                        delete pEncryptedNetBuffer;

#ifdef _DEBUG
                        m_bytesSent += encryptedDataSize;
                        DisplayStrAndInt("Encrypted data sent", encryptedDataSize);
#endif
                    }
                else
                    {
                        // do something
                        SSLError("Encrypt data failed for channel",
                                 LogTag_NumericIP, m_RemoteIp,
                                 LogTag_Port, m_RemotePort,
                                 LogTag_StatusCode, err);
                        // failure. close connection
                
                        CloseConnection();
                    }        

                //Netlib::m_Counters.g_ctrSendPacketSize->Set(ctx->m_Packet->GetPacketLength());
            }
    
        return false;
    }




    // CloseConnection
    // May be called on the user's call stack. It doesn't call back
    // the user on the same stack but schedules an Event to invoke
    // CloseCleanup() (that calls back the user with aborts)
    int NetSslCxn::CloseConnection(bool abort)
    {
#ifdef _DEBUG
        DisplayDebugInfo("CloseConnection");
#endif


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

        if(m_SendAuthDataq.head != NULL && m_DeleteSendAuthDataq.head == NULL)
            {
                NetPoolBuffer *tmp;
                while((tmp = m_SendAuthDataq.dequeue()) != NULL)
                    {
                        if(tmp->GetPoolBuffer()== m_Write.m_NetBuffer->GetPoolBuffer())
                            {
                                // If buffer is already given to NetVConnection, it will be cleaned by NetVConnection
                                tmp->Clear(); // Detaches the buffer from NetPoolBuffer, so we can delete it
                            }

                        m_DeleteSendAuthDataq.enqueue(tmp);
                    }
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


    int NetSslCxn::CloseCleanup()
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

        NetPoolBuffer *buffer;
        while((buffer = m_DeleteSendAuthDataq.dequeue()) != NULL)
            {
                delete buffer;
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
}

