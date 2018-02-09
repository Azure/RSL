#include "NetAccept.h"
#include "NetProcessor.h"
#include "NetVConnection.h"

#include "stdafx.h"

namespace RSLibImpl
{

    long NetVConnection::m_Gid = 0;

#define LogDebugNetVCxnMacro0(msg)              \
    Log(LogID_Netlib, LogLevel_Debug, msg,      \
        LogTag_NumericIP, this->GetRemoteIp(),  \
        LogTag_Port, this->GetRemotePort(),     \
        LogTag_End);

#define LogDebugNetVCxnMacro1(msg, type1, value1)       \
    Log(LogID_Netlib, LogLevel_Debug, msg,              \
        type1, value1,                                  \
        LogTag_NumericIP, this->GetRemoteIp(),          \
        LogTag_Port, this->GetRemotePort(),             \
        LogTag_End);

#define LogDebugNetVCxnMacro2(msg, type1, value1, type2, value2)        \
    Log(LogID_Netlib, LogLevel_Debug, msg,                              \
        type1, value1,                                                  \
        type2, value2,                                                  \
        LogTag_NumericIP, this->GetRemoteIp(),                          \
        LogTag_Port, this->GetRemotePort(),                             \
        LogTag_End);

#define LogDebugNetVCxnMacro3(msg, type1, value1, type2, value2, type3, value3) \
    Log(LogID_Netlib, LogLevel_Debug, msg,                              \
        type1, value1,                                                  \
        type2, value2,                                                  \
        type3, value3,                                                  \
        LogTag_NumericIP, this->GetRemoteIp(),                          \
        LogTag_Port, this->GetRemotePort(),                             \
        LogTag_End);

    //////////////////////////////////////////////////////////////////
    //
    //  NetVConnection::free()
    //
    //////////////////////////////////////////////////////////////////

    NetVConnection::~NetVConnection(void)
    {
        LogAssert (m_ReentrantCount == 0);
        CancelRetry(&m_ReadState.m_retryVioEvent);
        CancelRetry(&m_WriteState.m_retryVioEvent);
        LogAssert (m_RetryScheduleRead == 0);
        LogAssert (m_RetryScheduleWrite == 0);
        LogAssert (m_RetryClose == NULL);

        if (m_ReadState.m_Vio.m_Buffer) {
            delete m_ReadState.m_Vio.m_Buffer;
        }
        if (m_WriteState.m_Vio.m_Buffer) {
            delete m_WriteState.m_Vio.m_Buffer;
        }

        if (m_S != INVALID_SOCKET)
            {
                closesocket(m_S);
            }
        m_ConnectAction.m_Mutex = 0;
        m_NetAcceptAction = 0;

        m_Mutex = 0;
    }

    //////////////////////////////////////////////////////////////////
    //
    //  NetVConnection::NetVConnection()
    //
    //////////////////////////////////////////////////////////////////
    NetVConnection::NetVConnection()
        :
        m_S(INVALID_SOCKET),
        m_Id(0),
        m_ReadState(),
        m_WriteState(),
        m_WaitToClose(false),
        m_Lerror(0),
        m_ConnectAction(),
        m_NetAcceptAction(0),
        m_RetryScheduleRead(0),
        m_RetryScheduleWrite(0),
        m_RetryClose(0),
        m_RetrySigAccept(0),
        m_NetProc(0),
        m_ReentrantCount(0)
    {
        memset(&m_RemoteAddr, '\0', sizeof(m_RemoteAddr));
        memset(&m_LocalAddr, '\0', sizeof(m_LocalAddr));
        m_Mutex = new MSMutex();
        m_Id = InterlockedIncrement(&NetVConnection::m_Gid);

        m_ReadState.m_Vio.SetVcServer(this);
        m_ReadState.m_Vio.SetOp(VIO::READ);
        m_WriteState.m_Vio.SetVcServer(this);
        m_WriteState.m_Vio.SetOp(VIO::WRITE);

        SetHandler((Handler) &NetVConnection::HandleEventCallback);
    }

