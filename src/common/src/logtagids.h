//
// This file lists all the known tag values for logging.
//
// This file is included twice, with DeclareTag() defined to do different things, to initialize
// different data structures with the logging code.
//
// Add any new named log tags to this file
// The IDs MUST be sequential
// Update LogTag_End if you add a new one
//
// The value in quotes is the name under which it will appear in the log file;
// e.g. Filename="Foo"
//

// Use these as generic log tags parameters if you don't feel it makes sense to add a new
// log tag entry.  There really is no real overhead for adding more, though, so if you're
// going to log the same data item in more than a few places, you probably want to consider
// making a log tag for it, in case we want WatchDog to be able to key off it.
DeclareTag(0, LogTag_Int1,          "Int1",             LogTagType_Int32),
DeclareTag(1, LogTag_Int2,          "Int2",             LogTagType_Int32),
DeclareTag(2, LogTag_Int64_1,       "Int64_1",          LogTagType_Int64),
DeclareTag(3, LogTag_Int64_2,       "Int64_2",          LogTagType_Int64),
DeclareTag(4, LogTag_String1,       "String1",          LogTagType_String),
DeclareTag(5, LogTag_String2,       "String2",          LogTagType_String),
DeclareTag(6, LogTag_UInt1,         "UInt1",            LogTagType_UInt32),
DeclareTag(7, LogTag_UInt2,         "UInt2",            LogTagType_UInt32),
DeclareTag(8, LogTag_UInt641,       "UInt641",          LogTagType_UInt64),
DeclareTag(9, LogTag_UInt642,       "UInt642",          LogTagType_UInt64),
DeclareTag(10, LogTag_U32X1,         "Hex1",            LogTagType_Hex32), 
DeclareTag(11, LogTag_U32X2,         "Hex2",            LogTagType_Hex32),
DeclareTag(12, LogTag_U64X1,         "Hex64_1",         LogTagType_Hex64), 
DeclareTag(13, LogTag_U64X2,         "Hex64_2",         LogTagType_Hex64),
DeclareTag(14, LogTag_Float1,        "Float1",          LogTagType_Float),
#if defined(_M_AMD64)
DeclareTag(15, LogTag_Sizet1,       "Size_t1",      LogTagType_UInt64),
DeclareTag(16, LogTag_Sizet2,       "Size_t2",      LogTagType_UInt64),
#else
DeclareTag(15, LogTag_Sizet1,       "Size_t1",      LogTagType_UInt32),
DeclareTag(16, LogTag_Sizet2,       "Size_t2",      LogTagType_UInt32),
#endif

#if defined(_M_AMD64)
DeclareTag(17, LogTag_Ptr1,       "Ptr1",       LogTagType_Hex64),
DeclareTag(28, LogTag_Ptr2,       "Ptr2",       LogTagType_Hex64),
#else
DeclareTag(17, LogTag_Ptr1,       "Ptr1",       LogTagType_Hex32),
DeclareTag(18, LogTag_Ptr2,       "Ptr2",       LogTagType_Hex32),
#endif

DeclareTag(19, LogTag_Filename,     "Filename",          LogTagType_String),
DeclareTag(20, LogTag_ErrorCode,    "ErrorCode",        LogTagType_UInt32),
DeclareTag(21, LogTag_ThreadID,    "ThreadID",          LogTagType_Int32),

// ids for Netlib
DeclareTag(22, LogTag_Port,         "Port",             LogTagType_UInt32),
DeclareTag(23, LogTag_IP,           "IP",               LogTagType_String),
DeclareTag(24, LogTag_NumericIP,    "NumericIP",        LogTagType_Hex32),
DeclareTag(25, LogTag_LastError,    "LastError",        LogTagType_UInt32),


DeclareTag(26, LogTag_StatusCode, "StatusCode",      LogTagType_UInt32),
DeclareTag(27, LogTag_RSLMsg, "Msg", LogTagType_String),
DeclareTag(28, LogTag_RSLMsgLen, "MsgLen", LogTagType_UInt32),
DeclareTag(29, LogTag_Offset, "Offset", LogTagType_UInt64),
DeclareTag(30, LogTag_RSLState, "State", LogTagType_UInt32),
DeclareTag(31, LogTag_RSLMemberId, "MemberId", LogTagType_String),
DeclareTag(32, LogTag_RSLBallotId, "BallotId", LogTagType_UInt32),
DeclareTag(33, LogTag_RSLDecree, "Decree", LogTagType_UInt64),
DeclareTag(34, LogTag_RSLBallot, "Ballot", LogTagType_String),
DeclareTag(35, LogTag_RSLMsgVersion, "MsgVersion", LogTagType_UInt16),


// This must be the final tag, and it must have type None

DeclareTag(36, LogTag_End,         "End",               LogTagType_None),
