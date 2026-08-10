#include <veloco/common.h>

#include <stdio.h>

int main(void)
{
    printf("veloco_bench_fiber placeholder\n");
    printf("version=%d.%d.%d\n", VL_VERSION_MAJOR, VL_VERSION_MINOR,
           VL_VERSION_PATCH);
    printf("note=Task 2 adds real fiber context-switch measurements\n");
    return 0;
}
