#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <cilk/cilk.h>
#include <assert.h>
#include <string.h>

#include "timer.h"
#include "recursive_spawn.h"
#include "emu_chunked_array.h"

#ifdef __le64__
#include <memoryweb.h>
#else
#include "memoryweb_x86.h"
#endif

typedef struct global_stream_data {
    emu_chunked_array array_a;
    emu_chunked_array array_b;
    emu_chunked_array array_c;
    long ** a;
    long ** b;
    long ** c;
    long n;
    long num_threads;
} global_stream_data;


// #define INDEX(PTR, BLOCK, I) (PTR[I/BLOCK][I%BLOCK])
#define INDEX(PTR, BLOCK, I) (PTR[I >> PRIORITY(BLOCK)][I&(BLOCK-1)])

void
global_stream_init(global_stream_data * data, long n)
{
    data->n = n;
    emu_chunked_array_init(&data->array_a, n, sizeof(long));
    data->a = (long**)data->array_a.data;
    emu_chunked_array_init(&data->array_b, n, sizeof(long));
    data->b = (long**)data->array_b.data;
    emu_chunked_array_init(&data->array_c, n, sizeof(long));
    data->c = (long**)data->array_c.data;

    // TODO Initialize array values

#ifdef __le64__
    // Replicate pointers to all other nodelets
    data = mw_get_nth(data, 0);
    for (long i = 1; i < NODELETS(); ++i) {
        global_stream_data * remote_data = mw_get_nth(data, i);
        memcpy(remote_data, data, sizeof(global_stream_data));
    }
#endif
}

void
global_stream_deinit(global_stream_data * data)
{
    emu_chunked_array_deinit(&data->array_a);
    emu_chunked_array_deinit(&data->array_b);
    emu_chunked_array_deinit(&data->array_c);
}

// serial - just a regular for loop
void
global_stream_add_serial(global_stream_data * data)
{
    long block_sz = data->n / NODELETS();
    for (long i = 0; i < data->n; ++i) {
        INDEX(data->c, block_sz, i) = INDEX(data->a, block_sz, i) + INDEX(data->b, block_sz, i);
    }
}

// cilk_for - cilk_for loop with grainsize set to control number of threads
void
global_stream_add_cilk_for(global_stream_data * data)
{
    long block_sz = data->n / NODELETS();
    #pragma cilk grainsize = data->n / data->num_threads
    cilk_for (long i = 0; i < data->n; ++i) {
        INDEX(data->c, block_sz, i) = INDEX(data->a, block_sz, i) + INDEX(data->b, block_sz, i);
    }
}

noinline void
recursive_spawn_add_worker(long begin, long end, global_stream_data *data)
{
    long block_sz = data->n / NODELETS();
    for (long i = begin; i < end; ++i) {
        INDEX(data->c, block_sz, i) = INDEX(data->a, block_sz, i) + INDEX(data->b, block_sz, i);
    }
}

noinline void
recursive_spawn_add(long begin, long end, long grain, global_stream_data *data)
{
    RECURSIVE_CILK_SPAWN(begin, end, grain, recursive_spawn_add, data);
}

// recursive_spawn - recursively spawn threads to subdivide the range until the grain size is reached
void
global_stream_add_recursive_spawn(global_stream_data * data)
{
    recursive_spawn_add(0, data->n, data->n / data->num_threads, data);
}

// serial_spawn - spawn one thread to handle each grain-sized chunk of the range
void
global_stream_add_serial_spawn(global_stream_data * data)
{
    long grain = data->n / data->num_threads;
    for (long i = 0; i < data->n; i += grain) {
        long begin = i;
        long end = begin + grain <= data->n ? begin + grain : data->n;
        cilk_spawn recursive_spawn_add_worker(begin, end, data);
    }
    cilk_sync;
}

noinline void
serial_remote_spawn_level2(long begin, long end, long * a, long * b, long * c)
{
    for (long i = begin; i < end; ++i) {
        c[i] = a[i] + b[i];
    }
}

noinline void
serial_remote_spawn_level1(long * a, long * b, long * c, long n, long grain)
{
    for (long i = 0; i < n; i += grain) {
        long begin = i;
        long end = begin + grain <= n ? begin + grain : n;
        cilk_spawn serial_remote_spawn_level2(begin, end, a, b, c);
    }
    cilk_sync;
}

// serial_remote_spawn - remote spawn a thread on each nodelet, then do a serial spawn locally
void
global_stream_add_serial_remote_spawn(global_stream_data * data)
{
    // Each thread will be responsible for the elements on one nodelet
    long local_n = data->n / NODELETS();
    // Calculate the grain so we get the right number of threads globally
    long grain = data->n / data->num_threads;
    // Spawn a thread on each nodelet
    for (long i = 0; i < NODELETS(); ++i) {
        cilk_spawn serial_remote_spawn_level1(data->a[i], data->b[i], data->c[i], local_n, grain);
    }
    cilk_sync;
}

noinline void
recursive_remote_spawn_level2_worker(long begin, long end, long * a, long * b, long * c)
{
    for (long i = begin; i < end; ++i) {
        c[i] = a[i] + b[i];
    }
}

noinline void
recursive_remote_spawn_level2(long begin, long end, long grain, long * a, long * b, long * c)
{
    RECURSIVE_CILK_SPAWN(begin, end, grain, recursive_remote_spawn_level2, a, b, c);
}

