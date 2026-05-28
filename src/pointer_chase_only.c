#define _XOPEN_SOURCE 600

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define POINTER_CHASE_CACHE_LINE 128
#define POINTER_CHASE_DEFAULT_BYTES (100ULL * 1024ULL * 1024ULL)

struct pointer_chase_line
{
    uint64_t next_offset;
    uint8_t pad[POINTER_CHASE_CACHE_LINE - sizeof(uint64_t)];
};

typedef struct
{
    long long chase_array_elems;
    uint64_t chase_total_loads;
    int run_iterations;
    const char *walk_file_path;
    int m5_enabled;
    int debug_enabled;
    long long periodic_stats_ticks;
} cli_options;

static long long debug_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((long long)tv.tv_sec * 1000LL) + ((long long)tv.tv_usec / 1000LL);
}

static void debug_log_json(const char *message)
{
    fprintf(stdout, "{\"message\":\"%s\",\"timestamp\":%lld}\n", message, debug_now_ms());
    fflush(stdout);
}

static uint64_t now_cycles(void)
{
#if defined(__aarch64__)
    uint64_t cyc = 0;
    __asm__ __volatile__(
        "isb\n\t"
        "mrs %0, cntvct_el0\n\t"
        : "=r"(cyc));
    return cyc;
#else
    return 0;
#endif
}

static uint64_t cycles_per_second(void)
{
#if defined(__aarch64__)
    uint64_t freq = 0;
    __asm__ __volatile__(
        "mrs %0, cntfrq_el0\n\t"
        : "=r"(freq));
    return freq;
#else
    return 0;
#endif
}

static void m5_dump_reset_stats(uint64_t delay, uint64_t period)
{
#if defined(__aarch64__)
    register uint64_t x0 __asm__("x0") = delay;
    register uint64_t x1 __asm__("x1") = period;
    __asm__ __volatile__(
        ".inst 0xFF420110\n\t"
        : "+r"(x0), "+r"(x1));
#else
    (void)delay;
    (void)period;
#endif
}

static void m5_exit(uint64_t delay)
{
#if defined(__aarch64__)
    register uint64_t x0 __asm__("x0") = delay;
    __asm__ __volatile__(
        ".inst 0xFF210110\n\t"
        : "+r"(x0));
#else
    (void)delay;
#endif
}

static void m5_dump_stats(uint64_t delay, uint64_t period)
{
#if defined(__aarch64__)
    register uint64_t x0 __asm__("x0") = delay;
    register uint64_t x1 __asm__("x1") = period;
    __asm__ __volatile__(
        ".inst 0xFF410110\n\t"
        : "+r"(x0), "+r"(x1));
#else
    (void)delay;
    (void)period;
#endif
}

static void print_usage(const char *prog_name)
{
    printf("Usage: %s [-c <chase_elems>] [-x <chase_total_loads>] [-n <iterations>] ", prog_name);
    printf("[-w <walk_file>] [-P <period_ticks>] [-m] [-d] [-h]\n");
    printf("Options:\n");
    printf("  -c <chase_elems>        Pointer-chase nodes (cache-line nodes, > 0)\n");
    printf("  -x <chase_total_loads>  Dependent loads per kernel call (> 0)\n");
    printf("  -n <iterations>         Number of kernel calls (> 0)\n");
    printf("  -w <walk_file>          Pointer-walk file path to load/save\n");
    printf("  -P <period_ticks>       Period for m5_dump_stats when -m is enabled (>= 0)\n");
    printf("  -m                      Enable gem5 m5_* calls (m5_exit, m5_dump_stats, ...)\n");
    printf("  -d                      Enable debug logging\n");
    printf("  -h                      Show this help message\n");
}

static void shuffle_u64(uint64_t *array, uint64_t n)
{
    uint64_t i;

    srand(0);
    if (n <= 1)
        return;

    for (i = 0; i < n - 1; i++)
    {
        uint64_t j = i + (uint64_t)(rand() / (RAND_MAX / (n - i) + 1));
        uint64_t tmp = array[j];
        array[j] = array[i];
        array[i] = tmp;
    }
}

