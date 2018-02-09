#define _WINSOCKAPI_
#include <windows.h>
#include <stdio.h>
#include <list>
#include "libfuncs.h"
#include "logging.h"

using namespace std;
using namespace RSLibImpl;

#define NUM_REPLICAS 5
#define MAX_CMD_LEN 256

char procTitles[NUM_REPLICAS][256];
volatile bool endTest;
volatile bool endTestCompleted;

bool StartProcess(char * programName, int replicaId, HANDLE *processHandlePtr)
{
    _snprintf_s(procTitles[replicaId-1], MAX_CMD_LEN, "%s %d", programName, replicaId);
    printf("%s\n", procTitles[replicaId-1]);

    STARTUPINFOA startupInfo;
    GetStartupInfoA(&startupInfo);
    startupInfo.lpTitle = procTitles[replicaId-1];

    PROCESS_INFORMATION processInfo;
    if (!CreateProcessA(NULL,
                        procTitles[replicaId-1],
                        NULL, // no process security attributes
                        NULL, // no thread security attributes
                        FALSE, // inherit handles? no
                        CREATE_NEW_CONSOLE,
                        NULL, // no special environment
                        NULL, //directoryPath,
                        &startupInfo,
                        &processInfo)) {
        return false;
    }

    *processHandlePtr = processInfo.hProcess;
    CloseHandle(processInfo.hThread);
    return true;
}

BOOL CtrlHandler(DWORD ctrlType) 
{
    if (ctrlType == CTRL_C_EVENT ||
        ctrlType == CTRL_BREAK_EVENT)
    {
        endTest = true;
        while (endTestCompleted == false)
        {
            Sleep(100);
        }
    }
    return FALSE; 
} 

void ReadNextCommand(int *timeToWaitPtr, list<int> *membersPtr)
{
    *timeToWaitPtr = 0;
    membersPtr->clear();

    char line[1024];
    if (fgets(line, 1024, stdin) == NULL) {
        return;
    }

    char *curPos = line, *nextPos;
    long timeToWait = strtol(curPos, &nextPos, 10);
    if (nextPos == curPos) {
        return;
    }
    curPos = nextPos;

    *timeToWaitPtr = (int) timeToWait;

    for (;;) {
        long memberId = strtol(curPos, &nextPos, 10);
        if (nextPos == curPos) {
            break;
        }
        curPos = nextPos;
        membersPtr->push_back((int) memberId);
    }
}

bool SetConfiguration(int configurationNumber, const list<int>& membersInNextConfiguration)
{
    list<int>::const_iterator it;

    printf("Setting configuration to:\n");
    for (it = membersInNextConfiguration.begin();
        it != membersInNextConfiguration.end();
        ++it) {
        printf(" %d", *it);
    }
    printf("\n");

    FILE *fp;
    if (fopen_s(&fp, "members.txt", "w")) {
        fprintf(stderr, "Could not open members.txt for writing.\n");
        return false;
    }

    fprintf(fp, "%d\n%d\n", configurationNumber, (int)membersInNextConfiguration.size());
    for (it = membersInNextConfiguration.begin();
        it != membersInNextConfiguration.end();
        ++it) {
        fprintf(fp, "%d\n", *it);
    }

    fclose(fp);
    return true;
}

int GetLastReportedConfigurationNumber ()
{
    FILE *fp;
    if (fopen_s(&fp, "CurrentConfig.txt", "r")) {
        return 0;
    }

    int lastReportedConfigurationNumber = 0;
    fscanf_s(fp, "%d", &lastReportedConfigurationNumber);
    fclose(fp);

    return lastReportedConfigurationNumber;
}

int __cdecl main(int argc, char **argv)
{
    char * programName = "RSLNetTest.exe";
    if (argc == 2)
    {
        programName = argv[1];
    }
    if (GetFileAttributesA(programName) == INVALID_FILE_ATTRIBUTES)
    {
        printf("Program '%s' not found!\n", programName);
        ::ExitProcess(1);
    }

    if (Logger::Init(".\\") == FALSE)
    {
        printf("Logger::Init failed\n");
        ::ExitProcess(1);
    }

    system("cmd /c rmdir /s /q .\\data");
    DeleteFileA("members.txt");
    DeleteFileA("CurrentConfig.txt");

    Sleep(1000);

    int currentConfigurationNumber = 0;
    int timeToWaitForConfigurationChange = 0;
    list<int> membersInNextConfiguration;

    printf("Type timeout and replica set:\n");
    ReadNextCommand(&timeToWaitForConfigurationChange, &membersInNextConfiguration);
    currentConfigurationNumber++;
    SetConfiguration(currentConfigurationNumber, membersInNextConfiguration);

    HANDLE processHandle[NUM_REPLICAS];
    for (int processNumber = 0; processNumber < NUM_REPLICAS; ++processNumber) {
        if (!StartProcess(programName, processNumber+1, &processHandle[processNumber])) {
            fprintf(stderr, "ERROR - Could not start process #%d\n", processNumber+1);
            return -1;
        }
    }

    SetConsoleCtrlHandler( (PHANDLER_ROUTINE) CtrlHandler, TRUE );
    endTest = false;
    endTestCompleted = false;
    for (; endTest == false; ) {
        DWORD waitResult = WaitForMultipleObjects(NUM_REPLICAS,
                                                  processHandle,
                                                  FALSE, // don't wait for all of them
                                                  1000); // time out after 1000 ms
        if (waitResult < WAIT_OBJECT_0 + NUM_REPLICAS) {
            int processNumber = waitResult - WAIT_OBJECT_0;
            printf("Restarting server %d\n", processNumber+1);
            CloseHandle(processHandle[processNumber]);
            if (!StartProcess(programName, processNumber+1, &processHandle[processNumber])) {
                fprintf(stderr, "ERROR - Could not start process #%d\n", processNumber+1);
                break;
            }
        }
        else if (waitResult == WAIT_TIMEOUT) {
            --timeToWaitForConfigurationChange;
            if (timeToWaitForConfigurationChange < 1) {
                int lastReportedConfig = GetLastReportedConfigurationNumber();
                if (currentConfigurationNumber != lastReportedConfig) {
                    fprintf(stderr, "ERROR - Configuration change failed (%d!=%d)!\n",
                        currentConfigurationNumber, lastReportedConfig);
                    break;
                }
                else {
                    printf("Configuration number check successful.\n");
                }
                if (membersInNextConfiguration.size() == 0) {
                    break;
                }
                ReadNextCommand(&timeToWaitForConfigurationChange, &membersInNextConfiguration);
                if (membersInNextConfiguration.size() == 0)
                {
                    break;
                }
                currentConfigurationNumber++;
                if (!SetConfiguration(currentConfigurationNumber, membersInNextConfiguration)) {
                    break;
                }
            }
        }
        else {
            printf("WARNING - Unexpected return value %d from WaitForMultipleObjects.\n", waitResult);
        }
    }

    printf("Terminating all processes.\n");
    for (int processNumber = 0; processNumber < NUM_REPLICAS; ++processNumber) {
        TerminateProcess(processHandle[processNumber], 0);
        CloseHandle(processHandle[processNumber]);
    }
    endTestCompleted = true;

    printf("Test complete.\n");
    return 0;
}
