@echo off
setlocal
cd /d "%~dp0"
set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not exist "%MSBUILD%" (
  echo MSBuild not found: %MSBUILD%
  exit /b 1
)
"%MSBUILD%" MCPActivity.sln /m /t:Build /p:Configuration=Release /p:Platform=x64
exit /b %errorlevel%
