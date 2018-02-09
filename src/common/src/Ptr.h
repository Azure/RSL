#pragma once

#include "RefCount.h"

namespace RSLibImpl
{

class MSMutex : public RefCount {
public:
  CRITICAL_SECTION m_Section;

  MSMutex() {
    InitializeCriticalSection(&m_Section);
  }
  ~MSMutex() {
    DeleteCriticalSection(&m_Section);
  }

  void Acquire() {
    EnterCriticalSection(&m_Section);
  }

  void Release() {
    LeaveCriticalSection(&m_Section);
  }
  
  BOOL TryAcquire() {
    return TryEnterCriticalSection(&m_Section);
  }
  
};

struct MutexLock {
  Ptr<MSMutex>        m_Lock;
  MutexLock(MSMutex *am) : m_Lock(am) {
    if (m_Lock)
      m_Lock->Acquire();
  }
  ~MutexLock() {
    Release();
  }
  void Release() {
    if (m_Lock) {
      m_Lock->Release();
      m_Lock = NULL;
    }
  }
};
    
    
  
struct MutexTryLock {
  Ptr<MSMutex>        m_Lock;
  MutexTryLock(MSMutex * am) {
    BOOL lockAcquired = am->TryAcquire(); 
    if (lockAcquired)
      m_Lock = am;
  }
  
  ~MutexTryLock() {
    Release();
  }
  void Release() { 
    if (m_Lock) {
      m_Lock->Release();
      m_Lock = NULL;
    }
  }
  bool operator!() { return (m_Lock == NULL); }
  operator BOOL () { return (m_Lock != NULL); }
};

#define MUTEX_LOCK(_l, _m) MutexLock _l(_m)
#define MUTEX_TRY_LOCK(_l,_m) MutexTryLock _l(_m)
#define MUTEX_RELEASE(_l) (_l).Release()

} // namespace RSLibImpl
