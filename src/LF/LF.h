// --------------------------------------------------------------------------
//
// Lock-free, non-blocking stack, queue,and lock pool implementation
//
// Copyright (c) Microsoft Corporation 1999-2005
//
//
// --------------------------------------------------------------------------


#pragma once

#ifndef LFAPI
#define LFAPI __stdcall

#if 0
//
// do not use WIN32_LEAN_AND_MEAN by default
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif


#pragma warning (push)
#pragma warning (disable: 4255)
#pragma warning (disable: 4668)
#pragma warning (disable: 4820)

#include <windows.h>


#ifdef __cplusplus
extern "C" {
#endif


#pragma pack (push, 8)          // ensure proper data alignment and uniform layout


/* ------------------------ Global initialization ------------------------ */
/*                          ---------------------                          */

//
// LF has one static variable used to differentiate between MP and UP systems.
// All functions that create or initialize LF objects initialize this variable.
// However, if DLL is linked agains LF _statically_ and all objects it uses
// are created in another DLL(s), that static variable will not be initialized.
//
// Therefore, it is necessary to manually call LFGlobalInit() from each DLL
// that is statically linked against LF (if LFGlobalInit() was not called LF
// library will assume multiproc environment, and it will negatively affect
// performance of uniproc systems).
//

void
LFAPI
LFGlobalInit (
  void
);


/* ----------------- 64-bit InterlockedCompareAndExchange ---------------- */
/*                   ------------------------------------                  */

//
// use LF_DATA for local variables used to store "old" values
//
#pragma warning (push)
#pragma warning (disable: 4201) // nonstandard extension used : nameless struct/union
typedef union
{
  __int64 I64;
  struct
  {
    DWORD Word1;
    DWORD Word2;
  };
#if !defined (_M_IA64) && !defined (_M_AMD64) && !defined (IA64)
  struct
  {
    PVOID Ptr;
    DWORD Cnt;
  };
#endif /* 64-bit platforms: use packed pointer/counter instead */
  PVOID Next;
  struct
  {
    unsigned ShActive:   15;
    unsigned ExUpgrade:   1;
    unsigned ExActive:    1;
    unsigned ExWaiting:  15;
    unsigned ShWaiting:  15;
    unsigned SpinLock:    1;
    unsigned UserObject:  1;
    unsigned EventIndex: 15;
  };
} LF_DATA;
#pragma warning (pop)


//
// use LF_VDATA for shared variables
//
typedef __declspec(align(8)) volatile LF_DATA LF_VDATA;


//
// Atomically executes
// if (*pData == OldData)
//   *pData = NewData;
// return (*pData);
//

LF_DATA
LFAPI
LF_CAS (
  LF_VDATA *pData,
  LF_DATA   NewData,
  LF_DATA   OldData
);


//
// Support for protected pointers -- a pair <Ptr,Cnt>; Cnt is atomically incremented with every assignment to Ptr.
//
// Atomically executes
//      if (*pData == OldData)
//          *pData = (pNewPtr, OldData.uCnt + 1)
//
// Returns TRUE on success, FALSE on failure
//
BOOL
LFAPI
LF_CASPTR (
  LF_VDATA *pData,
  PVOID     pNewPtr,
  LF_DATA   OldData
);


//
// Load VDATA
//
LF_DATA
LFAPI
LF_LOAD (
    LF_VDATA *pData
);



//
// Compare a and b
//
BOOL
LFAPI
LF_SAMEPTR (
  LF_DATA a,
  LF_DATA b
);

//
// Get the pointer stored in LF_DATA
// 
PVOID
LFAPI
LF_GETPTR (
  LF_DATA   Data
);


//
// Push/pop
//
typedef struct
{
  PVOID YouReallyDontWantToKnowWhatIsInside0;
  PVOID YouReallyDontWantToKnowWhatIsInside1;
} LF_LINK;

void
LFAPI
LF_PUSH (
  LF_VDATA *pStack,
  LF_LINK  *pObjectLinkField,
  PVOID     pObject
);


PVOID
LFAPI
LF_POP (
  LF_VDATA *pStack
);



/* ----------------------------- Constants ------------------------------- */
/*                               ---------                                 */


enum
{
  LF_LOCK_SHARED        = 0,    // acquire shared lock
  LF_LOCK_EXCLUSIVE     = 1,    // acquire exclusive lock
  LF_LOCK_DOWNGRADE     = 2,    // downgrade exclusive lock to shared 
  LF_LOCK_UPGRADE_SAFELY= 3,    // upgrade shared lock to exclusive only if there are no waiting writers (fair queueing)
  LF_LOCK_UPGRADE_IGNORE= 4,    // upgrade shared lock to exclusive ignoring waiting writers (out of order)

  LF_LOCK_TRY           = 8     // do not wait or spin, try to fulfill request immediately
};


// cannot release a lock that is not acquired
#define ERROR_LF_UNLOCK_BOGUS 0xE0000001U

// cannot upgrade a lock when there is another pending upgrade
#define ERROR_LF_UPGRADE_PENDING 0xE0000002U

// cannot safely upgrade a lock when there are pending requests for exclusive lock
#define ERROR_LF_EXLOCK_PENDING 0xE0000003U

// thread tries to acquire a lock it already keeps but recursion is not allowed
#define ERROR_LF_LOCK_RECURSE 0xE0000004U



/* ------------------- User-defined memory allocator --------------------- */
/*                     -----------------------------                       */


typedef
PVOID                   // memory allocation callback
LFAPI                   // NB: it should NEVER raise an exception but return NULL on failure
LFALLOC (
  PVOID pContext,       // user-defined memory allocator context
  DWORD dwSize          // memory size (in bytes) to allocate
);

typedef
VOID                    // release memory callback
LFAPI
LFFREE (
  PVOID pContext,       // user-defined memory allocator context
  PVOID pAddress        // address of memory to release
);


/* -------------------- User-defined wait callback ----------------------- */
/*                      --------------------------                         */

typedef
DWORD                   // shall return WAIT_OBJECT_0 if wait on hWaitObject succeeded,
LFAPI                   // WAIT_TIMEOUT if wait timed out,
LFWAIT (                // respective error code otherwise
  PVOID  pWaitContext,  // user-defined context
  HANDLE hWaitObject,   // object to wait on
  DWORD  dwMilliseconds,// timeout in milliseconds
  BOOL   fAlertable     // if TRUE then allow I/O completion routines and APC calls
);


/* -------------------- Lock-free non-blocking stack --------------------- */
/*                      ----------------------------                       */

//
// Stack will use (8 * MaxUnits) extra bytes allocated from
// process heap (not more than 1 memory allocation per AllocUnits pushes).
// Allocated memory will NOT be released due to constrains of algorithm --
// it will be reused, therefore max memory usage is (8 * MaxUnits).
//

typedef struct
{
  int dummy;
} *PLFSTACK;            // anonymous data type

PLFSTACK                // create and initialize new stack
LFAPI                   // returns NULL if not enough memory
LFStackCreateEx (
  LONG  AllocUnits,     // allocate by AllocUnits at once
  LONG  MaxDepth,       // max stack depth (if 0 then no limits)
  BOOL  fWatchUsage,    // if TRUE then watch stack usage
  BOOL  fWaitForPush,   // if stack is empty then wait for push
  BOOL  fWaitForPop,    // if stack is full wait for pop
  PVOID pMemoryContext, // user-defined memory allocator context for pAllocCb and pFreeCb
  DWORD dwMemAlignment, // min. alignment guaranteed by memory allocator (must be power of 2)
  LFALLOC *pAllocCb,    // memory allocation callback function
  LFFREE  *pFreeCb      // release memory callback function
);

PLFSTACK                // create and initialize new stack using HeapAlloc/HeapFree with current process heap
LFAPI                   // returns NULL if not enough memory
LFStackCreate (
  LONG  AllocUnits,     // allocate by AllocUnits at once
  LONG  MaxDepth,       // max stack depth (if 0 then no limits)
  BOOL  fWatchUsage,    // if TRUE then watch stack usage
  BOOL  fWaitForPush,   // if stack is empty then wait for push
  BOOL  fWaitForPop     // if stack is full wait for pop
);

LONG                    // returns total amount of memory allocated by stack
LFAPI                   // returns -1 if stack was not properly initialized
LFStackTellMemory (
  PLFSTACK LFStack
);

LONG                    // returns number of stack nodes
LFAPI                   // or (-1) if stack was not properly initialized
LFStackTellNumberOfNodes (
  PLFSTACK LFStack
);

BOOL                    // allocate more stack nodes
LFAPI                   // returns TRUE if succeeded, FALSE otherwise
LFStackIncreaseNumberOfNodes (
  PLFSTACK LFStack,
  LONG     NodesToAllocate
);

LONG                    // returns stack max depth limit, (-1) if no depth limit, 0 if no enough memory
LFAPI
LFStackTellMaxDepth (
  PLFSTACK LFStack
);

BOOL                    // increase max stack depth
LFAPI                   // returns FALSE if stack was not properly initialized
LFStackIncreaseMaxDepth (
  PLFSTACK LFStack,
  LONG     Increase
);

BOOL                    // destroy the stack and release all the memory occupied
LFAPI                   // return FALSE if stack is in use
LFStackDestroy (
  PLFSTACK LFStack
);

BOOL                    // push pointer to user-defined data on top of the stack
LFAPI                   // returns FALSE if not enough memory or stack was not properly initialized
LFStackPush (
  PLFSTACK LFStack,
  PVOID    Data
);

DWORD                   // if memory usage limit reached and no available storage wait for pop
LFAPI                   // returns as of WaitForSingleObjectEx
LFStackPushWait (
  PLFSTACK LFStack,     // stack
  PVOID    Data,        // result [not modified if wait is timed out]
  BOOL     fWaitSpin,   // spin a bit waiting for push
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
);

DWORD                   // if memory usage limit reached and no available storage wait for pop
LFAPI                   // returns as of WaitForSingleObjectEx
LFStackPushWaitEx (
  PLFSTACK LFStack,     // stack
  PVOID    Data,        // result [not modified if wait is timed out]
  PVOID    pWaitContext,// user-defined wait callback function context
  LFWAIT  *pWaitCb,     // user-defined wait callback function (if NULL WaitForSingleObjectEx will be called)
  BOOL     fWaitSpin,   // spin a bit waiting for push
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
);

//
// Attention: this function is _extremely_ dangerous; the only case when it can
// be used without long-lasting consequences is when it's guaranteed that there
// may be only one queue reader. If there are two or more readers, LFStackPeek
// may return pointer to object that was dequeued and released long time ago.
//
// Use it at your own risk or, better yet, do not use it at all.
//
PVOID                   // get top of the stack without popping it
LFAPI                   // returns NULL if stack is empty or not initialized
LFStackPeek (
  PLFSTACK LFStack
);

PVOID                   // pop top of the stack
LFAPI                   // returns NULL if stack is empty or not initialized
LFStackPop (
  PLFSTACK LFStack
);

DWORD                   // wait for stack to be non-empty if necessary and pop top of the stack
LFAPI                   // returns as of WaitForSingleObjectEx
LFStackPopWait (
  PLFSTACK LFStack,     // stack
  PVOID   *pData,       // result [not modified if wait is timed out]
  BOOL     fWaitSpin,   // spin a bit waiting for push
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
);

DWORD                   // wait for stack to be non-empty if necessary and pop top of the stack
LFAPI                   // returns as of WaitForSingleObjectEx
LFStackPopWaitEx (
  PLFSTACK LFStack,     // stack
  PVOID   *pData,       // result [not modified if wait is timed out]
  PVOID    pWaitContext,// user-defined wait callback function context
  LFWAIT  *pWaitCb,     // user-defined wait callback function (if NULL WaitForSingleObjectEx will be called)
  BOOL     fWaitSpin,   // spin a bit waiting for push
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
);


DWORD                   // returns WAIT_TIMEOUT if queue is empty,
LFAPI                   // WAIT_OBJECT_0 if it's not, or
LFStackIsEmpty (        // ERROR_INVALID_PARAMETER if LFStack was not initialized properly or destroyed
  PLFSTACK LFStack      // queue
);


/* ------------------- Lock-free non-blocking queue ---------------------- */
/*                     ----------------------------                        */

//
// Queue will use (16 * max_length_of_queue) extra bytes allocated from process heap
// (not more than 1 memory allocation per 32 pushes).
// Allocated memory will NOT be released due to constrains of algorithm --
// it will be reused, therefore max memory usage is (16 * max_length_of_queue).
//


typedef struct
{
  int dummy;
} *PLFQUEUE;    // anonymous data type


typedef struct
{
  PVOID     Data;
  DWORD_PTR Size;
} LFMSG, *PLFMSG; // messages that user may put and get


PLFQUEUE                // create new queue
LFAPI                   // returns NULL if not enough memory
LFQueueCreateEx (
  LONG  AllocUnits,     // allocate by AllocUnits at once
  LONG  MaxLength,      // max queue length (if 0 then no limits)
  BOOL  fWatchUsage,    // if TRUE then watch queue usage
  BOOL  fWaitForPut,    // if queue is empty then wait for put
  BOOL  fWaitForGet,    // if queue is full wait for get
  PVOID pMemoryContext, // user-defined memory allocator context for pAllocCb and pFreeCb
  DWORD dwMemAlignment, // min. alignment guaranteed by memory allocator (must be power of 2)
  LFALLOC *pAllocCb,    // memory allocation callback function
  LFFREE  *pFreeCb      // release memory callback function
);

PLFQUEUE                // create new queue using HeapAlloc/HeapFree with current process heap
LFAPI                   // returns NULL if not enough memory
LFQueueCreate (
  LONG  AllocUnits,     // allocate by AllocUnits at once
  LONG  MaxLength,      // max queue length (if 0 then no limits)
  BOOL  fWatchUsage,    // if TRUE then watch queue usage
  BOOL  fWaitForPut,    // if queue is empty then wait for put
  BOOL  fWaitForGet     // if queue is full wait for get
);

LONG                    // returns total amount of memory allocated by queue
LFAPI                   // returns -1 if queue was not properly initialized
LFQueueTellMemory (
  PLFQUEUE LFQueue
);

LONG                    // returns number of queue nodes allocated
LFAPI                   // or (-1) if queue was not properly initialized
LFQueueTellNumberOfNodes (
  PLFQUEUE LFQueue
);

BOOL                    // allocate more queue nodes
LFAPI                   // returns TRUE if succeeded, FALSE otherwise
LFQueueIncreaseNumberOfNodes (
  PLFQUEUE LFQueue,
  LONG     NodesToAllocate
);

LONG                    // returns max queue length, (-1) if no length limit, 0 if not enough memory
LFAPI
LFQueueTellMaxLength (
  PLFQUEUE LFQueue
);

BOOL                    // increase max queue length
LFAPI                   // returns FALSE if queue was not properly initialized
LFQueueIncreaseMaxLength (
  PLFQUEUE LFQueue,
  LONG     Increase
);

BOOL            // free memory occupied by queue
LFAPI           // return FALSE if queue is in use
LFQueueDestroy (
  PLFQUEUE LFQueue
);

BOOL            // enqueue pointer to user data
LFAPI           // returns FALSE if Msg or Msg->Data is NULL or queue wasn't properly initialized
LFQueuePut (
  PLFQUEUE LFQueue,
  LFMSG    Msg
);

DWORD                   // if memory usage limit reached and no available storage wait for get
LFAPI                   // returns as of WaitForSingleObjectEx
LFQueuePutWait (
  PLFQUEUE LFQueue,     // queue
  LFMSG    Msg,         // message to enqueue
  BOOL     fWaitSpin,   // spin before waiting for get
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
);

DWORD                   // if memory usage limit reached and no available storage wait for get
LFAPI                   // returns as of WaitForSingleObjectEx
LFQueuePutWaitEx (
  PLFQUEUE LFQueue,     // queue
  LFMSG    Msg,         // message to enqueue
  PVOID    pWaitContext,// user-defined wait callback function context
  LFWAIT  *pWaitCb,     // user-defined wait callback function (if NULL WaitForSingleObjectEx will be called)
  BOOL     fWaitSpin,   // spin before waiting for get
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
);


//
// Attention: this function is _extremely_ dangerous; the only case when it can
// be used without long-lasting consequences is when it's guaranteed that there
// may be only one queue reader. If there are two or more readers, LFQueuePeek
// may return pointer to object that was dequeued and released long time ago.
//
// Use it at your own risk or, better yet, do not use it at all.
//
DWORD                   // peeks at first enqueued user object
LFAPI                   // returns WAIT_OBJECT_0 if got the element, otherwise returns WAIT_TIMEOUT
LFQueuePeek (           
  PLFQUEUE LFQueue,     // queue
  PLFMSG   LFMsg        // user data placeholder
);


DWORD           // dequeue and return pointer to user data
LFAPI           // returns WAIT_OBJECT_0 if got the element, otherwise returns WAIT_TIMEOUT
LFQueueGet (
  PLFQUEUE LFQueue,
  PLFMSG   LFMsg
);

DWORD                   // dequeue next element and wait if necessary
LFAPI                   // returns as of WaitForSingleObjectEx
LFQueueGetWait (        // [no waiting will be performed if queue was created with fWait == FALSE]
  PLFQUEUE LFQueue,     // queue
  PLFMSG   LFMsg,       // message to retrieve
  BOOL     fWaitSpin,   // spin a bit waiting for put
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
);

DWORD                   // dequeue next element and wait if necessary
LFAPI                   // returns as of WaitForSingleObjectEx
LFQueueGetWaitEx (      // [no waiting will be performed if queue was created with fWait == FALSE]
  PLFQUEUE LFQueue,     // queue
  PLFMSG   LFMsg,       // message to retrieve
  PVOID    pWaitContext,// user-defined wait callback function context
  LFWAIT  *pWaitCb,     // user-defined wait callback function (if NULL WaitForSingleObjectEx will be called)
  BOOL     fWaitSpin,   // spin a bit waiting for put
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
);

DWORD                   // returns WAIT_TIMEOUT if queue is empty,
LFAPI                   // WAIT_OBJECT_0 if it's not, or
LFQueueIsEmpty (        // ERROR_INVALID_PARAMETER if LFQueue was not initialized properly or destroyed
  PLFQUEUE LFQueue      // queue
);


/* ---------------------- Fast exclusive locks ------------------------ */
/*                        --------------------                          */

//
// Fast exclusive locks are about 10 times faster than the standard
// implementation based on WaitForSingleObjectEx and SetEvent()
//

typedef struct
{
  int dummy;
} *PLFEXLOCK;           // anonymous data type

PLFEXLOCK               // create exclusive lock
LFAPI                   // returns NULL if failed
LFExLockCreateEx (
  BOOL  fWatchThread,   // if TRUE then release MUST be from the same thread that acquired the lock
  BOOL  fAllowRecursion,// if TRUE then allow recursive lock aquiring from the same thread
  PVOID pMemoryContext, // user-defined memory allocator context for pAllocCb and pFreeCb
  DWORD dwMemAlignment, // min. alignment guaranteed by memory allocator (must be power of 2)
  LFALLOC *pAllocCb,    // memory allocation callback function
  LFFREE  *pFreeCb      // release memory callback function
);

PLFEXLOCK               // create exclusive lock using HeapAlloc/HeapFree with current process heap
LFAPI                   // returns NULL if failed
LFExLockCreate (
  BOOL fWatchThread,    // if TRUE then release MUST be from the same thread that acquired the lock
  BOOL fAllowRecursion  // if TRUE then allow recursive lock aquiring from the same thread
);

DWORD                   // acquire exclusive lock
LFAPI                   // return result is as of WaitForSingleObjectEx
LFExLockAcquire (
  PLFEXLOCK LFExLock,   // lock
  BOOL      fWaitSpin,  // if TRUE and MP then waiting threads will spin first waiting for lock
  DWORD     dwMilliseconds, // timeout in milliseconds
  BOOL      fAlertable  // if TRUE then allow I/O completion routines calls and/or APC while waiting
);

DWORD                   // acquire exclusive lock
LFAPI                   // return result is as of WaitForSingleObjectEx
LFExLockAcquireEx (
  PLFEXLOCK LFExLock,    // lock
  PVOID     pWaitContext,// user-defined wait callback function context
  LFWAIT   *pWaitCb,     // user-defined wait callback function (if NULL WaitForSingleObjectEx will be called)
  BOOL      fWaitSpin,   // if TRUE and MP then waiting threads will spin first waiting for lock
  DWORD     dwMilliseconds, // timeout in milliseconds
  BOOL      fAlertable   // if TRUE then allow I/O completion routines calls and/or APC while waiting
);

DWORD                   // release xclusive lock
LFAPI                   // return result is as of SetEvent/GetLastError
LFExLockRelease (       // (may also return ERROR_SEM_IS_SET if lock is owned by
  PLFEXLOCK LFExLock,   // another thread and lock was created with fWatchThread)
  BOOL fSwitchThreads   // if TRUE and threre are waiting threads then force thread switch by Sleep (1)
);

BOOL                    // destroy previousely created lock
LFAPI                   // returns FALSE if lock is in use, otherwise as CloseHandle;
LFExLockDestroy (
  PLFEXLOCK LFExLock
);


/* --------------------------- Fast locks ----------------------------- */
/*                             ----------                               */


typedef struct
{
  int dummy;
} *PLFLOCK;             // anonymous data type


PLFLOCK                 // create new lock
LFAPI                   // returns NULL if not enough memory
LFLockCreateEx (
  BOOL  fWatchThread,   // if TRUE then release MUST be from the same thread that aquired the lock
  BOOL  fAllowRecursion,// if TRUE then thread that owns exclusive lock may aquire any other locks
  PVOID pMemoryContext, // user-defined memory allocator context for pAllocCb and pFreeCb
  DWORD dwMemAlignment, // min. alignment guaranteed by memory allocator (must be power of 2)
  LFALLOC *pAllocCb,    // memory allocation callback function
  LFFREE  *pFreeCb      // release memory callback function
);

PLFLOCK                 // create new lock using HeapAlloc/HeapFree with current process heap
LFAPI                   // returns NULL if not enough memory
LFLockCreate (
  BOOL fWatchThread,    // if TRUE then release MUST be from the same thread that aquired the lock
  BOOL fAllowRecursion  // if TRUE then thread that owns exclusive lock may aquire any other locks
);

BOOL                    // destroy the lock and release occupied resources
LFAPI                   // returns FALSE if lock is in use or was not properly initialized
LFLockDestroy (
  PLFLOCK pLFLock       // lock to destroy (must be not in use to succeed)
);

DWORD                   // acquire a lock
LFAPI                   // returns FALSE if lock was not properly initialized
LFLockAcquire (
  PLFLOCK pLFLock,      // lock
  DWORD   dwAction,     // one of LF_LOCK_* actions
  BOOL    fWaitSpin,    // allow waiting threads to spin?
  DWORD   dwMilliseconds, // timeout in milliseconds
  BOOL    fAlertable    // if TRUE then allow I/O completion routines calls and/or APC while waiting
);

DWORD                   // acquire a lock
LFAPI                   // returns FALSE if lock was not properly initialized
LFLockAcquireEx (
  PLFLOCK pLFLock,      // lock
  DWORD   dwAction,     // one of LF_LOCK_* actions
  PVOID   pWaitContext, // user-defined wait callback function context
  LFWAIT *pWaitCb,      // user-defined wait callback function (if NULL WaitForSingleObjectEx will be called)
  BOOL    fWaitSpin,    // allow waiting threads to spin?
  DWORD   dwMilliseconds, // timeout in milliseconds
  BOOL    fAlertable    // if TRUE then allow I/O completion routines calls and/or APC while waiting
);

DWORD                   // release lock
LFAPI                   // returns FALSE if exclusive settings do not match or
LFLockRelease (         // if lock was not properly initialized
  PLFLOCK pLFLock,      // lock
  BOOL    fExclusive,   // if TRUE then lock was exclusively owned
  BOOL    fSwitchThreads
);



/* ----------------- Lock an object using lock pool --------------------- */
/*                   ------------------------------                       */

//
// LF uses 2 events per thread in worst case (all threads are deadlocked, and each is trying to attach its
// own event/semaphore pair to lock object); hitting 2 events per thread limit (if number of threads is large
// enough) is exceptionally unlikely on practice, therefore 32K events is more than sufficient for any
// practical purposes
//
#define LF_POOL_MAX_NUMBER_OF_EVENTS    ((1<<15)-1)

typedef struct
{
  __int64 dummy[2];      // 16-byte object that may be stored explicitely
#ifdef LF_PERF_STAT
  // WMS: you must recompile "lf.c" with -DLF_PERF_STAT if you want to use statistics
  DWORD dwAcquired;     // # of times when lock was acquired
  DWORD dwWaitEx;       // # of times when caller was blocked waiting for exclusive lock
  DWORD dwWaitSh;       // # of times when caller was blocked waiting for shared lock
  DWORD dwWaitUp;       // # of times when caller was blocked waiting for upgrade
#endif /* LF_PERF_STAT */
} LFOBJECT_STORE;

typedef volatile __declspec(align(16))  LFOBJECT_STORE  LFOBJECT;
typedef volatile                        LFOBJECT_STORE *PLFOBJECT; // anonymous data type


//
// use 8-byte LFSMALLOBJECT instead of 16-byte LFOBJECT if lock pool it is used with is
// created with both fWatchThread==FALSE and fAllowRecursion==FALSE -- in this case extra
// 8 bytes used to store recursion counter and owner thread ID are not necessary and will
// not be used.
//
// NB: you'll need to cast address of LFSMALLOBJECT to PLFOBJECT when passing it LFPool*() functions.
//
#ifdef LF_PERF_STAT
typedef LFOBJECT LFSMALLOBJECT;
#else
typedef volatile __declspec(align(8)) struct
{
  __int64 dummy[1];
} LFSMALLOBJECT;
#endif /* LF_PERF_STAT */

typedef struct
{
  int dummy;
} *PLFPOOL;     // anonymous data type

PLFPOOL                 // create new lock pool
LFAPI                   // returns NULL if no enough memory
LFPoolCreateEx (
  LONG  AllocObjects,   // number of AllocObjects to allocate at once
  BOOL  fWatchThread,   // if TRUE then only the thread that acquired the lock may release it
  BOOL  fAllowRecursion,// if TRUE then thread that owns exclusive lock may aquire any other locks
  PVOID pMemoryContext, // user-defined memory allocator context for pAllocCb and pFreeCb
  DWORD dwMemAlignment, // min. alignment guaranteed by memory allocator (must be power of 2)
  LFALLOC *pAllocCb,    // memory allocation callback function
  LFFREE  *pFreeCb      // release memory callback function
);

PLFPOOL                 // create new lock pool using HeapAlloc/HeapFree with current process heap
LFAPI                   // returns NULL if no enough memory
LFPoolCreate (
  LONG AllocObjects,    // number of AllocObjects to allocate at once
  BOOL fWatchThread,    // if TRUE then only the thread that acquired the lock may release it
  BOOL fAllowRecursion  // if TRUE then thread that owns exclusive lock may aquire any other locks
);

BOOL                    // destroy the pool
LFAPI                   // returns FALSE if pool is in use or was not properly initialized
LFPoolDestroy (
  PLFPOOL LFPool
);

LONG                    // returns total amount of memory allocated by lock pool
LFAPI                   // returns -1 if pool was not properly initialized
LFPoolTellMemory (
  PLFPOOL LFPool
);

LONG                    // returns total number of object allocated by lock pool
LFAPI                   // returns -1 if pool was not properly initialized
LFPoolTellNumberOfObjects (
  PLFPOOL LFPool
);

LONG                    // allocate more NumberOfObject lock objects
LFAPI                   // returns number of new objects allocated or -1 if pool was not properly initialized
LFPoolIncreaseNumberOfObjects (
  PLFPOOL LFPool,
  LONG    NumberOfObjects
);

LONG                    // returns total number of event pairs allocated by lock pool
LFAPI                   // returns -1 if pool was not properly initialized
LFPoolTellNumberOfEvents (
  PLFPOOL LFPool
);

LONG                    // allocate more events
LFAPI                   // returns total number of new events created or -1 if pool was not properly initialized
LFPoolIncreaseNumberOfEvents (
  PLFPOOL LFPool,
  LONG    NumberOfEvents// number of events cannot exceed LF_POOL_MAX_NUMBER_OF_EVENTS
);

PLFOBJECT               // create new object associated with LFPool
LFAPI                   // return NULL if no enough memory
LFPoolObjectCreate (
  PLFPOOL LFPool
);

BOOL                    // returns FALSE if LFObject == 0 or LFObject is misaligned
LFAPI                   // (it must be aligned on 8-byte boundary, better 16-byte boundary
LFPoolObjectInit (
  PLFPOOL   LFPool,
  PLFOBJECT LFObject    // object to initialize
);

BOOL                    // destroy the object
LFAPI                   // returns FALSE if either pool or object were not initialized properly
LFPoolObjectDestroy (
  PLFPOOL   LFPool,     // pool the object is associated with
  PLFOBJECT LFObject    // object
);

DWORD                   // acquire a lock
LFAPI
LFPoolObjectLockAcquire (
  PLFPOOL   LFPool,     // pool the object is associated with
  PLFOBJECT LFObject,   // object
  DWORD dwAction,       // shared, exclusive, upgrade, downgrade
  BOOL  fWaitSpin,      // if TRUE then waiting threads will spin before actual wait
  DWORD dwMilliseconds, // timeout in milliseconds
  BOOL  fAlertable      // if TRUE then allow I/O completion routines calls and/or APC while waiting
);

DWORD                           // acquire a lock
LFAPI
LFPoolObjectLockAcquireEx (
  PLFPOOL   LFPool,             // pool the object is associated with
  PLFOBJECT LFObject,           // object
  DWORD     dwAction,           // shared, exclusive, upgrade, downgrade
  PVOID     pWaitContext,       // user-defined wait callback function context
  LFWAIT   *pWaitCb,            // user-defined wait callback function (if NULL WaitForSingleObjectEx will be called)
  BOOL      fWaitSpin,          // if TRUE then waiting threads will spin before actual wait
  DWORD     dwMilliseconds,     // timeout in milliseconds
  BOOL      fAlertable          // if TRUE then allow I/O completion routines calls and/or APC while waiting
);

DWORD                   // release lock
LFAPI                   // returns FALSE if exclusive settings do not match or
LFPoolObjectLockRelease (// either object or pool were not properly initialized
  PLFPOOL   LFPool,     // pool the object is associated with
  PLFOBJECT LFObject,   // object
  BOOL      fExclusive, // if TRUE then lock was exclusively owned
  BOOL      fSwitchThreads
);


//
// for internal WMS usage -- use at your own risk. As unsafe as it could be
//
//
// Attention! Cannot be used with small objects!
//
// NB: *pdwThreadID may be 0 in case lock owner didn't set it yet; however if
// if caller owns the lock then (*pdwThreadID == GetCurrentThreadId()) will be TRUE.
// so that function may be used to check whether current thread owns exclusive lock
// or not (provided that respective pool was created with either fWatchThread or fRecursive).
//
BOOL                    // returns TRUE if lock is acquired for exclusive ownership
LFAPI
LFPoolObjectIsExclusive (
  PLFOBJECT LFObject,   // object
  DWORD    *pdwThreadID // ID of thread that keeps the lock
);


//
// The same as LFPoolObjectIsExclusive but may be used with small objects
//
BOOL                    // returns TRUE if lock is acquired for exclusive ownership
LFAPI
LFPoolObjectIsExclusiveEx (
  PLFPOOL   LFPool,     // pool the object is associated with
  PLFOBJECT LFObject,   // object
  DWORD    *pdwThreadID // thread ID (optional, may be NULL)
);


//
// for internal WMS usage -- use at your own risk. As unsafe as it could be
//
// NB: the use of LFPoolObjectCheck() does not guarantee that object will not be
// used later and thus prevent erroneous use of released lock object: it is possible
// that thread #1 is about to acquire a lock that thread #2 is releasing. This
// function should be used only as additional but primary check.
//
BOOL                    // check to see whether object is unused
LFAPI                   // returns FALSE if pool is released or object is in use
LFPoolObjectCheck (
  PLFPOOL   LFPool,     // pool the object is associated with
  PLFOBJECT LFObject    // object
);



/* ----------- Push/Pop/Put/Get/LockAcquire/Wait multiple --------------- */
/*             ------------------------------------------                 */


enum
{
  // operations on PLFOBJECTs (dwAction shall be LF_POOL_ACQUIRE + LF_LOCK_* + possibly LF_LOCK_TRY)
  LF_OBJECT_ACQUIRE     = 0*16,

  // operations on PLFLOCKs (dwAction shall be LF_LOCK_ACQUIRE + LF_LOCK_* + possibly LF_LOCK_TRY)
  LF_LOCK_ACQUIRE       = 1*16,

  // stack operations
  LF_STACK_PUSH         = 2*16, // LFStackPushWaitEx the value from specified queue
  LF_STACK_POP          = 3*16, // LFStackPopWaitEx  the value into specified queue

  // queue operations
  LF_QUEUE_GET          = 4*16, // LFQueuePutWaitEx the value from specified queue
  LF_QUEUE_PUT          = 5*16, // LFQueueGetWaitEx the value into specified queue

  // exclusive lock operations
  LF_EXLK_ACQUIRE       = 6*16, // LFExLockAcquireEx

  // wait on system handle
  LF_SYSTEM_HANDLE      = 7*16
};


#pragma warning (push)
#pragma warning (disable: 4201) // nonstandard extension used : nameless struct/union

// descriptor of action to be taken on LF-specific data
typedef struct
{
  union
  {
    // dwAction = LF_STACK_*
    struct
    {
      PLFSTACK Stack;   // LF stack
      PVOID    Data;    // if Data == NULL, call to Push/Pop failed; otherwise, Data is not a NULL
    };

    // dwAction = LF_QUEUE_*
    struct
    {
      PLFQUEUE Queue;   // LF queue
      LFMSG    Msg;     // if Msg.Data is NULL, call to Put/Get failed; otherwise, Msg.Data is not a NULL
    };

    // dwAction == LF_OBJECT_ACQUIRE + LF_LOCK_*
    struct
    {
      PLFPOOL   Pool;   // pool of lock objects
      PLFOBJECT Object; // object to lock using a queue
    };

    // dwAction == LF_EXLK_ACQUIRE
    PLFEXLOCK ExLock;

    // dwAction = LF_LOCK_ACQUIRE + LF_LOCK_* 
    PLFLOCK   Lock;

    // system handle
    HANDLE    Handle;
  };

  DWORD dwAction;       // push/pop/put/get/acquire
  DWORD dwStatus;       // status (WAIT_TIMEOUT if not touched)
} LFMULTI;

#pragma warning (pop)


// user-specified callback implementing WaitForMultipleObjects[Ex] functionality

typedef
DWORD
LFAPI
LFWAITMULTI (
  PVOID pWaitContext,           // user-specified context
  DWORD nHandleCount,           // number of handles listed in "pHandles" array
  CONST HANDLE *pHandles,       // pointer to array of handles
  BOOL  bWaitAll,               // see WaitForMultipleObjects[Ex]
  DWORD dwMilliseconds,         // timeout in milliseconds
  BOOL  fAlertable              // if TRUE then allow I/O completion routines calls and/or APC while waiting
);



/*
   LFProcessMultipleObject() executes specified operations on set of LF object and
   [optionally] user-defined system handles; in general, it has the same semantics as
   [Msg]WaitForMultipleObjects[Ex] with few subtle differences:

   bWaitAll == FALSE:
   success return values:
       (WAIT_OBJECT_0 + n):
           operation on n-th object succeeded; pObjects[n].dwStatus is set to WAIT_OBJECT_0,
           dwStatus of all other LF objects specified in pObject[] is set to WAIT_TIMEOUT
       (WAIT_ABANDONED_0 + n):
           n-th object is abandoned mutex that satisfied the wait
   failure return values:
       WAIT_IO_COMPLETION:
           one or more APCs queued to current thread, no operation on LF object succeeded
           (dwStatus of all LF objects is set to WAIT_TIMEOUT), no user-specified handles
           were acquired
       WAIT_TIMEOUT:
           time-out interval elapsed, no operation on LF object succeeded
           (dwStatus of all LF objects is set to WAIT_TIMEOUT), no user-specified handles
           were acquired, no APCs were queued
       anything else:
           respective error code; no operation on LF object succeeded, no user-specified
           handles were acquired, no APCs were queued

   bWaitAll == TRUE:
   success return values:
       WAIT_OBJECT_0:
           operations on all LF objects succeeded and all user-specified system handles are in
           signalling state; dwStatus of all user-defined handles is set to (WAIT_OBJECT_0)
           or, for abandoned mutex[es], (WAIT_ABANDONED_0).
       (WAIT_ABANDONED_0 + n), nObjectCount <= n < (nObjectCount + nHandleCount):
           operations on all LF objects succeeded and all user-specified system handles are in
           signalling state; dwStatus of all user-defined handles is set to (WAIT_OBJECT_0);
           pHandles[n - nObjectCount] is [one of] abandoned mutex[es]

   failure return values:
       WAIT_IO_COMPLETION:
           one or more APCs queued to current thread, some operations on LF objects may
           have succeeded, no user-specified handles were acquired
       WAIT_TIMEOUT:
           time-out interval elapsed, some operations on LF objects may have succeeded;
           no user-specified handles were acquired; no APCs were queued
       anything else:
           respective error code; some operations on LF objects may have succeeded;
           no user-specified handles were acquired; no APCs were queued

   Attention: if bWaitAll == TRUE, operations on LF objects may succeed even though
   LFProcessMultipleObjects() fails. These operations are irreversible and have to
   be handled by the user (or, better yet, not used with bWaitAll == TRUE):
        operations on queues (put, get)
        operations on stacks (push, pop)
        lock downgrades (please notice that downgrade -- provided that caller held
            exclusive lock -- will always succeed, so there is no need to downgrade
            exclusive lock to shared with LFProcessMultipleObjects).
*/

DWORD                           // returns (WAIT_OBJECT_0 + n) if operation on n-th LF object succeeds
LFAPI                           // returns (WAIT_OBJECT_0 + nObjectCount + n) if wait on n-th user-defined handle succeeds
LFProcessMultipleObjects (      // otherwise, return the same result as WaitForMultipleObjectsEx would
  PVOID            pWaitContext,// user-defined context for "pWaitMultiCb"
  LFWAITMULTI     *pWaitMultiCb,// user-defined WaitForMultipleObject[Ex]-like function (if NULL WaitForMultipleObjectEx will be used)
  DWORD            nObjectCount,// number of LF objects referenced in "pObjects" array
  LFMULTI         *pObjects,    // array of desciptors of rsystem handles or LF objects and required operations
  BOOL             bWaitAll,    // see WaitForMultipleObjects[Ex]
  BOOL             fWaitSpin,   // if TRUE then waiting threads will spin before actual wait
  DWORD            dwMilliseconds,// timeout in milliseconds
  BOOL             fAlertable   // if TRUE then allow I/O completion routines calls and/or APC while waiting
);


#pragma pack (pop)      // restore original data alignment settings


#ifdef __cplusplus
}; // extern "C" {
#endif

#pragma warning (pop)

#endif /* LFAPI */
