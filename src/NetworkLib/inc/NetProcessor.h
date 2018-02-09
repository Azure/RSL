#pragma once

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>

#include "basic_types.h"
#include "logging.h"
#include "Action.h"
#include "Ptr.h"
#include "NetAccept.h"
#include "CompletionEvent.h"
#include "EventQueue.h"
#include "NetCommon.h"

namespace RSLibImpl
{

class NetVConnection;

const int MUTEX_RETRY_DELAY = HRTIME_MSECONDS(10); // 10 milliseconds

class NetProcessor
{
public:

  static const int ACCEPTEX_POOL_SIZE;
  static const int LISTEN_QUEUE_SIZE;
  static const HiResTime MAX_EVENT_DELAY;

  static const HiResTime EVENT_RETRY_TIME;  

  static const HiResTime MAX_MAINLOOP_DELAY;
  static const HiResTime MAX_CALLBACK_DELAY;

  struct ThreadCtx {
    Queue<CompletionEvent> m_Queue;
    PriorityEventQueue* m_EventQueue;
    MSMutex m_QueueMutex;
    HANDLE m_Handle;
    DWORD m_Id;
    NetProcessor *m_Processor;
    HiResTime m_Now;

  };

  NetProcessor();
    virtual ~NetProcessor();
    ///////////////////////////////////////
    // connection oriented I/O interface
    // NOTE: Port is in host byte order.
    ///////////////////////////////////////
    Action * Connect(
        EventHandler * cont, 
        struct in_addr addr,
        unsigned short port,
      int           recv_bufsize = 0,
    int           send_bufsize = 0);

    // NOTE: Port is in host byte order, but IP 
    // is in network byte order. Will  bind to all IPs
    Action * Connect(
        EventHandler * cont, 
        unsigned int   ip,
        unsigned short port,
    int           recv_bufsize = 0,
    int           send_bufsize = 0);

    Action* Connect(
        EventHandler * cont,
        unsigned int   ip,
        unsigned short port,
        unsigned int   bindIp, // in network byte order
        int            recv_bufsize = 0,
        int            send_bufsize = 0);


    Action * Accept(
        EventHandler * cont,
        unsigned int   bindIp, // in network byte order
        unsigned short port,
        SOCKET         listen_socket_in = INVALID_SOCKET,
    int            accept_pool_size = ACCEPTEX_POOL_SIZE,
    int           recv_bufsize = 0,
    int           send_bufsize = 0
        );

        Action * Accept(
        EventHandler * cont,
        unsigned short port,
        SOCKET         listen_socket_in = INVALID_SOCKET,
    int            accept_pool_size = ACCEPTEX_POOL_SIZE,
    int           recv_bufsize = 0,
    int           send_bufsize = 0
        );

    bool SignalUserReInternal(
        EventHandler * c, 
        EventHandler::Event          event, 
        void         * data         );

    bool InitNetAcceptInternal(
        NetAcceptAction * net_accept_action,
        NetAccept       * net_accept, 
        int             * lerror,
    int               recv_bufsize = 0,
    int               send_bufsize = 0);

    NetVConnection* NewNetVConnection();
    void MainLoop(ThreadCtx *thread);
    void CallbackEvent(CompletionEvent *si_e);
    void ProcessTimedEvents(ThreadCtx *thread);

  SOCKET CreateStreamSocketInternal(struct sockaddr_in *addr, 
                     int recv_bufsize = 0,
                     int snd_bufsize = 0);

    bool Start(int n_threads);

    // Terminates all the netlib threads.
    void Stop();
    /////////////////////////////////////////////////////////////
  // register_handle() registers a HANDLE with a completion    //
  // port. The handle is associated with the completion port   //
  // used to recieve completion notification.                  //
  // The EventHandler to call back on a completion event is    //
  // passed through struct Overlapped which should be          //
  // passed to asynchronous I/O calls.                         //
  ///////////////////////////////////////////////////////////////
  HANDLE RegisterHandle(
    HANDLE h, 
    DWORD  completion_key = IO_COMPLETION_KEY_NONE);
  ///////////////////////////////////////////////////////
  // get a completion port. this function is used by   //
  // asynchronous callers that wish to be notified on  //
  // completion through a completion port event.       //
  // The code that executes the asynchronous request   //
  // post a completion message to the competion port.  //
  ///////////////////////////////////////////////////////
  HANDLE GetCompletionPort();
  ////////////////////////////////////////////////////
  // send a completion event to the completion port //
  ////////////////////////////////////////////////////
  void   ScheduleCompletion(
      CompletionEvent *e,
      HiResTime        timeout
      );

  bool IsNetlibThread();
  
  static LPFN_ACCEPTEX m_FnAcceptEx;
  static LPFN_CONNECTEX m_FnConnectEx;    
  static LPFN_GETACCEPTEXSOCKADDRS m_FnGetAcceptExSockaddrs;
private:

    //////////////////////
    // declaration only //
    //////////////////////
    NetProcessor(const NetProcessor &);
    NetProcessor & operator = (const NetProcessor &);

    int m_NThreads;
    ThreadCtx *m_Threads;
    HANDLE m_Cport;
    DWORD m_Lerror;
    int m_NextThread;

    volatile bool m_stopped;
    
    // function pointers for acceptex, connectex and GetAcceptExSockAddrs
friend class NetAccept;
};

} // namespace RSLibImpl