static void generate_pointer_walk(struct pointer_chase_line *walk_array, uint64_t elems)
{
    uint64_t *perm;
    uint64_t j;

    if (elems == 0)
        return;

    if (elems == 1)
    {
        walk_array[0].next_offset = 0;
        return;
    }

    perm = (uint64_t *)malloc(elems * sizeof(uint64_t));
    if (perm == NULL)
    {
        fprintf(stderr, "WARNING: permutation allocation failed; using sequential ring.\n");
        for (j = 0; j < elems; j++)
            walk_array[j].next_offset = ((j + 1) % elems) * POINTER_CHASE_CACHE_LINE;
        return;
    }

    for (j = 0; j < elems; j++)
        perm[j] = j;

    shuffle_u64(perm, elems);

    for (j = 0; j < elems; j++)
    {
        uint64_t cur = perm[j];
        uint64_t next = perm[(j + 1) % elems];
        walk_array[cur].next_offset = next * POINTER_CHASE_CACHE_LINE;
    }

    free(perm);
}

static int validate_pointer_walk(const struct pointer_chase_line *walk_array, uint64_t elems)
{
    uint8_t *in_degree = NULL;
    uint8_t *visited = NULL;
    uint64_t i;
    uint64_t node = 0;
    int ok = 0;

    if (walk_array == NULL || elems == 0)
        return 0;

    in_degree = (uint8_t *)calloc(elems, sizeof(uint8_t));
    visited = (uint8_t *)calloc(elems, sizeof(uint8_t));
    if (in_degree == NULL || visited == NULL)
        goto cleanup;

    for (i = 0; i < elems; i++)
    {
        uint64_t next_offset = walk_array[i].next_offset;
        uint64_t next_idx;

        if ((next_offset % POINTER_CHASE_CACHE_LINE) != 0)
            goto cleanup;
        next_idx = next_offset / POINTER_CHASE_CACHE_LINE;
        if (next_idx >= elems)
            goto cleanup;
        in_degree[next_idx]++;
        if (in_degree[next_idx] > 1)
            goto cleanup;
    }

    for (i = 0; i < elems; i++)
    {
        if (visited[node] != 0)
            goto cleanup;
        visited[node] = 1;
        node = walk_array[node].next_offset / POINTER_CHASE_CACHE_LINE;
    }

    if (node != 0)
        goto cleanup;

    for (i = 0; i < elems; i++)
    {
        if (visited[i] == 0)
            goto cleanup;
    }

    ok = 1;

cleanup:
    free(in_degree);
    free(visited);
    return ok;
}

static int load_pointer_walk_file(const char *walk_file_path,
                                  struct pointer_chase_line *walk_array,
                                  uint64_t elems)
{
    FILE *input_file;
    uint64_t i;
    unsigned long long tmp;
    uint64_t max_offset;

    if (walk_file_path == NULL || walk_file_path[0] == '\0')
        return -1;

    input_file = fopen(walk_file_path, "r");
    if (input_file == NULL)
        return -1;

    max_offset = elems * POINTER_CHASE_CACHE_LINE;
    for (i = 0; i < elems; i++)
    {
        if (fscanf(input_file, "%llu", &tmp) != 1)
        {
            fclose(input_file);
            return -1;
        }

        if ((tmp % POINTER_CHASE_CACHE_LINE) != 0 || tmp >= max_offset)
        {
            fclose(input_file);
            return -1;
        }
        walk_array[i].next_offset = (uint64_t)tmp;
    }

    fclose(input_file);
    if (!validate_pointer_walk(walk_array, elems))
        return -1;
    return 0;
}

static int save_pointer_walk_file(const char *walk_file_path,
                                  const struct pointer_chase_line *walk_array,
                                  uint64_t elems)
{
    FILE *output_file;
    uint64_t i;

    if (walk_file_path == NULL || walk_file_path[0] == '\0')
        return -1;

    output_file = fopen(walk_file_path, "w");
    if (output_file == NULL)
        return -1;

    for (i = 0; i < elems; i++)
    {
        if (fprintf(output_file, "%llu\n",
                    (unsigned long long)walk_array[i].next_offset) < 0)
        {
            fclose(output_file);
            return -1;
        }
    }

    if (fclose(output_file) != 0)
        return -1;
    return 0;
}

