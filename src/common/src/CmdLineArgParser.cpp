
#include "CmdLineArgParser.h"
#include <strsafe.h>

using namespace RSLibImpl;

static char * ArgTypes = "ISDTL";

static const int c_MaxArgs = 512;

static char * ArgTypesDesc[] = {
    (char *)"int  ",
    (char *)"str  ",
    (char *)"dbl  ",
    (char *)"tog  ",
    (char *)"i64  ",
    (char *)"     "
};

char * CmdLineArgParser::m_ExtraArgs[c_MaxExtraArgs] = { 0 };
char * CmdLineArgParser::m_ProgramName = NULL;
int CmdLineArgParser::m_NExtraArgs = 0;

int
CmdLineArgParser::Process(CmdLineArg * args, int nArgs, int i, char *arg, char *usageStr, bool &error)
{
    error = false;
    char type = args[i].m_Type[0];
  
    if (type=='T')
    {
        *(bool *)args[i].m_Loc = !*(bool *)args[i].m_Loc;
        return 0;
    }
    else
    {
        if (!arg)
        {
            Usage(args, nArgs, usageStr);
            error = true;
            //Returning 0 here helps us avoid an access violation when somebody uses a 
            //switch that requires additional data but forgets the data itself.
            return 0;
        }
        else
        {
            switch (type)
            {
            case 'I': 
                *(int *)args[i].m_Loc = atoi(arg);
                break;
            case 'D': 
                *(double *)args[i].m_Loc = atof(arg);
                break;
            case 'L':
                *(Int64 *)args[i].m_Loc = _atoi64(arg);
                break;
            case 'S':
            {
                int len = atoi(args[i].m_Type+1);
                char *loc = (char *) args[i].m_Loc;
                if (FAILED(StringCchCopyA(loc, len, arg)))
                {
                    Usage(args, nArgs, usageStr);
                    error = true;
                    return 0;
                }
                break;
            }
            default:
                LogAssert(!"bad argument description");
                break;
            }
        }
    }
    return 1;
}

void
CmdLineArgParser::ShowConfiguration(CmdLineArg * args, int nArgs)
{
    int i = 0;
    printf("Argument Configuration\n"); 
    for (i = 0; i < nArgs; i++)
    {
        printf("  %-40s",args[i].m_Description);
        
        switch (args[i].m_Type[0])
        {
            case 'T':
            {
                if (*(bool*)args[i].m_Loc)
                {
                    printf("TRUE");
                }
                else
                {
                    printf("FALSE");
                }
                break;
            }
            case 'I':
                printf("%d",*(int*)args[i].m_Loc);
                break;
            case 'D':
                printf("%f",*(double*)args[i].m_Loc);
                break;
            case 'L':
                printf("%I64d",*(Int64*)args[i].m_Loc);
                break;
            case 'S':
                printf("%s",(char*)args[i].m_Loc);
                break;
            default:
                LogAssert(!"bad argument description");
                break;
        }
        printf("\n");
    }
}

bool
CmdLineArgParser::ProcessArgs(CmdLineArg * args, int nArgs, char **argv, char *usageStr)
{
    bool error = false;
    int i = 0;
    //
    // Grab Command Line Arguments
    //
    m_ProgramName = argv[0];

    int seen[c_MaxArgs];
    // we never expect more than 512 arguments in the command line!
    LogAssert(nArgs < c_MaxArgs);
    memset(seen, 0, nArgs * sizeof(int));

    while (*++argv)
    {
        char *arg = argv[0];
        if (arg[0] == '-')
        {
            if (arg[1] == '-')
            {
                for (i = 0; i < nArgs; i++)
                {
                    if (!_stricmp(args[i].m_Name, arg + 2))
                    {
                        seen[i] = 1;
                        if (!Process(args, nArgs, i, *++argv, usageStr, error))
                        {
                            if (error)
                            {
                                return false;
                            }
                            
                            argv--;
                        }
                        break;
                    }
                }
                if (i >= nArgs)
                {
                    Usage(args, nArgs ,usageStr);
                    return false;
                }
            }
            else
            {
                arg++;
                while (*arg)
                {
                    for (i = 0; i < nArgs; i++)
                    {
                        if (args[i].m_Key == *arg)
                        {
                            seen[i] = 1;
                            if (*++arg)
                            {
                                if (Process(args, nArgs, i, arg, usageStr, error))
                                {
                                    arg += strlen(arg);
                                }
                            }
                            else
                            {
                                if (!Process(args, nArgs, i, *++argv, usageStr, error))
                                {
                                    argv--;
                                }
                            }
                            if (error)
                            {
                                return false;
                            }
                            break;
                        }
                    }
                    if (i >= nArgs)
                    {
                        Usage(args, nArgs ,usageStr);
                        return false;
                    }
                }
            }
        }
        else
        {
            if (m_NExtraArgs > c_MaxExtraArgs)
            {
                LogAssert(!"too many extra arguments");
            }
            m_ExtraArgs[m_NExtraArgs++] = argv[0];
            m_ExtraArgs[m_NExtraArgs] = NULL;
        }
    }
    for (i = 0; i < nArgs; i++)
    {
        if (args[i].m_Required && !seen[i])
        {
            Usage(args, nArgs, usageStr);
            return false;
        }
    }
    return true;
}

void
CmdLineArgParser::Usage(CmdLineArg * args, int nArgs, char * usageStr) 
{
    if (usageStr)
    {
        fprintf(stderr, "%s\n", usageStr);
    }
    else
    {
        fprintf(stderr,"Usage: %s [--SWITCH [ARG]]\n", m_ProgramName);
    }
    fprintf(stderr,"  switch____________________type__default___description\n");
    
    for (int i = 0; i < nArgs; i++)
    {
        char type = args[i].m_Type[0];

        fprintf(stderr, "  -%c, --%-20s", args[i].m_Key, args[i].m_Name);

        fprintf(stderr, "%s",
                ArgTypesDesc[
                    type ?
                    strchr(ArgTypes, type) - ArgTypes:
                    strlen(ArgTypes)]);
        
        if (args[i].m_Required)
        {
            fprintf(stderr, "          ");
        }
        else
        {
            switch(type)
            {
                case 0:
                    fprintf(stderr, "          ");
                    break;
                case 'L':
                    fprintf(stderr, " %-9I64d", *(Int64*)args[i].m_Loc);
                    break;
                case 'S':
                    if (*(char*)args[i].m_Loc)
                    {
                        char *loc = (char*) args[i].m_Loc;
                        if (strlen(loc) < 10)
                        {
                            fprintf(stderr, " %-9s", loc);
                        }
                        else
                        {
                            fprintf(stderr, " %.7s..", loc);
                        }
                    }
                    else
                    {
                        fprintf(stderr, " (null)   ");
                    }
                    break;
                case 'D':
                    fprintf(stderr, " %-9.3f", *(double*)args[i].m_Loc);
                    break;
                case 'I':
                    fprintf(stderr, " %-9d", *(int *)args[i].m_Loc);
                    break;
                case 'T': case 'f': case 'F': 
                    fprintf(stderr, " %-9s", *(bool *)args[i].m_Loc?"true ":"false");
                    break;
            }
        }
        fprintf(stderr," %s\n", args[i].m_Description);
    }
}