    VIO * 
    NetVConnection::IORead(EventHandler * c, INT64 nbytes, NetBuffer * buf)
    {
        LogDebugNetVCxnMacro2("In IO Read (nbytes, read avail)",
                              LogTag_Int64_1, nbytes,
                              LogTag_Int1, buf->ReadAvail());

        m_ReadState.m_Cancelled = false;
        return (IOInternal(c, buf, nbytes, &m_ReadState.m_Vio));
    }

    VIO * 
    NetVConnection::IOWrite(EventHandler * c, INT64 nbytes, NetBuffer * buf)
    {
        LogDebugNetVCxnMacro2("In IO Write (nbytes, write avail)",
                              LogTag_Int64_1, nbytes,
                              LogTag_Int1, buf->WriteAvail());

        m_WriteState.m_Cancelled = false;
        return (IOInternal(c, buf, nbytes, &m_WriteState.m_Vio));
    }

    void 
    NetVConnection::Reenable(VIO * vio)
    {
        ReenableInternal(vio);
        return;
    }


    void 
    NetVConnection::IOClose()
    {
        ///////////////////////////////////////////
        // both read and write locks must be held//
        // by the caller of IOClose.             //
        ///////////////////////////////////////////
        
        LogDebugNetVCxnMacro0("In IOClose");

        IOShutdown(SD_BOTH);
        CloseInternal();
        return;
    }

    void 
    NetVConnection::IOShutdown(int howto)
    {
        LogDebugNetVCxnMacro0("In IOShutdown");

        switch (howto) 
            {
            case SD_RECEIVE:
                m_ReadState.m_Cancelled = true;
                m_ReadState.m_Vio.SetEventHandler(0);
                CancelRetry(&m_RetryScheduleRead);
                break;
      
            case SD_SEND:
                m_WriteState.m_Cancelled = true;
                m_WriteState.m_Vio.SetEventHandler(0);
                CancelRetry(&m_RetryScheduleWrite);
                break;
      
            case SD_BOTH:
                m_ReadState.m_Cancelled = true;
                m_ReadState.m_Vio.SetEventHandler(0);
                CancelRetry(&m_RetryScheduleRead);
                m_WriteState.m_Cancelled = true;
                m_WriteState.m_Vio.SetEventHandler(0);
                CancelRetry(&m_RetryScheduleWrite);
                break;
            }
        if (::shutdown(m_S, howto) == SOCKET_ERROR) {
            m_Lerror = WSAGetLastError();
        }
  
        return;
    }

    //////////////////////////////////////////////////////////////////
    //
    //  NetVConnection::handle_event_callback()
    //
    //  this event is called by NTEventProcessor on a completion
    //  of asynchronus ReadFile or WriteFile or on a timeout event.
    //////////////////////////////////////////////////////////////////
    int 
    NetVConnection::HandleEventCallback(Event event, void * data)
    {
        m_ReentrantCount++;
        CompletionEvent *e;
        switch (event)
            {
            case NETVC_CONNECT_INTERNAL:
                e = (CompletionEvent *)data;
                HandleConnectInternal(e);
                break;

            case NETVC_ACCEPT_INTERNAL:
                e = (CompletionEvent *)data;
                HandleAcceptInternal(e);
                break;

            case NETVC_ASYNC_READ_COMPLETE:
                HandleIOCompletion(&m_ReadState.m_Vio);
                break;

            case NETVC_ASYNC_WRITE_COMPLETE:
                HandleIOCompletion(&m_WriteState.m_Vio);
                break;

            case NETVC_RETRY_DO_IO_READ:
                {
                    m_RetryScheduleRead = NULL;
                    ScheduleInternal(&m_ReadState.m_Vio);
                    break;
                }
            case NETVC_RETRY_DO_IO_WRITE:
                {
                    m_RetryScheduleWrite = NULL;
                    ScheduleInternal(&m_WriteState.m_Vio);
                    break;
                }
            case NETVC_RETRY_VIO_LOCK_READ:
                ScheduleInternal(&m_ReadState.m_Vio);
                break;

            case NETVC_RETRY_VIO_LOCK_WRITE:
                ScheduleInternal(&m_WriteState.m_Vio);
                break;
          
            case NETVC_RETRY_DO_IO_CLOSE:
                {
                    m_RetryClose = NULL;
                    CloseInternal();
                    break;
                }
      
            case NETVC_RETRY_SIGNAL_ACCEPT:
                {
                    CompleteAcceptCallback();
                    break;
                }

            default:
                LogAssert (0);
                break;
            }

        /////////////////////////////////////////////
        // delete this connection if it is already //
        // closed and we are not waiting for any   //
        // completion events.                      //
        /////////////////////////////////////////////
        if ((m_WaitToClose) && (m_ReentrantCount == 1) &&
            (m_ReadState.m_CompletionEvent == 0) &&
            (m_WriteState.m_CompletionEvent == 0)   ) {
            CloseInternal();
        }

        m_ReentrantCount--;
        if ((m_S == INVALID_SOCKET) && (m_ReentrantCount == 0) &&
            (m_ReadState.m_CompletionEvent == 0) &&
            (m_WriteState.m_CompletionEvent == 0)   ) 
            {
                delete this;
            }

        return (0);
    }

