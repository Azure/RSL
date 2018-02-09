//
// Read-Write Lock 
//
#pragma once

#include "basic_types.h"
#include "lf.h"

namespace RSLibImpl
{

// Simple C++ wrapper over LFPool locks. 
template <BOOL _WatchThread, BOOL _AllowRecursion>
class CPoolLockT
{

    // Maximum number of syncrhonization events. LF uses 2 events per
    // thread in worst case. With 200 events, we can support 100
    // concurrent threads. We're unlinkely to have more than 100
    // threads in single process. This way, we'll guarantee that
    // attempt to acquire a lock, even under extreme memory pressure,
    // won't return ERROR_NOT_ENOUGH_MEMORY.

    static const LONG c_MaxEvents = 200;
    
public:
    
    CPoolLockT()
    {
        if (s_LFPool == NULL)
        {
            // s_LFPool not initialized yet -- create new one
            PLFPOOL pNewPool = LFPoolCreate (0, _WatchThread, _AllowRecursion);
            LogAssert(pNewPool);
            LFPoolIncreaseNumberOfEvents(pNewPool, c_MaxEvents);
            
            // if s_LFPool is still NULL, set it [atomically] to pNewPool,
            // otherwise destroy pNewPool
            if (InterlockedCompareExchangePointer ((PVOID *) &s_LFPool, pNewPool, NULL) != NULL)
            {
                // somebody else initialized s_LFPool while we were
                // creating pNewPool; drop pNewPool
                LFPoolDestroy (pNewPool);
            }
        }
        
        LFPoolObjectInit ( s_LFPool, &m_LFObject );
    }

    ~CPoolLockT()
    {
    }

    // Takes an exclusive lock. Returns ERROR_SUCCESS if successful.
    DWORD Lock( BOOL fExclusive = TRUE )
    {
        return LFPoolObjectLockAcquire( s_LFPool, 
                                        &m_LFObject,
                                        (fExclusive) ? LF_LOCK_EXCLUSIVE : LF_LOCK_SHARED,
                                        FALSE,
                                        INFINITE,
                                        FALSE );
    }
    

    // Waits for dwTimeout when trying to take an exclusive lock. Returns WAIT_TIMEOUT
    // if it can't take an exlusive lock after dwTimeout. Returns ERROR_SUCCESS if
    // successful.
    
    DWORD Lock( BOOL fExclusive, DWORD dwTimeout )
    {
        return LFPoolObjectLockAcquire ( s_LFPool, 
                                         &m_LFObject,
                                         (fExclusive) ? LF_LOCK_EXCLUSIVE : LF_LOCK_SHARED,
                                         FALSE,
                                         dwTimeout,
                                         FALSE );
    }
    
    // Releases the lock. fExclusive must be true if the lock was held exclusively
    // by the caller.
    DWORD Unlock( BOOL fExclusive = TRUE )
    {
        return LFPoolObjectLockRelease ( s_LFPool, &m_LFObject, fExclusive, FALSE );
    }


    // Same as Lock(true)
    DWORD LockExclusive( )
    {
        DWORD dwRet =  LFPoolObjectLockAcquire ( s_LFPool, 
                                                 &m_LFObject,
                                                 LF_LOCK_EXCLUSIVE,
                                                 FALSE,
                                                 INFINITE,
                                                 FALSE );
        return ( dwRet );
    }

    // Same  as Unlock(true)
    DWORD UnlockExclusive( )
    {
       return LFPoolObjectLockRelease ( s_LFPool, &m_LFObject, TRUE, FALSE );
    }

    // same as Lock(false)
    DWORD LockShared( )
    {
        return LFPoolObjectLockAcquire ( s_LFPool, 
                                         &m_LFObject,
                                         LF_LOCK_SHARED,
                                         FALSE,
                                         INFINITE,
                                         FALSE );
    }

    // same as Unlock(false)
    DWORD UnlockShared( )
    {
        return LFPoolObjectLockRelease ( s_LFPool, &m_LFObject, FALSE, FALSE );
    }

    // Converts the lock from an exclusive lock to a shared lock. The calling
    // thread must have exclusive lock before calling this function.
    DWORD Downgrade()
    {
        return LFPoolObjectLockAcquire ( s_LFPool,
                                         &m_LFObject,
                                         LF_LOCK_DOWNGRADE,
                                         FALSE,
                                         INFINITE,
                                         FALSE );
    }

    // Tries to upgrade shared lock to exclusive lock.
    // Returns ERROR_LF_UPGRADE_PENDING if another thread is already trying to
    // upgrade
    // Returns ERROR_LF_EXLOCK_PENDING if another thread is waiting for
    // exclusive lock.
    DWORD Upgrade()
    {
        return LFPoolObjectLockAcquire ( s_LFPool,
                                         &m_LFObject,
                                         LF_LOCK_UPGRADE_SAFELY,
                                         FALSE,
                                         INFINITE,
                                         FALSE );
    }

    // returns TRUE if lock is acquired for exclusive ownership.
    // pdwHoldingThreadId is the id of the thread that has exclusive access
    // (provided either _WatchThread or _AllowRecursion is TRUE)
    
    BOOL IsLockHeld( DWORD *pdwHoldingThreadId )
    {
        return LFPoolObjectIsExclusive( &m_LFObject, pdwHoldingThreadId );
    }    

    // Returns TRUE if the current thread holds an exclusive lock.
    BOOL IsLockHeldByCurrentThread( )
    {
        DWORD dwHoldingThreadId = 0;

        BOOL fLockHeld = FALSE;
        BOOL fLockHeldByCurrentThread = FALSE;
        
        fLockHeld = IsLockHeld( &dwHoldingThreadId );

        if( fLockHeld )
        {
            fLockHeldByCurrentThread = ( GetCurrentThreadId() == dwHoldingThreadId );
        }

        return ( fLockHeldByCurrentThread );
    }

private:

    LFOBJECT m_LFObject;
    static volatile PLFPOOL s_LFPool;

};


template <class CLockT, BOOL _Exclusive>
class CAutoLockT : public INoHeapInstance
{
public:
    CAutoLockT( CLockT * pLock )
    {
        m_pLock = pLock;

        pLock->Lock( _Exclusive );
    }

    ~CAutoLockT()
    {
        m_pLock->Unlock( _Exclusive );
    }

protected:
    CLockT * m_pLock;
    BOOL m_fExclusive;
};

// Non recursive Read/Write Lock. Once a thread
// acquires an exclusive lock, it can not acquire exclusive
// or shared lock again before releasing the lock.
// Unlock() methods returns ERROR_LF_UNLOCK_BOGUS if the
// thread releasing the lock is not the same as the
// thread that acquired the lock - this is useful
// for debugging
typedef CPoolLockT<TRUE, FALSE> CPoolLockNR;

// Recursive Read/Write Lock. Exclusive owner
// can recursively acquire either shared or
// exclusive lock.
// Unlock() methods returns ERROR_LF_UNLOCK_BOGUS if the
// thread releasing the lock is not the same as the
// thread that acquired the lock 
typedef CPoolLockT<TRUE, TRUE> CPoolLockR;

// Recursive Read/WriteLock to be used with CAutoLockT class.
typedef CPoolLockT<FALSE, TRUE> CPoolLock;

typedef CAutoLockT<CPoolLock, TRUE> CAutoPoolLock;
typedef CAutoLockT<CPoolLock, TRUE> CAutoPoolLockExclusive;
typedef CAutoLockT<CPoolLock, FALSE> CAutoPoolLockShared;
} // namespace RSLibImpl

