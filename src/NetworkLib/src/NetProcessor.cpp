#include <process.h>

#include "NetAccept.h"
#include "NetProcessor.h"
#include "NetVConnection.h"

namespace RSLibImpl
{

    const int NetProcessor::ACCEPTEX_POOL_SIZE         = 16;
    const int NetProcessor::LISTEN_QUEUE_SIZE          = 1024;
    const HiResTime NetProcessor::MAX_EVENT_DELAY      = 10; // milliseconds

    const HiResTime NetProcessor::EVENT_RETRY_TIME     = HRTIME_MSECONDS(10);

    const HiResTime NetProcessor::MAX_MAINLOOP_DELAY   = HRTIME_MSECONDS(250);
    const HiResTime NetProcessor::MAX_CALLBACK_DELAY   = HRTIME_MSECONDS(100);

    LPFN_ACCEPTEX NetProcessor::m_FnAcceptEx = NULL;
    LPFN_CONNECTEX NetProcessor::m_FnConnectEx = NULL;
    LPFN_GETACCEPTEXSOCKADDRS NetProcessor::m_FnGetAcceptExSockaddrs;

    //
    //  NetProcessor::NetProcessor()
    //

    NetProcessor::NetProcessor() :
        m_NThreads(0), m_Threads(NULL), m_NextThread(0), m_stopped(false)
    {
        return;
    }

    NetVConnection * 
    NetProcessor::NewNetVConnection(void)
    {
        NetVConnection *res = new NetVConnection();
        res->m_NetProc = this;
        LogAssert(res);
        return(res);
    }

    //
    //  NetProcessor::~NetProcessor()
    //
    NetProcessor::~NetProcessor()
    {
        return;
    }

    unsigned __stdcall ThreadProc(void *arg)
    {
        BOOL ret = ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        LogAssert(TRUE == ret);
        LogAssert(arg != NULL);

        NetProcessor::ThreadCtx *thread = (NetProcessor::ThreadCtx *) arg;
        NetProcessor *net = thread->m_Processor;
        net->MainLoop(thread);
        // MainLoop should exit only if netprocess has been stopped.
        _endthreadex(0);
        return 0;
    }

    HANDLE 
    NetProcessor::RegisterHandle(HANDLE  h, DWORD completionKey)
    {
        HANDLE rcport = CreateIoCompletionPort(h, m_Cport, 
                                               completionKey, m_NThreads);

        if (rcport == NULL) {
            m_Lerror = GetLastError();
            Log(LogID_Netlib, LogLevel_Error, 
                "CreateIoCompletionPort Failed",
                LogTag_LastError, m_Lerror,
                LogTag_End);
            return (0);
        }

        LogAssert(rcport == m_Cport);
        return (m_Cport);
    }
    //
    //  NetProcessor::mainLoop()
    //

