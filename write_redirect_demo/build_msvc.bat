@echo off
setlocal
cd /d "%~dp0"
if not exist out mkdir out

cl /nologo /std:c++17 /EHsc /W4 /Fo:out\process_a.obj /Fe:out\process_a.exe src\process_a.cpp
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++17 /EHsc /W4 /Fo:out\process_b.obj /Fe:out\process_b.exe src\process_b.cpp Ws2_32.lib
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++17 /EHsc /W4 /Fo:out\process_c.obj /Fe:out\process_c.exe src\process_c.cpp Ws2_32.lib
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++17 /EHsc /W4 /Fo:out\hook_launcher.obj /Fe:out\hook_launcher.exe src\hook_launcher.cpp
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++17 /EHsc /W4 /LD /Fo:out\redirect_hook.obj /Fe:out\redirect_hook.dll src\redirect_hook.cpp
if errorlevel 1 exit /b %errorlevel%

echo Built:
echo   %cd%\out\process_a.exe
echo   %cd%\out\process_b.exe
echo   %cd%\out\process_c.exe
echo   %cd%\out\hook_launcher.exe
echo   %cd%\out\redirect_hook.dll
