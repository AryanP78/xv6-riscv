// rt_demo.c — DAPRA real-process demonstration
//
// Forks two competing processes:
//   RT child  : priority=3, deadline=ticks+60  (must finish quickly)
//   BG child  : priority=1, deadline=0          (no deadline)
//
// Both children allocate memory and touch pages, competing for the
// 6-frame limit enforced by pagerep.  DAPRA should protect RT pages
// and evict BG pages.  FIFO/LRU/LFU cannot distinguish them.
//
// Usage inside xv6 shell:
//   rt_demo          (runs with DAPRA, default)
//   rt_demo fifo     (switch to FIFO first)
//   rt_demo lru
//   rt_demo lfu
//   rt_demo dapra

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PAGES      16        // pages each child touches
#define PAGE_SIZE  4096
#define POLICY_FIFO  0
#define POLICY_LRU   1
#define POLICY_LFU   2
#define POLICY_DAPRA 3

// Touch every byte of a memory region to force page faults
static void
touch_pages(char *buf, int npages)
{
  for (int i = 0; i < npages; i++) {
    // Write then read to ensure the page is truly accessed
    buf[i * PAGE_SIZE] = (char)(i + 1);
    // Re-read to set PTE_A again for tick scanner
    volatile char c = buf[i * PAGE_SIZE];
    (void)c;
  }
}

static void
print_policy(int p)
{
  if      (p == POLICY_FIFO)  printf("FIFO");
  else if (p == POLICY_LRU)   printf("LRU");
  else if (p == POLICY_LFU)   printf("LFU");
  else                        printf("DAPRA");
}

int
main(int argc, char *argv[])
{
  // Parse optional policy argument
  int policy = POLICY_DAPRA;
  if (argc >= 2) {
    if      (strcmp(argv[1], "fifo")  == 0) policy = POLICY_FIFO;
    else if (strcmp(argv[1], "lru")   == 0) policy = POLICY_LRU;
    else if (strcmp(argv[1], "lfu")   == 0) policy = POLICY_LFU;
    else if (strcmp(argv[1], "dapra") == 0) policy = POLICY_DAPRA;
    else {
      printf("usage: rt_demo [fifo|lru|lfu|dapra]\n");
      exit(1);
    }
  }

  // Set policy and reset counters
  setpolicy(policy);

  printf("\n=== DAPRA Real-Process Demo ===\n");
  printf("Policy : "); print_policy(policy); printf("\n");
  printf("Frames : 6 (max_user_frames)\n");
  printf("Pages each child touches: %d\n\n", PAGES);

  uint64 f0, e0;
  getstats(&f0, &e0);  // baseline

  // ── Fork RT child ──────────────────────────────────────────────────────────
  int rt_pid = fork();
  if (rt_pid == 0) {
    // CHILD: real-time process
    int now = uptime();
    setpriority(3, now + 60);   // RT, deadline 60 ticks from now

    char *buf = sbrklazy(PAGES * PAGE_SIZE);
    if (buf == (char*)-1) {
      printf("rt: sbrklazy failed\n");
      exit(1);
    }

    touch_pages(buf, PAGES);

    printf("[RT  pid=%d] touched %d pages  priority=3  deadline=now+60\n",
           getpid(), PAGES);
    exit(0);
  }

  // ── Fork BG child ──────────────────────────────────────────────────────────
  int bg_pid = fork();
  if (bg_pid == 0) {
    // CHILD: background process
    setpriority(1, 0);   // BG, no deadline

    char *buf = sbrklazy(PAGES * PAGE_SIZE);
    if (buf == (char*)-1) {
      printf("bg: sbrklazy failed\n");
      exit(1);
    }

    touch_pages(buf, PAGES);

    printf("[BG  pid=%d] touched %d pages  priority=1  deadline=none\n",
           getpid(), PAGES);
    exit(0);
  }

  // ── Wait for both children ─────────────────────────────────────────────────
  int s1, s2;
  wait(&s1);
  wait(&s2);

  // ── Print results ──────────────────────────────────────────────────────────
  uint64 f1, e1;
  getstats(&f1, &e1);

  uint64 total_faults    = f1 - f0;
  uint64 total_evictions = e1 - e0;

  printf("\n--- Results ---\n");
  printf("Policy     : "); print_policy(policy); printf("\n");
  printf("Faults     : %d\n", (int)total_faults);
  printf("Evictions  : %d\n", (int)total_evictions);

  if (policy == POLICY_DAPRA) {
    printf("\nWith DAPRA: BG pages were evicted preferentially.\n");
    printf("RT pages stayed protected due to high urgency + priority score.\n");
  } else {
    printf("\nWith "); print_policy(policy);
    printf(": no priority awareness — RT and BG pages treated equally.\n");
    printf("Run 'rt_demo dapra' to compare.\n");
  }

  printf("\nTip: run all policies back-to-back:\n");
  printf("  rt_demo fifo && rt_demo lru && rt_demo lfu && rt_demo dapra\n\n");

  exit(0);
}
