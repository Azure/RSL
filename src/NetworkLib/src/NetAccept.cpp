#include "NetAccept.h"
#include "NetProcessor.h"
#include "NetVConnection.h"

#include "stdafx.h"

namespace RSLibImpl
{

NetAccept::NetAccept() :
  EventHandler(0),
  m_NetAcceptAction(0),
  m_Netvc(0),
  m_CompletionEvent(0)
{
  return;
}

NetAccept::~NetAccept()
{ 
  CleanUp();
}

void 
NetAccept::Setup(NetVConnection * netvc)
{
  MUTEX_LOCK(lock, m_NetAcceptAction->GetPoolMutex());    
  EventHandler::m_Mutex = netvc->m_Mutex;
  m_Netvc = netvc;
  SetHandler ((Handler) &NetAccept::HandleEvent);

  return;
}

void
NetAccept::CleanUp()
{
  if (m_CompletionEvent) 
  {
    m_CompletionEvent->Cancel();
    m_CompletionEvent = NULL;
  }
  
  if (m_Netvc) {
    ::closesocket(m_Netvc->m_S);
    m_Netvc->m_S = INVALID_SOCKET;
    delete m_Netvc;
    m_Netvc = NULL;
  }
  EventHandler::m_Mutex = NULL;
  m_NetAcceptAction = NULL;
  return;
}
//
//  NetAccept::setup()
//
//  This function is called on an AcceptEx completion
//  event. 
//  On a new accept this function will:
//  - call the instance of NetVConnection for this
//    connection.
//  - clear this instance of NetAccept, and reinitialize
//    it to accept a new connection.
//  - preserve original accept() configuration, e.g. 
//    'accept_only' no initial read behavior.
int 
NetAccept::HandleEvent(Event event, void * data)
{
  int lerror;
  CompletionEvent * completion_event = (CompletionEvent *)data;
  LogAssert (event == NET_EVENT_ACCEPT_INTERNAL);
  LogAssert (completion_event == m_CompletionEvent);
  LogAssert (m_Mutex == m_Netvc->m_Mutex);
  /////////////////////////////////////////////////////////
  // this instance of NetVConnection is still idle,      //
  // and no one other than this instance of NetAccept  //
  // should have a reference to it. Threre for the lock  //
  // should always be acquired.                          //
  /////////////////////////////////////////////////////////

  if (m_NetAcceptAction->m_Cancelled) {
    m_CompletionEvent = NULL;
    return (0);
  }

  m_Netvc->HandleEvent(NETVC_ACCEPT_INTERNAL, completion_event);

  /////////////////////////////////
  // clear and re-init AcceptEx. //
  /////////////////////////////////
  NetProcessor *net = m_NetAcceptAction->GetNetProcessor();
  net->InitNetAcceptInternal(m_NetAcceptAction, this, &lerror);
  LogAssert (lerror == 0);

  return (0);
}

//
//  NetAcceptAction::NetAcceptAction()
//
NetAcceptAction::NetAcceptAction(NetProcessor *np)
:
Action(),
RefCount(),
m_NetAcceptPoolSize(0),
m_NetAcceptPool(0),
m_ListenSocket(0)
{
  memset(&m_ServAddr, '\0', sizeof(m_ServAddr));
  m_NetProcessor = np;
  m_NetAcceptPoolMutex = new MSMutex();
  return;
}

//
//  NetAcceptAction::NetAcceptAction()
//
NetAcceptAction::~NetAcceptAction()
{
  delete [] m_NetAcceptPool;
  Action::m_Mutex = NULL;
  Debug("net_accept_cancel", "Deleting NetAcceptAction");
  return;
}

//
//  NetAcceptAction::startup()
//
void 
NetAcceptAction::Startup(EventHandler * c, int net_accept_pool_size)
{
  Action::operator = (c);
  m_NetAcceptPoolSize = net_accept_pool_size;
  m_NetAcceptPool = new NetAccept[net_accept_pool_size];
  for (int i = 0; i < m_NetAcceptPoolSize; i++) {
    m_NetAcceptPool[i].m_NetAcceptAction = this;
  }
  return;
}

//
//  NetAcceptAction::cancel()
//
void NetAcceptAction::Cancel()
{
  Debug("net_accept_cancel", "NetAcceptAction::cancel");
  {
    MUTEX_LOCK(lock, m_Mutex);
    Action::Cancel();
  }

  // hold on myself to prevent being deleted
  Ptr<NetAcceptAction> holdme = this;

  for (int i = 0; i < m_NetAcceptPoolSize; i++) {
    NetAccept* na = &m_NetAcceptPool[i];

    while(true)
    {
        Ptr<MSMutex> mutex = NULL;
        {
            MUTEX_LOCK(lock, GetPoolMutex());
            mutex = na->GetMutex();
        }

        MUTEX_LOCK(lock2, mutex);

        Ptr<MSMutex> mutex2 = NULL;
        {
            MUTEX_LOCK(lock, GetPoolMutex());
            mutex2 = na->GetMutex();
        }

        if ((MSMutex*)mutex == (MSMutex*)mutex2)
        {
            // nobody has changed the lock - we are safe
            na->CleanUp();
            break;
        }
    }    
  }

  //close the listen port
  if (m_ListenSocket != INVALID_SOCKET)
  {
    closesocket(m_ListenSocket);
    m_ListenSocket = INVALID_SOCKET;
  }

  return;
}

} // namespace RSLibImpl
