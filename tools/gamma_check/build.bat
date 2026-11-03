@echo off
rem Builds gamma_check.exe (photo mode's WidescreenFix gamma on the real GPU, outside the game). Run:
rem   gamma_check.exe "<game>\scripts\NFSUnderground2.WidescreenFix.asi"
rem u2sdk: the submodule, or the scripts workspace's common\u2sdk (this repo at scripts\photomode).
set U2=..\..\external\u2sdk
if not exist %U2%\src\log.cpp set U2=..\..\..\..\common\u2sdk
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /EHsc /O2 /std:c++17 gamma_check.cpp ..\..\wsfgamma.cpp %U2%\src\log.cpp /I..\.. /I%U2%\include d3d9.lib user32.lib /Fe:gamma_check.exe
