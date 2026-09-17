// FLAGS: -k reduce_sum -g_size 16 -l_size 4 --arg-buf 80 --arg-buf 16 --arg-int 4
/**
 * fancy_variables.cl
 *
 * Demonstrates rich, multi-domain variable inspection in ocldbg / VS Code:
 * - Thread & Work-Item Coordinates (gid, lid, group_id)
 * - Orbital Dynamics (altitude_km, orbital_velocity_kms, inclination_deg)
 * - Thermodynamics & Environment (temperature_c, pressure_kpa, solar_flux)
 * - Power & Avionics (battery_pct, bus_voltage, cpu_load_pct)
 * - Telemetry & Status Codes (status_flags, packet_sequence, error_count)
 *
 * Each work-item computes distinct physical states. Setting a breakpoint on
 * line 40 allows inspecting all 16 variables simultaneously in the VS Code
 * "Variables" section (Locals), and clicking between work-item threads in
 * the Call Stack reveals each thread's independent physical telemetry.
 */
__kernel void reduce_sum(__global const float *src,
                         __global float       *dst,
                         int                   n)
{

    // Work-Item & Grid Topology
    volatile int gid = get_global_id(0);
    volatile int lid = get_local_id(0);
    volatile int group_id = get_group_id(0);

    // Orbital Mechanics (unique per satellite work-item)
    volatile float altitude_km = 415.0f + 18.5f * (float)gid;
    volatile float orbital_velocity_kms = 7.67f - 0.02f * (float)lid;
    volatile float inclination_deg = 51.64f;
    volatile float eccentricity = 0.00045f * (float)(gid + 1);

    // Thermodynamics & Space Environment
    volatile float temperature_c = -20.5f + 3.2f * (float)lid;
    volatile float pressure_kpa = 101.325f;
    volatile float solar_flux_w_m2 = 1361.0f + 14.5f * (float)gid;

    // Avionics & Power Subsystems
    volatile float battery_pct = 98.5f - 0.75f * (float)gid;
    volatile float bus_voltage = 28.2f + 0.1f * (float)lid;
    volatile float cpu_load_pct = 42.0f + 2.5f * (float)gid;

    // Status Flags & Sensor Readings
    volatile int status_flags = 0xCAFE;
    volatile int packet_sequence = 1024 + gid;
    volatile int error_count = 0;

    volatile float acc = 0.0f;

    for (int step = 0; step < n; step++) {
        volatile float sample = src[gid * n + step];
        volatile float signal_snr_db = 28.4f + sample * 0.5f;

        // Dynamic updates over time
        battery_pct -= 0.05f;
        temperature_c += sample * 0.1f;
        acc += sample + signal_snr_db;

        /* <-- Set breakpoint here (line 53) to inspect all live variables! --> */
    }

    dst[gid] = acc + altitude_km + orbital_velocity_kms +
               temperature_c + solar_flux_w_m2 + battery_pct +
               src[gid * 0] * 0.0001f + (float)n * 0.001f +
               (float)(gid + lid + group_id + status_flags + packet_sequence);
}
