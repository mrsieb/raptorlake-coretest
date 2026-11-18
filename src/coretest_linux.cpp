// coretest (Linux) - finds CPU cores that silently compute wrong results under single-core turbo (Intel 13th/14th gen degradation).
// Same test as coretest.exe (Windows), same load (workload.h), same report. Differences on Linux:
//   - turbo is switched on via /sys/devices/system/cpu/intel_pstate/no_turbo (or cpufreq/boost) and restored afterwards; needs root.
//     Without root the test still runs if turbo is already on (the usual default).
//   - affinity via sched_setaffinity, P/E cores from /sys/devices/cpu_core|cpu_atom/cpus, clock from cpufreq/scaling_cur_freq.
//   - test processes die with coretest (PR_SET_PDEATHSIG); Ctrl+C, closing the terminal and kill restore the turbo setting,
//     after a hard kill (-9) it is restored on the next start (/var/tmp/coretest-restore).
//   - no minidumps: crashes are reported with signal, instruction address and fault address.
//   - refuses to run in a VM/WSL (affinity does not hit real cores there), --vm-ok skips that check (for development only).
// Usage: sudo ./coretest [output folder] [--quick] [--cpus 0-31] [--sek 60] [--py 3] [--nat 1] [--ekern-runden 1]
// Internal: coretest --worker <golden.bin|-> <seconds>, coretest --apic
// Comments in the code are German (my native language), program output is German or English depending on LANG.
#include "workload.h"
#include <sched.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <ftw.h>
#include <ucontext.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <map>
#include <string>
#include <tuple>

using std::string; using std::vector;

static bool g_de = false;                                  // Ausgabe Deutsch, wenn LANG/LC_* auf Deutsch steht, sonst Englisch
static const char* T(const char* de, const char* en) { return g_de ? de : en; }
static string Fmt(const char* f, ...) { char b[8192]; va_list v; va_start(v, f); vsnprintf(b, sizeof b, f, v); va_end(v); return b; }
static string Lies(const string& pfad) {                   // ganze Datei, Leerraum am Ende entfernt; "" wenn nicht lesbar
  FILE* f = fopen(pfad.c_str(), "rb"); if (!f) return ""; string r; char b[4096]; size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) r.append(b, n);
  fclose(f);
  while (!r.empty() && isspace((unsigned char)r.back())) r.pop_back();
  return r; }
static bool Schreib(const string& pfad, const string& text) {
  int fd = open(pfad.c_str(), O_WRONLY | O_CLOEXEC); if (fd < 0) return false;
  bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size(); close(fd); return ok; }
static bool Gibt(const string& pfad) { return access(pfad.c_str(), F_OK) == 0; }
static uint64_t Ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static tm Jetzt() { time_t t = time(nullptr); tm r; localtime_r(&t, &r); return r; }
static string CpuinfoFeld(const char* name) {               // erstes Vorkommen in /proc/cpuinfo
  string c = Lies("/proc/cpuinfo"); size_t p = c.find(string("\n") + name); if (p == string::npos) return "?";
  size_t d = c.find(':', p); size_t e = c.find('\n', p + 1); if (d == string::npos || d > e) return "?";
  string v = c.substr(d + 1, e == string::npos ? string::npos : e - d - 1); while (!v.empty() && isspace((unsigned char)v.front())) v.erase(0, 1); return v; }
static vector<int> CpuListe(const string& s) {              // "0-15,24-31" -> Liste
  vector<int> r; size_t i = 0;
  while (i < s.size()) {
    int a, b, n = 0; if (sscanf(s.c_str() + i, "%d%n", &a, &n) != 1) break; i += n; b = a;
    if (i < s.size() && s[i] == '-') { i++; if (sscanf(s.c_str() + i, "%d%n", &b, &n) != 1) break; i += n; }
    for (int c = a; c <= b && c < 4096; c++) r.push_back(c);
    if (i < s.size() && s[i] == ',') i++; else break;
  }
  return r; }