    //
    //  NetVConnection::handle_connect_internal()
    //
    //  this event is called on a successfull or failed connect.
    int 
    NetVConnection::HandleConnectInternal(CompletionEvent *c_event)
    {
        LogDebugNetVCxnMacro0("In HandleConnectInternal");

        /////////////////////////////////////////////////
        // try to acquire the action's lock first. If  //
        // this failes then reschedule this completion //
        // event.                                      //
        /////////////////////////////////////////////////
        MUTEX_TRY_LOCK(lock, m_ConnectAction.m_Mutex);
        if (!lock) {
            m_NetProc->ScheduleCompletion(c_event, 0);
            return (0);
        }
        ///////////////////////////////////
        // check if connect was cancelled//
        ///////////////////////////////////
        if (m_ConnectAction.m_Cancelled) {
            ::closesocket(m_S);
            m_S = INVALID_SOCKET;
            return (0);
        }

        /////////////////////////////////
        // the connect is not cancelled//
        /////////////////////////////////
        int lerror = c_event->GetLerror();
        if (lerror != 0) {
            ////////////////////
            // connect failed //
            ////////////////////
            //Netlib::m_Counters.g_ctrNumConnectsFail->Increment();
            m_ConnectAction.m_Handler->HandleEvent(NET_EVENT_CONNECT_FAILED, 
                                                   IntToPtr(-lerror));
            ::closesocket(m_S);
            m_S = INVALID_SOCKET;
            return (0);
        }
        else {
            //Netlib::m_Counters.g_ctrNumConnectsSuccess->Increment();
            if (setsockopt(m_S, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) == SOCKET_ERROR)
                {
                    m_Lerror = WSAGetLastError();
                    Log(LogID_Netlib, LogLevel_Error, "setsockopt Failed",
                        LogTag_Ptr1, m_S, LogTag_LastError, m_Lerror, LogTag_End);
                    return (0);
                }

            int local_addr_len = sizeof(m_LocalAddr);
            if (getsockname(m_S, (sockaddr *)&m_LocalAddr, &local_addr_len) == SOCKET_ERROR)
                {
                    m_Lerror = WSAGetLastError();
                    Log(LogID_Netlib, LogLevel_Error, "getsockname Failed",
                        LogTag_LastError, m_Lerror, LogTag_End);
                    return (0);
                }
      
            if (m_RemoteAddr.sin_addr.s_addr == m_LocalAddr.sin_addr.s_addr &&
                m_RemoteAddr.sin_port == m_LocalAddr.sin_port)
                {
                    Log(LogID_Netlib, LogLevel_Assert, "Identical Connection Endpoints",
                        LogTag_NumericIP, this->GetLocalIp(),
                        LogTag_Port, this->GetLocalPort(),
                        LogTag_End);
                }

            m_ConnectAction.m_Handler->HandleEvent(NET_EVENT_CONNECT, this);
        }
        return (0);
    }

