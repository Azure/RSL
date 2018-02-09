// --------------------------------------------------------------------------
//
// Lock-free, non-blocking stack, queue, and lock pool implementation
//
// Copyright (c) Microsoft Corporation 1999-2005
//
//
// --------------------------------------------------------------------------

#include "lf.h"

#pragma warning (push)
#pragma warning (disable: 4820)     // padding

typedef LF_DATA LF;

#define __inline __forceinline


#ifdef YieldProcessor

static
int
LFSpinPauseWithBackoff (
  int Backoff
)
{
  int YieldCount = Backoff & ((int) ReadTimeStampCounter ());

  do
  {
    YieldProcessor ();
  }
  while (--YieldCount >= 0);

  if (Backoff <= 511)
    Backoff = Backoff * 2 + 1;      // Backoff is always power of 2 minus 1.

  return (Backoff);
}

#define LF_SPIN_PAUSE() (_Backoff = LFSpinPauseWithBackoff (_Backoff))
#define LF_SPIN_DECL() int _Backoff = 0

#else

#define LF_SPIN_PAUSE()
#define LF_SPIN_DECL()

#endif /* YieldProcessor */


#ifdef _PREFAST_
#pragma prefast (disable: 326, "noise")
#endif /* _PREFAST */


#if defined (_WIN64)

#if !defined (IA64)
#define IA64 1
#endif

#else
#define I386 1
#endif


//
// Early version of VC 7.0 x86 backend had a bug that was immediately fixed;
// provide a temporary workaround until we check newer CL into build env
//
#if _MSC_FULL_VER >= 13000000 && _MSC_FULL_VER < 13009466
#define VCFIX_PUSH_ESI  __asm {push esi}
#define VCFIX_POP_ESI   __asm {pop esi}
#else
#define VCFIX_PUSH_ESI
#define VCFIX_POP_ESI
#endif


//
// Attention: data alignment is very important for this code -- in some places
// we trust result of read of volatile value because reading of aligned value
// is atomic operation on 486 and Pentiums (see Intel manual, vol. 3, chapter 7);
// if it was not be the case then additional CAS (or CompareExchange, if you
// prefer longer names) would be necessary. In fact, CAS is very expensive
// operation especially on MP machines because it locks the bus and serializes
// execution, so we better stay aligned. Besides data integrity issues,
// data alignement is very important from performance point of view --
// access to non-aligned data is considerably slower.
//



/* ---------------------- Configuration settings ---------------------- */
/*                        ----------------------                        */



// detection of number of CPUs
// <= 0 -- autodetect, 1 -- UP, >= 2 -- MP
// honestly the difference in performance between fixed config and auto config
// is negligable but autoconfig code is about 1.3 times larger. If you care
// about 20 KB of code you may load different DLLs depending on number of CPUs;
// the only question is whether you want to deal with such mess or not: if the
// answer is negative set NUMBER_OF_CPUS to 0 and forget about it
#define NUMBER_OF_CPUS  0               // 0 - autodetect (default), 1 = UP, >= 2 = MP

// spin SPIN_COUNT times on multiprocessor machine before waiting
// if SPIN_COUNT < 0 then no spinning will be performed
// it considerably (up to 2-3 times) improves performance on MP machines
// especially if locks are hold for short period of time
#define SPIN_COUNT 1000         // recommended value is 1000

// make sure that nobody uses data structures that we are destroying?
// turning CHECK_USAGE on reduces performance by 20-40%
#define CHECK_USAGE     0

// make sure that nobody uses lock that is about to release
#define CHECK_LOCK_USAGE 0

// compare actual and expected values before cmpxchg8b?
#define COMPARE_MANUALLY 0

// min and max number of allocation units
#define MIN_ALLOC_UNITS 16
#define MAX_ALLOC_UNITS 256


// best address alignment (size of cache line)
#define ALIGN 64

// alignment provided by HeapAlloc
#define HEAP_ALIGN 8

// non-existing thread id
#define THREAD_ID_NONE  0


#define ASSERT(cond)


#if NUMBER_OF_CPUS == 1
// no spin counts on single-CPU machine
#undef SPIN_COUNT
#define SPIN_COUNT 0
#undef LF_SPIN_DECL
#define LF_SPIN_DECL()
#endif


/* --------------------- Default compiler settings -------------------- */
/*                       -------------------------                      */
#pragma pack (push, 16)

#if !defined (DEBUG) || DEBUG == 0
#if !defined (_M_IX86) || _MSC_VER >= 14
#pragma optimize("tg", on)
#elif defined (_M_IX86)
#if _MSC_VER >= 13
#pragma optimize ("twaygi", on)
#else
#pragma optimize ("twaxyi", on)
#endif /* _MSC_VER */
#endif /* _M_IX86 || _MSC_VER */
#pragma inline_depth (16)
#endif /* !defined (DEBUG) || DEBUG == 0 */

#pragma warning (disable: 4127) // conditional expression is constant
#pragma warning (disable: 4100) // unreferenced formal parameter
#pragma warning (disable: 4711) // function selected for automatic inline expansion
#pragma warning (disable: 4201) // nonstandard extension used : nameless struct/union


/* -------------------- 64-bit Compare-And-Swap (CAS) -------------------- */
/*                      -----------------------------                      */


#define LF_DETECT_NUMBER_OF_CPUs()
#define LFCPUNumDetect()

#if NUMBER_OF_CPUS >= 2
  #define LFCPUNum 1
  #define X86_LOCK lock
#elif NUMBER_OF_CPUS == 1
  #define LFCPUNum (-1)
  #define X86_LOCK
#else
  #define X86_LOCK

#if !defined (I386) && SPIN_COUNT <= 0
  #define LFCPUNum error -- LFCPUNum should not be referenced
#else

// place static variable into appropriate section
#ifdef    AVRT_DATA_BEGIN
#pragma   AVRT_DATA_BEGIN
#endif /* AVRT_DATA_BEGIN */

  static volatile signed char LFCPUNum = 2;     // <0 for UP, >0 for MP (will be detected on initialization)
                                                // LFCPUNum is used by x86 asm code (lock vs not lock)
                                                // and for spinning (never spin on UP).
#ifdef    AVRT_DATA_END
#pragma   AVRT_DATA_END
#endif /* AVRT_DATA_END */


#undef LF_DETECT_NUMBER_OF_CPUs
#undef LFCPUNumDetect

#define LF_DETECT_NUMBER_OF_CPUs() do if (LFCPUNum == 2) LFCPUNumDetect (); while (0)

//
// Detect number of active CPUs and set LFCPUNum to (-1) if UP or (+1) if MP
//
static
void
LFCPUNumDetect (
  void
)
{
  volatile LONG Temp;
  SYSTEM_INFO si;

  if (LFCPUNum != 2)
    return;

  GetSystemInfo (&si);

  if (si.dwNumberOfProcessors > 1 || (si.dwActiveProcessorMask & (si.dwActiveProcessorMask - 1)) != 0)
  {
    // MP -- 2 or more CPUs are active, must lock the bus
    LFCPUNum = 1;
  }
  else
  {
    // UP -- only 1 CPU is active, no lock prefix required
    LFCPUNum = -1;
  }

  InterlockedIncrement (&Temp);    // set up memory barrier to serialize writes; needed for create/destroy
}

#endif /* defined (I386) || SPIN_COUNT > 0 */

#endif /* NUMBER_OF_CPUS */

#pragma pack (push, 8)



#if 1

//
// Format of two-way lock:
//
//   ExWaiting: 20;     00-19                   // number of threads waiting for exclusive lock
//   ExActive:   1;     20-20                   // if 1 then exclusive lock was granted
//   ExUpgrade:  1;     21-21                   // make a guess
//   SpinLock:   1;     22-22                   // waiting thread may spin
//   Disabled:   1;     23-23                   // lock is disabled and will be destroyed soon
//   ShWaiting: 20;     24-43   24-31 & 0..11   // number of threads waiting for shared lock
//   ShActive:  20;     44-63           12-32   // number of shared locks granted
//

#define LockGetExWaiting(s)     ((s).Word1 & ((1<<20)-1))
#define LockTstExWaiting(s)     ((s).Word1 & ((1<<20)-1))
#define LockIncExWaiting(s)     ((s).Word1 += 1)
#define LockDecExWaiting(s)     ((s).Word1 -= 1)

#define LockTstExActive(s)      ((s).Word1 & (1<<20))
#define LockSetExActive(s)      ((s).Word1 |= (1<<20))
#define LockClrExActive(s)      ((s).Word1 &= ~(1<<20))

#define LockTstExUpgrade(s)     ((s).Word1 & (1<<21))
#define LockSetExUpgrade(s)     ((s).Word1 |= (1<<21))
#define LockClrExUpgrade(s)     ((s).Word1 &= ~(1<<21))

#define LockTstSpinWait(s)      ((s).Word1 & (1<<22))
#define LockSetSpinWait(s)      ((s).Word1 |= (1<<22))
#define LockClrSpinWait(s)      ((s).Word1 &= ~(1<<22))

#define LockTstDisabled(s)      ((s).Word1 & (1<<23))
#define LockSetDisabled(s)      ((s).Word1 |= (1<<23))

// attn: trust the value of GetShWaiting only after CAS
#define LockGetShWaiting(s)     ((((s).Word1 >> 24) + ((s).Word2 << 8)) & ((1<<20)-1))
#define LockTstShWaiting(s)     (((s).Word1 & 0xff000000) | ((s).Word2 & 0xfff))
#define LockIncShWaiting(s)     ((s).I64 += 0x1000000) /* keep Prefast happy -- it doesn't like (1<<20) */
#define LockDecShWaiting(s)     ((s).I64 -= 0x1000000) /* keep Prefast happy -- it doesn't like (1<<20) */

#define LockGetShActive(s)      ((s).Word2 >> 12)
#define LockTstShActive(s)      ((s).Word2 & 0xfffff000)
#define LockIncShActive(s)      ((s).Word2 += (1<<12))
#define LockDecShActive(s)      ((s).Word2 -= (1<<12))

// if writers active or waiting then not 0
#define LockTstExAll(s)         ((s).Word1 & ((1<<21)-1))

// if readers or writers active then not 0 (CAS required)
#define LockTstActiveAll(s)     (((s).Word1 & (1<<20)) | ((s).Word2 & 0xfffff000))

// if readers or writers active or waiting for exclusive upgrade then not 0 (CAS required)
#define LockTstActiveAllOrExUpgrade(s)  (((s).Word1 & ((1<<20)|(1<<21))) | ((s).Word2 & 0xfffff000))

// if readers or writers active or writers waiting then not 0 (CAS required)
#define LockTstAllActiveOrExWaiting(s) (((s).Word1 & ((1<<21)-1)) | ((s).Word2 & 0xfffff000))


#else /* 0 */

//
// used for debugging lock pool that have the same format
//

//
// Format of two-way lock:
//
//   ShActive:  15;     00-14   // number of threads waiting for exclusive lock
//   ExUpgrade:  1;     15-15   // make a guess
//   ExActive:   1;     16-16   // if 1 then exclusive lock was granted
//   ExWaiting: 15;     17-32   // number of shared locks granted
//
//   ShWaiting: 15;     00-14   // number of threads waiting for shared lock
//   SpinLock:   1;     15-15   // waiting thread may spin
//   Disabled:   1;     16-16   // lock is disabled and will be destroyed soon
//

#define LockGetShActive(s)      ((s).Word1 & ((1<<15)-1))
#define LockTstShActive(s)      ((s).Word1 & ((1<<15)-1))
#define LockIncShActive(s)      ((s).Word1 += 1)
#define LockDecShActive(s)      ((s).Word1 -= 1)

#define LockTstExUpgrade(s)     ((s).Word1 & (1<<15))
#define LockSetExUpgrade(s)     ((s).Word1 |= (1<<15))
#define LockClrExUpgrade(s)     ((s).Word1 &= ~(1<<15))

#define LockTstExActive(s)      ((s).Word1 & (1<<16))
#define LockSetExActive(s)      ((s).Word1 |= (1<<16))
#define LockClrExActive(s)      ((s).Word1 &= ~(1<<16))

#define LockGetExWaiting(s)     ((s).Word1 >> 17)
#define LockTstExWaiting(s)     ((s).Word1 & 0xfffe0000)
#define LockIncExWaiting(s)     ((s).Word1 += (1<<17))
#define LockDecExWaiting(s)     ((s).Word1 -= (1<<17))

#define LockGetShWaiting(s)     ((s).Word2 & ((1<<15)-1))
#define LockTstShWaiting(s)     ((s).Word2 & ((1<<15)-1))
#define LockIncShWaiting(s)     ((s).Word2 += 1)
#define LockDecShWaiting(s)     ((s).Word2 -= 1)

#define LockTstSpinWait(s)      ((s).Word2 & (1<<15))
#define LockSetSpinWait(s)      ((s).Word2 |= (1<<15))
#define LockClrSpinWait(s)      ((s).Word2 &= ~(1<<15))

#define LockTstDisabled(s)      ((s).Word2 & (1<<16))
#define LockSetDisabled(s)      ((s).Word2 |= (1<<16))

// if writers active or waiting then not 0
#define LockTstExAll(s)         ((s).Word1 & 0xffff0000)

// if readers or writers active then not 0
#define LockTstActiveAll(s)     ((s).Word1 & 0x17fff)

// if readers or writers active or waiting for exclusive upgrade then not 0 (CAS required)
#define LockTstActiveAllOrExUpgrade(s) ((s).Word1 & 0x1ffff)

// if readers or writers active or writers are in queue then not 0
// in fact we also return ExUpgrade but since ExUpgrade implies ExWaiting it is OK
#define LockTstAllActiveOrExWaiting(s) ((s).Word1)

#endif /* 1 */


//
// Format of two-way lock object:
//
//   ShActive:  15;     00-14   // number of threads waiting for exclusive lock
//   ExUpgrade:  1;     15-15   // make a guess
//   ExActive:   1;     16-16   // if 1 then exclusive lock was granted
//   ExWaiting: 15;     17-31   // number of shared locks granted
//
//   ShWaiting: 15;     00-14   // number of threads waiting for shared lock
//   SpinLock:   1;     15-15   // waiting thread may spin
//   UserObject: 1;     16-16   // user-allocated object
//   EventIndex:15;     17-31   // event index
//

#define ObjectGetShActive(s)    ((s).Word1 & ((1<<15)-1))
#define ObjectTstShActive(s)    ((s).Word1 & ((1<<15)-1))
#define ObjectIncShActive(s)    ((s).Word1 += 1)
#define ObjectDecShActive(s)    ((s).Word1 -= 1)

#define ObjectTstExUpgrade(s)   ((s).Word1 & (1<<15))
#define ObjectSetExUpgrade(s)   ((s).Word1 |= (1<<15))
#define ObjectClrExUpgrade(s)   ((s).Word1 &= ~(1<<15))

#define ObjectTstExActive(s)    ((s).Word1 & (1<<16))
#define ObjectSetExActive(s)    ((s).Word1 |= (1<<16))
#define ObjectClrExActive(s)    ((s).Word1 &= ~(1<<16))

#define ObjectGetExWaiting(s)   ((s).Word1 >> 17)
#define ObjectTstExWaiting(s)   ((s).Word1 & 0xfffe0000)
#define ObjectIncExWaiting(s)   ((s).Word1 += (1<<17))
#define ObjectDecExWaiting(s)   ((s).Word1 -= (1<<17))

#define ObjectGetShWaiting(s)   ((s).Word2 & ((1<<15)-1))
#define ObjectTstShWaiting(s)   ((s).Word2 & ((1<<15)-1))
#define ObjectIncShWaiting(s)   ((s).Word2 += 1)
#define ObjectDecShWaiting(s)   ((s).Word2 -= 1)

#define ObjectTstSpinWait(s)    ((s).Word2 & (1<<15))
#define ObjectSetSpinWait(s)    ((s).Word2 |= (1<<15))
#define ObjectClrSpinWait(s)    ((s).Word2 &= ~(1<<15))

#define ObjectGetEvent(s)       ((s).Word2 >> 17)
#define ObjectTstEvent(s)       ((s).Word2 & 0xfffe0000)
#define ObjectSetEvent(s,i)     ((s).Word2 += ((i) << 17))
#define ObjectClrEvent(s)       ((s).Word2 &= 0x0001ffff)

#define ObjectGetUser(s)        ((s).Word2 & (1<<16))
#define ObjectSetUser(s)        ((s).Word2 |= (1<<16))

// if writers active or waiting then not 0
#define ObjectTstExAll(s)               ((s).Word1 & 0xffff0000)

// if readers or writers active then not 0
#define ObjectTstActiveAll(s)   ((s).Word1 & 0x17fff)

// if readers or writers active or waiting for exclusive upgrade then not 0 (CAS required)
#define ObjectTstActiveAllOrExUpgrade(s) ((s).Word1 & 0x1ffff)

// if readers or writers active or writers are in queue then not 0
// in fact we also return ExUpgrade but since ExUpgrade implies ExWaiting it is OK
#define ObjectTstAllActiveOrExWaiting(s) ((s).Word1)

// if anything is active or waiting then not 0
#define ObjectTstAll(s)         ((s).Word1 | ((s).Word2 & ~(1<<16)))

// if readers or writers waiting then not 0
#define ObjectTstWaitingAll(s)  (((s).Word1 & 0xfffe0000) | ((s).Word2 & 0x7fff))


#ifdef    AVRT_CODE_BEGIN
#pragma   AVRT_CODE_BEGIN
#endif /* AVRT_CODE_BEGIN */

//
// This is faster than respective Interlocked* functions
//
#ifndef IA64

#ifndef I386

#error unsupported platform (only x86, IA64, and AMD64 are supported currently)
#error if necessary, support for MIPS, Alpha, AXP64 and ARMv6 can be added

#else /* I386 */


#define REG     edx
static
__inline
LONG
Increment (
  volatile LONG *Adr
)
{
  __asm {
        mov     eax, 1
        mov     REG, Adr

#if NUMBER_OF_CPUS <= 0
        cmp     LFCPUNum, ah
        jle     CPU1

        lock    xadd [REG], eax
        jmp     CommonExit
CPU1:
        xadd    [REG], eax
CommonExit:
#else
        X86_LOCK        xadd [REG], eax
#endif /* NUMBER_OF_CPUS <= 0 */

        inc     eax
  }
}

static
__inline
LONG
Decrement (
  volatile LONG *Adr
)
{
  __asm {
        mov     eax, -1
        mov     REG, Adr

#if NUMBER_OF_CPUS <= 0
        cmp     LFCPUNum, al
        jle     CPU1

        lock    xadd [REG], eax
        jmp     CommonExit
CPU1:
        xadd    [REG], eax
CommonExit:
#else
        X86_LOCK        xadd [REG], eax
#endif /* NUMBER_OF_CPUS <= 0 */
        dec     eax
  }
}


static
__inline
LONG
LFInterlockedAdd (
  volatile LONG *Adr,
  LONG Value
)
{
  __asm {
        mov     eax, Value
        mov     REG, Adr

#if NUMBER_OF_CPUS <= 0
        cmp     LFCPUNum, 0
        jle     CPU1

        lock    xadd [REG], eax
        jmp     CommonExit
CPU1:
        xadd    [REG], eax
CommonExit:
#else
        X86_LOCK        xadd [REG], eax
#endif /* NUMBER_OF_CPUS <= 0 */
  }
}
#undef REG


//
// when loading LF for CASPtr, Cnt must be loaded first
//
#define LFLoadPtr(Dst,Src) do { \
  (Dst).Cnt = (Src).Cnt;        \
  (Dst).Ptr = (Src).Ptr;        \
} while (0)

//
// compare two protected pointers: Ptr first, Cnt last
//
#define SAME_POINTER(Ptr1,Ptr2) ((Ptr1).Ptr == (Ptr2).Ptr && (Ptr1).Cnt == (Ptr2).Cnt)


//
// Compare-and-swap for (pointer, modification counter) pair
//
static
__inline
UCHAR
CASPtr (
  volatile LF *Entry,
  PVOID Pointer,
  LF    Comp
)
{
  /*
    Atomically execute:
    if (Entry->Ptr == Comp.Ptr && Entry->Cnt == Comp.Cnt)
    {
      Entry->Ptr = Ptr;
      Entry->Cnt += 1;
      return TRUE;
    }
    return FALSE;
  */

  __asm {
        VCFIX_PUSH_ESI
        mov     esi, Entry
        mov     eax, Comp.Ptr
        mov     edx, Comp.Cnt

        mov     ebx, Pointer

#if NUMBER_OF_CPUS <= 0
        cmp     LFCPUNum, 0
#endif /* NUMBER_OF_CPUS <= 0 */

        lea     ecx, [edx+1]

#if NUMBER_OF_CPUS <= 0
        jle     CPU1
#endif /* NUMBER_OF_CPUS <= 0 */

#if COMPARE_MANUALLY && NUMBER_OF_CPUS != 1
        cmp     [esi], eax
        jne     failure
        cmp     [esi+4], edx
        jne     failure
#endif /* COMPARE_MANUALLY && NUMBER_OF_CPUS != 1 */

#if NUMBER_OF_CPUS <= 0
        lock    cmpxchg8b [esi]
        jmp     CommonExit
CPU1:
        cmpxchg8b [esi]
CommonExit:
#else
        X86_LOCK        cmpxchg8b [esi]
#endif /* NUMBER_OF_CPUS <= 0 */

#if COMPARE_MANUALLY && NUMBER_OF_CPUS != 1
failure:
#endif /* COMPARE_MANUALLY && NUMBER_OF_CPUS != 1 */

        VCFIX_POP_ESI
        sete    al
  }
}


//
// Compare-and-swap for pair of DWORDs
//
static
__inline
UCHAR
CASLock (
  volatile LF *State,
  LF NewState,
  LF OldState
)
{

  /*
    Atomically execute:
    if (State->I64 == OldState.I64)
    {
      *State = NewState;
      return (TRUE);
    }
    return (FALSE);
  */
  __asm {
        VCFIX_PUSH_ESI
        mov     eax, OldState.Word1
        mov     edx, OldState.Word2

        mov     ebx, NewState.Word1
        mov     ecx, NewState.Word2

#if NUMBER_OF_CPUS <= 0
        cmp     LFCPUNum, 0
#endif /* NUMBER_OF_CPUS <= 0 */

        mov     esi, State

#if NUMBER_OF_CPUS <= 0
        jle     CPU1
#endif /* NUMBER_OF_CPUS <= 0 */

#if COMPARE_MANUALLY && NUMBER_OF_CPUS != 1
        cmp     [esi], eax
        jne     failure
        cmp     [esi+4], edx
        jne     failure
#endif /* COMPARE_MANUALLY && NUMBER_OF_CPUS != 1 */

#if NUMBER_OF_CPUS <= 0
        lock    cmpxchg8b [esi]
        jmp     CommonExit
CPU1:
        cmpxchg8b [esi]
CommonExit:
#else
        X86_LOCK        cmpxchg8b [esi]
#endif /* NUMBER_OF_CPUS <= 0 */

#if COMPARE_MANUALLY && NUMBER_OF_CPUS != 1
failure:
#endif /* COMPARE_MANUALLY && NUMBER_OF_CPUS != 1 */

        VCFIX_POP_ESI
        sete    al
  }
}


LF_DATA
LFAPI
LF_CAS (
  LF_VDATA * pData,
  LF_DATA    NewData,
  LF_DATA    OldData
)
{
  /*
    Atomically execute:
    if (*pData == OldData)
      *pData = NewData;
    return (*pData);
  */
  __asm {
        VCFIX_PUSH_ESI
        mov     eax, OldData.Word1
        mov     edx, OldData.Word2

        mov     ebx, NewData.Word1
        mov     ecx, NewData.Word2

#if NUMBER_OF_CPUS <= 0
        cmp     LFCPUNum, 0
#endif /* NUMBER_OF_CPUS <= 0 */

        mov     esi, pData

#if NUMBER_OF_CPUS <= 0
        jle     CPU1
#endif /* NUMBER_OF_CPUS <= 0 */

#if COMPARE_MANUALLY && NUMBER_OF_CPUS != 1
        cmp     [esi], eax
        jne     failure
        cmp     [esi+4], edx
        jne     failure
#endif /* COMPARE_MANUALLY && NUMBER_OF_CPUS != 1 */

#if NUMBER_OF_CPUS <= 0
        lock    cmpxchg8b [esi]
        jmp     CommonExit
CPU1:
        cmpxchg8b [esi]
CommonExit:
#else
        X86_LOCK        cmpxchg8b [esi]
#endif /* NUMBER_OF_CPUS <= 0 */

#if COMPARE_MANUALLY && NUMBER_OF_CPUS != 1
failure:
#endif /* COMPARE_MANUALLY && NUMBER_OF_CPUS != 1 */
        VCFIX_POP_ESI
  }
}


//
// Compare-and-swap for LONGs (faster than InterlockedCompareExchange)
//
static
__inline
UCHAR
CASLong (
  volatile LONG *Value,
  LONG NewValue,
  LONG OldValue
)
{
  /*
    Atomically execute:
    if (*Value == OldValue)
    {
      *Value = NewValue;
      return (TRUE);
    }
    return (FALSE);
  */

  __asm {
        mov     eax, OldValue
        mov     edx, NewValue

#if NUMBER_OF_CPUS <= 0
        cmp     LFCPUNum, 0
#endif /* NUMBER_OF_CPUS <= 0 */

        mov     ebx, Value

#if NUMBER_OF_CPUS <= 0
        jle     CPU1
#endif /* NUMBER_OF_CPUS <= 0 */

#if COMPARE_MANUALLY && NUMBER_OF_CPUS != 1
        cmp     [ebx], eax
        jne     failure
#endif /* COMPARE_MANUALLY && NUMBER_OF_CPUS != 1 */

#if NUMBER_OF_CPUS <= 0
        lock    cmpxchg [ebx], edx
        jmp     CommonExit
CPU1:
        cmpxchg [ebx], edx
CommonExit:
#else
        X86_LOCK        cmpxchg [ebx], edx
#endif /* NUMBER_OF_CPUS <= 0 */

#if COMPARE_MANUALLY && NUMBER_OF_CPUS != 1
failure:
#endif /* COMPARE_MANUALLY && NUMBER_OF_CPUS != 1 */

        sete    al
  }
}


//
// Compare-and-swap for PVOIDs (faster than InterlockedCompareExchange)
//
static
__inline
UCHAR
CASRef (
  volatile PVOID *Value,
  PVOID NewValue,
  PVOID OldValue
)
{
  /*
    Atomically execute:
    if (*Value == OldValue)
    {
      *Value = NewValue;
      return (TRUE);
    }
    return (FALSE);
  */

  __asm {
        mov     eax, OldValue
        mov     edx, NewValue

#if NUMBER_OF_CPUS <= 0
        cmp     LFCPUNum, 0
#endif /* NUMBER_OF_CPUS <= 0 */

        mov     ebx, Value

#if NUMBER_OF_CPUS <= 0
        jle     CPU1
#endif /* NUMBER_OF_CPUS <= 0 */

#if COMPARE_MANUALLY && NUMBER_OF_CPUS != 1
        cmp     [ebx], eax
        jne     failure
#endif /* COMPARE_MANUALLY && NUMBER_OF_CPUS != 1 */

#if NUMBER_OF_CPUS <= 0
        lock    cmpxchg [ebx], edx
        jmp     CommonExit
CPU1:
        cmpxchg [ebx], edx
CommonExit:
#else
        X86_LOCK        cmpxchg [ebx], edx
#endif /* NUMBER_OF_CPUS <= 0 */

#if COMPARE_MANUALLY && NUMBER_OF_CPUS != 1
failure:
#endif /* COMPARE_MANUALLY && NUMBER_OF_CPUS != 1 */

        sete    al
  }
}


#define SET_POINTER(lf,pointer,counter) ((lf).Ptr = (pointer), (lf).Cnt = (counter))
#define GET_POINTER(lf) ((lf).Ptr)
#define GET_COUNTER(lf) ((lf).Cnt)

#endif /* I386 */

#else /* IA64 or another 64-bit architecture that doesn't have 128-bit InterlockedCompareExchange */

//
// Use the same technique that was used for kernel's SLists: pack pointer and
// a counter into 64-bit using 21 most significant bits and 3 least significant
// bits of a pointer value that should be all 0s to keep counter value
//

#define IA64_LOG_ALIGN          3       // 8-byte alignment is a h/w requirement
#define IA64_LOG_COUNTER        24      // # of bits per counter
#define IA64_MASK_COUNTER       ((1 << IA64_LOG_COUNTER) - 1)

#define PACK_POINTER(Ptr,Cnt)   ((((__int64) (Ptr)) << (IA64_LOG_COUNTER-IA64_LOG_ALIGN)) + ((Cnt) & IA64_MASK_COUNTER))
#define SET_POINTER(lf,ptr,cnt) ((lf).I64 = PACK_POINTER ((ptr), (cnt)))
#define GET_POINTER(lf)         ((PVOID) (LONG_PTR) ((((unsigned __int64) (lf).I64) >> IA64_LOG_COUNTER) << IA64_LOG_ALIGN))
#define GET_COUNTER(lf)         ((ULONG) (ULONG_PTR) ((lf).I64 & IA64_MASK_COUNTER))


