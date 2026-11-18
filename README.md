# raptorlake-coretest

A per-core test for Intel 13th/14th gen (Raptor Lake) CPUs that silently compute wrong results under single-core turbo.

## Goal

Help people with unexplained crashes on a Raptor Lake CPU find out whether their CPU is degraded, and if so which
cores, with a result they can hand to Intel for an RMA. Without having to set up stress test tools first.

## Background

My i9-14900KF had WHEA parity errors on one core, so I disabled that core in the BIOS. Later it turned out that two
more cores were producing wrong data at ~5.7 GHz, without a single WHEA entry: Python crashed with access violations
or raised errors that are impossible for the code it was running ("'int' object does not support the context manager
protocol" on a plain list access). Intel's diagnostic tool (IPDT) passed, because it only tests under full load.
With turbo disabled the errors were gone. This tool is what I ended up using to pin that down.

## What it does

- Runs a workload on one logical CPU at a time (affinity) while the rest of the system is idle, so that core gets the
  full single-core turbo clock. That is where degraded Raptor Lake cores fail first.
- Two workloads:
  - **py**: a small Python filter loop. The interpreter does a lot of dependent loads, stores and indirect jumps.
    On my CPU this was by far the most sensitive load.
  - **nat**: a native C++ load (float filter, small bytecode VM, pointer chain), compared against reference values
    computed beforehand on another core.
- Records per run: APIC ID of the core (same numbering as in WHEA events), average clock, result, exit code.
  Crashes of the native load write a minidump.
- Writes a report (`report.txt` / `bericht.txt`) with a verdict, a table for all cores, the environment
  (CPU, microcode, board, BIOS, Windows build) and what to do next. Plus `results.csv` and an `errors` folder with the
  full output of every failed run.
- Sets the turbo setting (processor performance boost mode) of the active power plan to "Aggressive" for the test and
  restores the previous value afterwards, also on Ctrl+C or when the window is closed. If the program is killed
  (Task Manager, crash), the old value is restored the next time coretest is started. Test processes never outlive it.

## What it does not do

- It does not repair anything, change BIOS settings, undervolt or overclock.
- It does not prove that a CPU is healthy. "No errors found" only means none were found in this run.
- It does not test full-load stability, memory (RAM), the GPU or temperatures. Use OCCT, MemTest86 etc. for that.
- It does not need admin rights, install anything or send anything anywhere.

## Status and limitations

- **So far tested on exactly one system** (mine: i9-14900KF, MSI Z790, BIOS with microcode 0x137). There it finds the
  bad cores reliably within seconds to minutes. Whether it works as well on other degraded CPUs is unknown.
  Results from other systems are very welcome: open an issue with your report.
- Errors on degraded cores are sporadic. A short clean run does not mean much; use the standard run or longer
  (`--sek 120 --py 5`).
- It only makes sense with turbo working. If the highest measured clock stays near base clock, the report says so.
- It is meant for Intel 13th/14th gen. It runs on other CPUs too, but the explanations in the report may not apply.
- Running a load on a possibly unstable CPU is the whole point, but it can crash programs. Close everything important
  first. Use at your own risk.
- Program output and report are German on German Windows, English otherwise. Code comments are German.

## Related tools

[CoreCycler](https://github.com/sp00n/corecycler) is the established tool for per-core stability testing and runs
Prime95, y-cruncher, Linpack etc. core by core. OCCT has a similar core cycling mode. This tool uses a different load
(an interpreter instead of math kernels) and produces a ready-to-use report. If in doubt, run both.

## Usage

Download `CoreTest.zip` from the releases, unzip, double-click `coretest.exe`, press Enter.

- Standard run: about 1 to 1.5 h. Don't use the PC meanwhile, other load lowers the turbo clock.
- `coretest.exe --quick`: P-cores only, about 10-15 min.
- The exe is not signed, so SmartScreen will probably warn ("More info" -> "Run anyway").

The zip includes the official Python 3.11.9 embeddable package from python.org (unchanged, license in
`PYTHON-LIZENZ.txt`), so Python does not need to be installed.

## If errors are found

The listed cores crashed or returned wrong results under turbo. On Raptor Lake this is typical for the known
degradation ("Vmin shift"). Back up your data, update the BIOS (microcode 0x12B or newer, Intel Default Settings),
run the test again with turbo disabled as a cross-check, and contact Intel for an RMA (13th/14th gen have an extended
5 year warranty). The result folder can be attached as evidence.

## Building

mingw-w64 on Linux/WSL: `src/build.sh` builds `coretest.exe`, `./package.sh` builds `dist/CoreTest.zip` (downloads the
Python embeddable package and checks its SHA-256).

## License

MIT, see `LICENSE`. Python is licensed under the PSF license.
