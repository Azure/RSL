#include "DateTime.h"

#include "DynString.h"
#include <strsafe.h>

using namespace RSLibImpl;

APDateTime::APDateTime() 
{ 
    m_dt.dwHighDateTime = 0;
    m_dt.dwLowDateTime = 0;
}

bool 
APDateTime::Parse(const char *cdt, size_t len)
{
    if (0 == cdt)
    {
        Log(LogID_Common, LogLevel_Error, "APDateTime::Parse",
            LogTag_String1, "input string is NULL.",
            LogTag_End);
        return false;
    }

    if (-1 == len)
    {
        len = strlen(cdt);
    }

    if (0 == len)
    {
        *this = MinTime();
        return true;
    }

    DynString ddt(cdt, len);
    ddt.ToLower();
    const char *dt = ddt.Str();
    const char *delims = "/ :-.";
    enum Format
    {
        mdy = 0, //"MM/DD/YYYY HH:MM:SS AM",
        ymd,      //"YYYY-MM-DD HH:MM:SS AM", or "YYYY-MM-DD HH:MM:SS.MMM",
        formatCount
    };

    Format format = formatCount;
    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));

    DynString cur;
    int iField = 0, hour = 0;
    const char *last = dt;
    char *field = cur.GetToken(dt, delims, &last);
    while (field != NULL)
    {
        switch (iField)
        {
        case 0:
            if (cur.Length() > 2)
            {
                format = ymd;
                st.wYear = (WORD) cur.ToInt();
            }
            else
            {
                format = mdy;
                st.wMonth = (WORD) cur.ToInt();
            }
            break;
        case 1:
            if (ymd == format)
            {
                st.wMonth = (WORD) cur.ToInt();
            }
            else
            {
                st.wDay = (WORD) cur.ToInt();
            }
            break;
        case 2:
            if (ymd == format)
            {
                st.wDay = (WORD) cur.ToInt();
            }
            else
            {
                st.wYear = (WORD) cur.ToInt();
            }
            break;
        case 3:
            hour = cur.ToInt();
            st.wHour = (WORD) hour;
            break;
        case 4:
            st.wMinute = (WORD) cur.ToInt();
            break;
        case 5:
            st.wSecond = (WORD) cur.ToInt();
            break;
        case 6:
            if ((cur == "am") || (cur == "pm"))
            {
                if (hour == 12)
                {
                    st.wHour = (cur == "am") ? 0 : (WORD)hour;
                }
                else
                {
                    st.wHour = (WORD) hour + (cur == "pm" ? 12 : 0);
                }
            }
            else
            {
                st.wHour = (WORD) hour;
                st.wMilliseconds = (WORD) cur.ToInt();
            }
            break;
        }
        ++iField;
        field = cur.GetToken(last, delims, &last);
    }

    if (iField < 6)
    {
        Log(LogID_Common, LogLevel_Error, "APDateTime::Parse",
            LogTag_String1, "Field count does not match.",
            LogTag_String1, dt,
            LogTag_Int1, iField,
            LogTag_End);
        return false;
    }

    if (!SystemTimeToFileTime(&st, &m_dt))
    {
        DWORD error = GetLastError();
        Log(LogID_Common, LogLevel_Error, "APDateTime::Parse",
            LogTag_String1, "Failed SystemTimeToFileTime.",
            LogTag_String2, dt,
            LogTag_UInt1, error,
            LogTag_End);
        return false;
    }

    return true;
}

