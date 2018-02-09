/*---------------------------------------------------------------------*

  internal\DynString.h - defines a simple str with parameterized static buffer size.

 *---------------------------------------------------------------------*/

#pragma once

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "DynamicBuffer.h"

namespace RSLibImpl
{

// a minimal/lightweight class for string conversions

struct DynStringArgC {
  size_t m_Len;
  const char * m_Ptr;

  DynStringArgC(const char *s = NULL, size_t len=-1) :
      m_Len(s ? (-1 == len ? strlen(s) : len) : 0),
      m_Ptr(s ? s : "") {}

  const char * Str() const { return m_Ptr; }

  size_t Length() const { return m_Len; }

  operator const char *() const { return m_Ptr; }

  bool
  operator==(const DynStringArgC& x) const
  {
    if (m_Len != x.m_Len)
    {
        return false;
    }
    return memcmp(m_Ptr, x.m_Ptr, m_Len) == 0;
  }
};

/*---------------------------------------------------------------------*

  a simple string with template static buffer size

 *---------------------------------------------------------------------*/

template <size_t S>
class DynStringT
{
private:
    typedef DynStringT<S> MyT;
    typedef DynamicBuffer<char, S + 1> BufferT; // extra byte for null

public:

    DynStringT() : m_Len(0) { m_Buf[0] = '\0'; }

    DynStringT(const MyT& s) : m_Len(s.m_Len) { m_Buf.Copy(s.m_Buf, m_Len+1); }

    explicit DynStringT(const DynStringArgC& s) : m_Len(0) { Append(s.m_Ptr, s.m_Len); }

    DynStringT(const char *str, size_t len) : m_Len(0) { Append(str, len); }

    explicit DynStringT(size_t n) : m_Len(0), m_Buf((int) n) { m_Buf[0] = '\0'; }

    ~DynStringT() {}

    size_t Length() const { return m_Len; }

    bool Empty() const { return m_Len == 0; }

    const char *Str() const { return m_Buf.Begin(); }

    char* Buffer() const { return m_Buf.Begin(); }

    operator char *() const { return m_Buf.Begin(); }

    operator const DynStringArgC() const { return DynStringArgC(m_Buf, m_Len); }

    MyT& operator=(const MyT& copy)
    {
        if (this == &copy)
        {
            return *this;
        }
        m_Len = copy.m_Len;
        m_Buf.Copy(copy.m_Buf, m_Len + 1);
        return *this;
    }
    MyT& operator=(const DynStringArgC& copy)
    {
        if (Str() == copy.m_Ptr)
        {
            return *this;
        }
        m_Len = 0;
        Append(copy.m_Ptr, copy.m_Len);
        return *this;
    }

    MyT& operator=(const char *str)
    {
        if (Str() == str)
        {
            return *this;
        }
        Set(str);
        return *this;
    }

    char operator*() const { return *m_Buf; }
    char& operator[](int i) { LogAssert(((size_t) i) <= m_Len); return  m_Buf[i]; }
    char& operator[](size_t i) { LogAssert(i <= m_Len); return  m_Buf[i]; }

    int Pop()
    {
        if (!m_Len)
        {
            return EOF;
        }
        int c = m_Buf[--m_Len];
        m_Buf[m_Len] = '\0';
        return c;
    }

    // comparison operators

    // like strcmp, returns a negative, zero, or positive value
    static int Compare(const MyT& s1, const MyT& s2, bool ignoreCase)
    {
        size_t cmplen = min(s1.Length(), s2.Length());
        if (cmplen > 0)
        {
            int cmp;
            if (ignoreCase)
            {
                cmp = _strnicmp(s1.Str(), s2.Str(), cmplen);
            }
            else
            {
                cmp = strncmp(s1.Str(), s2.Str(), cmplen);
            }

            if (cmp != 0)
            {
                return cmp;
            }
        }
        if (s1.Length() == s2.Length())
        {
            return 0;
        }
        else if (s1.Length() < s2.Length())
        {
            return -1;
        }
        return 1;
    }

    static int Compare(const MyT &s1, const MyT &s2)
    {
        return Compare(s1, s2, true);
    }

    int Compare(const char *s1, bool ignoreCase)
    {
        if (ignoreCase)
        {
            return stricmp(m_Buf, s1);
        }
        return strcmp(m_Buf, s1);
    }

    bool operator==(const MyT& peer) const
    {
        if (m_Len != peer.m_Len)
        {
            return false;
        }
        return memcmp(m_Buf, peer.m_Buf, m_Len) == 0;
    }

    bool operator==(const char *peer) const
    {
        return strcmp(m_Buf, peer) == 0;
    }

    bool Equals(const MyT& peer) const
    {
        return *this == peer;
    }

    bool Equals(const char *peer) const
    {
        return *this == peer;
    }

    static bool Equals(const MyT &str1, const MyT &str2)
    {
        return str1.Equals(str2);
    }

    bool operator!=(const MyT& peer) const
    {
        return !(*this == peer);
    }

    bool operator!=(const char *peer) const
    {
        return !(*this == peer);
    }

    bool operator<(const MyT& peer) const
    {
        return (strcmp(m_Buf, peer.m_Buf) < 0);
    }

    bool operator <=(const MyT& peer) const
    {
        return (strcmp(m_Buf, peer.m_Buf) <= 0);
    }

    bool operator >(const MyT& peer) const
    {
        return (strcmp(m_Buf, peer.m_Buf) > 0);
    }

    bool operator >=(const MyT& peer) const
    {
        return (strcmp(m_Buf, peer.m_Buf) >= 0);
    }

    MyT& Set(size_t i, char c)
    {
        m_Buf[i] = c;
        return *this;
    }