    void 
    NetProcessor::MainLoop(ThreadCtx *thread)
    {
        BOOL r;
        DWORD bytesTransferred;

        ULONG_PTR completionKey;

        LPOVERLAPPED overlapped;
        DWORD timeout;
        int lerror;

        HiResTime lastScheduledTime = GetHiResTime();

        timeout = MAX_EVENT_DELAY;
  
        while (1) 
            {
                if (m_stopped)
                    {
                        return;
                    }
      
                ///////////////////////////////////////////
                // block on the completion port until a  //
                // completion event arrived or a timeout.//
                ///////////////////////////////////////////
                r = GetQueuedCompletionStatus(m_Cport, &bytesTransferred, 
                                              &completionKey, &overlapped, timeout);
                ///////////////////////////////////////////////////////
                // return values from GetQueuedCompletionStatus:     // 
                //                                                   //
                //          +-------------+--------+--------------+  //
                //          | successfull | failed |    timeout   |  //
                //          |    I/O      |  I/O   |              |  //
                // ---------+-------------+--------+--------------+  //
                //       r  |    != 0     |   0    |       0      |  //
                // ---------+-------------+--------+--------------+  //
                // GetLast  |             |        | WAIT_TIMEOUT |  //
                //  Error   |             |        |              |  //
                // ---------+-------------+--------+--------------+  //
                ///////////////////////////////////////////////////////

                // get the current time
                thread->m_Now = GetHiResTime();

                HiResTime delay = thread->m_Now - lastScheduledTime;
                lastScheduledTime = thread->m_Now;
                if (delay > MAX_MAINLOOP_DELAY)
                    {
                        Log(LogID_Netlib, LogLevel_Warning, "long delay in mainloop (us)",
                            LogTag_Int64_1, delay,
                            LogTag_End);
                        Logger::CallNotificationsIfDefined(LogLevel_Warning, LogID_Netlib, "long delay in mainloop (us)");
                    }

                if (r == 0) 
                    {
                        if ( ERROR_OPERATION_ABORTED ==  (lerror = GetLastError()) )
                            // TODO: free the completion event.
                            continue;
                    } 
                else 
                    {
                        lerror = 0;
                    }

                if (completionKey == IO_COMPLETION_KEY_SHUTDOWN) {
                    ////////////////////////////////////////////////
                    // NetProcessor::shutdown signalled to exit.//
                    ////////////////////////////////////////////////
                    ExitThread(0);
                    return;
                }

                if (0 != overlapped && lerror !=  WAIT_TIMEOUT) {
                    /////////////////////////////////////
                    // successfull or failed I/O, but  //
                    // not a timeout.                  //
                    /////////////////////////////////////
                    /////////////////////////////////////
                    // get the Event from overlapped   //
                    /////////////////////////////////////
                    Overlapped * cpol = (Overlapped *)overlapped;
                    CompletionEvent * event= cpol->m_Event;
                    event->m_BytesTransferred = bytesTransferred;
                    event->m_CompletionKey = (DWORD) completionKey;
                    event->m_Lerror = ((r == 0) ? (lerror) : (0));

                    CallbackEvent(event);
                } /* end handle I/O completion */

                ProcessTimedEvents(thread);
                //////////////////////////////////////////////////////////////
                // At this point there are no more timed events to process. //
                // Go back to wait at GetQueuedCompletionStatus() for an    //
                // I/O completion event or for timeout to process more timed//
                // events.                                                  //
                //////////////////////////////////////////////////////////////
    
                ////////////////////////////////////////////////////////
                // get the earliest timeout from the thread's priority //
                // queue or return min_timeout, whatever is earlier.   //
                /////////////////////////////////////////////////////////
                timeout = MAX_EVENT_DELAY;
                HiResTime hiResTimeout = thread->m_EventQueue->EarliestTimeout();
                if ((hiResTimeout - thread->m_Now) < HRTIME_MSECONDS(MAX_EVENT_DELAY)) {
                    timeout = (DWORD) ((hiResTimeout - thread->m_Now)/(HRTIME_MSECOND));
                } 
            }
    }

    void
    NetProcessor::CallbackEvent(CompletionEvent *event) {


        MUTEX_TRY_LOCK (lock, event->m_Mutex);

        ///////////////////////////////////////////////
        // handle I/O completion event:
        // if acquire user's lock fails re-post this
        // completion to try again later.
        ///////////////////////////////////////////////
        if (event->m_Cancelled) {
            delete event;
            return;
        }

        if (lock) {
            if (!event->m_Cancelled) {
                event->m_Scheduled = 0;
                HiResTime startTime = GetHiResTime();
      
                event->m_Handler->HandleEvent(event->m_CallbackEvent, event);
      
                HiResTime delay = GetHiResTime() - startTime;
                if (delay > MAX_CALLBACK_DELAY)
                    {
                        Log(LogID_Netlib, LogLevel_Warning, "long delay in callback (us, event, callback addr)",
                            LogTag_Int64_1, delay,
                            LogTag_UInt1, event->m_CallbackEvent,
                            LogTag_Ptr1, event->m_Handler,
                            LogTag_End);
                        Logger::CallNotificationsIfDefined(LogLevel_Warning, LogID_Netlib, "long delay in callback (us, event, callback addr)");
                    }

                if (!event->m_Scheduled) {
                    delete event;
                }
            }
            else {
                //////////////////////////////////////
                // the I/O was cancelled by the user//
                /////////////////////////////////////
                delete event;
            }
        }
        else {
#if 0
            printf("missed lock, %p %p %d %u\n", event, event->m_Handler,
                   event->m_CallbackEvent, (int) event->m_Timeout);
#endif
            ScheduleCompletion(event, EVENT_RETRY_TIME);
        }
    }

