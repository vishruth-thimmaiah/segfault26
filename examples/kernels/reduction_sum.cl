// FLAGS: -k reduce_sum -g_size 16 -l_size 4 --arg-buf 80 --arg-buf 16 --arg-int 4
/**
 * reduction_bug.cl  —  primary validation kernel for segfault26
 *
 * Performs a parallel reduction (sum) over an input array.
 * Contains an intentional off-by-one bug: the loop bound uses '<='
 * instead of '<', causing one extra iteration that reads out-of-bounds.
 *
 * Used in Milestone 2 deliverable: set a breakpoint at the marked line,
 * halt, inspect `acc` and `i` for work-item (3,0,0).
 */
__kernel void reduce_sum(__global const float *src,
                         __global float       *dst,
                         int                   n)
{
    int gx = get_global_id(0);

    float acc = 0.0f;
    for (int i = 0; i <= n; i++) {
        acc += src[gx * n + i];
    }

    dst[gx] = acc;
}