#define Increment(Adr)                  InterlockedIncrement ((PLONG) (Adr))
#define Decrement(Adr)                  InterlockedDecrement ((PLONG) (Adr))
#define LFInterlockedAdd(Adr,Value)     InterlockedExchangeAdd ((PLONG) (Adr), (Value))


#define LFLoadPtr(Dst,Src) ((Dst).I64 = (Src).I64)

//
// compare two protected pointers: Ptr first, Cnt last
//
#define SAME_POINTER(Ptr1,Ptr2) ((Ptr1).I64 == (Ptr2).I64)


#ifndef _M_IX86
#define LFInterlockedCompareExchange64(Address,ExchValue,CompValue) \
  ((__int64) InterlockedCompareExchangePointer ((PVOID *) (Address), (PVOID) (ExchValue), (PVOID) (CompValue)))
#else
#pragma message ("Attention! Generating 64-bit code on 32-bit platform -- checking syntax only")
__int64 LFInterlockedCompareExchange64 (PVOID Address, __int64 ExchValue, __int64 CompValue)
{
  return (* (volatile __int64 *) Address);
}
#endif /* _M_IX86 */


static
__inline
__int64
CASPtr (
  volatile LF *Entry,
  PVOID Pointer,
  LF    Comp
)
{
  return (LFInterlockedCompareExchange64 ((__int64 *) &Entry->I64, PACK_POINTER (Pointer, (Comp.I64 + 1) & IA64_MASK_COUNTER), Comp.I64) == Comp.I64);
}

static
__inline
__int64
CASLock (
  volatile LF *State,
  LF NewState,
  LF OldState
)
{
  return (LFInterlockedCompareExchange64 ((__int64 *) State, NewState.I64, OldState.I64) == OldState.I64);
}

LF_DATA
LFAPI
LF_CAS (
  LF_VDATA * pData,
  LF_DATA    NewData,
  LF_DATA    OldData
)
{
  NewData.I64 = LFInterlockedCompareExchange64 ((__int64 *) &pData->I64, NewData.I64, OldData.I64);
  return (NewData);
}


static
__inline
__int64
CASLong (
  volatile LONG *Value,
  LONG NewValue,
  LONG OldValue
)
{
  return (InterlockedCompareExchange ((PLONG) Value, NewValue, OldValue) == OldValue);
}


static
__inline
__int64
CASRef (
  volatile PVOID *Value,
  PVOID NewValue,
  PVOID OldValue
)
{
  return (InterlockedCompareExchangePointer ((PVOID) Value, NewValue, OldValue) == OldValue);
}

#endif /* IA64 */


void
LFAPI
LFGlobalInit (
  void
)
{
  LF_DETECT_NUMBER_OF_CPUs();
}

static
void
LFSerializeWrites (
  void
)
{
  volatile LONG Temp;
  Increment (&Temp);    // set up memory barrier to serialize writes; needed for create/destroy
}


/* -------------------- WaitForSingleObjectEx proxy ---------------------- */
/*                      ---------------------------                        */

static
DWORD
LFWaitObject (HANDLE Event, DWORD dwMilliseconds, BOOL fAlertable)
{
  if (dwMilliseconds == 0)
  {
    // if we are not going to wait do not go to the system -- return immediately
    return (WAIT_TIMEOUT);
  }
  return (WaitForSingleObjectEx (Event, dwMilliseconds, fAlertable));
}

#define LF_WAIT(pWaitCb,pWaitContext,hWaitObject,dwMilliseconds,fAlertable)     \
(                                                                               \
  ((pWaitCb) == 0)                                                              \
    ? LFWaitObject ((hWaitObject), (dwMilliseconds), (fAlertable))              \
    : (pWaitCb) ((pWaitContext), (hWaitObject), (dwMilliseconds), (fAlertable)) \
)

//
// never use regular wait functions internally; use LF_WAIT instead
//

#undef WaitForSingleObject
#define WaitForSingleObject(Event,dwMilliseconds,fAlertable) NeverUseWaitForSingleObject

#undef WaitForSingleObjectEx
#define WaitForSingleObjectEx(Event,dwMilliseconds,fAlertable) NeverUseWaitForSingleObjectEx



/* ------------- Simple, non-blocking, lock-free push and pop ------------ */
/*               --------------------------------------------              */

//
// when pushing we simply link existing head after new node and substitute
// current node with new node (if substitution fails we repeat it).
//
// when popping we not just substitute the head with next node but also
// check/update pop counter so that pop may succeed iff nobody popped
// in between; it guarantees that stack is always correct (if there were
// pushes then head had changed; if there were pops then counter had changed).
//


#ifndef IA64

#if 0

//
// pure C implementation of PUSH and POP; since these macros will be
// extensively used throughout the code it makes sense to rewrite them
// entirely in asm making code faster and shorter
//

#define PUSH(Head,Node,Next) \
do \
{ \
  LF HeadCopy; \
  do \
  { \
    LFLoadPtr (HeadCopy, (Head)); \
    (Node)->Next = GET_POINTER (HeadCopy); \
  } \
  while (!CASPtr (&(Head), Node, HeadCopy)); \
} \
while (0)

#define POP(Head,Node,Next) \
do \
{ \
  LF HeadCopy; \
  do \
  { \
    LFLoadPtr (HeadCopy, (Head)); \
    (Node) = GET_POINTER (HeadCopy); \
    /* insert new head -- if nobody did it meanwhile */ \
  } \
  while ((Node) != NULL && !CASPtr (&(Head), (Node)->Next, HeadCopy)); \
} \
while (0)

#else

//
// x86 asm implementation of non-blocking, lock-free PUSH and POP
// see above implementation in C that is less obscure
//

#define UNIQUE_LABEL_DEF(a,x)           a##x
#define UNIQUE_LABEL_DEF_X(a,x)         UNIQUE_LABEL_DEF(a,x)
#define UNIQUE_LABEL(a)                 UNIQUE_LABEL_DEF_X(_unique_label_##a##_, __LINE__)

#if NUMBER_OF_CPUS > 0

#define PUSH(Head,Node,Next)                            \
do                                                      \
{                                                       \
  volatile LF *pHead = &(Head);                         \
  __asm                                                 \
  {                                                     \
        VCFIX_PUSH_ESI                                  \
        __asm   {mov    esi, pHead}                     \
        __asm   {mov    ebx, Node}                      \
        __asm   {mov    eax, dword ptr [esi]}           \
__asm   {UNIQUE_LABEL(1):}                              \
        __asm   {mov    dword ptr [ebx]Node.Next, eax}  \
        __asm   {X86_LOCK cmpxchg dword ptr [esi], ebx} \
        __asm   {jne    UNIQUE_LABEL(1)}                \
        VCFIX_POP_ESI                                   \
  }                                                     \
}                                                       \
while (0)


#define POP(Head,Node,Next)                             \
do                                                      \
{                                                       \
  volatile LF *pHead = &(Head);                         \
  __asm                                                 \
  {                                                     \
        VCFIX_PUSH_ESI                                  \
        __asm   {mov    esi, pHead}                     \
        __asm   {mov    edx, dword ptr [esi+4]}         \
        __asm   {mov    eax, dword ptr [esi]}           \
__asm   {UNIQUE_LABEL(1):}                              \
        __asm   {test   eax, eax}                       \
        __asm   {lea    ecx, [edx+1]}                   \
        __asm   {je     UNIQUE_LABEL(2)}                \
        __asm   {mov    ebx, dword ptr [eax]Node.Next}  \
        __asm   {X86_LOCK cmpxchg8b qword ptr [esi]}    \
        __asm   {jne    UNIQUE_LABEL(1)}                \
__asm   {UNIQUE_LABEL(2):}                              \
        VCFIX_POP_ESI                                   \
        __asm   {mov    Node, eax}                      \
  }                                                     \
}                                                       \
while (0)

#else /* NUMBER_OF_CPUS <= 0 -- autodetect */

#define PUSH(Head,Node,Next)                            \
do                                                      \
{                                                       \
  volatile LF *pHead = &(Head);                         \
  __asm                                                 \
  {                                                     \
        VCFIX_PUSH_ESI                                  \
        __asm   {mov    esi, pHead}                     \
        __asm   {mov    ebx, Node}                      \
        __asm   {cmp    LFCPUNum, 0}                    \
        __asm   {mov    eax, dword ptr [esi]}           \
        __asm   {jl     UNIQUE_LABEL(2)}                \
__asm   {UNIQUE_LABEL(1):}                              \
        __asm   {mov    dword ptr [ebx]Node.Next, eax}  \
        __asm   {lock cmpxchg dword ptr [esi], ebx}     \
        __asm   {je     UNIQUE_LABEL(3)}                \
        __asm   {jmp    UNIQUE_LABEL(1)}                \
__asm   {UNIQUE_LABEL(2):}                              \
        __asm   {mov    dword ptr [ebx]Node.Next, eax}  \
        __asm   {cmpxchg dword ptr [esi], ebx}          \
        __asm   {jne    UNIQUE_LABEL(2)}                \
__asm   {UNIQUE_LABEL(3):}                              \
        VCFIX_POP_ESI                                   \
  }                                                     \
}                                                       \
while (0)

#define POP(Head,Node,Next)                             \
do                                                      \
{                                                       \
  volatile LF *pHead = &(Head);                         \
  __asm                                                 \
  {                                                     \
        VCFIX_PUSH_ESI                                  \
        __asm   {mov    esi, pHead}                     \
        __asm   {mov    edx, dword ptr [esi+4]}         \
        __asm   {cmp    LFCPUNum, 0}                    \
        __asm   {mov    eax, dword ptr [esi]}           \
        __asm   {jl     UNIQUE_LABEL(2)}                \
__asm   {UNIQUE_LABEL(1):}                              \
        __asm   {test   eax, eax}                       \
        __asm   {lea    ecx, [edx+1]}                   \
        __asm   {je     UNIQUE_LABEL(3)}                \
        __asm   {mov    ebx, dword ptr [eax]Node.Next}  \
        __asm   {lock cmpxchg8b qword ptr [esi]}        \
        __asm   {je     UNIQUE_LABEL(3)}                \
        __asm   {jmp    UNIQUE_LABEL(1)}                \
__asm   {UNIQUE_LABEL(2):}                              \
        __asm   {test   eax, eax}                       \
        __asm   {lea    ecx, [edx+1]}                   \
        __asm   {je     UNIQUE_LABEL(3)}                \
        __asm   {mov    ebx, dword ptr [eax]Node.Next}  \
        __asm   {cmpxchg8b qword ptr [esi]}             \
        __asm   {jne    UNIQUE_LABEL(2)}                \
__asm   {UNIQUE_LABEL(3):}                              \
        VCFIX_POP_ESI                                   \
        __asm   {mov    Node, eax}                      \
  }                                                     \
}                                                       \
while (0)

#endif /* NUMBER_OF_CPUS > 0 */

#endif /* 0 */

#else /* IA64 */

#define PUSH(Head,Node,Next) \
do \
{ \
  LF HeadCopy; \
  do \
  { \
    LFLoadPtr (HeadCopy, (Head)); \
    (Node)->Next = GET_POINTER (HeadCopy); \
  } \
  while (!CASPtr (&(Head), Node, HeadCopy)); \
} \
while (0)

#define POP(Head,Node,Next) \
do \
{ \
  LF HeadCopy; \
  do \
  { \
    LFLoadPtr (HeadCopy, (Head)); \
    (Node) = GET_POINTER (HeadCopy); \
    /* insert new head -- if nobody did it meanwhile */ \
  } \
  while ((Node) != NULL && !CASPtr (&(Head), (Node)->Next, HeadCopy)); \
} \
while (0)

#endif /* IA64 */



/* ------------------------- User-defined stuff -------------------------- */
/*                           ------------------                            */

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
)
{
  return ((BOOL) CASPtr (pData, pNewPtr, OldData));
}


//
// Get the pointer stored in LF_DATA
// 
PVOID
LFAPI
LF_GETPTR (
  LF_DATA   Data
)
{
  return (GET_POINTER (Data));
}


//
// Load VDATA
//
LF_DATA
LFAPI
LF_LOAD (
    LF_VDATA *pData
)
{
    LF_DATA Data;
    LFLoadPtr (Data, (*pData));
    return (Data);
}



//
// Compare
//
BOOL
LFAPI
LF_SAMEPTR (
    LF_DATA a,
    LF_DATA b
)
{
  return (SAME_POINTER (a, b));
}


//
// Push/pop
//
void
LFAPI
LF_PUSH (
  LF_VDATA *pStack,
  LF_LINK  *pObjectLinkField,
  PVOID     pObject
)
{
  pObjectLinkField->YouReallyDontWantToKnowWhatIsInside1 = pObject;
  PUSH ((*pStack), pObjectLinkField, YouReallyDontWantToKnowWhatIsInside0);
}


PVOID
LFAPI
LF_POP (
  LF_VDATA *pStack
)
{
  LF_LINK *pObjectLinkField;
  POP ((*pStack), pObjectLinkField, YouReallyDontWantToKnowWhatIsInside0);
  if (pObjectLinkField != NULL)
    pObjectLinkField = pObjectLinkField->YouReallyDontWantToKnowWhatIsInside1;
  return (pObjectLinkField);
}


/* ------------------------- Memory allocation --------------------------- */
/*                           -----------------                             */

typedef struct NODE_BLOCK_T NODE_BLOCK;

struct NODE_BLOCK_T
{
  NODE_BLOCK *BlockNext;
  PVOID       BlockFree;
};

typedef struct
{
  DWORD    dwAlign;             // min. allocator alignment
  PVOID    pContext;            // user-defined memory allocator context
  LFALLOC *pAlloc;              // memory allocation function
  LFFREE  *pFree;               // memory release function
} LFMEM;


static
int                     // check validity of user-defined memory allocator
LFInitMem (
  LFMEM *pMem,          // user-defined memory allocator storage
  PVOID pMemoryContext, // user-defined memory allocator context for pAllocCb and pFreeCb
  DWORD dwMemAlignment, // min. alignment guaranteed by memory allocator (must be power of 2)
  LFALLOC *pAllocCb,    // memory allocation callback function
  LFFREE  *pFreeCb      // release memory callback function
)
{
  if (dwMemAlignment <= 0)
    dwMemAlignment = 1;
  pMem->pContext = pMemoryContext;
  pMem->dwAlign = dwMemAlignment;
  pMem->pAlloc = pAllocCb;
  pMem->pFree = pFreeCb;
  return (pAllocCb != NULL && pFreeCb != NULL && (dwMemAlignment & (dwMemAlignment-1)) == 0);
}


static          // allocate "size" bytes
PVOID           // returns NULL if no enough memory
LFAlloc (LFMEM *pMem, LONG Size, volatile LONG *AllocatedSize)
{
  PVOID Ptr;
  if (Size <= 0)
    return (NULL);
  if (pMem != NULL)
  {
    Ptr = pMem->pAlloc (pMem->pContext, Size);
    if (AllocatedSize != NULL)
      LFInterlockedAdd (AllocatedSize, Size);
  }
  else
  {
    HANDLE Heap = GetProcessHeap ();
    Ptr = HeapAlloc (Heap, 0, Size);
    if (Ptr != NULL && AllocatedSize != NULL)
    {
      Size = (LONG) HeapSize (Heap, 0, Ptr);
      if (Size > 0)
        LFInterlockedAdd (AllocatedSize, Size);
    }
  }
  return (Ptr);
}

static          // releases memory referenced by "ptr"
void
LFFree (LFMEM *pMem, PVOID Ptr, volatile LONG *AllocatedSize)
{
  if (pMem != NULL)
  {
    pMem->pFree (pMem->pContext, Ptr);
  }
  else
  {
    HANDLE Heap = GetProcessHeap ();
    if (AllocatedSize != NULL)
    {
      LONG   Size = (LONG) HeapSize (Heap, 0, Ptr);
      if (Size > 0)
        LFInterlockedAdd (AllocatedSize, -Size);
    }
    HeapFree (GetProcessHeap (), 0, Ptr);
  }
}

static
PVOID
LFAllocAligned (LFMEM *pMem, PVOID *FreePtr, LONG Size, volatile LONG *AllocatedSize)
{
  PUCHAR Ptr;
  DWORD Align, HeapAlign;

  // overflow check
  if (Size <= 0)
  {
    *FreePtr = NULL;
    return (NULL);
  }

  Align = ALIGN;
  while (((DWORD) Size) <= (Align >> 1))
    Align >>= 1;

  HeapAlign = HEAP_ALIGN;
  if (pMem != 0)
    HeapAlign = pMem->dwAlign;
  if (Align <= HeapAlign)
    Align = 1;

  // overflow check
  if ((LONG) (Size + Align) <= 1 || (LONG) Align <= 0)
  {
    *FreePtr = NULL;
    return (NULL);
  }
    

  Ptr = LFAlloc (pMem, Size + Align - 1, AllocatedSize);
  *FreePtr = Ptr;
  Ptr += (Align - ((LONG) (LONG_PTR) Ptr)) & (Align - 1);

  return (Ptr);
}
  

static          // allocate block of "Size" bytes and link it into "Head"
PVOID
LFAllocBlock (LFMEM *pMem, volatile LF *Head, LONG Size, volatile LONG *AllocatedSize)
{
  NODE_BLOCK *Block;
  PVOID BlockFree;
  PUCHAR BlockAligned;

  // overflow check
#ifdef _PREFAST_
#pragma prefast(suppress:12011, "silly sizeof checking")
#endif /* _PREFAST_ */
  if (Size <= 0 || Size + sizeof (*Block) <= 0)
    return (NULL);

  // allocate new block containing extra "Size" bytes
#ifdef _PREFAST_
#pragma prefast(suppress:12011, "silly sizeof checking")
#endif /* _PREFAST_ */
  BlockAligned = LFAllocAligned (pMem, &BlockFree, sizeof (*Block) + Size, AllocatedSize);
  if (BlockAligned == NULL)
    return (NULL);
  Block = (NODE_BLOCK *) (BlockAligned + Size);
  Block->BlockFree = BlockFree;

  // add the block to the list of allocated blocks
  PUSH ((*Head), Block, BlockNext);

  // return pointer to first available byte
  return (BlockAligned);
}

static          // release list of memory blocks referenced by "Head"
void
LFFreeBlocks (LFMEM *pMem, volatile LF *Head, volatile LONG *AllocatedSize)
{
  NODE_BLOCK *Block;

  for (;;)
  {
    // get next block from list of allocated blocks
    POP ((*Head), Block, BlockNext);

    // if such does not exist just return
    if (Block == NULL)
      break;

    // release it
    LFFree (pMem, Block->BlockFree, AllocatedSize);
  }
}

     

/* -------------------- Check data structures usage ---------------------- */
/*                      ---------------------------                        */

#if CHECK_USAGE

#define ENTER(Root,RetValue) \
do \
{ \
  if (Root->fWatchUsage && Increment (&Root->Usage) <= 0) \
  { \
    Decrement (&Root->Usage); \
    return (RetValue); \
  } \
} \
while (0)

#define LEAVE(Root,RetValue) \
{ \
  if (Root->fWatchUsage) \
    Decrement (&Root->Usage); \
  return (RetValue); \
}

#define ENTER_STOP(Root,RetValue) \
do \
{ \
  if (Root->fWatchUsage && !CASLong (&Root->Usage, -0x7fffffff, 0) != 0) \
    return (RetValue); \
} \
while (0)

#else

#define ENTER(Root,RetValue)
#define LEAVE(Root,RetValue) return (RetValue)
#define ENTER_STOP(Root,RetValue)

#endif /* CHECK_USAGE */



/* -------------------- Lock-free non-blocking stack --------------------- */
/*                      ----------------------------                       */


#define MAGIC_STACK 'LfSt'

typedef struct STACK_NODE_T STACK_NODE;

//
// Attention: alignment of structure below as well as order of fields
// is EXTREMELY important performance-wise
//
typedef struct
{
  volatile LF   Free;   // list of free nodes (used always)
  volatile LF   Head;   // list of used nodes (used always)

  volatile LONG FreeCnt;// # of available free nodes
  HANDLE   FreeEvent;   // signal availability of free nodes

  volatile LONG UsedCnt;// # of available nodes in used list
  HANDLE   UsedEvent;   // signal availability of used nodes

  volatile LF   Block;  // list of allocated node blocks
  LONG   AllocUnits;    // allocate by AllocUnits at once
  volatile LONG MaxUnits;       // max # of nodes that we allowed to allocate

  volatile LONG Units;  // current number of units
  volatile LONG AllocatedSize;
  volatile LONG Magic;

#if CHECK_USAGE
  volatile LONG Usage;  // usage counter (if < 0 then stack is destroyed)
  BOOL         fWatchUsage; // watch usage
#endif

  PVOID     StackFree;  // address to release

  LFMEM    *pMem;       // user-defined allocator [if any]
} STACK;

struct STACK_NODE_T
{
  STACK_NODE *NodeNext;
  PVOID       Data;
};




static
PLFSTACK                // create and initialize new stack
LFAPI                   // return NULL if not enough memory
LFStackCreateBase (
  LONG  AllocUnits,     // allocate by AllocUnits at once
  LONG  MaxUnits,       // max stack depth (if 0 then no limits)
  BOOL  fWatchUsage,    // if TRUE then watch stack usage
  BOOL  fWaitForPush,   // if stack is empty then wait for push
  BOOL  fWaitForPop,    // if stack is full wait for pop
  LFMEM *pMem           // user-supplied allocator (if any)
)
{
  STACK        *Stack;
  STACK_NODE   *Node;
  PVOID         StackFree;
  volatile LONG AllocatedSize = 0;

  if ((Stack = LFAllocAligned (pMem, &StackFree, sizeof (*Stack) + sizeof (*Node) + (pMem == NULL ? 0 : sizeof (*pMem)), &AllocatedSize)) == NULL)
  {
    return (NULL);
  }
  memset (Stack, 0, sizeof (*Stack) + sizeof (*Node));
  Stack->StackFree = StackFree;
  Stack->AllocatedSize = AllocatedSize;
  Node = (STACK_NODE *) (Stack + 1);
  if (pMem == NULL)
    Stack->pMem = NULL;
  else
  {
    Stack->pMem = (LFMEM *) (Node + 1);
    *(Stack->pMem) = *pMem;
  }

#if CHECK_USAGE
  Stack->fWatchUsage = fWatchUsage;
#endif

  if (AllocUnits < MIN_ALLOC_UNITS)
    AllocUnits = MIN_ALLOC_UNITS;
  else if (AllocUnits > MAX_ALLOC_UNITS)
    AllocUnits = MAX_ALLOC_UNITS;

  if (MaxUnits >= (LONG) (0x7fffffff / sizeof (STACK_NODE)))
    MaxUnits = 0x7fffffff / sizeof (STACK_NODE) - 1;

  if (MaxUnits <= 0)
  {
    MaxUnits = -1;
    fWaitForPop = FALSE;
  }
  else if (MaxUnits != 0 && AllocUnits > MaxUnits)
    AllocUnits = MaxUnits;
  Stack->AllocUnits = AllocUnits;
  Stack->MaxUnits   = MaxUnits;

  if (fWaitForPush)
  {
    Stack->UsedEvent = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
    if (Stack->UsedEvent == NULL)
    {
      LFFree (Stack->pMem, StackFree, NULL);
      return (NULL);
    }
  }

  if (fWaitForPop)
  {
    Stack->FreeEvent = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
    if (Stack->FreeEvent == NULL)
    {
      if (Stack->UsedEvent != NULL)
        CloseHandle (Stack->UsedEvent);
      LFFree (Stack->pMem, StackFree, NULL);
      return (NULL);
    }
  }

  // add new node to the free list
  PUSH (Stack->Free, Node, NodeNext);
  Stack->Units = 1;
  if (Stack->FreeEvent != NULL)
    Stack->FreeCnt = 1;

  Stack->Magic = MAGIC_STACK;

  LF_DETECT_NUMBER_OF_CPUs();
  LFSerializeWrites ();

  return ((PLFSTACK) Stack);
}

PLFSTACK                // create and initialize new stack
LFAPI                   // return NULL if not enough memory
LFStackCreate (
  LONG  AllocUnits,     // allocate by AllocUnits at once
  LONG  MaxUnits,       // max stack depth (if 0 then no limits)
  BOOL  fWatchUsage,    // if TRUE then watch stack usage
  BOOL  fWaitForPush,   // if stack is empty then wait for push
  BOOL  fWaitForPop     // if stack is full wait for pop
)
{
  return (LFStackCreateBase (AllocUnits, MaxUnits, fWatchUsage, fWaitForPush, fWaitForPop, NULL));
}


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
)
{
  LFMEM Mem;
  if (!LFInitMem (&Mem, pMemoryContext, dwMemAlignment, pAllocCb, pFreeCb))
    return (NULL);
  return (LFStackCreateBase (AllocUnits, MaxDepth, fWatchUsage, fWaitForPush, fWaitForPop, &Mem));
}