    void
    NetProcessor::ProcessTimedEvents(ThreadCtx *thread) {

        PriorityEventQueue & eventq = *thread->m_EventQueue;
  
        CompletionEvent * e = 0;
        HiResTime now = thread->m_Now;
  
        Queue<CompletionEvent> q;
        thread->m_QueueMutex.Acquire();
        q = thread->m_Queue;
        thread->m_Queue.clear();
        thread->m_QueueMutex.Release();
  
        while ((e = q.dequeue()) != NULL) {
            if (e->m_Cancelled) {
                delete e;
                continue;
            }
            if (e->m_Timeout <= now) {
                CallbackEvent(e);
            }
            else {
                eventq.Enqueue(e, now);
            }
        }
        ///////////////////////////////////
        // process timed events from the //
        // thread's priority queue       //
        ///////////////////////////////////
        bool done_one;
        do {
            done_one = false;
            eventq.Check(now);
            while ((e = eventq.Dequeue()) != NULL) {
                done_one = true;
                CallbackEvent(e);
            } 
        } while (done_one);

        thread->m_QueueMutex.Acquire();
        q = thread->m_Queue;
        thread->m_Queue.clear();
        thread->m_QueueMutex.Release();
  
        while ((e = q.dequeue()) != NULL) {
            if (e->m_Cancelled) {
                delete e;
                continue;
            }
            eventq.Enqueue(e, now);
        }
    }

    void   
    NetProcessor::ScheduleCompletion(CompletionEvent *e, HiResTime timeout)
    {
  
        int next = m_NextThread++ % m_NThreads;
        ThreadCtx *thread = &m_Threads[next];
        //////////////////////////////////////////////
        // insert at the ethread's protected queue. //
        //////////////////////////////////////////////
        HiResTime now = GetHiResTime();  

        e->m_Timeout = now + timeout;
        e->m_Scheduled = 1;
        thread->m_QueueMutex.Acquire();
        thread->m_Queue.enqueue(e);
        thread->m_QueueMutex.Release();

        // maybe we need to wake up the thread if it is sleeping
        return;
    }

    //
    //  NetProcessor::start()
    //
    bool 
    NetProcessor::Start(int n_threads)
    {
  
        LogAssert(n_threads > 0);
        m_NextThread = 0;
        // get all the extenstion functions;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            m_Lerror = GetLastError();
            Log(LogID_Netlib, LogLevel_Error, "Create Socket Failed",
                LogTag_LastError, m_Lerror,
                LogTag_End);
            return false;
        }

        GUID AcceptExId = WSAID_ACCEPTEX;
        GUID ConnectExId = WSAID_CONNECTEX;
        GUID GetSockaddrsId = WSAID_GETACCEPTEXSOCKADDRS;
        DWORD bytes = 0;

