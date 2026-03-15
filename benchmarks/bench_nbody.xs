-- n-body: 5-body solar system sim, 1000 steps.
-- Standard "benchmarks game" workload; measures floating-point loop speed.

import math

let dt = 0.01
let steps = 1000
let pi = math.pi
let solar_mass = 4.0 * pi * pi
let days_per_year = 365.24

fn mkbody(x, y, z, vx, vy, vz, m) {
    return #{
        x: x, y: y, z: z,
        vx: vx * days_per_year, vy: vy * days_per_year, vz: vz * days_per_year,
        mass: m * solar_mass,
    }
}

var bodies = [
    mkbody(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0),
    mkbody(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
           1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05,
           9.54791938424326609e-04),
    mkbody(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
           -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05,
           2.85885980666130812e-04),
    mkbody(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
           2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05,
           4.36624404335156298e-05),
    mkbody(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
           2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05,
           5.15138902046611451e-05),
]

-- offset momentum
var px = 0.0; var py = 0.0; var pz = 0.0
for b in bodies {
    px = px + b.vx * b.mass
    py = py + b.vy * b.mass
    pz = pz + b.vz * b.mass
}
bodies[0].vx = 0.0 - px / solar_mass
bodies[0].vy = 0.0 - py / solar_mass
bodies[0].vz = 0.0 - pz / solar_mass

fn energy(bs) {
    var e = 0.0
    for i in 0..bs.len() {
        let b = bs[i]
        e = e + 0.5 * b.mass * (b.vx*b.vx + b.vy*b.vy + b.vz*b.vz)
        for j in (i+1)..bs.len() {
            let c = bs[j]
            let dx = b.x - c.x
            let dy = b.y - c.y
            let dz = b.z - c.z
            e = e - b.mass * c.mass / math.sqrt(dx*dx + dy*dy + dz*dz)
        }
    }
    return e
}

fn advance(bs, dt) {
    let n = bs.len()
    for i in 0..n {
        let bi = bs[i]
        for j in (i+1)..n {
            let bj = bs[j]
            let dx = bi.x - bj.x
            let dy = bi.y - bj.y
            let dz = bi.z - bj.z
            let d2 = dx*dx + dy*dy + dz*dz
            let mag = dt / (d2 * math.sqrt(d2))
            bi.vx = bi.vx - dx * bj.mass * mag
            bi.vy = bi.vy - dy * bj.mass * mag
            bi.vz = bi.vz - dz * bj.mass * mag
            bj.vx = bj.vx + dx * bi.mass * mag
            bj.vy = bj.vy + dy * bi.mass * mag
            bj.vz = bj.vz + dz * bi.mass * mag
        }
    }
    for b in bs {
        b.x = b.x + b.vx * dt
        b.y = b.y + b.vy * dt
        b.z = b.z + b.vz * dt
    }
}

let e0 = energy(bodies)
for _ in 0..steps { advance(bodies, dt) }
let e1 = energy(bodies)

println("nbody e0=", e0, "e1=", e1)
