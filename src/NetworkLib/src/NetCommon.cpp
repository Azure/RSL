#include "stdafx.h"

#include "NetCommon.h"
#include "NetProcessor.h"
#include "BufferPool.h"
#include "NetPacket.h"

namespace RSLibImpl
{

static volatile bool s_initialized = 0;

static CRITSEC s_initLock;

//NetCounters     Netlib::m_Counters;
NetProcessor*   Netlib::m_Processor;
BufferPool*     Netlib::m_Pool;

#if 0
void NetCounters::Initialize(char *prefix)
{
    char lowLayerName[256];
    char packetLayerName[256];
    if (!prefix)
    {
        prefix = "";
    }

    sprintf(lowLayerName, "%s%s", prefix, "NetLibLowLayer");
    sprintf(packetLayerName, "%s%s", prefix, "NetLibPacketLayer");


    g_ctrNumReads =             Counters::CreateUint64Counter(lowLayerName, "NumReads",             CounterFlag_Rate);
    g_ctrNumWrites =            Counters::CreateUint64Counter(lowLayerName, "NumWrites",            CounterFlag_Rate);
    g_ctrNumConnects =          Counters::CreateUint64Counter(lowLayerName, "NumConnects",          CounterFlag_Rate);
    g_ctrNumConnectsSuccess =   Counters::CreateUint64Counter(lowLayerName, "NumConnectsSuccess",   CounterFlag_Rate);
    g_ctrNumConnectsFail =      Counters::CreateUint64Counter(lowLayerName, "NumConnectsFail",      CounterFlag_Rate);
    g_ctrNumAccepts =           Counters::CreateUint64Counter(lowLayerName, "NumAccepts",           CounterFlag_Rate);
    g_ctrNumAcceptsSuccess =    Counters::CreateUint64Counter(lowLayerName, "NumAcceptsSuccess",    CounterFlag_Rate);
    g_ctrNumAcceptsFail =       Counters::CreateUint64Counter(lowLayerName, "NumAcceptsFail",       CounterFlag_Rate);
    g_ctrBytesWritten =         Counters::CreateUint64Counter(lowLayerName, "BytesWritten",         CounterFlag_Number);
    g_ctrBytesRead =            Counters::CreateUint64Counter(lowLayerName, "BytesRead",            CounterFlag_Number);

    g_ctrNumActiveConnection =  Counters::CreateUint64Counter(packetLayerName,  "NumActiveConnections", CounterFlag_Number);
    g_ctrNumPacketsSent =       Counters::CreateUint64Counter(packetLayerName,  "NumPacketsSent",       CounterFlag_Rate);
    g_ctrSendPacketSize =       Counters::CreateUint64Counter(packetLayerName,  "SendPacketSize",       CounterFlag_Number);
    g_ctrNumPacketsReceived =   Counters::CreateUint64Counter(packetLayerName,  "NumPacketsReceived",   CounterFlag_Rate);
    g_ctrReceivePacketSize =    Counters::CreateUint64Counter(packetLayerName,  "ReceivePacketSize",    CounterFlag_Number);
    g_ctrNumPacketsDropped =    Counters::CreateUint64Counter(packetLayerName,  "NumPacketsDropped",    CounterFlag_Rate);
    g_ctrNumPacketsTimedOut =   Counters::CreateUint64Counter(packetLayerName,  "NumPacketsTimedOut",   CounterFlag_Rate);

    g_ctrNumReads->Set(0);
    g_ctrNumWrites->Set(0);
    g_ctrNumConnects->Set(0);
    g_ctrNumConnectsSuccess->Set(0);
    g_ctrNumConnectsFail->Set(0);
    g_ctrNumAccepts->Set(0);
    g_ctrNumAcceptsSuccess->Set(0);
    g_ctrNumAcceptsFail->Set(0);
    g_ctrBytesWritten->Set(0);
    g_ctrBytesRead->Set(0);

    g_ctrNumActiveConnection->Set(0);
    g_ctrNumPacketsSent->Set(0);
    g_ctrSendPacketSize->Set(0);
    g_ctrNumPacketsReceived->Set(0);
    g_ctrReceivePacketSize->Set(0);
    g_ctrNumPacketsDropped->Set(0);
    g_ctrNumPacketsTimedOut->Set(0);
}
#endif

bool Netlib::Initialize(NetProcessor *proc, 
                        BufferPool *pool,
                        char * /*countersPrefix*/)
{
    // Initialize Counters
//    m_Counters.Initialize(countersPrefix);

    // Initialize BufferPool        

    if (pool == NULL)
    {
        pool = new BufferPool();
    }
    m_Pool = pool;

    LogAssert(proc != NULL);

    m_Processor = proc;
    return true;
}

// This method can be called multiple times simultaneously.
// Returns true is netlib has already been initialized
bool Netlib::Initialize(int nThreads, 
                        BufferPool *pool,
                        char *countersPrefix)
{
    if (s_initialized == false)
    {
        AutoCriticalSection lock(&s_initLock);

        // Check the flag again after taking the lock. Another thread
        // might have initialized netlib inbetwen the time we checked
        // the flag and acquired the lock
        if (s_initialized)
        {
            return true;
        }
        
        if (nThreads <= 0)
        {
            nThreads = s_DefaultNThreads;
        }

        WSADATA data;
        int ret = WSAStartup(MAKEWORD(2,2), &data);
        if (ret != 0)
        {
            return false;
        }
        
        NetProcessor *proc = new NetProcessor();
        if (!proc->Start(nThreads))
        {
            return false;
        }
        if (!Netlib::Initialize(proc, pool, countersPrefix))
        {
            return false;
        }
        // register a function to call back during CRT exit.
        // This gives us a chance to stop the netlib threads
        // before CRT starts destroying static objects
        if (atexit(&Netlib::Stop) != 0)
        {
            return false;
        }
        s_initialized = true;
        return true;
    }
    return true;
}

void Netlib::Stop()
{
    AutoCriticalSection lock(&s_initLock);
    if (m_Processor)
    {
        m_Processor->Stop();
        m_Processor = NULL;
    }
    s_initialized = false;
}

NetProcessor* Netlib::GetNetProcessor()
{
    return m_Processor;
}

BufferPool* Netlib::GetBufferPool()
{
    return m_Pool;
}

bool Netlib::IsNetlibInitialized()
{
    return s_initialized;
}

} // namespace RSLibImpl
