# Assignment 7: Analysis of the `faulty` driver kernel oops

## How it was triggered

On the Buildroot QEMU image (aarch64, Linux 6.1.44), with the `faulty` module loaded by `/etc/init.d/S98lddmodules start`:

    echo "hello_world" > /dev/faulty

The write system call reached `faulty_write` in the `faulty` module, which dereferences a NULL pointer. The kernel printed the oops below (captured with `dmesg`) and killed the writing process, which was the login shell, so the console returned to the login prompt. The kernel itself kept running.

## Kernel oops output

```
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Mem abort info:
  ESR = 0x0000000096000045
  EC = 0x25: DABT (current EL), IL = 32 bits
  SET = 0, FnV = 0
  EA = 0, S1PTW = 0
  FSC = 0x05: level 1 translation fault
Data abort info:
  ISV = 0, ISS = 0x00000045
  CM = 0, WnR = 1
user pgtable: 4k pages, 39-bit VAs, pgdp=0000000041c14000
[0000000000000000] pgd=0000000000000000, p4d=0000000000000000, pud=0000000000000000
Internal error: Oops: 0000000096000045 [#1] SMP
Modules linked in: faulty(O) scull(O) hello(O) [last unloaded: hello(O)]
CPU: 0 PID: 136 Comm: sh Tainted: G           O       6.1.44 #1
Hardware name: linux,dummy-virt (DT)
pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
pc : faulty_write+0x10/0x20 [faulty]
lr : vfs_write+0xc8/0x390
sp : ffffffc008dfbd20
x29: ffffffc008dfbd80 x28: ffffff8001ba5cc0 x27: 0000000000000000
x26: 0000000000000000 x25: 0000000000000000 x24: 0000000000000000
x23: 000000000000000c x22: 000000000000000c x21: ffffffc008dfbdc0
x20: 000000555bb00ba0 x19: ffffff8001aedd00 x18: 0000000000000000
x17: 0000000000000000 x16: 0000000000000000 x15: 0000000000000000
x14: 0000000000000000 x13: 0000000000000000 x12: 0000000000000000
x11: 0000000000000000 x10: 0000000000000000 x9 : 0000000000000000
x8 : 0000000000000000 x7 : 0000000000000000 x6 : 0000000000000000
x5 : 0000000000000001 x4 : ffffffc000790000 x3 : ffffffc008dfbdc0
x2 : 000000000000000c x1 : 0000000000000000 x0 : 0000000000000000
Call trace:
 faulty_write+0x10/0x20 [faulty]
 ksys_write+0x74/0x110
 __arm64_sys_write+0x1c/0x30
 invoke_syscall+0x54/0x130
 el0_svc_common.constprop.0+0x44/0xf0
 do_el0_svc+0x2c/0xc0
 el0_svc+0x2c/0x90
 el0t_64_sync_handler+0xf4/0x120
 el0t_64_sync+0x18c/0x190
Code: d2800001 d2800000 d503233f d50323bf (b900003f) 
---[ end trace 0000000000000000 ]---
```

## Analysis

**What went wrong.** The first line says it directly: `Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000`. Code running in the kernel tried to access address 0, which is never mapped. `Internal error: Oops: 0000000096000045 [#1] SMP` is the oops itself, and `[#1]` means it is the first oops since boot.

**Decoding the fault.** The `ESR` value (`0x96000045`) explains the kind of fault:

- `EC = 0x25`: a data abort taken in the current exception level, so the fault came from kernel code, not from user space.
- `WnR = 1`: the access was a write.
- `FSC = 0x05`: a level 1 translation fault, meaning the address had no page table mapping.

So this is a kernel write to the unmapped address 0.

**Where it happened.** The most useful line is:

    pc : faulty_write+0x10/0x20 [faulty]

`pc` is the program counter, the instruction that faulted. It reads as: in the module `faulty`, in function `faulty_write`, at byte offset `0x10` into a function that is `0x20` bytes long. `lr : vfs_write+0xc8/0x390` is the link register, the return address, showing that `faulty_write` was called from the kernel's VFS write path. The `Call trace:` lists the same path from the fault outwards: `faulty_write` called by `ksys_write`, called by `__arm64_sys_write`, then the system call entry code (`invoke_syscall`, `el0_svc_common`, `do_el0_svc`, `el0_svc`, `el0t_64_sync_handler`, `el0t_64_sync`). In other words, a user process called `write()` on `/dev/faulty` and the system call went straight into the driver.

**Which process.** `CPU: 0 PID: 136 Comm: sh` shows the process that made the system call was the shell running my `echo` command. `Tainted: G O` means an out-of-tree module (`O`) was loaded, which is expected here. `Modules linked in: faulty(O) scull(O) hello(O)` lists the loaded modules.

**The faulting instruction.** The last line before the end trace is `Code: d2800001 d2800000 d503233f d50323bf (b900003f)`. The value in parentheses is the instruction that faulted. `b900003f` decodes to `str wzr, [x1]`, a store of the zero register to the address in `x1`. The register dump shows `x1 : 0000000000000000`, which matches the fault address. So the driver is storing zero through a NULL pointer.

## How to find the faulty line

1. Read `pc : faulty_write+0x10/0x20 [faulty]` to learn the module (`faulty`), function (`faulty_write`) and offset (`0x10`).
2. Disassemble the module with the cross toolchain's objdump (the unstripped `faulty.ko` is in the Buildroot build directory) and look at `faulty_write`. The instruction at offset `0x10` should be the `str wzr, [x1]` seen in the `Code:` line. If the module is built with debug info, `objdump -dS` interleaves the source lines and shows the C statement directly.
3. Open `misc-modules/faulty.c` and find `faulty_write`. The statement that stores zero through a pointer to address 0 is the faulty line:

        *(int *)0 = 0;

   This is the deliberate bug in the ldd3 `faulty` example: a write through a NULL pointer.
4. The call trace and `lr` confirm the path that reached it, and `x1 = 0` in the register dump confirms the pointer was NULL.

## Disassembly of `faulty_write`

```
0000000000000000 <faulty_write>:
   0:	d2800001 	mov	x1, #0x0                   	// #0
   4:	d2800000 	mov	x0, #0x0                   	// #0
   8:	d503233f 	paciasp
   c:	d50323bf 	autiasp
  10:	b900003f 	str	wzr, [x1]
  14:	d65f03c0 	ret
  18:	d503201f 	nop
  1c:	d503201f 	nop

```

Output of `aarch64-buildroot-linux-gnu-objdump -d faulty.ko`. The instruction at offset `0x10` is the faulting one: `str wzr, [x1]`. It matches both the `pc : faulty_write+0x10` line and the `Code:` line of the oops. The first instruction sets `x1` to 0 and the store writes zero through it, which is the C statement `*(int *)0 = 0;`. The function is `0x20` bytes long, matching the `/0x20` in the oops. The `mov x0, #0x0` sets the return value to 0, which is never reached because the store faults first.