static void init_pointer_walk(const char *walk_file_path,
                              struct pointer_chase_line *walk_array,
                              uint64_t elems)
{
    if (load_pointer_walk_file(walk_file_path, walk_array, elems) == 0)
    {
        printf("Pointer walk loaded from '%s'.\n", walk_file_path);
        return;
    }

    printf("Pointer walk file '%s' unavailable or invalid; generating deterministic walk in-memory.\n",
           walk_file_path ? walk_file_path : "(null)");
    generate_pointer_walk(walk_array, elems);

    if (save_pointer_walk_file(walk_file_path, walk_array, elems) == 0)
    {
        printf("Pointer walk saved to '%s' for future executions.\n", walk_file_path);
    }
    else
    {
        printf("WARNING: failed to save pointer walk to '%s' (%s).\n",
               walk_file_path ? walk_file_path : "(null)", strerror(errno));
    }
}

static uint64_t pointer_chase_kernel(struct pointer_chase_line *walk_array,
                                     uint64_t elems,
                                     uint64_t total_loads,
                                     uint64_t *next_offset_state,
                                     uint64_t *kernel_cycles_out)
{
    uint64_t next_offset = 0;
    uint64_t max_offset;
    uint64_t begin_cycles = 0;

    if (walk_array == NULL || elems == 0 || total_loads == 0 || next_offset_state == NULL)
        return 0;

    max_offset = elems * POINTER_CHASE_CACHE_LINE;
    if (*next_offset_state < max_offset && ((*next_offset_state % POINTER_CHASE_CACHE_LINE) == 0))
        next_offset = *next_offset_state;

    begin_cycles = now_cycles();
#if defined(__aarch64__)
    {
        uint64_t remaining = total_loads;
        uint64_t next = next_offset;
        uint64_t base = (uint64_t)(uintptr_t)walk_array;
        asm volatile(
            "cmp %0, #0\n\t"
            "beq 2f\n\t"
            "1:\n\t"
            "add x3, %2, %1\n\t"
            "ldr %1, [x3]\n\t"
            "subs %0, %0, #1\n\t"
            "bne 1b\n\t"
            "2:\n\t"
            : "+r"(remaining), "+r"(next)
            : "r"(base)
            : "x3", "cc", "memory");
        next_offset = next;
    }
#else
    {
        uint64_t i;
        uint8_t *base = (uint8_t *)walk_array;
        for (i = 0; i < total_loads; i++)
        {
            volatile uint64_t *entry = (volatile uint64_t *)(base + next_offset);
            next_offset = *entry;
        }
    }
#endif

    if (kernel_cycles_out != NULL)
        *kernel_cycles_out += (now_cycles() - begin_cycles);

    *next_offset_state = next_offset;
    return next_offset;
}

static void parse_args(int argc, char *argv[], cli_options *opts)
{
    int opt;

    while ((opt = getopt(argc, argv, ":c:x:n:w:P:mdh")) != -1)
    {
        switch (opt)
        {
        case 'c':
            opts->chase_array_elems = atoll(optarg);
            if (opts->chase_array_elems <= 0)
            {
                printf("ERROR: pointer-chase elements must be > 0.\n");
                exit(1);
            }
            break;
        case 'x':
            opts->chase_total_loads = (uint64_t)strtoull(optarg, NULL, 10);
            if (opts->chase_total_loads == 0ULL)
            {
                printf("ERROR: pointer-chase total loads must be > 0.\n");
                exit(1);
            }
            break;
        case 'n':
            opts->run_iterations = atoi(optarg);
            if (opts->run_iterations <= 0)
            {
                printf("ERROR: iterations must be > 0.\n");
                exit(1);
            }
            break;
        case 'w':
            opts->walk_file_path = optarg;
            if (opts->walk_file_path[0] == '\0')
            {
                printf("ERROR: walk file path cannot be empty.\n");
                exit(1);
            }
            break;
        case 'P':
            opts->periodic_stats_ticks = atoll(optarg);
            if (opts->periodic_stats_ticks < 0)
            {
                printf("ERROR: periodic stats ticks must be >= 0.\n");
                exit(1);
            }
            break;
        case 'm':
            opts->m5_enabled = 1;
            break;
        case 'd':
            opts->debug_enabled = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            exit(1);
        }
    }
}