noinline void
recursive_remote_spawn_level1(long low, long high, long * hint, global_stream_data * data)
{
    for (;;) {
        long count = high - low;
        if (count == 1) break;
        long mid = low + count / 2;
        cilk_spawn recursive_remote_spawn_level1(low, mid, data->a[low], data);
        low = mid;
    }

    /* Recursive base case: call worker function */
    long local_n = data->n / NODELETS();
    long grain = data->n / data->num_threads;
    recursive_remote_spawn_level2(0, local_n, grain, data->a[low], data->b[low], data->c[low]);
}

// recursive_remote_spawn - Recursively spawns threads to divice up the loop range, using remote spawns where possible.
void
global_stream_add_recursive_remote_spawn(global_stream_data * data)
{
    recursive_remote_spawn_level1(0, NODELETS(), data->a[0], data);
}

void
global_stream_add_emu_for_2d_worker(emu_chunked_array * array, long begin, long end, void * arg1)
{
    (void)array;
    global_stream_data * data = (global_stream_data *)arg1;
    long block_sz = data->n / NODELETS();

    long * c = &INDEX(data->c, block_sz, begin);
    long * b = &INDEX(data->b, block_sz, begin);
    long * a = &INDEX(data->a, block_sz, begin);

    for (long i = 0; i < end-begin; ++i) {
        c[i] = a[i] + b[i];
    }
}

void
global_stream_add_emu_for_2d(global_stream_data * data)
{
    emu_chunked_array_apply_v1(&data->array_a, GLOBAL_GRAIN(data->n),
        global_stream_add_emu_for_2d_worker, data
    );
}

// serial_remote_spawn_shallow - same as serial_remote_spawn, but with only one level of spawning
void
global_stream_add_serial_remote_spawn_shallow(global_stream_data * data)
{
    long local_n = data->n / NODELETS();
    long grain = data->n / data->num_threads;

    for (long i = 0; i < NODELETS(); ++i) {
        long * a = data->a[i];
        long * b = data->b[i];
        long * c = data->c[i];
        for (long j = 0; j < local_n; j += grain) {
            long begin = j;
            long end = begin + grain <= local_n ? begin + grain : local_n;
            cilk_spawn serial_remote_spawn_level2(begin, end, a, b, c);
        }
    }
    cilk_sync;
}

#define RUN_BENCHMARK(X) \
do {                                                        \
    timer_start();                                          \
    X (&data);                                              \
    long ticks = timer_stop();                              \
    double bw = timer_calc_bandwidth(ticks, data.n * sizeof(long) * 3); \
    timer_print_bandwidth( #X , bw);                        \
} while (0)

void
runtime_assert(bool condition, const char* message) {
    if (!condition) {
        printf("ERROR: %s\n", message); fflush(stdout);
        exit(1);
    }
}


replicated global_stream_data data;

int main(int argc, char** argv)
{
    struct {
        const char* mode;
        long log2_num_elements;
        long num_threads;
    } args;

    if (argc != 4) {
        printf("Usage: %s mode log2_num_elements num_threads\n", argv[0]);
        exit(1);
    } else {
        args.mode = argv[1];
        args.log2_num_elements = atol(argv[2]);
        args.num_threads = atol(argv[3]);

        if (args.log2_num_elements <= 0) { printf("log2_num_elements must be > 0"); exit(1); }
        if (args.num_threads <= 0) { printf("num_threads must be > 0"); exit(1); }
    }

    long n = 1L << args.log2_num_elements;
    long mbytes = n * sizeof(long) / (1024*1024);
    long mbytes_per_nodelet = mbytes / NODELETS();
    printf("Initializing arrays with %li elements each (%li MiB total, %li MiB per nodelet)\n", 3 * n, 3 * mbytes, 3 * mbytes_per_nodelet);
    fflush(stdout);
    data.num_threads = args.num_threads;
    global_stream_init(&data, n);
    printf("Doing vector addition using %s\n", args.mode); fflush(stdout);

    if (!strcmp(args.mode, "cilk_for")) {
        RUN_BENCHMARK(global_stream_add_cilk_for);
    } else if (!strcmp(args.mode, "serial_spawn")) {
        RUN_BENCHMARK(global_stream_add_serial_spawn);
    } else if (!strcmp(args.mode, "serial_remote_spawn")) {
        runtime_assert(data.num_threads >= NODELETS(), "serial_remote_spawn mode will always use at least one thread per nodelet");
        RUN_BENCHMARK(global_stream_add_serial_remote_spawn);
    } else if (!strcmp(args.mode, "serial_remote_spawn_shallow")) {
        runtime_assert(data.num_threads >= NODELETS(), "serial_remote_spawn_shallow mode will always use at least one thread per nodelet");
        RUN_BENCHMARK(global_stream_add_serial_remote_spawn_shallow);
    } else if (!strcmp(args.mode, "recursive_spawn")) {
        RUN_BENCHMARK(global_stream_add_recursive_spawn);
    } else if (!strcmp(args.mode, "recursive_remote_spawn")) {
        runtime_assert(data.num_threads >= NODELETS(), "recursive_remote_spawn mode will always use at least one thread per nodelet");
        RUN_BENCHMARK(global_stream_add_recursive_remote_spawn);
    } else if (!strcmp(args.mode, "emu_for_2d")) {
        runtime_assert(data.num_threads >= NODELETS(), "emu_for_2d mode will always use at least one thread per nodelet");
        RUN_BENCHMARK(global_stream_add_emu_for_2d);
    } else if (!strcmp(args.mode, "serial")) {
        runtime_assert(data.num_threads == 1, "serial mode can only use one thread");
        RUN_BENCHMARK(global_stream_add_serial);
    } else {
        printf("Mode %s not implemented!", args.mode);
    }

    global_stream_deinit(&data);
    return 0;
}
