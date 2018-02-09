#pragma once

namespace RSLibImpl
{

// Add your log entries (areas) here
// This list must be kept in sync with g_LogIDNames[] below
// The list must start at 0 and increase sequentially
typedef enum
{
	// IMPORTANT:
	// IF YOU UPDATE THIS ENUM, MAKE SURE YOU DO CONSEQUENTLY WITH file ManagedRSLib.h, enum NotificationLevel
    LogID_Logging, // The logging system can log stuff too!
    LogID_Common,
    LogID_Netlib,
    LogID_RSLLIB,
    LogID_NetlibCorruptPacket,
    // This must be the last entry
    LogID_Count
} LogID;

extern char *g_LogIDNames[];

#ifdef DECLARE_DATA

// This is an array of possible log entries (areas)
// This must be kept in sync with the LogID enumeration above
char *g_LogIDNames[] =
{
    "Logging",
    "Common",
    "Netlib",
    "RSL",
    "NetlibCorruptPacket",
    // Last entry must be a NULL (for sanity checking)
    NULL
};

#endif

} // namespace RSLibImpl
