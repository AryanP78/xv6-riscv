// benchmark.c — DAPRA page replacement comparison benchmark
//
// Demonstrates DAPRA's advantage by using mixed-priority workloads:
//   RT pages  (0–2):  priority=3, tight deadline — should stay in memory
//   BG pages  (3–12): priority=1, far deadline   — expendable
//
// With max_user_frames=6, the system can hold 3 RT + 3 BG pages at once.
// Every time a new BG page is accessed, one page must be evicted.
//
// DAPRA: always evicts BG pages (low score) → RT pages never re-fault
// LFU/FIFO/LRU: may evict RT pages → RT pages re-fault repeatedly

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE     4096
#define N_RT       3       // number of RT (high-priority) pages
#define N_BG       10      // number of BG (low-priority) pages
#define NPAGES     (N_RT + N_BG)
#define ROUNDS     60      // access rounds

static const char *policy_names[] = {"FIFO", "LRU", "LFU", "DAPRA"};

static void
touch(volatile char *page)
{
  page[0] = 1;
}

static void
run_policy(int policy)
{
  setpolicy(policy);

  // Lazily allocate all pages — each access causes vmfault → pagerep_alloc
  char *mem = sbrklazy(NPAGES * PGSIZE);
  if (mem == (char*)-1) {
    printf("sbrklazy failed\n");
    return;
  }

  // ── Phase 1: fault in RT pages with high priority + tight deadline ──
  // These get stored in the frame table with priority=3.
  setpriority(3, uptime() + 200);
  for (int i = 0; i < N_RT; i++)
    touch((volatile char*)(mem + i * PGSIZE));

  // ── Phase 2: fault in BG pages with low priority + far deadline ──
  // These get stored with priority=1. Because limit=6 and we already
  // have 3 RT pages, the 4th BG page (7th total) MUST evict something.
  // DAPRA evicts BG; LFU/FIFO/LRU may evict RT.
  setpriority(1, uptime() + 5000);
  for (int i = N_RT; i < NPAGES; i++)
    touch((volatile char*)(mem + i * PGSIZE));

  // ── Phase 3: repeatedly access RT pages + new BG pages each round ──
  // Each round accesses all 3 RT pages, then a rotating BG page.
  // The BG cycle forces constant evictions.
  setpriority(3, uptime() + 200);   // back to RT priority
  for (int r = 0; r < ROUNDS; r++) {
    // Access all RT pages
    for (int i = 0; i < N_RT; i++)
      touch((volatile char*)(mem + i * PGSIZE));

    // Access one BG page (rotate through all 10)
    // Switch to BG priority before touching so the re-fault gets correct metadata
    setpriority(1, uptime() + 5000);
    touch((volatile char*)(mem + (N_RT + (r % N_BG)) * PGSIZE));
    setpriority(3, uptime() + 200);
  }

  uint64 faults = 0, evictions = 0;
  getstats(&faults, &evictions);
  printf("  %s\t faults=%d  evictions=%d\n",
         policy_names[policy], (int)faults, (int)evictions);

  // Release pages (clears frame table entries via pagerep_on_kfree)
  sbrk(-(NPAGES * PGSIZE));
}

int
main(void)
{
  printf("\n=== DAPRA Page Replacement Benchmark (xv6) ===\n");
  printf("  %d RT pages (priority=3, tight deadline)\n", N_RT);
  printf("  %d BG pages (priority=1, far deadline)\n", N_BG);
  printf("  Frame limit: 6 | Rounds: %d\n\n", ROUNDS);
  printf("  Policy\t faults     evictions\n");
  printf("  ------\t ------     ---------\n");

  for (int p = 0; p <= 3; p++)
    run_policy(p);

  printf("\nDAPRA keeps RT pages alive by protecting high-score\n");
  printf("(priority + urgency) frames from eviction.\n\n");
  exit(0);
}
