#include "basic_types.h"
#include "logging.h"

namespace RSLibImpl
{

struct CmdLineArg
{
    char * m_Name;
    char   m_Key;
    char * m_Description;
    /* 
       "I" = integer
       "L" = long
       "D" = double (floating point)
       "T" = toggle
       "S80" = read string, 80 chars max
    */
    char * m_Type;
    void * m_Loc;
    bool   m_Required;
};

class CmdLineArgParser
{
    
public:

    static const int c_MaxExtraArgs = 100;
    static char * m_ExtraArgs[c_MaxExtraArgs];
    static char * m_ProgramName;
    static int m_NExtraArgs;
    
    /* Print out arguments and values
     */
    static void ShowConfiguration(CmdLineArg *arg, int nArgs);
    
    static void Usage(CmdLineArg * arg, int nArgs, char *usageStr);
    
    /* Process all arguments
     */
    static bool ProcessArgs(CmdLineArg * arg, int nArgs, char **argv, char *usageStr = 0);

 private:    
    static int Process(CmdLineArg * args, int nArgs, int i, char *arg, char *usageStr, bool &error);
};

} // namespace RSLibImpl
