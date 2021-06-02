#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  uint sp;
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_PGFLT:;
    uint offending_addr = PGROUNDUP(rcr2());
    cprintf("stack top proc %d\n", myproc()->st);
    cprintf("offending address %d\n", offending_addr);
    //determine how many pages in user stack and where page guard begins
    cprintf("Number of pages %d\n", myproc()->num_user_pages);
    uint page_guard_start = myproc()->st - myproc()->num_user_pages*PGSIZE;
    uint page_guard_end = page_guard_start - PGSIZE;
    cprintf("Page guard start %d\n", page_guard_start);//start
    cprintf("Page guard end %d\n", page_guard_end);//end
    if(offending_addr < page_guard_start && offending_addr > page_guard_end){//STACKTOP to STACKTOP - 1*PGSIZE is he userstack, STACKTOP -1*PGSIZE to STACKTOP-2*PGSIZE is the page guard
      cprintf("Increased stack size\n");
      //call allocuvm to allocate another page below the old page guard
      //if succeded set PTE_U flag for old page guard, and call clearpteu on new allocated page
      if((sp = allocuvm(myproc()->pgdir, page_guard_end, page_guard_end - PGSIZE)) == 0)
        panic("Panic allocuvm failed to allocate");
      setpteu(myproc()->pgdir, (char*)(page_guard_end)); //set pte_u for old page guard
      clearpteu(myproc()->pgdir, (char*)(page_guard_end - PGSIZE)); //clear pte_u for new page guard
      myproc()->num_user_pages++;
      // myproc()->sz = myproc()->sz + 4096;
      // myproc()->tf->esp = sp;
    }
    break;
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
