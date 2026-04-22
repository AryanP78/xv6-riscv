// pagerep.c — DAPRA Page Replacement for xv6-riscv
// Implements FIFO, LRU, LFU, and DAPRA (deadline-aware) eviction policies.
// All arithmetic is integer-only (no FPU in xv6 kernel).

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

// ── Constants ────────────────────────────────────────────────────────────────
#define NFRAMES      ((PHYSTOP - KERNBASE) / PGSIZE)
#define PA2IDX(pa)   (((uint64)(pa) - KERNBASE) / PGSIZE)

#define POLICY_FIFO   0
#define POLICY_LRU    1
#define POLICY_LFU    2
#define POLICY_DAPRA  3

// ── Per-frame metadata ────────────────────────────────────────────────────────
struct frame_info {
  pagetable_t pt;        // owning page table
  uint64      va;        // virtual address in that page table
  int         priority;  // process priority (1=BG, 2=INT, 3=RT)
  int         deadline;  // absolute tick deadline
  int         freq;      // fault-in count (proxy for access frequency)
  int         last;      // ticks of last fault-in
  int         prev_last; // ticks of 2nd-to-last fault-in (for IRI)
  int         load_tick; // ticks when first loaded (FIFO ordering)
  int         valid;     // 1 = occupied
};

static struct frame_info frametable[NFRAMES];
static struct spinlock   pagerep_lock;
static int               frame_count = 0;

// Configurable: lower this to force evictions with a small working set.
// With 3 RT + 10 BG = 13 pages and limit=6, every BG access beyond the
// first 3 forces an eviction. DAPRA always evicts BG; others may evict RT.
int max_user_frames = 6;
int current_policy  = POLICY_DAPRA;

// Stats
uint64 pagerep_faults    = 0;
uint64 pagerep_evictions = 0;

// ── Initialisation ────────────────────────────────────────────────────────────
void
pagerep_init(void)
{
  initlock(&pagerep_lock, "pagerep");
  for (int i = 0; i < NFRAMES; i++)
    frametable[i].prev_last = -1;
}

// ── Called from kfree() to clear a frame entry ───────────────────────────────
// IMPORTANT: kfree holds kmem.lock when calling this.
// We acquire pagerep_lock here, so the ordering is always:
//   pagerep_lock (outer) → kmem.lock (inner via kalloc/kfree)
// Never hold pagerep_lock and then call kalloc/kfree. We honour this below.
void
pagerep_on_kfree(uint64 pa)
{
  if (pa < KERNBASE || pa >= PHYSTOP)
    return;
  int idx = (int)PA2IDX(pa);
  if (idx < 0 || idx >= NFRAMES)
    return;
  // If already invalid (set by do_evict before calling kfree), nothing to do.
  if (!frametable[idx].valid)
    return;
  acquire(&pagerep_lock);
  if (frametable[idx].valid) {
    frametable[idx].valid = 0;
    frame_count--;
    if (frame_count < 0) frame_count = 0;
  }
  release(&pagerep_lock);
}

// ── Eviction selectors (called with pagerep_lock held) ───────────────────────

static int
pick_fifo(void)
{
  int victim = -1, min_tick = 0x7fffffff;
  for (int i = 0; i < NFRAMES; i++)
    if (frametable[i].valid && frametable[i].load_tick < min_tick) {
      min_tick = frametable[i].load_tick;
      victim   = i;
    }
  return victim;
}

static int
pick_lru(void)
{
  int victim = -1, min_last = 0x7fffffff;
  for (int i = 0; i < NFRAMES; i++)
    if (frametable[i].valid && frametable[i].last < min_last) {
      min_last = frametable[i].last;
      victim   = i;
    }
  return victim;
}

static int
pick_lfu(void)
{
  int victim = -1, min_freq = 0x7fffffff;
  for (int i = 0; i < NFRAMES; i++)
    if (frametable[i].valid && frametable[i].freq < min_freq) {
      min_freq = frametable[i].freq;
      victim   = i;
    }
  return victim;
}