    //
    //  NetVConnection::handle_accept_event_callback()
    //
    //  This function is called by NetAccept when a new
    //  connection is accepted.
    int 
    NetVConnection::HandleAcceptInternal(CompletionEvent *c_event)
    {
        LogDebugNetVCxnMacro0("In HandleAcceptInternal");

        LPSOCKADDR  local_sockaddr;
        LPSOCKADDR  remote_sockaddr;    
        INT         local_sockaddr_length = 0;
        INT         remote_sockaddr_length = 0;

        MUTEX_TRY_LOCK(lock, this->m_Mutex);
        LogAssert (lock);

        if (c_event->m_Lerror != 0) {
            //Netlib::m_Counters.g_ctrNumAcceptsFail->Increment();
            ::closesocket(m_S);
            m_S = INVALID_SOCKET;
            return (0);
        }

        /////////////////////////////////////////////////
        // setsockopt SO_UPDATE_ACCEPT_CONTEXT to      //
        // inherit the properties of the listen socket.//
        /////////////////////////////////////////////////
        SOCKET listen_socket = m_NetAcceptAction->GetListenSocket();
        if ((setsockopt(m_S, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, 
                        (char *)&listen_socket, sizeof(SOCKET))) 
            == SOCKET_ERROR) {

            m_Lerror = WSAGetLastError();
            Log(LogID_Netlib, LogLevel_Error, "setsockopt Failed",
                LogTag_LastError, m_Lerror,
                LogTag_End);
            ::closesocket(m_S);
            m_S = INVALID_SOCKET;
            //Netlib::m_Counters.g_ctrNumAcceptsFail->Increment();
            return (0);
        }
        /////////////////////////////////////////////////////////
        // here we need to decrypt the return values from      //
        // AcceptEx completion event to get information about  //
        // the new connection.                                 //
        /////////////////////////////////////////////////////////

        //Netlib::m_Counters.g_ctrNumAcceptsSuccess->Increment();

        NetAccept *net_accept = (NetAccept *)c_event->m_Handler;
        NetProcessor::m_FnGetAcceptExSockaddrs(net_accept->GetAcceptBuffer(),
                                               0,
                                               net_accept->GetLocalAddressLength(),
                                               net_accept->GetRemoteAddressLength(),
                                               &local_sockaddr,
                                               &local_sockaddr_length,
                                               &remote_sockaddr,
                                               &remote_sockaddr_length);

        LogAssert (sizeof(m_LocalAddr)  == local_sockaddr_length);
        LogAssert (sizeof(m_RemoteAddr) == remote_sockaddr_length);

        m_LocalAddr  = *(struct sockaddr_in *)local_sockaddr;
        m_RemoteAddr = *(struct sockaddr_in *)remote_sockaddr;

        CompleteAcceptCallback();
        return (0);
    }

    void 
    NetVConnection::CompleteAcceptCallback()
    {
        LogDebugNetVCxnMacro0("In CompleteAcceptCallback");

        MUTEX_TRY_LOCK(lock, m_NetAcceptAction->m_Mutex);
        if (!lock) {
            m_RetrySigAccept = 
                new CompletionEvent(this, NETVC_RETRY_SIGNAL_ACCEPT);
            m_NetProc->ScheduleCompletion(m_RetrySigAccept, MUTEX_RETRY_DELAY);
            return;
        }
        // got the lock
        if (!m_NetAcceptAction->m_Cancelled) {
            m_NetAcceptAction->m_Handler->HandleEvent(NET_EVENT_ACCEPT, this);
        }
        else {
            ::closesocket(m_S);
            m_S = INVALID_SOCKET;
        }
    } 

