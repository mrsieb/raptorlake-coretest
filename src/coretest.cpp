// coretest - finds CPU cores that silently compute wrong results under single-core turbo (Intel 13th/14th gen degradation).
// Tests each logical processor on its own (affinity) with two loads:
//   py  = pure_win.py, a Python interpreter loop (most sensitive: dependent loads, stores, indirect jumps)
//   nat = native C++ load (filter, small bytecode VM, pointer chain), compared against reference values from another core
// Per run: APIC ID (CPUID 0xB, same numbering as WHEA events), average clock (PDH), result, exit code.
// Turbo: sets the boost mode of the active power plan to Aggressive for the test and restores the previous value.
// If PowerPlanTray (a turbo-lock tray tool) is running, it is closed during the test and restarted afterwards.
// Usage: coretest.exe [output folder] [--quick] [--cpus 0-31] [--sek 60] [--py 3] [--nat 1] [--ekern-runden 1]
// Internal: coretest.exe --worker <golden.bin|-> <seconds> <dumpfolder>, coretest.exe --apic
// Comments in the code are German (my native language), program output is German or English depending on Windows.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <pdh.h>
#include <powrprof.h>
#include <cpuid.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

using std::wstring; using std::string; using std::vector;

static bool g_de = false;                                  // Ausgabe Deutsch, wenn Windows auf Deutsch läuft, sonst Englisch
static const wchar_t* T(const wchar_t* de, const wchar_t* en) { return g_de ? de : en; }
static wstring Fmt(const wchar_t* f, ...) { wchar_t b[4096]; va_list v; va_start(v, f); _vsnwprintf(b, 4095, f, v); va_end(v); b[4095] = 0; return b; }
static string U8(const wstring& s) { int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr); string r(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n, nullptr, nullptr); return r; }
static wstring W(const string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0); wstring r(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n); return r; }

static unsigned ApicId() { unsigned a, b, c, d; if (__get_cpuid_count(0xB, 0, &a, &b, &c, &d)) return d; __get_cpuid(1, &a, &b, &c, &d); return b >> 24; }

// ============================================================ Last (Worker)
static uint64_t g_x;
static uint64_t Rnd() { g_x ^= g_x << 13; g_x ^= g_x >> 7; g_x ^= g_x << 17; return g_x; }

static double Filter(int n, int seed) {          // wie pure.py chunk(), Dämpfung je Seed leicht anders
  static double b[1557]; for (auto& v : b) v = 0; int i0 = 0; double f0 = 0, f1 = 0, f2 = 0, f3 = 0, damp = 0.25 + (seed % 16) * 0.01, acc = 0;
  for (int i = 0; i < n; i++) {
    double x = (i % 1000 == 0) ? 1.0 : 0.0, o0 = b[i0], o1 = o0 * 0.5, o2 = o0 * 0.25, o3 = -o0;
    f0 += (1 - damp) * (o0 - f0); f1 += (1 - damp) * (o1 - f1); f2 += (1 - damp) * (o2 - f2); f3 += (1 - damp) * (o3 - f3);
    b[i0] = x + (f0 + f1 - f2 - f3) * 0.4; if (++i0 == 1557) i0 = 0;
    acc += f0 * 1e-3 + std::sqrt(std::fabs(f1)) * 1e-9;
  }
  return acc;
}
static uint64_t Vm(int schritte, int seed) {     // kleine Stack-Maschine mit switch-Dispatch (Interpreter-Muster)
  static uint8_t code[4096]; static int64_t mem[8192]; int64_t st[256]; int sp = 0;
  g_x = 0x9E3779B97F4A7C15ull ^ (uint64_t)seed * 0x100000001B3ull;
  for (auto& c : code) c = (uint8_t)(Rnd() % 12);
  for (int i = 0; i < 8192; i++) mem[i] = (int64_t)Rnd();
  uint64_t h = 1469598103934665603ull; int pc = 0;
  for (int s = 0; s < schritte; s++) {
    uint8_t op = code[pc]; pc = (pc + 1) & 4095;
    switch (op) {
      case 0: if (sp < 250) st[sp++] = (int64_t)pc * 2654435761ll; break;
      case 1: if (sp >= 2) { st[sp - 2] += st[sp - 1]; sp--; } break;
      case 2: if (sp >= 2) { st[sp - 2] *= (st[sp - 1] | 1); sp--; } break;
      case 3: if (sp >= 2) { st[sp - 2] ^= st[sp - 1] >> 3; sp--; } break;
      case 4: if (sp >= 1 && sp < 250) { st[sp] = st[sp - 1]; sp++; } break;
      case 5: if (sp >= 2) std::swap(st[sp - 1], st[sp - 2]); break;
      case 6: if (sp >= 1) h = (h ^ (uint64_t)st[--sp]) * 1099511628211ull; break;
      case 7: if (sp >= 1 && sp < 250) { st[sp - 1] = mem[(uint64_t)st[sp - 1] & 8191]; } break;
      case 8: if (sp >= 2) { mem[(uint64_t)st[sp - 1] & 8191] = st[sp - 2]; sp -= 2; } break;
      case 9: if (sp >= 1 && (st[sp - 1] & 7) == 0) pc = (int)((uint64_t)st[sp - 1] & 4095); break;
      case 10: if (sp >= 2 && st[sp - 1] != 0) { st[sp - 2] = st[sp - 2] / ((st[sp - 1] & 0xFFFF) | 1); sp--; } break;
      default: if (sp >= 1) { double d = (double)(st[sp - 1] & 0xFFFFF); st[sp - 1] = (int64_t)(std::sqrt(d) * 1000.0); } break;
    }
  }
  for (int i = 0; i < sp; i++) h = (h ^ (uint64_t)st[i]) * 1099511628211ull;
  for (int i = 0; i < 8192; i += 64) h = (h ^ (uint64_t)mem[i]) * 1099511628211ull;
  return h;
}
static uint64_t Kette(int schritte, int seed) {  // zufällige Zeigerkette über 4 MB (L2/L3)
  static vector<uint32_t> nx(1 << 20);
  g_x = 0xD1B54A32D192ED03ull + (uint64_t)seed;
  for (uint32_t i = 0; i < nx.size(); i++) nx[i] = i;
  for (uint32_t i = (uint32_t)nx.size() - 1; i > 0; i--) std::swap(nx[i], nx[Rnd() % (i + 1)]);
  uint32_t p = 0; uint64_t h = 0;
  for (int s = 0; s < schritte; s++) { p = nx[p]; h = h * 31 + p; nx[p] ^= (uint32_t)(s & 1); nx[p] ^= (uint32_t)(s & 1); }
  return h;
}
struct Gold { uint64_t f, v, k; };
static const int SEEDS = 32;
static Gold Block(int seed) { double f = Filter(1500000, seed); Gold g; memcpy(&g.f, &f, 8); g.v = Vm(3000000, seed); g.k = Kette(400000, seed); return g; }

