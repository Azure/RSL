#include "utils.h"
#include "fingerprint.h"
#include <memory.h>

namespace RSLibImpl
{

//////////////////////////////////////////////////////////////////////////////
///  public static CalculateChecksum
///  Given a blob of data, get its fingerprint. This uses the code from MSR to 
///  actually calculate the fingerprint.
///
///  @param  blob     const void *  
///  @param  dataSize const size_t  
///
///  @return UInt64   
//////////////////////////////////////////////////////////////////////////////
    UInt64 Utils::CalculateChecksum(const void* blob, const size_t dataSize)
    {
        return FingerPrint64::GetInstance()->GetFingerPrint(blob, dataSize);
    }

}

