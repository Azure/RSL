#pragma once

// TODO
//#include "counters.h"
#include "basic_types.h"

namespace RSLibImpl
{

class NetProcessor;
class BufferPool;
class PacketFactory;
static const int s_DefaultNThreads = 4;

#if 0
class NetCounters
{
public:
    // Lower Layer counters
    Uint64Counter*  g_ctrNumReads;                  // Number of Read Calls
    Uint64Counter*  g_ctrNumWrites;                 // Number of Write Calls
    Uint64Counter*  g_ctrNumConnects;               // Number of Successful Connects
    Uint64Counter*  g_ctrNumConnectsSuccess;        // Number of Successful Connects
    Uint64Counter*  g_ctrNumConnectsFail;           // Number of Failed Connects
    Uint64Counter*  g_ctrNumAccepts;                // Number of Accepts
    Uint64Counter*  g_ctrNumAcceptsSuccess;         // Number of Successful Accepts
    Uint64Counter*  g_ctrNumAcceptsFail;            // Number of Failed Accepts
    Uint64Counter*  g_ctrBytesWritten;              // Total Bytes Written
    Uint64Counter*  g_ctrBytesRead;                 // Total Bytes Read

    // Packet Layer counters
    Uint64Counter*  g_ctrNumActiveConnection;       // Number of Active Connections
    Uint64Counter*  g_ctrNumPacketsSent;            // Number of Packets Sent
    Uint64Counter*  g_ctrSendPacketSize;            // Send Packet Size
    Uint64Counter*  g_ctrNumPacketsReceived;        // Number of Packets Received
    Uint64Counter*  g_ctrReceivePacketSize;         // Receive Packet Size
    Uint64Counter*  g_ctrNumPacketsDropped;         // Number of dropped packets
    Uint64Counter*  g_ctrNumPacketsTimedOut;        // Number of packets that timed out

    // Initialize
    // This method initializes all the counters. The sections names 
    // ("NetLibLowLayer" and "NetLibPacketLayer") are prepended with
    // the prefix, if one is specified
    void Initialize(char *prefix = NULL);
};

#endif

class Netlib
{
public:

    // Initialize
    // One of the following two variations should be called to 
    // initialize Netlib. This will set the internal NetProcessor,
    // BufferPool and initialize counters
    //
    // INPUT PARAMETERS
    // <nThreads> [int]
    // Number of threads to start in NetProcessor. If this argument
    // is 0, a default value is used
    //
    // <proc> [NetProcessor *]
    // A client owned netprocessor to use. It must be started 
    // before calling this method. If the user doesn't want to 
    // create their own NetProcessor, the first variation of 
    // Initilize() (with nThreads) should be used instead
    //
    // <pool> [BufferPool *]
    // A client owned buffer pool to be used by netlib. If NULL, 
    // a default pool is created
    //
    // <countersPrefix> [char *]
    // Prefix for the netlib counters. This is to distinguish 
    // between the counters from two different processes using
    // netlib and running on the same machine
    //
    // RETURN VALUES
    // true: If the netlib was initialized successfully
    // false: otherwise
    //
    // NOTES
    // If using the second version below and passing a NetProcessor,
    // the caller must start the NetProcessor before invoking 
    // Initialize()
    static bool Initialize(int nThreads = 0, 
                    BufferPool *pool = NULL,
                    char *countersPrefix = NULL);
    static bool Initialize(NetProcessor *proc, 
                    BufferPool *pool = NULL,
                    char *countersPrefix = NULL);

    // Stops the netProcessor. Note that it doesn't
    // deallocate the memory - it terminates the netlib threads.
    static void __cdecl Stop();
    
    // GetNetProcessor
    // Returns the NetProcessor being used by the netlib in the context
    // of the current process
    static NetProcessor* GetNetProcessor();

    // GetBufferPool
    // Returns the BufferPool being used by the netlib in the context
    // of the current process
    static BufferPool*  GetBufferPool();

    // Exposes the static variable s_initialized
    // Can be used to query if netlib has been initialized.
    static bool IsNetlibInitialized();       

    // Counters
    //static NetCounters m_Counters;

private:
    static NetProcessor*    m_Processor;
    static BufferPool*      m_Pool;
};

} // namespace RSLibImpl