BOOL                    // destroy the stack and release all the memory occupied
LFAPI                   // return FALSE if stack is in use
LFStackDestroy (
  PLFSTACK LFStack
)
{
  STACK *Stack = (STACK *) LFStack;
  volatile LONG AllocatedSize;

  // if not initialized properly then nothing to do
  if (Stack == NULL || InterlockedExchange ((PLONG) &Stack->Magic, 0) != MAGIC_STACK)
  {
    return (TRUE);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if is in use then cannot destroy
  ENTER_STOP (Stack, FALSE);

  // release events
  if (Stack->FreeEvent != NULL)
    CloseHandle (Stack->FreeEvent);
  if (Stack->UsedEvent != NULL)
    CloseHandle (Stack->UsedEvent);


  AllocatedSize = Stack->AllocatedSize;

  // release all nodes
  LFFreeBlocks (Stack->pMem, &Stack->Block, &AllocatedSize);

  // release stack itself
  LFFree (Stack->pMem, Stack->StackFree, &AllocatedSize);

  ASSERT (Stack->pMem != NULL || AllocatedSize == 0);

  LFSerializeWrites ();

  return (TRUE);
}


LONG                    // returns total amount of memory allocated by stack
LFAPI                   // or (-1) if stack was not properly initialized
LFStackTellMemory (
  PLFSTACK LFStack
)
{
  STACK *Stack = (STACK *) LFStack;

  // if not initialized properly then nothing to do
  if (Stack == NULL || Stack->Magic != MAGIC_STACK)
  {
    return (-1);
  }

  // read of aligned word is atomic
  return (Stack->AllocatedSize);
}


LONG                    // returns current depth of stack
LFAPI                   // or (-1) if stack was not properly initialized
LFStackTellNumberOfNodes (
  PLFSTACK LFStack
)
{
  STACK *Stack = (STACK *) LFStack;

  // if not initialized properly then nothing to do
  if (Stack == NULL || Stack->Magic != MAGIC_STACK)
  {
    return (-1);
  }

  // read of aligned word is atomic
  return (Stack->Units);
}


BOOL                    // increase number of stack nodes
LFAPI                   // returns TRUE if succeeded, FALSE otherwise
LFStackIncreaseNumberOfNodes (
  PLFSTACK LFStack,
  LONG     AllocUnits
)
{
  STACK *Stack;
  STACK_NODE *Node;
  LONG i, MaxUnits;

  if (AllocUnits <= 0 || (Stack = (STACK *) LFStack) == NULL || Stack->Magic != MAGIC_STACK || (ULONG) AllocUnits >= (ULONG) 0x7fffffff / sizeof (*Node))
  {
    return (FALSE);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  do
  {
    // get remaining number of nodes that we allowed to allocate
    MaxUnits = Stack->MaxUnits;
    if (MaxUnits < 0)
    {
      // no limitations -- allocate AllocUnits
      break;
    }
    if (MaxUnits == 0)
    {
      // reached allocation limit
      return (0);
    }
    if (AllocUnits > MaxUnits)
      AllocUnits = MaxUnits;
    // decrease current allocation limit by AllocUnits nodes we will allocate
  }
  while (!CASLong (&Stack->MaxUnits, MaxUnits - AllocUnits, MaxUnits));

  // no overflow: AllocUnits > 0 && AllocUnits * sizeof (*Node) < 2 GB

  // allocate block of nodes
  Node = LFAllocBlock (Stack->pMem, &Stack->Block, AllocUnits * sizeof (*Node), &Stack->AllocatedSize);

  if (Node == NULL)
  {
    // allocation failed
    // now a kind of theological question: we run out of memory, should
    // we decrease Stack->MaxUnits and try allocating again [very soon]
    // or just leave it as is? -- I think that once you run out of memory
    // nothing will save you -- be prepared to die, nothing matters anymore

    Stack->MaxUnits = 0;        // prevent from further allocations
    return (FALSE);
  }

  LFInterlockedAdd (&Stack->Units, AllocUnits);

  // insert all nodes except for first into free node list

  i     = AllocUnits;
  Node += AllocUnits;
  do
  {
    --Node;
    --i;
    // add new node to the free list
    PUSH (Stack->Free, Node, NodeNext);
  }
  while (i != 0);

  if (Stack->FreeEvent != NULL && (i = LFInterlockedAdd (&Stack->FreeCnt, AllocUnits)) < 0)
  {
    // somebody if waiting for free node(s) -- let them run
    // we just made AllocUnits available (please notice decrement above)
    i = -i;
    if (i > AllocUnits) i = AllocUnits;
    ReleaseSemaphore (Stack->FreeEvent, i, NULL);
  }

  return (TRUE);
}


LONG                    // returns stack max depth limit, (-1) if no depth limit, 0 if no enough memory
LFAPI
LFStackTellMaxDepth (
  PLFSTACK LFStack
)
{
  STACK *Stack = (STACK *) LFStack;
  LONG MaxUnits;

  // if not initialized properly then nothing to do
  if (Stack == NULL || Stack->Magic != MAGIC_STACK)
  {
    return (-1);
  }

  // read of aligned word is atomic
  MaxUnits = Stack->MaxUnits;
  if (MaxUnits > 0)
  {
    MaxUnits += Stack->Units;
    if (MaxUnits >= (LONG) (0x7fffffff / sizeof (STACK_NODE)))
      MaxUnits = 0x7fffffff / sizeof (STACK_NODE) - 1;
  }

  return (MaxUnits);
}


BOOL                    // increase max stack depth
LFAPI                   // returns FALSE if stack was not properly initialized
LFStackIncreaseMaxDepth (
  PLFSTACK LFStack,
  LONG     Increase
)
{
  STACK *Stack = (STACK *) LFStack;
  LONG   MaxUnits, NewMaxUnits;

  // if not initialized properly then nothing to do
  if (Stack == NULL || Stack->Magic != MAGIC_STACK || Increase < 0)
  {
    return (FALSE);
  }

  if (Increase == 0 || Stack->MaxUnits < 0)
  {
    // no increase or no limit
    return (TRUE);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  ENTER (Stack, FALSE);

  do
  {
    MaxUnits = Stack->MaxUnits;
    NewMaxUnits = MaxUnits + Increase;
    if (NewMaxUnits < MaxUnits || (ULONG) NewMaxUnits >= 0x7fffffff / sizeof (STACK_NODE))
    {
      // fix integer overflow
      MaxUnits = 0x7fffffff / sizeof (STACK_NODE) - 1;
    }
  }
  while (!CASLong (&Stack->MaxUnits, NewMaxUnits, MaxUnits));

  LEAVE (Stack, TRUE);
}

static                  // allocate several nodes at once and return one of them
STACK_NODE *
LFStackAllocNodes (
  STACK *Stack
)
{
  STACK_NODE *Node;
  LONG i, MaxUnits, AllocUnits;

  AllocUnits = Stack->AllocUnits;
  do
  {
    // get remaining number of nodes that we allowed to allocate
    MaxUnits = Stack->MaxUnits;
    if (MaxUnits < 0)
    {
      // no limitations -- allocate AllocUnits
      break;
    }
    if (MaxUnits == 0)
    {
      // reached allocation limit
      return (NULL);
    }
    if (AllocUnits > MaxUnits)
      AllocUnits = MaxUnits;
    // decrease current allocation limit by AllocUnits nodes we will allocate
  }
  while (!CASLong (&Stack->MaxUnits, MaxUnits - AllocUnits, MaxUnits));

  // no overflow: Stack->AllocUnits shall be >= 0 && Stack->AllocUnits * sizeof (*Node) < 2GB

  // allocate block of nodes
  Node = LFAllocBlock (Stack->pMem, &Stack->Block, AllocUnits * sizeof (*Node), &Stack->AllocatedSize);

  if (Node == NULL)
  {
    // allocation failed
    // now a kind of theological question: we run out of memory, should
    // we decrease Stack->MaxUnits and try allocating again [very soon]
    // or just leave it as is? -- I think that once you run out of memory
    // nothing will save you -- be prepared to die, nothing matters anymore

    Stack->MaxUnits = 0;        // prevent from further allocations
    return (NULL);
  }

  LFInterlockedAdd (&Stack->Units, AllocUnits);

  // insert all nodes except for first into free node list
  i = AllocUnits;
  Node += AllocUnits;
  while (--Node, --i)
  {
    // add new node to the free list
    PUSH (Stack->Free, Node, NodeNext);
  }

  if (Stack->FreeEvent != NULL && (i = LFInterlockedAdd (&Stack->FreeCnt, AllocUnits)) < -1 && --AllocUnits > 0)
  {
    // somebody if waiting for free node(s) -- let them run
    // we just made AllocUnits available (please notice decrement above)
    i = ~i; // ~i = -i-1 -- # of waiting threads less caller
    if (i > AllocUnits) i = AllocUnits;
    ReleaseSemaphore (Stack->FreeEvent, i, NULL);
  }

  // return last node
  return (Node);
}
    


BOOL                    // push pointer to user-defined data on top of the stack
LFAPI                   // returns FALSE if not enough memory (it rarely needs few bytes)
LFStackPush (           // or if stack was not properly initialized
  PLFSTACK LFStack,
  PVOID    Data
)
{
  STACK *Stack = (STACK *) LFStack;
  STACK_NODE *Node;

  // if not initialized properly or destroyed then nothing to do
  if (Data == NULL || Stack == NULL || Stack->Magic != MAGIC_STACK)
  {
    return (FALSE);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Stack, FALSE);

  if (Stack->FreeEvent == NULL)
  {
    // waiting not supported -- get node from free list
    POP (Stack->Free, Node, NodeNext);

    // if nothing is in free list then try to allocate
    if (Node == NULL && (Stack->MaxUnits == 0 || (Node = LFStackAllocNodes (Stack)) == NULL))
    {
      // cannot allocate more nodes
      LEAVE (Stack, FALSE);
    }
  }
  else
  {
    Node = NULL;
    if (Decrement (&Stack->FreeCnt) >= 0)
    {
      // supposedly we should have free nodes
      POP (Stack->Free, Node, NodeNext);
    }

    // if nothing is in free list then try to allocate
    if (Node == NULL && (Stack->MaxUnits == 0 || (Node = LFStackAllocNodes (Stack)) == NULL))
    {
      // cannot allocate more nodes
      Increment (&Stack->FreeCnt);
      LEAVE (Stack, FALSE);
    }
  }

  // push node
  Node->Data = Data;
  PUSH (Stack->Head, Node, NodeNext);

  // if somebody is waiting for push then let it run
  if (Stack->UsedEvent != NULL && Increment (&Stack->UsedCnt) <= 0)
  {
    ReleaseSemaphore (Stack->UsedEvent, 1, NULL);
  }

  LEAVE (Stack, TRUE);
}


DWORD                   // if memory usage limit reached and no available storage wait for pop
LFAPI                   // returns as of WaitForSingleObjectEx
LFStackPushWaitEx (
  PLFSTACK LFStack,     // stack
  PVOID    Data,        // result [not modified if wait is timed out]
  PVOID    pWaitContext,// user-defined wait callback function context
  LFWAIT  *pWaitCb,     // user-defined wait callback function
  BOOL     fWaitSpin,   // spin a bit waiting for push
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
)
{
  STACK *Stack = (STACK *) LFStack;
  STACK_NODE *Node;
  LF_SPIN_DECL ();

  // if not initialized properly or destroyed then nothing to do
  if (Data == NULL || Stack == NULL || Stack->Magic != MAGIC_STACK)
  {
    return (ERROR_INVALID_PARAMETER);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Stack, ERROR_INVALID_PARAMETER);

  if (Stack->FreeEvent == NULL)
  {
    // waiting is not supported -- get node from free list
    POP (Stack->Free, Node, NodeNext);

    // if nothing is in free list then try to allocate
    if (Node == NULL && (Stack->MaxUnits == 0 || (Node = LFStackAllocNodes (Stack)) == NULL))
    {
      // cannot allocate more nodes
      LEAVE (Stack, WAIT_TIMEOUT);
    }
  }
  else
  {
    Node = NULL;
    if (Decrement (&Stack->FreeCnt) >= 0)
    {
      // we should have free node
      POP (Stack->Free, Node, NodeNext);
    }
  
    // if nothing is in free list then try to allocate
    if (Node == NULL && (Stack->MaxUnits == 0 || (Node = LFStackAllocNodes (Stack)) == NULL))
    {
      // cannot allocate more nodes -- have to wait
#if SPIN_COUNT > 0
      if (LFCPUNum > 0 && fWaitSpin)
      {
        LONG SpinCount = SPIN_COUNT;
        do
        {
          LF_SPIN_PAUSE ();
          POP (Stack->Free, Node, NodeNext);
        }
        while (Node == NULL && --SpinCount != 0);
      }
#endif

      while (Node == NULL)
      {
        DWORD dwResult;

#if SPIN_COUNT > 0
        if (LFCPUNum > 0 && fWaitSpin)
          LF_SPIN_PAUSE ();
#endif
        
        dwResult = LF_WAIT (pWaitCb, pWaitContext, Stack->FreeEvent, dwMilliseconds, fAlertable);
        if (dwResult != WAIT_OBJECT_0)
        {
          // we are not waiting anymore
          Increment (&Stack->FreeCnt);
          LEAVE (Stack, dwResult);
        }

        // we got the event but it may be bogus event set to somebody
        // who timed out and did not pick it up
        POP (Stack->Free, Node, NodeNext);
      }
    }
  }

  // push the node
  Node->Data = Data;
  PUSH (Stack->Head, Node, NodeNext);

  // if somebody is waiting for push then let it run
  if (Stack->UsedEvent != NULL && Increment (&Stack->UsedCnt) <= 0)
  {
    ReleaseSemaphore (Stack->UsedEvent, 1, NULL);
  }

  LEAVE (Stack, WAIT_OBJECT_0);
}


DWORD                   // if memory usage limit reached and no available storage wait for pop
LFAPI                   // returns as of WaitForSingleObjectEx
LFStackPushWait (
  PLFSTACK LFStack,     // stack
  PVOID    Data,        // result [not modified if wait is timed out]
  BOOL     fWaitSpin,   // spin a bit waiting for push
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
)
{
  return (
    LFStackPushWaitEx (
      LFStack,
      Data,
      0,
      0,
      fWaitSpin,
      dwMilliseconds,
      fAlertable
    )
  );
}


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
)
{
  STACK *Stack = (STACK *) LFStack;
  STACK_NODE *Node;
  PVOID       Data;
  LF          HeadCopy;

  // if not initialized properly or destroyed then nothing to do
  if (Stack == NULL || Stack->Magic != MAGIC_STACK)
  {
    return (NULL);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Stack, NULL);

  do
  {
    Data = NULL;
    LFLoadPtr (HeadCopy, Stack->Head);
    Node = GET_POINTER (HeadCopy);
    if (Node == NULL)
      break;
    Data = Node->Data;
  }
  while (!SAME_POINTER (HeadCopy, Stack->Head));

  LEAVE (Stack, Data);
}

PVOID                   // pop top of the stack
LFAPI                   // returns NULL if stack is empty or not initialized
LFStackPop (
  PLFSTACK LFStack
)
{
  STACK *Stack = (STACK *) LFStack;
  STACK_NODE *Node;
  PVOID       Data;

  // if not initialized properly or destroyed then nothing to do
  if (Stack == NULL || Stack->Magic != MAGIC_STACK)
  {
    return (NULL);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Stack, NULL);

  // try to obtain head node
  if (Stack->UsedEvent == NULL)
  {
    // waiting is not supported -- try to get head now
    POP (Stack->Head, Node, NodeNext);
    if (Node == NULL)
    {
      LEAVE (Stack, NULL);
    }
  }
  else
  {
    Node = NULL;
    if (Decrement (&Stack->UsedCnt) >= 0)
    {
      // we should have something on the stack
      POP (Stack->Head, Node, NodeNext);
    }
    if (Node == NULL)
    {
      // we are not going to pop anything anymore
      Increment (&Stack->UsedCnt);
      LEAVE (Stack, NULL);
    }
  }

  // get data pointer now -- it will be spoiled after freeing
  Data = Node->Data;

  // push node into free list
  PUSH (Stack->Free, Node, NodeNext);

  // if somebody is waiting for free node let it run
  if (Stack->FreeEvent != NULL && Increment (&Stack->FreeCnt) <= 0)
  {
    ReleaseSemaphore (Stack->FreeEvent, 1, NULL);
  }

  LEAVE (Stack, Data);
}


DWORD                   // returns WAIT_TIMEOUT if queue is empty,
LFAPI                   // WAIT_OBJECT_0 if it's not, or
LFStackIsEmpty (        // ERROR_INVALID_PARAMETER if LFStack was not initialized properly or destroyed
  PLFSTACK LFStack      // queue
)
{
  STACK *Stack = (STACK *) LFStack;
  LF     HeadCopy;

  // if not initialized properly or destroyed then nothing to do
  if (Stack == NULL || Stack->Magic != MAGIC_STACK)
  {
    return (ERROR_INVALID_PARAMETER);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Stack, ERROR_INVALID_PARAMETER);

  // get pointer to first stack node (if such exist)
  LFLoadPtr (HeadCopy, Stack->Head);
  if (GET_POINTER (HeadCopy) == NULL)
  {
    // Stack was empty
    LEAVE (Stack, WAIT_TIMEOUT);
  }

  // Stack had something few cycles ago
  LEAVE (Stack, WAIT_OBJECT_0);
}


DWORD                   // wait for stack to be non-empty if necessary and pop top of the stack
LFAPI                   // returns as of WaitForSingleObjectEx
LFStackPopWaitEx (
  PLFSTACK LFStack,     // stack
  PVOID   *pData,       // result [not modified if wait is timed out]
  PVOID    pWaitContext,// user-defined wait callback function context
  LFWAIT  *pWaitCb,     // user-defined wait callback function
  BOOL     fWaitSpin,   // spin a bit waiting for push
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
)
{
  STACK *Stack = (STACK *) LFStack;
  STACK_NODE *Node;
  LF_SPIN_DECL ();

  // if not initialized properly or destroyed then nothing to do
  if (pData == NULL || Stack == NULL || Stack->Magic != MAGIC_STACK)
  {
    return (ERROR_INVALID_PARAMETER);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Stack, ERROR_INVALID_PARAMETER);

  if (Stack->UsedEvent == NULL)
  {
    // waiting is not supported -- get node immediately
    POP (Stack->Head, Node, NodeNext);
    if (Node == NULL)
    {
      LEAVE (Stack, WAIT_TIMEOUT);
    }
  }
  else
  {
    Node = NULL;
    if (Decrement (&Stack->UsedCnt) >= 0)
    {
      // we should have something
      POP (Stack->Head, Node, NodeNext);
    }

    if (Node == NULL)
    {
      // have to wait
#if SPIN_COUNT > 0
      if (LFCPUNum > 0 && fWaitSpin)
      {
        LONG SpinCount = SPIN_COUNT;
        do
        {
          LF_SPIN_PAUSE ();
          POP (Stack->Head, Node, NodeNext);
        }
        while (Node == NULL && --SpinCount != 0);
      }
#endif

      while (Node == NULL)
      {
        DWORD dwResult;

#if SPIN_COUNT > 0
        if (LFCPUNum > 0 && fWaitSpin)
          LF_SPIN_PAUSE ();
#endif
        
        dwResult = LF_WAIT (pWaitCb, pWaitContext, Stack->UsedEvent, dwMilliseconds, fAlertable);
        if (dwResult != WAIT_OBJECT_0)
        {
          // timed out -- return immediately
          Increment (&Stack->UsedCnt);
          LEAVE (Stack, dwResult);
        }

        // we got the event but it may be bogus event set to somebody
        // who timed out and did not pick it up
        POP (Stack->Head, Node, NodeNext);
      }
    }
  }

  // get data pointer now -- it will be spoiled after freeing
  *pData = Node->Data;

  // push node into free list
  PUSH (Stack->Free, Node, NodeNext);

  // if somebody is waiting for free node let it run
  if (Stack->FreeEvent != NULL && Increment (&Stack->FreeCnt) <= 0)
  {
    ReleaseSemaphore (Stack->FreeEvent, 1, NULL);
  }

  LEAVE (Stack, WAIT_OBJECT_0);
}


DWORD                   // wait for stack to be non-empty if necessary and pop top of the stack
LFAPI                   // returns as of WaitForSingleObjectEx
LFStackPopWait (
  PLFSTACK LFStack,     // stack
  PVOID   *pData,       // result [not modified if wait is timed out]
  BOOL     fWaitSpin,   // spin a bit waiting for push
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
)
{
  return (
    LFStackPopWaitEx (
      LFStack,
      pData,
      0,
      0,
      fWaitSpin,
      dwMilliseconds,
      fAlertable
    )
  );
}

#undef MAGIC_STACK



/* ------------------- Lock-free non-blocking queue ---------------------- */
/*                     ----------------------------                        */

#define MAGIC_QUEUE 'LfQu'


typedef struct
{
  union
  {
    volatile LF Link;   // link info
    LFMSG       GoodAlignmentFiller;
  };
  volatile LFMSG Msg;   // message
} QUEUE_NODE;

//
// Attention: alignment of structure below as well as order of fields
// is EXTREMELY important performance-wise
//
typedef struct
{
  volatile LF   Free;   // list of free nodes (used always)
  volatile LF   Tail;   // tail of the queue  (used always)
  volatile LF   Head;   // head of the queue  (dequeue only)

  volatile LONG FreeCnt;// # of available free nodes
  HANDLE   FreeEvent;   // signal availability of free nodes

  volatile LONG UsedCnt;// # of available nodes in used list
  HANDLE   UsedEvent;   // signal availability of used nodes

  volatile LF   Block;  // list of allocated node blocks
  LONG     AllocUnits;  // allocate by AllocUnits at once
  volatile LONG MaxUnits;       // max # of nodes that we allowed to allocate
  volatile LONG Units;  // current number of units

  volatile LONG AllocatedSize;
  volatile LONG Magic;

#if CHECK_USAGE
  volatile LONG Usage;  // usage counter (if < 0 then queue is destroyed)
  BOOL          fWatchUsage; // watch usage
#endif

  PVOID     QueueFree;  // address to release
  LFMEM    *pMem;       // user-defined memory allocator
} QUEUE;


static                  // allocate several nodes at once and return one of them
QUEUE_NODE *
LFQueueAllocNodes (
  QUEUE *Queue
)
{
  QUEUE_NODE *Node;
  LONG i, MaxUnits, AllocUnits;

  AllocUnits = Queue->AllocUnits;
  do
  {
    // get remaining number of nodes that we allowed to allocate
    MaxUnits = Queue->MaxUnits;
    if (MaxUnits < 0)
    {
      // no limitations -- allocate AllocUnits
      break;
    }
    if (MaxUnits == 0)
    {
      // reached allocation limit
      return (NULL);
    }
    if (AllocUnits > MaxUnits)
      AllocUnits = MaxUnits;
    // decrease current allocation limit by AllocUnits nodes we will allocate
  }
  while (!CASLong (&Queue->MaxUnits, MaxUnits - AllocUnits, MaxUnits));

  // no overflow: AllocUnits > 0 && AllocUnits * sizeof (*Node) < 2GB

  // allocate block of nodes
  Node = LFAllocBlock (Queue->pMem, &Queue->Block, AllocUnits * sizeof (*Node), &Queue->AllocatedSize);

  if (Node == NULL)
  {
    // allocation failed
    // now a kind of theological question: we run out of memory, should
    // we decrease Queue->MaxUnits and try allocating again [very soon]
    // or just leave it as is? -- I think that once you run out of memory
    // nothing will save you -- be prepared to die, nothing matters anymore

    Queue->MaxUnits = 0;        // prevent from further allocations
    return (NULL);
  }

  LFInterlockedAdd (&Queue->Units, AllocUnits);

  // insert all nodes except for first into free node list
  i = AllocUnits;
  Node += AllocUnits;
  while (--Node, --i)
  {
    // add new node to the free list
    PUSH (Queue->Free, Node, Msg.Data);
  }

  if (Queue->FreeEvent != NULL && (i = LFInterlockedAdd (&Queue->FreeCnt, AllocUnits)) < -1 && --AllocUnits > 0)
  {
    // somebody if waiting for free node(s) -- let them run
    // we just made AllocUnits available (please notice decrement above)
    i = ~i; // ~i = -i-1 -- # of waiting threads less caller
    if (i > AllocUnits) i = AllocUnits;
    ReleaseSemaphore (Queue->FreeEvent, i, NULL);
  }

  // return last node
  return (Node);
}

PLFQUEUE                // create new queue
LFAPI                   // returns NULL if not enough memory
LFQueueCreateBase (
  LONG  AllocUnits,     // allocate by AllocUnits at once
  LONG  MaxUnits,       // max queue length (if 0 then no limits)
  BOOL  fWatchUsage,    // if TRUE then watch queue usage
  BOOL  fWaitForPut,    // if queue is empty then wait for put
  BOOL  fWaitForGet,    // if queue is full wait for get
  LFMEM *pMem           // user-defined memory allocator
)
{
  QUEUE      *Queue;
  PVOID       QueueFree;
  QUEUE_NODE *Node;
  volatile LONG AllocatedSize = 0;

  if ((Queue = LFAllocAligned (pMem, &QueueFree, sizeof (*Queue) + (pMem == NULL ? 0 : sizeof (*pMem)), &AllocatedSize)) == NULL)
  {
    return (NULL);
  }
  memset (Queue, 0, sizeof (*Queue));
  Queue->QueueFree = QueueFree;
  Queue->AllocatedSize = AllocatedSize;
  if (pMem == NULL)
    Queue->pMem = NULL;
  else
  {
    Queue->pMem = (LFMEM *) (Queue + 1);
    *(Queue->pMem) = *pMem;
  }

#if CHECK_USAGE
  Queue->fWatchUsage = fWatchUsage;
#endif

  if (AllocUnits < MIN_ALLOC_UNITS)
    AllocUnits = MIN_ALLOC_UNITS;
  else if (AllocUnits > MAX_ALLOC_UNITS)
    AllocUnits = MAX_ALLOC_UNITS;

  if (MaxUnits >= (LONG) (0x7fffffff / sizeof (QUEUE_NODE)))
    MaxUnits = 0x7fffffff / sizeof (QUEUE_NODE) - 1;

  if (MaxUnits <= 0)
  {
    MaxUnits = -1;
    fWaitForGet = FALSE;
  }
  else if (MaxUnits != 0 && AllocUnits > MaxUnits)
    AllocUnits = MaxUnits;
  Queue->AllocUnits = AllocUnits;
  Queue->MaxUnits   = MaxUnits;

  if (fWaitForPut)
  {
    Queue->UsedEvent = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
    if (Queue->UsedEvent == NULL)
    {
      LFFree (Queue->pMem, QueueFree, NULL);
      return (NULL);
    }
  }

  if (fWaitForGet)
  {
    Queue->FreeEvent = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
    if (Queue->FreeEvent == NULL)
    {
      if (Queue->UsedEvent != NULL)
        CloseHandle (Queue->UsedEvent);
      LFFree (Queue->pMem, QueueFree, NULL);
      return (NULL);
    }
  }


  LF_DETECT_NUMBER_OF_CPUs();
  
  if ((Node = LFQueueAllocNodes (Queue)) == NULL)
  {
    if (Queue->UsedEvent != NULL)
      CloseHandle (Queue->UsedEvent);
    if (Queue->FreeEvent != NULL)
      CloseHandle (Queue->FreeEvent);
    LFFree (Queue->pMem, QueueFree, NULL);
    return (NULL);
  }
  if (Queue->FreeEvent != NULL)
  {
    // one node is NOT available
    Queue->FreeCnt -= 1;
  }

  SET_POINTER (Node->Link, 0, 0);

  SET_POINTER (Queue->Head, Node, 0);
  SET_POINTER (Queue->Tail, Node, 0);

  Queue->Magic = MAGIC_QUEUE;

  LFSerializeWrites ();

  return ((PLFQUEUE) Queue);
}


PLFQUEUE                // create new queue
LFAPI                   // returns NULL if not enough memory
LFQueueCreate (
  LONG  AllocUnits,     // allocate by AllocUnits at once
  LONG  MaxDepth,       // max queue length (if 0 then no limits)
  BOOL  fWatchUsage,    // if TRUE then watch queue usage
  BOOL  fWaitForPut,    // if queue is empty then wait for put
  BOOL  fWaitForGet     // if queue is full wait for get
)
{
  return (LFQueueCreateBase (AllocUnits, MaxDepth, fWatchUsage, fWaitForPut, fWaitForGet, NULL));
}


PLFQUEUE                // create new queue
LFAPI                   // returns NULL if not enough memory
LFQueueCreateEx (
  LONG  AllocUnits,     // allocate by AllocUnits at once
  LONG  MaxDepth,      // max queue length (if 0 then no limits)
  BOOL  fWatchUsage,    // if TRUE then watch queue usage
  BOOL  fWaitForPut,    // if queue is empty then wait for put
  BOOL  fWaitForGet,    // if queue is full wait for get
  PVOID pMemoryContext, // user-defined memory allocator context for pAllocCb and pFreeCb
  DWORD dwMemAlignment, // min. alignment guaranteed by memory allocator (must be power of 2)
  LFALLOC *pAllocCb,    // memory allocation callback function
  LFFREE  *pFreeCb      // release memory callback function
)
{
  LFMEM Mem;
  if (!LFInitMem (&Mem, pMemoryContext, dwMemAlignment, pAllocCb, pFreeCb))
    return (NULL);
  return (LFQueueCreateBase (AllocUnits, MaxDepth, fWatchUsage, fWaitForPut, fWaitForGet, &Mem));
}


BOOL            // free memory occupied by queue
LFAPI           // return FALSE if queue is in use
LFQueueDestroy (
  PLFQUEUE LFQueue
)
{
  QUEUE *Queue = (QUEUE *) LFQueue;
  volatile LONG AllocatedSize;

  // if not initialized properly then nothing to do
  if (Queue == NULL || InterlockedExchange ((PLONG) &Queue->Magic, 0) != MAGIC_QUEUE)
  {
    return (TRUE);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if is in use then cannot destroy
  ENTER_STOP (Queue, FALSE);

  // release events
  if (Queue->FreeEvent != NULL)
    CloseHandle (Queue->FreeEvent);
  if (Queue->UsedEvent != NULL)
    CloseHandle (Queue->UsedEvent);


  AllocatedSize = Queue->AllocatedSize;

  // release all nodes
  LFFreeBlocks (Queue->pMem, &Queue->Block, &AllocatedSize);

  // release queue itself
  LFFree (Queue->pMem, Queue->QueueFree, &AllocatedSize);

  ASSERT (Stack->pMem != NULL || AllocatedSize == 0);

  LFSerializeWrites ();

  return (TRUE);
}


LONG                    // returns total amount of memory allocated by queue
LFAPI                   // returns -1 if queue was not properly initialized
LFQueueTellMemory (
  PLFQUEUE LFQueue
)
{
  QUEUE *Queue = (QUEUE *) LFQueue;

  // if not initialized properly then nothing to do
  if (Queue == NULL || Queue->Magic != MAGIC_QUEUE)
  {
    return (-1);
  }

  return (Queue->AllocatedSize);
}


LONG                    // returns current length of queue
LFAPI                   // or (-1) if queue was not properly initialized
LFQueueTellNumberOfNodes (
  PLFQUEUE LFQueue
)
{
  QUEUE *Queue = (QUEUE *) LFQueue;

  // if not initialized properly then nothing to do
  if (Queue == NULL || Queue->Magic != MAGIC_QUEUE)
  {
    return (-1);
  }

  // read of aligned word is atomic
  return (Queue->Units);
}


BOOL                    // increase number of queue nodes
LFAPI                   // returns TRUE if succeeded, FALSE otherwise
LFQueueIncreaseNumberOfNodes (
  PLFQUEUE LFQueue,
  LONG     AllocUnits
)
{
  QUEUE *Queue;
  QUEUE_NODE *Node;
  LONG i, MaxUnits;

  if (AllocUnits <= 0 || (Queue = (QUEUE *) LFQueue) == NULL || Queue->Magic != MAGIC_QUEUE || (ULONG) AllocUnits >= 0x7fffffff / sizeof (*Node))
  {
    return (FALSE);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  do
  {
    // get remaining number of nodes that we allowed to allocate
    MaxUnits = Queue->MaxUnits;
    if (MaxUnits < 0)
    {
      // no limitations -- allocate AllocUnits
      break;
    }
    if (MaxUnits == 0)
    {
      // reached allocation limit
      return (FALSE);
    }
    if (AllocUnits > MaxUnits)
      AllocUnits = MaxUnits;
    // decrease current allocation limit by AllocUnits nodes we will allocate
  }
  while (!CASLong (&Queue->MaxUnits, MaxUnits - AllocUnits, MaxUnits));

  // no overflow: AllocUnits > 0 && AllocUnits * sizeof (*Node) < 2GB

  // allocate block of nodes
  Node = LFAllocBlock (Queue->pMem, &Queue->Block, AllocUnits * sizeof (*Node), &Queue->AllocatedSize);

  if (Node == NULL)
  {
    // allocation failed
    // now a kind of theological question: we run out of memory, should
    // we decrease Queue->MaxUnits and try allocating again [very soon]
    // or just leave it as is? -- I think that once you run out of memory
    // nothing will save you -- be prepared to die, nothing matters anymore

    Queue->MaxUnits = 0;        // prevent from further allocations
    return (FALSE);
  }

  LFInterlockedAdd (&Queue->Units, AllocUnits);

  // insert all nodes except for first into free node list

  i     = AllocUnits;
  Node += AllocUnits;
  do
  {
    --Node;
    --i;
    // add new node to the free list
    PUSH (Queue->Free, Node, Msg.Data);
  }
  while (i != 0);

  if (Queue->FreeEvent != NULL && (i = LFInterlockedAdd (&Queue->FreeCnt, AllocUnits)) < 0)
  {
    // somebody if waiting for free node(s) -- let them run
    // we just made AllocUnits available (please notice decrement above)
    i = -i;
    if (i > AllocUnits) i = AllocUnits;
    ReleaseSemaphore (Queue->FreeEvent, i, NULL);
  }

  return (TRUE);
}


LONG                    // returns max queue length, (-1) if no length limit, 0 if not enough memory
LFAPI
LFQueueTellMaxLength (
  PLFQUEUE LFQueue
)
{
  QUEUE *Queue = (QUEUE *) LFQueue;
  LONG MaxUnits;

  // if not initialized properly then nothing to do
  if (Queue == NULL || Queue->Magic != MAGIC_QUEUE)
  {
    return (-1);
  }

  // read of aligned word is atomic
  MaxUnits = Queue->MaxUnits;
  if (MaxUnits > 0)
  {
    MaxUnits += Queue->Units;
    if (MaxUnits >= (LONG) (0x7fffffff / sizeof (QUEUE_NODE)))
      MaxUnits = 0x7fffffff / sizeof (QUEUE_NODE) - 1;
  }

  return (MaxUnits);
}


BOOL                    // increase max queue length
LFAPI                   // returns FALSE if queue was not properly initialized
LFQueueIncreaseMaxLength (
  PLFQUEUE LFQueue,
  LONG     Increase
)
{
  QUEUE *Queue = (QUEUE *) LFQueue;
  LONG   MaxUnits, NewMaxUnits;

  // if not initialized properly then nothing to do
  if (Queue == NULL || Queue->Magic != MAGIC_QUEUE || Increase < 0)
  {
    return (FALSE);
  }

  if (Increase == 0 || Queue->MaxUnits < 0)
  {
    // no increase or no limit
    return (TRUE);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  ENTER (Queue, FALSE);

  do
  {
    MaxUnits = Queue->MaxUnits;
    NewMaxUnits = MaxUnits + Increase;
    if ((ULONG) NewMaxUnits >= 0x7fffffff / sizeof (QUEUE_NODE))
      NewMaxUnits = 0x7fffffff / sizeof (QUEUE_NODE) - 1;
  }
  while (!CASLong (&Queue->MaxUnits, NewMaxUnits, MaxUnits));

  LEAVE (Queue, TRUE);
}


BOOL            // enqueue pointer to user data
LFAPI           // returns FALSE if Data is NULL or queue wasn't properly initialized
LFQueuePut (
  PLFQUEUE LFQueue,
  LFMSG    Msg
)
{
  QUEUE      *Queue = (QUEUE *) LFQueue;
  QUEUE_NODE *Node;

  // if not initialized properly then nothing to do
  if (Queue == NULL || Queue->Magic != MAGIC_QUEUE)
  {
    return (FALSE);
  }

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Queue, FALSE);

  if (Queue->FreeEvent == NULL)
  {
    // waiting not supported -- get node from free list
    POP (Queue->Free, Node, Msg.Data);

    // if nothing is in free list then try to allocate
    if (Node == NULL && (Queue->MaxUnits == 0 || (Node = LFQueueAllocNodes (Queue)) == NULL))
    {
      // cannot allocate more nodes
      LEAVE (Queue, FALSE);
    }
  }
  else
  {
    Node = NULL;
    if (Decrement (&Queue->FreeCnt) >= 0)
    {
      // supposedly we should have free nodes
      POP (Queue->Free, Node, Msg.Data);
    }

    // if nothing is in free list then try to allocate
    if (Node == NULL && (Queue->MaxUnits == 0 || (Node = LFQueueAllocNodes (Queue)) == NULL))
    {
      // cannot allocate more nodes
      Increment (&Queue->FreeCnt);
      LEAVE (Queue, FALSE);
    }
  }

  // initialize node
  Node->Msg = Msg;
  SET_POINTER (Node->Link, 0, GET_COUNTER (Node->Link) + 1);

  // insert node after last queue element
  for (;;)
  {
    LF          TailCopy, LastCopy;
    QUEUE_NODE *LastNode;

    // copy current tail
    LFLoadPtr (TailCopy, Queue->Tail);

    // copy last element (queue list is never empty)
    LastNode = GET_POINTER (TailCopy); // != NULL
    LFLoadPtr (LastCopy, LastNode->Link);

    if (!SAME_POINTER (TailCopy, Queue->Tail))
    {
      // retry; pointers may be not coherent
      continue;
    }

    if (GET_POINTER (LastCopy) == NULL)
    {
      // it looks like it's really last element
      if (CASPtr (&LastNode->Link, Node, LastCopy))
      {
        // nobody changed last in between -- replace the Tail (if nobody else did it meanwhile)
        // other threads could adjust Tail already so if CAS fails it is OK
        CASPtr (&Queue->Tail, Node, TailCopy);
        break;
      }
    }
    else
    {
      // somebody already inserted last element but did not update Tail yet
      CASPtr (&Queue->Tail, GET_POINTER (LastCopy), TailCopy);
    }
  }

  // if there are threads waiting for put let one of them run
  if (Queue->UsedEvent != NULL && Increment (&Queue->UsedCnt) <= 0)
  {
    ReleaseSemaphore (Queue->UsedEvent, 1, NULL);
  }

  LEAVE (Queue, TRUE);
}

DWORD                   // if memory usage limit reached and no available storage wait for get
LFAPI                   // returns as of WaitForSingleObjectEx
LFQueuePutWaitEx (
  PLFQUEUE LFQueue,     // queue
  LFMSG    Msg,         // message to enqueue
  PVOID    pWaitContext,// user-defined wait callback function context
  LFWAIT  *pWaitCb,     // user-defined wait callback function
  BOOL     fWaitSpin,   // spin before waiting for get
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
)
{
  QUEUE *Queue = (QUEUE *) LFQueue;
  QUEUE_NODE *Node;

  // if not initialized properly or destroyed then nothing to do
  if (Queue == NULL || Queue->Magic != MAGIC_QUEUE)
  {
    return (ERROR_INVALID_PARAMETER);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Queue, ERROR_INVALID_PARAMETER);

  if (Queue->FreeEvent == NULL)
  {
    // waiting is not supported -- get node from free list
    POP (Queue->Free, Node, Msg.Data);

    // if nothing is in free list then try to allocate
    if (Node == NULL && (Queue->MaxUnits == 0 || (Node = LFQueueAllocNodes (Queue)) == NULL))
    {
      // cannot allocate more nodes
      LEAVE (Queue, WAIT_TIMEOUT);
    }
  }
  else
  {
    LF_SPIN_DECL ();

    Node = NULL;
    if (Decrement (&Queue->FreeCnt) >= 0)
    {
      // we should have free node
      POP (Queue->Free, Node, Msg.Data);
    }

    // if nothing is in free list then try to allocate
    if (Node == NULL && (Queue->MaxUnits == 0 || (Node = LFQueueAllocNodes (Queue)) == NULL))
    {
      // cannot allocate more nodes -- have to wait
#if SPIN_COUNT > 0
      if (LFCPUNum > 0 && fWaitSpin)
      {
        LONG SpinCount = SPIN_COUNT;
        do
        {
          LF_SPIN_PAUSE ();
          POP (Queue->Free, Node, Msg.Data);
        }
        while (Node == NULL && --SpinCount != 0);
      }
#endif

      while (Node == NULL)
      {
        DWORD dwResult;

#if SPIN_COUNT > 0
        if (LFCPUNum > 0 && fWaitSpin)
          LF_SPIN_PAUSE ();
#endif
          
        dwResult = LF_WAIT (pWaitCb, pWaitContext, Queue->FreeEvent, dwMilliseconds, fAlertable);
        if (dwResult != WAIT_OBJECT_0)
        {
          // we are not waiting anymore
          Increment (&Queue->FreeCnt);
          LEAVE (Queue, dwResult);
        }

        // we got the event but it may be bogus event set to somebody
        // who timed out and did not pick it up
        POP (Queue->Free, Node, Msg.Data);
      }
    }
  }

  // initialize node
  Node->Msg = Msg;
  SET_POINTER (Node->Link, 0, GET_COUNTER (Node->Link) + 1);

  // insert node after last queue element
  for (;;)
  {
    LF          TailCopy, LastCopy;
    QUEUE_NODE *LastNode;

    // copy current tail
    LFLoadPtr (TailCopy, Queue->Tail);

    // copy last element (queue list is never empty)
    LastNode = GET_POINTER (TailCopy); // != NULL
    LFLoadPtr (LastCopy, LastNode->Link);

    if (!SAME_POINTER (TailCopy, Queue->Tail))
    {
      // retry; pointers may be not coherent
      continue;
    }

    if (GET_POINTER (LastCopy) == NULL)
    {
      // it looks like it's really last element
      if (CASPtr (&LastNode->Link, Node, LastCopy))
      {
        // nobody changed last in between -- replace the Tail (if nobody else did it meanwhile)
        // other threads could adjust Tail already so if CAS fails it is OK
        CASPtr (&Queue->Tail, Node, TailCopy);
        break;
      }
    }
    else
    {
      // somebody already inserted last element but did not update Tail yet
      CASPtr (&Queue->Tail, GET_POINTER (LastCopy), TailCopy);
    }
  }

  // if there are threads waiting for put let one of them run
  if (Queue->UsedEvent != NULL && Increment (&Queue->UsedCnt) <= 0)
  {
    ReleaseSemaphore (Queue->UsedEvent, 1, NULL);
  }

  LEAVE (Queue, WAIT_OBJECT_0);
}


DWORD                   // if memory usage limit reached and no available storage wait for get
LFAPI                   // returns as of WaitForSingleObjectEx
LFQueuePutWait (
  PLFQUEUE LFQueue,     // queue
  LFMSG    Msg,         // message to enqueue
  BOOL     fWaitSpin,   // spin before waiting for get
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
)
{
  return (
    LFQueuePutWaitEx (
      LFQueue,
      Msg,
      0,
      0,
      fWaitSpin,
      dwMilliseconds,
      fAlertable
    )
  );
}


static
DWORD
QueueGet (
  QUEUE *Queue,
  PLFMSG LFMsg
)
{
  QUEUE_NODE *Node;

  for (;;)
  {
    LF          HeadCopy, TailCopy;
    QUEUE_NODE *NextNode;
  
    // copy current head and tail (order of loads is important)
    LFLoadPtr (HeadCopy, Queue->Head);
    LFLoadPtr (TailCopy, Queue->Tail);

    // obtain pointer to second element (queue is never empty so it exist)
    Node = GET_POINTER (HeadCopy); // != NULL
    NextNode = GET_POINTER (Node->Link);

    if (!SAME_POINTER (HeadCopy, Queue->Head))
    {
      // something was dequeued, pointers coherence is not guaranteed -- retry
      continue;
    }
    // pointers are coherent

    if (Node == GET_POINTER (TailCopy))
    {
      // either only one element in queue or Tail wasn't adjusted yet
      if (NextNode == NULL)
      {
        // queue is empty
        return (WAIT_TIMEOUT);
      }

      // adjust Tail ourselves and retry
      CASPtr (&Queue->Tail, NextNode, TailCopy);
    }
    else
    {
      // obtain pointer to data -- it may spoil after head replacement
      LFMSG Msg = NextNode->Msg;

      // try to shift current head further
      if (CASPtr (&Queue->Head, NextNode, HeadCopy))
      {
        // succeeded -- release previous head and return
  
        *LFMsg = Msg;
        break;
      }
      // state changed in between -- retry
    }
  }

  PUSH (Queue->Free, Node, Msg.Data);

  // if there are threads waiting for free nodes let one of them run
  if (Queue->FreeEvent != NULL && Increment (&Queue->FreeCnt) <= 0)
  {
    ReleaseSemaphore (Queue->FreeEvent, 1, NULL);
  }

  return (WAIT_OBJECT_0);
}


DWORD                   // returns WAIT_TIMEOUT if queue is empty,
LFAPI                   // WAIT_OBJECT_0 if it's not, or
LFQueueIsEmpty (        // ERROR_INVALID_PARAMETER if LFQueue was not initialized properly or destroyed
  PLFQUEUE LFQueue      // queue
)
{
  QUEUE *Queue = (QUEUE *) LFQueue;

  // if not initialized properly then nothing to do
  if (Queue == NULL || Queue->Magic != MAGIC_QUEUE)
  {
    return (ERROR_INVALID_PARAMETER);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Queue, ERROR_INVALID_PARAMETER);

  for (;;)
  {
    LF          HeadCopy, TailCopy;
    QUEUE_NODE *Node, *NextNode;

    // copy current head and tail (order of loads is important)
    LFLoadPtr (HeadCopy, Queue->Head);
    LFLoadPtr (TailCopy, Queue->Tail);

    // obtain pointer to second element (queue is never empty so it exist)
    Node = GET_POINTER (HeadCopy); // != NULL
    NextNode = GET_POINTER (Node->Link);

    if (!SAME_POINTER (HeadCopy, Queue->Head))
    {
      // something was dequeued, pointers coherence is not guaranteed -- retry
      continue;
    }
    // pointers are coherent

    if (Node != GET_POINTER (TailCopy))
    {
      // queue is not empty
      break;
    }

    // either only one element in queue or Tail wasn't adjusted yet
    if (NextNode == NULL)
    {
      // queue is empty
      LEAVE (Queue, WAIT_TIMEOUT);
    }

    // adjust Tail ourselves and retry
    CASPtr (&Queue->Tail, NextNode, TailCopy);
  }

  // Queue wasn't empty few cycles ago
  LEAVE (Queue, WAIT_OBJECT_0);
}


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
)
{
  QUEUE *Queue = (QUEUE *) LFQueue;

  // if not initialized properly then nothing to do
  if (Queue == NULL || Queue->Magic != MAGIC_QUEUE)
  {
    return (ERROR_INVALID_PARAMETER);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Queue, ERROR_INVALID_PARAMETER);


  for (;;)
  {
    LF          HeadCopy, TailCopy;
    QUEUE_NODE *NextNode;
    QUEUE_NODE *Node;
  
    // copy current head and tail (order of loads is important)
    LFLoadPtr (HeadCopy, Queue->Head);
    LFLoadPtr (TailCopy, Queue->Tail);

    // obtain pointer to second element (queue is never empty so it exist)
    Node = GET_POINTER (HeadCopy); // != NULL
    NextNode = GET_POINTER (Node->Link);

    if (!SAME_POINTER (HeadCopy, Queue->Head))
    {
      // something was dequeued, pointers coherence is not guaranteed -- retry
      continue;
    }
    // pointers are coherent

    if (Node == GET_POINTER (TailCopy))
    {
      // either only one element in queue or Tail wasn't adjusted yet
      if (NextNode == NULL)
      {
        // queue is empty
        return (WAIT_TIMEOUT);
      }

      // adjust Tail ourselves and retry
      CASPtr (&Queue->Tail, NextNode, TailCopy);
    }
    else
    {
      // obtain pointer to data -- it may spoil after head replacement
      LFMSG Msg = NextNode->Msg;

      // try to shift current head further
      if (SAME_POINTER (HeadCopy, Queue->Head))
      {
        // nobody dequeued anything in between -- got first node  
        *LFMsg = Msg;
        break;
      }
      // state changed in between -- retry
    }
  }

  LEAVE (Queue, WAIT_OBJECT_0);
}



DWORD           // dequeue and return pointer to user data
LFAPI           // returns WAIT_OBJECT_0 if got the element, otherwise returns WAIT_TIMEOUT
LFQueueGet (
  PLFQUEUE LFQueue,
  PLFMSG   LFMsg
)
{
  QUEUE *Queue = (QUEUE *) LFQueue;
  DWORD  dwResult;

  // if not initialized properly then nothing to do
  if (LFMsg == NULL || Queue == NULL || Queue->Magic != MAGIC_QUEUE)
  {
    return (ERROR_INVALID_PARAMETER);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Queue, ERROR_INVALID_PARAMETER);

  if (Queue->UsedEvent == NULL)
  {
    // waiting is not supported -- try to get head now
    dwResult = QueueGet (Queue, LFMsg);
  }
  else
  {
    dwResult = WAIT_TIMEOUT;
    if (Decrement (&Queue->UsedCnt) >= 0)
    {
      // we should have something in the queue
      dwResult = QueueGet (Queue, LFMsg);
    }
    if (dwResult == WAIT_TIMEOUT)
    {
      // we didn't get anything and are not going to get anything anymore
      Increment (&Queue->UsedCnt);
    }
  }
 
  LEAVE (Queue, dwResult);
}


DWORD                   // dequeue next element and wait if necessary
LFAPI                   // returns as of WaitForSingleObjectEx
LFQueueGetWaitEx (      // [no waiting will be performed if queue was created with fWait == FALSE]
  PLFQUEUE LFQueue,     // queue
  PLFMSG   LFMsg,       // message to retrieve
  PVOID    pWaitContext,// user-defined wait callback function context
  LFWAIT  *pWaitCb,     // user-defined wait callback function
  BOOL     fWaitSpin,   // spin a bit waiting for put
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
)
{
  QUEUE *Queue = (QUEUE *) LFQueue;
  DWORD  dwResult;
  LF_SPIN_DECL ();

  // if not initialized properly or destroyed then nothing to do
  if (LFMsg == NULL || Queue == NULL || Queue->Magic != MAGIC_QUEUE)
  {
    return (ERROR_INVALID_PARAMETER);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // if will be destroyed very soon don't do anything, otherwise increase usage count
  ENTER (Queue, ERROR_INVALID_PARAMETER);

  if (Queue->UsedEvent == NULL)
  {
    // waiting is not supported -- get node immediately
    dwResult = QueueGet (Queue, LFMsg);
  }
  else
  {
    dwResult = WAIT_TIMEOUT;
    if (Decrement (&Queue->UsedCnt) >= 0)
    {
      // we should have something
      dwResult = QueueGet (Queue, LFMsg);
    }

    if (dwResult == WAIT_TIMEOUT)
    {
      // have to wait
#if SPIN_COUNT > 0
      if (LFCPUNum > 0 && fWaitSpin)
      {
        LONG SpinCount = SPIN_COUNT;
        do
        {
          LF_SPIN_PAUSE ();
          dwResult = QueueGet (Queue, LFMsg);
        }
        while (dwResult == WAIT_TIMEOUT && --SpinCount != 0);
      }
#endif

      while (dwResult == WAIT_TIMEOUT)
      {
#if SPIN_COUNT > 0
        if (LFCPUNum > 0 && fWaitSpin)
          LF_SPIN_PAUSE ();
#endif
        
        dwResult = LF_WAIT (pWaitCb, pWaitContext, Queue->UsedEvent, dwMilliseconds, fAlertable);
        if (dwResult != WAIT_OBJECT_0)
        {
          // timed out -- return immediately
          Increment (&Queue->UsedCnt);
          LEAVE (Queue, dwResult);
        }

        // we got the event but it may be bogus event set to somebody
        // who timed out and did not pick it up
        dwResult = QueueGet (Queue, LFMsg);
      }
    }
  }

  LEAVE (Queue, dwResult);
}


DWORD                   // dequeue next element and wait if necessary
LFAPI                   // returns as of WaitForSingleObjectEx
LFQueueGetWait (        // [no waiting will be performed if queue was created with fWait == FALSE]
  PLFQUEUE LFQueue,     // queue
  PLFMSG   LFMsg,       // message to retrieve
  BOOL     fWaitSpin,   // spin a bit waiting for put
  DWORD    dwMilliseconds,      // timeout
  BOOL     fAlertable   // if TRUE then allow I/O completion routines and APC calls
)
{
  return (
    LFQueueGetWaitEx (
      LFQueue,
      LFMsg,
      0,
      0,
      fWaitSpin,
      dwMilliseconds,
      fAlertable
    )
  );
}

/* ---------------------- Fast exclusive locks ------------------------ */
/*                        --------------------                          */

#define MAGIC_EXLOCK    'LfEx'

#define EXLOCK_RELEASED 1
#define EXLOCK_DISABLED 2
#define EXLOCK_SPIN     4
#define EXLOCK_WAITING  8


typedef struct
{
  volatile LONG State;


  HANDLE Event;
  // event by default is OFF and turns ON only when State goes to released
  // state and there are waiting threads
  // however event may be erroneousely left ON if wait for event timed out,
  // then another thread holding the lock released it and set the event,
  // and then thread waiting for lock decreased number of waiting threads
  // and did not wait for the event more.

  LONG  Flag;   // 1 -- recursion allowed, 0 -- watch thread, -1 -- nothing
  volatile DWORD dwThreadID;    // owner thread id
  DWORD  dwRecursion;   // recursion count

  PVOID  exlockFree;    // address to release
  LFMEM *pMem;          // user-defined memory allocator
  int    Magic;         // magic
} EXLOCK;



PLFEXLOCK               // create exclusive lock
LFAPI                   // returns NULL if failed
LFExLockCreateBase (
  BOOL fWatchThread,    // if TRUE then release MUST be from the same thread that acquired the lock
  BOOL fAllowRecursion, // if TRUE then allow recursive lock aquiring from the same thread
  LFMEM *pMem           // user-defined memory allocator
)
{
  EXLOCK *Lock;
  PVOID exlockFree;

  Lock = LFAllocAligned (pMem, &exlockFree, sizeof (*Lock) + (pMem == NULL ? 0 : sizeof (*pMem)), NULL);
  if (Lock == NULL)
    return (NULL);
  memset (Lock, 0, sizeof (*Lock));
  Lock->exlockFree = exlockFree;
  if (pMem == NULL)
    Lock->pMem = NULL;
  else
  {
    Lock->pMem = (LFMEM *) (Lock + 1);
    *(Lock->pMem) = *pMem;
  }

  Lock->Event = CreateEvent (NULL, FALSE, FALSE, NULL);
  if (Lock->Event == NULL)
  {
    LFFree (Lock->pMem, exlockFree, NULL);
    return (NULL);
  }

  Lock->Flag = -1;
  if (fAllowRecursion)
    Lock->Flag = 1;
  else if (fWatchThread)
    Lock->Flag = 0;
  Lock->State = EXLOCK_RELEASED;
  Lock->dwThreadID = THREAD_ID_NONE;

  Lock->Magic = MAGIC_EXLOCK;

  LF_DETECT_NUMBER_OF_CPUs();
  LFSerializeWrites ();

  return ((PLFEXLOCK) Lock);
}

PLFEXLOCK               // create exclusive lock
LFAPI                   // returns NULL if failed
LFExLockCreate (
  BOOL fWatchThread,    // if TRUE then release MUST be from the same thread that acquired the lock
  BOOL fAllowRecursion  // if TRUE then allow recursive lock aquiring from the same thread
)
{
  return (LFExLockCreateBase (fWatchThread, fAllowRecursion, NULL));
}

PLFEXLOCK               // create exclusive lock
LFAPI                   // returns NULL if failed
LFExLockCreateEx (
  BOOL fWatchThread,    // if TRUE then release MUST be from the same thread that acquired the lock
  BOOL fAllowRecursion, // if TRUE then allow recursive lock aquiring from the same thread
  PVOID pMemoryContext, // user-defined memory allocator context for pAllocCb and pFreeCb
  DWORD dwMemAlignment, // min. alignment guaranteed by memory allocator (must be power of 2)
  LFALLOC *pAllocCb,    // memory allocation callback function
  LFFREE  *pFreeCb      // release memory callback function
)
{
  LFMEM Mem;
  if (!LFInitMem (&Mem, pMemoryContext, dwMemAlignment, pAllocCb, pFreeCb))
    return (NULL);
  return (LFExLockCreateBase (fWatchThread, fAllowRecursion, &Mem));
}


#pragma warning (push)
#pragma warning (disable: 4701)

DWORD                   // acquire exclusive lock
LFAPI                   // return result is as of WaitForSingleObjectEx
LFExLockAcquireEx (
  PLFEXLOCK ExLock,      // lock
  PVOID     pWaitContext,// user-defined wait callback function context
  LFWAIT   *pWaitCb,     // user-defined wait callback function (if NULL WaitForSingleObjectEx will be called)
  BOOL      fWaitSpin,   // if TRUE and MP then waiting threads will spin first waiting for lock
  DWORD     dwMilliseconds, // timeout in milliseconds
  BOOL      fAlertable   // if TRUE then allow I/O completion routines calls and/or APC while waiting
)
{
  EXLOCK *Lock;
  LONG    State;
  DWORD   dwResult;
  DWORD   dwThreadID;
  LF_SPIN_DECL ();

  if ((Lock = (EXLOCK *) ExLock) == NULL || Lock->Magic != MAGIC_EXLOCK)
    return (ERROR_INVALID_PARAMETER);

  LF_DETECT_NUMBER_OF_CPUs();

  dwThreadID = THREAD_ID_NONE;
  if (Lock->Flag >= 0)
  {
    dwThreadID = GetCurrentThreadId ();
    if (Lock->dwThreadID == dwThreadID)
    {
      // recursive acquire
      ASSERT ((Lock->State & EXLOCK_RELEASED) == 0);

      if (Lock->Flag == 0)
      {
        // recursive acquire is not allowed
        return (ERROR_LF_LOCK_RECURSE);
      }
      Lock->dwRecursion += 1;
      return (WAIT_OBJECT_0);
    }
  }

  for (;;)
  {
    State = Lock->State;

    if ((State & EXLOCK_RELEASED) && ((State & EXLOCK_SPIN) || State == EXLOCK_RELEASED))
    {
      // lock is released -- get it now
#if SPIN_COUNT > 0
      if (fWaitSpin)
      {
        // get the lock and allow waiting threads to spin
        if (CASLong (&Lock->State, (State | EXLOCK_SPIN) - EXLOCK_RELEASED, State))
          goto LockObtained;
        if (LFCPUNum > 0)
          LF_SPIN_PAUSE ();
      }
      else if (CASLong (&Lock->State, (State & ~EXLOCK_SPIN) - EXLOCK_RELEASED, State))
        goto LockObtained;
#else
      if (CASLong (&Lock->State, State - EXLOCK_RELEASED, State))
        goto LockObtained;
#endif /* SPIN_COUNT > 0 */
      // state changed in between -- retry
      continue;
    }

#if CHECK_LOCK_USAGE
    if (State & EXLOCK_DISABLED)
    {
      // lock is disabled
      return (ERROR_INVALID_PARAMETER);
    }
#endif /* CHECK_LOCK_USAGE */

#if SPIN_COUNT > 0
    if (LFCPUNum > 0 && ((State & EXLOCK_SPIN) | dwMilliseconds) == 0)
    {
      return (WAIT_TIMEOUT);
    }
#endif /* SPIN_COUNT > 0 */

    // register as one more waiting thread
    if (!CASLong (&Lock->State, State + EXLOCK_WAITING, State))
    {
      // state changed in between -- retry
      continue;
    }
    break;
  }

  // registered as waiting for lock

#if SPIN_COUNT > 0
  if (LFCPUNum > 0 && (State & EXLOCK_SPIN) != 0)
  {
    // multi-processor machine -- spin
    int SpinCount = SPIN_COUNT;
    do
    {
      LF_SPIN_PAUSE ();
      State = Lock->State;
      //
      // spinning is very agressive, rude, impolite and dangerous practice
      // because it gets resources out of order ignoring everybody else;
      // also, it wastes CPU resources, overloads the bus, causes writer
      // starvation, provicates race, and reduces natural computer immunity
      // to these strange and unpredictable human beings
      //
      if (State & EXLOCK_RELEASED)
      {
        // lock is released -- get it now
        if (fWaitSpin)
        {
          if (CASLong (&Lock->State, (State | EXLOCK_SPIN) - EXLOCK_WAITING - EXLOCK_RELEASED, State))
            goto LockObtained;
        }
        else if (CASLong (&Lock->State, (State & ~EXLOCK_SPIN) - EXLOCK_WAITING - EXLOCK_RELEASED, State))
          goto LockObtained;
      }
    }
    while (--SpinCount && (State & EXLOCK_SPIN) != 0);
  }
#endif /* SPIN_COUNT > 0 */

  // wait for event
  for (;;)
  {
    // wait for event
    dwResult = LF_WAIT (pWaitCb, pWaitContext, Lock->Event, dwMilliseconds, fAlertable);
    if (dwResult != WAIT_OBJECT_0)
    {
      // wait failed -- reduce number of waiting threads
      do
      {
        State = Lock->State;
      }
      while (!CASLong (&Lock->State, State - EXLOCK_WAITING, State));
      return (dwResult);
    }

    //
    // we got the event but it does not necessarily mean that
    // lock is released -- it is possible that one thread timed out
    // waiting for event while another thread was setting the event
    // so waiting thread did not get the event and left it in signalled
    // state. Later lock was obtained, we started to wait for it to release
    // and got the event that was set for somebody else long time ago
    //

    for (;;)
    {
      State = Lock->State;
      if ((State & EXLOCK_RELEASED) == 0)
      {
        // bogus event -- wait more
        break;
      }
      // lock is released -- get it now
#if SPIN_COUNT > 0
      if (fWaitSpin)
      {
        if (CASLong (&Lock->State, (State | EXLOCK_SPIN) - EXLOCK_WAITING - EXLOCK_RELEASED, State))
          goto LockObtained;
        if (LFCPUNum > 0)
          LF_SPIN_PAUSE ();
      }
      else if (CASLong (&Lock->State, (State & ~EXLOCK_SPIN) - EXLOCK_WAITING - EXLOCK_RELEASED, State))
        goto LockObtained;
#else
      if (CASLong (&Lock->State, State - EXLOCK_WAITING - EXLOCK_RELEASED, State))
        goto LockObtained;
#endif /* SPIN_COUNT > 0 */
      // state changed in between -- retry
    }
  }

LockObtained:
  Lock->dwThreadID = dwThreadID;
  return (WAIT_OBJECT_0);
}

#pragma warning (pop)

DWORD                   // acquire exclusive lock
LFAPI                   // return result is as of WaitForSingleObjectEx
LFExLockAcquire (
  PLFEXLOCK ExLock,     // lock
  BOOL      fWaitSpin,  // if TRUE and MP then waiting threads will spin first waiting for lock
  DWORD dwMilliseconds, // timeout in milliseconds
  BOOL      fAlertable  // if TRUE then allow I/O completion routines calls and/or APC while waiting
)
{
  return (
    LFExLockAcquireEx (
      ExLock,
      0,
      0,
      fWaitSpin,
      dwMilliseconds,
      fAlertable
    )
  );
}

DWORD                   // release exclusive lock
LFAPI                   // return result is as of SetEvent/GetLastError
LFExLockRelease (       // also may return ERROR_SEM_IS_SET if lock is owned by
  PLFEXLOCK ExLock,     // another thread and lock was created with fWatchThread
  BOOL fSwitchThreads   // if TRUE and there are waiting threads then force thread switch
)
{
  EXLOCK *Lock;
  LONG State;

  if ((Lock = (EXLOCK *) ExLock) == NULL || Lock->Magic != MAGIC_EXLOCK)
    return (ERROR_INVALID_PARAMETER);

  LF_DETECT_NUMBER_OF_CPUs();

  if (Lock->Flag >= 0)
  {
    if (Lock->dwThreadID != GetCurrentThreadId ())
    {
      // lock belongs to another thread -- bogus unlock
      return (ERROR_LF_UNLOCK_BOGUS);
    }
    ASSERT ((Lock->State & EXLOCK_RELEASED) == 0);

    if (Lock->dwRecursion != 0)
    {
      // there were recursive calls -- reduce recursion level
      // if Lock->Flag == 0 (fWatchThread only) then dwRecursion == 0
      Lock->dwRecursion -= 1;
      return (ERROR_SUCCESS);
    }

    // out of recursion, good unlock; clear locking thread ID
    Lock->dwThreadID = THREAD_ID_NONE;
  }

  do
  {
    State = Lock->State;
    if (State & EXLOCK_RELEASED)
    {
      // lock already released -- bogus unlock
      return (ERROR_LF_UNLOCK_BOGUS);
    }
  } while (!CASLong (&Lock->State, State + EXLOCK_RELEASED, State));

  if (State >= EXLOCK_WAITING)
  {
    // we have waiting threads -- let them run
    SetEvent (Lock->Event);

    // make sure that they got the point
    if (fSwitchThreads)
    {
      State = Lock->State;
      if ((State & EXLOCK_RELEASED) && State >= EXLOCK_WAITING)
      {
        // force thread switch and avoid overallocation of CPU to this thread
        Sleep (1);
      }
    }
  }

  // lock released
  return (ERROR_SUCCESS);
}

BOOL                    // destroy previousely created lock
LFAPI                   // returns FALSE if lock is in use, otherwise as CloseHandle;
LFExLockDestroy (
  PLFEXLOCK ExLock
)
{
  EXLOCK *Lock;
  LONG State;
  BOOL fResult;

  if ((Lock = (EXLOCK *) ExLock) == NULL || InterlockedExchange ((PLONG) &Lock->Magic, 0) != MAGIC_EXLOCK)
    return (FALSE);

  LF_DETECT_NUMBER_OF_CPUs();
  
  do
  {
    State = Lock->State;
    if ((State & ~EXLOCK_SPIN) != EXLOCK_RELEASED)
    {
      return (FALSE);
    }
  }
  while (!CASLong (&Lock->State, EXLOCK_DISABLED, State));

  fResult = CloseHandle (Lock->Event);
  LFFree (Lock->pMem, Lock->exlockFree, NULL);
  LFSerializeWrites ();

  return (fResult);
}


/* --------------------------- Fast locks ----------------------------- */
/*                             ----------                               */

#define MAGIC_LOCK      'LfLk'

typedef struct LOCK_T LOCK;
struct LOCK_T
{
  volatile LF State;            // current lock state
  LONG  Flag;   // 1 -- allow recursive aquires, 0 -- watch thread, -1 -- nothing
  volatile DWORD dwThreadID;    // ID of thread holding exclusive lock

  HANDLE ReadersEvent;          // event for readers
  HANDLE WriterEvent;           // event for writers

  volatile LONG Magic;          // magic

  DWORD  dwRecursion;           // recursion count

  PVOID  lockFree;              // address to release
  LFMEM *pMem;                  // user-defined memory allocator
};

#define LF_LOCK_MASK    (~(LF_LOCK_TRY))

#define LF_ACTION_EQ(dwAction,LF_LOCK) ((((dwAction) - (LF_LOCK)) & LF_LOCK_MASK) == 0)


DWORD                   // acquire a lock
LFAPI                   // returns FALSE if lock was not properly initialized
LFLockAcquireEx (
  PLFLOCK LFLock,       // lock
  DWORD   dwAction,     // one of LF_LOCK_* actions
  PVOID   pWaitContext, // user-defined wait callback function context
  LFWAIT *pWaitCb,      // user-defined wait callback function (if NULL WaitForSingleObjectEx will be called)
  BOOL    fWaitSpin,    // allow waiting threads to spin?
  DWORD   dwMilliseconds, // timeout in milliseconds
  BOOL    fAlertable    // if TRUE then allow I/O completion routines calls and/or APC while waiting
)
{
  LOCK *Lock;
  DWORD dwResult;
  LF    OldState, NewState;
  LF_SPIN_DECL ();

  if ((Lock = (LOCK *) LFLock) == NULL || Lock->Magic != MAGIC_LOCK)
    return (ERROR_INVALID_PARAMETER);

  LF_DETECT_NUMBER_OF_CPUs();

  if (LF_ACTION_EQ (dwAction, LF_LOCK_SHARED))
  {
    // obtain shared lock

    if (Lock->Flag >= 0 && Lock->dwThreadID == GetCurrentThreadId ())
    {
      // recursive acquire
      ASSERT (LockTstExActive (Lock->State) != 0);

      if (Lock->Flag == 0)
      {
        // recursion is not allowed
        return (ERROR_LF_LOCK_RECURSE);
      }

      // increase number of active shared locks
      do
      {
        OldState = NewState = Lock->State;
#if SPIN_COUNT > 0
        if (!fWaitSpin)
          LockClrSpinWait (NewState);
        else if (LockTstShActive (NewState) == 0)
          LockSetSpinWait (NewState);
#endif
        LockIncShActive (NewState);
      }
      while (!CASLock (&Lock->State, NewState, OldState));
      return (WAIT_OBJECT_0);
    }

    for (;;)
    {
      OldState = NewState = Lock->State;
      if (LockTstExAll (NewState) == 0)
      {
        // no writers on horizon -- obtain shared lock
#if SPIN_COUNT > 0
        if (!fWaitSpin)
          LockClrSpinWait (NewState);
        else if (LockTstShActive (NewState) == 0)
          LockSetSpinWait (NewState);
#endif
        LockIncShActive (NewState);
        if (CASLock (&Lock->State, NewState, OldState))
        {
          // obtained shared lock
          return (WAIT_OBJECT_0);
        }
        // state changed in between -- retry
        continue;
      }

#if CHECK_LOCK_USAGE
      if (LockTstDisabled (NewState))
      {
        // lock is disabled
        return (ERROR_INVALID_PARAMETER);
      }
#endif

      // cannot obtain lock now
      if ((dwAction & LF_LOCK_TRY) != 0 || (SPIN_COUNT > 0 && LFCPUNum > 0 && (LockTstSpinWait (NewState) | dwMilliseconds) == 0))
      {
        return (WAIT_TIMEOUT);
      }

      // register as waiting reader
      LockIncShWaiting (NewState);
      if (CASLock (&Lock->State, NewState, OldState))
      {
        // registered
        break;
      }
    }

#if SPIN_COUNT > 0
    if (LFCPUNum > 0 && LockTstSpinWait (NewState) != 0)
    {
      // multi-processor machine -- spin
      int SpinCount = SPIN_COUNT;
      do
      {
        LF_SPIN_PAUSE ();
        OldState = NewState = Lock->State;
        // if nobody is active or waiting for exlusive lock get shared lock
        if (LockTstExAll (NewState) == 0)
        {
          // no writers on horizon -- obtain shared lock
          if (!fWaitSpin)
            LockClrSpinWait (NewState);
          else if (LockTstShActive (NewState) == 0)
            LockSetSpinWait (NewState);

          LockIncShActive (NewState);
          LockDecShWaiting (NewState);
          if (CASLock (&Lock->State, NewState, OldState))
          {
            // obtained shared lock
            return (WAIT_OBJECT_0);
          }
        }
      }
      while (--SpinCount && LockTstSpinWait (NewState) != 0);
    }
#endif /* SPIN_COUNT > 0 */

    for (;;)
    {
      dwResult = LF_WAIT (pWaitCb,pWaitContext, Lock->ReadersEvent, dwMilliseconds, fAlertable);
      if (dwResult != WAIT_OBJECT_0)
      {
        // wait failed -- reduce number of waiting threads
        do
        {
          OldState = NewState = Lock->State;
          LockDecShWaiting (NewState);
        }
        while (!CASLock (&Lock->State, NewState, OldState));

        return (dwResult);
      }

      //
      // we got the event but it is possible that due to timeout it was
      // left in signaled state for somebody else long time ago -- make sure
      // that the lock was really released
      //

      for (;;)
      {
        OldState = NewState = Lock->State;
        if (LockTstExAll (NewState) != 0)
        {
          // bogus event -- wait more
          // NB: since all Ex counters are in one word we may trust it because
          // read is atomic operation
          break;
        }
        // no writers on horizon -- obtain shared lock
#if SPIN_COUNT > 0
        if (!fWaitSpin)
          LockClrSpinWait (NewState);
        else if (LockTstShActive (NewState) == 0)
          LockSetSpinWait (NewState);
#endif
        LockIncShActive (NewState);
        LockDecShWaiting (NewState);
        if (CASLock (&Lock->State, NewState, OldState))
        {
          // obtained shared lock
          return (WAIT_OBJECT_0);
        }
      }
    }
  }
  else if (LF_ACTION_EQ (dwAction, LF_LOCK_EXCLUSIVE))
  {
    DWORD dwThreadID = THREAD_ID_NONE;

    // acquire exclusive lock

    if (Lock->Flag >= 0)
    {
      // will need thread id either to watch it or to allow recursion
      dwThreadID = GetCurrentThreadId ();
      if (Lock->dwThreadID == dwThreadID)
      {
        // recursive aquire
        ASSERT (LockTstExActive (Lock->State) != 0);

        if (Lock->Flag == 0)
        {
          // recursion is not allowed
          return (ERROR_LF_LOCK_RECURSE);
        }

        // increase number of active exclusive locks
        Lock->dwRecursion += 1;

        // clean SpinLock flag if necessary
        if (!fWaitSpin && LockTstSpinWait (Lock->State) != 0)
        {
          do
          {
            OldState = NewState = Lock->State;
            LockClrSpinWait (OldState);
          }
          while (!CASLock (&Lock->State, NewState, OldState));
        }


        // lock is obtained
        return (WAIT_OBJECT_0);
      }
    }

    for (;;)
    {
      OldState = NewState = Lock->State;

      if (LockTstAllActiveOrExWaiting (NewState) == 0 ||
        (LockTstSpinWait (NewState) && LockTstActiveAllOrExUpgrade (NewState) == 0)
      )
      {
        // no readers, no writers, or hostile lock takeover is allowed -- try to obtain exclusive lock
        LockSetExActive (NewState);
#if SPIN_COUNT > 0
        LockClrSpinWait (NewState);
        if (fWaitSpin)
          LockSetSpinWait (NewState);
#endif
        if (CASLock (&Lock->State, NewState, OldState))
        {
          // acquired exclusive lock
          Lock->dwThreadID = dwThreadID;
          return (WAIT_OBJECT_0);
        }
        // state changed in between -- retry
        continue;
      }

#if CHECK_LOCK_USAGE
      if (LockTstDisabled (NewState))
      {
        // lock is disabled
        return (ERROR_INVALID_PARAMETER);
      }
#endif

      // cannot obtain exclusive lock right now
      if ((dwAction & LF_LOCK_TRY) != 0 || (SPIN_COUNT > 0 && LFCPUNum > 0 && (LockTstSpinWait (NewState) | dwMilliseconds) == 0))
      {
        // use CAS to make sure that state has active locks indeed because
        // ShActive and ExActive are stored in different words, and read
        // of the state is not atomic operation
        if (CASLock (&Lock->State, OldState, OldState))
          return (WAIT_TIMEOUT);
        continue;
      }

      // register as waiting for exclusive lock
      LockIncExWaiting (NewState);
      if (CASLock (&Lock->State, NewState, OldState))
      {
        // registered
        break;
      }
    }

#if SPIN_COUNT > 0
    if (LFCPUNum > 0 && LockTstSpinWait (NewState) != 0)
    {
      // multi-processor machine -- spin
      int SpinCount = SPIN_COUNT;
      do
      {
        LF_SPIN_PAUSE ();
        OldState = NewState = Lock->State;
  
        //
        // spinning is very agressive, rude, impolite and dangerous practice
        // because it gets resources out of order ignoring everybody else;
        // also, it wastes CPU resources, overloads the bus, causes writer
        // starvation, provicates race, and reduces natural computer immunity
        // to these strange and unpredictable human beings
        //
        if (LockTstActiveAllOrExUpgrade (NewState) == 0)
        {
          LockSetExActive (NewState);
          LockDecExWaiting (NewState);

          LockClrSpinWait (NewState);
          if (fWaitSpin)
            LockSetSpinWait (NewState);

          if (CASLock (&Lock->State, NewState, OldState))
          {
            // lock acquired
            Lock->dwThreadID = dwThreadID;
            return (WAIT_OBJECT_0);
          }
        }
      }
      while (--SpinCount && LockTstSpinWait (NewState) != 0);
    }
#endif /* SPIN_COUNT > 0 */
  
    for (;;)
    {
      dwResult = LF_WAIT (pWaitCb,pWaitContext, Lock->WriterEvent, dwMilliseconds, fAlertable);
      if (dwResult != WAIT_OBJECT_0)
      {
        // wait failed -- reduce number of waiting threads
        do
        {
          OldState = NewState = Lock->State;
          LockDecExWaiting (NewState);
        }
        while (!CASLock (&Lock->State, NewState, OldState));

        // if we are the last writer release all waiting readers if such exist
        if (LockTstExAll (NewState) == 0 && LockTstShWaiting (NewState) != 0)
        {
          ReleaseSemaphore (Lock->ReadersEvent, LockGetShWaiting (NewState), NULL);
          // do not ensure that readers picked the event -- return ASAP
        }
        return (dwResult);
      }

      //
      // we got the event but it is possible that due to time out it was
      // left in signaled state for somebody else long time ago -- make sure
      // lock was really released
      //

      for (;;)
      { 
        NewState = Lock->State;
        if (LockTstActiveAllOrExUpgrade (NewState) != 0 && CASLock (&Lock->State, NewState, NewState))
        {
          // bogus event -- wait more
          // NB: have to CAS to make sure that ActiveAll == 0 indeed because
          // ExActive and ShActive counters are in two different words and
          // read could be incorrect

          if (LockTstExUpgrade (NewState) != 0)
          {
            // somebody is waiting for exclusive upgrade -- let him go first
            SetEvent (Lock->WriterEvent);
          }
          break;
        }

        // no active readers nor writers -- obtain exclusive lock
        OldState = NewState;
        LockSetExActive (NewState);
        LockDecExWaiting (NewState);
#if SPIN_COUNT > 0
        LockClrSpinWait (NewState);
        if (fWaitSpin)
          LockSetSpinWait (NewState);
#endif
        if (CASLock (&Lock->State, NewState, OldState))
        {
          // lock acquired
          Lock->dwThreadID = dwThreadID;
          return (WAIT_OBJECT_0);
        }
      }
    }
  }
  else if (LF_ACTION_EQ (dwAction, LF_LOCK_DOWNGRADE))
  {
    // downgrade exclusive lock to shared lock

    if (Lock->Flag >= 0)
    {
      // we know the thread that owns exclusive lock
      if (Lock->dwThreadID != GetCurrentThreadId ())
      {
        // oops, it is not you
        return (ERROR_LF_UNLOCK_BOGUS);
      }

      if (Lock->dwRecursion == 0)
      {
        // there is only one exclusive lock, and we are releasing it
        // clear ID of thread that keeps exclusive lock
        Lock->dwThreadID = THREAD_ID_NONE;
      }
    }

    do
    {
      OldState = NewState = Lock->State;
      if (Lock->dwRecursion == 0)
      {
        // exclusive lock is not owned recursively -- release it
        if (LockTstExActive (NewState) == 0)
        {
          // oh boy, you do not have exclusive lock!
          return (ERROR_LF_UNLOCK_BOGUS);
        }
        LockClrExActive (NewState);
      }
#if SPIN_COUNT > 0
      LockClrSpinWait (NewState);
      if (fWaitSpin)
        LockSetSpinWait (NewState);
#endif
      LockIncShActive (NewState);
    }
    while (!CASLock (&Lock->State, NewState, OldState));

    if (Lock->dwRecursion != 0)
    {
      // we hold exclusive lock recursively
      Lock->dwRecursion -= 1;
    }
    else if (LockTstExWaiting (NewState) == 0 && LockTstShWaiting (NewState) != 0)
    {
      // release other threads waiting for shared lock
      ReleaseSemaphore (Lock->ReadersEvent, LockGetShWaiting (NewState), NULL);
    }

    return (WAIT_OBJECT_0);
  }
  else if (LF_ACTION_EQ (dwAction, LF_LOCK_UPGRADE_SAFELY) || LF_ACTION_EQ (dwAction, LF_LOCK_UPGRADE_IGNORE))
  {
    DWORD dwThreadID = THREAD_ID_NONE;

    // upgrade shared lock to exlusive lock

    if (Lock->Flag >= 0)
    {
      dwThreadID = GetCurrentThreadId ();
      if (Lock->dwThreadID == dwThreadID)
      {
        // calling thread already holds exclusive lock so upgrade will be quick
        ASSERT (Lock->Flag > 0 && LockTstExActive (Lock->State) != 0);
        do
        {
          OldState = NewState = Lock->State;
          if (LockTstShActive (NewState) == 0)
          {
            // oops -- you do not have a shared lock, boy!
            return (ERROR_INVALID_PARAMETER);
          }
          LockDecShActive (NewState);
#if SPIN_COUNT > 0
          if (!fWaitSpin)
            LockClrSpinWait (NewState);
#endif
        }
        while (!CASLock (&Lock->State, NewState, OldState));

        Lock->dwRecursion += 1;
        return (WAIT_OBJECT_0);
      }
    }

    for (;;)
    {
      OldState = NewState = Lock->State;
      if (LockTstShActive (NewState) == 0)
      {
        // oops -- you do not have a shared lock, boy!
        return (ERROR_INVALID_PARAMETER);
      }
      if (LockTstExUpgrade (NewState))
      {
        // somebody already wants upgrade -- fail
        return (ERROR_LF_UPGRADE_PENDING);
      }
      if (LockTstExWaiting (NewState) != 0 && LF_ACTION_EQ (dwAction, LF_LOCK_UPGRADE_SAFELY))
      {
        // cannot safely upgrade the lock if there are waiting writers
        return (ERROR_LF_EXLOCK_PENDING);
      }

      // reduce number of active shared locks
      LockDecShActive (NewState);
      if (LockTstActiveAll (NewState) == 0)
      {
        // looks like nobody holds the lock -- try to obtain it now
        LockSetExActive (NewState);
#if SPIN_COUNT > 0
        LockClrSpinWait (NewState);
        if (fWaitSpin)
          LockSetSpinWait (NewState);
#endif
        if (CASLock (&Lock->State, NewState, OldState))
        {
          // upgraded shared lock to exlusive
          if (Lock->Flag >= 0)
          Lock->dwThreadID = dwThreadID;
          return (WAIT_OBJECT_0);
        }
        continue;
      }

      // cannot obtain exclusive lock right now
      if ((dwAction & LF_LOCK_TRY) != 0 || (SPIN_COUNT > 0 && LFCPUNum > 0 && (LockTstSpinWait (NewState) | dwMilliseconds) == 0))
      {
        // use CAS to make sure that state has active locks indeed because
        // ShActive and ExActive are stored in different words, and read is
        // of state is not atomic operation
        if (CASLock (&Lock->State, OldState, OldState))
          return (WAIT_TIMEOUT);
        continue;
      }

      // register as waiting for exclusive lock with upgrade
      LockSetExUpgrade (NewState);
      LockIncExWaiting (NewState);
      if (CASLock (&Lock->State, NewState, OldState))
        break;
    }

    // successfully registered as thread waiting for exclusive lock out-of-order
#if SPIN_COUNT > 0
    if (LFCPUNum > 0 && LockTstSpinWait (NewState) != 0)
    {
      // multi-processor machine -- spin
      int SpinCount = SPIN_COUNT;
      do
      {
        LF_SPIN_PAUSE ();
        OldState = NewState = Lock->State;
        if (LockTstActiveAll (NewState) == 0)
        {
          // lock is released -- obtain it now
          LockSetExActive (NewState);
          LockDecExWaiting (NewState);
          LockClrExUpgrade (NewState);

          LockClrSpinWait (NewState);
          if (fWaitSpin)
            LockSetSpinWait (NewState);

          if (CASLock (&Lock->State, NewState, OldState))
          {
            // upgraded shared lock to exlusive
            if (Lock->Flag >= 0)
            Lock->dwThreadID = dwThreadID;
            return (WAIT_OBJECT_0);
          }
        }
      }
      while (--SpinCount && LockTstSpinWait (NewState) != 0);
    }
#endif
  
    for (;;)
    {
      dwResult = LF_WAIT (pWaitCb,pWaitContext, Lock->WriterEvent, dwMilliseconds, fAlertable);
      if (dwResult != WAIT_OBJECT_0)
      {
        // wait failed -- reduce number of waiting threads
        do
        {
          OldState = NewState = Lock->State;
          LockDecExWaiting (NewState);
          LockClrExUpgrade (NewState);

          // we were holding read lock -- get it back
          // Writers honour either ExWaiting [normal acquire] or ExUpgrade [spin/wait]
          LockIncShActive (NewState);
        }
        while (!CASLock (&Lock->State, NewState, OldState));

        // if we are the last writer release all waiting readers if such exist
        if (LockTstExAll (NewState) == 0 && LockTstShWaiting (NewState) != 0)
        {
          ReleaseSemaphore (Lock->ReadersEvent, LockGetShWaiting (NewState), NULL);
          // do not ensure that readers picked the event -- return ASAP
        }
        return (dwResult);
      }

      //
      // we got the event but it is possible that due to time out it was
      // left in signaled state for somebody else long time ago -- make sure
      // lock was really released
      //

      for (;;)
      { 
        NewState = Lock->State;
        if (LockTstActiveAll (NewState) != 0 && CASLock (&Lock->State, NewState, NewState))
        {
          // bogus event -- wait more
          // NB: have to CAS to make sure that ActiveAll == 0 indeed because
          // Active counters are in two different words and read could be incorrect
          break;
        }

        // no active readers nor writers -- obtain exclusive lock
        OldState = NewState;
        LockSetExActive (NewState);
        LockDecExWaiting (NewState);
        LockClrExUpgrade (NewState);
#if SPIN_COUNT > 0
        LockClrSpinWait (NewState);
        if (fWaitSpin)
          LockSetSpinWait (NewState);
#endif
        if (CASLock (&Lock->State, NewState, OldState))
        {
          // upgraded shared lock to exlusive
          if (Lock->Flag >= 0)
          Lock->dwThreadID = dwThreadID;
          return (WAIT_OBJECT_0);
        }
      }
    }
  }

  // unknown dwAction
  return (ERROR_INVALID_PARAMETER);
}

DWORD                   // acquire a lock
LFAPI                   // returns FALSE if lock was not properly initialized
LFLockAcquire (
  PLFLOCK pLFLock,      // lock
  DWORD   dwAction,     // one of LF_LOCK_* actions
  BOOL    fWaitSpin,    // allow waiting threads to spin?
  DWORD   dwMilliseconds, // timeout in milliseconds
  BOOL    fAlertable    // if TRUE then allow I/O completion routines calls and/or APC while waiting
)
{
  return (
    LFLockAcquireEx (
      pLFLock,
      dwAction,
      0,
      0,
      fWaitSpin,
      dwMilliseconds,
      fAlertable
    )
  );
}


DWORD                   // release a lock
LFAPI                   // returns FALSE if exclusive settings do not match or
LFLockRelease (         // if lock was not properly initialized
  PLFLOCK LFLock,       // lock to release
  BOOL fExclusive,      // if TRUE then lock was exclusively owned
  BOOL fSwitchThreads
)
{
  LOCK *Lock;
  LF OldState, NewState;

  if ((Lock = (LOCK *) LFLock) == NULL || Lock->Magic != MAGIC_LOCK)
    return (ERROR_INVALID_PARAMETER);

  LF_DETECT_NUMBER_OF_CPUs();

  if (!fExclusive)
  {
    // release shared lock

    // decrement count of active shared locks
    do
    {
      OldState = NewState = Lock->State;
      if (LockTstShActive (NewState) == 0)
      {
        // nobody keeps shared lock -- ignore bogus unlock
        return (ERROR_LF_UNLOCK_BOGUS);
      }
      LockDecShActive (NewState);
    }
    while (!CASLock (&Lock->State, NewState, OldState));

    // release waiting writers if such exist

    // check BOTH exclusive and shared active counts because
    // it may be recursive shared unlock while keeping exclusive lock
    if (LockTstActiveAll (NewState) == 0 && LockTstExWaiting (NewState) != 0)
    {
      // if we are the last reader and there are waiting writers let them run
      SetEvent (Lock->WriterEvent);

      // make sure they got the point
      if (fSwitchThreads && LockTstExWaiting (Lock->State) == LockTstExWaiting (NewState))
      {
        // force thread switch and avoid overallocation of CPU to this thread
        Sleep (1);
      }
    }
  }
  else
  {
    // release exclusive lock

    // clear exclusive lock flag
    if (Lock->Flag >= 0)
    {
      // we know the owner of the lock

      if (Lock->dwThreadID != GetCurrentThreadId ())
      {
        // it's locked but not by this thread -- return error
        return (ERROR_LF_UNLOCK_BOGUS);
      }
      ASSERT (LockTstExActive (Lock->State) != 0);
  
      // OK, owner of the lock is current thread -- may release it
      if (Lock->dwRecursion != 0)
      {
        // had recursive acquires
        Lock->dwRecursion -= 1;
        return (ERROR_SUCCESS);
      }

      // clear locking thread id
      Lock->dwThreadID = THREAD_ID_NONE;
    }

    // clear ExActive flag 
    do
    {
      OldState = NewState = Lock->State;
      if (LockTstExActive (NewState) == 0)
      {
        // nobody hold exclusive lock -- ignore bogus unlock
        return (ERROR_LF_UNLOCK_BOGUS);
      }
      LockClrExActive (NewState);
    }
    while (!CASLock (&Lock->State, NewState, OldState));

    // release waiting threads
    if (LockTstExWaiting (NewState) != 0)
    {
      // there are waiting writers -- let them run first

      // due to recursion it is possible that we have active shared locks
      if (LockTstShActive (NewState) == 0)
      {
        // if there are more writers let them run
        SetEvent (Lock->WriterEvent);
  
        // make sure they got the point
        if (fSwitchThreads && LockTstExWaiting (Lock->State) == LockTstExWaiting (NewState))
        {
          // force thread switch and avoid overallocation of CPU to this thread
          Sleep (1);
        }
      }
    }
    else if (LockTstShWaiting (NewState) != 0)
    {
      // if there are waiting readers let of them run
      ReleaseSemaphore (Lock->ReadersEvent, LockGetShWaiting (NewState), NULL);

      // make sure they got the point -- provided we do not hold shared lock
      if (fSwitchThreads && LockTstShActive (NewState) == 0)
      {
        OldState = Lock->State;
        if (LockTstShWaiting (OldState) == LockTstShWaiting (NewState))
        {
          // force thread switch and avoid overallocation of CPU to this thread
          Sleep (1);
        }
      }
    }
  }

  // successfully released the lock
  return (ERROR_SUCCESS);
}

BOOL                    // destroy the lock and release occupied resources
LFAPI                   // returns FALSE if lock is in use or was not properly initialized
LFLockDestroy (
  PLFLOCK LFLock        // lock to destroy
)
{
  LOCK *Lock;
  BOOL  fResult;

  LF_DETECT_NUMBER_OF_CPUs();

  if ((Lock = (LOCK *) LFLock) == NULL || InterlockedExchange ((PLONG) &Lock->Magic, 0) != MAGIC_LOCK)
    return (ERROR_INVALID_PARAMETER);

  {
    LF OldState, NewState;
    for (;;)
    {
      OldState = NewState = Lock->State;
      if (LockTstExAll (NewState) != 0 || LockTstShWaiting (NewState) != 0 || LockTstShActive (NewState) != 0)
      {
        // we used two word -- CAS needed to verify result of read
        if (CASLock (&Lock->State, NewState, NewState))
        {
          // somebody is using the lock
          return (FALSE);
        }
        // bad read -- retry
        continue;
      }
      LockSetDisabled (NewState);
      if (CASLock (&Lock->State, NewState, OldState))
        break;
    }
  }

  fResult = CloseHandle (Lock->WriterEvent) & CloseHandle (Lock->ReadersEvent);
  LFFree (Lock->pMem, Lock->lockFree, NULL);

  LFSerializeWrites ();

  return (fResult);
}


PLFLOCK                 // create new lock
LFAPI                   // returns NULL if not enough memory
LFLockCreateBase (
  BOOL fWatchThread,    // if TRUE then release MUST be from the same thread that aquired the lock
  BOOL fAllowRecursion, // if TRUE then thread that owns exclusive lock may aquire any other locks
  LFMEM *pMem           // user-defined memory allocator
)
{
  LOCK *Lock;
  PVOID lockFree;

  Lock = LFAllocAligned (pMem, &lockFree, sizeof (*Lock) + (pMem == NULL ? 0 : sizeof (*pMem)), NULL);
  if (Lock == NULL)
    return (NULL);
  memset (Lock, 0, sizeof (*Lock));
  Lock->lockFree = lockFree;
  if (pMem == NULL)
    Lock->pMem = NULL;
  else
  {
    Lock->pMem = (LFMEM *) (Lock + 1);
    *(Lock->pMem) = *pMem;
  }

  Lock->Flag = -1;
  if (fAllowRecursion)
    Lock->Flag = 1;
  else if (fWatchThread)
    Lock->Flag = 0;


  Lock->State.I64 = 0;

  Lock->ReadersEvent = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
  if (Lock->ReadersEvent == NULL)
    goto Failure;

  // auto reset, nonsignaled
  Lock->WriterEvent = CreateEvent (NULL, FALSE, FALSE, NULL);


  if (Lock->WriterEvent == NULL)
  {
    CloseHandle (Lock->ReadersEvent);
  Failure:
    LFFree (Lock->pMem, lockFree, NULL);
    return (NULL);
  }

  Lock->Magic = MAGIC_LOCK;

  LF_DETECT_NUMBER_OF_CPUs();
  LFSerializeWrites ();

  return ((PLFLOCK) Lock);
}


PLFLOCK                 // create new lock
LFAPI                   // returns NULL if not enough memory
LFLockCreate (
  BOOL fWatchThread,    // if TRUE then release MUST be from the same thread that aquired the lock
  BOOL fAllowRecursion  // if TRUE then thread that owns exclusive lock may aquire any other locks
)
{
  return (LFLockCreateBase (fWatchThread, fAllowRecursion, NULL));
}

PLFLOCK                 // create new lock
LFAPI                   // returns NULL if not enough memory
LFLockCreateEx (
  BOOL fWatchThread,    // if TRUE then release MUST be from the same thread that aquired the lock
  BOOL fAllowRecursion, // if TRUE then thread that owns exclusive lock may aquire any other locks
  PVOID pMemoryContext, // user-defined memory allocator context for pAllocCb and pFreeCb
  DWORD dwMemAlignment, // min. alignment guaranteed by memory allocator (must be power of 2)
  LFALLOC *pAllocCb,    // memory allocation callback function
  LFFREE  *pFreeCb      // release memory callback function
)
{
  LFMEM Mem;
  if (!LFInitMem (&Mem, pMemoryContext, dwMemAlignment, pAllocCb, pFreeCb))
    return (NULL);
  return (LFLockCreateBase (fWatchThread, fAllowRecursion, &Mem));
}


/* ----------------- Lock an object using lock pool --------------------- */
/*                   ------------------------------                       */


#define MAGIC_POOL 'LfPl'

typedef struct OBJECT_T OBJECT;
typedef struct POOL_T POOL;
typedef struct EVENT_T EVENT;

//
// NB: EVENT_TABLE_MAX_SIZE_LOG must match Object*Event() macros
//
#define EVENT_TABLE_MIN_SIZE            32
#define EVENT_TABLE_MAX_SIZE_LOG        15
#define EVENT_TABLE_MAX_SIZE            (1<<EVENT_TABLE_MAX_SIZE_LOG)

#if LF_POOL_MAX_NUMBER_OF_EVENTS != (EVENT_TABLE_MAX_SIZE-1)
#error LF_POOL_MAX_NUMBER_OF_EVENTS and EVENT_TABLE_MAX_SIZE do not match
#endif

struct EVENT_T
{
  HANDLE ReadersEvent;  // event for threads waiting for shared lock
  HANDLE WriterEvent;   // event for writers waiting for exclusive lock
  DWORD  Index;         // event number in event table
  EVENT *EventNext;     // next free event
};

struct OBJECT_T
{
  volatile LF    State;         // this field shall go first -- next fields will be absent if Pool->Flag < 0
  volatile DWORD dwThreadID;
  DWORD dwRecursion;
};

#define CASObject  CASLock


struct POOL_T
{
  volatile LF FreeEvents;       // list of free events
  volatile LONG Magic;
  LONG          Flag;
  volatile LF EventTable;       // pair [ptr, #of elements]
  volatile LF FreeObjects;      // list of free objects (stack)
  volatile LF AllocBlocks;      // list of allocated blocks of objects

  volatile LONG AllocatedSize;
  volatile LONG AllocatedObjects;
  LONG          AllocObjects;

  PVOID EventTableList[EVENT_TABLE_MAX_SIZE_LOG];
  EVENT *EventTableStatic[EVENT_TABLE_MIN_SIZE];

  PVOID         FreePool;
  LFMEM        *pMem;           // user-defined memory allocator
};



PLFPOOL                 // create new lock pool
LFAPI                   // returns NULL if no enough memory
LFPoolCreateBase (
  LONG AllocObjects,    // allocate AllocObjects at once
  BOOL fWatchThread,    // if TRUE then only the thread that acquired the lock may release it
  BOOL fAllowRecursion, // if TRUE then thread that owns exclusive lock may aquire any other locks
  LFMEM *pMem           // user-defined memory allocator
)
{
  POOL *Pool;
  PVOID FreePool;
  volatile LONG AllocatedSize = 0;

  Pool = LFAllocAligned (pMem, &FreePool, sizeof (*Pool) + (pMem == NULL ? 0 : sizeof (*pMem)), &AllocatedSize);
  if (Pool == NULL)
    return (NULL);
  memset (Pool, 0, sizeof (*Pool));
  Pool->FreePool = FreePool;
  Pool->AllocatedSize = AllocatedSize;
  if (pMem == NULL)
    Pool->pMem = NULL;
  else
  {
    Pool->pMem = (LFMEM *) (Pool + 1);
    *(Pool->pMem) = *pMem;
  }

  Pool->Flag = -1;
  if (fAllowRecursion)
    Pool->Flag = 1;
  else if (fWatchThread)
    Pool->Flag = 0;

  if (AllocObjects < MIN_ALLOC_UNITS)
    AllocObjects = MIN_ALLOC_UNITS;
  else if (AllocObjects > MAX_ALLOC_UNITS)
    AllocObjects = MAX_ALLOC_UNITS;
  Pool->AllocObjects = AllocObjects;

  Pool->EventTableStatic[0] = 0;        // reserve entry #0
  SET_POINTER (Pool->EventTable, Pool->EventTableStatic, 1);

  Pool->Magic = MAGIC_POOL;

  LF_DETECT_NUMBER_OF_CPUs();
  LFSerializeWrites ();

  return ((PLFPOOL) Pool);
}

PLFPOOL                 // create new lock pool
LFAPI                   // returns NULL if no enough memory
LFPoolCreate (
  LONG AllocObjects,    // allocate AllocObjects at once
  BOOL fWatchThread,    // if TRUE then only the thread that acquired the lock may release it
  BOOL fAllowRecursion  // if TRUE then thread that owns exclusive lock may aquire any other locks
)
{
  return (LFPoolCreateBase (AllocObjects, fWatchThread, fAllowRecursion, NULL));
}


PLFPOOL                 // create new lock pool
LFAPI                   // returns NULL if no enough memory
LFPoolCreateEx (
  LONG AllocObjects,    // number of AllocObjects to allocate at once
  BOOL fWatchThread,    // if TRUE then only the thread that acquired the lock may release it
  BOOL fAllowRecursion, // if TRUE then thread that owns exclusive lock may aquire any other locks
  PVOID pMemoryContext, // user-defined memory allocator context for pAllocCb and pFreeCb
  DWORD dwMemAlignment, // min. alignment guaranteed by memory allocator (must be power of 2)
  LFALLOC *pAllocCb,    // memory allocation callback function
  LFFREE  *pFreeCb      // release memory callback function
)
{
  LFMEM Mem;
  if (!LFInitMem (&Mem, pMemoryContext, dwMemAlignment, pAllocCb, pFreeCb))
    return (NULL);
  return (LFPoolCreateBase (AllocObjects, fWatchThread, fAllowRecursion, &Mem));
}


BOOL                    // destroy the pool
LFAPI                   // returns FALSE if pool is in use or was not properly initialized
LFPoolDestroy (
  PLFPOOL LFPool
)
{
  POOL *Pool;
  EVENT *Event, **Table;
  volatile LONG AllocatedSize;
  LONG i;

  if ((Pool = (POOL *) LFPool) == 0 || InterlockedExchange ((PLONG) &Pool->Magic, 0) != MAGIC_POOL)
    return (FALSE);

  LF_DETECT_NUMBER_OF_CPUs();

  // release all events
  Table = GET_POINTER (Pool->EventTable);
  i = GET_COUNTER (Pool->EventTable);
  while (--i >= 0)
  {
    Event = Table[i];
    if (Event != NULL)
    {
      CloseHandle (Event->ReadersEvent);
      CloseHandle (Event->WriterEvent);
      LFFree (Pool->pMem, Event, &Pool->AllocatedSize);
    }
  }

  // release all tables
  for (i = 0; i < EVENT_TABLE_MAX_SIZE_LOG; ++i)
  {
    if (Pool->EventTableList[i] != NULL)
      LFFree (Pool->pMem, Pool->EventTableList[i], &Pool->AllocatedSize);
  }

  // release all objects
  LFFreeBlocks (Pool->pMem, &Pool->AllocBlocks, &Pool->AllocatedSize);

  // release the pool itself
  AllocatedSize = Pool->AllocatedSize;
  LFFree (Pool->pMem, Pool->FreePool, &AllocatedSize);

  ASSERT (Stack->pMem != NULL || AllocatedSize == 0);

  LFSerializeWrites ();

  return (TRUE);
}


LONG                    // returns total amount of memory allocated by lock pool
LFAPI                   // returns -1 if pool was not properly initialized
LFPoolTellMemory (
  PLFPOOL LFPool
)
{
  POOL *Pool;
  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL)
    return (-1);
  return (Pool->AllocatedSize);
}

static
EVENT *
PoolAllocEvent (
  POOL *Pool
)
{
  EVENT *Event;
  EVENT **Table;
  LF    OldTable, NewTable;
  DWORD Cnt;

  if (GET_COUNTER (Pool->EventTable) == EVENT_TABLE_MAX_SIZE)
  {
    // table if full -- cannot create more events
    return (NULL);
  }

  // create new event pair
  Event = LFAlloc (Pool->pMem, sizeof (*Event), &Pool->AllocatedSize);
  if (Event == NULL)
    goto nomem3;
  Event->ReadersEvent = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
  if (Event->ReadersEvent == NULL)
    goto nomem2;
  Event->WriterEvent = CreateEvent (NULL, FALSE, FALSE, NULL);
  if (Event->WriterEvent == NULL)
    goto nomem1;

  for (;;)
  {
    LFLoadPtr (OldTable, Pool->EventTable);
    Cnt = GET_COUNTER (OldTable);

    if (Cnt == EVENT_TABLE_MAX_SIZE)
    {
      // cannot add more elements
      break;
    }

    if (Cnt >= EVENT_TABLE_MIN_SIZE && ((Cnt-1) & Cnt) == 0)
    {
      // overflow check
      if (Cnt >= 0x7fffffff / (2*sizeof (Table[0])))
      {
        // overflow
        break;
      }

      // current table is full and contains exactly 2^n elements -- extend it
      Table = LFAlloc (Pool->pMem, Cnt * (2*sizeof (Table[0])), &Pool->AllocatedSize);
      if (Table == NULL)
      {
        // run out of memory
        break;
      }

      //
      // copy old table into new one (please notice that old table will NEVER
      // be released so if somebody is still using it)
      //
      memcpy (Table, GET_POINTER (OldTable), Cnt * sizeof (Table[0]));
      Table[Cnt] = Event;
      memset (Table + Cnt + 1, 0, (Cnt - 1) * sizeof (Table[0]));

      //
      // when table size increased we have full control over the new table
      // and may (and need) store new event there BEFORE we swap tables
      //
      SET_POINTER (NewTable, Table, Cnt + 1);
      if (CASLock (&Pool->EventTable, NewTable, OldTable))
      {
        //
        // if we successfully swapped tables then first (Cnt+1) entries
        // of new table are valid because we first insert new entry into
        // the table and increase the counter later. So if counter became
        // 2^n that means that all first Cnt entries are already filled
        // and memcpy copied them all into new table. New element was
        // inserted before swapping and counter increased, and table
        // size increase from 2^n to 2^(n+1) may succeed only once
        //

        // set event index in table
        Event->Index = Cnt;

        // store table address for releasing
        Cnt = 0;
        while (GET_COUNTER (OldTable) != (1U << Cnt))
          ++Cnt;
#pragma warning (push)                  // suppress warning about casting ptr to LONG and back
#pragma warning (disable: 4311 4312)    // when compiling for x86 platform with /Wp64 compiler option
        InterlockedExchangePointer (Pool->EventTableList + Cnt, Table);
#pragma warning (default: 4311 4312)
#pragma warning (pop)

        // done
        return (Event);
      }

      // somebody already increased size of pool table in between -- retry
      LFFree (Pool->pMem, Table, &Pool->AllocatedSize);
      continue;
    }
    
    // insert event into table
    Table = GET_POINTER (OldTable);
    SET_POINTER (NewTable, Table, Cnt + 1);
    if (!CASRef ((volatile PVOID *) (Table + GET_COUNTER (OldTable)), Event, NULL))
    {
      // somebody already occupied the slot but did not increase the counter yet

      //
      // increase counter ourselves if nobody did it meanwhile.
      // since Cnt may be incremented already and since CASRef could
      // fail simply because EventTable had changed while we were reading it
      // just do CAS: if it succeeds -- fine, if not -- it is OK
      //
      CASLock (&Pool->EventTable, NewTable, OldTable);

      // retry
      continue;
    }

    // we found empty slot in the table and copied pointer to event there
    Event->Index = GET_COUNTER (OldTable);

    // increase number of elements in table
    // if CAS fails it means that somebody else already increased the counter
    CASLock (&Pool->EventTable, NewTable, OldTable);

    // return event
    return (Event);
  }

  // run out of memory
  CloseHandle (Event->WriterEvent);

nomem1:
  CloseHandle (Event->ReadersEvent);

nomem2:
  LFFree (Pool->pMem, Event, &Pool->AllocatedSize);

nomem3:
  // prevent further allocations
  do
  {
    OldTable = Pool->EventTable;
    SET_POINTER (NewTable, GET_POINTER (OldTable), EVENT_TABLE_MAX_SIZE);
  }
  while (GET_COUNTER (OldTable) != EVENT_TABLE_MAX_SIZE && !CASLock (&Pool->EventTable, NewTable, OldTable));

  // cannot create new event
  return (NULL);
}


LONG                    // returns total number of event pairs allocated by lock pool
LFAPI                   // returns -1 if pool was not properly initialized
LFPoolTellNumberOfEvents (
  PLFPOOL LFPool
)
{
  POOL *Pool;
  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL)
    return (-1);
  return (GET_COUNTER (Pool->EventTable) - 1);
}

LONG                    // allocate more events
LFAPI                   // returns total number of new events created or -1 if pool was not properly initialized
LFPoolIncreaseNumberOfEvents (
  PLFPOOL LFPool,
  LONG    NumberOfEvents// number of events cannot exceed LF_POOL_MAX_NUMBER_OF_EVENTS
)
{
  POOL  *Pool;
  EVENT *Event;
  LONG  i;

  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL || NumberOfEvents < 0)
    return (-1);

  LF_DETECT_NUMBER_OF_CPUs();

  i = 0;
  while (i < NumberOfEvents && (Event = PoolAllocEvent (Pool)) != NULL)
  {
    ++i;
    PUSH (Pool->FreeEvents, Event, EventNext);
  }

  return (i);
}

BOOL                    // returns FALSE if LFObject == 0 or LFObject is misaligned
LFAPI                   // (it must be aligned on 8-byte boundary, better 16-byte boundary
LFPoolObjectInit (
  PLFPOOL   LFPool,
  PLFOBJECT LFObject    // object to initialize
)
{
  OBJECT *Object;
  POOL   *Pool;
  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL ||
    (Object = (OBJECT *) LFObject) == NULL || (((DWORD_PTR) Object) & 7) != 0
  )
  {
    return (FALSE);
  }
  Object->State.I64 = 0;
  ObjectSetUser (Object->State);

  if (Pool->Flag >= 0)
  {
    //
    // do not touch dwThreadID or dwRecursion if usage / recursion is not tracked
    // since these fields may be not present in short lock
    //
    Object->dwThreadID = THREAD_ID_NONE;
    Object->dwRecursion = 0;
  }

#ifdef LF_PERF_STAT
  LFObject->dwAcquired = 0;
  LFObject->dwWaitEx = 0;
  LFObject->dwWaitSh = 0;
  LFObject->dwWaitUp = 0;
#endif /* LF_PERF_STAT */
  LF_DETECT_NUMBER_OF_CPUs();
  LFSerializeWrites ();
  return (TRUE);
}

