
#pragma once

#include "basic_types.h"
#include "logging.h"

namespace RSLibImpl
{

/**
 * This class provides directory related utilities.
 */
class DirUtils
{
public:

    /**
     * Creates a directory with all the intermediate subdirectories as needed.
     *
     * Returns false if the directory or subdirectories do not exist and
     * could not be created.
     */
    static bool MakeDirectory(const char* dirname);

    /**
     * Creates a directory for a file pathname.
     *
     * The pathname must contain the directory and the file name components.
     * This method will create the directory as needed, creating all the
     * intermediate subdirectories as needed.
     *
     * This method may be used to make sure that a directory exists before
     * a file is created there.
     *
     * Returns false if the path does not exist and could not be created.
     */
    static bool MakeDirectoryForFile(const char* filePathname);


    /**
     * Deletes a directory and everything within it
     *
     * This is recursive, so all subdirectories and their contents will also be deleted
     * 
     * Returns false if any part of the operation fails and it can't do the delete.
     */
    static bool DeleteDirectoryAndContents(const char * dirname)
    { return _DeleteDirectoryBase(dirname, true); };


    /**
     * Deletes everything within a directory but not the directory itself
     *
     * This is recursive, so all subdirectories and their contents will also be deleted
     * 
     * Returns false if any part of the operation fails and it can't do the delete.
     */
    static bool ClearDirectory(const char * dirname)
    { return _DeleteDirectoryBase(dirname, false); };
    

    /**
     * Checks if a folder is empty
     *
     * All folders will contain [.] and [..] so these are ignored
     *
     * Returns ERROR_SUCCESS if the folder is empty, ERROR_DIR_NOT_EMPTY if the folder has contents,
     * or the winerror.h error code for the error encountered trying to check the folder
     */
    static DWORD IsEmptyDirectory(const char * dirname);

    /**
     * Checks if the given path exists
     *
     * Returns ERROR_SUCCESS if the path exists, ERROR_PATH_NOT_FOUND if it doesn't
     * or the winerror.h error code for the error encountered trying to verify the path
     */
    static DWORD PathExists(const char * path);

private:

    /**
     * This class has no constructor - all the methods are static.
     */
    DirUtils();

private:

    /**
     * Creates a directory with all the intermediate subdirectories as needed.
     *
     * This is an internal method that expects a fully qualified pathname.
     * Returns false if the directory or subdirectories do not exist and
     * could not be created.
     */
    static bool _MakeDirectory(const char* dirname);


    /**
     * Deletes a directory and everything within it
     *
     * This is an internal method that calls itself recursively to perform deletes
     * of entire directory sub-trees. The corresponding public method just allocates
     * some storage space and sets up the entry into this
     * 
     * Returns false if any part of the operation fails and it can't do the delete.
     */
    static bool _DeleteDirectoryBaseRecursive(char * fullpath, size_t buffSize, size_t pathLen, WIN32_FIND_DATAA * pFindData, bool deleteRootDirectory);


    /**
    * Deletes the contents of a directory and optionally the root directory
    *
    * Allocates the storage for _DeleteDirectoryBaseRecursive and then calls it
    * 
    * Returns false if any part of the operation fails
    */
    static bool _DeleteDirectoryBase(const char * dirname, bool deleteRootDirectory);
};

} // namespace RSLibImpl

