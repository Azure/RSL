#pragma once
#include "basic_types.h"
#include "logging.h"
#include "scopeguard.h"
#include "constants.h"

namespace RSLibImpl
{

struct Utils
{
    static UInt64 CalculateChecksum(
        const void* blob, //Data to calculate checksum on
        const size_t dataSize //Number of data bytes
        );

};

} // namespace RSLibImpl