// ============================================================ Worker
static void Absturz(int sig, siginfo_t* si, void* ctx) {
  ucontext_t* u = (ucontext_t*)ctx; char b[200];
  int n = snprintf(b, sizeof b, "CRASH signal %d at %p, address %p, apic %u\n", sig, (void*)u->uc_mcontext.gregs[REG_RIP], si->si_addr, ApicId());
  if (n > 0) { ssize_t w = write(1, b, (size_t)n); (void)w; }
  signal(sig, SIG_DFL); raise(sig);                        // Signal-Ende bleibt für die Steuerung sichtbar
}
static int Worker(const char* gold, double sek) {
  struct sigaction sa {}; sa.sa_sigaction = Absturz; sa.sa_flags = SA_SIGINFO;
  for (int s : { SIGSEGV, SIGBUS, SIGILL, SIGFPE }) sigaction(s, &sa, nullptr);
  printf("apic %u\n", ApicId()); fflush(stdout);
  if (!strcmp(gold, "-")) {                                // Referenz erzeugen
    for (int s = 0; s < SEEDS; s++) { Gold g = Block(s); printf("gold %d %016llx %016llx %016llx\n", s, (unsigned long long)g.f, (unsigned long long)g.v, (unsigned long long)g.k); }
    return 0;
  }
  vector<Gold> ref(SEEDS);
  FILE* f = fopen(gold, "rb"); if (!f || fread(ref.data(), sizeof(Gold), SEEDS, f) != SEEDS) { printf("ERROR golden\n"); return 9; } fclose(f);
  uint64_t ende = Ms() + (uint64_t)(sek * 1000); int n = 0, fehl = 0;
  while (Ms() < ende) {
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
static string g_aus; static FILE* g_log; static FILE* g_csv;
static void Log(const string& t) {
  tm s = Jetzt(); string z = Fmt("%02d:%02d:%02d  %s\n", s.tm_hour, s.tm_min, s.tm_sec, t.c_str());
  fputs(z.c_str(), stdout); fflush(stdout); if (g_log) { fputs(z.c_str(), g_log); fflush(g_log); }
}

// Turbo: intel_pstate/no_turbo (0 = Turbo an) oder cpufreq/boost (1 = Turbo an, acpi-cpufreq)
#ifndef MERK_PFAD                                           // nur für Tests überschreibbar (-D...)
#define MERK_PFAD "/var/tmp/coretest-restore"
#define PSTATE_PFAD "/sys/devices/system/cpu/intel_pstate/no_turbo"
#endif
static const char* MERK = MERK_PFAD;    // alte Turbo-Werte, falls coretest hart beendet wird (kill -9, Absturz)
static const char* PSTATE = PSTATE_PFAD;
static const char* BOOST = "/sys/devices/system/cpu/cpufreq/boost";
static char s_pfad[128], s_alt[8];                         // für den Signal-Handler vorab als C-Strings
static volatile sig_atomic_t g_turboGesetzt = 0; static volatile pid_t g_kind = 0, g_inhibit = 0;
static string g_turboPfad, g_turboAn; static bool g_turboWirkt = false;   // Turbo ist an bzw. ließ sich einschalten
static void TurboZurueckRoh() {                            // async-signal-safe
  int fd = open(s_pfad, O_WRONLY); if (fd >= 0) { ssize_t w = write(fd, s_alt, strlen(s_alt)); (void)w; close(fd); } unlink(MERK); }
static void Aufraeumen() {
  if (g_inhibit > 0) { kill(g_inhibit, SIGTERM); waitpid(g_inhibit, nullptr, 0); g_inhibit = 0; }
  if (!g_turboGesetzt) return;
  g_turboGesetzt = 0; TurboZurueckRoh();
  Log(Fmt(T("Turbo-Einstellung wie vorher (%s = %s)", "Turbo setting restored (%s = %s)"), s_pfad, s_alt));
}
static void Abbruch(int sig) {                             // Strg+C, Terminal geschlossen, kill
  if (g_kind > 0) kill(g_kind, SIGKILL);
  if (g_inhibit > 0) kill(g_inhibit, SIGTERM);
  if (g_turboGesetzt) {
    TurboZurueckRoh();
    const char* m = g_de ? "\n  Abgebrochen - Turbo-Einstellung wiederhergestellt.\n" : "\n  Cancelled - turbo setting restored.\n";
    ssize_t w = write(1, m, strlen(m)); (void)w;
  }
  _exit(128 + sig);
}
static void AlteWerteNachAbsturz() {
  string m = Lies(MERK); if (m.empty()) return;
  char pfad[128] = "", wert[8] = ""; if (sscanf(m.c_str(), "%127s %7s", pfad, wert) == 2 && (!strcmp(pfad, PSTATE) || !strcmp(pfad, BOOST)) && (!strcmp(wert, "0") || !strcmp(wert, "1"))) {
    if (Schreib(pfad, wert)) printf(T("\n  Ein früherer Test wurde nicht sauber beendet – Turbo-Einstellung wiederhergestellt (%s = %s).\n",
                                     "\n  A previous test did not exit cleanly – turbo setting restored (%s = %s).\n"), pfad, wert);
  }
  unlink(MERK);
}
static bool TurboAn() { return !g_turboPfad.empty() && Lies(g_turboPfad) == g_turboAn; }

struct Lauf { string out; int code = 0, sig = 0; double mhz = 0; bool timeout = false; };
static string ExitText(const Lauf& l) { return l.sig ? Fmt("signal %d", l.sig) : Fmt("%d", l.code); }
static double Mhz(int cpu) {                               // aktueller Takt des Ziel-Prozessors
  string s = Lies(Fmt("/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu)); if (!s.empty()) return atof(s.c_str()) / 1000;
  string c = Lies("/proc/cpuinfo"); size_t p = c.find(Fmt("processor\t: %d\n", cpu)); if (p == string::npos) return 0;
  size_t m = c.find("cpu MHz", p), e = c.find("\n\n", p); if (m == string::npos || (e != string::npos && m > e)) return 0;
  size_t d = c.find(':', m); return d == string::npos ? 0 : atof(c.c_str() + d + 1); }
static pid_t Kind(const vector<string>& args, int cpu, int ausFd) {   // Prozess auf genau einem logischen Prozessor
  pid_t eltern = getpid(), pid = fork();
  if (pid != 0) return pid;
  prctl(PR_SET_PDEATHSIG, SIGKILL); if (getppid() != eltern) _exit(126);   // endet mit coretest
  for (int s : { SIGINT, SIGTERM, SIGHUP, SIGPIPE }) signal(s, SIG_DFL);
  if (ausFd >= 0) { dup2(ausFd, 1); dup2(ausFd, 2); } else { int n = open("/dev/null", O_WRONLY); if (n >= 0) { dup2(n, 1); dup2(n, 2); } }
  if (cpu >= 0) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); if (sched_setaffinity(0, sizeof s, &s)) { dprintf(1, "affinity cpu %d failed: %s\n", cpu, strerror(errno)); _exit(125); } }
  vector<char*> a; for (auto& x : args) a.push_back((char*)x.c_str()); a.push_back(nullptr);
  execvp(a[0], a.data()); dprintf(1, "exec %s failed: %s\n", a[0], strerror(errno)); _exit(127);
}
static Lauf Starte(const vector<string>& args, int cpu, double sek) {
  Lauf r; int p[2]; if (pipe2(p, O_CLOEXEC)) { r.out = "pipe failed"; r.code = 124; return r; }
  pid_t pid = Kind(args, cpu, p[1]); close(p[1]);
  if (pid < 0) { close(p[0]); r.out = "fork failed"; r.code = 124; return r; }
  g_kind = pid;
  uint64_t ende = Ms() + (uint64_t)(sek * 1000) + 60000, naechst = Ms() + 1000; double summe = 0; int n = 0, st = 0; bool fertig = false, eof = false;
  char b[4096];
  while (!fertig) {
    pollfd pf{ p[0], POLLIN, 0 };
    if (!eof && poll(&pf, 1, 100) > 0) { ssize_t m = read(p[0], b, sizeof b); if (m > 0) r.out.append(b, (size_t)m); else if (m == 0) eof = true; }
    else if (eof) usleep(100000);
    if (waitpid(pid, &st, WNOHANG) == pid) fertig = true;
    else {
      if (Ms() >= naechst) { naechst += 1000; double f = Mhz(cpu); if (f > 0) { summe += f; n++; } }
      if (Ms() > ende) { kill(pid, SIGKILL); waitpid(pid, &st, 0); r.timeout = true; fertig = true; }
    }
  }
  g_kind = 0;
  for (ssize_t m; (m = read(p[0], b, sizeof b)) > 0;) r.out.append(b, (size_t)m);
  close(p[0]);
  if (WIFEXITED(st)) r.code = WEXITSTATUS(st); else if (WIFSIGNALED(st)) r.sig = WTERMSIG(st);
  r.mhz = n ? summe / n : 0; return r;
}
static string Zeile(const string& out, const char* pre) { size_t p = out.find(pre); if (p == string::npos) return ""; size_t e = out.find('\n', p); string z = out.substr(p, e == string::npos ? string::npos : e - p); while (!z.empty() && (z.back() == '\r' || z.back() == ' ')) z.pop_back(); return z; }
static string Pfad(const string& name) {                   // Programm im PATH finden
  if (name.find('/') != string::npos) return access(name.c_str(), X_OK) == 0 ? name : "";
  const char* p = getenv("PATH"); string s = p ? p : "/usr/local/bin:/usr/bin:/bin"; size_t i = 0;
  while (i <= s.size()) { size_t e = s.find(':', i); if (e == string::npos) e = s.size(); string d = s.substr(i, e - i) + "/" + name; if (e > i && access(d.c_str(), X_OK) == 0) return d; i = e + 1; }
  return ""; }
static uid_t g_uid = 0; static gid_t g_gid = 0; static bool g_chown = false;
static int Besitz(const char* p, const struct stat*, int, struct FTW*) { if (lchown(p, g_uid, g_gid)) {} return 0; }

int main(int argc, char** argv) {
  if (argc >= 4 && !strcmp(argv[1], "--worker")) return Worker(argv[2], atof(argv[3]));
  if (argc >= 2 && !strcmp(argv[1], "--apic")) { printf("apic %u\n", ApicId()); return 0; }
  { const char* l = getenv("LC_ALL"); if (!l || !*l) l = getenv("LC_MESSAGES"); if (!l || !*l) l = getenv("LANG"); g_de = l && !strncmp(l, "de", 2); }
  int von = 0, bis = -1, py = 3, nat = 1, eRunden = 1; double sek = 60; bool schnell = false, vmOk = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--cpus") && i + 1 < argc) { if (sscanf(argv[++i], "%d-%d", &von, &bis) < 2 || bis < von) bis = von; }
    else if (!strcmp(argv[i], "--sek") && i + 1 < argc) sek = atof(argv[++i]);
    else if (!strcmp(argv[i], "--py") && i + 1 < argc) py = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--nat") && i + 1 < argc) nat = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--ekern-runden") && i + 1 < argc) eRunden = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--schnell") || !strcmp(argv[i], "--quick")) { schnell = true; py = 1; nat = 0; sek = 45; eRunden = 0; }
    else if (!strcmp(argv[i], "--vm-ok")) vmOk = true;
    else if (!strcmp(argv[i], "--hilfe") || !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      printf("%s", T("coretest – findet CPU-Kerne, die unter Single-Core-Turbo still falsch rechnen (Intel 13./14. Gen)\n\n"
             "Aufruf: sudo ./coretest [Ausgabeordner] [--schnell] [--cpus 0-31] [--sek 60] [--py 3] [--nat 1] [--ekern-runden 1]\n"
             "Ohne Angaben: Ordner ./CoreTest-<Datum>, Standardtest (ca. 1-1,5 h), wartet auf Enter.\n"
             "--schnell: nur P-Kerne, je 45 s Python (ca. 10-15 min).\n"
             "root wird gebraucht, um den Turbo einzuschalten und danach zurückzustellen.\n",
             "coretest – finds CPU cores that silently compute wrong results under single-core turbo (Intel 13th/14th gen)\n\n"
             "Usage: sudo ./coretest [output folder] [--quick] [--cpus 0-31] [--sek 60] [--py 3] [--nat 1] [--ekern-runden 1]\n"
             "No arguments: folder ./CoreTest-<date>, standard run (about 1-1.5 h), waits for Enter.\n"
             "--quick: P-cores only, 45 s Python each (about 10-15 min).\n"
             "root is needed to switch turbo on and restore it afterwards.\n"));
      return 0; }
    else if (argv[i][0] != '-') g_aus = argv[i];
  }
  bool root = geteuid() == 0;
  if (root) AlteWerteNachAbsturz();                         // vorheriger Lauf hart beendet? Turbo zurückstellen
  { unsigned a = 0, b = 0, c = 0, d = 0; __get_cpuid(1, &a, &b, &c, &d);
    bool vm = (c >> 31) & 1; string v = Lies("/proc/version"); for (auto& ch : v) ch = (char)tolower(ch);
    if ((vm || v.find("microsoft") != string::npos) && !vmOk) {
      printf("%s", T("\n  Dieses Linux läuft in einer virtuellen Maschine bzw. WSL. Dort trifft die Kernzuordnung keine echten\n"
                     "  Kerne und der Turbo ist nicht steuerbar – der Test wäre wertlos. Bitte auf einem echt installierten\n"
                     "  Linux oder einem Live-USB-Stick starten (unter Windows: coretest.exe verwenden).\n",
                     "\n  This Linux runs in a virtual machine or WSL. There, affinity does not hit real cores and turbo\n"
                     "  cannot be controlled – the test would be meaningless. Please run it on a real Linux installation or\n"
                     "  a live USB stick (on Windows: use coretest.exe).\n"));
      return 2; } }
  bool interaktiv = g_aus.empty();
  string cpuName = CpuinfoFeld("model name");
  { // gedacht für Intel Core 13./14. Generation (Raptor Lake) – sonst nur Hinweis
    bool rpl = cpuName.find("Intel") != string::npos && (cpuName.find("-13") != string::npos || cpuName.find("-14") != string::npos);
    if (!rpl) printf(T("\n  HINWEIS: Dieser Test ist für Intel Core 13./14. Generation (Raptor Lake) gedacht.\n  Erkannte CPU: %s\n  Er läuft trotzdem, die Erklärungen im Bericht passen dann aber nur bedingt.\n",
                       "\n  NOTE: This test is meant for Intel Core 13th/14th gen (Raptor Lake).\n  Detected CPU: %s\n  It runs anyway, but the explanations in the report may not apply.\n"), cpuName.c_str());
  }
  // Turbo-Steuerung finden
  if (Gibt(PSTATE)) { g_turboPfad = PSTATE; g_turboAn = "0"; } else if (Gibt(BOOST)) { g_turboPfad = BOOST; g_turboAn = "1"; }
  bool turboVorher = TurboAn(); g_turboWirkt = turboVorher;
  if (!root && !g_turboPfad.empty() && !turboVorher) {
    printf(T("\n  Der Turbo ist ausgeschaltet (%s). Zum Einschalten für den Test bitte mit sudo starten:\n    sudo %s\n",
             "\n  Turbo is switched off (%s). To switch it on for the test, please run with sudo:\n    sudo %s\n"), g_turboPfad.c_str(), argv[0]);
    return 2; }
  if (interaktiv) {
    char cwd[4096]; if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    tm st = Jetzt(); g_aus = string(cwd) + Fmt("/CoreTest-%04d-%02d-%02d_%02d%02d", st.tm_year + 1900, st.tm_mon + 1, st.tm_mday, st.tm_hour, st.tm_min);
    printf(T("\n  CORETEST – sucht CPU-Kerne, die mit Turbo still falsch rechnen\n  ===============================================================\n\n"
             "  Jeder Kern wird einzeln mit vollem Single-Core-Turbo belastet (Python-Interpreter + C++-Last),\n"
             "  die Ergebnisse werden mit Referenzwerten verglichen. Typisch für degradierte Intel 13./14. Gen:\n"
             "  einzelne Kerne liefern falsche Werte oder stürzen ab – oft OHNE Machine-Check-Meldung im Kernel-Log.\n\n"
             "  Dauer: %s. Den PC in der Zeit nicht benutzen (andere Last senkt den Turbo-Takt).\n"
             "  %s\n\n"
             "  Ergebnis: %s\n\n  Enter = starten, Strg+C = abbrechen ",
             "\n  CORETEST – finds CPU cores that silently compute wrong results under turbo\n  ==========================================================================\n\n"
             "  Each core is loaded on its own at full single-core turbo (Python interpreter + C++ load),\n"
             "  results are compared against reference values. Typical for degraded Intel 13th/14th gen:\n"
             "  single cores return wrong values or crash – often WITHOUT any machine check in the kernel log.\n\n"
             "  Duration: %s. Don't use the PC meanwhile (other load lowers the turbo clock).\n"
             "  %s\n\n"
             "  Results: %s\n\n  Enter = start, Ctrl+C = cancel "),
           schnell ? T("ca. 10–15 Minuten", "about 10-15 minutes") : T("ca. 1 bis 1,5 Stunden", "about 1 to 1.5 hours"),
           turboVorher ? T("Der Turbo ist bereits an, es wird nichts umgestellt. Es wird nichts installiert.", "Turbo is already on, nothing gets changed. Nothing gets installed.")
                       : T("Der Turbo wird für den Test eingeschaltet und danach wieder ausgeschaltet. Es wird nichts installiert.",
                           "Turbo is switched on for the test and switched off again afterwards. Nothing gets installed."),
           g_aus.c_str());
    fflush(stdout); if (getchar() == EOF) return 1;
  }
  { const char* su = getenv("SUDO_UID"); const char* sg = getenv("SUDO_GID");   // Ergebnisordner gehört dem aufrufenden Benutzer
    if (root && su && sg) { g_uid = (uid_t)atoi(su); g_gid = (gid_t)atoi(sg); g_chown = g_uid != 0; } }
  mkdir(g_aus.c_str(), 0755); string fehlerDir = g_aus + "/errors"; mkdir(fehlerDir.c_str(), 0755);
  g_log = fopen((g_aus + "/log.txt").c_str(), "ab"); g_csv = fopen((g_aus + "/results.csv").c_str(), "ab");
  if (!g_log || !g_csv) { printf(T("Ausgabeordner nicht beschreibbar: %s\n", "Output folder not writable: %s\n"), g_aus.c_str()); return 2; }
  fputs("time;cpu;coretype;core;apic;load;run;seconds;mhz_avg;result;exitcode;detail\n", g_csv); fflush(g_csv);
  { struct sigaction sa {}; sa.sa_handler = Abbruch; for (int s : { SIGINT, SIGTERM, SIGHUP }) sigaction(s, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN); }                              // Ausgabe in eine geschlossene Pipe darf den Test nicht ohne Zurückstellen beenden
  if (string si = Pfad("systemd-inhibit"); !si.empty())     // Bereitschaft/Ruhezustand während des Tests verhindern
    g_inhibit = Kind({ si, "--what=sleep:idle", "--who=coretest", "--why=CPU core test running", "--mode=block", "sleep", "infinity" }, -1, -1);

  // Topologie: P-/E-Kerne aus /sys/devices/cpu_core|cpu_atom, Kernnummer je Klasse
  vector<int> online = CpuListe(Lies("/sys/devices/system/cpu/online"));
  if (online.empty()) for (int c = 0; c < (int)sysconf(_SC_NPROCESSORS_ONLN); c++) online.push_back(c);
  int ncpu = online.empty() ? 1 : *std::max_element(online.begin(), online.end()) + 1;
  vector<int> klasse(ncpu, -1), kern(ncpu, -1);
  vector<int> pk = CpuListe(Lies("/sys/devices/cpu_core/cpus")), ek = CpuListe(Lies("/sys/devices/cpu_atom/cpus"));
  bool hybrid = !pk.empty() && !ek.empty();                 // Raptor Lake ohne E-Kerne (z. B. i3-13100/14100): alle wie P-Kerne testen
  { std::map<std::tuple<int, int, int>, int> nr; int zaehler[2] = { 0, 0 };
    for (int c : online) {
      klasse[c] = hybrid ? (std::find(pk.begin(), pk.end(), c) != pk.end() ? 1 : 0) : 1;
      int paket = atoi(Lies(Fmt("/sys/devices/system/cpu/cpu%d/topology/physical_package_id", c)).c_str());
      string ci = Lies(Fmt("/sys/devices/system/cpu/cpu%d/topology/core_id", c)); int core = ci.empty() ? c : atoi(ci.c_str());
      auto key = std::make_tuple(klasse[c], paket, core); auto it = nr.find(key);
      kern[c] = it != nr.end() ? it->second : (nr[key] = zaehler[klasse[c]]++);
    } }
  if (bis < 0) bis = ncpu - 1;
  int eRef = online.back(); if (hybrid) for (int i = (int)online.size() - 1; i >= 0; i--) if (klasse[online[i]] == 0) { eRef = online[i]; break; }
  { cpu_set_t s; CPU_ZERO(&s); CPU_SET(eRef, &s); sched_setaffinity(0, sizeof s, &s); }   // Steuerung selbst auf einem E-Kern

  char self[4096]; ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1); self[sl > 0 ? sl : 0] = 0;
  string exe = self, ordner = exe.substr(0, exe.find_last_of('/'));
  string pyskript = Gibt(ordner + "/coretest_load.py") ? ordner + "/coretest_load.py" : ordner + "/pure_win.py";
  string pyexe = Pfad("python3"); if (pyexe.empty() && access("/usr/libexec/platform-python", X_OK) == 0) pyexe = "/usr/libexec/platform-python";   // RHEL 8 minimal
  bool mitPy = py > 0 && Gibt(pyskript) && !pyexe.empty();
  if (py > 0 && !mitPy) Log(pyexe.empty() ? T("WARNUNG: python3 nicht gefunden – der empfindlichste Test entfällt (Paket python3 installieren)",
                                              "WARNING: python3 not found – the most sensitive test is skipped (install the python3 package)")
                                          : T("WARNUNG: coretest_load.py fehlt neben coretest", "WARNING: coretest_load.py missing next to coretest"));
  Log(Fmt(T("Kerntest: %d logische Prozessoren, getestet %d–%d, %g s je Lauf, py %d×, nat %d× (E-Kerne %d Runde(n)), Referenz auf CPU %d",
            "coretest: %d logical processors, testing %d-%d, %g s per run, py %dx, nat %dx (E-cores %d round(s)), reference on CPU %d"),
          (int)online.size(), von, bis, sek, mitPy ? py : 0, nat, eRunden, eRef));

  // Turbo an
  if (g_turboPfad.empty()) Log(T("Keine Turbo-Steuerung gefunden (weder intel_pstate noch cpufreq/boost) – Turbo bleibt, wie er ist",
                                 "No turbo control found (neither intel_pstate nor cpufreq/boost) – turbo stays as it is"));
  else if (turboVorher) Log(Fmt(T("Turbo bereits an (%s = %s)", "Turbo already on (%s = %s)"), g_turboPfad.c_str(), g_turboAn.c_str()));
  else {
    snprintf(s_pfad, sizeof s_pfad, "%s", g_turboPfad.c_str()); snprintf(s_alt, sizeof s_alt, "%s", Lies(g_turboPfad).c_str());
    if (FILE* f = fopen(MERK, "w")) { fprintf(f, "%s %s\n", s_pfad, s_alt); fclose(f); }
    g_turboGesetzt = 1;
    if (Schreib(g_turboPfad, g_turboAn) && TurboAn()) g_turboWirkt = true, Log(Fmt(T("Turbo an (%s = %s)", "Turbo on (%s = %s)"), s_pfad, g_turboAn.c_str()));
    else Log(Fmt(T("WARNUNG: Turbo ließ sich nicht einschalten (%s) – im BIOS deaktiviert?", "WARNING: could not switch turbo on (%s) – disabled in the BIOS?"), s_pfad));
  }
  sleep(3);

  // Referenzwerte auf dem E-Kern, zweimal, müssen gleich sein
  string gold = g_aus + "/golden.bin"; vector<Gold> g[2];
  for (int k = 0; k < 2; k++) {
    Lauf l = Starte({ exe, "--worker", "-", "0" }, eRef, 600);
    g[k].resize(SEEDS); int gef = 0; size_t p = 0;
    while ((p = l.out.find("gold ", p)) != string::npos) { int s; unsigned long long a, b2, c; if (sscanf(l.out.c_str() + p, "gold %d %llx %llx %llx", &s, &a, &b2, &c) == 4 && s >= 0 && s < SEEDS) { g[k][s] = { a, b2, c }; gef++; } p += 5; }
    if (gef != SEEDS) { Log(T("Referenz fehlgeschlagen: ", "Reference failed: ") + l.out); Aufraeumen(); return 2; }
  }
  if (memcmp(g[0].data(), g[1].data(), sizeof(Gold) * SEEDS)) { Log(T("Referenz zweimal verschieden – Referenzkern selbst unzuverlässig, Abbruch", "Reference differs between two runs – reference core itself unreliable, aborting")); Aufraeumen(); return 2; }
  { FILE* f = fopen(gold.c_str(), "wb"); if (!f || fwrite(g[0].data(), sizeof(Gold), SEEDS, f) != SEEDS) { Log("golden.bin?"); Aufraeumen(); return 2; } fclose(f); }
  string pyRef;
  if (mitPy) {
    Lauf l = Starte({ pyexe, "-X", "faulthandler", pyskript, "15" }, eRef, 15);
    string z = Zeile(l.out, "ok "); if (z.empty()) { Log(T("Python-Referenz fehlgeschlagen, nur C++-Last: ", "Python reference failed, C++ load only: ") + l.out); mitPy = false; }
    else { pyRef = z.substr(z.find_last_of(' ') + 1); Log(T("Python-Referenz: ", "Python reference: ") + pyRef); }
  }
  Log(T("Referenzen ok", "References ok"));

  struct Sum { int laeufe = 0, fehl = 0; unsigned apic = 0; double mhz = 0; };
  vector<Sum> sum(ncpu);
  for (int cpu = von; cpu <= bis && cpu < ncpu; cpu++) {
    if (cpu == eRef || klasse[cpu] < 0) continue;           // Referenzkern und abgeschaltete Prozessoren überspringen
    if (root && !g_turboPfad.empty() && g_turboWirkt && !TurboAn()) {   // z. B. power-profiles-daemon „Energiesparen“
      Schreib(g_turboPfad, g_turboAn);
      Log(T("Turbo wurde von einem anderen Programm ausgeschaltet (Energieprofil?) – wieder eingeschaltet", "Turbo was switched off by another program (power profile?) – switched on again"));
    }
    { Lauf a = Starte({ exe, "--apic" }, cpu, 5); string az = Zeile(a.out, "apic "); if (!az.empty()) sum[cpu].apic = (unsigned)atoi(az.c_str() + 5); }
    bool pkern = klasse[cpu] > 0; int np = pkern ? py : (mitPy ? std::min(py, eRunden) : 0), nn = pkern ? nat : std::min(nat, eRunden);
    for (int art : { 1, 0 }) for (int r = 0; r < (art == 0 ? (mitPy ? np : 0) : nn); r++) {   // erst C++ (liefert die APIC-ID), dann Python
      double ls = pkern ? sek : sek / 2;                    // E-Kerne: nur zum Vergleich, halbe Laufzeit
      Lauf l = art == 0 ? Starte({ pyexe, "-X", "faulthandler", pyskript, Fmt("%g", ls) }, cpu, ls)
                        : Starte({ exe, "--worker", gold, Fmt("%g", ls) }, cpu, ls);
      unsigned apic = sum[cpu].apic; string az = Zeile(l.out, "apic "); if (!az.empty()) apic = (unsigned)atoi(az.c_str() + 5);
      string erg, detail;
      if (art == 0) {
        string ok = Zeile(l.out, "ok "), mm = Zeile(l.out, "MISMATCH");
        if (l.timeout) erg = "HANG";
        else if (!ok.empty() && l.code == 0 && !l.sig) { string ref = ok.substr(ok.find_last_of(' ') + 1); erg = ref == pyRef ? "ok" : "WRONG"; if (erg != "ok") detail = "result " + ref + " instead of " + pyRef; }
        else if (!mm.empty()) { erg = "WRONG"; detail = mm; }
        else { erg = l.sig ? "CRASH" : "ERROR"; string t = l.out; std::replace(t.begin(), t.end(), '\n', ' '); detail = t.substr(t.size() > 300 ? t.size() - 300 : 0); }
      } else {
        string z = Zeile(l.out, "ok "); string f = Zeile(l.out, "WRONG"); string c = Zeile(l.out, "CRASH ");
        if (l.timeout) erg = "HANG"; else if (!c.empty()) { erg = "CRASH"; detail = c; } else if (!f.empty()) { erg = "WRONG"; detail = f + " | " + Zeile(l.out, "MISMATCH"); }
        else if (!z.empty() && l.code == 0 && !l.sig) { erg = "ok"; detail = z; } else { erg = l.sig ? "CRASH" : "ERROR"; detail = l.out.substr(0, 200); std::replace(detail.begin(), detail.end(), '\n', ' '); }
      }
      std::replace(detail.begin(), detail.end(), ';', ',');
      tm st = Jetzt(); const char* kt = pkern ? "P" : "E";
      fprintf(g_csv, "%04d-%02d-%02d %02d:%02d:%02d;%d;%s;%d;%u;%s;%d;%g;%.0f;%s;%s;%s\n", st.tm_year + 1900, st.tm_mon + 1, st.tm_mday, st.tm_hour, st.tm_min, st.tm_sec,
              cpu, kt, kern[cpu], apic, art ? "nat" : "py", r + 1, ls, l.mhz, erg.c_str(), ExitText(l).c_str(), detail.c_str()); fflush(g_csv);
      if (erg != "ok") {
        FILE* f = fopen((fehlerDir + Fmt("/cpu%02d-apic%u-%s-%d-%02d%02d%02d.txt", cpu, apic, art ? "nat" : "py", r + 1, st.tm_hour, st.tm_min, st.tm_sec)).c_str(), "wb");
        if (f) { fprintf(f, "CPU %d (%s-core %d), APIC %u, load %s, exit %s, average clock %.0f MHz\n\n%s", cpu, kt, kern[cpu], apic, art ? "nat" : "py", ExitText(l).c_str(), l.mhz, l.out.c_str()); fclose(f); }
      }
      sum[cpu].laeufe++; if (erg != "ok") sum[cpu].fehl++; if (apic) sum[cpu].apic = apic; sum[cpu].mhz = std::max(sum[cpu].mhz, l.mhz);
      Log(Fmt(T("CPU %2d %s-Kern %d APIC %3u  %s %d  %5.0f MHz  %s %s", "CPU %2d %s-core %d APIC %3u  %s %d  %5.0f MHz  %s %s"), cpu, kt, kern[cpu], apic, art ? "nat" : "py ", r + 1, l.mhz, erg.c_str(), detail.substr(0, 120).c_str()));
    }
  }
  Aufraeumen();
  // ---------------- Bericht
  unsigned nennMhz = 0; { unsigned a = 0, b = 0, c = 0, d = 0; if (__get_cpuid_count(0x16, 0, &a, &b, &c, &d)) nennMhz = a & 0xFFFF; }
  if (!nennMhz) nennMhz = (unsigned)(atof(Lies("/sys/devices/system/cpu/cpu0/cpufreq/base_frequency").c_str()) / 1000);
  string board = Lies("/sys/class/dmi/id/board_vendor") + " " + Lies("/sys/class/dmi/id/board_name"), bios = Lies("/sys/class/dmi/id/bios_version");
  string os = "?"; { string r = Lies("/etc/os-release"); size_t p = r.find("PRETTY_NAME="); if (p != string::npos) { size_t e = r.find('\n', p); os = r.substr(p + 12, e == string::npos ? string::npos : e - p - 12); os.erase(std::remove(os.begin(), os.end(), '"'), os.end()); } }
  { utsname u; if (!uname(&u)) os += string(", Kernel ") + u.release; }
  string treiber = Lies("/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver") + " / " + Lies("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  string pyVer = "–"; if (mitPy) { Lauf v = Starte({ pyexe, "-V" }, eRef, 5); pyVer = v.out; while (!pyVer.empty() && isspace((unsigned char)pyVer.back())) pyVer.pop_back(); }
  double maxP = 0; int fehlerKerne = 0, laeufe = 0, fehlLaeufe = 0;
  for (int c = von; c <= bis && c < ncpu; c++) { if (klasse[c] > 0) maxP = std::max(maxP, sum[c].mhz); if (sum[c].fehl) fehlerKerne++; laeufe += sum[c].laeufe; fehlLaeufe += sum[c].fehl; }
  bool turboAktiv = nennMhz ? maxP > nennMhz * 1.15 : true;
  string t, kernW = T("Kern", "core");
  t += T("CORETEST – Bericht (Linux)\n==========================\n\n", "CORETEST – report (Linux)\n=========================\n\n");
  { tm st = Jetzt(); t += g_de ? Fmt("Datum:      %02d.%02d.%04d %02d:%02d\n", st.tm_mday, st.tm_mon + 1, st.tm_year + 1900, st.tm_hour, st.tm_min)
                               : Fmt("Date:       %04d-%02d-%02d %02d:%02d\n", st.tm_year + 1900, st.tm_mon + 1, st.tm_mday, st.tm_hour, st.tm_min); }
  string nenn = nennMhz ? Fmt("%u", nennMhz) : "?";
  t += "CPU:        " + cpuName + Fmt(T(" (Nenntakt %s MHz, %d logische Prozessoren%s)\n", " (base clock %s MHz, %d logical processors%s)\n"), nenn.c_str(), (int)online.size(), hybrid ? T(", P- und E-Kerne", ", P- and E-cores") : "");
  t += "Microcode:  " + CpuinfoFeld("microcode") + "\nMainboard:  " + board + ", BIOS " + bios + "\nLinux:      " + os + "\ncpufreq:    " + treiber + "\nPython:     " + pyVer
     + (mitPy ? "" : T(" (FEHLT – der aussagekräftigste Test lief nicht!)", " (MISSING – the most sensitive test did not run!)")) + "\n";
  t += Fmt(T("Test:       %s, %g s je Lauf, Python %d×, C++ %d× je P-Kern; %d Läufe\n\n", "Test:       %s, %g s per run, Python %dx, C++ %dx per P-core; %d runs\n\n"),
           schnell ? T("schnell", "quick") : "Standard", sek, mitPy ? py : 0, nat, laeufe);
  if (fehlerKerne) {
    t += Fmt(T("ERGEBNIS: FEHLER GEFUNDEN – %d Kern(e) liefern unter Turbo falsche Ergebnisse oder stürzen ab (%d von %d Läufen).\n\n",
               "RESULT: ERRORS FOUND – %d core(s) returned wrong results or crashed under turbo (%d of %d runs).\n\n"), fehlerKerne, fehlLaeufe, laeufe);
    t += T("Betroffen:\n", "Affected:\n");
    for (int c = von; c <= bis && c < ncpu; c++) if (sum[c].fehl)
      t += Fmt(T("  CPU %2d  (%s-Kern %d, APIC-ID %u)  %d von %d Läufen fehlerhaft, Takt bis %.0f MHz\n", "  CPU %2d  (%s-core %d, APIC ID %u)  %d of %d runs failed, clock up to %.0f MHz\n"),
               c, klasse[c] > 0 ? "P" : "E", kern[c], sum[c].apic, sum[c].fehl, sum[c].laeufe, sum[c].mhz);
    t += T("\nDas heißt: Die CPU ist bei hohem Turbo-Takt instabil (bei Intel 13./14. Gen typisch für die bekannte\n"
           "Degradation, „Vmin Shift“). Solche Fehler erscheinen meist NICHT als Machine Check (MCE) im Kernel-Log,\n"
           "können aber Programme abstürzen lassen und Daten still verfälschen (Dateien, Backups, Exporte).\n\n"
           "Was tun:\n  1. Wichtige Daten sichern und nichts Wichtiges mit Turbo rechnen lassen.\n"
           "  2. Gegenprobe: Turbo aus (echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo, oder im BIOS)\n"
           "     und Test wiederholen – verschwinden die Fehler, ist das ein klarer Hinweis auf die CPU.\n"
           "  3. BIOS aktualisieren (Microcode 0x12B oder neuer), „Intel Default Settings“ verwenden.\n"
           "  4. Bei Intel eine Garantie-Anfrage (RMA) stellen – 13./14. Gen haben 5 Jahre erweiterte Garantie.\n"
           "     Diesen Ordner (bericht.txt, results.csv, errors/) als Beleg beilegen.\n\n",
           "\nThis means: the CPU is unstable at high turbo clocks (on Intel 13th/14th gen typical for the known\n"
           "degradation, \"Vmin shift\"). Such errors usually do NOT show up as machine checks (MCE) in the kernel log,\n"
           "but they can crash programs and silently corrupt data (files, backups, exports).\n\n"
           "What to do:\n  1. Back up important data and don't run important work with turbo enabled.\n"
           "  2. Cross-check: disable turbo (echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo, or in the BIOS)\n"
           "     and run the test again. If the errors disappear, that clearly points at the CPU.\n"
           "  3. Update the BIOS (microcode 0x12B or newer) and use \"Intel Default Settings\".\n"
           "  4. Contact Intel for a warranty replacement (RMA) – 13th/14th gen have an extended 5 year warranty.\n"
           "     Attach this folder (report.txt, results.csv, errors/) as evidence.\n\n");
  } else {
    t += Fmt(T("ERGEBNIS: keine Fehler gefunden (%d Läufe).\n\n", "RESULT: no errors found (%d runs).\n\n"), laeufe);
    t += T("Das ist ein gutes Zeichen, aber kein Beweis für eine gesunde CPU: Fehler können selten sein.\n"
           "Bei weiter ungeklärten Abstürzen: Test länger laufen lassen (z. B. --sek 120 --py 5) und zusätzlich\n"
           "einen anerkannten Einzelkern-Test wie CoreCycler (Prime95/y-cruncher, unter Windows) verwenden.\n\n",
           "That's a good sign, but no proof of a healthy CPU: errors can be rare.\n"
           "If unexplained crashes continue: run longer (e.g. --sek 120 --py 5) and additionally use an established\n"
           "per-core test such as CoreCycler (Prime95/y-cruncher, on Windows).\n\n");
  }
  if (!turboAktiv) t += Fmt(T("ACHTUNG: Höchster gemessener Takt %.0f MHz – der Turbo war offenbar nicht aktiv (BIOS? Energieprofil „Energiesparen“?).\n"
                              "Der Test ist dann wenig aussagekräftig.\n\n",
                              "WARNING: highest measured clock %.0f MHz – turbo apparently was not active (BIOS? \"power saver\" profile?).\n"
                              "The test is then not very meaningful.\n\n"), maxP);
  t += T("Alle Kerne (Fehler / Läufe, höchster mittlerer Takt):\n", "All cores (failed / runs, highest average clock):\n");
  for (int c = von; c <= bis && c < ncpu; c++) if (sum[c].laeufe)
    t += Fmt("  CPU %2d  %s-%s %2d  APIC %3u  %5.0f MHz  %d/%d %s\n", c, klasse[c] > 0 ? "P" : "E", kernW.c_str(), kern[c], sum[c].apic, sum[c].mhz, sum[c].fehl, sum[c].laeufe, sum[c].fehl ? T("<<< FEHLER", "<<< FAILED") : "");
  t += T("\nErklärung: Jeder logische Prozessor wurde einzeln (Affinity) mit vollem Single-Core-Turbo belastet:\n"
         "„py“ = Python-Interpreter (empfindlichster Test: viele Speicherzugriffe und Sprünge), „nat“ = C++-Rechenlast.\n"
         "Ergebnisse wurden mit Referenzwerten verglichen, die vorher auf einem anderen Kern berechnet wurden.\n"
         "WRONG = falsches Ergebnis, CRASH = Absturz (Signal), ERROR = unmögliche Programmfehlermeldung, HANG = hängt.\n"
         "APIC-ID = Hardware-Nummer des Kerns. Die CPU-Nummern unter Linux sind anders als unter Windows –\n"
         "zum Vergleich mit einem Windows-Bericht die APIC-ID verwenden. Details je Fehler im Unterordner „errors“.\n",
         "\nExplanation: each logical processor was loaded on its own (affinity) at full single-core turbo:\n"
         "\"py\" = Python interpreter (most sensitive: many memory accesses and jumps), \"nat\" = C++ compute load.\n"
         "Results were compared against reference values computed beforehand on another core.\n"
         "WRONG = wrong result, CRASH = crash (signal), ERROR = impossible program error, HANG = hung.\n"
         "APIC ID = hardware number of the core. CPU numbers on Linux differ from Windows – to compare with a\n"
         "Windows report, use the APIC ID. Details per failure in the \"errors\" folder.\n");
  string berichtDat = g_aus + (g_de ? "/bericht.txt" : "/report.txt");
  { FILE* z = fopen(berichtDat.c_str(), "wb"); if (z) { fputs(t.c_str(), z); fclose(z); } }
  Log("\n" + t);
  fclose(g_log); g_log = nullptr; fclose(g_csv); g_csv = nullptr;
  if (g_chown) nftw(g_aus.c_str(), Besitz, 16, FTW_PHYS);
  printf(T("\n  Fertig. Bericht: %s\n", "\n  Done. Report: %s\n"), berichtDat.c_str());
  return 0;
}
