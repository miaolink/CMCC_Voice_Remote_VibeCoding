@echo off
cd /d "%~dp0"
if not exist libg7221 (
  git clone --branch v0.2.0 --depth 1 https://github.com/freeswitch/libg7221.git
  if errorlevel 1 exit /b 1
  cd libg7221
  git apply ..\libg722_1-v0.2.0-debug-globals.patch
  cd ..
)
cd libg7221
if not exist src\.libs\libg722_1.a (
  if exist Makefile (
    mingw32-make -j
  ) else (
    echo Configure/make may need MSYS. Trying direct compile of sources...
  )
)
cd ..
REM Direct compile of decoder sources if static lib missing
if not exist decode.exe (
  gcc -O2 -DG722_1_USE_FIXED_POINT -Ilibg7221\include -o decode.exe decode_main.c libg7221\src\*.c 2>build_err.txt
  if errorlevel 1 (
    echo Build failed, see build_err.txt
    type build_err.txt
    exit /b 1
  )
)
echo OK: %cd%\decode.exe
dir decode.exe
