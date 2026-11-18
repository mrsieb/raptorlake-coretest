CORETEST (Linux) - finds CPU cores that silently compute wrong results under turbo
===================================================================================

For Intel Core 13th/14th gen (Raptor Lake: i5/i7/i9-13xxx/14xxx), which can be affected by the known degradation
("Vmin shift"). Typical symptoms: unexplained crashes, programs crashing, unpacking or backups reporting corrupt
data - often WITHOUT any machine check (MCE) in the kernel log.

LINUX VERSION: NOT YET CONFIRMED ON REAL HARDWARE. The Windows version is confirmed on one system; this version
uses the same load, but has so far only been checked for correct operation in WSL.

HOW TO USE
  1. Unpack:  tar xzf CoreTest-linux.tar.gz && cd CoreTest-linux
  2. Run:     sudo ./coretest
     (root is only needed to switch turbo on for the test and restore it afterwards.)
  3. Press Enter. The test takes about 1 to 1.5 hours. Do NOT use the PC meanwhile.
  4. At the end the report is in ./CoreTest-<date>/report.txt (bericht.txt on German systems), with a clear result.

  Quick test (P-cores only, about 10-15 min):  sudo ./coretest --quick

REQUIREMENTS
  - Real Linux (installed or a live USB stick, e.g. Ubuntu). NOT in WSL or a VM - coretest refuses to start there,
    because affinity does not hit real cores.
  - python3 (preinstalled on Ubuntu, Fedora, RHEL 9; otherwise: apt install python3 / dnf install python3).
    Without Python the most sensitive test is skipped; the report says so clearly.
  - Don't use the "power saver" power profile - it can switch turbo off.

WHAT HAPPENS
  - Each core is loaded on its own at full single-core turbo: a Python interpreter (the most sensitive test) and a
    C++ compute load. Results are compared against reference values.
  - If turbo is off (/sys/devices/system/cpu/intel_pstate/no_turbo), it is switched on for the test and restored
    afterwards - also on Ctrl+C or when the terminal is closed; after a hard kill (kill -9, crash) on the next start.
    Nothing gets installed, nothing is sent anywhere.
  - Suspend/sleep is blocked during the test (systemd-inhibit, if available).

RESULT
  - "no errors found": a good sign, but no proof.
  - "ERRORS FOUND": the listed cores compute wrong results under turbo -> back up data, update BIOS (Intel Default
    Settings, microcode 0x12B or newer) and ask Intel for a warranty replacement (RMA). The result folder
    (report.txt, results.csv, errors/) is the evidence.
  - CPU numbers on Linux differ from Windows. Use the APIC ID to compare.