static
OBJECT *
PoolAllocObject (
  POOL *Pool
)
{
  LONG i = Pool->AllocObjects;

  // no overflow: Pool->AllocObjects > 0 && Pool->AllocObjects < 0x7fffffff / sizeof (*Object)

  if (Pool->Flag < 0)
  {
    LF *Object;

    // create short objects that do not have dwThreadID and dwRecursion

#ifdef _PREFAST_
#pragma prefast(suppress:12011, "silly sizeof checking")
#endif /* _PREFAST_ */
    Object = LFAllocBlock (Pool->pMem, &Pool->AllocBlocks, sizeof (*Object) * i, &Pool->AllocatedSize);
    if (Object == NULL)
      return (NULL);
    LFInterlockedAdd (&Pool->AllocatedObjects, i);

    Object += i;
    while (--Object, --i)
    {
      OBJECT *ActualObject = (OBJECT *) Object;
      // all these dances with ActualObject are needed because of inline ASM in PUSH,
      // Ptr is ambigious field name
      PUSH (Pool->FreeObjects, ActualObject, State.Next);
    }
  
    return ((OBJECT *) Object);
  }
  else
  {
    OBJECT *Object;

#ifdef _PREFAST_
#pragma prefast(suppress:12011, "silly sizeof checking")
#endif /* _PREFAST_ */
    Object = LFAllocBlock (Pool->pMem, &Pool->AllocBlocks, sizeof (*Object) * i, &Pool->AllocatedSize);
    if (Object == NULL)
      return (NULL);
    LFInterlockedAdd (&Pool->AllocatedObjects, i);

    Object += i;
    while (--Object, --i)
    {
      PUSH (Pool->FreeObjects, Object, State.Next);
    }

    return (Object);
  }
}

