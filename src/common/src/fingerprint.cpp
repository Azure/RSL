#include "fingerprint.h"
#include "utils.h"

using namespace RSLibImpl;

UInt64 FingerPrint64::FingerPrint64Init::count;
FingerPrint64* FingerPrint64::instance;

FingerPrint64::FingerPrint64Init::FingerPrint64Init()
{
    if (0 == count)
    {
        FingerPrint64::Init();
    }
    ++count;
}

FingerPrint64::FingerPrint64Init::~FingerPrint64Init()
{
    LogAssert(count > 0);
    --count;
    if (0 == count)
    {
        FingerPrint64::Dispose();
    }
}

void FingerPrint64::Init()
{
    FingerPrint64::instance = new FingerPrint64();
}

void FingerPrint64::Dispose()
{
    delete FingerPrint64::instance;
    FingerPrint64::instance = 0;
}

FingerPrint64* FingerPrint64::GetInstance()
{
    return FingerPrint64::instance;    
}

UInt64 FingerPrint64::GetFingerPrint(UInt64 init, const void *data, const size_t length)
{
    return msn_fprint_of(this->fp, init, (void*) data, (size_t) length);
}

UInt64 FingerPrint64::GetFingerPrint(const void *data, const size_t length)
{
    return msn_fprint_of(this->fp, (void*) data, (size_t) length);
}

FingerPrint64::FingerPrint64(UInt64 poly)
{
    this->fp = ::msn_fprint_new(poly);
}

FingerPrint64::FingerPrint64(void)
{
    this->fp = ::msn_fprint_new();
}

FingerPrint64::~FingerPrint64(void)
{
    ::msn_fprint_destroy(this->fp);
}