    //  This function is called by IORead() and IOWrite()
    //  - schedule a new read/write
    //  - update the parameters of an already scheduled read.
    //  EventHandler's lock must already be taken. 
    VIO * 
    NetVConnection::IOInternal(EventHandler * c, NetBuffer * buf, 
                               INT64 nbytes, VIO * vio)
    {

        LogDebugNetVCxnMacro3("In IOInternal (nbytes, read avail, write avail)",
                              LogTag_Int64_1, nbytes,
                              LogTag_Int1, buf->ReadAvail(),
                              LogTag_Int2, buf->WriteAvail());

        /////////////////////////////////
        // assert rules and assumptions//
        /////////////////////////////////
        LogAssert (c != 0);
        LogAssert (buf != NULL || nbytes == 0);
        LogAssert (nbytes >= 0);

        Debug("iocore", "[%d] do_io_internal", m_Id);

        ///////////////////////////////////////////////////////////////////////
        // * The VIO is protected by the EventHandler's lock. We can safely  //
        //   set the new values in the VIO, but in order to schedule the     //
        //   I/O  or to check if an I/O is already scheduled we need to hold //
        //   this VConnection's lock.                                        //
        // * If an asynchronous I/O  is already scheduled - _xxx_state.      //
        //   completion_event is not NULL - then: mbuf must be the same as   //
        //   the one which is currently being read into. the new value       //
        //   for nbytes cannot be less than what is already read plus the    //
        //   maximum number of bytes that can be read in the read that is    //
        //   already scheduled.                                              //
        ///////////////////////////////////////////////////////////////////////


        /////////////////////////////////////////////////
        // setup the VIO with attributes for this read.//
        /////////////////////////////////////////////////
        vio->m_Mutex = c->GetMutex();
        vio->SetEventHandler(c);
        LogAssert (nbytes >= 0);
        vio->SetNDone(0);
        vio->SetNBytes(nbytes);
        vio->SetBuffer(buf);

        ReenableInternal(vio);
        return vio;
    }

    void 
    NetVConnection::CloseInternal()
    {
        /////////////////////////////////////////////////////////////
        // close socket:                                           //
        // ::closesocket() help page says "Any pending blocking,   //
        // asynchronous calls issued by any thread in this process //
        // are canceled without posting any notification messages. //
        // My understandong is that this can't be possible,        //
        // especially if the thread that calls ::closesocket() is  //
        // different then a thread that is about to pick up a      //
        // completion event for this socket. Testing this also     //
        // proves that we will get completion events after calling //
        // ::closesocket().                                        //
        // For now, it seems that the safest and most deterministic//
        // thing to do will be to not close the socket if there is //
        // a pending I/O. We will wait for the I/O to complete and //
        // only then close the socket.                             //
        /////////////////////////////////////////////////////////////

        LogDebugNetVCxnMacro0("In CloseInternal");

        MUTEX_TRY_LOCK(lock, this->m_Mutex);

        if (!lock) {
            m_RetryClose = new CompletionEvent(this, NETVC_RETRY_DO_IO_CLOSE);
            m_NetProc->ScheduleCompletion(m_RetryClose, MUTEX_RETRY_DELAY);
            return;
        }
  
        CancelRetry(&m_ReadState.m_retryVioEvent);
        CancelRetry(&m_WriteState.m_retryVioEvent);

        ///////////////////////////////////////////////////////////////
        // The sockets API help recommends to use shutdown           //
        // before calling close to avoid abortive shutdown problems. //
        // shutdown initiates the tear down sequence by sending a FIN//
        // packet to the other peer. It is not clear from the help   //
        // how long this side of the connection would wait for the   //
        // FIN packet from the peer.                                 //    
        ///////////////////////////////////////////////////////////////
        if ((m_ReadState.m_CompletionEvent == 0) &&
            (m_WriteState.m_CompletionEvent == 0)) {
            if (::closesocket(m_S) == SOCKET_ERROR) {
                LogAssert (!"closesocket failed");
            }
            m_WaitToClose = false;
            m_S = INVALID_SOCKET;
            /////////////////////////////////////////////////
            // m_ReentrantCount would be 0 if do_io_close()//
            // was not called from a callback of this      //
            // issued by this instance of NetVConnection.  //
            /////////////////////////////////////////////////
            if (m_ReentrantCount == 0) {
                delete this;;
            }
        }
        else {
            ////////////////////////////
            // there is a pending I/O //
            ////////////////////////////
            m_WaitToClose = true;
        }
        return;
    }

