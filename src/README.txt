CORETEST - finds CPU cores that silently compute wrong results under turbo
===========================================================================

For Intel Core 13th/14th gen (Raptor Lake: i5/i7/i9-13xxx/14xxx), which can be affected by the known degradation
("Vmin shift"). Typical symptoms: unexplained crashes, games or programs crashing, unpacking or backups reporting
corrupt data - often WITHOUT any error (WHEA) in the event log. Intel's diagnostic tool (IPDT) often misses this,
because it only tests under full load.

HOW TO USE
  1. Unzip (e.g. to the desktop).
  2. Double-click coretest.exe. Windows SmartScreen may warn ("Unknown publisher") because the program is not
     signed: "More info" -> "Run anyway".
  3. Press Enter. The test takes about 1 to 1.5 hours. Do NOT use the PC meanwhile.
  4. At the end the report opens (Documents\CoreTest-<date>\report.txt) with a clear result.

  Quick test (P-cores only, about 10-15 min): open a command prompt in this folder -> coretest.exe --quick

WHAT HAPPENS
  - Each core is loaded on its own at full single-core turbo: a Python interpreter (the most sensitive test) and a
    C++ compute load. Results are compared against reference values.
  - The turbo setting of the active power plan is set to "Aggressive" for the test and restored exactly as before.
    No admin rights, nothing gets installed, nothing is sent anywhere.
  - The bundled Python (folder "python") is the official "embeddable package" 3.11.9 from python.org, unchanged
    (license: PYTHON-LIZENZ.txt).

RESULT
  - "no errors found": a good sign, but no proof.
  - "ERRORS FOUND": the listed cores compute wrong results under turbo -> back up data, update BIOS (Intel Default
    Settings, microcode 0x12B or newer) and ask Intel for a warranty replacement (RMA). The result folder (report.txt,
    results.csv, errors\, dumps\) is the evidence.

So far this test has been confirmed on one system only. Cancel: Ctrl+C - the turbo setting is restored.
