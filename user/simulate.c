// simulate.c — Full DAPRA simulation inside xv6
// Runs 500 test cases, each with a randomly generated page reference string.
// Compares FIFO, LRU, LFU, and DAPRA across all tests.
// Reports average faults and improvement percentages — mirrors dapra_v4.c.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// ── Random number generator (Park-Miller, from grind.c) ──────────────────────
static unsigned long rand_next = 1;

static unsigned long
do_rand(unsigned long *ctx)
{
    long hi, lo, x;
    x   = (*ctx % 0x7ffffffe) + 1;
    hi  = x / 127773;
    lo  = x % 127773;
    x   = 16807 * lo - 2836 * hi;
    if (x < 0) x += 0x7fffffff;
    x--;
    *ctx = x;
    return x;
}

static int
xrand(void)
{
    return (int)do_rand(&rand_next);
}

static void
xsrand(unsigned long seed)
{
    rand_next = seed;
}

// ── Constants ────────────────────────────────────────────────────────────────
#define TESTS      500    // number of random test cases
#define MAX_N      50     // max pages in one test sequence
#define MAX_CAP    8      // max frame capacity per test
#define PGSIZE     4096

#define PRIO_BG    1
#define PRIO_INT   2
#define PRIO_RT    3

// ── Standalone software simulators (no kernel pagerep, pure user-space) ──────
// These mirror dapra_v4.c exactly so the comparison is fair.

// FIFO
static int
sim_fifo(int pages[], int n, int capacity)
{
    int frames[MAX_CAP];
    int front = 0, size = 0, faults = 0;
    for (int i = 0; i < capacity; i++) frames[i] = -1;

    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int j = 0; j < size; j++)
            if (frames[j] == pages[i]) { found = 1; break; }
        if (!found) {
            faults++;
            if (size < capacity)
                frames[size++] = pages[i];
            else {
                frames[front] = pages[i];
                front = (front + 1) % capacity;
            }
        }
    }
    return faults;
}

// LRU
static int
sim_lru(int pages[], int n, int capacity)
{
    int frames[MAX_CAP], last[MAX_CAP], faults = 0;
    for (int i = 0; i < capacity; i++) { frames[i] = -1; last[i] = 0; }

    for (int i = 0; i < n; i++) {
        int found = -1;
        for (int j = 0; j < capacity; j++)
            if (frames[j] == pages[i]) { found = j; break; }
        if (found != -1) {
            last[found] = i;
        } else {
            faults++;
            int lru = 0;
            for (int j = 1; j < capacity; j++)
                if (last[j] < last[lru]) lru = j;
            frames[lru] = pages[i];
            last[lru]   = i;
        }
    }
    return faults;
}

// LFU with aging every 8 ticks (mirrors dapra_v4.c)
static int
sim_lfu(int pages[], int n, int capacity)
{
    int frames[MAX_CAP], freq[MAX_CAP], faults = 0;
    for (int i = 0; i < capacity; i++) { frames[i] = -1; freq[i] = 0; }

    for (int i = 0; i < n; i++) {
        int found = -1;
        for (int j = 0; j < capacity; j++)
            if (frames[j] == pages[i]) { found = j; break; }
        if (found != -1) {
            freq[found]++;
        } else {
            faults++;
            int lfu = 0;
            for (int j = 1; j < capacity; j++)
                if (freq[j] < freq[lfu]) lfu = j;
            frames[lfu] = pages[i];
            freq[lfu]   = 1;
        }
        // Aging every 8 accesses
        if (i > 0 && i % 8 == 0)
            for (int j = 0; j < capacity; j++)
                if (freq[j] > 1) freq[j] = (freq[j] * 8) / 10;
    }
    return faults;
}

// DAPRA v4 — Protection Score (integer ×1000)
// Score = 0.50×urgency + 0.30×norm_freq + 0.20×norm_prio + 0.35×IRI_bonus
// Evict the page with the LOWEST score.
typedef struct {
    int page;
    int priority;
    int deadline;
    int freq;
    int last;
    int prev_last;
    int valid;
} DFrame;

static int
dapra_score(DFrame *f, int t, int max_freq)
{
    int gap     = f->deadline - t;
    int urgency = (gap <= 0) ? 1000 : 1000 / (gap > 0 ? gap : 1);
    int nfreq   = (max_freq > 0) ? (f->freq * 1000) / max_freq : 0;
    int nprio   = (f->priority * 1000) / 3;
    int base    = (500 * urgency + 300 * nfreq + 200 * nprio) / 1000;

    int reuse = 0;
    if (f->prev_last >= 0) {
        int iri = f->last - f->prev_last;
        if (iri > 0) {
            int ttl = (f->last + iri) - t;
            if (ttl > -iri) {
                int r = (ttl <= 0) ? 1000 : 1000 / ttl;
                reuse = (r * nprio) / 1000;
            }
        }
    }
    return base + (350 * reuse) / 1000;
}

