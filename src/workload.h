// Gemeinsame Testlast für coretest (Windows) und coretest_linux: muss auf beiden Systemen bitgleich rechnen.
// APIC-ID (CPUID 0xB), Filter, kleine Stack-Maschine, Zeigerkette; Block(seed) liefert die drei Prüfwerte.
#pragma once
#include <cpuid.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

static unsigned ApicId() { unsigned a = 0, b = 0, c = 0, d = 0; if (__get_cpuid_count(0xB, 0, &a, &b, &c, &d)) return d; __get_cpuid(1, &a, &b, &c, &d); return b >> 24; }

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
  static std::vector<uint32_t> nx(1 << 20);
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
