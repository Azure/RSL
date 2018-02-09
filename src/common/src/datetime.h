#pragma once

#include <windows.h>
#include <baseTsd.h>

namespace RSLibImpl
{

class APDateTime
{

public:
    APDateTime();
    APDateTime(const APDateTime &dt):m_dt(dt.m_dt){};
    APDateTime(FILETIME t) : m_dt(t) {};

    static APDateTime ConvertToDateTime(ULONG64 ui)
    {
        APDateTime dt;
        ULARGE_INTEGER uiTmp;
        uiTmp.QuadPart = ui;
        dt.m_dt.dwHighDateTime = uiTmp.HighPart;
        dt.m_dt.dwLowDateTime = uiTmp.LowPart;
        return dt;
    };
    
    ULONG64 ConvertToUInt64() const
    {
        ULARGE_INTEGER ui;
        ui.HighPart = m_dt.dwHighDateTime;
        ui.LowPart = m_dt.dwLowDateTime;
        return ui.QuadPart;
    }

    bool Parse(const char *cdt, size_t len=-1);

    bool Set(const SYSTEMTIME &st);

    void Set(FILETIME t)
        {   m_dt=t; };    

    bool GetAsSystemTime(SYSTEMTIME * pSystemTime) const;

    //Returns this datetime minus the supplied datetime in seconds
    LONG64  DiffInSecond(const APDateTime &dt) const;
    
    LONG64  operator-(const APDateTime &dt) const;
    APDateTime operator-(UINT diff);
    APDateTime & operator-= (UINT diff);
    APDateTime operator+(UINT   diff);
    APDateTime & operator+= (UINT diff);
    bool operator> (const APDateTime &dt) const;
    bool operator< (const APDateTime &dt) const;
    bool operator>= (const APDateTime &dt) const;
    bool operator<= (const APDateTime &dt) const;
    bool operator== (const APDateTime &dt) const;
    bool operator!= (const APDateTime &dt) const;
    void ToString(char *buf, size_t len) const;
    void ToString2(char *buf, size_t len) const; //yyyy-mm-dd hh:mm:ss.sss

    //MinTime is 1900-1-1 0:0:0.0
    static APDateTime MinTime();
    static APDateTime Now();

private:
    FILETIME m_dt;
};

} // namespace RSLibImpl
