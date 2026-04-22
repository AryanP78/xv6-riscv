/*
 * ============================================================
 *  DAPRA v4  —  xv6-RISC-V port of dapra_v4_final.c
 *
 *  All float arithmetic replaced with integer x1000 scaling
 *  (xv6 kernel has no FPU; user programs avoid FPU state issues).
 *  rand()/srand()/time() replaced with Park-Miller RNG + uptime().
 *  printf format strings use only %d / %s (xv6 printf subset).
 *
 *  References:
 *  [1] Kim et al., Scientific Reports 2025   — IRI reuse bonus
 *  [2] Maas et al., CACM 2024               — freq aging
 *  [3] Giannessi et al., IEEE SEED 2024     — mixed workload
 * ============================================================
 */

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* ── tunables ─────────────────────────────────────────────── */
#define MAX_FRAMES     10
#define MAX_REFS       120
#define NUM_TESTS      500   /* 5000 too slow in QEMU; 500 matches paper */

/* ── priority classes ─────────────────────────────────────── */
#define PRIO_BG   1
#define PRIO_INT  2
#define PRIO_RT   3

/* ── aging ────────────────────────────────────────────────── */
#define AGING_INTERVAL  8
#define DECAY_BG        6   /* x0.60 */
#define DECAY_INT       8   /* x0.80 */

#define INT_MAX_VAL  0x7fffffff

/* ── Park-Miller RNG (no rand()/srand() in xv6) ───────────── */
static unsigned long rand_next = 1;

static unsigned long
do_rand(unsigned long *ctx)
{
    long hi, lo, x;
    x  = (*ctx % 0x7ffffffe) + 1;
    hi = x / 127773;
    lo = x % 127773;
    x  = 16807 * lo - 2836 * hi;
    if (x < 0) x += 0x7fffffff;
    x--;
    *ctx = x;
    return x;
}

static int  xrand(void)                { return (int)do_rand(&rand_next); }
static void xsrand(unsigned long seed) { rand_next = seed; }

/* ============================================================
 *  FIFO
 * ============================================================ */
static int
sim_fifo(int pages[], int n, int capacity)
{
    int frames[MAX_FRAMES];
    int head = 0, size = 0, faults = 0;
    for (int i = 0; i < capacity; i++) frames[i] = -1;

    for (int i = 0; i < n; i++) {
        int hit = 0;
        for (int j = 0; j < size; j++)
            if (frames[j] == pages[i]) { hit = 1; break; }
        if (!hit) {
            faults++;
            frames[head] = pages[i];
            head = (head + 1) % capacity;
            if (size < capacity) size++;
        }
    }
    return faults;
}

/* ============================================================
 *  LRU
 * ============================================================ */
static int
sim_lru(int pages[], int n, int capacity)
{
    int frames[MAX_FRAMES], ts[MAX_FRAMES], faults = 0;
    for (int i = 0; i < capacity; i++) { frames[i] = -1; ts[i] = -1; }

    for (int i = 0; i < n; i++) {
        int found = -1;
        for (int j = 0; j < capacity; j++)
            if (frames[j] == pages[i]) { found = j; break; }
        if (found >= 0) {
            ts[found] = i;
        } else {
            faults++;
            int v = 0;
            for (int j = 1; j < capacity; j++)
                if (ts[j] < ts[v]) v = j;
            frames[v] = pages[i];
            ts[v] = i;
        }
    }
    return faults;
}

/* ============================================================
 *  LFU  (tie-break: LRU)
 * ============================================================ */
static int
sim_lfu(int pages[], int n, int capacity)
{
    int frames[MAX_FRAMES], freq[MAX_FRAMES], ts[MAX_FRAMES], faults = 0;
    for (int i = 0; i < capacity; i++) {
        frames[i] = -1; freq[i] = 0; ts[i] = -1;
    }

    for (int i = 0; i < n; i++) {
        int found = -1;
        for (int j = 0; j < capacity; j++)
            if (frames[j] == pages[i]) { found = j; break; }
        if (found >= 0) {
            freq[found]++; ts[found] = i;
        } else {
            faults++;
            int v = 0;
            for (int j = 1; j < capacity; j++)
                if (freq[j] < freq[v] ||
                    (freq[j] == freq[v] && ts[j] < ts[v])) v = j;
            frames[v] = pages[i];
            freq[v] = 1; ts[v] = i;
        }
    }
    return faults;
}

/* ============================================================
 *  DAPRA v4  — base + IRI reuse bonus + priority aging
 *
 *  Score = base_v3 + (350 * reuse) / 1000
 *
 *  reuse = (1/max(1,TTL)) * norm_prio    [IRI prediction]
 *  Aging every AGING_INTERVAL ticks: BG x0.60, INT x0.80
 * ============================================================ */