static int
sim_dapra(int pages[], int prios[], int deadlines[], int n, int capacity)
{
    DFrame fr[MAX_CAP];
    int faults = 0, count = 0;

    for (int i = 0; i < capacity; i++) {
        fr[i].page = -1; fr[i].freq = 0;
        fr[i].last = 0;  fr[i].prev_last = -1;
        fr[i].valid = 0; fr[i].priority = 1;
        fr[i].deadline = 9999;
    }

    for (int t = 0; t < n; t++) {
        // Find page in frames
        int found = -1;
        for (int j = 0; j < count; j++)
            if (fr[j].valid && fr[j].page == pages[t]) { found = j; break; }

        if (found != -1) {
            fr[found].prev_last = fr[found].last;
            fr[found].last      = t;
            fr[found].freq++;
        } else {
            faults++;
            int slot;
            if (count < capacity) {
                slot = count++;
            } else {
                // Find max_freq
                int mf = 1;
                for (int j = 0; j < capacity; j++)
                    if (fr[j].freq > mf) mf = fr[j].freq;
                // Find victim (lowest score)
                int min_score = 0x7fffffff, victim = 0;
                for (int j = 0; j < capacity; j++) {
                    int s = dapra_score(&fr[j], t, mf);
                    if (s < min_score) { min_score = s; victim = j; }
                }
                slot = victim;
            }
            fr[slot].page      = pages[t];
            fr[slot].priority  = prios[t];
            fr[slot].deadline  = deadlines[t];
            fr[slot].freq      = 1;
            fr[slot].last      = t;
            fr[slot].prev_last = -1;
            fr[slot].valid     = 1;
        }

        // Priority-stratified aging every 8 ticks
        if (t > 0 && t % 8 == 0) {
            for (int j = 0; j < count; j++) {
                if (fr[j].priority == PRIO_BG)
                    fr[j].freq = (fr[j].freq * 6) / 10;
                else if (fr[j].priority == PRIO_INT)
                    fr[j].freq = (fr[j].freq * 8) / 10;
                if (fr[j].freq < 1) fr[j].freq = 1;
            }
        }
    }
    return faults;
}

// ── Test case generator (structured workload, mirrors dapra_v4.c) ─────────────
static void
gen_test(int pages[], int prios[], int deadlines[], int *n, int *capacity)
{
    *n        = 30 + xrand() % 20;   // 30–49 accesses
    *capacity = 3  + xrand() % 6;    // 3–8 frames

    int rt_idx  = xrand() % 3;
    int bg_idx  = xrand() % 10;
    int int_base= 3 + xrand() % 4;

    for (int i = 0; i < *n; i++) {
        int r = xrand() % 100;
        if (r < 25) {
            // RT: round-robin through 3 hot pages
            prios[i]     = PRIO_RT;
            pages[i]     = rt_idx % 3;
            deadlines[i] = 2 + xrand() % 4;   // tight: 2–5 ticks
            rt_idx++;
        } else if (r < 50) {
            // INT: 70% locality
            prios[i]     = PRIO_INT;
            pages[i]     = (xrand() % 100 < 70) ? int_base : (3 + xrand() % 4);
            deadlines[i] = 8 + xrand() % 12;  // medium: 8–19 ticks
        } else {
            // BG: sequential scan over 10 pages
            prios[i]     = PRIO_BG;
            pages[i]     = 7 + (bg_idx % 10);
            deadlines[i] = 25 + xrand() % 25; // far: 25–49 ticks
            bg_idx++;
        }
    }
}

// ── Main ─────────────────────────────────────────────────────────────────────
int
main(void)
{
    // Seed with uptime for variation
    xsrand((unsigned long)uptime() + 42);

    int pages[MAX_N], prios[MAX_N], deadlines[MAX_N];
    int n, capacity;

    long total_fifo = 0, total_lru = 0, total_lfu = 0, total_dapra = 0;

    printf("\n=== DAPRA Full Simulation (%d test cases) ===\n\n", TESTS);
    printf("Running");

    for (int t = 0; t < TESTS; t++) {
        gen_test(pages, prios, deadlines, &n, &capacity);

        total_fifo  += sim_fifo (pages, n, capacity);
        total_lru   += sim_lru  (pages, n, capacity);
        total_lfu   += sim_lfu  (pages, n, capacity);
        total_dapra += sim_dapra(pages, prios, deadlines, n, capacity);

        // Progress dot every 50 tests
        if ((t + 1) % 50 == 0) printf(".");
    }

    printf(" done\n\n");

    // Integer averages (×100 for 2 decimal places)
    int avg_fifo  = (int)((total_fifo  * 100) / TESTS);
    int avg_lru   = (int)((total_lru   * 100) / TESTS);
    int avg_lfu   = (int)((total_lfu   * 100) / TESTS);
    int avg_dapra = (int)((total_dapra * 100) / TESTS);

    printf("===== AVERAGE RESULTS (%d TESTS) =====\n\n", TESTS);
    printf("  FIFO  avg faults = %d.%02d\n", avg_fifo/100,  avg_fifo%100);
    printf("  LRU   avg faults = %d.%02d\n", avg_lru/100,   avg_lru%100);
    printf("  LFU   avg faults = %d.%02d\n", avg_lfu/100,   avg_lfu%100);
    printf("  DAPRA avg faults = %d.%02d\n", avg_dapra/100, avg_dapra%100);

    // Improvement over LRU and LFU (integer ×100 = basis points)
    int imp_fifo = (int)(((total_fifo  - total_dapra) * 10000) / total_fifo);
    int imp_lru  = (int)(((total_lru   - total_dapra) * 10000) / total_lru);
    int imp_lfu  = (int)(((total_lfu   - total_dapra) * 10000) / total_lfu);

    printf("\n  Improvement over FIFO : %d.%02d%%\n", imp_fifo/100, imp_fifo%100);
    printf("  Improvement over LRU  : %d.%02d%%\n", imp_lru/100,  imp_lru%100);
    printf("  Improvement over LFU  : %d.%02d%%\n", imp_lfu/100,  imp_lfu%100);
    printf("\n");

    exit(0);
}