static wstring g_dumpDir;
static LONG WINAPI Absturz(EXCEPTION_POINTERS* e) {
  SYSTEMTIME t; GetLocalTime(&t);
  wstring p = g_dumpDir + Fmt(L"\\coretest-apic%u-%02d%02d%02d-%u.dmp", ApicId(), t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mi{ GetCurrentThreadId(), e, FALSE };
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithThreadInfo), &mi, nullptr, nullptr);
    CloseHandle(h);
  }
  printf("CRASH 0x%08lX at %p, dump %s\n", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress, U8(p).c_str()); fflush(stdout);
  return EXCEPTION_EXECUTE_HANDLER;
}
static int Worker(const wstring& gold, double sek, const wstring& dumps) {
  g_dumpDir = dumps; SetUnhandledExceptionFilter(Absturz);
  printf("apic %u\n", ApicId()); fflush(stdout);
  if (gold == L"-") {                            // Referenz erzeugen
    for (int s = 0; s < SEEDS; s++) { Gold g = Block(s); printf("gold %d %016llx %016llx %016llx\n", s, (unsigned long long)g.f, (unsigned long long)g.v, (unsigned long long)g.k); }
    return 0;
  }
  vector<Gold> ref(SEEDS);
  FILE* f = _wfopen(gold.c_str(), L"rb"); if (!f || fread(ref.data(), sizeof(Gold), SEEDS, f) != SEEDS) { printf("ERROR golden\n"); return 9; } fclose(f);
  ULONGLONG ende = GetTickCount64() + (ULONGLONG)(sek * 1000); int n = 0, fehl = 0;
  while (GetTickCount64() < ende) {
    int s = n % SEEDS; Gold g = Block(s); n++;
    if (g.f != ref[s].f || g.v != ref[s].v || g.k != ref[s].k) {
      fehl++;
      printf("MISMATCH block %d seed %d%s%s%s\n", n, s, g.f != ref[s].f ? " filter" : "", g.v != ref[s].v ? " vm" : "", g.k != ref[s].k ? " kette" : ""); fflush(stdout);
    }
  }
  printf("%s %d blocks %d errors\n", fehl ? "WRONG" : "ok", n, fehl);
  return fehl ? 3 : 0;
}

