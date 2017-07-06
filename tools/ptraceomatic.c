// Fun little utility that single-steps a program using ptrace and
// simultaneously runs the program in ish, and asserts that everything's
// working the same.
// Many apologies for the messy code.
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#undef PAGE_SIZE // defined in sys/user.h, but we want the version from emu/memory.h
#include <sys/personality.h>
#include <sys/socket.h>

#include "sys/calls.h"
#include "emu/interrupt.h"
#include "emu/cpuid.h"

#include "sys/elf.h"
#include "tools/transplant.h"
#include "tools/ptutil.h"
#include "undefined-flags.h"
#include "libvdso.so.h"

// ptrace utility functions

// returns 1 for a signal stop
static inline int step(int pid) {
    trycall(ptrace(PTRACE_SINGLESTEP, pid, NULL, 0), "ptrace step");
    int status;
    trycall(waitpid(pid, &status, 0), "wait step");
    if (WIFSTOPPED(status) && WSTOPSIG(status) != SIGTRAP) {
        int signal = WSTOPSIG(status);
        // a signal arrived, we now have to actually deliver it
        trycall(ptrace(PTRACE_SINGLESTEP, pid, NULL, signal), "ptrace step");
        trycall(waitpid(pid, &status, 0), "wait step");
        return 1;
    }
    return 0;
}

static inline void getregs(int pid, struct user_regs_struct *regs) {
    trycall(ptrace(PTRACE_GETREGS, pid, NULL, regs), "ptrace getregs");
}

static inline void setregs(int pid, struct user_regs_struct *regs) {
    trycall(ptrace(PTRACE_SETREGS, pid, NULL, regs), "ptrace setregs");
}

