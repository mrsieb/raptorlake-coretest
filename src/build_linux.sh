#!/bin/bash
# Baut coretest (Linux x86-64, statisch, läuft auf Debian/Ubuntu/Fedora/RHEL usw.). Testen nur auf echter Hardware, nicht in WSL/VM.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -static -s -Wall -Wextra -Wno-unused-result coretest_linux.cpp -o coretest
ls -la coretest
