#include "StringUtils.h"

namespace RSLibImpl
{

// This works only for ASCII characters.
// Trivial implementaion of tolower
inline int CStringUtil::MyTolower(int ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        ch += 'a' - 'A';
    }
    return(ch);
}

// Case insensitive string buffer compare
// StringEqual("ab", "abc", 2) is true
// StringEqual("ab", "abc", 3) is false
// StringEqual("ab", "ab", 5) is true
bool CStringUtil::StringEqual(const char * str1, const char * str2, size_t cchBuffer)
{
    const char * str1Max = str1 + cchBuffer;

    while (str1 < str1Max && *str1 && *str2)
    {
        // Convert characters to lowercase before comparing them
        int chStr1 = MyTolower(*str1);
        int chStr2 = MyTolower(*str2);
        
        if (chStr1 == chStr2)
        {
            str1++;
            str2++;
        }
        else
        {
            return false;
        }
    }

    if ( (*str1 == *str2) || (str1 == str1Max) )
    {
        return true;
    }

    return false;
}

// Case-insensitive strstr
char * CStringUtil::FindStringI(const char * str1, const char * str2)
{
    return(FindStringIN(str1, strlen(str1), str2));
}

// Case-insensitive strstr with a buffer size for the string to be searched
char * CStringUtil::FindStringIN(const char * str1, size_t cchBuffer, const char * str2)
{
    const char *cp = str1;
    const char *s1;
    const char *s2;

    const char *s1Max = cp + cchBuffer;

    if ( !*str2 )
    {
        return((char *)str1);
    }

    while (cp < s1Max && *cp)
    {
        s1 = cp;
        s2 = str2;

        while (s1 < s1Max && *s1 && *s2)
        {
            // Convert characters to lowercase before comparing them
            int chBigString = MyTolower(*s1);
            int chLittleString = MyTolower(*s2);
            
            if (chBigString == chLittleString)
            {
                s1++;
                s2++;
            }
            else
            {
                break;    // strings don't match
            }        
        }

        if (!*s2)
        {
            return((char *)cp);
        }

        cp++;
    }

    return(NULL);
}

} // namespace RSLibImpl