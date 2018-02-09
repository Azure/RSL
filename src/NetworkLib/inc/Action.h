#pragma once

#include "EventHandler.h"
#include "Ptr.h"

/*
  Actions should be returned from netprocessor as a receipt for
  any asynchronous action.

  Netprocessor which was called with a EventHandler, creates
  an Action and stores the EventHandler its supposed to call
  back and the mutex for that continuation in the Action 
  structure. When the processor is ready to call back it:
  1. acquire the mutex
  2. test the cancelled flag
  3. if cancelled is false call the EventHandler,
     else cancel the operation.
*/

namespace RSLibImpl
{

class Action 
{
public:
  virtual void Cancel()    
  { 
    if (!m_Cancelled) m_Cancelled = true; 
  }

  EventHandler *   m_Handler;              // EventHandler
  Ptr<MSMutex>     m_Mutex;                // EventHandler's mutex
  volatile int     m_Cancelled;
  int              m_Lerror;               // errno

  EventHandler * operator = (EventHandler * ahandler) {
    m_Handler = ahandler;
    if (ahandler)
      m_Mutex = ahandler->m_Mutex;
    else
      m_Mutex = 0;
    return ahandler;
  }

  Action() : m_Handler(NULL), m_Cancelled(false), m_Lerror(0) {}
  virtual ~Action() {}
};

} // namespace RSLibImpl

