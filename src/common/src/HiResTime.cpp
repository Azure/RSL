#include "basic_types.h"
#include "logging.h"
#include "HiResTime.h"
#include "Interlocked.h"

namespace RSLibImpl
{

// Since GetTickCount() is only 32 bits, it wraps around
// every 49.5 days. We have to handle the wrap around
// so that GetHiResTime() does not go back.
// If it has been c_halfWrapMs since we last updated
// s_baseMs, we add the difference to s_baseTimeInterval
// and update s_baseMs to current tick count.
// One optimization we do is to avoid acquiring
// a lock for just reading the s_baseTimeInterval and
// s_baseMs. Since these values are updated once every
// 25 days, acquiring a lock is very wasteful.
//
// Note that whenever we update s_baseMs, the most significant
// bit always toggles. So we can use that bit to
// synchronize access. Whenever we update the s_baseMs and
// s_baseTimeIntervaltime, we put the most significant bit of the
// base into the least significant bit of s_baseTimeInterval.
// If the reader reads these two values while the writer is
// in the middle of updating them, their bits won't match and
// the reader should retry reading these values until it
// gets the consistent values.

static const DWORD c_halfWrapMs = 0x80000000;

static volatile Int64 s_baseTimeInterval;
static volatile DWORD s_baseMs;
static CRITSEC s_elapseTimeLock;

// returns the most significant bit in a 32 bit integer
static UInt8 MSB(UInt32 x)
{
    return (UInt8) (x >> 31);
}

// returns the least significant bit in a 32 or 64 bit integer
static UInt8 LSB(UInt64 x)
{
    return (UInt8) (x & 1);
}

// remove the least significant bit
static Int64 VAL(Int64 value)
{
    return (value >> 1);
}

// puts the most signifcant bit of x into the least significant bit of value 
static Int64 COMBINE(Int64 value, UInt32 x)
{
    return ((value << 1) | MSB(x));
}

Int64 GetHiResTime()
{
    Int64 baseTimeInterval;
    DWORD baseMs;

    // read until baseTimeInterval and baseMs they are consistent. 
    do
    {
        baseMs = s_baseMs;
        baseTimeInterval = Interlocked::Read64(&s_baseTimeInterval);
    } while (LSB(baseTimeInterval) != MSB(baseMs));

    DWORD ms = GetTickCount();
    
    // If we exceed about 25 days, reset the base for the
    // clock so we don't miss a wrap.
    if ((ms - baseMs) > c_halfWrapMs)
    {
        AutoCriticalSection lock(&s_elapseTimeLock);
        // we have to do the comparision again since somebody else
        // could have update the base before we acquired the lock
        if ((ms - s_baseMs) > c_halfWrapMs)
        {
            Int64 interval = VAL(s_baseTimeInterval);
            // add the elapsed interval to the base.
            interval += (ms - s_baseMs);
            baseTimeInterval = COMBINE(interval, ms);
            s_baseMs = ms;
            // we have to change the value atomically.
            Interlocked::Exchange64(&s_baseTimeInterval, baseTimeInterval);
            LogAssert(LSB(s_baseTimeInterval) == MSB(s_baseMs));
        }
        baseMs = s_baseMs;
        baseTimeInterval = s_baseTimeInterval;
    }
    
    return ((VAL(baseTimeInterval) + (ms - baseMs)) * 1000);
}

} // namespace RSLibImpl
