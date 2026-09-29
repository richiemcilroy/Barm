// The Benchmarks Game n-body: a double-precision simulation of the Jovian planets.
#include <math.h>
#include <stdio.h>

#define PI 3.141592653589793
#define SOLAR_MASS (4 * PI * PI)
#define DAYS_PER_YEAR 365.24
#define NBODIES 5

typedef struct {
    double x, y, z, vx, vy, vz, mass;
} Body;

static Body body(double x, double y, double z, double vx, double vy, double vz, double mass) {
    return (Body){x, y, z, vx * DAYS_PER_YEAR, vy * DAYS_PER_YEAR, vz * DAYS_PER_YEAR, mass * SOLAR_MASS};
}

static void offset_momentum(Body *bodies, int n) {
    double px = 0.0, py = 0.0, pz = 0.0;
    for (int i = 0; i < n; i++) {
        px += bodies[i].vx * bodies[i].mass;
        py += bodies[i].vy * bodies[i].mass;
        pz += bodies[i].vz * bodies[i].mass;
    }
    bodies[0].vx = -px / SOLAR_MASS;
    bodies[0].vy = -py / SOLAR_MASS;
    bodies[0].vz = -pz / SOLAR_MASS;
}

static void advance(Body *bodies, int n, double dt) {
    for (int i = 0; i < n; i++) {
        Body *bi = &bodies[i];
        for (int j = i + 1; j < n; j++) {
            Body *bj = &bodies[j];
            double dx = bi->x - bj->x;
            double dy = bi->y - bj->y;
            double dz = bi->z - bj->z;
            double d2 = dx * dx + dy * dy + dz * dz;
            double mag = dt / (d2 * sqrt(d2));
            double mi = bi->mass * mag;
            double mj = bj->mass * mag;
            bi->vx -= dx * mj;
            bi->vy -= dy * mj;
            bi->vz -= dz * mj;
            bj->vx += dx * mi;
            bj->vy += dy * mi;
            bj->vz += dz * mi;
        }
    }
    for (int i = 0; i < n; i++) {
        Body *b = &bodies[i];
        b->x += dt * b->vx;
        b->y += dt * b->vy;
        b->z += dt * b->vz;
    }
}

static double energy(const Body *bodies, int n) {
    double e = 0.0;
    for (int i = 0; i < n; i++) {
        const Body *bi = &bodies[i];
        e += 0.5 * bi->mass * (bi->vx * bi->vx + bi->vy * bi->vy + bi->vz * bi->vz);
        for (int j = i + 1; j < n; j++) {
            const Body *bj = &bodies[j];
            double dx = bi->x - bj->x;
            double dy = bi->y - bj->y;
            double dz = bi->z - bj->z;
            e -= (bi->mass * bj->mass) / sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

int main(void) {
    const long steps = 10000000;
    Body bodies[NBODIES] = {
        // Sun
        body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0),
        // Jupiter
        body(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
             1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05,
             9.54791938424326609e-04),
        // Saturn
        body(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
             -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05,
             2.85885980666130812e-04),
        // Uranus
        body(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
             2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05,
             4.36624404335156298e-05),
        // Neptune
        body(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
             2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05,
             5.15138902046611451e-05),
    };
    offset_momentum(bodies, NBODIES);
    printf("%.9f\n", energy(bodies, NBODIES));
    for (long i = 0; i < steps; i++) {
        advance(bodies, NBODIES, 0.01);
    }
    printf("%.9f\n", energy(bodies, NBODIES));
    return 0;
}