        int r;
        r = WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, 
                     &AcceptExId, sizeof(AcceptExId),
                     &m_FnAcceptEx, sizeof(m_FnAcceptEx), 
                     &bytes, NULL, NULL);
        if (r == SOCKET_ERROR) {
            m_Lerror = WSAGetLastError();
            Log(LogID_Netlib, LogLevel_Error, 
                "WSAIoctl for AcceptEx failed",
                LogTag_LastError, m_Lerror,
                LogTag_End);
            return false;
        }

        r = WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, 
                     &ConnectExId, sizeof(ConnectExId),
                     &m_FnConnectEx, sizeof(m_FnConnectEx), 
                     &bytes, NULL, NULL);
        if (r == SOCKET_ERROR) {
            m_Lerror = WSAGetLastError();
            Log(LogID_Netlib, LogLevel_Error, 
                "WSAIoctl for ConnectEx failed",
                LogTag_LastError, m_Lerror,
                LogTag_End);
            return false;
        }

        r = WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, 
                     &GetSockaddrsId, sizeof(GetSockaddrsId),
                     &m_FnGetAcceptExSockaddrs, sizeof(m_FnGetAcceptExSockaddrs), 
                     &bytes, NULL, NULL);
        if (r == SOCKET_ERROR) {
            m_Lerror = WSAGetLastError();
            Log(LogID_Netlib, LogLevel_Error, 
                "WSAIoctl for GetAccepExSockAddrs failed",
                LogTag_LastError, m_Lerror,
                LogTag_End);
            return false;
        }
        closesocket(s);
        m_NThreads = n_threads;
        m_Threads = new ThreadCtx[m_NThreads];

        m_Cport = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, m_NThreads);

        if (m_Cport == NULL) {
            m_Lerror = GetLastError();
            Log(LogID_Netlib, LogLevel_Error, "CreateIoCompletionPort Failed",
                LogTag_LastError, m_Lerror,
                LogTag_End);
            return false;
        }

        for (int i = 0; i < m_NThreads; i++) {
            ThreadCtx *thread = &m_Threads[i];
            thread->m_Processor = this;
            thread->m_Now = GetHiResTime();
            thread->m_EventQueue = new PriorityEventQueue();
            unsigned threadId;
            uintptr_t h = _beginthreadex(NULL,0, ThreadProc, (void *)thread, NULL, &threadId);
        
            if (h == 0) {
                m_Lerror = GetLastError();
                // This is bad. Terminate
                Log(LogID_Netlib, LogLevel_Assert, "_beginthreadex Failed",
                    LogTag_LastError, m_Lerror,
                    LogTag_End);
            }
            m_Threads[i].m_Handle = (HANDLE) h;
            m_Threads[i].m_Id = threadId;
        }

        return true;
    }

    void
    NetProcessor::Stop()
    {
        // DON'T log anything else. The logging API might have been
        // uninitialized at this point
        m_stopped = true;
        for (int i = 0; i < m_NThreads; i++)
            {
                Logger::WaitForThreadOrExit(m_Threads[i].m_Handle, m_Threads[i].m_Id);
                CloseHandle(m_Threads[i].m_Handle);
            }
        m_NThreads = 0;
    }

    bool 
    NetProcessor::IsNetlibThread(){
        DWORD myId = GetCurrentThreadId();

        for(int i = 0; i < m_NThreads; i ++){
            if(myId == m_Threads[i].m_Id) return true;
        }

        return false;
    }
  
    //
    //  NetProcessor::connect()
    //
    Action * 
    NetProcessor::Connect(EventHandler * cont, unsigned int ip,
                          unsigned short port, int recv_bufsize,
                          int send_bufsize)
    {
        return (Connect(cont, ip, port, INADDR_ANY,
                        recv_bufsize, send_bufsize));
    }

    Action * 
    NetProcessor::Connect(EventHandler * cont, struct in_addr addr,
                          unsigned short port, int recv_bufsize, int send_bufsize)

    {
        return (Connect(cont, addr.s_addr, port, INADDR_ANY, recv_bufsize, send_bufsize));
    }

    //
    //  NetProcessor::connect()
    //
    Action * 
    NetProcessor::Connect(EventHandler * cont, unsigned int ip,
                          unsigned short port, unsigned int bindIp, 
                          int recv_bufsize, int send_bufsize)

    {
        int lerror = 0;
        SOCKET sockfd = INVALID_SOCKET;
        NetVConnection * netvc = 0;
        CompletionEvent * connectCompletionEvent = 0;

        //Netlib::m_Counters.g_ctrNumConnects->Increment();

        MUTEX_TRY_LOCK (lock, cont->GetMutex());
        LogAssert (lock);

        ///////////////////////////////////////////////
        // create an instance of NetVConnection. On  //
        // completion handle_event of NetVConnection //
        // is called.                                //
        ///////////////////////////////////////////////
        netvc = NewNetVConnection();
        struct sockaddr_in & local_addr = netvc->m_LocalAddr;
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = bindIp;
        local_addr.sin_port = 0;

        /////////////////////////////////////////////////////////
        // create socket.
        /////////////////////////////////////////////////////////
        if ((sockfd = 
             CreateStreamSocketInternal(&local_addr, recv_bufsize, send_bufsize)) 
            == INVALID_SOCKET) 
            {
                // If we got here, CreateStreamSocketInternal would have logged
                // the error - so no need to log again
                lerror = WSAGetLastError();
                delete netvc;
                SignalUserReInternal(cont, EventHandler::NET_EVENT_CONNECT_FAILED, 
                                     IntToPtr(-lerror));

                return (NULL);
            }

        /////////////////////////////////////////////////////////////
        // - create completion event which encapsulate OVERLAPPED  //
        //   that is passed to the asynchronous call.              //
        // - create ConnectAction, which is the Action structure //
        //   that returned to the user. The user uses this to      //
        //   cancel the connect before it completes. This Action   //
        //   contains reference to the EventHandler that is passed //
        //   in to this call.
        /////////////////////////////////////////////////////////////
        connectCompletionEvent = 
            new CompletionEvent(netvc, EventHandler::NETVC_CONNECT_INTERNAL);

        netvc->m_S = sockfd;
        Log(LogID_Netlib, LogLevel_Debug, "Created Socket for Connecting",
            LogTag_Ptr1, netvc->m_S, LogTag_NumericIP, ip,
            LogTag_Port, port, LogTag_End);
  
        /////////////////////
        // setup sockaddr. //
        /////////////////////
        struct sockaddr_in & remote_addr = netvc->m_RemoteAddr;
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_addr.s_addr = ip;
        remote_addr.sin_port = htons(port);


        netvc->m_ConnectAction.Action::operator = (cont);

        /////////////
        // connect //
        /////////////
        if (RegisterHandle((HANDLE) sockfd, IO_COMPLETION_KEY_NONE) == NULL)
            {
                LogAssert (!"Connect Socket Succeeded but RegisterHandle failed");
                delete netvc;

                delete connectCompletionEvent;
                SignalUserReInternal(cont, EventHandler::NET_EVENT_CONNECT_FAILED, 
                                     (void*)0);

                return (NULL);
            }

        BOOL r = m_FnConnectEx(sockfd, (struct sockaddr *)&remote_addr, 
                               sizeof(remote_addr), NULL, 0, NULL, 
                               connectCompletionEvent->GetOverlapped());
        if (!r && ((lerror = WSAGetLastError()) != ERROR_IO_PENDING)) {

            Log(LogID_Netlib, LogLevel_Warning, "ConnectEx Failed",
                LogTag_IP, inet_ntoa(remote_addr.sin_addr),
                LogTag_Port, remote_addr.sin_port,
                LogTag_LastError, lerror,
                LogTag_End);

            delete netvc;
            delete connectCompletionEvent;
            // schedule an io completion
            SignalUserReInternal(cont, EventHandler::NET_EVENT_CONNECT_FAILED, 
                                 IntToPtr(-lerror));
            return NULL;
        }
        return (&netvc->m_ConnectAction);
    }

    //
    //  NetProcessor::accept()
    //

    Action * 
    NetProcessor::Accept(EventHandler * cont, unsigned short port,
                         SOCKET listen_socket_in, int accept_pool_size,
                         int recv_bufsize, int send_bufsize)
    {
        return (Accept(cont, INADDR_ANY, port, listen_socket_in,
                       accept_pool_size, recv_bufsize, send_bufsize));
    }

    Action * 
    NetProcessor::Accept(EventHandler * cont, unsigned int bindIp, 
                         unsigned short port, SOCKET listen_socket_in, 
                         int accept_pool_size, int recv_bufsize, 
                         int send_bufsize)
    {
        int i;
        int lerror;
        SOCKET listen_socket;

        //Netlib::m_Counters.g_ctrNumAccepts->Increment();

        NetAcceptAction * net_accept_action = new NetAcceptAction(this);
        net_accept_action->Startup(cont, accept_pool_size);
        struct sockaddr_in & serv_addr = net_accept_action->m_ServAddr;

        if (listen_socket_in != INVALID_SOCKET) {
            listen_socket = listen_socket_in;
            int addr_len = sizeof(serv_addr);
            lerror = getsockname(listen_socket, 
                                 (struct sockaddr *)&serv_addr, 
                                 &addr_len);

            if (lerror == SOCKET_ERROR) {
                m_Lerror = WSAGetLastError();
                Log(LogID_Netlib, LogLevel_Error, "getsockname Failed",
                    LogTag_Port, port,
                    LogTag_LastError, m_Lerror,
                    LogTag_End);
                return (NULL);
            }
        }
        else {
            ///////////////////////////////////////////
            // create and initialize a listen socket //
            ///////////////////////////////////////////
            serv_addr.sin_family      = AF_INET;
            serv_addr.sin_addr.s_addr = bindIp;
            serv_addr.sin_port = htons(port);

            listen_socket = CreateStreamSocketInternal(&serv_addr);
            if( listen_socket == INVALID_SOCKET) {
                lerror = WSAGetLastError();
                Log(LogID_Netlib, LogLevel_Error, "Create Listen Socket Failed",
                    LogTag_Port, port,
                    LogTag_LastError, lerror,
                    LogTag_End);
                return (NULL);
            }
            /////////////////
            // listen ...  //
            /////////////////
            if ((listen(listen_socket, LISTEN_QUEUE_SIZE)) != 0) {
                lerror = WSAGetLastError();
                Log(LogID_Netlib, LogLevel_Error, "Listen Failed",
                    LogTag_Port, port,
                    LogTag_LastError, lerror,
                    LogTag_End);
                return (NULL);
            }
    
            ///////////////////////////////////////
            // associate the listen socket with  //
            // a completion port.                //
            ///////////////////////////////////////
            if (RegisterHandle((HANDLE)listen_socket, IO_COMPLETION_KEY_NONE) 
                == NULL) {
                Log(LogID_Netlib, LogLevel_Error,
                    "Associating Listen Socket With Completion Port Failed",
                    LogTag_Port, port,
                    LogTag_End);
                return (NULL);
            }
        }
  
        net_accept_action->SetListenSocket(listen_socket);

        /////////////////////////////////////////////////
        // setup pool. for each instance of NetAccept//
        // in the pool create a NetVConnection and     //
        // a completion port.                          //
        /////////////////////////////////////////////////
        for (i = 0; i < accept_pool_size; i++) {
            if (!InitNetAcceptInternal(net_accept_action, 
                                       &((*net_accept_action)[i]), 
                                       &lerror,
                                       recv_bufsize, 
                                       send_bufsize)) {
                break;
            }
        }
        return (net_accept_action);
    }

    //
    //  NetProcessor::init_net_accept_internal()
    //
    bool 
    NetProcessor::InitNetAcceptInternal(NetAcceptAction * net_accept_action,
                                        NetAccept * net_accept,
                                        int * lerror,
                                        int recv_bufsize, int send_bufsize)
    {
        SOCKET accept_socket;
        SOCKET listen_socket;
        DWORD bytes_received;
        CompletionEvent * completion_event;
        NetVConnection  * netvc;
        BOOL r;
        OVERLAPPED * overlapped = 0;

        *lerror = 0;
        //////////////////////////////////////////////////
        // For this instance of NetAccept in the pool //
        // create an NetVConnection and a completion    //
        // port, and call AcceptEx().                   //
        //////////////////////////////////////////////////
        /////////////////////
        // create socket.  //
        /////////////////////
        if ((accept_socket = 
             CreateStreamSocketInternal(NULL, recv_bufsize, 
                                        send_bufsize)) == INVALID_SOCKET) {
            *lerror = WSAGetLastError();
            Log(LogID_Netlib, LogLevel_Error, "Create Accept Socket Failed",
                LogTag_LastError, *lerror,
                LogTag_End);
            return false;
        }
        ///////////////////////////////////////////////////
        // associate this socket with a completion port. //
        ///////////////////////////////////////////////////
        if ((RegisterHandle((HANDLE)accept_socket, IO_COMPLETION_KEY_NONE)) 
            == NULL) {
            *lerror = WSAGetLastError();
            Log(LogID_Netlib, LogLevel_Error, 
                "Associating Accept Socket With Completion Port Failed",
                LogTag_LastError, *lerror,
                LogTag_End);
            return false;
        }
        ////////////////////////////////
        // setup a new NetVConnection //
        ////////////////////////////////
        netvc = NewNetVConnection();

        netvc->m_S = accept_socket;
        netvc->m_NetAcceptAction = net_accept_action;

        /////////////////////////////////////////////
        // setup this NetAccept data structure.  //
        /////////////////////////////////////////////

        // make sure no other threads to use this NetAccept with the new mutex
        // until we are done
        MUTEX_LOCK(lock, netvc->GetMutex());
      
        net_accept->Setup(netvc);
        ///////////////////////////////
        // allocate completion event //
        ///////////////////////////////
        completion_event = 
            new CompletionEvent(net_accept, 
                                EventHandler::NET_EVENT_ACCEPT_INTERNAL);
        net_accept->SetCompletionEvent(completion_event);

        listen_socket = net_accept_action->GetListenSocket();
        ///////////////////////////////////
        // call AcceptEx for this socket //
        ///////////////////////////////////
        overlapped = completion_event->GetOverlapped();

        r = m_FnAcceptEx(listen_socket, accept_socket,
                         net_accept->GetAcceptBuffer(),
                         0,
                         net_accept->GetLocalAddressLength(),
                         net_accept->GetRemoteAddressLength(),
                         &bytes_received,
                         overlapped
                         );

        if (!r && ((*lerror = WSAGetLastError()) != ERROR_IO_PENDING)) {
            Log(LogID_Netlib, LogLevel_Error, "Accept Failed",
                LogTag_LastError, *lerror,
                LogTag_End);
            return false;
        }
        //////////////////////////////////////////////
        // ERROR_IO_PENDING mean that the operation //
        // did not complete synchronously, which is //
        // what expected. (this is not an error).   //
        //////////////////////////////////////////////
        *lerror = 0;
        return true;
    }

    //
    //  NetProcessor::signal_user_re_internal()
    //
    bool 
    NetProcessor::SignalUserReInternal(EventHandler * c, 
                                       EventHandler::Event event, void * data)
    {
        MUTEX_TRY_LOCK(lock, c->m_Mutex);
        LogAssert (lock);
        c->HandleEvent(event, data);

        return (true);
    }

    //
    //  NetProcessor::create_stream_socket_internal()
    //
    //  create a socket for TCP.
    SOCKET 
    NetProcessor::CreateStreamSocketInternal(struct sockaddr_in *addr, 
                                             int recv_bufsize, int send_bufsize)
    {

        SOCKET sockfd = INVALID_SOCKET;
        ///////////////////////////////
        // create network socket and //
        // set socket options.       //
        ///////////////////////////////
        if ((sockfd = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                                0,0,WSA_FLAG_OVERLAPPED)) == INVALID_SOCKET) {
            int lerror = WSAGetLastError();
            Log(LogID_Netlib, LogLevel_Error, "WSASocket failed",
                LogTag_LastError, lerror,
                LogTag_End);
            return (INVALID_SOCKET);
        }
        if (recv_bufsize) {
            if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, 
                           (char *) &recv_bufsize, sizeof(recv_bufsize)) < 0) {
                int lerror = WSAGetLastError();
                Log(LogID_Netlib, LogLevel_Warning, "setsockopt of SO_RCVBUF Failed",
                    LogTag_LastError, lerror,
                    LogTag_End);
            }
        }
        if (send_bufsize) {
            if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, 
                           (char *) &send_bufsize, sizeof(send_bufsize)) < 0) {
                int lerror = WSAGetLastError();
                Log(LogID_Netlib, LogLevel_Warning, "setsockopt of SO_SENDBUF Failed",
                    LogTag_LastError, lerror,
                    LogTag_End);
            }
        }
        int noDelay=1;
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, 
                       (char *)&noDelay, sizeof(int)) != 0) {
    
            int lerror = WSAGetLastError();
            Log(LogID_Netlib, LogLevel_Warning,
                "setsockopt of IPPROTO_TCP/TCP_NODELAY Failed",
                LogTag_LastError, lerror,
                LogTag_End);
            // No reason to die!!!!
        }
  
  
        int bOpt = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_DONTLINGER,
                       (char *)&bOpt, sizeof(int) ) != 0) {
            int lerror = WSAGetLastError();
            Log(LogID_Netlib, LogLevel_Warning,
                "setsockopt of SO_DONTLINGER Failed",
                LogTag_LastError, lerror,
                LogTag_End);
        }
        if (addr != NULL) {

            // If the bind fails below, retry for sometime before giving up

            const UInt32 totalWaitTime = 5 * 60000;       // Wait for 5 minutes
            const UInt32 delay = 5000;                    // Wait 5s before retrying
            const UInt32 nRetries = totalWaitTime / delay;
            bool boundSuccessfully = false;

            for (UInt32 i = 0; i < nRetries; ++i)
                {
                    if ((bind(sockfd, (struct sockaddr *)addr, sizeof(*addr))) != 0)
                        {
                            int lerror = WSAGetLastError();
                            const char* tmpAddr = inet_ntoa(addr->sin_addr);
                            if (!tmpAddr)
                                {
                                    tmpAddr = "?.?.?.?";
                                }
                            Log(LogID_Netlib, LogLevel_Error,
                                "Bind Failed. Retrying...",
                                LogTag_IP, tmpAddr,
                                LogTag_Port, ntohs(addr->sin_port),
                                LogTag_LastError, lerror,
                                LogTag_End);
                            Sleep(delay);
                        }
                    else
                        {
                            boundSuccessfully = true;
                            break;
                        }
                }

            if (!boundSuccessfully)
                {
                    const char* tmpAddr = inet_ntoa(addr->sin_addr);
                    if (!tmpAddr)
                        {
                            tmpAddr = "?.?.?.?";
                        }
                    Log(LogID_Netlib, LogLevel_Error,
                        "Bind Failed. Failing",
                        LogTag_IP, tmpAddr,
                        LogTag_Port, ntohs(addr->sin_port),
                        LogTag_End);
                    closesocket(sockfd);
                    return (INVALID_SOCKET);
                }
        }
  
        return (sockfd);
    }

} // namespace RSLibImpl
