"use strict";
const DT = 0.01, STEPS = 1000, PI = Math.PI;
const SOLAR_MASS = 4 * PI * PI, DAYS = 365.24;

function body(x, y, z, vx, vy, vz, m) {
  return [x, y, z, vx * DAYS, vy * DAYS, vz * DAYS, m * SOLAR_MASS];
}

const BODIES = [
  body(0, 0, 0, 0, 0, 0, 1.0),
  body(4.84143144246472090, -1.16032004402742839, -0.103622044471123109,
       0.00166007664274403694, 0.00769901118419740425, -0.0000690460016972063023,
       0.000954791938424326609),
  body(8.34336671824457987, 4.12479856412430479, -0.403523417114321381,
       -0.00276742510726862411, 0.00499852801234917238, 0.0000230417297573763929,
       0.000285885980666130812),
  body(12.8943695621391310, -15.1111514016986312, -0.223307578892655734,
       0.00296460137564761618, 0.00237847173959480950, -0.0000296589568540237556,
       0.0000436624404335156298),
  body(15.3796971148509165, -25.9193146099879641, 0.179258772950371181,
       0.00268067772490389322, 0.00162824170038242295, -0.0000951592254519715870,
       0.0000515138902046611451),
];

let px = 0, py = 0, pz = 0;
for (const b of BODIES) { px += b[3]*b[6]; py += b[4]*b[6]; pz += b[5]*b[6]; }
BODIES[0][3] = -px / SOLAR_MASS;
BODIES[0][4] = -py / SOLAR_MASS;
BODIES[0][5] = -pz / SOLAR_MASS;

function energy(bs) {
  let e = 0;
  for (let i = 0; i < bs.length; i++) {
    const b = bs[i];
    e += 0.5 * b[6] * (b[3]*b[3] + b[4]*b[4] + b[5]*b[5]);
    for (let j = i + 1; j < bs.length; j++) {
      const c = bs[j];
      const dx = b[0]-c[0], dy = b[1]-c[1], dz = b[2]-c[2];
      e -= b[6] * c[6] / Math.sqrt(dx*dx + dy*dy + dz*dz);
    }
  }
  return e;
}

function advance(bs, dt) {
  const n = bs.length;
  for (let i = 0; i < n; i++) {
    const bi = bs[i];
    for (let j = i + 1; j < n; j++) {
      const bj = bs[j];
      const dx = bi[0] - bj[0], dy = bi[1] - bj[1], dz = bi[2] - bj[2];
      const d2 = dx*dx + dy*dy + dz*dz;
      const mag = dt / (d2 * Math.sqrt(d2));
      bi[3] -= dx * bj[6] * mag; bi[4] -= dy * bj[6] * mag; bi[5] -= dz * bj[6] * mag;
      bj[3] += dx * bi[6] * mag; bj[4] += dy * bi[6] * mag; bj[5] += dz * bi[6] * mag;
    }
  }
  for (const b of bs) { b[0] += b[3]*dt; b[1] += b[4]*dt; b[2] += b[5]*dt; }
}

const e0 = energy(BODIES);
for (let i = 0; i < STEPS; i++) advance(BODIES, DT);
const e1 = energy(BODIES);
console.log("nbody e0=", e0, "e1=", e1);