static int compare_cpus(struct cpu_state *cpu, int pid, int undefined_flags) {
    struct user_regs_struct regs;
    struct user_fpregs_struct fpregs;
    getregs(pid, &regs);
    trycall(ptrace(PTRACE_GETFPREGS, pid, NULL, &fpregs), "ptrace getregs compare");
    collapse_flags(cpu);
#define CHECK_REG(pt, cp) \
    if (regs.pt != cpu->cp) { \
        printf(#pt " = 0x%llx, " #cp " = 0x%x\n", regs.pt, cpu->cp); \
        debugger; \
        return -1; \
    }
#define CHECK_FPREG(pt, cp) \
    if (fpregs.pt != cpu->cp) { \
        printf(#pt " = 0x%x, " #cp " = 0x%x\n", fpregs.pt, cpu->cp); \
        debugger; \
        return -1; \
    }
    CHECK_REG(rax, eax);
    CHECK_REG(rbx, ebx);
    CHECK_REG(rcx, ecx);
    CHECK_REG(rdx, edx);
    CHECK_REG(rsi, esi);
    CHECK_REG(rdi, edi);
    CHECK_REG(rsp, esp);
    CHECK_REG(rbp, ebp);
    CHECK_REG(rip, eip);
    undefined_flags |= (1 << 8); // treat trap flag as undefined
    regs.eflags = (regs.eflags & ~undefined_flags) | (cpu->eflags & undefined_flags);
    // give a nice visual representation of the flags
    if (regs.eflags != cpu->eflags) {
#define f(x,n) ((regs.eflags & (1 << n)) ? #x : "-"),
        printf("real eflags = 0x%llx %s%s%s%s%s%s%s%s%s, fake eflags = 0x%x %s%s%s%s%s%s%s%s%s\n%0d",
                regs.eflags, f(o,11)f(d,10)f(i,9)f(t,8)f(s,7)f(z,6)f(a,4)f(p,2)f(c,0)
#undef f
#define f(x,n) ((cpu->eflags & (1 << n)) ? #x : "-"),
                cpu->eflags, f(o,11)f(d,10)f(i,9)f(t,8)f(s,7)f(z,6)f(a,4)f(p,2)f(c,0)0);
        debugger;
        return -1;
    }
    CHECK_FPREG(xmm_space[0], xmm[0].dw[0]);
    CHECK_FPREG(xmm_space[1], xmm[0].dw[1]);
    CHECK_FPREG(xmm_space[2], xmm[0].dw[2]);
    CHECK_FPREG(xmm_space[3], xmm[0].dw[3]);
    CHECK_FPREG(xmm_space[4], xmm[1].dw[0]);
    CHECK_FPREG(xmm_space[5], xmm[1].dw[1]);
    CHECK_FPREG(xmm_space[6], xmm[1].dw[2]);
    CHECK_FPREG(xmm_space[7], xmm[1].dw[3]);
    CHECK_FPREG(xmm_space[8], xmm[2].dw[0]);
    CHECK_FPREG(xmm_space[9], xmm[2].dw[1]);
    CHECK_FPREG(xmm_space[10], xmm[2].dw[2]);
    CHECK_FPREG(xmm_space[11], xmm[2].dw[3]);
    CHECK_FPREG(xmm_space[12], xmm[3].dw[0]);
    CHECK_FPREG(xmm_space[13], xmm[3].dw[1]);
    CHECK_FPREG(xmm_space[14], xmm[3].dw[2]);
    CHECK_FPREG(xmm_space[15], xmm[3].dw[3]);
    // 4 xmm regs is enough for now

    // compare pages marked dirty
    int fd = open_mem(pid);
    page_t dirty_page = cpu->mem.dirty_page;
    char real_page[PAGE_SIZE];
    trycall(lseek(fd, dirty_page, SEEK_SET), "compare seek mem");
    trycall(read(fd, real_page, PAGE_SIZE), "compare read mem");
    void *fake_page = cpu->mem.pt[PAGE(dirty_page)]->data;

    if (memcmp(real_page, fake_page, PAGE_SIZE) != 0) {
        printf("page %x doesn't match\n", dirty_page);
        debugger;
        return -1;
    }

    close(fd);
    setregs(pid, &regs);
    return 0;
}

// I'd like to apologize in advance for this code
static int transmit_fd(int pid, int sender, int receiver, int fake_fd) {
    // this sends the fd over a unix domain socket. yes, I'm crazy

    // sending part
    int real_fd = current->files[fake_fd]->real_fd;
    struct msghdr msg = {};
    char cmsg[CMSG_SPACE(sizeof(int))];
    memset(cmsg, 0, sizeof(cmsg));

    msg.msg_control = cmsg;
    msg.msg_controllen = sizeof(cmsg);

    struct cmsghdr *cmsg_hdr = CMSG_FIRSTHDR(&msg);
    cmsg_hdr->cmsg_level = SOL_SOCKET;
    cmsg_hdr->cmsg_type = SCM_RIGHTS;
    cmsg_hdr->cmsg_len = CMSG_LEN(sizeof(int));
    *(int *) CMSG_DATA(cmsg_hdr) = real_fd;

    trycall(sendmsg(sender, &msg, 0), "sendmsg insanity");

    // receiving part
    // painful, because we're 64-bit and the child is 32-bit and I want to kill myself
    struct user_regs_struct saved_regs;
    getregs(pid, &saved_regs);
    struct user_regs_struct regs = saved_regs;

    // reserve space for 32-bit version of cmsg
    regs.rsp -= 16; // according to my calculations
    addr_t cmsg_addr = regs.rsp;
    char cmsg_bak[16];
    pt_readn(pid, regs.rsp, cmsg_bak, sizeof(cmsg_bak));

    // copy 32-bit msghdr
    regs.rsp -= 32;
    int msg32[] = {0, 0, 0, 0, cmsg_addr, 20, 0};
    addr_t msg_addr = regs.rsp;
    char msg_bak[32];
    pt_readn(pid, regs.rsp, msg_bak, sizeof(msg_bak));
    pt_writen(pid, regs.rsp, &msg32, sizeof(msg32));

    regs.rax = 372;
    regs.rbx = receiver;
    regs.rcx = msg_addr;
    regs.rdx = 0;
    // assume we're already on an int $0x80
    setregs(pid, &regs);
    step(pid);
    getregs(pid, &regs);

    int sent_fd;
    if ((long) regs.rax >= 0)
        pt_readn(pid, cmsg_addr + 12, &sent_fd, sizeof(sent_fd));
    else
        sent_fd = regs.rax;

    // restore crap
    pt_writen(pid, cmsg_addr, cmsg_bak, sizeof(cmsg_bak));
    pt_writen(pid, msg_addr, msg_bak, sizeof(msg_bak));
    setregs(pid, &regs);

    if (sent_fd < 0) {
        errno = -sent_fd;
        perror("remote recvmsg insanity");
        exit(1);
    }

    return sent_fd;
}

static void remote_close_fd(int pid, int fd, long int80_ip) {
    // lettuce spray
    struct user_regs_struct saved_regs;
    getregs(pid, &saved_regs);
    struct user_regs_struct regs = saved_regs;
    regs.rip = int80_ip;
    regs.rax = 6;
    regs.rbx = fd;
    setregs(pid, &regs);
    step(pid);
    getregs(pid, &regs);
    if ((long) regs.rax < 0) {
        errno = -regs.rax;
        perror("remote close fd");
        exit(1);
    }
    setregs(pid, &regs);
}

static void pt_copy(int pid, addr_t start, size_t size) {
    for (addr_t addr = start; addr < start + size; addr++)
        pt_write8(pid, addr, user_get8(addr));
}

// Please don't use unless absolutely necessary.
static void pt_copy_to_real(int pid, addr_t start, size_t size) {
    byte_t byte;
    for (addr_t addr = start; addr < start + size; addr++) {
        pt_readn(pid, addr, &byte, sizeof(byte));
        user_put8(addr, byte);
    }
}

static void step_tracing(struct cpu_state *cpu, int pid, int sender, int receiver) {
    // step fake cpu
    int interrupt;
restart:
    cpu->tf = 1;
    interrupt = cpu_step32(cpu);
    if (interrupt != INT_NONE) {
        cpu->trapno = interrupt;
        // hack to clean up before the exit syscall
        if (interrupt == INT_SYSCALL && cpu->eax == 1) {
            if (kill(pid, SIGKILL) < 0) {
                perror("kill tracee during exit");
                exit(1);
            }
        }
        if (handle_interrupt(cpu, interrupt)) {
            goto restart;
        }
    }

    // step real cpu
    // intercept cpuid, rdtsc, and int $0x80, though
    struct user_regs_struct regs;
    errno = 0;
    getregs(pid, &regs);
    long inst = trycall(ptrace(PTRACE_PEEKTEXT, pid, regs.rip, NULL), "ptrace get inst step");
    long saved_fd = -1; // annoying hack for mmap
    long old_sp = regs.rsp; // so we know where a sigframe ends

    if ((inst & 0xff) == 0x0f) {
        if (((inst & 0xff00) >> 8) == 0xa2) {
            // cpuid
            do_cpuid((dword_t *) &regs.rax, (dword_t *) &regs.rbx, (dword_t *) &regs.rcx, (dword_t *) &regs.rdx);
            regs.rip += 2;
        } else if (((inst & 0xff00) >> 8) == 0x31) {
            // rdtsc, no good way to get the same result here except copy from fake cpu
            regs.rax = cpu->eax;
            regs.rdx = cpu->edx;
            regs.rip += 2;
        } else {
            goto do_step;
        }
    } else if ((inst & 0xff) == 0xcd && ((inst & 0xff00) >> 8) == 0x80) {
        // int $0x80, intercept the syscall unless it's one of a few actually important ones
        dword_t syscall_num = (dword_t) regs.rax;
        switch (syscall_num) {
            // put syscall result from fake process into real process
            case 3: // read
                pt_copy(pid, regs.rcx, cpu->edx); break;
            case 13: // time
                if (regs.rbx != 0)
                    pt_copy(pid, regs.rbx, sizeof(dword_t));
                break;
            case 54: { // ioctl (god help us)
                struct fd *fd = current->files[cpu->ebx];
                if (fd) {
                    ssize_t ioctl_size = fd->ops->ioctl_size(fd, cpu->ecx);
                    if (ioctl_size >= 0)
                        pt_copy(pid, regs.rdx, ioctl_size);
                }
                break;
            }
            case 116: // sysinfo
                pt_copy(pid, regs.rbx, sizeof(struct sys_info)); break;
            case 122: // uname
                pt_copy(pid, regs.rbx, sizeof(struct uname)); break;
            case 140: // _llseek
                pt_copy(pid, regs.rsi, 8); break;
            case 183: // getcwd
                pt_copy(pid, regs.rbx, cpu->eax); break;

            case 195: // stat64
            case 196: // lstat64
            case 197: // fstat64
                pt_copy(pid, regs.rcx, sizeof(struct newstat64)); break;
            case 220: // getdents64
                pt_copy(pid, regs.rcx, cpu->eax); break;
            case 265: // clock_gettime
                pt_copy(pid, regs.rcx, sizeof(struct time_spec)); break;

            case 90: // mmap
            case 192: // mmap2
                if (cpu->eax < 0xfffff000 && cpu->edi != (dword_t) -1) {
                    // fake mmap didn't fail, change fd
                    saved_fd = regs.rdi;
                    regs.rdi = transmit_fd(pid, sender, receiver, cpu->edi);
                }
                goto do_step;

            // some syscalls need to just happen
            case 45: // brk
            case 91: // munmap
            case 125: // mprotect
            case 174: // rt_sigaction
            case 243: // set_thread_area
                goto do_step;
        }
        regs.rax = cpu->eax;
        regs.rip += 2;
    } else {
do_step:
        setregs(pid, &regs);
        // single step on a repeated string instruction only does one
        // iteration, so loop until ip changes
        unsigned long ip = regs.rip;
        int was_signal;
        while (regs.rip == ip) {
            was_signal = step(pid);
            getregs(pid, &regs);
        }
        if (saved_fd >= 0) {
            remote_close_fd(pid, regs.rdi, ip);
            regs.rdi = saved_fd;
        }

        if (was_signal) {
            // copy the return address
            pt_copy(pid, regs.rsp, sizeof(addr_t));
            // and copy the rest the other way
            pt_copy_to_real(pid, regs.rsp + sizeof(addr_t), old_sp - regs.rsp - sizeof(addr_t));
        }
    }
    setregs(pid, &regs);
}

static void prepare_tracee(int pid) {
    transplant_vdso(pid, vdso_data, sizeof(vdso_data));
    aux_write(pid, AX_HWCAP, 0); // again, suck that
    aux_write(pid, AX_UID, 0);
    aux_write(pid, AX_EUID, 0);
    aux_write(pid, AX_GID, 0);
    aux_write(pid, AX_EGID, 0);

    // copy random bytes
    addr_t random = aux_read(pid, AX_RANDOM);
    for (addr_t a = random; a < random + 16; a += sizeof(dword_t)) {
        pt_write(pid, a, user_get(a));
    }

    // find out how big the signal stack frame needs to be
    __asm__("cpuid"
            : "=b" (xsave_extra)
            : "a" (0xd), "c" (0)
            : "edx");
    // if xsave is supported, add 4 bytes. why? idk
    int features_ecx;
    __asm__("cpuid"
            : "=c" (features_ecx)
            : "a" (1)
            : "ebx", "edx");
    if (features_ecx & (1 << 26))
        xsave_extra += 4;
    // if fxsave/fxrestore is supported, use 112 bytes for that
    int features_edx;
    __asm__("cpuid"
            : "=c" (features_edx)
            : "a" (1)
            : "ebx", "edx");
    if (features_edx & (1 << 24))
        fxsave_extra = 112;
}

int main(int argc, char *const argv[]) {
    int err;
    setup();
    char *envp[] = {NULL};
    if ((err = sys_execve(argv[1], argv + 1, envp)) < 0) {
        return -err;
    }

    int fds[2];
    trycall(socketpair(AF_UNIX, SOCK_DGRAM, 0, fds), "socketpair");
    int pid = start_tracee(argv[1], argv + 1, envp);
    int sender = fds[0], receiver = fds[1];
    /* close(receiver); // only needed in the child */
    prepare_tracee(pid);

    struct cpu_state *cpu = &current->cpu;
    while (true) {
        struct cpu_state old_cpu = *cpu;
        int undefined_flags = undefined_flags_mask(pid, cpu);
        step_tracing(cpu, pid, sender, receiver);
        if (compare_cpus(cpu, pid, undefined_flags) < 0) {
            printf("failure: resetting cpu\n");
            *cpu = old_cpu;
            __asm__("int3");
            cpu_step32(cpu);
            return -1;
        }
    }
}

// useful for calling from the debugger
void dump_memory(int pid, const char *file, addr_t start, addr_t end) {
    FILE *f = fopen(file, "w");
    for (addr_t addr = start; addr <= end; addr += sizeof(dword_t)) {
        dword_t val = pt_read(pid, addr);
        fwrite(&val, sizeof(dword_t), 1, f);
    }
    fclose(f);
}