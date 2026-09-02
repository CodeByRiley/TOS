/* kernel/loader/process.c , process create / exec / spawn.
 *
 * Orchestrates the steps to launch a user-mode task:
 *   1. process_pml4_create()    , fresh PML4 sharing kernel-low identity
 *   2. elf_load()               , load the ELF into the PML4
 *   3. user_stack_alloc_in()    , populate the user stack pages
 *   4. argv marshal             , copy argv strings and Linux-style
 *                                  argc/argv/envp/auxv terminators onto the
 *                                  user stack
 *   5. task activation          , publish the reserved task as runnable
 *
 * process_exec blocks until the child exits and returns its code.
 * process_spawn_async snapshots the request, reserves the child's final pid,
 * and queues the expensive image work on the BSP loader task before returning.
 */
#include <display/tty.h>
#include <fs/stdio.h>
#include <loader/elf.h>
#include <loader/pe.h>
#include <loader/process.h>
#include <memory/heap.h>
#include <memory/hhdm.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <sched/sched.h>
#include <stdint.h>
#include <utilities/log.h>
#include <utilities/string.h>

#define ARGV_MAX 16
#define ARG_LEN_MAX 128

/* Page-table field accessors live in vmm.h, where the entry layout is
 * documented. Local copies of these masks used to sit here and in three other
 * files. */
#define ADDR_MASK VMM_ADDR_MASK
#define PAGE_PS VMM_PS

#ifndef PROCESS_PML4_HOST_TEST
extern u64 *kernel_pml4;
#endif

/* Walk one PDPT subtree and free every present user frame underneath, plus
 * each level's table. `start_idx` skips entries (used to leave the kernel-low
 * identity map intact under PML4[0]'s PDPT[0]). Always frees the PDPT frame
 * itself when done. The previous version copy-pasted this loop twice; one
 * day someone would have "fixed" one half and not the other. */
static void free_pdpt_subtree(u64 pdpt_phys, int start_idx) {
  u64 *pdpt = phys_to_virt(pdpt_phys);
  for (int i = start_idx; i < 512; i++) {
    u64 pdpt_e = pdpt[i];
    if (!(pdpt_e & VMM_PRESENT) || (pdpt_e & PAGE_PS))
      continue;
    u64 pd_phys = pdpt_e & ADDR_MASK;
    u64 *pd = phys_to_virt(pd_phys);
    for (int j = 0; j < 512; j++) {
      u64 pd_e = pd[j];
      if (!(pd_e & VMM_PRESENT) || (pd_e & PAGE_PS))
        continue;
      u64 pt_phys = pd_e & ADDR_MASK;
      u64 *pt = phys_to_virt(pt_phys);
      for (int k = 0; k < 512; k++) {
        u64 pte = pt[k];
        if (!(pte & VMM_PRESENT))
          continue;
        /* Skip borrowed frames. Shared-memory receivers and direct
         * framebuffer clients do not own these pages; freeing one
         * here lets the PMM reuse a frame while its owner still
         * writes to it. */
        if (pte & VMM_SHARED)
          continue;
        pmm_free_frame(pte & ADDR_MASK);
      }
      pmm_free_frame(pt_phys);
    }
    pmm_free_frame(pd_phys);
  }
  pmm_free_frame(pdpt_phys);
}

/* Walk a process PML4 and free every page it uses (page tables + user
 * frames in the lower half + the PML4 frame itself). PDPT[0] under PML4[0]
 * is the kernel-shared low 1 GiB identity map: skip walking that subtree
 * but still free the per-process PDPT frame. PML4[256..511] is also shared
 * (kernel high half) so leave those alone. Called from sched.c::task_reap. */
void free_user_pml4(u64 *pml4) {
  if (!pml4)
    return;

  u64 pml4_0 = pml4[0];
  if (pml4_0 & VMM_PRESENT) {
    free_pdpt_subtree(pml4_0 & ADDR_MASK, /*start_idx=*/1);
  }

  for (int p = 1; p < 256; p++) {
    u64 pml4_e = pml4[p];
    if (!(pml4_e & VMM_PRESENT))
      continue;
    free_pdpt_subtree(pml4_e & ADDR_MASK, /*start_idx=*/0);
  }

  pmm_free_frame(virt_to_phys(pml4));
}

#ifndef PROCESS_PML4_HOST_TEST
int user_stack_alloc(void) { return user_stack_alloc_in(kernel_pml4); }

