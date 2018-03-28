# Contributing

This project welcomes contributions and suggestions.  Most contributions require you to agree to a
Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us
the rights to use your contribution. For details, visit https://cla.microsoft.com.

When you submit a pull request, a CLA-bot will automatically determine whether you need to provide
a CLA and decorate the PR appropriately (e.g., label, comment). Simply follow the instructions
provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.

# How to build

## Prerequisite

Install the latest version of [Git for Windows](https://git-scm.com/download/win) for working with the repo.

Install [Visual Studio 2017](https://www.visualstudio.com/downloads/) with Windows desktop C# and C++ support.
Either Professional or Enterprise edition will work, Community edition is not tested.

[NuGet](https://www.nuget.org/downloads) should be already installed with Visual Studio 2017. If you choose to install
MSBuild / .NET SDK / Windows SDK, then install the command line version. NuGet is required to restore several packages
before the build.

## Bootstrap the development environment

In Start Menu (or whatever equivalent), find "Visual Studio 2017", open "visual Studio Tools" folder, click "Developer
Command Prompt for VS 2017". A command prompt will show up, where one may run MSBuild, C# and C++ compilers.

Change to the `ossbuild` directory in the repo, start PowerShell, and run `ossbuild.ps1`. The script will restore all
required packages, generate a file for package definitions, and set several environment variables.

## Build the source code

Go to any directory at root, `src`, or under `src`, run MSBuild like what you normally do. The binaries are saved at
`out` directory under the repo root.

The projects are designed to be built in parallel. If the number of processor is 8 (check the environment variable
`NUMBER_OF_PROCESSOR`), the recommended command to build is:

    msbuild /m:8 /v:m /fl

The second argument sets the verbosity to minimal.

### Build in Visual Studio IDE

Once in the bootstrapped PowerShell window, one can open any project or `src\RSL.sln` in VS IDE.
