#include <windows.h>

#include "logging.h"
#include "DirUtils.h"
#include <strsafe.h>

using namespace RSLibImpl;

bool DirUtils::MakeDirectory(const char* dirname)
{
    if (!dirname || !*dirname || strlen(dirname) >= _MAX_PATH)
    {
        Log(LogID_Common,LogLevel_Error,"DirUtils",
            "invalid directory name",
            dirname);
        return (false);
    }
    char fullpathname[_MAX_PATH];
    if (!_fullpath(fullpathname,dirname,sizeof(fullpathname)))
    {
        Log(LogID_Common,LogLevel_Error,"DirUtils",
            "invalid directory name: %s",
            dirname);
        return (false);
    }
    return (_MakeDirectory(fullpathname));
}

bool DirUtils::MakeDirectoryForFile(const char* filePathname)
{
    if (!filePathname || !*filePathname || strlen(filePathname) >= _MAX_PATH)
    {
        Log(LogID_Common,LogLevel_Error,"DirUtils",
            "invalid file pathname",
            filePathname);
        return (false);
    }
    char fullpathname[_MAX_PATH];
    if (!_fullpath(fullpathname,filePathname,sizeof(fullpathname)))
    {
        Log(LogID_Common,LogLevel_Error,"DirUtils",
            "invalid file pathname: %s",
            filePathname);
        return (false);
    }

    char *lastPath = strrchr(fullpathname, '\\');

    if (lastPath == NULL)
    {
        lastPath = strrchr(fullpathname, '/');
    }

    if (lastPath == NULL)
    {
         Log(LogID_Common,LogLevel_Error,"DirUtils",
            "invalid file pathname: %s",
            filePathname);
        return (false);
    }

    *lastPath = '\0';
    return (_MakeDirectory(fullpathname));
}

bool DirUtils::_MakeDirectory(const char* startdirname)
{
    DWORD attr = GetFileAttributesA(startdirname);
    //
    // If the directory does not exist, we will try to create it.
    //
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        char dirname[_MAX_PATH];
        HRESULT hr = StringCchCopyA(dirname, NUMELEM(dirname), startdirname);
        
        if (FAILED(hr))
        {
            Log(LogID_Common,LogLevel_Error,"DirUtils",
                "unable to copy startdirname '%s', hresult: %d",
                dirname, hr);
            return false;
        }

        //
        // Remove the trailing path separator from the directory.
        //
        size_t dirLength = strlen(dirname);
        while (dirLength && (dirname[dirLength - 1] == '\\' || dirname[dirLength - 1] == '/'))
        {
            dirname[--dirLength] = '\0';
        }
        //
        // Treat root directory as always there.
        //
        if (!dirLength)
        {
            return (true);
        }
        //
        // Try to make the parent directory since this directory is not there.
        //
        char *lastPath = strrchr(dirname, '\\');

        if (lastPath == NULL)
        {
            lastPath = strrchr(dirname, '/');
        }

        if (lastPath == NULL)
        {
             Log(LogID_Common,LogLevel_Error,"DirUtils",
                "invalid pathname: %s",
                startdirname);
            return (false);
        }        
        *lastPath = '\0';

        if (_MakeDirectory(dirname))
        {
            *lastPath = '\\';

            //
            // Since the parent directory already exists or was created,
            // we can attempt to create the target directory.
            //
            if (CreateDirectoryA(dirname,0))
            {
                return (true);
            }
             DWORD ec = GetLastError();
             // some other thread/process created the directory
             // between the time we checked for this directory
             // and created the parentdirectory
             if (ec == ERROR_ALREADY_EXISTS)
             {
                 return true;
             }
            Log(LogID_Common,LogLevel_Error,"DirUtils",
                "unable to create directory: %s, error %lu",
                dirname, (unsigned long) ec);
        }
        return (false);
    }
    //
    // The directory already exists.
    //
    if(attr & FILE_ATTRIBUTE_DIRECTORY)
    {
        return (true);
    }
    //
    // This is a regular file.
    //
    Log(LogID_Common,LogLevel_Error,"DirUtils",
        "pathname exists but is not a directory: %s",
        startdirname);
    return (false);
}


bool DirUtils::_DeleteDirectoryBase(const char * dirname, bool deleteRootDirectory)
{
    //Implemention is recursive. This is the entry point function that 
    //reserves some data space to work with. Given its recursive we'd like to limit
    //the memory used in the recursive part so we have a single char array for
    //building filepaths and append / strip from that as we move up and down 
    //the directory structure. Similarly we use the same FIND_DATA structure
    //at all levels in the recursion
    char fullpath[_MAX_PATH];
    WIN32_FIND_DATAA findData;

    size_t len=strlen(dirname);
    LogAssert(len<_MAX_PATH);
    memcpy(fullpath, dirname, len+1);

    //Call into the recursive part
    return _DeleteDirectoryBaseRecursive(fullpath, _MAX_PATH, len, &findData, deleteRootDirectory);
}