// DAPRA Protection Score (integer arithmetic, ×1000 scaling):
//   base  = 0.50×urgency + 0.30×norm_freq + 0.20×norm_prio
//   score = base + 0.35×(IRI_reuse × norm_prio)
// Evict the page with the LOWEST score.
static int
pick_dapra(void)
{
  // Find max frequency for normalisation
  int max_freq = 1;
  for (int i = 0; i < NFRAMES; i++)
    if (frametable[i].valid && frametable[i].freq > max_freq)
      max_freq = frametable[i].freq;

  int min_score = 0x7fffffff, victim = -1;
  for (int i = 0; i < NFRAMES; i++) {
    if (!frametable[i].valid)
      continue;
    struct frame_info *f = &frametable[i];

    // Urgency = 1000 / max(1, deadline-ticks); 1000 if overdue
    int gap     = f->deadline - (int)ticks;
    int urgency = (gap <= 0) ? 1000 : 1000 / gap;

    // Normalised frequency ∈ [0,1000]
    int nfreq = (f->freq * 1000) / max_freq;

    // Normalised priority ∈ [0,1000]
    int nprio = (f->priority <= 0) ? 0 : (f->priority * 1000) / 3;

    // Base score (scaled ×1000 then divided back)
    int base = (500 * urgency + 300 * nfreq + 200 * nprio) / 1000;

    // IRI reuse bonus
    int reuse = 0;
    if (f->prev_last >= 0) {
      int iri = f->last - f->prev_last;
      if (iri > 0) {
        int ttl = (f->last + iri) - (int)ticks;
        if (ttl > -iri) {
          int r = (ttl <= 0) ? 1000 : 1000 / ttl;
          reuse = (r * nprio) / 1000;
        }
      }
    }
    int score = base + (350 * reuse) / 1000;

    if (score < min_score) {
      min_score = score;
      victim    = i;
    }
  }
  return victim;
}

// ── Core allocator ────────────────────────────────────────────────────────────

// Allocate one user page for (pt, va) with priority/deadline metadata.
// Evicts a victim frame if the user frame limit is reached.
// Returns physical address on success, 0 on failure.
void *
pagerep_alloc(pagetable_t pt, uint64 va, int priority, int deadline)
{
  acquire(&pagerep_lock);
  pagerep_faults++;

  if (frame_count >= max_user_frames) {
    // Choose victim
    int v = -1;
    switch (current_policy) {
    case POLICY_FIFO:  v = pick_fifo();  break;
    case POLICY_LRU:   v = pick_lru();   break;
    case POLICY_LFU:   v = pick_lfu();   break;
    default:           v = pick_dapra(); break;
    }
    if (v < 0) {
      release(&pagerep_lock);
      return 0;
    }

    // Snapshot victim info, then mark invalid BEFORE releasing lock.
    // This prevents pagerep_on_kfree (called via uvmunmap→kfree) from
    // double-decrementing frame_count.
    struct frame_info snap = frametable[v];
    frametable[v].valid    = 0;
    frame_count--;
    pagerep_evictions++;
    release(&pagerep_lock);

    // Unmap and free outside the lock to avoid lock-order inversion
    // (uvmunmap → kfree acquires kmem.lock).
    uvmunmap(snap.pt, snap.va, 1, 1);  // do_free=1 frees the physical page
  } else {
    release(&pagerep_lock);
  }

  // Allocate a fresh physical page (outside pagerep_lock)
  void *mem = kalloc();
  if (!mem)
    return 0;

  // Register in frame table
  int idx = (int)PA2IDX((uint64)mem);
  acquire(&pagerep_lock);
  frametable[idx].pt        = pt;
  frametable[idx].va        = va;
  frametable[idx].priority  = priority;
  frametable[idx].deadline  = deadline;
  frametable[idx].freq      = 1;
  frametable[idx].last      = (int)ticks;
  frametable[idx].prev_last = -1;
  frametable[idx].load_tick = (int)ticks;
  frametable[idx].valid     = 1;
  frame_count++;
  release(&pagerep_lock);

  return mem;
}