LONG                    // returns total number of object allocated by lock pool
LFAPI                   // returns -1 if pool was not properly initialized
LFPoolTellNumberOfObjects (
  PLFPOOL LFPool
)
{
  POOL *Pool;
  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL)
    return (-1);
  return (Pool->AllocatedObjects);
}

LONG                    // allocate more NumberOfObject lock objects
LFAPI                   // returns number of new objects allocated or -1 if pool was not properly initialized
LFPoolIncreaseNumberOfObjects (
  PLFPOOL LFPool,
  LONG    NumberOfObjects
)
{
  LONG i = NumberOfObjects;
  POOL *Pool;

  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL)
    return (-1);

  LF_DETECT_NUMBER_OF_CPUs();

  if (Pool->Flag < 0)
  {
    LF *Object;

    // create short objects that do not have dwThreadID and dwRecursion

    // no overflow: Pool->AllocObjects > 0 && Pool->AllocObjects < 0x7fffffff / sizeof (*Object)

    i = Pool->AllocObjects;
    Object = LFAllocBlock (Pool->pMem, &Pool->AllocBlocks, sizeof (*Object) * i, &Pool->AllocatedSize);
    if (Object == NULL)
      return (0);
    LFInterlockedAdd (&Pool->AllocatedObjects, i);

    Object += i;
    while (--Object, --i >= 0)
    {
      OBJECT *ActualObject = (OBJECT *) Object;
      // all these dances with ActualObject are needed because of inline ASM in PUSH,
      // Ptr is ambigious field name
      PUSH (Pool->FreeObjects, ActualObject, State.Next);
    }
  }
  else
  {
    OBJECT *Object;

    // overflow check
    if (i <= 0 || i >= 0x7fffffff / sizeof (*Object))
      return (0);

    Object = LFAllocBlock (Pool->pMem, &Pool->AllocBlocks, sizeof (*Object) * i, &Pool->AllocatedSize);
    if (Object == NULL)
      return (0);
    LFInterlockedAdd (&Pool->AllocatedObjects, i);

    Object += i;
    while (--Object, --i >= 0)
    {
      PUSH (Pool->FreeObjects, Object, State.Next);
    }
  }

  return (NumberOfObjects);
}

