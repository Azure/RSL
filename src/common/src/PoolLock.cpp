
#include "lf.h"
#include "PoolLock.h"

using namespace RSLibImpl;

// Static initializers for PoolLock class

volatile PLFPOOL CPoolLockNR::s_LFPool = NULL;
volatile PLFPOOL CPoolLockR::s_LFPool = NULL;
volatile PLFPOOL CPoolLock::s_LFPool = NULL;