typedef struct {
    int page, priority, deadline, freq, last, prev_last, valid;
} DFrame4;

static int
score_v4(const DFrame4 *f, int t, int max_freq)
{
    /* base (same as v3) */
    int gap     = f->deadline - t;
    int urgency = (gap <= 1) ? 1000 : 1000 / gap;
    int nfreq   = (max_freq > 0) ? (f->freq * 1000) / max_freq : 0;
    int nprio   = (f->priority * 1000) / 3;
    int base    = (500 * urgency + 300 * nfreq + 200 * nprio) / 1000;

    /* IRI reuse bonus [Kim et al. 2025] */
    int reuse = 0;
    if (f->prev_last >= 0) {
        int iri = f->last - f->prev_last;
        if (iri > 0) {
            int ttl = (f->last + iri) - t;
            if (ttl > -iri) {
                int raw = (ttl <= 1) ? 1000 : 1000 / ttl;
                reuse = (raw * nprio) / 1000;
            }
        } else {
            reuse = nprio;   /* same-tick double access */
        }
    }
    return base + (350 * reuse) / 1000;
}

static int
sim_dapra_v4(int pages[], int prios[], int deadlines[], int n, int capacity)
{
    DFrame4 fr[MAX_FRAMES];
    int faults = 0;
    memset(fr, 0, sizeof(fr));
    for (int j = 0; j < capacity; j++) fr[j].prev_last = -1;

    for (int t = 0; t < n; t++) {

        /* priority-stratified aging [Maas et al. 2024] */
        if (t > 0 && t % AGING_INTERVAL == 0) {
            for (int j = 0; j < capacity; j++) {
                if (!fr[j].valid) continue;
                if (fr[j].priority == PRIO_BG) {
                    fr[j].freq = (fr[j].freq * DECAY_BG) / 10;
                    if (fr[j].freq < 1) fr[j].freq = 1;
                } else if (fr[j].priority == PRIO_INT) {
                    fr[j].freq = (fr[j].freq * DECAY_INT) / 10;
                    if (fr[j].freq < 1) fr[j].freq = 1;
                }
                /* RT: no aging */
            }
        }

        int found = -1;
        for (int j = 0; j < capacity; j++)
            if (fr[j].valid && fr[j].page == pages[t]) { found = j; break; }

        if (found >= 0) {
            fr[found].prev_last = fr[found].last;
            fr[found].freq++;
            fr[found].last     = t;
            fr[found].deadline = deadlines[t];
            fr[found].priority = prios[t];
            continue;
        }

        faults++;
        int slot = -1;
        for (int j = 0; j < capacity; j++)
            if (!fr[j].valid) { slot = j; break; }

        if (slot < 0) {
            int mf = 1;
            for (int j = 0; j < capacity; j++)
                if (fr[j].valid && fr[j].freq > mf) mf = fr[j].freq;
            int min_s = INT_MAX_VAL;
            slot = 0;
            for (int j = 0; j < capacity; j++) {
                int s = score_v4(&fr[j], t, mf);
                if (s < min_s || (s == min_s && fr[j].freq < fr[slot].freq)) {
                    min_s = s; slot = j;
                }
            }
        }

        fr[slot].page     = pages[t];
        fr[slot].priority = prios[t];
        fr[slot].deadline = deadlines[t];
        fr[slot].freq     = 1;
        fr[slot].last     = t;
        fr[slot].prev_last = -1;
        fr[slot].valid    = 1;
    }
    return faults;
}

/* ============================================================
 *  WORKLOAD GENERATOR  [Giannessi et al., IEEE SEED 2024]
 *
 *  RT  (25%): pages {0-2}  round-robin, IRI~12, deadline 2-5
 *  INT (25%): pages {3-6}  70% locality, deadline 8-19
 *  BG  (50%): pages {7-16} sequential scan, deadline 25-49
 * ============================================================ */
static void
generateTest(int pages[], int prios[], int deadlines[], int *n, int *cap)
{
    *n   = 60 + xrand() % 40;
    *cap = 4  + xrand() % 3;

    int rt_idx   = xrand() % 3;
    int bg_idx   = xrand() % 10;
    int last_int = 3 + xrand() % 4;

    for (int i = 0; i < *n; i++) {
        int r = xrand() % 100;
        if (r < 25) {
            prios[i]     = PRIO_RT;
            pages[i]     = rt_idx % 3;
            rt_idx++;
            deadlines[i] = i + 2 + xrand() % 4;
        } else if (r < 50) {
            prios[i] = PRIO_INT;
            if (xrand() % 100 < 70) {
                pages[i] = last_int;
            } else {
                pages[i] = 3 + xrand() % 4;
                last_int = pages[i];
            }
            deadlines[i] = i + 8 + xrand() % 12;
        } else {
            prios[i]     = PRIO_BG;
            pages[i]     = 7 + (bg_idx % 10);
            bg_idx++;
            deadlines[i] = i + 25 + xrand() % 25;
        }
    }
}