PLFOBJECT               // create new object associated with LFPool
LFAPI                   // return NULL if no enough memory
LFPoolObjectCreate (
  PLFPOOL LFPool
)
{
  POOL *Pool;
  OBJECT *Object;

  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL)
    return (NULL);

  LF_DETECT_NUMBER_OF_CPUs();

  POP (Pool->FreeObjects, Object, State.Next);
  if (Object == NULL)
  {
    Object = PoolAllocObject (Pool);
    if (Object == NULL)
      return (NULL);
  }

  Object->State.I64 = 0;

  if (Pool->Flag >= 0)
  {
    Object->dwRecursion = 0;
    Object->dwThreadID = THREAD_ID_NONE;
  }

  LFSerializeWrites ();

  return ((PLFOBJECT) Object);
}

BOOL                    // destroy the object
LFAPI                   // returns FALSE if either pool or object were not initialized properly
LFPoolObjectDestroy (
  PLFPOOL   LFPool,     // pool the object is associated with
  PLFOBJECT LFObject    // object
)
{
  POOL *Pool;
  OBJECT *Object;

  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL
    || (Object = (OBJECT *) LFObject) == NULL
    || (((DWORD_PTR) Object) & 7) != 0
    || ObjectTstAll (Object->State) != 0
    || (Pool->Flag >= 0 && (((DWORD_PTR) Object) & 15) != 0)
  )
  {
    return (FALSE);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  PUSH (Pool->FreeObjects, Object, State.Next);
  
  return (TRUE);
}



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
)
{
  POOL   *Pool;
  OBJECT *Object;
  DWORD  dwResult;
  LF     OldState, NewState;
  EVENT *Event;
  LF_SPIN_DECL ();

  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL
    || (Object = (OBJECT *) LFObject) == NULL
    || (((DWORD_PTR) Object) & 3) != 0
  )
  {
    return (ERROR_INVALID_PARAMETER);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  // no event allocated yet
  Event = NULL;

#ifdef LF_PERF_STAT
  if (ObjectGetUser (Object->State))
    ((PLFOBJECT) Object)->dwAcquired += 1;
#endif /* LF_PERF_STAT */

  if (LF_ACTION_EQ (dwAction, LF_LOCK_SHARED))
  {
    // obtain shared lock

    if (Pool->Flag >= 0 && Object->dwThreadID == GetCurrentThreadId ())
    {
      // recursive acquire
      ASSERT (ObjectTstExActive (Object->State) != 0);
      if (Pool->Flag == 0)
      {
        // recursion is not allowed
        return (ERROR_LF_LOCK_RECURSE);
      }

      // increase number of active shared locks
      do
      {
        OldState = NewState = Object->State;
#if SPIN_COUNT > 0
        if (!fWaitSpin)
          ObjectClrSpinWait (NewState);
        else if (ObjectTstShActive (NewState) == 0)
          ObjectSetSpinWait (NewState);
#endif
        ObjectIncShActive (NewState);
      }
      while (!CASObject (&Object->State, NewState, OldState));

      // acquired the lock
      return (WAIT_OBJECT_0);
    }

    for (;;)
    {
      OldState = NewState = Object->State;
      if (ObjectTstExAll (NewState) == 0)
      {
        // no writers on horizon -- obtain shared lock
#if SPIN_COUNT > 0
        if (!fWaitSpin)
          ObjectClrSpinWait (NewState);
        else if (ObjectTstShActive (NewState) == 0)
          ObjectSetSpinWait (NewState);
#endif
        ObjectIncShActive (NewState);
        if (CASObject (&Object->State, NewState, OldState))
        {
          // obtained shared lock -- return and release allocated event
          if (Event != NULL)
          {
            // we allocated event that we do not need
            PUSH (Pool->FreeEvents, Event, EventNext);
          }
          return (WAIT_OBJECT_0);
        }
        // state changed in between -- retry
        continue;
      }

#if CHECK_LOCK_USAGE && 0
      if (ObjectTstDisabled (NewState))
      {
        // lock is disabled
        if (Event != NULL)
        {
          // we allocated event that we do not need
          PUSH (Pool->FreeEvents, Event, EventNext);
        }
        return (ERROR_INVALID_PARAMETER);
      }
#endif

      // cannot obtain lock now
      if ((dwAction & LF_LOCK_TRY) != 0 || (SPIN_COUNT > 0 && LFCPUNum > 0 && (ObjectTstSpinWait (NewState) | dwMilliseconds) == 0))
      {
        // not going to try further
        if (Event != NULL)
        {
          // we allocated event that we do not need
          PUSH (Pool->FreeEvents, Event, EventNext);
        }
        return (WAIT_TIMEOUT);
      }

      // going to wait -- make sure that event is attached to the state
      if (ObjectTstEvent (NewState) == 0)
      {
        // no event attached -- will need one
        if (Event == NULL)
        {
          // get event from stack of available events
          POP (Pool->FreeEvents, Event, EventNext);

          // if nothing is available try to allocate
          if (Event == NULL && (Event = PoolAllocEvent (Pool)) == NULL)
          {
            // allocation failed
            return (ERROR_NOT_ENOUGH_MEMORY);
          }
        }
        // attach event to the state
        ObjectSetEvent (NewState, Event->Index);
      }

      // register as waiting reader
      ObjectIncShWaiting (NewState);
      if (CASObject (&Object->State, NewState, OldState))
      {
        // registered
        break;
      }
    }

#ifdef LF_PERF_STAT
    if (ObjectGetUser (Object->State))
      Increment ((volatile LONG *) &((PLFOBJECT) Object)->dwWaitSh);
#endif /* LF_PERF_STAT */

    // registered as waiting reader -- release allocated event if it differs from attached
    // and get event we will use for waiting
    if (Event == NULL)
    {
      // no event -- get it from table
      Event = ((EVENT **) GET_POINTER (Pool->EventTable))[ObjectGetEvent (NewState)];
    }
    else if (Event->Index != ObjectGetEvent (NewState))
    {
      // we allocated event but somebody already put another one into state
      PUSH (Pool->FreeEvents, Event, EventNext);
      Event = ((EVENT **) GET_POINTER (Pool->EventTable))[ObjectGetEvent (NewState)];
    }

#if SPIN_COUNT > 0
    if (LFCPUNum > 0 && ObjectTstSpinWait (NewState) != 0)
    {
      // multi-processor machine -- spin
      int SpinCount = SPIN_COUNT;
      do
      {
        LF_SPIN_PAUSE ();
        OldState = NewState = Object->State;
        ASSERT (ObjectGetEvent (NewState) == Event->Index);

        // if nobody is active or waiting for exlusive lock get shared lock
        if (ObjectTstExAll (NewState) == 0)
        {
          // no writers on horizon -- obtain shared lock
          if (!fWaitSpin)
            ObjectClrSpinWait (NewState);
          else if (ObjectTstShActive (NewState) == 0)
            ObjectSetSpinWait (NewState);

          ObjectIncShActive (NewState);
          ObjectDecShWaiting (NewState);

          // if do not need event anymore detach it from state
          if (ObjectTstWaitingAll (NewState) == 0)
            ObjectClrEvent (NewState);

          if (CASObject (&Object->State, NewState, OldState))
          {
            // obtained shared lock
            if (ObjectTstEvent (NewState) == 0)
            {
              // event is not needed anymore -- release
              PUSH (Pool->FreeEvents, Event, EventNext);
            }
            return (WAIT_OBJECT_0);
          }
        }
      }
      while (--SpinCount && ObjectTstSpinWait (NewState) != 0);
    }
#endif

    for (;;)
    {
      ASSERT (ObjectGetEvent (Object->State) == Event->Index);
      dwResult = LF_WAIT (pWaitCb,pWaitContext, Event->ReadersEvent, dwMilliseconds, fAlertable);
      ASSERT (ObjectGetEvent (Object->State) == Event->Index);
      if (dwResult != WAIT_OBJECT_0)
      {
        // wait failed -- reduce number of waiting threads
        do
        {
          OldState = NewState = Object->State;
          ObjectDecShWaiting (NewState);

          // if do not need event anymore detach it from state
          if (ObjectTstWaitingAll (NewState) == 0)
            ObjectClrEvent (NewState);
        }
        while (!CASObject (&Object->State, NewState, OldState));

        if (ObjectTstEvent (NewState) == 0)
        {
          // event is not needed anymore -- release
          PUSH (Pool->FreeEvents, Event, EventNext);
        }
        return (dwResult);
      }

      //
      // we got the event but it is possible that due to timeout it was
      // left in signaled state for somebody else long time ago -- make sure
      // that the lock was really released
      //

      for (;;)
      {
        OldState = NewState = Object->State;
        if (ObjectTstExAll (NewState) != 0)
        {
          // bogus event -- wait more
          break;
        }
        // no writers on horizon -- obtain shared lock
#if SPIN_COUNT > 0
        if (!fWaitSpin)
          ObjectClrSpinWait (NewState);
        else if (ObjectTstShActive (NewState) == 0)
          ObjectSetSpinWait (NewState);
#endif
        ObjectIncShActive (NewState);
        ObjectDecShWaiting (NewState);

        // if do not need event anymore detach it from state
        if (ObjectTstWaitingAll (NewState) == 0)
          ObjectClrEvent (NewState);

        if (CASObject (&Object->State, NewState, OldState))
        {
          // obtained shared lock
          if (ObjectTstEvent (NewState) == 0)
          {
            // event is not needed anymore -- release
            PUSH (Pool->FreeEvents, Event, EventNext);
          }
          return (WAIT_OBJECT_0);
        }
      }
    }
  }
  else if (LF_ACTION_EQ (dwAction, LF_LOCK_EXCLUSIVE))
  {
    DWORD dwThreadID = THREAD_ID_NONE;

    // acquire exclusive lock

    if (Pool->Flag >= 0)
    {
      // will need thread id either to watch it or to allow recursion
      dwThreadID = GetCurrentThreadId ();
      if (Object->dwThreadID == dwThreadID)
      {
        // recursive aquire
        ASSERT (ObjectTstExActive (Object->State) != 0);

        if (Pool->Flag == 0)
        {
          // recursion is not allowed
          return (ERROR_LF_LOCK_RECURSE);
        }

        // increase number of active exclusive locks
        Object->dwRecursion += 1;

        // clean SpinWait flag if necessary
        if (!fWaitSpin && ObjectTstSpinWait (Object->State) != 0)
        {
          do
          {
            OldState = NewState = Object->State;
            ObjectClrSpinWait (OldState);
          }
          while (!CASObject (&Object->State, NewState, OldState));
        }

        // lock is obtained
        return (WAIT_OBJECT_0);
      }
    }

    for (;;)
    {
      OldState = NewState = Object->State;

      if (ObjectTstAllActiveOrExWaiting (NewState) == 0 || 
        (ObjectTstSpinWait (NewState) && ObjectTstActiveAllOrExUpgrade (NewState) == 0)
      )
      {
        // no readers, no writers, or hostile lock takeover is enabled -- try to obtain exclusive lock
        ObjectSetExActive (NewState);
#if SPIN_COUNT > 0
        ObjectClrSpinWait (NewState);
        if (fWaitSpin)
          ObjectSetSpinWait (NewState);
#endif
        if (CASObject (&Object->State, NewState, OldState))
        {
          // acquired exclusive lock
          if (Event != NULL)
          {
            // we allocated event that we do not need
            PUSH (Pool->FreeEvents, Event, EventNext);
          }
          if (Pool->Flag >= 0)
            Object->dwThreadID = dwThreadID;
          return (WAIT_OBJECT_0);
        }
        // state changed in between -- retry
        continue;
      }

#if CHECK_LOCK_USAGE && 0
      if (ObjectTstDisabled (NewState))
      {
        // lock is disabled
        if (Event != NULL)
        {
          // we allocated event that we do not need
          PUSH (Pool->FreeEvents, Event, EventNext);
        }
        return (ERROR_INVALID_PARAMETER);
      }
#endif

      // cannot obtain exclusive lock right now
      if ((dwAction & LF_LOCK_TRY) != 0 || (SPIN_COUNT > 0 && LFCPUNum > 0 && (ObjectTstSpinWait (NewState) | dwMilliseconds) == 0))
      {
        // not going to wait further
        if (Event != NULL)
        {
          // we allocated event that we do not need
          PUSH (Pool->FreeEvents, Event, EventNext);
        }
        return (WAIT_TIMEOUT);
      }

      // going to wait -- make sure that event is attached to the state
      if (ObjectTstEvent (NewState) == 0)
      {
        // no event attached -- will need one
        if (Event == NULL)
        {
          // get event from stack of available events
          POP (Pool->FreeEvents, Event, EventNext);

          // if nothing is available try to allocate
          if (Event == NULL && (Event = PoolAllocEvent (Pool)) == NULL)
          {
            // allocation failed
            return (ERROR_NOT_ENOUGH_MEMORY);
          }
        }
        // attach event to the state
        ObjectSetEvent (NewState, Event->Index);
      }

      // register as waiting for exclusive lock
      ObjectIncExWaiting (NewState);
      if (CASObject (&Object->State, NewState, OldState))
      {
        // registered
        break;
      }
    }

#ifdef LF_PERF_STAT
    if (ObjectGetUser (Object->State))
      Increment ((volatile LONG *) &((PLFOBJECT) Object)->dwWaitEx);
#endif /* LF_PERF_STAT */

    // registered as waiting writer -- release allocated event if it differs from attached
    // and get event we will use for waiting
    if (Event == NULL)
    {
      // no event -- get it from table
      Event = ((EVENT **) GET_POINTER (Pool->EventTable))[ObjectGetEvent (NewState)];
    }
    else if (Event->Index != ObjectGetEvent (NewState))
    {
      // we allocated event but somebody already put another one into state
      PUSH (Pool->FreeEvents, Event, EventNext);
      Event = ((EVENT **) GET_POINTER (Pool->EventTable))[ObjectGetEvent (NewState)];
    }

#if SPIN_COUNT > 0
    if (LFCPUNum > 0 && ObjectTstSpinWait (NewState) != 0)
    {
      // multi-processor machine -- spin
      int SpinCount = SPIN_COUNT;
      do
      {
        LF_SPIN_PAUSE ();
        OldState = NewState = Object->State;

        //
        // spinning is very agressive, rude, impolite and dangerous practice
        // because it gets resources out of order ignoring everybody else;
        // also, it wastes CPU resources, overloads the bus, causes writer
        // starvation, provicates race, and reduces natural computer immunity
        // to these strange and unpredictable human beings
        //
        if (ObjectTstActiveAllOrExUpgrade (NewState) == 0)
        {
          ObjectSetExActive (NewState);
          ObjectDecExWaiting (NewState);

          ObjectClrSpinWait (NewState);
          if (fWaitSpin)
            ObjectSetSpinWait (NewState);

          // if do not need event anymore detach it from state
          if (ObjectTstWaitingAll (NewState) == 0)
            ObjectClrEvent (NewState);

          if (CASObject (&Object->State, NewState, OldState))
          {
            // obtained shared lock
            if (ObjectTstEvent (NewState) == 0)
            {
              // we allocated event that we do not need
              PUSH (Pool->FreeEvents, Event, EventNext);
            }
            if (Pool->Flag >= 0)
              Object->dwThreadID = dwThreadID;
            return (WAIT_OBJECT_0);
          }
        }
      }
      while (--SpinCount && ObjectTstSpinWait (NewState) != 0);
    }
#endif

    for (;;)
    {
      ASSERT (ObjectGetEvent (Object->State) == Event->Index);
      dwResult = LF_WAIT (pWaitCb,pWaitContext, Event->WriterEvent, dwMilliseconds, fAlertable);
      ASSERT (ObjectGetEvent (Object->State) == Event->Index);
      if (dwResult != WAIT_OBJECT_0)
      {
        // wait failed -- reduce number of waiting threads
        do
        {
          OldState = NewState = Object->State;
          ObjectDecExWaiting (NewState);

          // if do not need event anymore detach it from state
          if (ObjectTstWaitingAll (NewState) == 0)
            ObjectClrEvent (NewState);
        }
        while (!CASObject (&Object->State, NewState, OldState));

        if (ObjectTstEvent (NewState) == 0)
        {
          // event is not needed anymore -- release
          PUSH (Pool->FreeEvents, Event, EventNext);
        }

        // if we are the last writer release all waiting readers if such exist
        if (ObjectTstExAll (NewState) == 0 && ObjectTstShWaiting (NewState) != 0)
        {
          ReleaseSemaphore (Event->ReadersEvent, ObjectGetShWaiting (NewState), NULL);
          // do not ensure that readers picked the event -- return ASAP
        }

        return (dwResult);
      }

      //
      // we got the event but it is possible that due to time out it was
      // left in signaled state for somebody else long time ago -- make sure
      // lock was really released
      //

      for (;;)
      {
        NewState = Object->State;
        if (ObjectTstActiveAllOrExUpgrade (NewState) != 0)
        {
          // bogus event -- wait more
          if (ObjectTstExUpgrade (NewState) != 0)
          {
            // somebody is waiting for exclusive upgrade -- let him go first
            SetEvent (Event->WriterEvent);
          }
          break;
        }

        // no active readers nor writers -- obtain exclusive lock
        OldState = NewState;
        ObjectSetExActive (NewState);
        ObjectDecExWaiting (NewState);
#if SPIN_COUNT > 0
        ObjectClrSpinWait (NewState);
        if (fWaitSpin)
          ObjectSetSpinWait (NewState);
#endif

        // if do not need event anymore detach it from state
        if (ObjectTstWaitingAll (NewState) == 0)
          ObjectClrEvent (NewState);
        if (CASObject (&Object->State, NewState, OldState))
        {
          // obtained exclusive lock
          if (ObjectTstEvent (NewState) == 0)
          {
            // event is not needed anymore -- release
            PUSH (Pool->FreeEvents, Event, EventNext);
          }
          if (Pool->Flag >= 0)
            Object->dwThreadID = dwThreadID;
          return (WAIT_OBJECT_0);
        }
      }
    }
  }
  else if (LF_ACTION_EQ (dwAction, LF_LOCK_DOWNGRADE))
  {
    // downgrade exclusive lock to shared lock

    if (Pool->Flag >= 0)
    {
      // we know the thread that owns exclusive lock
      if (Object->dwThreadID != GetCurrentThreadId ())
      {
        // oops, it is not you
        return (ERROR_LF_UNLOCK_BOGUS);
      }

      if (Object->dwRecursion == 0)
      {
        // there is only one exclusive lock, and we are releasing it
        // clear ID of thread that keeps exclusive lock
        Object->dwThreadID = THREAD_ID_NONE;
      }
    }

    do
    {
      OldState = NewState = Object->State;
      if (Pool->Flag < 0 || Object->dwRecursion == 0)
      {
        // exclusive lock is not owned recursively -- release it
        if (ObjectTstExActive (NewState) == 0)
        {
          // oh boy, you do not have exclusive lock!
          return (ERROR_LF_UNLOCK_BOGUS);
        }
        ObjectClrExActive (NewState);
      }
#if SPIN_COUNT > 0
      ObjectClrSpinWait (NewState);
      if (fWaitSpin)
        ObjectSetSpinWait (NewState);
#endif
      ObjectIncShActive (NewState);
    }
    while (!CASObject (&Object->State, NewState, OldState));

    if (Pool->Flag >= 0 && Object->dwRecursion != 0)
    {
      // we hold exclusive lock recursively
      Object->dwRecursion -= 1;
    }
    else if (ObjectTstExWaiting (NewState) == 0 && ObjectTstShWaiting (NewState) != 0)
    {
      // release other threads waiting for shared lock
      Event = ((EVENT **) GET_POINTER (Pool->EventTable))[ObjectGetEvent (NewState)];
      ASSERT (Event != NULL);
      ReleaseSemaphore (Event->ReadersEvent, ObjectGetShWaiting (NewState), NULL);
    }

    return (WAIT_OBJECT_0);
  }
  else if (LF_ACTION_EQ (dwAction, LF_LOCK_UPGRADE_SAFELY) || LF_ACTION_EQ (dwAction, LF_LOCK_UPGRADE_IGNORE))
  {
    DWORD dwThreadID = THREAD_ID_NONE;

    // upgrade shared lock to exlusive lock

    if (Pool->Flag >= 0)
    {
      dwThreadID = GetCurrentThreadId ();
      if (Object->dwThreadID == dwThreadID)
      {
        // calling thread already holds exclusive lock so upgrade will be quick
        ASSERT (Pool->Flag > 0 && ObjectTstExActive (Object->State) != 0);
        do
        {
          OldState = NewState = Object->State;
          if (ObjectTstShActive (NewState) == 0)
          {
            // oops -- you do not have a shared lock, boy!
            return (ERROR_INVALID_PARAMETER);
          }
          ObjectDecShActive (NewState);
#if SPIN_COUNT > 0
          if (!fWaitSpin)
            ObjectClrSpinWait (NewState);
#endif
        }
        while (!CASObject (&Object->State, NewState, OldState));

        Object->dwRecursion += 1;
        return (WAIT_OBJECT_0);
      }
    }

    for (;;)
    {
      OldState = NewState = Object->State;
      if (ObjectTstShActive (NewState) == 0)
      {
        // oops -- you do not have a shared lock, boy!
        if (Event != NULL)
        {
          // we allocated event that we do not need
          PUSH (Pool->FreeEvents, Event, EventNext);
        }
        return (ERROR_INVALID_PARAMETER);
      }
      if (ObjectTstExUpgrade (NewState))
      {
        // somebody already wants upgrade -- fail
        if (Event != NULL)
        {
          // we allocated event that we do not need
          PUSH (Pool->FreeEvents, Event, EventNext);
        }
        return (ERROR_LF_UPGRADE_PENDING);
      }
      if (ObjectTstExWaiting (NewState) != 0 && LF_ACTION_EQ (dwAction, LF_LOCK_UPGRADE_SAFELY))
      {
        // cannot safely upgrade the lock if there are waiting writers
        if (Event != NULL)
        {
          // we allocated event that we do not need
          PUSH (Pool->FreeEvents, Event, EventNext);
        }
        return (ERROR_LF_EXLOCK_PENDING);
      }

      // reduce number of active shared locks
      ObjectDecShActive (NewState);
      if (ObjectTstActiveAll (NewState) == 0)
      {
        // looks like nobody holds the lock -- try to obtain it now
        ObjectSetExActive (NewState);
#if SPIN_COUNT > 0
        ObjectClrSpinWait (NewState);
        if (fWaitSpin)
          ObjectSetSpinWait (NewState);
#endif
        if (CASObject (&Object->State, NewState, OldState))
        {
          // upgraded shared lock to exlusive
          if (Event != NULL)
          {
            // we allocated event that we do not need
            PUSH (Pool->FreeEvents, Event, EventNext);
          }
          if (Pool->Flag >= 0)
            Object->dwThreadID = dwThreadID;
          return (WAIT_OBJECT_0);
        }
        continue;
      }

      // cannot obtain exclusive lock right now
      if ((dwAction & LF_LOCK_TRY) != 0 || (SPIN_COUNT > 0 && LFCPUNum > 0 && (ObjectTstSpinWait (NewState) | dwMilliseconds) == 0))
      {
        // not going to try further
        if (Event != NULL)
        {
          // we allocated event that we do not need
          PUSH (Pool->FreeEvents, Event, EventNext);
        }
        return (WAIT_TIMEOUT);
      }

      // going to wait -- make sure that event is attached to the state
      if (ObjectTstEvent (NewState) == 0)
      {
        // no event attached -- will need one
        if (Event == NULL)
        {
          // get event from stack of available events
          POP (Pool->FreeEvents, Event, EventNext);

          // if nothing is available try to allocate
          if (Event == NULL && (Event = PoolAllocEvent (Pool)) == NULL)
          {
            // allocation failed
            return (ERROR_NOT_ENOUGH_MEMORY);
          }
        }
        // attach event to the state
        ObjectSetEvent (NewState, Event->Index);
      }

      // register as waiting for exclusive lock with upgrade
      ObjectSetExUpgrade (NewState);
      ObjectIncExWaiting (NewState);
      if (CASObject (&Object->State, NewState, OldState))
        break;
    }

#ifdef LF_PERF_STAT
    if (ObjectGetUser (Object->State))
      Increment ((volatile LONG *) &((PLFOBJECT) Object)->dwWaitEx);
#endif /* LF_PERF_STAT */

    // successfully registered as thread waiting for exclusive lock out-of-order
    // release allocated event if it differs from attached and get event we will use for waiting
    if (Event == NULL)
    {
      // no event -- get it from table
      Event = ((EVENT **) GET_POINTER (Pool->EventTable))[ObjectGetEvent (NewState)];
    }
    else if (Event->Index != ObjectGetEvent (NewState))
    {
      // we allocated event but somebody already put another one into state
      PUSH (Pool->FreeEvents, Event, EventNext);
      Event = ((EVENT **) GET_POINTER (Pool->EventTable))[ObjectGetEvent (NewState)];
    }

#if SPIN_COUNT > 0
    if (LFCPUNum > 0 && ObjectTstSpinWait (NewState) != 0)
    {
      // multi-processor machine -- spin
      int SpinCount = SPIN_COUNT;
      do
      {
        LF_SPIN_PAUSE ();
        OldState = NewState = Object->State;
        if (ObjectTstActiveAll (NewState) == 0)
        {
          // lock is released -- obtain it now
          ObjectSetExActive (NewState);
          ObjectDecExWaiting (NewState);
          ObjectClrExUpgrade (NewState);

          ObjectClrSpinWait (NewState);
          if (fWaitSpin)
            ObjectSetSpinWait (NewState);

          // if do not need event anymore detach it from state
          if (ObjectTstWaitingAll (NewState) == 0)
            ObjectClrEvent (NewState);

          if (CASObject (&Object->State, NewState, OldState))
          {
            // obtained exclusive lock
            if (ObjectTstEvent (NewState) == 0)
            {
              // event is not needed anymore -- release
              PUSH (Pool->FreeEvents, Event, EventNext);
            }
            if (Pool->Flag >= 0)
              Object->dwThreadID = dwThreadID;
            return (WAIT_OBJECT_0);
          }
        }
      }
      while (--SpinCount && ObjectTstSpinWait (NewState) != 0);
    }
#endif

    for (;;)
    {
      ASSERT (ObjectGetEvent (Object->State) == Event->Index);
      dwResult = LF_WAIT (pWaitCb,pWaitContext, Event->WriterEvent, dwMilliseconds, fAlertable);
      ASSERT (ObjectGetEvent (Object->State) == Event->Index);
      if (dwResult != WAIT_OBJECT_0)
      {
        // wait failed -- reduce number of waiting threads
        do
        {
          OldState = NewState = Object->State;
          ObjectDecExWaiting (NewState);
          ObjectClrExUpgrade (NewState);

          // we hold read lock -- get it back
          // Writers honour either ExWaiting [normal acquire] or ExUpgrade [spin/wait]
          ObjectIncShActive (NewState);

          // if do not need event anymore detach it from state
          if (ObjectTstWaitingAll (NewState) == 0)
            ObjectClrEvent (NewState);
        }
        while (!CASObject (&Object->State, NewState, OldState));

        if (ObjectTstEvent (NewState) == 0)
        {
          // event is not needed anymore -- release
          PUSH (Pool->FreeEvents, Event, EventNext);
        }

        // if we are the last writer release all waiting readers if such exist
        if (ObjectTstExAll (NewState) == 0 && ObjectTstShWaiting (NewState) != 0)
        {
          ReleaseSemaphore (Event->ReadersEvent, ObjectGetShWaiting (NewState), NULL);
          // do not ensure that readers picked the event -- return ASAP
        }

        return (dwResult);
      }

      //
      // we got the event but it is possible that due to time out it was
      // left in signaled state for somebody else long time ago -- make sure
      // lock was really released
      //

      for (;;)
      {
        NewState = Object->State;
        if (ObjectTstActiveAll (NewState) != 0)
        {
          // bogus event -- wait more
          break;
        }

        // no active readers nor writers -- obtain exclusive lock
        OldState = NewState;
        ObjectSetExActive (NewState);
        ObjectDecExWaiting (NewState);
        ObjectClrExUpgrade (NewState);

#if SPIN_COUNT > 0
        ObjectClrSpinWait (NewState);
        if (fWaitSpin)
          ObjectSetSpinWait (NewState);
#endif

        // if do not need event anymore detach it from state
        if (ObjectTstWaitingAll (NewState) == 0)
          ObjectClrEvent (NewState);

        if (CASObject (&Object->State, NewState, OldState))
        {
          // obtained exclusive lock
          if (ObjectTstEvent (NewState) == 0)
          {
            // event is not needed anymore -- release
            PUSH (Pool->FreeEvents, Event, EventNext);
          }
          if (Pool->Flag >= 0)
            Object->dwThreadID = dwThreadID;
          return (WAIT_OBJECT_0);
        }
      }
    }
  }

  // wrong dwAction
  return (ERROR_INVALID_PARAMETER);
}


