
#include "smp.hpp"
#include "acpi.hpp"
#include "apic.hpp"
#include "apic_revenant.hpp"
#include "pit.hpp"
#include <os.hpp>
#include <kernel/events.hpp>
#include <kernel/threads.hpp>
#include <malloc.h>
#include <algorithm>
#include <cstring>
#include <thread>

extern "C" {
  extern char _binary_apic_boot_bin_start;
  extern char _binary_apic_boot_bin_end;
  extern void __apic_trampoline(); // 64-bit entry
}

static const uintptr_t BOOTLOADER_LOCATION = 0x10000;
static const uint32_t  REV_STACK_SIZE = 1 << 19; // 512kb
static_assert((BOOTLOADER_LOCATION & 0xfff) == 0, "Must be page-aligned");

struct apic_boot {
  // the jump instruction at the start
  uint32_t  padding;
  // stuff we will modify
  uint32_t  worker_addr;
  uint32_t  stack_base;
  uint32_t  stack_size;
};

namespace x86
{

void init_SMP()
{
  const uint32_t CPUcount = ACPI::get_cpus().size();
  if (CPUcount <= 1) return;
  assert(CPUcount <= SMP_MAX_CORES);
  // avoid heap usage during AP init
  x86::smp_main.initialized_cpus.reserve(CPUcount);

  // copy our bootloader to APIC init location
  const char* start = &_binary_apic_boot_bin_start;
  const ptrdiff_t bootl_size = &_binary_apic_boot_bin_end - start;
  memcpy((char*) BOOTLOADER_LOCATION, start, bootl_size);

  // allocate revenant main stacks
  void* stack = memalign(4096, CPUcount * REV_STACK_SIZE);
  smp_main.stack_base = (uintptr_t) stack;
  smp_main.stack_size = REV_STACK_SIZE;

  // modify bootloader to support our cause
  auto* boot = (apic_boot*) BOOTLOADER_LOCATION;

#if defined(ARCH_i686)
  boot->worker_addr = (uint32_t) &revenant_main;
#elif defined(ARCH_x86_64)
  boot->worker_addr = (uint32_t) (uintptr_t) &__apic_trampoline;
#else
  #error "Unimplemented arch"
#endif
  boot->stack_base = (uint32_t) smp_main.stack_base;
  // add to start at top of each stack, remove to offset cpu 1 to idx 0
  boot->stack_base -= 16;
  boot->stack_size = smp_main.stack_size;
  debug("APIC stack base: %#x  size: %u   main size: %u\n",
      boot->stack_base, boot->stack_size, sizeof(boot->worker_addr));
  assert((boot->stack_base & 15) == 0);

  // reset barrier
  smp_main.boot_barrier.reset(1);

  auto& apic = x86::APIC::get();
  // massage musl to create a main thread for each AP
  for (const auto& cpu : ACPI::get_cpus())
  {
	  if (cpu.id == apic.get_id()) continue;
	  //printf("Creating main thread for CPU %d\n", cpu.id);
	  // thread should immediately yield
	  auto* t = new std::thread(&revenant_thread_main, cpu.id);
	  const long tid = kernel::get_last_thread_id();
	  //printf("Back at the main thread, last thread: %ld\n", tid);
	  // store thread info in SMP structure
	  auto& system = smp_system.at(cpu.id);
	  system.main_thread = t;
	  system.main_thread_id = tid;
  }

  // turn on CPUs
  INFO("SMP", "Initializing APs");
  for (const auto& cpu : ACPI::get_cpus())
  {
    if (cpu.id == apic.get_id()) continue;
    debug("-> CPU %u ID %u  fl 0x%x\n",
          cpu.cpu, cpu.id, cpu.flags);
    apic.ap_init(cpu.id);
  }
  //PIT::blocking_cycles(10);

  // start CPUs
  INFO("SMP", "Starting APs");
  for (const auto& cpu : ACPI::get_cpus())
  {
    if (cpu.id == apic.get_id()) continue;
    // Send SIPI with start page at BOOTLOADER_LOCATION
    apic.ap_start(cpu.id, BOOTLOADER_LOCATION >> 12);
    apic.ap_start(cpu.id, BOOTLOADER_LOCATION >> 12);
  }
  //PIT::blocking_cycles(1);

  // wait for all APs to start
  smp_main.boot_barrier.spin_wait(CPUcount);
  INFO("SMP", "All %u APs are online now\n", CPUcount);

  // subscribe to IPIs
  Events::get().subscribe(BSP_LAPIC_IPI_IRQ,
  [] {
    int next = smp_main.bitmap.first_set();
    while (next != -1)
    {
      // remove bit
      smp_main.bitmap.atomic_reset(next);
      // get jobs from other CPU
      std::vector<smp_done_func> done;
      lock(smp_system[next].flock);
      smp_system[next].completed.swap(done);
      unlock(smp_system[next].flock);

      // execute all tasks
      for (auto& func : done) func();

      // get next set bit
      next = smp_main.bitmap.first_set();
    }
  });
}

} // x86