static void user_stack_rollback(u64 *pml4, int mapped_pages) {
  for (int i = 0; i < mapped_pages; i++) {
    u64 va = USER_STACK_TOP - (u64)(i + 1) * 4096;
    u64 phys = vmm_translate_in(pml4, va);
    if (!phys)
      continue;
    vmm_unmap_in(pml4, va);
    pmm_free_frame(phys & ADDR_MASK);
  }
}

/* The stack is data. VMM_NX keeps an overflow that lands a payload here
 * from being executable. Requires EFER.NXE, set by enable_paging on the
 * BSP and by the AP trampoline on every other core. */
int user_stack_alloc_in(u64 *pml4) {
  for (int i = 0; i < USER_STACK_PAGES; i++) {
    u64 phys = pmm_alloc_frame();
    if (!phys) {
      log_write("process: user stack frame allocation failed", KERNEL,
                LOG_ERROR);
      user_stack_rollback(pml4, i);
      return -1;
    }
    memset(phys_to_virt(phys), 0, 4096);
    if (vmm_map_in(pml4, USER_STACK_TOP - (i + 1) * 4096, phys,
                   VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX) != 0) {
      pmm_free_frame(phys);
      log_write("process: user stack mapping failed", KERNEL, LOG_ERROR);
      user_stack_rollback(pml4, i);
      return -1;
    }
    if ((i & 31) == 31)
      task_yield();
  }
  return 0;
}

/* Write into a child address space without activating its PML4. */
static int copy_to_user_pml4(u64 *pml4, u64 user_dst, const void *src,
                             usize length) {
  const u8 *input = (const u8 *)src;
  while (length > 0) {
    u64 phys = vmm_translate_in(pml4, user_dst);
    if (!phys)
      return -1;

    usize chunk = 4096 - (usize)(user_dst & 0xFFF);
    if (chunk > length)
      chunk = length;
    memcpy(phys_to_virt(phys), input, chunk);
    user_dst += chunk;
    input += chunk;
    length -= chunk;
  }
  return 0;
}

u64 *process_pml4_create(void) {
  u64 pml4_phys = pmm_alloc_frame();
  if (!pml4_phys)
    return 0;
  u64 *pml4 = phys_to_virt(pml4_phys);
  memset(pml4, 0, 4096);

  /* Each process gets a private PDPT under PML4[0]. PDPT[0] is copied
   * from the kernel's PML4[0]'s PDPT[0] so the kernel-low identity map
   * (first 1 GiB) is shared. PDPT[1..511] starts empty for private
   * user mappings. */
  u64 pdpt_phys = pmm_alloc_frame();
  if (!pdpt_phys) {
    pmm_free_frame(pml4_phys);
    return 0;
  }
  u64 *pdpt = phys_to_virt(pdpt_phys);
  memset(pdpt, 0, 4096);

  u64 kernel_pml4_0 = kernel_pml4[0];
  if (kernel_pml4_0 & 1) {
    u64 *kernel_pdpt = phys_to_virt(kernel_pml4_0 & ADDR_MASK);
    pdpt[0] = kernel_pdpt[0];
  }

  pml4[0] = pdpt_phys | VMM_PRESENT | VMM_WRITE | VMM_USER;

  /* Share the kernel-half entries by physical address, so mappings made
   * beneath them later are visible here too. vmm_init pre-creates every
   * one of these slots, which is what makes the copy safe: no kernel
   * mapping can introduce a *new* top-level entry after this point and
   * leave already-spawned processes behind. */
  for (int i = 256; i < 512; i++) {
    pml4[i] = kernel_pml4[i];
  }

  return pml4;
}

struct spawn_request {
  struct task *reserved;
  struct spawn_request *next;
  int argc;
  int cancelled;
  long cancel_code;
  char path[ARG_LEN_MAX];
  char args[ARGV_MAX][ARG_LEN_MAX];
};

struct loaded_image {
  u64 *pml4;
  u64 entry;
  u64 user_rsp;
};

static struct spawn_request *load_head;
static struct spawn_request *load_tail;
static struct spawn_request *load_active;
static struct task *loader_task;

