#pragma once

#include "basic_types.h"
#include "logging.h"
#include "Ptr.h"

namespace RSLibImpl
{

//////////////////////////////////////////////////////////////////////////////
//
//    The EventHandler Class
//
// EventHandlers have a handleEvent() method to invoke them.
// Users can determine the behavior of a EventHandler by
// setting the "Handler" (member function name)
// which is invoked when events arrive.  This function can be changed
// with the "setHandler" method.
//
// EventHandlers can be subclassed to add additional state and methods.
//
//////////////////////////////////////////////////////////////////////////////


class EventHandler {
public:

  static const UInt32 EVENTS_START              =  100;
  static const UInt32 VC_EVENTS_START           =  200;
  static const UInt32 NET_EVENTS_START          =  300;
  static const UInt32 NETVC_EVENTS_START        =  400;
  static const UInt32 PACKET_EVENTS_START       =  500;
  
  enum Event {
    EVENT_IMMEDIATE           =  EVENTS_START,
    EVENT_INTERVAL,
    EVENT_TIMEOUT,

    VC_EVENT_READ_READY       =  VC_EVENTS_START,
    VC_EVENT_READ_COMPLETE,
    VC_EVENT_WRITE_READY,
    VC_EVENT_WRITE_COMPLETE,
    VC_EVENT_EOS,
    VC_EVENT_ERROR,
    
    NET_EVENT_CONNECT         =  NET_EVENTS_START,
    NET_EVENT_CONNECT_FAILED,
    NET_EVENT_ACCEPT,
    NET_EVENT_ACCEPT_FAILED,
    NET_EVENT_ACCEPT_INTERNAL,
    NET_EVENT_ACCEPT_CANCEL_INTERNAL,
    NET_EVENT_CONNECT_INTERNAL,

    NETVC_CONNECT_INTERNAL     =  NETVC_EVENTS_START,
    NETVC_ACCEPT_INTERNAL,
    NETVC_ASYNC_READ_COMPLETE,
    NETVC_ASYNC_WRITE_COMPLETE,
    NETVC_RETRY_DO_IO_READ,
    NETVC_RETRY_DO_IO_WRITE, 
    NETVC_RETRY_VIO_LOCK_READ,
    NETVC_RETRY_VIO_LOCK_WRITE,
    NETVC_RETRY_DO_IO_CLOSE ,
    NETVC_RETRY_SIGNAL_ACCEPT,

    PACKET_REENABLE_RECEIVE     = PACKET_EVENTS_START,
    PACKET_CONNECT_FAILED
  };

  typedef int (EventHandler::*Handler) (Event event, void * data);

  EventHandler(MSMutex * amutex = NULL) :
    m_Handler(NULL),
    m_Mutex(amutex)
    {}

  virtual ~EventHandler() {}
  
  int HandleEvent(Event event, void * data) {
    return (this->*m_Handler)(event,data);
  }

  MSMutex * GetMutex() {
    return (m_Mutex);
  }
  void SetHandler(Handler handler) {
    m_Handler = handler;
  }

  Handler              m_Handler;        // member function to invoke
  Ptr<MSMutex>         m_Mutex;

};

} // namespace RSLibImpl
