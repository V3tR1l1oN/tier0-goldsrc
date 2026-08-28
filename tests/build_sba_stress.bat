@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /O2 /GS- /nologo tests\test_sba_stress.cpp /Fetests\test_sba_stress.exe /link /SUBSYSTEM:CONSOLE