using namespace x86;

/// implementation of the SMP interface ///
int SMP::cpu_id() noexcept
{
#ifdef INCLUDEOS_SMP_ENABLE
  int cpuid;
#ifdef ARCH_x86_64
  asm("movl %%gs:(0x0), %0" : "=r" (cpuid));
#elif defined(ARCH_i686)
  asm("movl %%fs:(0x0), %0" : "=r" (cpuid));
#else
  #error "Implement me?"
#endif
  return cpuid;
#else
  return 0;
#endif
}

int SMP::cpu_count() noexcept {
  return x86::smp_main.initialized_cpus.size();
}
const std::vector<int>& SMP::active_cpus() {
  return x86::smp_main.initialized_cpus;
}

__attribute__((weak))
void SMP::init_task()
{
  /* do nothing */
}

void SMP::add_task(smp_task_func task, smp_done_func done, int cpu)
{
#ifdef INCLUDEOS_SMP_ENABLE
  lock(smp_system[cpu].tlock);
  smp_system[cpu].tasks.emplace_back(std::move(task), std::move(done));
  unlock(smp_system[cpu].tlock);
#else
  assert(cpu == 0);
  task(); done();
#endif
}
void SMP::add_task(smp_task_func task, int cpu)
{
#ifdef INCLUDEOS_SMP_ENABLE
  lock(smp_system[cpu].tlock);
  smp_system[cpu].tasks.emplace_back(std::move(task), nullptr);
  unlock(smp_system[cpu].tlock);
#else
  assert(cpu == 0);
  task();
#endif
}
void SMP::add_bsp_task(smp_done_func task)
{
#ifdef INCLUDEOS_SMP_ENABLE
  // queue job
  auto& system = PER_CPU(smp_system);
  lock(system.flock);
  system.completed.push_back(std::move(task));
  unlock(system.flock);
  // set this CPU bit
  smp_main.bitmap.atomic_set(SMP::cpu_id());
  // call home
  x86::APIC::get().send_bsp_intr();
#else
  task();
#endif
}

void SMP::signal(int cpu)
{
#ifdef INCLUDEOS_SMP_ENABLE
  // broadcast that there is work to do
  // 0: Broadcast to everyone except BSP
  if (cpu == 0)
      x86::APIC::get().bcast_ipi(0x20);
  // 1-xx: Unicast specific vCPU
  else
      x86::APIC::get().send_ipi(cpu, 0x20);
#else
  (void) cpu;
#endif
}
void SMP::signal_bsp()
{
  x86::APIC::get().send_bsp_intr();
}

void SMP::broadcast(uint8_t irq)
{
  x86::APIC::get().bcast_ipi(IRQ_BASE + irq);
}
void SMP::unicast(int cpu, uint8_t irq)
{
  x86::APIC::get().send_ipi(cpu, IRQ_BASE + irq);
}

static spinlock_t __global_lock = 0;

void SMP::global_lock() noexcept
{
  lock(__global_lock);
}
void SMP::global_unlock() noexcept
{
  unlock(__global_lock);
}