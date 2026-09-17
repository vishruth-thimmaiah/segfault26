// FLAGS: -k reduce_sum -g_size 16 -l_size 4 --arg-buf 80 --arg-buf 16 --arg-int 4
__kernel void reduce_sum(__global const float *src,
                         __global float       *dst,
                         int                   n)
{
    // Work-item identification
    int sat_id = 100 + get_global_id(0);
    int station_id = get_group_id(0);
    int channel = get_local_id(0);

    // Orbital parameters
    float altitude_km = 420.0f + 12.5f * (float)sat_id;
    float orbital_velocity_kms = 7.66f - 0.05f * (float)channel;
    float inclination_deg = 51.64f;
    float eccentricity = 0.0008f * (float)(channel + 1);

    // Environmental & sensor telemetry
    float solar_flux_w_m2 = 1361.0f + 5.2f * (float)channel;
    float panel_temp_c = -15.4f + 8.2f * (float)channel;
    float battery_soc_pct = 98.5f - 1.2f * (float)channel;
    float bus_voltage_v = 28.15f + 0.1f * (float)channel;

    // Attitude quaternions & dynamics
    float quat_w = 0.7071f;
    float quat_x = 0.0f;
    float quat_y = 0.0f;
    float quat_z = 0.7071f;
    float pitch_rate_dps = 0.025f * (float)(channel + 1);
    float yaw_rate_dps = -0.015f * (float)(channel + 1);

    // Status flags
    int telemetry_ok = 1;
    int payload_active = 0xFF;
    int comms_lock = 0xAA;

    float acc = 0.0f;

    for (int step = 0; step < n; step++) {
        float raw_sensor = src[get_global_id(0) * n + step];
        float filtered_signal = raw_sensor * 1.024f;
        float signal_to_noise_db = 28.5f + filtered_signal;

        battery_soc_pct -= 0.05f;
        panel_temp_c += filtered_signal * 0.1f;
        acc += filtered_signal + signal_to_noise_db;

        // Keep variables live by referencing them
        acc += altitude_km * 0.00001f + orbital_velocity_kms * 0.001f +
               solar_flux_w_m2 * 0.0001f + battery_soc_pct * 0.001f +
               bus_voltage_v * 0.01f + pitch_rate_dps + yaw_rate_dps +
               (float)(sat_id + station_id + channel + telemetry_ok + payload_active + comms_lock);

        /* <-- BREAKPOINT HERE (line 46) --> */
    }

    dst[get_global_id(0)] = acc;
}
