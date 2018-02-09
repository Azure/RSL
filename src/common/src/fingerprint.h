#pragma once

#include "basic_types.h"
#include "logging.h"
#include "msn_fprint.h"

namespace RSLibImpl
{

class FingerPrint64
{
public:
    static FingerPrint64* GetInstance();

    UInt64 GetFingerPrint(
        const void* data,
        const size_t length
        );

    UInt64 GetFingerPrint(
        UInt64 init,
        const void* data,
        const size_t length
        );

    class FingerPrint64Init 
    {
    public:
        FingerPrint64Init();
        ~FingerPrint64Init();
    private:
        static UInt64 count;
    };
    static void Init();
    static void Dispose();
    FingerPrint64(UInt64 poly);
    FingerPrint64();
    ~FingerPrint64();
private:
    msn_fprint_data_t fp;
    static FingerPrint64* instance;
};

static FingerPrint64::FingerPrint64Init fpInit;

} // namespace RSLibImpl