// ============================================================ Steuerung
static wstring g_aus; static FILE* g_log; static FILE* g_csv;
static void Log(const wstring& t) {
  SYSTEMTIME s; GetLocalTime(&s); wstring z = Fmt(L"%02d:%02d:%02d  %s\n", s.wHour, s.wMinute, s.wSecond, t.c_str());
  fputs(U8(z).c_str(), stdout); fflush(stdout); if (g_log) { fputs(U8(z).c_str(), g_log); fflush(g_log); }
}
static const GUID SUB_PROC = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
static const GUID BOOST = { 0xbe337238, 0x0d82, 0x4146, { 0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7 } };
static GUID g_plan; static bool g_turboGesetzt = false, g_trayWar = false; static DWORD g_altAC = 2, g_altDC = 2;
static void SetzeBoost(DWORD v) {
  PowerWriteACValueIndex(nullptr, &g_plan, &SUB_PROC, &BOOST, v); PowerWriteDCValueIndex(nullptr, &g_plan, &SUB_PROC, &BOOST, v);
  PowerSetActiveScheme(nullptr, &g_plan);
}
static void TrayStarten() {
  wchar_t la[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
  wstring exe = wstring(la) + L"\\PowerPlanTray\\PowerPlanTray.exe";
  wstring dir = wstring(la) + L"\\PowerPlanTray";   // eigener Ordner als Arbeitsverzeichnis (sonst blockiert der Tray den Testordner)
  if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
}
static void Aufraeumen() {
  if (!g_turboGesetzt) return;
  g_turboGesetzt = false;
  PowerWriteACValueIndex(nullptr, &g_plan, &SUB_PROC, &BOOST, g_altAC); PowerWriteDCValueIndex(nullptr, &g_plan, &SUB_PROC, &BOOST, g_altDC);
  PowerSetActiveScheme(nullptr, &g_plan);
  Log(Fmt(T(L"Turbo-Einstellung wie vorher (Boost-Modus %u)", L"Turbo setting restored (boost mode %u)"), g_altAC));
  if (g_trayWar) { TrayStarten(); Log(T(L"PowerPlanTray wieder gestartet", L"PowerPlanTray restarted")); }
}
static BOOL WINAPI Strg(DWORD) { Aufraeumen(); return FALSE; }

struct Lauf { string out; DWORD code = 0; double mhz = 0; bool timeout = false; };
static Lauf Starte(const wstring& cmd, int cpu, double sek) {
  Lauf r; SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE }; HANDLE rd, wr; CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{ sizeof si }; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
  PROCESS_INFORMATION pi{}; wstring c = cmd;
  if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { r.out = "Start fehlgeschlagen"; r.code = GetLastError(); CloseHandle(rd); CloseHandle(wr); return r; }
  CloseHandle(wr);
  SetProcessAffinityMask(pi.hProcess, (DWORD_PTR)1 << cpu);
  // Takt messen: Processor Frequency × % Processor Performance des Ziel-Prozessors
  PDH_HQUERY q; PDH_HCOUNTER cf, cp; PdhOpenQueryW(nullptr, 0, &q);
  PdhAddEnglishCounterW(q, Fmt(L"\\Processor Information(0,%d)\\Processor Frequency", cpu).c_str(), 0, &cf);
  PdhAddEnglishCounterW(q, Fmt(L"\\Processor Information(0,%d)\\%% Processor Performance", cpu).c_str(), 0, &cp);
  PdhCollectQueryData(q);
  ResumeThread(pi.hThread);
  ULONGLONG ende = GetTickCount64() + (ULONGLONG)(sek * 1000) + 60000, naechst = GetTickCount64() + 1000; double summe = 0; int n = 0;
  char b[4096]; DWORD m, verf;
  for (;;) {
    while (PeekNamedPipe(rd, nullptr, 0, nullptr, &verf, nullptr) && verf && ReadFile(rd, b, std::min<DWORD>(verf, sizeof b), &m, nullptr) && m) r.out.append(b, m);
    if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
    if (GetTickCount64() >= naechst) {
      naechst += 1000; PdhCollectQueryData(q); PDH_FMT_COUNTERVALUE vf, vp;
      if (PdhGetFormattedCounterValue(cf, PDH_FMT_DOUBLE, nullptr, &vf) == ERROR_SUCCESS && PdhGetFormattedCounterValue(cp, PDH_FMT_DOUBLE, nullptr, &vp) == ERROR_SUCCESS) { summe += vf.doubleValue * vp.doubleValue / 100; n++; }
    }
    if (GetTickCount64() > ende) { TerminateProcess(pi.hProcess, 0xDEAD); r.timeout = true; }
  }
  while (PeekNamedPipe(rd, nullptr, 0, nullptr, &verf, nullptr) && verf && ReadFile(rd, b, std::min<DWORD>(verf, sizeof b), &m, nullptr) && m) r.out.append(b, m);
  GetExitCodeProcess(pi.hProcess, &r.code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd); PdhCloseQuery(q);
  r.mhz = n ? summe / n : 0; return r;
}
static string Zeile(const string& out, const char* pre) { size_t p = out.find(pre); if (p == string::npos) return ""; size_t e = out.find('\n', p); string z = out.substr(p, e == string::npos ? string::npos : e - p); while (!z.empty() && (z.back() == '\r' || z.back() == ' ')) z.pop_back(); return z; }

static wstring RegText(HKEY wurzel, const wchar_t* pfad, const wchar_t* name) {
  wchar_t b[512]; DWORD n = sizeof b; if (RegGetValueW(wurzel, pfad, name, RRF_RT_REG_SZ, nullptr, b, &n) == ERROR_SUCCESS) return b; return L"?"; }