/* ── print "xx.xx" from a value already multiplied by 100 ─── */
static void
print_avg(int v100)
{
    int whole = v100 / 100;
    int frac  = v100 % 100;
    if (frac < 10)
        printf("%d.0%d", whole, frac);
    else
        printf("%d.%d", whole, frac);
}

/* ── print "+xx.xx%" improvement of other vs dapra4 ────────── */
static void
print_imp(int other, int d4)
{
    int bp = ((other - d4) * 10000) / other;   /* basis points */
    int sign = (bp >= 0) ? 1 : -1;
    if (bp < 0) bp = -bp;
    int whole = bp / 100;
    int frac  = bp % 100;
    if (sign >= 0) {
        if (frac < 10) printf("+%d.0%d%%", whole, frac);
        else           printf("+%d.%d%%",  whole, frac);
    } else {
        if (frac < 10) printf("-%d.0%d%%", whole, frac);
        else           printf("-%d.%d%%",  whole, frac);
    }
}

/* ============================================================
 *  MAIN
 * ============================================================ */
int
main(void)
{
    xsrand((unsigned long)uptime() + 7);

    int pages[MAX_REFS], prios[MAX_REFS], deadlines[MAX_REFS];
    int n, cap;

    int t_fifo = 0, t_lru = 0, t_lfu = 0, t_d4 = 0;

    printf("\nRunning %d tests", NUM_TESTS);

    for (int t = 0; t < NUM_TESTS; t++) {
        generateTest(pages, prios, deadlines, &n, &cap);
        t_fifo += sim_fifo(pages, n, cap);
        t_lru  += sim_lru (pages, n, cap);
        t_lfu  += sim_lfu (pages, n, cap);
        t_d4   += sim_dapra_v4(pages, prios, deadlines, n, cap);
        if ((t + 1) % 50 == 0) printf(".");
    }
    printf(" done\n\n");

    /* averages x100 for 2 decimal places */
    int a_fifo = (t_fifo * 100) / NUM_TESTS;
    int a_lru  = (t_lru  * 100) / NUM_TESTS;
    int a_lfu  = (t_lfu  * 100) / NUM_TESTS;
    int a_d4   = (t_d4   * 100) / NUM_TESTS;

    printf("  +================================================+\n");
    printf("  |  DAPRA v4 vs All Algorithms (%d tests)       |\n", NUM_TESTS);
    printf("  |  Workload: RT round-robin + BG scan + INT loc |\n");
    printf("  |  Frames: 4-6    Refs/test: 60-99              |\n");
    printf("  +================================================+\n");
    printf("  | Algorithm       | Total  | Avg Faults |\n");
    printf("  |-----------------+--------+------------|\n");

    printf("  | FIFO            | %d | ", t_fifo); print_avg(a_fifo); printf("     |\n");
    printf("  | LRU             | %d | ", t_lru ); print_avg(a_lru ); printf("     |\n");
    printf("  | LFU             | %d | ", t_lfu ); print_avg(a_lfu ); printf("     |\n");
    printf("  | DAPRA v4 (ours) | %d | ", t_d4  ); print_avg(a_d4  ); printf(" <-- |\n");

    printf("  +================================================+\n");

    printf("\n  -- DAPRA v4 improvement --\n");
    printf("  vs FIFO  : "); print_imp(t_fifo, t_d4); printf("\n");
    printf("  vs LRU   : "); print_imp(t_lru,  t_d4); printf("\n");
    printf("  vs LFU   : "); print_imp(t_lfu,  t_d4); printf("\n");

    printf("\n  -- Research contributions --\n");
    printf("  [1] IRI Reuse Bonus   (Kim et al., Sci.Reports 2025)\n");
    printf("      Predicts next access = last + IRI\n");
    printf("      RT round-robin pages flagged before eviction\n");
    printf("  [2] Priority Aging    (Maas et al., CACM 2024)\n");
    printf("      BG x0.60, INT x0.80 every %d ticks\n", AGING_INTERVAL);
    printf("      Stale BG counts fade; RT urgency dominates\n");
    printf("  [3] Mixed Workload    (Giannessi et al., IEEE 2024)\n");
    printf("      RT:round-robin  BG:seq-scan  INT:70pct-locality\n\n");

    exit(0);
}
