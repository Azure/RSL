#pragma once
#include <windows.h>

// The Interlocked64 APIs are only available on Windows Server 2003
// and later.  Even though the Interlocked64 operation are declared in
// winbase.h, they are not in kernel32, so the application breaks at
// runtime.

// Also, AMD64 machines do not have InterlockedCompareExchange64() in
// kernel32, since the function is an intrinsic, so it must be called
// directly.
//
// This file defines Interlocked64 operations on 32 bit platforms.
// On 64 bit platforms (AMD or IA64), the inlined functions
// call the intrinsic functions.

namespace RSLibImpl
{

namespace Interlocked
{
#if defined(_M_AMD64) || defined(_M_IA64) || defined(_M_ARM64)
    
    LONGLONG inline CompareExchange64(LONGLONG volatile* destination,
                                      LONGLONG comparand,
                                      LONGLONG exchange)
    {
        return ::InterlockedCompareExchange64(destination, comparand, exchange);
    }

    LONGLONG inline Increment64(LONGLONG volatile *Addend)
    {
        return ::InterlockedIncrement64(Addend);
    }
    
    LONGLONG inline Decrement64(LONGLONG volatile *Addend)
    {
        return InterlockedDecrement64(Addend);
    }

    LONGLONG inline Exchange64(LONGLONG volatile *Target, LONGLONG Value)
    {
        return ::InterlockedExchange64(Target, Value);
    }

    LONGLONG inline ExchangeAdd64(LONGLONG volatile *Addend, LONGLONG Value)
    {
        return ::InterlockedExchangeAdd64(Addend, Value);
    }
    
    LONGLONG inline Read64(LONGLONG volatile *target)
    {
        return *target;
    }
    
#else
    
    LONGLONG inline __cdecl CompareExchange64(LONGLONG volatile* destination,
                                              LONGLONG comparand,
                                              LONGLONG exchange)
    {
        __asm
            {
                mov   esi, [destination]
                mov   ebx, dword ptr [comparand]
                mov   ecx, dword ptr [comparand + 4]
                mov   eax, dword ptr [exchange]
                mov   edx, dword ptr [exchange + 4]
                lock  cmpxchg8b [esi]
            };
    }

    LONGLONG inline Increment64(LONGLONG volatile *Addend)
    {
        LONGLONG Old;

        do {
            Old = *Addend;
        } while (CompareExchange64(Addend, Old + 1, Old) != Old);
        return Old + 1;
    }

    LONGLONG inline Decrement64(LONGLONG volatile *Addend)
    {
        LONGLONG Old;

        do {
            Old = *Addend;
        } while (CompareExchange64(Addend, Old - 1, Old) != Old);
        return Old - 1;
    }

    LONGLONG inline Exchange64(LONGLONG volatile *Target, LONGLONG Value)
    {
        LONGLONG Old;

        do {
            Old = *Target;
        } while (CompareExchange64(Target, Value, Old) != Old);

        return Old;
    }

    LONGLONG inline ExchangeAdd64(LONGLONG volatile *Addend,
                                  LONGLONG Value)
    {
        LONGLONG Old;

        do {
            Old = *Addend;
        } while (CompareExchange64(Addend, Old + Value, Old) != Old);

        return Old;
    }

    LONGLONG inline Read64(LONGLONG volatile *target)
    {
        // As far as I know, this is the only way to atomically read a
        // 64 bit value on a 32 bit platform.
        // InterlockedCompareExchange64 reads the target value
        // atomically.  If the value is 0, it sets the value to 0 and
        // returns 0 (no-op) If the value is non-zero, it does not
        // change the value and returns old value.
        return CompareExchange64(target, 0, 0);
    }
    
#endif
};

} // namespace RSLibImpl
