#pragma once

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>

#include "EventHandler.h"
#include "Action.h"
#include "Ptr.h"
#include "CompletionEvent.h"

#pragma prefast(push)
#pragma prefast(disable:24002, "struct sockaddr_in not ipv6 compatible")

namespace RSLibImpl
{

class NetProcessor;
class NetAccept;
class NetVConnection;

//////////////////////////////////////////////////////////////////
//
//  class NetAcceptAction
//
//////////////////////////////////////////////////////////////////
class NetAcceptAction : public Action, public RefCount
{
public:
    NetAcceptAction(NetProcessor *np);
    virtual ~NetAcceptAction();

    void Startup(EventHandler * c, int net_accept_pool_size);
    void Cleanup();

    virtual void Cancel();
    NetProcessor    *GetNetProcessor(void)
    {
        return(m_NetProcessor);
    }

    ///////////////////////////////////////////////
    // these methods are used by NetProcessor.   //
    ///////////////////////////////////////////////
    void    SetListenSocket(SOCKET s);
    SOCKET  GetListenSocket();
    NetAccept & operator [] (int i);
    MSMutex* GetPoolMutex();

private:
    struct sockaddr_in  m_ServAddr;
    int                 m_NetAcceptPoolSize;
    NetAccept *       m_NetAcceptPool;

    SOCKET              m_ListenSocket;
    NetProcessor *      m_NetProcessor;

    Ptr<MSMutex>        m_NetAcceptPoolMutex; 

private:
    NetAcceptAction(const NetAcceptAction &);
    NetAcceptAction & operator = (const NetAcceptAction &);

friend class NetProcessor;
};

//////////////////////////////////////////////////////////////////
//
//  class NetAccept
//
//////////////////////////////////////////////////////////////////
class NetAccept : public EventHandler
{
public:
    static const DWORD LOCAL_ADDR_LEN = (sizeof(struct sockaddr_in) + 16);
    static const DWORD REMOTE_ADDR_LEN = (sizeof(struct sockaddr_in) + 16);
  
    virtual ~NetAccept();

    void Setup(NetVConnection * netvc);

    void CleanUp();

    NetAccept();
    int HandleEvent(Event event, void * data);

private:
    ////////////////////////////////////////
    // these methods are used by          //
    // NetProcessor and NetVConnection. //
    ////////////////////////////////////////
    char              * GetAcceptBuffer();
    void                SetCompletionEvent(CompletionEvent * c);
    CompletionEvent * GetCompletionEvent();
    NetAcceptAction * GetNetAcceptAction();


    static DWORD GetLocalAddressLength();
    static DWORD GetRemoteAddressLength();

private:
    Ptr<NetAcceptAction>  m_NetAcceptAction;
    NetVConnection      * m_Netvc;
    CompletionEvent     * m_CompletionEvent;
    char                  m_AcceptBuf[LOCAL_ADDR_LEN+REMOTE_ADDR_LEN];
    /////////////////////////////////////
    // these parameters are constants. //
    /////////////////////////////////////

private:
    NetAccept(const NetAccept &);
    NetAccept & operator = (const NetAccept &);
    
friend class NetProcessor;
friend class NetAcceptAction;
friend class NetVConnection;
};

//////////////////////////////////////////////////////////////////
//
//  class NetAcceptAction - inline functions
//
//////////////////////////////////////////////////////////////////

inline void NetAcceptAction::SetListenSocket(SOCKET s)
{
    m_ListenSocket = s;
    return;
}

inline SOCKET NetAcceptAction::GetListenSocket()
{
    return (m_ListenSocket);
}

inline NetAccept & NetAcceptAction::operator [] (int i)
{
    return (m_NetAcceptPool[i]);
}

inline MSMutex* NetAcceptAction::GetPoolMutex()
{
    return m_NetAcceptPoolMutex;
}
//////////////////////////////////////////////////////////////////
//
//  class NetAccept - inline functions definitions
//
//////////////////////////////////////////////////////////////////

inline void NetAccept::SetCompletionEvent(CompletionEvent * c)
{
    m_CompletionEvent = c;
    return;
}

inline char * NetAccept::GetAcceptBuffer() 
{
    return (m_AcceptBuf);
}

inline CompletionEvent * NetAccept::GetCompletionEvent()
{
    return (m_CompletionEvent);
}

inline NetAcceptAction * NetAccept::GetNetAcceptAction()
{
    return (m_NetAcceptAction);
}

inline DWORD NetAccept::GetLocalAddressLength()
{
    return (LOCAL_ADDR_LEN);
}

inline DWORD NetAccept::GetRemoteAddressLength()
{
    return (REMOTE_ADDR_LEN);
}

#pragma prefast(pop)

} // namespace RSLibImpl
