@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
rem builds ..\app\RaceVision.exe; while RaceVision is running it stays as ..\app\RaceVision_new.exe
rem (a running exe cannot be overwritten): build again, or rename it, once RaceVision is closed
rem a window app (no console) that asks for administrator rights itself (NR2003 runs as administrator)
rem racevision.rc builds the phone page (racevision_phone.html) and the logo into the exe
rc /nologo /fo racevision.res racevision.rc || exit /b 1
cl /nologo /W3 /MT /EHsc /O2 /Fe:..\app\RaceVision_new.exe tv_director.cpp racevision.res user32.lib gdi32.lib gdiplus.lib xaudio2.lib ole32.lib ^
   comdlg32.lib shell32.lib dwmapi.lib uxtheme.lib ws2_32.lib iphlpapi.lib ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator'" || exit /b 1
del tv_director.obj racevision.res 2>nul
move /y ..\app\RaceVision_new.exe ..\app\RaceVision.exe >nul 2>nul && echo built ..\app\RaceVision.exe || echo RaceVision is running: built ..\app\RaceVision_new.exe