static struct spawn_request *snapshot_request(const char *path,
                                              char *const argv[]) {
  if (!path || !path[0])
    return 0;

  struct spawn_request *req = (struct spawn_request *)kmalloc(sizeof(*req));
  if (!req)
    return 0;
  memset(req, 0, sizeof(*req));

  usize n = 0;
  while (n < sizeof(req->path) - 1 && path[n]) {
    req->path[n] = path[n];
    n++;
  }
  req->path[n] = 0;

  while (argv && req->argc < ARGV_MAX && argv[req->argc]) {
    const char *src = argv[req->argc];
    usize len = 0;
    while (len < ARG_LEN_MAX - 1 && src[len]) {
      req->args[req->argc][len] = src[len];
      len++;
    }
    req->args[req->argc][len] = 0;
    req->argc++;
  }
  return req;
}

static void set_process_name(struct task *task, const char *path) {
  const char *base = path;
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\')
      base = p + 1;
  }

  char name[16];
  usize n = 0;
  while (n < sizeof(name) - 1 && base[n] && base[n] != '.') {
    char c = base[n];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c + ('a' - 'A'));
    name[n++] = c;
  }
  name[n] = 0;
  task_set_name(task, name);
}

static int load_request_image(struct spawn_request *req,
                              struct loaded_image *image) {
  memset(image, 0, sizeof(*image));
  image->pml4 = process_pml4_create();
  if (!image->pml4) {
    log_write("process_spawn: pml4 alloc failed", KERNEL, LOG_ERROR);
    return -1;
  }

  u8 magic[16] = {0};
  FILE *sniff = fopen(req->path, "r");
  if (!sniff) {
    log_write("process_spawn: fopen failed", KERNEL, LOG_ERROR);
    goto fail;
  }
  usize read = fread(magic, 1, sizeof(magic), sniff);
  fclose(sniff);

  if (read == sizeof(magic) && magic[0] == 'M' && magic[1] == 'Z') {
    image->entry = pe_load(req->path, image->pml4);
    log_write_hex("process_spawn: using pe entry =", image->entry, KERNEL,
                  LOG_INFO);
  } else if (read == sizeof(magic) && magic[0] == 0x7f && magic[1] == 'E' &&
             magic[2] == 'L' && magic[3] == 'F') {
    image->entry = elf_load(req->path, image->pml4);
    log_write_hex("process_spawn: using elf entry =", image->entry, KERNEL,
                  LOG_INFO);
  } else {
    log_write("process_spawn: unknown or truncated executable", KERNEL,
              LOG_ERROR);
  }
  if (!image->entry || req->cancelled)
    goto fail;

  if (user_stack_alloc_in(image->pml4) != 0 || req->cancelled)
    goto fail;

  image->user_rsp = USER_STACK_TOP;
  u64 arg_ptrs[ARGV_MAX];
  for (int i = 0; i < req->argc; i++) {
    usize len = strlen(req->args[i]) + 1;
    image->user_rsp -= len;
    if (copy_to_user_pml4(image->pml4, image->user_rsp, req->args[i], len) != 0)
      goto fail;
    arg_ptrs[i] = image->user_rsp;
  }
  image->user_rsp &= ~0xFULL;

  /* Initial userspace stack:
   *   argc
   *   argv[0..argc-1]
   *   NULL            argv terminator
   *   NULL            empty envp terminator
   *   AT_NULL, 0      empty auxiliary vector
   *
   * The original TOS crt0 only consumes argc/argv, but musl's crt1 scans
   * envp and auxv before calling main. Supplying explicit terminators keeps
   * both startup paths on the same ABI. */
  u64 initial_stack[ARGV_MAX + 5];
  initial_stack[0] = (u64)req->argc;
  for (int i = 0; i < req->argc; i++)
    initial_stack[i + 1] = arg_ptrs[i];
  initial_stack[req->argc + 1] = 0;
  initial_stack[req->argc + 2] = 0;
  initial_stack[req->argc + 3] = 0;
  initial_stack[req->argc + 4] = 0;

  usize initial_bytes = (usize)(req->argc + 5) * sizeof(u64);
  if (initial_bytes & 0xF)
    image->user_rsp -= sizeof(u64);
  image->user_rsp -= initial_bytes;
  if (copy_to_user_pml4(image->pml4, image->user_rsp, initial_stack,
                        initial_bytes) != 0)
    goto fail;
  return 0;

fail:
  free_user_pml4(image->pml4);
  memset(image, 0, sizeof(*image));
  return -1;
}

