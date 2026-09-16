// FLAGS: -k reduce_sum -g_size 16 -l_size 4 --arg-buf 80 --arg-buf 16 --arg-int 4
/**
 * fancy_physics.cl — Particle dynamics & thermodynamics kernel
 *
 * Designed for rich per-work-item variable inspection in ocldbg:
 * - Topology: gx, lx, group_id
 * - Dynamics: mass, pos_x, velocity_x, force_x, impulse
 * - Thermodynamics: temperature, pressure, kinetic_energy, potential_energy, total_energy
 * - Simulation state: step, sample, acc, damping_factor, status_flags
 */
__kernel void reduce_sum(__global const float *src,
                         __global float       *dst,
                         int                   n)
{
    // Work-item topology
    int gx = get_global_id(0);
    int lx = get_local_id(0);
    int group_id = get_group_id(0);

    // Particle physical properties (unique per work-item)
    float mass = 1.0f + 0.25f * (float)gx;
    float damping_factor = 0.985f;
    float pos_x = 10.0f + (float)gx;
    float velocity_x = 2.5f * (float)(gx + 1);
    float force_x = -9.81f * mass;

    // Thermodynamic & energy state
    float kinetic_energy = 0.5f * mass * velocity_x * velocity_x;
    float potential_energy = mass * 9.81f * pos_x;
    float total_energy = kinetic_energy + potential_energy;
    float temperature = 293.15f + 12.5f * (float)lx;    // Kelvin
    float pressure = 101.325f + 1.8f * (float)group_id;  // kPa

    float acc = 0.0f;
    int status_flags = 0x4A;

    // Time-integration loop
    for (int step = 0; step < n; step++) {
        float sample = src[gx * n + step];
        float impulse = sample * 0.15f;

        velocity_x = (velocity_x + (force_x / mass) * 0.01f + impulse) * damping_factor;
        pos_x += velocity_x * 0.01f;
        acc += sample * velocity_x;

        kinetic_energy = 0.5f * mass * velocity_x * velocity_x;
        total_energy = kinetic_energy + (mass * 9.81f * pos_x);
    }

    dst[gx] = total_energy;
}
