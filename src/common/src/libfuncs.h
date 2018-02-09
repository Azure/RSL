// This file contains global common library function prototypes
#pragma once

#include <windows.h>

namespace RSLibImpl
{

#if 0
bool CommonInit(
    const char* applicationDir,
    const char* defaultConfigName,
    const configuration::IConfiguration* defaultConfiguration,
    const char* bootstrapConfigDir,
    const configuration::IConfiguration* bootstrapConfiguration,
    const configuration::Configuration::OverridePathnames* overridePathnames);

bool CommonInit(
    int instanceNumber,
    const char* applicationDir,
    const char* defaultConfigName,
    const configuration::IConfiguration* defaultConfiguration,
    const char* bootstrapConfigDir,
    const configuration::IConfiguration* bootstrapConfiguration,
    const configuration::Configuration::OverridePathnames* overridePathnames);

BOOL CommonInit(
    const char *defaultConfigFilename = 0,
    DWORD flags = 0,
    const configuration::IConfiguration* inMemoryAutoPilot = 0,
    const configuration::IConfiguration* inMemoryDefaultConfig = 0);

BOOL CommonInit(
    const char *defaultConfigFilename,
    DWORD flags,
    int instanceNumber,
    const configuration::IConfiguration* inMemoryAutoPilot = 0,
    const configuration::IConfiguration* inMemoryDefaultConfig = 0);

#endif

BOOL CommonInit(
    const char *defaultConfigFilename,
    DWORD flags);

void SetCrashOnCrtInvalidParameters(bool crash);

} // namespace RSLibImpl

