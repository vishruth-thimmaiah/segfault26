/** hello_kernel.cl — simplest possible kernel for Milestone 1 DWARF checks */
__kernel void vec_add(__global const float *a,
                      __global const float *b,
                      __global float       *c)
{
    int gx = get_global_id(0);
    c[gx] = a[gx] + b[gx];   /* breakpoint target for M1 */
}
