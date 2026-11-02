@echo off
rem Builds gamma_check.exe (photo mode's WidescreenFix gamma on the real GPU, outside the game). Run:
rem   gamma_check.exe "..\..\..\..\scripts\NFSUnderground2.WidescreenFix.asi"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /EHsc /O2 /std:c++17 gamma_check.cpp ..\..\..\scripts\photomode\wsfgamma.cpp ..\..\..\common\u2sdk\src\log.cpp /I..\..\..\scripts\photomode /I..\..\..\common\u2sdk\include d3d9.lib user32.lib /Fe:gamma_check.exe