// ── Tick handler: PTE_A scanning + priority aging ────────────────────────────
//
// Called every clock tick from clockintr() (trap.c).
// Two jobs:
//   1. Scan every running process's page table for the hardware-set PTE_A bit.
//      If set → the page was accessed since the last tick.
//      Update prev_last/last/freq in the frame table, then CLEAR PTE_A so we
//      can detect accesses in the NEXT tick.
//   2. Every AGING_INTERVAL ticks, decay BG and INT frame frequencies so
//      stale background pages lose protection and RT pages dominate.

#define AGING_INTERVAL 8

// walk one level of a page table looking for leaf PTEs with PTE_A set.
// This mirrors xv6's walk() but scans the whole tree.
static void
scan_pagetable(pagetable_t pt, int level, uint64 base_va,
               int prio, int deadline)
{
  for (int i = 0; i < 512; i++) {
    pte_t pte = pt[i];
    if (!(pte & PTE_V))
      continue;
    uint64 va = base_va | ((uint64)i << (12 + 9 * level));
    if (level > 0) {
      // internal node → recurse
      pagetable_t child = (pagetable_t)PTE2PA(pte);
      scan_pagetable(child, level - 1, va, prio, deadline);
    } else {
      // leaf PTE
      if (pte & PTE_A) {
        // Hardware flagged this page as accessed — update frame table
        uint64 pa = PTE2PA(pte);
        if (pa >= KERNBASE && pa < PHYSTOP) {
          int idx = (int)PA2IDX(pa);
          if (idx >= 0 && idx < NFRAMES && frametable[idx].valid) {
            frametable[idx].prev_last = frametable[idx].last;
            frametable[idx].last      = (int)ticks;
            frametable[idx].freq++;
            // Refresh priority/deadline from current proc state
            frametable[idx].priority = prio;
            frametable[idx].deadline = deadline;
          }
        }
        // Clear PTE_A so next tick we detect fresh accesses
        pt[i] &= ~PTE_A;
      }
    }
  }
}

void
pagerep_tick(void)
{
  extern struct proc proc[];   // from proc.c

  acquire(&pagerep_lock);

  // 1. Scan all RUNNING/RUNNABLE processes for PTE_A bits
  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    // Only care about live user processes
    if (p->state != RUNNING && p->state != RUNNABLE && p->state != SLEEPING)
      continue;
    if (p->pagetable == 0)
      continue;
    // RISC-V Sv39: 3-level page table, top level = level 2
    scan_pagetable(p->pagetable, 2, 0, p->priority, p->deadline);
  }

  // 2. Priority aging every AGING_INTERVAL ticks
  if ((ticks % AGING_INTERVAL) == 0) {
    for (int i = 0; i < NFRAMES; i++) {
      if (!frametable[i].valid)
        continue;
      if (frametable[i].priority == 1)       // BG  → heavy decay ×0.60
        frametable[i].freq = (frametable[i].freq * 6) / 10;
      else if (frametable[i].priority == 2)  // INT → mild decay  ×0.80
        frametable[i].freq = (frametable[i].freq * 8) / 10;
      // RT (priority==3) → no decay
      if (frametable[i].freq < 1)
        frametable[i].freq = 1;  // never decay to zero (avoid /0 in nfreq)
    }
  }

  release(&pagerep_lock);
}

// ── Public helpers ────────────────────────────────────────────────────────────

void
pagerep_set_policy(int p)
{
  current_policy = p;
}

int
pagerep_get_policy(void)
{
  return current_policy;
}

void
pagerep_get_stats(uint64 *faults, uint64 *evictions)
{
  *faults    = pagerep_faults;
  *evictions = pagerep_evictions;
}

void
pagerep_reset_stats(void)
{
  pagerep_faults    = 0;
  pagerep_evictions = 0;
}