bool
APDateTime::Set(const SYSTEMTIME &st)
{
    if (!SystemTimeToFileTime(&st, &m_dt))
    {
        DynString sz;
        sz.AppendF("%d:%d:%d %d:%d:%d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        DWORD error = GetLastError();
        Log(LogID_Common, LogLevel_Error, "APDateTime::Parse",
            LogTag_String1, "Failed SystemTimeToFileTime.",
            LogTag_String2, sz.Str(),
            LogTag_UInt1, error,
            LogTag_End);
        return false;
    }

    return true;
}

LONG64
APDateTime::DiffInSecond(const APDateTime &dt) const
{
    ULARGE_INTEGER u1, u2;
    LONG64 u3;

    u1.HighPart = m_dt.dwHighDateTime;
    u1.LowPart = m_dt.dwLowDateTime;

    u2.HighPart = dt.m_dt.dwHighDateTime;
    u2.LowPart = dt.m_dt.dwLowDateTime;

    u3 = 10000000;//100nanoseconds to seconds
    return u1.QuadPart/u3 - u2.QuadPart/u3;
}

LONG64
APDateTime::operator- (const APDateTime &dt) const
{
    return DiffInSecond(dt);
}

APDateTime 
APDateTime::operator- (UINT diff)
{
    APDateTime dt = *this;
    dt -= diff;
    return dt;
}

APDateTime &
APDateTime::operator-= (UINT diff)
{
    ULARGE_INTEGER u;
    ULONG64 u1;

    u.HighPart = m_dt.dwHighDateTime;
    u.LowPart = m_dt.dwLowDateTime;
    
    u1 = diff;
    u1 *= 10000000; //100nanoseconds to seconds
    
    u.QuadPart = u.QuadPart - u1;
    m_dt.dwHighDateTime = u.HighPart;
    m_dt.dwLowDateTime = u.LowPart;
    return *this;
}

APDateTime 
APDateTime::operator+ (UINT diff)
{
    APDateTime dt = *this;
    dt += diff;
    return dt;
}

APDateTime & 
APDateTime::operator+= (UINT diff)
{
    ULARGE_INTEGER u;
    ULONG64 u1;

    u.HighPart = m_dt.dwHighDateTime;
    u.LowPart = m_dt.dwLowDateTime;
    
    u1 = diff;
    u1 *= 10000000; //100nanoseconds to seconds
    
    u.QuadPart = u.QuadPart + u1;
    m_dt.dwHighDateTime = u.HighPart;
    m_dt.dwLowDateTime = u.LowPart;
    return *this;
}

bool 
APDateTime::operator> (const APDateTime &dt) const
{
    return ConvertToUInt64() > dt.ConvertToUInt64();
}

bool 
APDateTime::operator< (const APDateTime &dt) const
{
    return ConvertToUInt64() < dt.ConvertToUInt64();
}

bool 
APDateTime::operator>= (const APDateTime &dt) const
{
    return ConvertToUInt64() >= dt.ConvertToUInt64();
}

bool 
APDateTime::operator<= (const APDateTime &dt) const
{
    return ConvertToUInt64() <= dt.ConvertToUInt64();
}

bool 
APDateTime::operator== (const APDateTime &dt) const
{
    return (dt.m_dt.dwHighDateTime == m_dt.dwHighDateTime &&
        dt.m_dt.dwLowDateTime == m_dt.dwLowDateTime);
}

bool 
APDateTime::operator!= (const APDateTime &dt) const
{
    return !(*this == dt);
}

void 
APDateTime::ToString(char *buf, size_t len) const
{
    *buf = 0;
    if (0 == m_dt.dwHighDateTime && 0 == m_dt.dwLowDateTime)
    {
        return;
    }
    SYSTEMTIME st;
    if (!GetAsSystemTime(&st))
    {
        return;
    }

    StringCbPrintfA(
        buf, 
        len, 
        "%.2d/%.2d/%.4d %.2d:%.2d:%.2d", 
        st.wMonth, 
        st.wDay, 
        st.wYear, 
        st.wHour, 
        st.wMinute, 
        st.wSecond);
}

//yyyy-mm-dd hh:mm:ss.sss
void 
APDateTime::ToString2(char *buf, size_t len) const
{
    *buf = 0;
    if (0 == m_dt.dwHighDateTime && 0 == m_dt.dwLowDateTime)
    {
        return;
    }
    SYSTEMTIME st;
    if (!GetAsSystemTime(&st))
    {
        return;
    }
    StringCbPrintf(
        buf, 
        len, 
        "%.4d-%.2d-%.2d %.2d:%.2d:%.2d.%.3d", 
        st.wYear, 
        st.wMonth, 
        st.wDay, 
        st.wHour, 
        st.wMinute, 
        st.wSecond, 
        st.wMilliseconds);
}

//MinTime is 1900-1-1 0:0:0.0
APDateTime
APDateTime::MinTime()
{
    APDateTime dt;
    SYSTEMTIME st;
    st.wYear = 1900;
    st.wMonth = 1;
    st.wDay = 1;
    st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;

    if (!SystemTimeToFileTime(&st, &dt.m_dt))
    {
        DWORD error = GetLastError();
        Log(LogID_Common, LogLevel_Error, "APDateTime::MinTime",
            LogTag_String1, "Failed SystemTimeToFileTime.",
            LogTag_UInt1, error,
            LogTag_End);
        dt.m_dt.dwHighDateTime = 0;
        dt.m_dt.dwLowDateTime = 0;
    }

    return dt;
}

APDateTime 
APDateTime::Now()
{
    APDateTime dt;
    GetSystemTimeAsFileTime(&(dt.m_dt));
    return dt;
}


bool APDateTime::GetAsSystemTime(SYSTEMTIME * pSystemTime) const
{
    if (!FileTimeToSystemTime(&m_dt, pSystemTime))
    {
        DWORD error = GetLastError();
        Log(LogID_Common, LogLevel_Error, "APDateTime",
            LogTag_String1, "Failed FileTimeToSystemTime.",
            LogTag_UInt1, error,
            LogTag_End);
        return false;
    }    
    return true;
}
