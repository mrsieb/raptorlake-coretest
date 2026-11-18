# Python load for coretest.exe: a simple filter loop, every result must be identical.
# On degraded cores this leads to crashes or impossible error messages under turbo.
import sys, time
def chunk(n):
    b = [0.0] * 1557; i0 = 0; f0 = f1 = f2 = f3 = 0.0; damp = 0.25; acc = 0.0
    for i in range(n):
        x = 1.0 if i % 1000 == 0 else 0.0
        o0 = b[i0]; o1 = o0 * 0.5; o2 = o0 * 0.25; o3 = -o0
        f0 += (1 - damp) * (o0 - f0)
        f1 += (1 - damp) * (o1 - f1)
        f2 += (1 - damp) * (o2 - f2)
        f3 += (1 - damp) * (o3 - f3)
        b[i0] = x + (f0 + f1 - f2 - f3) * 0.4
        i0 += 1
        if i0 == 1557:
            i0 = 0
        acc += f0 * 1e-3
    return repr(acc)
t0 = time.time(); ref = None; k = 0
while time.time() - t0 < float(sys.argv[1]):
    r = chunk(1500000); k += 1
    if ref is None: ref = r
    elif r != ref: print('MISMATCH', k, r, ref); sys.exit(3)
print('ok', k, ref)