static int spawn_request_now(struct spawn_request *req) {
  struct loaded_image image;
  if (load_request_image(req, &image) != 0)
    return -1;

  struct task *parent = task_current();
  struct task *child = task_spawn_user(image.pml4, image.entry, image.user_rsp,
                                       parent ? parent->pid : 0);
  if (!child) {
    free_user_pml4(image.pml4);
    return -1;
  }
  set_process_name(child, req->path);
  return child->pid;
}

static void loader_worker(void) {
  for (;;) {
    struct spawn_request *req = load_head;
    if (!req) {
      task_sleep_ticks(1);
      continue;
    }

    load_head = req->next;
    if (!load_head)
      load_tail = 0;
    req->next = 0;
    load_active = req;

    struct loaded_image image;
    int loaded = load_request_image(req, &image);
    if (req->cancelled) {
      if (loaded == 0)
        free_user_pml4(image.pml4);
      task_fail_reserved_user(req->reserved, req->cancel_code);
    } else if (loaded != 0) {
      task_fail_reserved_user(req->reserved, -1);
    } else if (task_activate_reserved_user(req->reserved, image.pml4,
                                           image.entry, image.user_rsp) != 0) {
      free_user_pml4(image.pml4);
      task_fail_reserved_user(req->reserved, -1);
    } else {
      log_write_hex("process_spawn: ready pid =", (u64)req->reserved->pid, USER,
                    LOG_INFO);
    }

    load_active = 0;
    kfree(req);
    task_yield();
  }
}

long process_exec(const char *path, char *const argv[]) {
  struct spawn_request *req = snapshot_request(path, argv);
  if (!req)
    return -1;
  int child_pid = spawn_request_now(req);
  kfree(req);
  if (child_pid < 0)
    return -1;
  log_write_hex("process_exec: created process id", (u64)child_pid, USER,
                LOG_INFO);

  task_block(child_pid);

  // resumed by child's task_exit. Pull the exit code, reap, return.
  struct task *t = task_find(child_pid);
  long code = -1;
  if (t) {
    code = t->exit_code;
    task_reap(t);
  }
  return code;
}

/* tty < 0 means "inherit the caller's", which is what plain spawn wants. */
static long spawn_async_on_tty(const char *path, char *const argv[], int tty) {
  struct spawn_request *req = snapshot_request(path, argv);
  if (!req)
    return -1;

  if (!loader_task) {
    loader_task = task_spawn(loader_worker);
    if (!loader_task) {
      kfree(req);
      return -1;
    }
    task_set_name(loader_task, "loader");
  }

  struct task *parent = task_current();
  req->reserved = task_reserve_user(parent ? parent->pid : 0);
  if (!req->reserved) {
    kfree(req);
    return -1;
  }
  set_process_name(req->reserved, req->path);

  /* task_reserve_user already inherited the caller's channel; override it
   * here, while the task is still TASK_LOADING and nothing has run on it. */
  if (tty >= 0)
    req->reserved->tty = tty;

  if (load_tail)
    load_tail->next = req;
  else
    load_head = req;
  load_tail = req;
  log_write_hex("process_spawn: queued pid =", (u64)req->reserved->pid, USER,
                LOG_INFO);
  return req->reserved->pid;
}

long process_spawn_async(const char *path, char *const argv[]) {
  return spawn_async_on_tty(path, argv, -1);
}

long process_spawn_async_tty(const char *path, char *const argv[], int tty) {
  if (tty < 0 || tty >= TTY_MAX || !tty_is_open(tty))
    return -1;
  return spawn_async_on_tty(path, argv, tty);
}

int process_cancel_async(int pid, long code) {
  if (load_active && load_active->reserved &&
      load_active->reserved->pid == pid) {
    load_active->cancelled = 1;
    load_active->cancel_code = code;
    return 0;
  }

  struct spawn_request *prev = 0;
  for (struct spawn_request *req = load_head; req; req = req->next) {
    if (!req->reserved || req->reserved->pid != pid) {
      prev = req;
      continue;
    }
    if (prev)
      prev->next = req->next;
    else
      load_head = req->next;
    if (load_tail == req)
      load_tail = prev;
    task_fail_reserved_user(req->reserved, code);
    kfree(req);
    return 0;
  }
  return -1;
}

long process_kill(long pid, int signal) {
  if (pid <= 0 || signal <= 0 || signal > 64)
    return -1;
  return task_kill((int)pid, 128 + signal);
}
#endif
