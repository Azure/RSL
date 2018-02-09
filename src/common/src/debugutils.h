/***************************************************************************
 *
 *  Copyright (C) Microsoft Corporation.  All Rights Reserved.
 *
 *  File:       DebugUtils.h
 *  Content:    Debug utilities
 *
 ***************************************************************************/
#pragma once

#include "windows.h"
#include "logassert.h"

namespace RSLibImpl
{

#ifdef TODO_OFF
#define    __TODO(e,m,n)
#define    _TODO(e,m,n)
#define TODO(e,m)
#else
#define __TODO(e,m,n)   message(__FILE__ "(" #n ") : TODO: " #e ": " m)
#define _TODO(e,m,n)    __TODO(e,m,n)
#define TODO(e,m)       _TODO(e,m,__LINE__)
#endif    // TODO_OFF

#ifdef ASSERT_OFF
#define    DBG_ASSERT( cond )
#else
#define    DBG_ASSERT( cond )    LogAssert( cond )
#endif    // ASSERT_OFF

#ifdef DEBUG
#define Dprintf printf
#else
#define Dprintf __noop
#endif

} // namespace RSLibImpl