bool DirUtils::_DeleteDirectoryBaseRecursive(char * fullpath, size_t buffSize, 
                                                size_t pathLen, WIN32_FIND_DATAA * pFindData, bool deleteRootDirectory)
{
    //Make sure we've got space to add a \ * & null if needed
    if (pathLen+3>buffSize)
    {
        Log(LogID_Common,LogLevel_Error,"DirUtils",
                    "Path too long to extend. dir %s ", fullpath);
        return false;
    }

    //Make sure we've got a \ at the end so we can append to this to create full file paths
    if (fullpath[pathLen-1]!='\\')
    {
        fullpath[pathLen]='\\';
        pathLen++;
    }

    //Create a pattern to match everything within the directory
    fullpath[pathLen]='*';
    fullpath[pathLen+1]=0;

    HANDLE hFind=FindFirstFileA(fullpath, pFindData);
    if (hFind==INVALID_HANDLE_VALUE)
    {
        //If directory is already gone then life is really easy
        DWORD dwLastError=GetLastError();
        if (dwLastError==ERROR_FILE_NOT_FOUND || dwLastError==ERROR_NO_MORE_FILES)
        {
            return true;
        }
    
        Log(LogID_Common,LogLevel_Error,"DirUtils",
                    "Couldn't do a FindFirstFileA on %s error %u", fullpath, dwLastError);
        return false;
    }

    //Attempt to iterate over the specified directory
    bool bIterationCompletedOK=false;
    for (;;)
    {
        //Ignore the special directory entries
        if (strcmp(".", pFindData->cFileName)!=0 && strcmp("..", pFindData->cFileName)!=0)
        {
            //Create the full path to the file or directory
            size_t fileNameLen=strlen(pFindData->cFileName);
            if (pathLen+fileNameLen+1>buffSize)
            {
                //path overflows buffer space we have
                Log(LogID_Common,LogLevel_Error,"DirUtils",
                    "Path too long to store. dir %s filename %s", fullpath, pFindData->cFileName);
                break;
            }
            memcpy(fullpath+pathLen, pFindData->cFileName, fileNameLen+1);

            //If we've got a directory then recurse into it. Otherwise attempt to remove the file
            if (pFindData->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (_DeleteDirectoryBaseRecursive(fullpath, buffSize, pathLen+fileNameLen, pFindData, true)==false)
                {
                    break;
                }
            }
            else 
            {
                if (DeleteFileA(fullpath)==FALSE)
                {
                    //If the file has already gone then thats OK
                    DWORD dwLastError=GetLastError();
                    if (dwLastError!=ERROR_FILE_NOT_FOUND)
                    {
                        Log(LogID_Common,LogLevel_Error,"DirUtils",
                            "Couldn't delete file %s error %u", fullpath, dwLastError);
                       break;
                    }
                }
            }
        }    

        //Move onto next entry in the directory
        if (FindNextFileA(hFind, pFindData)==FALSE)
        {
            //Make sure the reason this failed is because we've finished iterating over all the files
            DWORD dwLastError=GetLastError();
            if (dwLastError==ERROR_NO_MORE_FILES)
            {
                //Success for this directory!
                bIterationCompletedOK=true;
            }
            break;
        }
    }
    FindClose(hFind);

    //If we failed in any part of the above loop then give up
    if (bIterationCompletedOK==false)
    {
        return false;
    }  

    if(deleteRootDirectory)
    {
        //Now remove the actual directory itself
        fullpath[pathLen-1]=0;
         
        if (RemoveDirectoryA(fullpath)==FALSE)
        {
            DWORD dwLastError=GetLastError();
            if (dwLastError!=ERROR_FILE_NOT_FOUND)
            {
                Log(LogID_Common,LogLevel_Error,"DirUtils",
                    "Couldn't delete directory %s error %u", fullpath, dwLastError);
               return false;
            }
        }
    }

    return true;
}


DWORD DirUtils::PathExists(const char * path)
{
    if (!path || !*path)
    {
        Log(LogID_Common,LogLevel_Error,"DirUtils", "invalid path %s", path);
        return ERROR_INVALID_PARAMETER;
    }
  
    DWORD attr = GetFileAttributesA(path);

    //we should get an error if it does not exist
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        //make sure that the error we get is path not found
        DWORD dwError=GetLastError();
        if(dwError != ERROR_PATH_NOT_FOUND)
        {
            Log(LogID_Common,LogLevel_Error,"DirUtils", "got error %u trying to access path %s", dwError, path);
        }
        return dwError;
    }

    return ERROR_SUCCESS;
}

DWORD DirUtils::IsEmptyDirectory(const char * dirname)
{
    if (!dirname || !*dirname)
    {
        Log(LogID_Common,LogLevel_Error,"DirUtils", "invalid directory name %s", dirname);
        return ERROR_INVALID_PARAMETER;
    }
    
    char fullpath[_MAX_PATH];
    size_t len = strlen(dirname);

    if(len + 3 > _MAX_PATH)
    {
        Log(LogID_Common,LogLevel_Error,"DirUtils", "Path too long to create search pattern for dir %s ", fullpath);
        return ERROR_INVALID_PARAMETER;
    }

    memcpy(fullpath, dirname, len);

    //Make sure we've got a \ at the end so we can append to this to create full file paths
    if (fullpath[len-1]!='\\')
    {
        fullpath[len]='\\';
        len++;
    }
    
    //Create a pattern to match everything within the directory
    fullpath[len]='*';
    fullpath[len+1]=0;

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(fullpath, &findData);
    
    if (hFind==INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }
    
    bool dirEmpty = true;
    //check folder contents ignoring [.] and [..] until we find the first real entry
    do
    {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || 
            (strcmp(findData.cFileName, ".")!=0 && strcmp(findData.cFileName, "..")!=0))
        {
            dirEmpty = false;
            break;
        }        
    }while(FindNextFileA(hFind, &findData));

    FindClose(hFind);

    //check that we didn't encounter an error
    if(dirEmpty)
    {
        DWORD dwError=GetLastError();
        if(dwError != ERROR_NO_MORE_FILES)
        {
            return dwError;
        }

        return ERROR_SUCCESS;
    }
    
    return ERROR_DIR_NOT_EMPTY;
}

