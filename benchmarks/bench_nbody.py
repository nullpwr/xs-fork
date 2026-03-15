import math

DT = 0.01
STEPS = 1000
PI = math.pi
SOLAR_MASS = 4.0 * PI * PI
DAYS_PER_YEAR = 365.24


def body(x, y, z, vx, vy, vz, m):
    return [x, y, z,
            vx * DAYS_PER_YEAR, vy * DAYS_PER_YEAR, vz * DAYS_PER_YEAR,
            m * SOLAR_MASS]


BODIES = [
    body(0, 0, 0, 0, 0, 0, 1.0),
    body(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
         1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05,
         9.54791938424326609e-04),
    body(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
         -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05,
         2.85885980666130812e-04),
    body(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
         2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05,
         4.36624404335156298e-05),
    body(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
         2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05,
         5.15138902046611451e-05),
]

px = py = pz = 0.0
for b in BODIES:
    px += b[3] * b[6]
    py += b[4] * b[6]
    pz += b[5] * b[6]
BODIES[0][3] = -px / SOLAR_MASS
BODIES[0][4] = -py / SOLAR_MASS
BODIES[0][5] = -pz / SOLAR_MASS


def energy(bs):
    e = 0.0
    for i, b in enumerate(bs):
        e += 0.5 * b[6] * (b[3]**2 + b[4]**2 + b[5]**2)
        for c in bs[i + 1:]:
            dx, dy, dz = b[0]-c[0], b[1]-c[1], b[2]-c[2]
            e -= b[6] * c[6] / math.sqrt(dx*dx + dy*dy + dz*dz)
    return e


def advance(bs, dt):
    n = len(bs)
    for i in range(n):
        bi = bs[i]
        for j in range(i + 1, n):
            bj = bs[j]
            dx = bi[0] - bj[0]; dy = bi[1] - bj[1]; dz = bi[2] - bj[2]
            d2 = dx*dx + dy*dy + dz*dz
            mag = dt / (d2 * math.sqrt(d2))
            bi[3] -= dx * bj[6] * mag
            bi[4] -= dy * bj[6] * mag
            bi[5] -= dz * bj[6] * mag
            bj[3] += dx * bi[6] * mag
            bj[4] += dy * bi[6] * mag
            bj[5] += dz * bi[6] * mag
    for b in bs:
        b[0] += b[3] * dt; b[1] += b[4] * dt; b[2] += b[5] * dt


e0 = energy(BODIES)
for _ in range(STEPS):
    advance(BODIES, DT)
e1 = energy(BODIES)
print(f"nbody e0={e0} e1={e1}")
