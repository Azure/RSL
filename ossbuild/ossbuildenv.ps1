#Requires -version 3.0
param(
    [string] $PackagesDirectory = $null
)

# Go to C:\Program Files (x86)\Microsoft Visual Studio\2017\Enterprise\Common7\Tools, run VsMSBuildCmd.bat, 
# and start PowerShell.
Set-StrictMode -version latest

function SetEnv
{
    # Get the root directory
    $repoRoot = Join-Path -Resolve $PSScriptRoot ".."
    $srcRoot = Join-Path -Resolve $repoRoot "src"
    $outRoot = Join-Path $repoRoot "out"
    $buildPropsRoot = Join-Path -Resolve $PSScriptRoot "ossbuildenv.props"
    $confRoot = Join-Path -Resolve $repoRoot ".config"
    $verPath = Join-Path -Resolve $confRoot ".inc"
    $asmVerDefFile = Join-Path -Resolve $verPath "versions.xml"

    $verXml = [XML](Get-Content $asmVerDefFile)
    $ver = $verXml.root.versions.version.value
    
    $env:REPOROOT =         $repoRoot
    $env:BaseDir =          $repoRoot
    $env:EnlistmentRoot =   $repoRoot
    $env:INETROOT =         $repoRoot
    $env:OBJECT_ROOT =      $repoRoot
    $env:ROOT =             $repoRoot
    $env:_NTTREE =          $repoRoot
    
    $env:SRCROOT =          $srcRoot
    $env:OUTPUTROOT =       $outRoot

    $env:EnvironmentConfig =                $buildPropsRoot
    $env:CONFROOT =                         $confRoot
    $env:AssemblyVersionDefinitionFile =    $asmVerDefFile

    $env:OSSBUILD =         "1"
    $env:BUILD_COREXT =     "0"
    $env:NOTQBUILD =        "1"
    $env:MsBuildArgs =      "/consoleloggerparameters:Summary;ForceNoAlign;Verbosity=minimal"
    $env:BUILDNUMBER =      $ver
}

function CreatePackagesProps($rootDir)
{
    $rootDir = [IO.Path]::GetFullPath($rootDir)
    $propsFile = Join-Path $PSScriptRoot "packageList.props"

    # Restore all nuget packages
    $conf = Join-Path -Resolve $PSScriptRoot "..\src\packages.config"
    & nuget restore $conf -PackagesDirectory $rootDir

    # Then generate packageList.props for MSBuild
    $content = @()
    $content += "<Project xmlns='http://schemas.microsoft.com/developer/msbuild/2003'>"
    $content += "  <PropertyGroup>"

    $pkgXml = [XML](Get-Content $conf)
    $pkgXml.packages.package | % {
        $id = $_.id
        $ver = $_.version
        $dir = Join-Path $rootDir "$id.$ver"
        $name = "Pkg" + ($id.Replace('.', '_'))
        $content += "    <$name>$dir</$name>"
    }

    $content += "  </PropertyGroup>"
    $content += "</Project>"
    $content | Out-File -FilePath $propsFile -Encoding utf8 -Force
}

#######################################################################################################################

if ([string]::IsNullOrEmpty($PackagesDirectory)) {
    $PackagesDirectory = Join-Path $PSScriptRoot "..\packages"
    if (-not (Test-Path -PathType Container $PackagesDirectory)) {
        mkdir $PackagesDirectory
    }
}

SetEnv
CreatePackagesProps $PackagesDirectory