DWORD                           // acquire a lock
LFAPI
LFPoolObjectLockAcquire (
  PLFPOOL   LFPool,             // pool the object is associated with
  PLFOBJECT LFObject,           // object
  DWORD     dwAction,           // shared, exclusive, upgrade, downgrade
  BOOL      fWaitSpin,          // if TRUE then waiting threads will spin before actual wait
  DWORD     dwMilliseconds,     // timeout in milliseconds
  BOOL      fAlertable          // if TRUE then allow I/O completion routines calls and/or APC while waiting
)
{
  return (
    LFPoolObjectLockAcquireEx (
      LFPool,
      LFObject,
      dwAction,
      0,
      0,
      fWaitSpin,
      dwMilliseconds,
      fAlertable
    )
  );
}


DWORD                   // release lock
LFAPI                   // returns FALSE if exclusive settings do not match or
LFPoolObjectLockRelease (// either object or pool were not properly initialized
  PLFPOOL   LFPool,     // pool the object is associated with
  PLFOBJECT LFObject,   // object
  BOOL    fExclusive,   // if TRUE then lock was exclusively owned
  BOOL    fSwitchThreads
)
{
  POOL   *Pool;
  OBJECT *Object;
  LF    OldState, NewState;
  EVENT *Event;

  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL
    || (Object = (OBJECT *) LFObject) == NULL
    || (((DWORD_PTR) Object) & 3) != 0
  )
  {
    return (ERROR_INVALID_PARAMETER);
  }

  LF_DETECT_NUMBER_OF_CPUs();

  if (!fExclusive)
  {
    // release shared lock

    // decrement count of active shared locks
    do
    {
      OldState = NewState = Object->State;
      if (ObjectTstShActive (NewState) == 0)
      {
        // nobody keeps shared lock -- ignore bogus unlock
        return (ERROR_LF_UNLOCK_BOGUS);
      }
      ObjectDecShActive (NewState);
    }
    while (!CASObject (&Object->State, NewState, OldState));

    // release waiting writers if such exist

    // check BOTH exclusive and shared active counts because
    // it may be recursive shared unlock while keeping exclusive lock
    if (ObjectTstActiveAll (NewState) == 0 && ObjectTstExWaiting (NewState) != 0)
    {
      // if we are the last reader and there are waiting writers let them run
      Event = ((EVENT **) GET_POINTER (Pool->EventTable))[ObjectGetEvent (NewState)];
      ASSERT (Event != NULL);
      SetEvent (Event->WriterEvent);

      // make sure they got the point
      if (fSwitchThreads && ObjectTstExWaiting (Object->State) == ObjectTstExWaiting (NewState))
      {
        // force thread switch and avoid overallocation of CPU to this thread
        Sleep (1);
      }
    }
  }
  else
  {
    // release exclusive lock

    // clear exclusive lock flag
    if (Pool->Flag >= 0)
    {
      // we know the owner of the lock

      if (Object->dwThreadID != GetCurrentThreadId ())
      {
        // it's locked but not by this thread -- return error
        return (ERROR_LF_UNLOCK_BOGUS);
      }
      ASSERT (ObjectTstExActive (Object->State) != 0);
  
      // OK, owner of the lock is current thread -- may release it
      if (Object->dwRecursion != 0)
      {
        // had recursive acquires
        Object->dwRecursion -= 1;
        return (ERROR_SUCCESS);
      }

      // clear locking thread id
      Object->dwThreadID = THREAD_ID_NONE;
    }

    // clear ExActive flag 
    do
    {
      OldState = NewState = Object->State;
      if (ObjectTstExActive (NewState) == 0)
      {
        // nobody hold exclusive lock -- ignore bogus unlock
        return (ERROR_LF_UNLOCK_BOGUS);
      }
      ObjectClrExActive (NewState);
    }
    while (!CASObject (&Object->State, NewState, OldState));

    // release waiting threads
    if (ObjectTstExWaiting (NewState) != 0)
    {
      // there are waiting writers -- let them run first
      ASSERT (ObjectGetEvent (NewState) != 0);

      // due to recursion it is possible that we have active shared locks
      if (ObjectTstShActive (NewState) == 0)
      {
        // if there are more writers let them run
        Event = ((EVENT **) GET_POINTER (Pool->EventTable))[ObjectGetEvent (NewState)];
        SetEvent (Event->WriterEvent);
  
        // make sure they got the point
        if (fSwitchThreads && ObjectTstExWaiting (Object->State) == ObjectTstExWaiting (NewState))
        {
          // force thread switch and avoid overallocation of CPU to this thread
          Sleep (1);
        }
      }
    }
    else if (ObjectTstShWaiting (NewState) != 0)
    {
      // if there are waiting readers let of them run
      Event = ((EVENT **) GET_POINTER (Pool->EventTable))[ObjectGetEvent (NewState)];
      ASSERT (Event != NULL);
      ReleaseSemaphore (Event->ReadersEvent, ObjectGetShWaiting (NewState), NULL);

      // make sure they got the point -- provided we do not hold shared lock
      if (fSwitchThreads && ObjectTstShActive (NewState) == 0)
      {
        if (ObjectTstShWaiting (Object->State) == ObjectTstShWaiting (NewState))
        {
          // force thread switch and avoid overallocation of CPU to this thread
          Sleep (1);
        }
      }
    }
  }

  // successfully released the lock
  return (ERROR_SUCCESS);
}


//
// Attention! Cannot be used with small objects!
//
BOOL
LFAPI
LFPoolObjectIsExclusive (
  PLFOBJECT LFObject,   // object
  DWORD    *pdwThreadID // thread ID
)
{
  OBJECT *Object = (OBJECT *) LFObject;
  *pdwThreadID = Object->dwThreadID;
  return (ObjectTstExActive (Object->State) != 0);
}