    void 
    NetVConnection::ReenableInternal(VIO *vio) {

        MUTEX_TRY_LOCK(lock, this->m_Mutex);

        if (!lock) {
            ScheduleRetry(vio);
            return;
        }
        ScheduleInternal(vio);
    }  

    void
    NetVConnection::ScheduleRetry(VIO *vio)
    {
        LogDebugNetVCxnMacro0("In ScheduleRetry");

        if (vio == &m_ReadState.m_Vio && !m_RetryScheduleRead) {
            m_RetryScheduleRead = 
                new CompletionEvent(this, NETVC_RETRY_DO_IO_READ);
            m_NetProc->ScheduleCompletion(m_RetryScheduleRead, MUTEX_RETRY_DELAY);
        }
        else if (vio == &m_WriteState.m_Vio && !m_RetryScheduleWrite) {
            m_RetryScheduleWrite = 
                new CompletionEvent(this, NETVC_RETRY_DO_IO_WRITE);
            m_NetProc->ScheduleCompletion(m_RetryScheduleWrite, MUTEX_RETRY_DELAY);
        }
    }

    void 
    NetVConnection::ScheduleInternal(VIO *vio)
    {
        int io_status;
        DWORD bytes_done = 0;
        OVERLAPPED * overlapped = 0;
        INT64 todo = 0;
        int avail, last_error = 0;
        IOState *state;
        char *buf;
        Event event;
  
        if (vio == &m_ReadState.m_Vio) {
            state = &m_ReadState;
            event = NETVC_RETRY_VIO_LOCK_READ;
        }    
        else {
            state = &m_WriteState;
            event = NETVC_RETRY_VIO_LOCK_WRITE;
        }

        CancelRetry(&state->m_retryVioEvent);

        MUTEX_TRY_LOCK(lock, vio->GetMutex());
        if (!lock)
            {
                if (state->m_CompletionEvent == NULL)
                    {
                        state->m_retryVioEvent = new CompletionEvent(this, event);
                        m_NetProc->ScheduleCompletion(state->m_retryVioEvent, MUTEX_RETRY_DELAY);
                    }
                return;
            }
  
        if (vio == &m_ReadState.m_Vio) {
        
            state = &m_ReadState;
            buf = vio->GetEnd();
            CancelRetry(&m_RetryScheduleRead);
        }    
        else {

            state = &m_WriteState;
            buf = vio->GetStart();
            CancelRetry(&m_RetryScheduleWrite);
        }


        if (state->m_Cancelled || state->m_CompletionEvent != 0) {
            //////////////////////////////////////////
            // an I/O is already scheduled. cannot  //
            // schedule more than one read on the   //
            // same VConnection.                    //
            //////////////////////////////////////////

            LogDebugNetVCxnMacro0("State Cancelled or Completion Event Not Null");
            return;
        }
  
        todo = vio->GetNTodo();
        avail = vio->GetContiguousAvail();

        if (!todo || !avail)
            return;            

        if (todo > avail)
            todo = avail;

        if (todo > (Int64) NET_IO_BATCH_SIZE)
            {
                todo = (Int64) NET_IO_BATCH_SIZE;
            }

        MUTEX_RELEASE(lock);
        /////////////////////////////////
        // setup for asynchronous read //
        /////////////////////////////////
        state->m_CompletionEvent = 
            new CompletionEvent(this, NETVC_ASYNC_READ_COMPLETE);
        overlapped = state->m_CompletionEvent->GetOverlapped();
        CompletionEvent * & ce = state->m_CompletionEvent;

        if (vio == &m_ReadState.m_Vio) {
      
            io_status = ReadFile((HANDLE)m_S, buf, (DWORD) todo,
                                 &bytes_done, overlapped);
            // Netlib::m_Counters.g_ctrNumReads->Increment();
        }
        else {

            state->m_CompletionEvent->SetCallbackEvent(NETVC_ASYNC_WRITE_COMPLETE);
            io_status = WriteFile((HANDLE)m_S, buf, (DWORD) todo,
                                  &bytes_done, overlapped);
            //Netlib::m_Counters.g_ctrNumWrites->Increment();
        }

        if (io_status == 0 && ((last_error = WSAGetLastError()) != ERROR_IO_PENDING)) {
            ///////////////////////////////////////////////
            // an error occurred. reschedule to call the //
            // user later as in completion event.        //
            ///////////////////////////////////////////////

            m_Lerror = last_error;

            // Log warning
            if (vio == &m_ReadState.m_Vio)
                {
                    LogDebugNetVCxnMacro1("ReadFile Failed", LogTag_LastError, m_Lerror);
                }
            else
                {
                    LogDebugNetVCxnMacro1("WriteFile Failed", LogTag_LastError, m_Lerror);
                }

            LogAssert (bytes_done == 0);
            ce->SetBytesTransferred(bytes_done);
            ce->SetLerror(m_Lerror);
            m_NetProc->ScheduleCompletion(ce, 0);
        }
    }