    MyT& Set(const char *str, size_t len = -1)
    {
        Clear();
        if (str && len == (size_t) -1)
        {
            len = strlen(str);
        }
        return Append(str, len);
    }

    MyT& Append(char c)
    {

        char *p = m_Buf.Resize(m_Len + 2);
        p[m_Len++] = c;
        p[m_Len] = '\0';
        return *this;
    }

    MyT& Append(const char *str, size_t len)
    {
        if (str == NULL) return *this;
        size_t end = m_Len;
        m_Len += len;
        char *p = m_Buf.Resize(m_Len + 1);
        memcpy(p + end, str, len);
        p[m_Len] = '\0';
        return *this;
    }

    MyT& Append(const MyT& str)
    {
        return Append(str.Str(), str.m_Len);
    }


    MyT& Append(const DynStringArgC& str)
    {
        return Append(str.m_Ptr, str.m_Len);
    }

    MyT& AppendVA(const char *format, va_list ap)
    {
        int len = _vscprintf(format, ap);
        size_t end = m_Len;
        m_Len += len;
        char *p = m_Buf.Resize(m_Len + 1);
        int written = _vsnprintf_s(p + end, m_Buf.Size() - end, _TRUNCATE, format, ap);
        LogAssert(written == len, "written = %d, len = %d, previous end = %d, buf size = %d", written, len, (int)end, (int)m_Buf.Size());
        p[m_Len] = '\0';
        return *this;
    }

    MyT& AppendF(const char *format, ...)
    {
        va_list ap;
        va_start(ap, format);
        AppendVA(format, ap);
        va_end(ap);
        return *this;
    }

    MyT& Join(char **str, int count, char separator)
    {
        for (int i = 0; i < count; i++)
        {
            if (str[i] != NULL)
            {
                Append(str[i]);
            }
            Append(separator);
        }
        if (count > 0)
        {
            SetLength(m_Len -1);
        }
        return *this;
    }

    MyT& Clear()
    {
        m_Len = 0;
        *m_Buf = '\0';
        return *this;
    }

    MyT& SetLength(size_t n = (size_t)-1, bool allowShrink = false)
    {
        if ((size_t)-1 == n)
        {
            m_Len = strlen(m_Buf);
            return *this;
        }
        char *p = m_Buf.Resize(n + 1, true, allowShrink);
        m_Len = n;
        p[n] = '\0';
        return *this;
    }

    void Reserve(size_t len)
    {
        m_Buf.Resize(len+1);
    }

    void SizeTrim()
    {
        m_Buf.Resize(m_Len+1, true, true);
    }

    void Trim()
    {
        size_t i = 0;
        while (i < m_Len && isspace((unsigned char)m_Buf[i])) i++;
        if (i < m_Len && i != 0)
        {
            m_Len -= i;
            if (m_Len)
            {
                memmove(m_Buf.Begin(), &m_Buf[i], m_Len);
            }
            m_Buf[m_Len] = '\0';
        }
        while (m_Len > 0 && isspace((unsigned char ) m_Buf[m_Len - 1])) m_Len--;
        m_Buf[m_Len] = '\0';
    }

    // take ownership of the content, empty string. The buffer must
    // be freed using free().
    char * Detach()
    {
        m_Len = 0;
        return m_Buf.Detach();
    }

    char * GetDelim(FILE *in, int delim)
    {
        size_t s = 0;
        int c;
        size_t bufSize = m_Buf.Size();
        char *p = m_Buf.Begin();

        while ((c = getc(in)) != EOF && c != delim)
        {
            p[s++] = c;

            if (s == bufSize)
            {
                p = m_Buf.ReSize(s + 1);
                bufSize = m_Buf.Size();
            }
        }
        p[s] = '\0';
        m_Len = s;

        return (m_Len || c != EOF) ? m_Buf.begin() : 0;
    }


    // the right way to get a line from a stream, handles embedded nulls,
    // and carriage returns.
    char * GetLine(FILE *in)
    {
        if (!GetDelim(in, '\n'))
        {
            return NULL;
        }
        if ('\r' == m_Buf[m_Len - 1])
        {
            m_Buf[m_Len - 1] = '\0';
            --m_Len;
        }
        return m_Buf.Begin();
    }

    // Gets the string up to, but not including, any delimiter found in
    // delims.  Very much like strtok_r but doesn't overwrite the original
    // string and likely doesn't need to do any mallocs.
    //
    char *GetToken(const char *in, const char *delims, const char **lasts)
    {
        Clear();
        const char *cp = in;

        if (*lasts)
        {
            cp = *lasts;
        }

        // skip over leading delims
        while (*cp && strchr(delims, *cp))
        {
            cp++;
        }
        if (*cp == 0)
        {
            *lasts = cp;
            SetLength(0);
            return 0;
        }

        // find next delim
        const char *brk = strpbrk(cp, delims);
        if (brk == NULL)
        {
            size_t len = strlen(cp);
            Set(cp, len);
            *lasts = cp+len;
        }
        else
        {
            Set(cp, (int) (brk-cp));
            *lasts = brk;
        }
        return m_Buf.Begin();
    }

    int ToInt (int base = 10) const
    {
        char *end;
        return (int) strtoul(m_Buf.Begin(), &end, base);
    }

    MyT &ToLower()
    {
        _strlwr(m_Buf.Begin());
        return *this;
    }

    size_t BufSize() const { return m_Buf.Size(); }

 private:
    size_t    m_Len;
    BufferT    m_Buf;
};

typedef DynStringT<127> DynString;
typedef DynStringT<1> DynString1;
typedef DynStringT<1023> DynString1k;
typedef DynStringT<2047> DynString2k; // 2KB
typedef DynStringT<8191> DynString8k; // 8KB


} // namespace RSLibImpl
