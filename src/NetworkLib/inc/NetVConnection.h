#pragma once

#include "Action.h"
#include "VIO.h"
#include "CompletionEvent.h"
#include "Ptr.h"
#include "List.h"

namespace RSLibImpl
{

    class NetVConnection : public EventHandler {

    private:
  
        //////////////////////////////////////////////////////////////////
        //
        //  class IOState
        //
        //////////////////////////////////////////////////////////////////
        struct IOState 
        {
        IOState() :
            m_Vio(),
                m_Cancelled(false),
                m_CompletionEvent(0),
                m_retryVioEvent(0)
            {}
      
            ~IOState() {
                m_Vio.m_Buffer = NULL;
                m_Vio.m_Mutex = 0;
      
                LogAssert (m_CompletionEvent == 0);
                LogAssert (m_retryVioEvent == 0);
                return;
            }

            //////////////////////////////////////////////
            // VIO: a descriptor for this I/O.          //
            //////////////////////////////////////////////
            VIO    m_Vio;
            //////////////////////////////////////////////
            // cancel is set if this VIO was cancelled  //
            // by the user. _cancelled and _vio._cont   //
            // are protected by the continuation's lock.//
            //////////////////////////////////////////////
            bool   m_Cancelled;
            //////////////////////////////////////////
            // overlapped is passed to asynchronous //
            // ReadFile and WriteFile.              //
            //////////////////////////////////////////
            CompletionEvent * m_CompletionEvent;
            CompletionEvent *m_retryVioEvent;
        };

    public:
  
        NetVConnection();
        virtual ~NetVConnection();

        virtual VIO * IORead(
                             EventHandler   * c, 
                             INT64          nbytes,
                             NetBuffer       * buf
                             );
        virtual VIO *  IOWrite(
                               EventHandler   * c,
                               INT64          nbytes,
                               NetBuffer       * buf
                               );

        virtual void IOClose();
        virtual void IOShutdown(int howto);

        virtual void Reenable(VIO * vio);

        SOCKET GetSocket();

        virtual const struct sockaddr_in & GetLocalAddr();
        virtual const struct sockaddr_in & GetRemoteAddr();
        virtual unsigned int               GetLocalIp();
        virtual unsigned short             GetLocalPort();
        virtual unsigned int               GetRemoteIp();
        virtual unsigned short             GetRemotePort();

        /////////////////////
        // I/O information //
        /////////////////////
        INT64 GetReadNBytes();
        INT64 GetReadNDone();
        INT64 GetWriteNBytes();
        INT64 GetWriteNDone();
        INT64 GetReadNTodo();
        INT64 GetWriteNTodo();

    private:
        ////////////
        // states //
        ////////////
        int HandleConnectInternal(CompletionEvent *c_event);
        int HandleAcceptInternal(CompletionEvent *c_event);

        void CancelRetry(CompletionEvent **c_event);
    
        void CompleteAcceptCallback();

        int HandleEventCallback(Event event, void * data);

        //////////////
        // internal //
        //////////////

        VIO * IOInternal(
                         EventHandler   * c, 
                         NetBuffer      * buf, 
                         INT64          nbytes, 
                         VIO            * vio
                         );
        void CloseInternal();
        void ReenableInternal(VIO *vio);
        void ScheduleInternal(VIO *vio);
        void ScheduleRetry(VIO *vio);
        void HandleIOCompletion(VIO *vio);
        Event GetCallbackEvent(IOState *state, int bytes_transferred);

        ////////////////////////////////////
        // attributes of the connection   //
        ////////////////////////////////////
        SOCKET              m_S;
        struct sockaddr_in  m_RemoteAddr;
        struct sockaddr_in  m_LocalAddr;
        static long          m_Gid;
        unsigned long        m_Id;
        ////////////////////////////////////
        // attributes of current read and //
        // current write operations.      //
        ////////////////////////////////////
        IOState    m_ReadState;
        IOState    m_WriteState;
        bool       m_WaitToClose;

        ////////////////
        // last error //
        ////////////////
        int   m_Lerror;

        ///////////////////////////////////////////////
        // this Action and its EventHandler is saved //
        // until the asynchronous open is complete.  //
        // The EventHandler of _connect_action is the//
        // user's EventHandler, and it is called     //
        // back when the connect is complete.        //
        ///////////////////////////////////////////////
        Action m_ConnectAction;
        ///////////////////////////////////////////////////
        // _net_accept_action is kept for the case that  //
        // signalling the user's EventHanlder failed     //
        // to acquire the lock, and we need to retry.    //
        ///////////////////////////////////////////////////
        Ptr<NetAcceptAction> m_NetAcceptAction;

        CompletionEvent * m_RetryScheduleRead;
        CompletionEvent * m_RetryScheduleWrite;
        CompletionEvent * m_RetryClose;
        CompletionEvent * m_RetrySigAccept;
        NetProcessor    * m_NetProc;
        int               m_ReentrantCount;
        static const DWORD NET_IO_BATCH_SIZE = 1024 * 1024 * 4;

        friend class NetProcessor;
        friend class NetAccept;
    };

    //////////////////////////////////////////////////////////////////
    //
    //  class NetVConnection - inline functions definitions
    //
    //////////////////////////////////////////////////////////////////

    inline SOCKET NetVConnection::GetSocket()
    {
        return (m_S);
    }

    inline const struct sockaddr_in & NetVConnection::GetLocalAddr()
    {
        return (m_LocalAddr);
    }

    inline const struct sockaddr_in & NetVConnection::GetRemoteAddr()
    {
        return (m_RemoteAddr);
    }

    inline unsigned int NetVConnection::GetLocalIp()
    {
        return (m_LocalAddr.sin_addr.s_addr);
    }

    inline unsigned short NetVConnection::GetLocalPort()
    {
        return ntohs((m_LocalAddr.sin_port));
    }

    inline unsigned int NetVConnection::GetRemoteIp()
    {
        return (m_RemoteAddr.sin_addr.s_addr);
    }

    inline unsigned short NetVConnection::GetRemotePort()
    {
        return ntohs((m_RemoteAddr.sin_port));
    }

    inline INT64 NetVConnection::GetReadNBytes()
    {
        return (m_ReadState.m_Vio.GetNBytes());
    }

    inline INT64 NetVConnection::GetReadNDone()
    {
        return (m_ReadState.m_Vio.GetNDone());
    }

    inline INT64 NetVConnection::GetWriteNBytes()
    {
        return (m_WriteState.m_Vio.GetNBytes());
    }

    inline INT64 NetVConnection::GetWriteNDone()
    {
        return (m_WriteState.m_Vio.GetNDone());
    }

    inline INT64 NetVConnection::GetReadNTodo()
    {
        return (m_ReadState.m_Vio.GetNTodo());
    }

    inline INT64 NetVConnection::GetWriteNTodo()
    {
        return (m_WriteState.m_Vio.GetNTodo());
    }

    inline void NetVConnection::CancelRetry(CompletionEvent ** c_event) {
        if (*c_event) (*c_event)->Cancel();
        *c_event = NULL;
    }

} // namespace RSLibImpl
