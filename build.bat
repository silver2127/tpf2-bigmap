@echo off
REM tpf2_bigmap.dll -- a tpf2mp plugin. Standalone: nothing here depends on the
REM host's source tree, only on the vendored src\tpf2mp_plugin.h.
REM
REM Optional %1 suffix: a loaded dll stays locked for the life of the process,
REM so rebuilding to the same name fails with LNK1104 while the game is running.
REM
REM   build.bat            -> out\tpf2_bigmap.dll
REM   build.bat -deploy    -> also copy into %LOCALAPPDATA%\tpf2mp\plugins\
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out

cl /nologo /O2 /MT /W3 /EHsc /c src\bigmap.cpp /Fo:out\bigmap.obj || exit /b 1
link /nologo /DLL /OUT:out\tpf2_bigmap.dll out\bigmap.obj          || exit /b 1
echo BUILD BIGMAP OK

if /i "%1"=="-deploy" (
    if not exist "%LOCALAPPDATA%\tpf2mp\plugins" mkdir "%LOCALAPPDATA%\tpf2mp\plugins"
    copy /y out\tpf2_bigmap.dll "%LOCALAPPDATA%\tpf2mp\plugins\" >nul ^
        && echo DEPLOYED to %LOCALAPPDATA%\tpf2mp\plugins ^
        || echo DEPLOY SKIPPED ^(dll locked by a running game^)
)
