@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /O1 /GS- /nologo tests\bench.cpp /Fetests\bench.exe /link /SUBSYSTEM:CONSOLE