static DWORD RegZahl(HKEY wurzel, const wchar_t* pfad, const wchar_t* name) { DWORD v = 0, n = sizeof v; RegGetValueW(wurzel, pfad, name, RRF_RT_REG_DWORD, nullptr, &v, &n); return v; }
static wstring Microcode() {
  BYTE b[8] = {}; DWORD n = sizeof b;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"Update Revision", RRF_RT_REG_BINARY, nullptr, b, &n) != ERROR_SUCCESS) return L"?";
  DWORD v = 0; memcpy(&v, n >= 8 ? b + 4 : b, 4);              // 8 Byte: Revision im oberen Teil (ältere Windows), 4 Byte: direkt
  if (!v && n >= 8) memcpy(&v, b, 4);
  return Fmt(L"0x%X", v); }

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc >= 5 && !wcscmp(argv[1], L"--worker")) return Worker(argv[2], _wtof(argv[3]), argv[4]);
  if (argc >= 2 && !wcscmp(argv[1], L"--apic")) { printf("apic %u\n", ApicId()); return 0; }
  g_de = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
  int von = 0, bis = -1, py = 3, nat = 1, eRunden = 1; double sek = 60; bool schnell = false;
  for (int i = 1; i < argc; i++) {
    if (!wcscmp(argv[i], L"--cpus") && i + 1 < argc) { swscanf(argv[++i], L"%d-%d", &von, &bis); if (bis < von) bis = von; }
    else if (!wcscmp(argv[i], L"--sek") && i + 1 < argc) sek = _wtof(argv[++i]);
    else if (!wcscmp(argv[i], L"--py") && i + 1 < argc) py = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--nat") && i + 1 < argc) nat = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--ekern-runden") && i + 1 < argc) eRunden = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--schnell") || !wcscmp(argv[i], L"--quick")) { schnell = true; py = 1; nat = 0; sek = 45; eRunden = 0; }
    else if (!wcscmp(argv[i], L"--hilfe") || !wcscmp(argv[i], L"/?")) {
      g_de = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
      printf("%s", U8(T(L"coretest – findet CPU-Kerne, die unter Single-Core-Turbo still falsch rechnen (Intel 13./14. Gen)\n\n"
             L"Aufruf: coretest.exe [Ausgabeordner] [--schnell] [--cpus 0-31] [--sek 60] [--py 3] [--nat 1] [--ekern-runden 1]\n"
             L"Ohne Angaben: Ordner Documents\\CoreTest-<Datum>, Standardtest (ca. 1-1,5 h), wartet auf Enter.\n"
             L"--schnell: nur P-Kerne, je 45 s Python (ca. 10-15 min).\n",
             L"coretest – finds CPU cores that silently compute wrong results under single-core turbo (Intel 13th/14th gen)\n\n"
             L"Usage: coretest.exe [output folder] [--quick] [--cpus 0-31] [--sek 60] [--py 3] [--nat 1] [--ekern-runden 1]\n"
             L"No arguments: folder Documents\\CoreTest-<date>, standard run (about 1-1.5 h), waits for Enter.\n"
             L"--quick: P-cores only, 45 s Python each (about 10-15 min).\n")).c_str());
      return 0; }
    else if (argv[i][0] != L'-') g_aus = argv[i];
  }
  bool interaktiv = g_aus.empty();
  { // gedacht für Intel Core 13./14. Generation (Raptor Lake) – sonst nur Hinweis
    wstring n = RegText(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    bool rpl = n.find(L"Intel") != wstring::npos && (n.find(L"-13") != wstring::npos || n.find(L"-14") != wstring::npos);
    if (!rpl) printf("%s", U8(Fmt(T(L"\n  HINWEIS: Dieser Test ist für Intel Core 13./14. Generation (Raptor Lake) gedacht.\n  Erkannte CPU: %s\n  Er läuft trotzdem, die Erklärungen im Bericht passen dann aber nur bedingt.\n",
                                         L"\n  NOTE: This test is meant for Intel Core 13th/14th gen (Raptor Lake).\n  Detected CPU: %s\n  It runs anyway, but the explanations in the report may not apply.\n"), n.c_str())).c_str());
  }
  if (interaktiv) {
    wchar_t doc[MAX_PATH]; SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, doc);
    SYSTEMTIME st; GetLocalTime(&st);
    g_aus = wstring(doc) + Fmt(L"\\CoreTest-%04d-%02d-%02d_%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    printf("%s", U8(Fmt(T(
      L"\n  CORETEST – sucht CPU-Kerne, die mit Turbo still falsch rechnen\n  ===============================================================\n\n"
      L"  Jeder Kern wird einzeln mit vollem Single-Core-Turbo belastet (Python-Interpreter + C++-Last),\n"
      L"  die Ergebnisse werden mit Referenzwerten verglichen. Typisch für degradierte Intel 13./14. Gen:\n"
      L"  einzelne Kerne liefern falsche Werte oder stürzen ab – oft OHNE Windows-Fehlermeldung (WHEA).\n\n"
      L"  Dauer: %s. Den PC in der Zeit nicht benutzen (andere Last senkt den Turbo-Takt).\n"
      L"  Der Turbo im aktiven Energieplan wird für den Test auf „Aggressiv“ gestellt und danach\n"
      L"  wieder wie vorher eingestellt. Keine Admin-Rechte nötig, es wird nichts installiert.\n\n"
      L"  Ergebnis: %s\n\n  Enter = starten, Strg+C = abbrechen ",
      L"\n  CORETEST – finds CPU cores that silently compute wrong results under turbo\n  ==========================================================================\n\n"
      L"  Each core is loaded on its own at full single-core turbo (Python interpreter + C++ load),\n"
      L"  results are compared against reference values. Typical for degraded Intel 13th/14th gen:\n"
      L"  single cores return wrong values or crash – often WITHOUT any Windows error (WHEA).\n\n"
      L"  Duration: %s. Don't use the PC meanwhile (other load lowers the turbo clock).\n"
      L"  The turbo setting of the active power plan is set to \"Aggressive\" for the test and\n"
      L"  restored afterwards. No admin rights needed, nothing gets installed.\n\n"
      L"  Results: %s\n\n  Enter = start, Ctrl+C = cancel "),
      schnell ? T(L"ca. 10–15 Minuten", L"about 10-15 minutes") : T(L"ca. 1 bis 1,5 Stunden", L"about 1 to 1.5 hours"), g_aus.c_str())).c_str());
    fflush(stdout); getchar();
  }
  CreateDirectoryW(g_aus.c_str(), nullptr); wstring dumps = g_aus + L"\\dumps", fehlerDir = g_aus + L"\\errors";
  CreateDirectoryW(dumps.c_str(), nullptr); CreateDirectoryW(fehlerDir.c_str(), nullptr);
  g_log = _wfopen((g_aus + L"\\log.txt").c_str(), L"ab"); g_csv = _wfopen((g_aus + L"\\results.csv").c_str(), L"ab");
  fputs("time;cpu;coretype;core;apic;load;run;seconds;mhz_avg;result;exitcode;detail\n", g_csv); fflush(g_csv);
  SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);

  // Topologie: Effizienzklasse je logischem Prozessor, Kernnummer je Klasse
  int ncpu = 0; vector<int> klasse(64, -1), kern(64, -1);
  { DWORD len = 0; GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len); vector<char> buf(len);
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) {
      int kp = 0, ke = 0;
      for (DWORD off = 0; off < len;) {
        auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
        int k = ((BYTE*)&e->Processor.Flags)[1] ? kp++ : ke++;
        for (int bit = 0; bit < 64; bit++) if (e->Processor.GroupMask[0].Mask & ((KAFFINITY)1 << bit)) { klasse[bit] = ((BYTE*)&e->Processor.Flags)[1]; kern[bit] = k; ncpu = std::max(ncpu, bit + 1); }
        off += e->Size;
      }
    } }
  if (bis < 0) bis = ncpu - 1;
  bool hybrid = false; for (int c = 0; c < ncpu; c++) if (klasse[c] != klasse[0]) hybrid = true;
  if (!hybrid) for (int c = 0; c < ncpu; c++) klasse[c] = 1;            // Raptor Lake ohne E-Kerne (z. B. i3-13100/14100): alle wie P-Kerne testen
  int eRef = ncpu - 1; if (hybrid) for (int c = ncpu - 1; c >= 0; c--) if (klasse[c] == 0) { eRef = c; break; }
  SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)1 << eRef);   // Steuerung selbst auf einem E-Kern

  wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH); wstring exe = self, ordner = exe.substr(0, exe.find_last_of(L'\\'));
  wstring pyskript = ordner + L"\\pure_win.py";
  wstring pyexe = GetFileAttributesW((ordner + L"\\python\\python.exe").c_str()) != INVALID_FILE_ATTRIBUTES ? L"\"" + ordner + L"\\python\\python.exe\"" : L"python.exe";   // mitgeliefertes Python bevorzugen
  bool mitPy = py > 0 && GetFileAttributesW(pyskript.c_str()) != INVALID_FILE_ATTRIBUTES;
  Log(Fmt(T(L"Kerntest: %d logische Prozessoren, getestet %d–%d, %g s je Lauf, py %d×, nat %d× (E-Kerne %d Runde(n)), Referenz auf CPU %d",
            L"coretest: %d logical processors, testing %d-%d, %g s per run, py %dx, nat %dx (E-cores %d round(s)), reference on CPU %d"),
          ncpu, von, bis, sek, mitPy ? py : 0, nat, eRunden, eRef));

  // Turbo an (Tray schließen, sonst setzt die Turbo-Sperre ihn sofort zurück)
  if (HWND t = FindWindowW(L"PowerPlanTrayNativ", nullptr)) { PostMessageW(t, WM_CLOSE, 0, 0); Sleep(2000); g_trayWar = true; Log(T(L"PowerPlanTray geschlossen", L"PowerPlanTray closed")); }
  GUID* ag = nullptr; PowerGetActiveScheme(nullptr, &ag); g_plan = *ag; LocalFree(ag);
  PowerReadACValueIndex(nullptr, &g_plan, &SUB_PROC, &BOOST, &g_altAC); PowerReadDCValueIndex(nullptr, &g_plan, &SUB_PROC, &BOOST, &g_altDC);
  SetConsoleCtrlHandler(Strg, TRUE); g_turboGesetzt = true; SetzeBoost(2); Log(T(L"Turbo an (Boost Aggressiv) im aktiven Plan", L"Turbo on (boost mode Aggressive) in the active plan"));
  Sleep(3000);

  // Referenzwerte auf dem E-Kern, zweimal, müssen gleich sein
  wstring gold = g_aus + L"\\golden.bin"; vector<Gold> g[2];
  for (int k = 0; k < 2; k++) {
    Lauf l = Starte(L"\"" + exe + L"\" --worker - 0 \"" + dumps + L"\"", eRef, 600);
    g[k].resize(SEEDS); int gef = 0; size_t p = 0;
    while ((p = l.out.find("gold ", p)) != string::npos) { int s; unsigned long long a, b2, c; if (sscanf(l.out.c_str() + p, "gold %d %llx %llx %llx", &s, &a, &b2, &c) == 4 && s < SEEDS) { g[k][s] = { a, b2, c }; gef++; } p += 5; }
    if (gef != SEEDS) { Log(T(L"Referenz fehlgeschlagen: ", L"Reference failed: ") + W(l.out)); Aufraeumen(); return 2; }
  }
  if (memcmp(g[0].data(), g[1].data(), sizeof(Gold) * SEEDS)) { Log(T(L"Referenz zweimal verschieden – Referenzkern selbst unzuverlässig, Abbruch", L"Reference differs between two runs – reference core itself unreliable, aborting")); Aufraeumen(); return 2; }
  { FILE* f = _wfopen(gold.c_str(), L"wb"); fwrite(g[0].data(), sizeof(Gold), SEEDS, f); fclose(f); }
  string pyRef;
  if (mitPy) {
    Lauf l = Starte(pyexe + L" -X faulthandler \"" + pyskript + L"\" 15", eRef, 15);
    string z = Zeile(l.out, "ok "); if (z.empty()) { Log(T(L"Python-Referenz fehlgeschlagen, nur C++-Last: ", L"Python reference failed, C++ load only: ") + W(l.out)); mitPy = false; }
    else { pyRef = z.substr(z.find_last_of(' ') + 1); Log(T(L"Python-Referenz: ", L"Python reference: ") + W(pyRef)); }
  }
  Log(T(L"Referenzen ok", L"References ok"));

  struct Sum { int laeufe = 0, fehl = 0; unsigned apic = 0; double mhz = 0; };
  vector<Sum> sum(ncpu);
  for (int cpu = von; cpu <= bis && cpu < ncpu; cpu++) {
    if (cpu == eRef) continue;
    { Lauf a = Starte(L"\"" + exe + L"\" --apic", cpu, 5); string az = Zeile(a.out, "apic "); if (!az.empty()) sum[cpu].apic = (unsigned)atoi(az.c_str() + 5); }
    bool pk = klasse[cpu] > 0; int np = pk ? py : (mitPy ? std::min(py, eRunden) : 0), nn = pk ? nat : std::min(nat, eRunden);
    for (int art : { 1, 0 }) for (int r = 0; r < (art == 0 ? (mitPy ? np : 0) : nn); r++) {   // erst C++ (liefert die APIC-ID), dann Python
      double ls = pk ? sek : sek / 2;                                  // E-Kerne: nur zum Vergleich, halbe Laufzeit
      Lauf l = art == 0 ? Starte(pyexe + L" -X faulthandler \"" + pyskript + L"\" " + Fmt(L"%g", ls), cpu, ls)
                        : Starte(L"\"" + exe + L"\" --worker \"" + gold + L"\" " + Fmt(L"%g", ls) + L" \"" + dumps + L"\"", cpu, ls);
      unsigned apic = sum[cpu].apic; string az = Zeile(l.out, "apic "); if (!az.empty()) apic = (unsigned)atoi(az.c_str() + 5);
      string erg, detail;
      if (art == 0) {
        string ok = Zeile(l.out, "ok "), mm = Zeile(l.out, "MISMATCH");
        if (l.timeout) erg = "HANG";
        else if (!ok.empty() && l.code == 0) { string ref = ok.substr(ok.find_last_of(' ') + 1); erg = ref == pyRef ? "ok" : "WRONG"; if (erg != "ok") detail = "result " + ref + " instead of " + pyRef; }
        else if (!mm.empty()) { erg = "WRONG"; detail = mm; }
        else { erg = (l.code >= 0xC0000000) ? "CRASH" : "ERROR"; string t = l.out; std::replace(t.begin(), t.end(), '\n', ' '); std::replace(t.begin(), t.end(), ';', ','); detail = t.substr(t.size() > 300 ? t.size() - 300 : 0); }
      } else {
        string z = Zeile(l.out, "ok "); string f = Zeile(l.out, "WRONG"); string c = Zeile(l.out, "CRASH ");
        if (l.timeout) erg = "HANG"; else if (!c.empty()) { erg = "CRASH"; detail = c; } else if (!f.empty()) { erg = "WRONG"; detail = f + " | " + Zeile(l.out, "MISMATCH"); }
        else if (!z.empty() && l.code == 0) { erg = "ok"; detail = z; } else { erg = l.code >= 0xC0000000 ? "CRASH" : "ERROR"; detail = l.out.substr(0, 200); std::replace(detail.begin(), detail.end(), '\n', ' '); }
      }
      std::replace(detail.begin(), detail.end(), ';', ',');
      SYSTEMTIME st; GetLocalTime(&st);
      wstring kt = pk ? L"P" : L"E";
      fprintf(g_csv, "%04d-%02d-%02d %02d:%02d:%02d;%d;%s;%d;%u;%s;%d;%g;%.0f;%s;0x%08lX;%s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              cpu, U8(kt).c_str(), kern[cpu], apic, art ? "nat" : "py", r + 1, sek, l.mhz, erg.c_str(), l.code, detail.c_str()); fflush(g_csv);
      if (erg != "ok") {
        FILE* f = _wfopen((fehlerDir + Fmt(L"\\cpu%02d-apic%u-%s-%d-%02d%02d%02d.txt", cpu, apic, art ? L"nat" : L"py", r + 1, st.wHour, st.wMinute, st.wSecond)).c_str(), L"wb");
        if (f) { fprintf(f, "CPU %d (%s-core %d), APIC %u, load %s, exit 0x%08lX, average clock %.0f MHz\n\n%s", cpu, U8(kt).c_str(), kern[cpu], apic, art ? "nat" : "py", l.code, l.mhz, l.out.c_str()); fclose(f); }
      }
      sum[cpu].laeufe++; if (erg != "ok") sum[cpu].fehl++; if (apic) sum[cpu].apic = apic; sum[cpu].mhz = std::max(sum[cpu].mhz, l.mhz);
      Log(Fmt(T(L"CPU %2d %s-Kern %d APIC %3u  %s %d  %5.0f MHz  %s %s", L"CPU %2d %s-core %d APIC %3u  %s %d  %5.0f MHz  %s %s"), cpu, kt.c_str(), kern[cpu], apic, art ? L"nat" : L"py ", r + 1, l.mhz, W(erg).c_str(), W(detail.substr(0, 120)).c_str()));
    }
  }
  Aufraeumen();
  // ---------------- Bericht
  const wchar_t* CP = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
  wstring cpuName = RegText(HKEY_LOCAL_MACHINE, CP, L"ProcessorNameString"); DWORD nennMhz = RegZahl(HKEY_LOCAL_MACHINE, CP, L"~MHz");
  wstring bios = RegText(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BIOSVersion"), board = RegText(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardProduct");
  wstring build = RegText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild");
  wstring pyVer = L"–"; if (mitPy) { Lauf v = Starte(pyexe + L" -V", eRef, 5); pyVer = W(v.out); while (!pyVer.empty() && iswspace(pyVer.back())) pyVer.pop_back(); }
  double maxP = 0; int fehlerKerne = 0, laeufe = 0, fehlLaeufe = 0;
  for (int c = von; c <= bis && c < ncpu; c++) { if (klasse[c] > 0) maxP = std::max(maxP, sum[c].mhz); if (sum[c].fehl) fehlerKerne++; laeufe += sum[c].laeufe; fehlLaeufe += sum[c].fehl; }
  bool turboAktiv = maxP > nennMhz * 1.15;
  wstring t, kernW = T(L"Kern", L"core");
  t += T(L"CORETEST – Bericht\r\n==================\r\n\r\n", L"CORETEST – report\r\n=================\r\n\r\n");
  { SYSTEMTIME st; GetLocalTime(&st); t += Fmt(T(L"Datum:      %02d.%02d.%04d %02d:%02d\r\n", L"Date:       %04d-%02d-%02d %02d:%02d\r\n"),
      g_de ? st.wDay : st.wYear, st.wMonth, g_de ? st.wYear : st.wDay, st.wHour, st.wMinute); }
  t += L"CPU:        " + cpuName + Fmt(T(L" (Nenntakt %u MHz, %d logische Prozessoren%s)\r\n", L" (base clock %u MHz, %d logical processors%s)\r\n"), nennMhz, ncpu, hybrid ? T(L", P- und E-Kerne", L", P- and E-cores") : L"");
  t += L"Microcode:  " + Microcode() + L"\r\nMainboard:  " + board + L", BIOS " + bios + L"\r\nWindows:    Build " + build + L"\r\nPython:     " + pyVer
     + (mitPy ? L"" : T(L" (FEHLT – der aussagekräftigste Test lief nicht!)", L" (MISSING – the most sensitive test did not run!)")) + L"\r\n";
  t += Fmt(T(L"Test:       %s, %g s je Lauf, Python %d×, C++ %d× je P-Kern; %d Läufe\r\n\r\n", L"Test:       %s, %g s per run, Python %dx, C++ %dx per P-core; %d runs\r\n\r\n"),
           schnell ? T(L"schnell", L"quick") : L"Standard", sek, mitPy ? py : 0, nat, laeufe);
  if (fehlerKerne) {
    t += Fmt(T(L"ERGEBNIS: FEHLER GEFUNDEN – %d Kern(e) liefern unter Turbo falsche Ergebnisse oder stürzen ab (%d von %d Läufen).\r\n\r\n",
               L"RESULT: ERRORS FOUND – %d core(s) returned wrong results or crashed under turbo (%d of %d runs).\r\n\r\n"), fehlerKerne, fehlLaeufe, laeufe);
    t += T(L"Betroffen:\r\n", L"Affected:\r\n");
    for (int c = von; c <= bis && c < ncpu; c++) if (sum[c].fehl)
      t += Fmt(T(L"  CPU %2d  (%s-Kern %d, APIC-ID %u)  %d von %d Läufen fehlerhaft, Takt bis %.0f MHz\r\n", L"  CPU %2d  (%s-core %d, APIC ID %u)  %d of %d runs failed, clock up to %.0f MHz\r\n"),
               c, klasse[c] > 0 ? L"P" : L"E", kern[c], sum[c].apic, sum[c].fehl, sum[c].laeufe, sum[c].mhz);
    t += T(L"\r\nDas heißt: Die CPU ist bei hohem Turbo-Takt instabil (bei Intel 13./14. Gen typisch für die bekannte\r\n"
           L"Degradation, „Vmin Shift“). Solche Fehler erscheinen meist NICHT als WHEA-Fehler im Ereignisprotokoll,\r\n"
           L"können aber Programme abstürzen lassen und Daten still verfälschen (Dateien, Backups, Exporte).\r\n\r\n"
           L"Was tun:\r\n  1. Wichtige Daten sichern und nichts Wichtiges mit Turbo rechnen lassen.\r\n"
           L"  2. Gegenprobe: Turbo aus (Energieplan → Prozessor-Leistungssteigerungsmodus „Deaktiviert“ bzw. BIOS) und\r\n"
           L"     Test wiederholen – verschwinden die Fehler, ist das ein klarer Hinweis auf die CPU.\r\n"
           L"  3. BIOS aktualisieren (Microcode 0x12B oder neuer), „Intel Default Settings“ verwenden.\r\n"
           L"  4. Bei Intel eine Garantie-Anfrage (RMA) stellen – 13./14. Gen haben 5 Jahre erweiterte Garantie.\r\n"
           L"     Diesen Ordner (bericht.txt, results.csv, errors\\, dumps\\) als Beleg beilegen.\r\n\r\n",
           L"\r\nThis means: the CPU is unstable at high turbo clocks (on Intel 13th/14th gen typical for the known\r\n"
           L"degradation, \"Vmin shift\"). Such errors usually do NOT show up as WHEA errors in the event log, but they\r\n"
           L"can crash programs and silently corrupt data (files, backups, exports).\r\n\r\n"
           L"What to do:\r\n  1. Back up important data and don't run important work with turbo enabled.\r\n"
           L"  2. Cross-check: disable turbo (power plan -> processor performance boost mode \"Disabled\", or BIOS) and\r\n"
           L"     run the test again. If the errors disappear, that clearly points at the CPU.\r\n"
           L"  3. Update the BIOS (microcode 0x12B or newer) and use \"Intel Default Settings\".\r\n"
           L"  4. Contact Intel for a warranty replacement (RMA) – 13th/14th gen have an extended 5 year warranty.\r\n"
           L"     Attach this folder (report.txt, results.csv, errors\\, dumps\\) as evidence.\r\n\r\n");
  } else {
    t += Fmt(T(L"ERGEBNIS: keine Fehler gefunden (%d Läufe).\r\n\r\n", L"RESULT: no errors found (%d runs).\r\n\r\n"), laeufe);
    t += T(L"Das ist ein gutes Zeichen, aber kein Beweis für eine gesunde CPU: Fehler können selten sein.\r\n"
           L"Bei weiter ungeklärten Abstürzen: Test länger laufen lassen (z. B. --sek 120 --py 5) und zusätzlich\r\n"
           L"einen anerkannten Einzelkern-Test wie CoreCycler (Prime95/y-cruncher) verwenden.\r\n\r\n",
           L"That's a good sign, but no proof of a healthy CPU: errors can be rare.\r\n"
           L"If unexplained crashes continue: run longer (e.g. --sek 120 --py 5) and additionally use an established\r\n"
           L"per-core test such as CoreCycler (Prime95/y-cruncher).\r\n\r\n");
  }
  if (!turboAktiv) t += Fmt(T(L"ACHTUNG: Höchster gemessener Takt %.0f MHz – der Turbo war offenbar nicht aktiv (BIOS?). Der Test ist dann wenig aussagekräftig.\r\n\r\n",
                              L"WARNING: highest measured clock %.0f MHz – turbo apparently was not active (BIOS?). The test is then not very meaningful.\r\n\r\n"), maxP);
  t += T(L"Alle Kerne (Fehler / Läufe, höchster mittlerer Takt):\r\n", L"All cores (failed / runs, highest average clock):\r\n");
  for (int c = von; c <= bis && c < ncpu; c++) if (sum[c].laeufe)
    t += Fmt(L"  CPU %2d  %s-%s %2d  APIC %3u  %5.0f MHz  %d/%d %s\r\n", c, klasse[c] > 0 ? L"P" : L"E", kernW.c_str(), kern[c], sum[c].apic, sum[c].mhz, sum[c].fehl, sum[c].laeufe, sum[c].fehl ? T(L"<<< FEHLER", L"<<< FAILED") : L"");
  t += T(L"\r\nErklärung: Jeder logische Prozessor wurde einzeln (Affinity) mit vollem Single-Core-Turbo belastet:\r\n"
         L"„py“ = Python-Interpreter (empfindlichster Test: viele Speicherzugriffe und Sprünge), „nat“ = C++-Rechenlast.\r\n"
         L"Ergebnisse wurden mit Referenzwerten verglichen, die vorher auf einem anderen Kern berechnet wurden.\r\n"
         L"WRONG = falsches Ergebnis, CRASH = Zugriffsverletzung o. ä., ERROR = unmögliche Programmfehlermeldung, HANG = hängt.\r\n"
         L"APIC-ID = Hardware-Nummer des Kerns (wie in WHEA-Meldungen). Details je Fehler im Unterordner „errors“.\r\n",
         L"\r\nExplanation: each logical processor was loaded on its own (affinity) at full single-core turbo:\r\n"
         L"\"py\" = Python interpreter (most sensitive: many memory accesses and jumps), \"nat\" = C++ compute load.\r\n"
         L"Results were compared against reference values computed beforehand on another core.\r\n"
         L"WRONG = wrong result, CRASH = access violation etc., ERROR = impossible program error, HANG = hung.\r\n"
         L"APIC ID = hardware number of the core (same as in WHEA events). Details per failure in the \"errors\" folder.\r\n");
  wstring berichtDat = g_aus + (g_de ? L"\\bericht.txt" : L"\\report.txt");
  { FILE* z = _wfopen(berichtDat.c_str(), L"wb"); if (z) { fputs("\xEF\xBB\xBF", z); fputs(U8(t).c_str(), z); fclose(z); } }
  Log(L"\n" + t);
  if (interaktiv) {
    ShellExecuteW(nullptr, L"open", berichtDat.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    printf("%s", U8(Fmt(T(L"\n  Fertig. Der Bericht ist geöffnet (Ordner: %s).\n  Enter = Fenster schließen ", L"\n  Done. The report is open (folder: %s).\n  Enter = close window "), g_aus.c_str())).c_str()); fflush(stdout); getchar();
  }
  SetThreadExecutionState(ES_CONTINUOUS);
  return 0;
}