int main(int argc, char *argv[])
{
    cli_options opts = {
        .chase_array_elems = (long long)(POINTER_CHASE_DEFAULT_BYTES / POINTER_CHASE_CACHE_LINE),
        .chase_total_loads = 320000ULL,
        .run_iterations = 5,
        .walk_file_path = "array.dat",
        .m5_enabled = 0,
        .debug_enabled = 0,
        .periodic_stats_ticks = 10000,
    };

    struct pointer_chase_line *chase_array = NULL;
    ssize_t chase_array_bytes = 0;
    int rc;
    int iter;
    uint64_t pointer_chase_total_cycles = 0;
    uint64_t pointer_chase_next_offset = 0;
    unsigned long long pointer_chase_total_loads = 0ULL;
    volatile uint64_t chase_sink = 0;
    uint64_t timer_hz;
    double latency_cycles = 0.0;
    double latency_ns = 0.0;
    double total_time_ns = 0.0;
    uint64_t wall_begin;
    uint64_t wall_end;

    parse_args(argc, argv, &opts);

    chase_array_bytes = (ssize_t)opts.chase_array_elems * (ssize_t)sizeof(struct pointer_chase_line);
    rc = posix_memalign((void **)&chase_array, POINTER_CHASE_CACHE_LINE, (size_t)chase_array_bytes);
    if (rc != 0)
    {
        printf("Allocation of pointer-chase array failed, return code is %d\n", rc);
        return 1;
    }

    if (opts.debug_enabled)
    {
        char dbg_msg[512];
        snprintf(dbg_msg, sizeof(dbg_msg),
                 "Pointer chase config: elems=%lld total_loads_per_iter=%llu iterations=%d walk_file=%s m5_enabled=%d",
                 opts.chase_array_elems,
                 (unsigned long long)opts.chase_total_loads,
                 opts.run_iterations,
                 opts.walk_file_path,
                 opts.m5_enabled);
        debug_log_json(dbg_msg);
    }

    init_pointer_walk(opts.walk_file_path, chase_array, (uint64_t)opts.chase_array_elems);

    timer_hz = cycles_per_second();
    if (opts.m5_enabled)
    {
        m5_dump_reset_stats(0, 0);
        m5_dump_stats(0, (uint64_t)opts.periodic_stats_ticks);
    }

    wall_begin = now_cycles();
    for (iter = 0; iter < opts.run_iterations; iter++)
    {
        uint64_t kernel_cycles = 0;
        uint64_t chase_value = pointer_chase_kernel(chase_array,
                                                    (uint64_t)opts.chase_array_elems,
                                                    opts.chase_total_loads,
                                                    &pointer_chase_next_offset,
                                                    &kernel_cycles);
        chase_sink ^= chase_value;
        pointer_chase_total_cycles += kernel_cycles;
        pointer_chase_total_loads += (unsigned long long)opts.chase_total_loads;
    }
    wall_end = now_cycles();

    if (opts.m5_enabled)
    {
        m5_dump_stats(0, 0);
        m5_exit(0);
    }

    if (pointer_chase_total_loads > 0ULL && pointer_chase_total_cycles > 0ULL)
        latency_cycles = (double)pointer_chase_total_cycles / (double)pointer_chase_total_loads;

    if (latency_cycles > 0.0 && timer_hz > 0ULL)
        latency_ns = latency_cycles * (1.0e9 / (double)timer_hz);

    if (wall_end > wall_begin && timer_hz > 0ULL)
        total_time_ns = ((double)(wall_end - wall_begin) * 1.0e9) / (double)timer_hz;

    printf("Pointer-chase-only: sink=%llu total_cycles=%llu total_loads=%llu avg_latency_cycles=%.6f avg_latency_ns=%.6f total_time_ns=%.6f\n",
           (unsigned long long)chase_sink,
           (unsigned long long)pointer_chase_total_cycles,
           pointer_chase_total_loads,
           latency_cycles,
           latency_ns,
           total_time_ns);

    free(chase_array);
    return 0;
}
