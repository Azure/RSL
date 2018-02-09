#pragma once

#include "Action.h"
#include "List.h"
#include "HiResTime.h"

namespace RSLibImpl
{

class NetProcessor;
class CompletionEvent;

const int IO_COMPLETION_KEY_NONE     = 0;
const int IO_COMPLETION_KEY_SHUTDOWN = 1;

//////////////////////////////////////////////////////////////////
//
//  struct Overlapped
//
//  This structure is an extension to OVERLAPPED structure.
//  (WSAOVERLAPPED belongs to WinSock2 and is compatible with
//  the Win32 OVERLAPPED structure.
//  The extension has a reference to Event, which is
//  the handler for completion events. IOCPortOverlapped is
//  casted to OVERLAPPED and passed to asynchronous io operations.
//  OVERLAPPED ol must be the first member alwayes.
//  Event is the context data, which used as a reference
//  when the asynchronous completes.
//////////////////////////////////////////////////////////////////

struct Overlapped : public OVERLAPPED
{
    CompletionEvent * m_Event;
    Overlapped() : m_Event(0) {}
};

typedef Overlapped *POverlapped;

//////////////////////////////////////////////////////////////////
//
//  class CompletionEvent
//
//////////////////////////////////////////////////////////////////
class CompletionEvent : public Action
{
public:

    CompletionEvent(EventHandler *c, EventHandler::Event key);

    virtual ~CompletionEvent();

    /////////////////////////////////////////
    // base class Action virtual interface //
    /////////////////////////////////////////
    virtual void Cancel();

    ///////////////////////////////////
    // return values from completion //
    // event.                        //
    ///////////////////////////////////
    int GetBytesTransferred();
    int GetLerror();
    int GetCallbackEvent();

    ////////////////////////////////////////////
    // set callback event and completion data //
    ////////////////////////////////////////////
    void SetCallbackEvent(EventHandler::Event callbackEvent);
    void SetBytesTransferred(int bytesTransferred);
    void SetCompletionKey(int completionKey);
    void SetLerror(int lerror);

    ///////////////////////////////////////
    // use to pass to asynchronous calls //
    ///////////////////////////////////////
    Overlapped * GetOverlapped();

    /////////////////////////////////////////////////////
    // overlapped is passed by the code that schedule  //
    // the asynchronous I/O to an asynchronous call    //
    // such as ReadFile(). _overlapped has a reference //
    // to the instance of this class.                  //
    /////////////////////////////////////////////////////
    Overlapped  m_Overlapped;     /* for asynch io   */
    EventHandler::Event  m_CallbackEvent;
    HiResTime m_Timeout;
    int  m_BytesTransferred;
    int  m_CompletionKey;
    Link<Overlapped> link;
    int m_InBucket;
    bool m_Scheduled;

private:
    //////////////////////
    // declaration only //
    //////////////////////
    CompletionEvent(const CompletionEvent &);
    CompletionEvent & operator = (const CompletionEvent &);

    friend class NetProcessor;

};
//////////////////////////////////////////////////////////////////
//
//  class CompletionEvent - inline functions definitions
//
//////////////////////////////////////////////////////////////////

inline CompletionEvent::CompletionEvent(EventHandler * c, 
                                        EventHandler::Event event) :
  Action(),
     m_Timeout(0),
     m_BytesTransferred(0),
     m_CompletionKey(IO_COMPLETION_KEY_NONE),
     m_InBucket(-1),
     m_Scheduled(false)
{
  Action::operator = (c);
  m_CallbackEvent = event;
  memset(&m_Overlapped, '\0', sizeof(m_Overlapped));
  m_Overlapped.m_Event = this;
  
}

inline CompletionEvent::~CompletionEvent()
{
    Action::m_Mutex = 0;
    return;
}

inline int CompletionEvent::GetBytesTransferred()
{
    return (m_BytesTransferred);
}

inline int CompletionEvent::GetLerror()
{
    return (Action::m_Lerror);
}

inline int CompletionEvent::GetCallbackEvent()
{
    return (m_CallbackEvent);
}

inline Overlapped * CompletionEvent::GetOverlapped()
{
    return (&m_Overlapped);
}

inline void CompletionEvent::SetCallbackEvent(
     EventHandler::Event acallbackEvent)
{
    m_CallbackEvent = acallbackEvent;
    return;
}

inline void CompletionEvent::SetBytesTransferred(int bytesTransferred)
{
    m_BytesTransferred = bytesTransferred;
    return;
}

inline void CompletionEvent::SetCompletionKey(int completionKey)
{
    m_CompletionKey = completionKey;
    return;
}

inline void CompletionEvent::SetLerror(int lerror)
{
    Action::m_Lerror = lerror;
    return;
}

inline void CompletionEvent::Cancel()
{
    Action::Cancel();
    return;
}

} // namespace RSLibImpl
