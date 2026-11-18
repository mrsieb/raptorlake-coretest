#!/bin/bash
# Baut die weitergebbaren CoreTest-Pakete:
#   Windows: coretest.exe + pure_win.py + Python 3.11.9 embeddable (python.org) + LIESMICH.txt → CoreTest.zip
#   Linux:   coretest (statisch) + coretest_load.py + LIESMICH.txt/README.txt → CoreTest-linux.tar.gz (nutzt das python3 des Systems)
# Aufruf: ./package.sh [Zielordner]   (Standard: ./dist)
set -e
cd "$(dirname "$0")"
ziel="$(realpath -m "${1:-dist}")"; mkdir -p "$ziel"
cd src
PY_URL=https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip
PY_SHA=009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b   # python.org, signiert von Steve Dower (Python Release Signing)
./build.sh >/dev/null 2>&1 || { ./build.sh; exit 1; }
arbeit=$(mktemp -d); trap 'rm -rf "$arbeit"' EXIT
curl -sSfL "$PY_URL" -o "$arbeit/py.zip"
echo "$PY_SHA  $arbeit/py.zip" | sha256sum -c --quiet
mkdir -p "$arbeit/CoreTest/python"
python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$arbeit/py.zip" "$arbeit/CoreTest/python"
cp coretest.exe pure_win.py LIESMICH.txt README.txt "$arbeit/CoreTest/"
cp "$arbeit/CoreTest/python/LICENSE.txt" "$arbeit/CoreTest/PYTHON-LIZENZ.txt"
(cd "$arbeit" && python3 -c "import shutil; shutil.make_archive('CoreTest', 'zip', '.', 'CoreTest')")
cp "$arbeit/CoreTest.zip" "$ziel/CoreTest.zip"
mkdir -p "$arbeit/CoreTest-linux"
./build_linux.sh >/dev/null 2>&1 || { ./build_linux.sh; exit 1; }
cp coretest "$arbeit/CoreTest-linux/coretest"; cp pure_win.py "$arbeit/CoreTest-linux/coretest_load.py"
cp LIESMICH-linux.txt "$arbeit/CoreTest-linux/LIESMICH.txt"; cp README-linux.txt "$arbeit/CoreTest-linux/README.txt"
chmod 755 "$arbeit/CoreTest-linux/coretest"; chmod 644 "$arbeit/CoreTest-linux/"*.py "$arbeit/CoreTest-linux/"*.txt
tar -C "$arbeit" --owner=0 --group=0 --numeric-owner -czf "$ziel/CoreTest-linux.tar.gz" CoreTest-linux
ls -la "$ziel/CoreTest.zip" "$ziel/CoreTest-linux.tar.gz"