    //  update IOState structure, call user, and reschedule
    //  read, if there is still data to read.
    void 
    NetVConnection::HandleIOCompletion(VIO *vio)
    {
        int transferred;
        IOState *state;

        if (vio == &m_ReadState.m_Vio) {
            state = &m_ReadState;
        } else {
            state = &m_WriteState;
        }

        LogAssert (m_S != INVALID_SOCKET);

        MUTEX_TRY_LOCK(lock, vio->GetMutex());

        if (!lock) {
            m_NetProc->ScheduleCompletion(state->m_CompletionEvent, 0);
            return;
        }

        if (state->m_Cancelled) {
            ////////////////////////////////////////////////
            // This I/O has been cancelled, but we had to //
            // wait for its completion. nothing to do.    //
            ////////////////////////////////////////////////
            state->m_CompletionEvent = 0;
            return;
        }

        transferred = state->m_CompletionEvent->GetBytesTransferred();

        LogAssert (transferred >= 0);
        LogAssert (vio->GetNTodo() > 0);
        if (transferred > 0) {
            LogAssert(transferred <= vio->GetNTodo());
            if (vio == &m_ReadState.m_Vio)
                {
                    vio->Fill(transferred);
                    //Netlib::m_Counters.g_ctrBytesRead->Add(transferred);
                }
            else
                {
                    vio->Consume(transferred);
                    //Netlib::m_Counters.g_ctrBytesWritten->Add(transferred);
                }
            vio->AddNDone(transferred);
        }

        Event event = GetCallbackEvent(state, transferred);

        if (event == VC_EVENT_ERROR)
            {
                LogDebugNetVCxnMacro1("Error in Completion Event",
                                      LogTag_LastError, state->m_CompletionEvent->GetLerror());
            }

        ///////////////////////////////////////////////////////////
        // processing the completion ends. CompletionEvent will  //
        // be freed by the event processor.                      //
        // signal the user. The EventHandler's lock is already   //
        // held by this function.                                //
        ///////////////////////////////////////////////////////////
        state->m_CompletionEvent = 0;
        vio->GetEventHandler()->HandleEvent(event, vio);
  
        MUTEX_RELEASE(lock);

        if (event != VC_EVENT_ERROR && event != VC_EVENT_EOS)
            ScheduleInternal(vio);
    }

    EventHandler::Event 
    NetVConnection::GetCallbackEvent(IOState *state, 
                                     int bytes_transferred) 
    {
        if ((m_Lerror = state->m_CompletionEvent->GetLerror()) != 0) {
            return VC_EVENT_ERROR;
        }

        if (state == &m_ReadState) {
            if (bytes_transferred == 0) {
                return VC_EVENT_EOS;
            } else if (state->m_Vio.GetNBytes() == state->m_Vio.GetNDone()) {
                return VC_EVENT_READ_COMPLETE;
            } else {
                return VC_EVENT_READ_READY;
            }
        }
        else {
            if (state->m_Vio.GetNBytes() == state->m_Vio.GetNDone()) {
                return VC_EVENT_WRITE_COMPLETE;
            } else {
                return VC_EVENT_WRITE_READY;
            }
        }
    }

} // namespace RSLibImpl