//
// May be used with small objects
//
BOOL
LFAPI
LFPoolObjectIsExclusiveEx (
  PLFPOOL   LFPool,     // pool the object is associated with
  PLFOBJECT LFObject,   // object
  DWORD    *pdwThreadID // thread ID (optional, may be NULL)
)
{
  BOOL    fResult;

  POOL   *Pool;
  OBJECT *Object;
  DWORD   dwThreadID;

  if ((Pool = (POOL *) LFPool) == 0 || Pool->Magic != MAGIC_POOL
    || (Object = (OBJECT *) LFObject) == NULL
  )
  {
    return (FALSE);
  }

  //
  // get ID of the thread holding exclusive lock (if available)
  //
  for (;;)
  {
    //
    // get ID of the thread that owns the lock (if available and if caller cares)
    //
    dwThreadID = THREAD_ID_NONE;
    if (Pool->Flag >= 0 && pdwThreadID != NULL)
      dwThreadID = Object->dwThreadID;
    
    if (ObjectTstExActive (Object->State) == 0)
    {
      //
      // lock is not owned by anyone -- set dwThreadID to none and return
      //
      dwThreadID = THREAD_ID_NONE;
      fResult = FALSE;
      break;
    }

    //
    // somebody owns the lock; see whether we got ID of the thread holding the lock
    //
    if (Pool->Flag < 0 || dwThreadID != THREAD_ID_NONE || pdwThreadID == NULL)
    {
      //
      // got thread ID, or ID is not available, or user doesn't care
      //
      fResult = TRUE;
      break;
    }

    //
    // lock is owned by somebody but we didn't get thread ID right; retry
    //
  }

  if (pdwThreadID != NULL)
    *pdwThreadID = dwThreadID;

  return (fResult);
}


BOOL                    // check to see whether object is unused
LFAPI                   // returns FALSE if pool is released or object is in use
LFPoolObjectCheck (
  PLFPOOL   LFPool,     // pool the object is associated with
  PLFOBJECT LFObject    // object
)
{
  OBJECT *Object;

  if ((Object = (OBJECT *) LFObject) == NULL
    || (((DWORD_PTR) Object) & 7) != 0
    || (Object->State.Word1 | ((Object->State.Word2 ^ (1<<16)) & ~(1<<15))) != 0
  )
  {
    return (FALSE);
  }

  return (TRUE);
}



/* ----------- Push/Pop/Put/Get/LockAcquire/Wait multiple --------------- */
/*             ------------------------------------------                 */


#define LF_TIMEOUT_ADJUST 0x40000000 // adjust timeouts smaller than LF_TIMEOUT_ADJUST (300 hrs) to avoid clock rollover problem

typedef struct
{
  //
  // caller arguments
  //
  PVOID        *pWaitContext;
  LFWAITMULTI  *pWaitMultiCb;

  LFMULTI      *pObjects;
  DWORD         nObjectCount;

  BOOL          fWaitSpin;
  BOOL          fWaitCompleted;
  BOOL          fRetry;
  BOOL          fTimeout;

  // state for WAIT_ALL
  //
  // current position
  //
  DWORD         dwObjectIndex;          // current position in "pObjects" array
  DWORD         dwHandleIndex;          // current position in "hHandles" array

  //
  // actual status
  //
  DWORD         dwStatus;               // return status

  //
  // handles to wait on
  //
  HANDLE        hHandles[MAXIMUM_WAIT_OBJECTS];

  //
  // mapping between hHandles and pObjects
  //
  DWORD         dwMap[MAXIMUM_WAIT_OBJECTS];

} LF_MULTI_STATE;


static
DWORD
LFMultiCallObject (
  LF_MULTI_STATE *pState,           // state
  LFMULTI        *pObject,          // wait object
  LFWAIT         *pMultiWaitCb,     // wait callback
  DWORD           dwMilliseconds,   // timeout in milliseconds
  BOOL            fAlertable        // if TRUE then allow I/O completion routines and APC calls
)
{
  DWORD dwStatus;

  switch (pObject->dwAction >> 4)
  {
  case (LF_OBJECT_ACQUIRE >> 4):
    dwStatus = LFPoolObjectLockAcquireEx (
      pObject->Pool,
      pObject->Object,
      pObject->dwAction & 15,
      pState,
      pMultiWaitCb,
      pState->fWaitSpin,
      dwMilliseconds,
      fAlertable
    );
    break;

  case (LF_LOCK_ACQUIRE >> 4):
    dwStatus = LFLockAcquireEx (
      pObject->Lock,
      pObject->dwAction & 15,
      pState,
      pMultiWaitCb,
      pState->fWaitSpin,
      dwMilliseconds,
      fAlertable
    );
    break;

  case (LF_STACK_PUSH >> 4):
    dwStatus = LFStackPushWaitEx (
      pObject->Stack,
      pObject->Data,
      pState,
      pMultiWaitCb,
      pState->fWaitSpin,
      dwMilliseconds,
      fAlertable
    );
    break;

  case (LF_STACK_POP >> 4):
    dwStatus = LFStackPopWaitEx (
      pObject->Stack,
     &pObject->Data,
      pState,
      pMultiWaitCb,
      pState->fWaitSpin,
      dwMilliseconds,
      fAlertable
    );
    break;

  case (LF_QUEUE_PUT >> 4):
    dwStatus = LFQueuePutWaitEx (
      pObject->Queue,
      pObject->Msg,
      pState,
      pMultiWaitCb,
      pState->fWaitSpin,
      dwMilliseconds,
      fAlertable
    );
    break;

  case (LF_QUEUE_GET >> 4):
    dwStatus = LFQueueGetWaitEx (
      pObject->Queue,
     &pObject->Msg,
      pState,
      pMultiWaitCb,
      pState->fWaitSpin,
      dwMilliseconds,
      fAlertable
    );
    break;

  case (LF_EXLK_ACQUIRE >> 4):
    dwStatus = LFExLockAcquireEx (
      pObject->ExLock,
      pState,
      pMultiWaitCb,
      pState->fWaitSpin,
      dwMilliseconds,
      fAlertable
    );
    break;

  default:
    // oops, wrong action
    dwStatus = ERROR_INVALID_PARAMETER;
    break;
  }

  return (dwStatus);
}


static
DWORD
LFMultiWait (
  LF_MULTI_STATE *pState,
  DWORD           dwObjectIndex,
  BOOL            fWaitAll,
  DWORD           dwMilliseconds,
  BOOL            fAlertable
)
{
  DWORD dwStatus;

  if (dwObjectIndex == 0)
  {
    // nobody to wait on; everything is fine
    dwStatus = WAIT_OBJECT_0;
  }
  else if (dwMilliseconds == 0)
  {
    // wait timed out
    dwStatus = WAIT_TIMEOUT;
  }
  else if (pState->pWaitMultiCb == 0)
  {
    dwStatus = WaitForMultipleObjectsEx (
      dwObjectIndex,
      pState->hHandles,
      fWaitAll,
      dwMilliseconds,
      fAlertable
    );
  }
  else
  {
    dwStatus = pState->pWaitMultiCb (
      pState->pWaitContext,
      dwObjectIndex,
      pState->hHandles,
      fWaitAll,
      dwMilliseconds,
      fAlertable
    );
  }

  return (dwStatus);
}



static
DWORD
LFAPI
LFMultiWaitAnyCb (
  PVOID  pWaitContext,  // user-defined context
  HANDLE hWaitObject,   // object to wait on
  DWORD  dwMilliseconds,// timeout in milliseconds
  BOOL   fAlertable     // if TRUE then allow I/O completion routines and APC calls
)
{
  LF_MULTI_STATE *pState = pWaitContext;
  DWORD dwObjectIndex = pState->dwObjectIndex;  // current object index
  DWORD dwWaitObjectIndex = dwObjectIndex - 1;  // index of object previous call is waiting on
  DWORD dwStatus;
  LFMULTI *pObject;

  if (pState->fWaitCompleted)
  {
    // this is not a first pass, do not do anything
    //
    // However, we'll need to retry waiting (if we do not hit a timeout)
    // because if we were called again it means that LF got bogus signal
    // and will need to retry the operation. We'd better get all the way
    // back and try again.
    //
    pState->fRetry = TRUE;
    ASSERT (dwWaitObjectIndex < pState->nObjectCount && pState->dwStatus == WAIT_OBJECT_0 + dwWaitObjectIndex);

    dwStatus = WAIT_TIMEOUT;
    if (dwWaitObjectIndex < pState->nObjectCount)
    {
      // abort retries by LF (it may get bogus events when spinning is enabled)
      dwStatus = pState->pObjects[dwWaitObjectIndex].dwStatus;
      if (dwStatus == WAIT_OBJECT_0)
      {
        dwStatus = WAIT_TIMEOUT;
        pState->pObjects[dwWaitObjectIndex].dwStatus = dwStatus;
        if (pState->dwStatus == WAIT_OBJECT_0 + dwWaitObjectIndex)
          pState->dwStatus = WAIT_TIMEOUT;
      }
    }

    return (dwStatus);
  }

  if (dwObjectIndex != 0)
  {
    // attempt to acquire previous object ended up waiting; record wait handle
    pState->hHandles[dwWaitObjectIndex] = hWaitObject;
  }

  for (; dwObjectIndex != pState->nObjectCount; ++dwObjectIndex)
  {
    //
    // process next object
    //
    pObject = pState->pObjects + dwObjectIndex;

    if (pObject->dwAction == LF_SYSTEM_HANDLE)
    {
      // if it's system handle, remember it and proceed further
      pState->hHandles[dwObjectIndex] = pObject->Handle;
      continue;
    }

    // next call to LFMultiWaitCb will need to handle next object
    pState->dwObjectIndex = dwObjectIndex + 1;

    // call the object
    dwStatus = LFMultiCallObject (pState, pObject, LFMultiWaitAnyCb, dwMilliseconds, fAlertable);

    // restore pState->dwObjectIndex
    pState->dwObjectIndex = dwWaitObjectIndex + 1;

    // save status
    pObject->dwStatus = dwStatus;

    // check the status
    if (dwStatus == WAIT_OBJECT_0)
    {
      // successfully acquired current object; remember its index
      pState->dwStatus = WAIT_OBJECT_0 + dwObjectIndex;

      // abandon all pending waits
      return (WAIT_TIMEOUT);
    }
    
    if (dwStatus == WAIT_TIMEOUT && pState->dwStatus == WAIT_OBJECT_0 + dwWaitObjectIndex)
    {
      // got fake timeout; notify previous caller that wait succeeded
      // NB: it's OK to return WAIT_OBJECT_0 cwhen dwWaitObjectIndex == (-1) (initial call)

      // tell waiting LF object that wait had succeeded
      return (WAIT_OBJECT_0);
    }

    if (pState->dwStatus == WAIT_TIMEOUT)
    {
      // remember error code
      pState->dwStatus = dwStatus;
    }

    // abandon all pending waits
    return (WAIT_TIMEOUT);
  }


  // processed all objects; need to wait
  pState->fWaitCompleted = TRUE;
  dwObjectIndex = pState->nObjectCount;

  // wait on all handles at once
  dwStatus = LFMultiWait (pState, dwObjectIndex, FALSE, dwMilliseconds, fAlertable);

  // see what we got
  pState->dwStatus = dwStatus;

  if ((dwStatus - WAIT_OBJECT_0) < dwObjectIndex)
  {
    // got (WAIT_OBJECT_0 + n)
    pState->pObjects[dwStatus - WAIT_OBJECT_0].dwStatus = WAIT_OBJECT_0;        // set status of signalling object
    if (dwStatus == dwWaitObjectIndex + WAIT_OBJECT_0)
    {
      // event for last object is signalling; tell waiting LF object that wait succeeded
      return (WAIT_OBJECT_0);
    }
  }
  else if ((dwStatus - WAIT_ABANDONED_0) < dwObjectIndex)
  {
    // got (WAIT_ABANDONED_0 + n)
    pState->pObjects[dwStatus - WAIT_ABANDONED_0].dwStatus = WAIT_ABANDONED_0;  // set status of signalling object

    // LF doesn't use mutexes so it can't be LF object -- return WAIT_TIMEOUT
  }
  else if (dwStatus == WAIT_TIMEOUT)
  {
    // tell the caller to back off because we got timeout from wait
    pState->fTimeout = TRUE;
  }

  // abandon all pending waits (if there was signalling handle, it will be handled on return)
  return (WAIT_TIMEOUT);
}


static
void
LFMultiLockRelease (
  LFMULTI        *pObject,
  DWORD           dwNewStatus
)
{
  DWORD dwStatus;

  if (pObject->dwStatus != WAIT_OBJECT_0)
    return;

  dwStatus = dwNewStatus;
  switch (pObject->dwAction >> 4)
  {
  case (LF_OBJECT_ACQUIRE >> 4):
    switch (pObject->dwAction & 7)
    {
    case LF_LOCK_SHARED:
      // acquired shared lock -- release it
      dwStatus = LFPoolObjectLockRelease (pObject->Pool, pObject->Object, FALSE, FALSE);
      if (dwStatus == ERROR_SUCCESS)
        dwStatus = dwNewStatus;
      else
      {
        // attempt to release the lock failed; keep old status
        dwStatus = pObject->dwStatus;
      }
      break;

    case LF_LOCK_EXCLUSIVE:
      // acquired exclusive lock -- release it
      dwStatus = LFPoolObjectLockRelease (pObject->Pool, pObject->Object, TRUE, FALSE);
      if (dwStatus == ERROR_SUCCESS)
        dwStatus = dwNewStatus;
      else
      {
        // attempt to release the lock failed; keep old status
        dwStatus = pObject->dwStatus;
      }
      break;

    case LF_LOCK_UPGRADE_SAFELY:
    case LF_LOCK_UPGRADE_IGNORE:
      // upgraded shared lock to exclusive -- downgrade it
      dwStatus = LFPoolObjectLockAcquire (pObject->Pool, pObject->Object, LF_LOCK_DOWNGRADE, FALSE, 0, FALSE);
      if (dwStatus == ERROR_SUCCESS)
        dwStatus = dwNewStatus;
      else
      {
        // attempt to downgrade the lock failed; keep old status
        dwStatus = pObject->dwStatus;
      }
      break;

    case LF_LOCK_DOWNGRADE:
      // downgraded shared to exclusive; try to upgrade it
      dwStatus = LFPoolObjectLockAcquire (pObject->Pool, pObject->Object, LF_LOCK_UPGRADE_IGNORE, FALSE, 0, FALSE);
      if (dwStatus == ERROR_SUCCESS)
        dwStatus = dwNewStatus;
      else
      {
        // attempt to upgrade failed; keep the same status
        dwStatus = pObject->dwStatus;
      }
      break;

    default:
      //
      // we'd better leave everything the way it was and let the caller handle the failure which
      // is likely memory corruption -- action was correct before and bogus now
      //
      break;
    }
    break;

  case (LF_LOCK_ACQUIRE >> 4):
    switch (pObject->dwAction & 7)
    {
    case LF_LOCK_SHARED:
      // acquired shared lock -- release it
      dwStatus = LFLockRelease (pObject->Lock, FALSE, FALSE);
      if (dwStatus == ERROR_SUCCESS)
        dwStatus = dwNewStatus;
      else
      {
        // attempt to release the lock failed; keep old status
        dwStatus = pObject->dwStatus;
      }
      break;

    case LF_LOCK_EXCLUSIVE:
      // acquired exclusive lock -- release it
      dwStatus = LFLockRelease (pObject->Lock, TRUE, FALSE);
      if (dwStatus == ERROR_SUCCESS)
        dwStatus = dwNewStatus;
      else
      {
        // attempt to release the lock failed; keep old status
        dwStatus = pObject->dwStatus;
      }
      break;

    case LF_LOCK_UPGRADE_SAFELY:
    case LF_LOCK_UPGRADE_IGNORE:
      // upgraded shared lock to exclusive -- downgrade it
      dwStatus = LFLockAcquire (pObject->Lock, LF_LOCK_DOWNGRADE, FALSE, 0, FALSE);
      if (dwStatus == ERROR_SUCCESS)
        dwStatus = dwNewStatus;
      else
      {
        // attempt to downgrade the lock failed; keep old status
        dwStatus = pObject->dwStatus;
      }
      break;

    case LF_LOCK_DOWNGRADE:
      // downgraded shared to exclusive; try to upgrade it
      dwStatus = LFLockAcquire (pObject->Lock, LF_LOCK_UPGRADE_IGNORE, FALSE, 0, FALSE);
      if (dwStatus == ERROR_SUCCESS)
        dwStatus = dwNewStatus;
      else
      {
        // attempt to upgrade failed; keep the same status
        dwStatus = pObject->dwStatus;
      }
      break;

    default:
      ASSERT (FALSE);
      //
      // we'd better leave everything the way it was and let the caller handle the failure which
      // is likely memory corruption -- action was correct before and bogus now
      //
      break;
    }
    break;

  case (LF_EXLK_ACQUIRE >> 4):
    dwStatus = LFExLockRelease (pObject->ExLock, FALSE);
    if (dwStatus == ERROR_SUCCESS)
      dwStatus = dwNewStatus;
    else
    {
      // attempt to release the lock failed; keep old status
      dwStatus = pObject->dwStatus;
    }
    break;

  default:
    //
    // we'd better leave everything the way it was and let the caller handle the failure
    //
    break;
  }

  pObject->dwStatus = dwStatus;
}


static
DWORD
LFMultiWaitAllSetStatus (
  LF_MULTI_STATE *pState,
  DWORD           dwStatus,
  DWORD           dwObjectIndex
)
{
  if ((dwStatus - WAIT_ABANDONED_0) < dwObjectIndex)
  {
    // wait succeeded and mutex is abandoned; keep the index of abandoned mutex

    // get actual object's index and set its status
    dwStatus = pState->dwMap[dwStatus - WAIT_ABANDONED_0];
    pState->pObjects[dwStatus].dwStatus = WAIT_ABANDONED_0;

    // set operation's status
    pState->dwStatus = WAIT_ABANDONED_0 + dwStatus;

    dwStatus = WAIT_OBJECT_0;
  }
  else if ((dwStatus - WAIT_OBJECT_0) < dwObjectIndex)
  {
    // wait was successful

    // get actual object's index and set its status
    dwStatus = pState->dwMap[dwStatus - WAIT_OBJECT_0];
    pState->pObjects[dwStatus].dwStatus = WAIT_OBJECT_0;

    // set operation's status
    pState->dwStatus = WAIT_OBJECT_0;

    dwStatus = WAIT_OBJECT_0;
  }
  else if (dwStatus == WAIT_TIMEOUT)
  {
    // tell the caller to back off because we got timeout from wait
    pState->fTimeout = TRUE;
  }
  else
  {
    // that was an error
    pState->dwStatus = dwStatus;
  }

  // set status of all objects we were waiting on to dwStatus
  if (dwStatus != WAIT_TIMEOUT && dwObjectIndex > 0)
  {
    do
    {
      --dwObjectIndex;
      if (pState->pObjects[pState->dwMap[dwObjectIndex]].dwStatus == WAIT_TIMEOUT)
        pState->pObjects[pState->dwMap[dwObjectIndex]].dwStatus = dwStatus;
    }
    while (dwObjectIndex != 0);
  }

  return (dwStatus);
}


static
DWORD
LFAPI
LFMultiWaitAllCb (
  PVOID  pWaitContext,  // user-defined context
  HANDLE hWaitObject,   // object to wait on
  DWORD  dwMilliseconds,// timeout in milliseconds
  BOOL   fAlertable     // if TRUE then allow I/O completion routines and APC calls
)
{
  LF_MULTI_STATE *pState = pWaitContext;
  DWORD dwObjectIndex = pState->dwObjectIndex;  // current object index
  DWORD dwWaitObjectIndex = dwObjectIndex - 1;  // index of object previous call is waiting on
  DWORD dwStatus;
  LFMULTI *pObject;

  if (pState->fWaitCompleted)
  {
    // this is not a first pass, do not do anything
    //
    // However, we'll need to retry waiting (if we do not hit a timeout)
    // because if we were called again it means that LF got bogus signal
    // and will need to retry the operation. We'd better get all the way
    // back and try again.
    //
    pState->fRetry = TRUE;
    ASSERT (dwWaitObjectIndex < pState->nObjectCount);

    dwStatus = WAIT_TIMEOUT;
    if (dwWaitObjectIndex < pState->nObjectCount)
    {
      // abort retries by LF (it may get bogus events when spinning is enabled)
      dwStatus = pState->pObjects[dwWaitObjectIndex].dwStatus;
      if (dwStatus == WAIT_OBJECT_0)
      {
        dwStatus = WAIT_TIMEOUT;
        pState->pObjects[dwWaitObjectIndex].dwStatus = dwStatus;
      }
    }

    return (dwStatus);
  }

  // OK, it's first pass (recurse down)
  if (dwObjectIndex != 0)
  {
    // caller couldn't acquire previous object without waiting; record wait handle
    pState->hHandles[pState->dwHandleIndex] = hWaitObject;
    pState->dwMap[pState->dwHandleIndex] = dwObjectIndex - 1;   // index of object that waits on this handle
    pState->dwHandleIndex += 1;
  }

  for (; dwObjectIndex != pState->nObjectCount; ++dwObjectIndex)
  {
    //
    // process next object
    //
    pObject = pState->pObjects + dwObjectIndex;

    if (pObject->dwAction == LF_SYSTEM_HANDLE)
    {
      // do not call on system handle, it'll be handled later
      continue;
    }

    if (pObject->dwStatus != WAIT_TIMEOUT)
    {
      // already got this one
      continue;
    }

    // next call to LFMultiWaitCb will need to handle next object
    pState->dwObjectIndex = dwObjectIndex + 1;

    // call the object
    dwStatus = LFMultiCallObject (pState, pObject, LFMultiWaitAllCb, dwMilliseconds, fAlertable);

    // restore pState->dwObjectIndex
    pState->dwObjectIndex = dwWaitObjectIndex + 1;

    // save status
    pObject->dwStatus = dwStatus;

    // see whether we need to revert the status of entire operation
    if (pState->dwStatus == WAIT_OBJECT_0 && dwStatus != WAIT_OBJECT_0)
      pState->dwStatus = dwStatus;

    // see whether we need to proceed further
    if (pState->fWaitCompleted)
    {
      // we already processed all objects; return the status of the wait
      dwStatus = WAIT_TIMEOUT;
      if (dwWaitObjectIndex < pState->nObjectCount)
        dwStatus = pState->pObjects[dwWaitObjectIndex].dwStatus;
      return (dwStatus);
    }

    if (dwStatus != WAIT_OBJECT_0)
    {
      // the call failed; do not even try to process remaining objects
      pState->fWaitCompleted = TRUE;
      pState->dwStatus = dwStatus;
      return (WAIT_TIMEOUT);
    }
  }

  // processed all objects; need to wait
  pState->fWaitCompleted = TRUE;
  dwObjectIndex = pState->dwHandleIndex;

  // wait on all handles at ones
  dwStatus = LFMultiWait (pState, dwObjectIndex, TRUE, dwMilliseconds, fAlertable);

  // set the status
  dwStatus = LFMultiWaitAllSetStatus (pState, dwStatus, dwObjectIndex);

  // wait was satisfied instantly (it should be LF object); keep going
  if (pState->dwStatus == WAIT_TIMEOUT)
    pState->dwStatus = dwStatus;

  return (dwStatus);
}


DWORD                           // returns (WAIT_OBJECT_0 + n) if operation on n-th LF object succeeds
LFAPI                           // returns (WAIT_OBJECT_0 + nObjectCount + n) if wait on n-th user-defined handle succeeds
LFProcessMultipleObjects (      // otherwise, return the same result as WaitForMultipleObjectsEx would
  PVOID            pWaitContext,// user-defined context for "pWaitMultiCb"
  LFWAITMULTI     *pWaitMultiCb,// user-defined WaitForMultipleObject[Ex]-like function (if NULL WaitForMultipleObjectEx will be used)
  DWORD            nObjectCount,// number of LF objects referenced in "pObjects" array
  LFMULTI         *pObjects,    // array of desciptors of LF objects and required operations
  BOOL             fWaitAll,    // see WaitForMultipleObjects[Ex]
  BOOL             fWaitSpin,   // if TRUE then waiting threads will spin before actual wait
  DWORD            dwMilliseconds,// timeout in milliseconds
  BOOL             fAlertable   // if TRUE then allow I/O completion routines calls and/or APC while waiting
)
{
  DWORD          dwObjectIndex;
  LF_MULTI_STATE State, *pState = &State;
  DWORD          dwTickCount0;
  DWORD          dwElapsed;
  DWORD          dwTimeout;
  DWORD          dwStatus;
  LFMULTI       *pObject;

  if (pObjects == 0 || (nObjectCount - 1) > (MAXIMUM_WAIT_OBJECTS - 1))
  {
    // you should give me somethings
    return (ERROR_INVALID_PARAMETER);
  }

  pState->pWaitContext   = pWaitContext;
  pState->pWaitMultiCb   = pWaitMultiCb;
  pState->nObjectCount   = nObjectCount;
  pState->pObjects       = pObjects;
  pState->fWaitSpin      = fWaitSpin;
  pState->dwStatus       = WAIT_TIMEOUT;
  pState->fRetry         = FALSE;
  pState->fTimeout       = FALSE;

  dwTimeout = dwMilliseconds;
  dwTickCount0 = 0;         // keep compiler happy
  if (dwMilliseconds != INFINITE && (dwMilliseconds - 1) < 0x3fffffff)
    dwTickCount0 = GetTickCount ();

  //
  // copy arguments to state descriptor
  //
  do
  {
    //
    // check dwAction and set dwStatus
    //
    if (!pState->fRetry || !fWaitAll)
    {
      for (dwObjectIndex = 0; dwObjectIndex < nObjectCount; ++dwObjectIndex)
      {
        if ((pObjects[dwObjectIndex].dwAction >> 4) > (LF_SYSTEM_HANDLE >> 4))
        {
          return (ERROR_INVALID_PARAMETER);
        }

        pObjects[dwObjectIndex].dwStatus = WAIT_TIMEOUT;
      }
    }

    pState->dwObjectIndex  = 0;
    pState->dwHandleIndex  = 0;
    pState->fWaitCompleted = FALSE;
    pState->fRetry         = FALSE;

    if (fWaitAll)
    {
      // try to get as many events as possible
      LFMultiWaitAllCb (&State, NULL, dwMilliseconds, fAlertable);
    }
    else
    {
      // get first available event, set status for remaining ones to WAIT_TIMEOUT, and return
      LFMultiWaitAnyCb (&State, NULL, dwMilliseconds, fAlertable);
    }

    // recompute timeout
    if (dwMilliseconds != INFINITE && (dwMilliseconds - 1) < 0x3fffffff)
    {
      dwTimeout = 0;
      dwElapsed = GetTickCount () - dwTickCount0;
      if (dwMilliseconds > dwElapsed)
        dwTimeout = dwMilliseconds - dwElapsed;
    }
  }
  while (pState->dwStatus == WAIT_TIMEOUT && pState->fRetry && dwTimeout != 0 && !pState->fTimeout);

  if (fWaitAll)
  {
    dwStatus = pState->dwStatus;
    if (dwStatus == WAIT_OBJECT_0)
    {
      // acquired all LF objects; now wait for system handles
      pState->dwHandleIndex = 0;
      for (dwObjectIndex = 0; dwObjectIndex != pState->nObjectCount; ++dwObjectIndex)
      {
        //
        // process next object
        //
        pObject = pObjects + dwObjectIndex;

        if (pObject->dwAction == LF_SYSTEM_HANDLE)
        {
          // enqueue system handle to wait on
          pState->hHandles[pState->dwHandleIndex] = pObject->Handle;
          pState->dwMap[pState->dwHandleIndex] = dwObjectIndex;
          pState->dwHandleIndex += 1;
        }
      }

      // now wait on all system handles at once
      dwObjectIndex = pState->dwHandleIndex;

      // wait on all handles at ones
      dwStatus = LFMultiWait (pState, dwObjectIndex, TRUE, dwTimeout, fAlertable);

      // set the status
      dwStatus = LFMultiWaitAllSetStatus (pState, dwStatus, dwObjectIndex);

      // see whether we need to revert the status of entire operation
      if (pState->dwStatus == WAIT_OBJECT_0 && dwStatus != WAIT_OBJECT_0)
        pState->dwStatus = dwStatus;
    }

    if ((dwStatus - WAIT_OBJECT_0) >= nObjectCount && (dwStatus - WAIT_ABANDONED_0) >= nObjectCount)
    {
      // waiting failed; undo as many LF operations as possible

      // walk through already acquired LF locks and release them
      for (dwObjectIndex = 0; dwObjectIndex != pState->nObjectCount; ++dwObjectIndex)
      {
        pObject = pState->pObjects + dwObjectIndex;
        if (pObject->dwStatus != WAIT_OBJECT_0)
          continue;

        LFMultiLockRelease (pObject, dwStatus);
      }
    }
  }

  return (pState->dwStatus);
}

#ifdef    AVRT_CODE_END
#pragma   AVRT_CODE_END
#endif /* AVRT_CODE_END */
