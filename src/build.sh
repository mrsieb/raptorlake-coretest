#!/bin/bash
# Baut coretest.exe (mingw-w64, statisch). Läuft unter Windows, nicht in WSL (Affinity muss echte Kerne treffen).
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -municode -static -s -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Wall -Wno-unused-result \
  coretest.cpp -o coretest.exe -ldbghelp -lpdh -lpowrprof -lshell32 -luser32 -ladvapi32 -lole32
ls -la coretest.exe
