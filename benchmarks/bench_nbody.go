package main

import (
	"fmt"
	"math"
)

const DT = 0.01
const STEPS = 1000
const DAYS = 365.24

var SOLAR_MASS = 4 * math.Pi * math.Pi

type Body struct{ x, y, z, vx, vy, vz, mass float64 }

func body(x, y, z, vx, vy, vz, m float64) *Body {
	return &Body{x, y, z, vx * DAYS, vy * DAYS, vz * DAYS, m * SOLAR_MASS}
}

func energy(bs []*Body) float64 {
	e := 0.0
	for i, b := range bs {
		e += 0.5 * b.mass * (b.vx*b.vx + b.vy*b.vy + b.vz*b.vz)
		for _, c := range bs[i+1:] {
			dx := b.x - c.x
			dy := b.y - c.y
			dz := b.z - c.z
			e -= b.mass * c.mass / math.Sqrt(dx*dx+dy*dy+dz*dz)
		}
	}
	return e
}

func advance(bs []*Body, dt float64) {
	n := len(bs)
	for i := 0; i < n; i++ {
		bi := bs[i]
		for j := i + 1; j < n; j++ {
			bj := bs[j]
			dx := bi.x - bj.x
			dy := bi.y - bj.y
			dz := bi.z - bj.z
			d2 := dx*dx + dy*dy + dz*dz
			mag := dt / (d2 * math.Sqrt(d2))
			bi.vx -= dx * bj.mass * mag
			bi.vy -= dy * bj.mass * mag
			bi.vz -= dz * bj.mass * mag
			bj.vx += dx * bi.mass * mag
			bj.vy += dy * bi.mass * mag
			bj.vz += dz * bi.mass * mag
		}
	}
	for _, b := range bs {
		b.x += b.vx * dt
		b.y += b.vy * dt
		b.z += b.vz * dt
	}
}

func main() {
	bodies := []*Body{
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
	}
	px, py, pz := 0.0, 0.0, 0.0
	for _, b := range bodies {
		px += b.vx * b.mass
		py += b.vy * b.mass
		pz += b.vz * b.mass
	}
	bodies[0].vx = -px / SOLAR_MASS
	bodies[0].vy = -py / SOLAR_MASS
	bodies[0].vz = -pz / SOLAR_MASS

	e0 := energy(bodies)
	for i := 0; i < STEPS; i++ {
		advance(bodies, DT)
	}
	e1 := energy(bodies)
	fmt.Println("nbody e0=", e0, "e1=", e1)
}
