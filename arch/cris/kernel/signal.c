/*
 *  linux/arch/cris/kernel/signal.c
 *
 *  Based on arch/i386/kernel/signal.c by
 *     Copyright (C) 1991, 1992  Linus Torvalds
 *     1997-11-28  Modified for POSIX.1b signals by Richard Henderson *
 *
 *  Ideas also taken from arch/arm.
 *
 *  Copyright (C) 2000, 2001 Axis Communications AB
 *
 *  Authors:  Bjorn Wesen (bjornw@axis.com)
 *
 */

#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/unistd.h>
#include <linux/stddef.h>

#include <asm/processor.h>
#include <asm/ucontext.h>
#include <asm/uaccess.h>

#define DEBUG_SIG 0

#define _BLOCKABLE (~(sigmask(SIGKILL) | sigmask(SIGSTOP)))

/* a syscall in Linux/CRIS is a break 13 instruction which is 2 bytes */
/* manipulate regs so that upon return, it will be re-executed */

/* We rely on that pc points to the instruction after "break 13", so the
 * library must never do strange things like putting it in a delay slot.
 */
#define RESTART_CRIS_SYS(regs) regs->r10 = regs->orig_r10; regs->irp -= 2;

int do_signal(int canrestart, sigset_t *oldset, struct pt_regs *regs);

/*
 * Atomically swap in the new signal mask, and wait for a signal.  Define 
 * dummy arguments to be able to reach the regs argument.  (Note that this
 * arrangement relies on old_sigset_t occupying one register.)
 */
int
sys_sigsuspend(old_sigset_t mask, long r11, long r12, long r13, long mof, 
               long srp, struct pt_regs *regs)
{
	sigset_t saveset;

	mask &= _BLOCKABLE;
	spin_lock_irq(&current->sigmask_lock);
	saveset = current->blocked;
	siginitset(&current->blocked, mask);
	recalc_sigpending();
	spin_unlock_irq(&current->sigmask_lock);

	regs->r10 = -EINTR;
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (do_signal(0, &saveset, regs))
			/* We will get here twice: once to call the signal
			   handler, then again to return from the
			   sigsuspend system call.  When calling the
			   signal handler, R10 holds the signal number as
			   set through do_signal.  The sigsuspend call
			   will return with the restored value set above;
			   always -EINTR.  */
			return regs->r10;
	}
}

/* Define dummy arguments to be able to reach the regs argument.  (Note that
 * this arrangement relies on size_t occupying one register.)
 */
int
sys_rt_sigsuspend(sigset_t *unewset, size_t sigsetsize, long r12, long r13, 
                  long mof, long srp, struct pt_regs *regs)
{
	sigset_t saveset, newset;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&newset, unewset, sizeof(newset)))
		return -EFAULT;
	sigdelsetmask(&newset, ~_BLOCKABLE);

	spin_lock_irq(&current->sigmask_lock);
	saveset = current->blocked;
	current->blocked = newset;
	recalc_sigpending();
	spin_unlock_irq(&current->sigmask_lock);

	regs->r10 = -EINTR;
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (do_signal(0, &saveset, regs))
			/* We will get here twice: once to call the signal
			   handler, then again to return from the
			   sigsuspend system call.  When calling the
			   signal handler, R10 holds the signal number as
			   set through do_signal.  The sigsuspend call
			   will return with the restored value set above;
			   always -EINTR.  */
			return regs->r10;
	}
}

int 
sys_sigaction(int sig, const struct old_sigaction *act,
	      struct old_sigaction *oact)
{
	struct k_sigaction new_ka, old_ka;
	int ret;

	if (act) {
		old_sigset_t mask;
		if (verify_area(VERIFY_READ, act, sizeof(*act)) ||
		    __get_user(new_ka.sa.sa_handler, &act->sa_handler) ||
		    __get_user(new_ka.sa.sa_restorer, &act->sa_restorer))
			return -EFAULT;
		__get_user(new_ka.sa.sa_flags, &act->sa_flags);
		__get_user(mask, &act->sa_mask);
		siginitset(&new_ka.sa.sa_mask, mask);
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		if (verify_area(VERIFY_WRITE, oact, sizeof(*oact)) ||
		    __put_user(old_ka.sa.sa_handler, &oact->sa_handler) ||
		    __put_user(old_ka.sa.sa_restorer, &oact->sa_restorer))
			return -EFAULT;
		__put_user(old_ka.sa.sa_flags, &oact->sa_flags);
		__put_user(old_ka.sa.sa_mask.sig[0], &oact->sa_mask);
	}

	return ret;
}

int
sys_sigaltstack(const stack_t *uss, stack_t *uoss)
{
	return do_sigaltstack(uss, uoss, rdusp());
}


/*
 * Do a signal return; undo the signal stack.
 */

struct sigframe {
	struct sigcontext sc;
	unsigned long extramask[_NSIG_WORDS-1];
	unsigned char retcode[8];  /* trampoline code */
};

struct rt_sigframe {
	struct siginfo *pinfo;
	void *puc;
	struct siginfo info;
	struct ucontext uc;
	unsigned char retcode[8];  /* trampoline code */
};


static int
restore_sigcontext(struct pt_regs *regs, struct sigcontext *sc)
{
	unsigned int err = 0;
	unsigned long old_usp;

	/* restore the regs from &sc->regs (same as sc, since regs is first)
	 * (sc is already checked for VERIFY_READ since the sigframe was
	 *  checked in sys_sigreturn previously)
	 */

	if (__copy_from_user(regs, sc, sizeof(struct pt_regs)))
                goto badframe;

	/* make sure the U-flag is set so user-mode cannot fool us */

	regs->dccr |= 1 << 8;

	/* restore the old USP as it was before we stacked the sc etc.
	 * (we cannot just pop the sigcontext since we aligned the sp and
	 *  stuff after pushing it)
	 */

	err |= __get_user(old_usp, &sc->usp);

	wrusp(old_usp);

	/* TODO: the other ports use regs->orig_XX to disable syscall checks
	 * after this completes, but we don't use that mechanism. maybe we can
	 * use it now ? 
	 */

	return err;

badframe:
	return 1;
}

/* Define dummy arguments to be able to reach the regs argument.  */

asmlinkage int sys_sigreturn(long r10, long r11, long r12, long r13, long mof, 
                             long srp, struct pt_regs *regs)
{
	struct sigframe *frame = (struct sigframe *)rdusp();
	sigset_t set;

        /*
         * Since we stacked the signal on a dword boundary,
         * then frame should be dword aligned here.  If it's
         * not, then the user is trying to mess with us.
         */
        if (((long)frame) & 3)
                goto badframe;

	if (verify_area(VERIFY_READ, frame, sizeof(*frame)))
		goto badframe;
	if (__get_user(set.sig[0], &frame->sc.oldmask)
	    || (_NSIG_WORDS > 1
		&& __copy_from_user(&set.sig[1], &frame->extramask,
				    sizeof(frame->extramask))))
		goto badframe;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending();
	spin_unlock_irq(&current->sigmask_lock);
	
	if (restore_sigcontext(regs, &frame->sc))
		goto badframe;

	/* TODO: SIGTRAP when single-stepping as in arm ? */

	return regs->r10;

badframe:
	force_sig(SIGSEGV, current);
	return 0;
}	

/* Define dummy arguments to be able to reach the regs argument.  */

asmlinkage int sys_rt_sigreturn(long r10, long r11, long r12, long r13, 
                                long mof, long srp, struct pt_regs *regs)
{
	struct rt_sigframe *frame = (struct rt_sigframe *)rdusp();
	sigset_t set;
	stack_t st;

        /*
         * Since we stacked the signal on a dword boundary,
         * then frame should be dword aligned here.  If it's
         * not, then the user is trying to mess with us.
         */
        if (((long)frame) & 3)
                goto badframe;

	if (verify_area(VERIFY_READ, frame, sizeof(*frame)))
		goto badframe;
	if (__copy_from_user(&set, &frame->uc.uc_sigmask, sizeof(set)))
		goto badframe;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sigmask_lock);
	current->blocked = set;
	recalc_sigpending();
	spin_unlock_irq(&current->sigmask_lock);
	
	if (restore_sigcontext(regs, &frame->uc.uc_mcontext))
		goto badframe;

	if (__copy_from_user(&st, &frame->uc.uc_stack, sizeof(st)))
		goto badframe;
	/* It is more difficult to avoid calling this function than to
	   call it and ignore errors.  */
	do_sigaltstack(&st, NULL, rdusp());

	return regs->r10;

badframe:
	force_sig(SIGSEGV, current);
	return 0;
}	

/*
 * Set up a signal frame.
 */

static int
setup_sigcontext(struct sigcontext *sc, struct pt_regs *regs, unsigned long mask)
{
	int err = 0;
	unsigned long usp = rdusp();

	/* copy the regs. they are first in sc so we can use sc directly */

	err |= __copy_to_user(sc, regs, sizeof(struct pt_regs));

        /* Set the frametype to CRIS_FRAME_NORMAL for the execution of
           the signal handler. The frametype will be restored to its previous
           value in restore_sigcontext. */
        regs->frametype = CRIS_FRAME_NORMAL;

	/* then some other stuff */

	err |= __put_user(mask, &sc->oldmask);

	err |= __put_user(usp, &sc->usp);

	return err;
}

/* figure out where we want to put the new signal frame - usually on the stack */

static inline void *
get_sigframe(struct k_sigaction *ka, struct pt_regs * regs, size_t frame_size)
{
	unsigned long sp = rdusp();

	/* This is the X/Open sanctioned signal stack switching.  */
	if (ka->sa.sa_flags & SA_ONSTACK) {
		if (! on_sig_stack(sp))
			sp = current->sas_ss_sp + current->sas_ss_size;
	}

	/* make sure the frame is dword-aligned */

	sp &= ~3;

	return (void *)(sp - frame_size);
}

/* grab and setup a signal frame.
 * 
 * basically we stack a lot of state info, and arrange for the
 * user-mode program to return to the kernel using either a
 * trampoline which performs the syscall sigreturn, or a provided
 * user-mode trampoline.
 */

static void setup_frame(int sig, struct k_sigaction *ka,
			sigset_t *set, struct pt_regs * regs)
{
	struct sigframe *frame;
	unsigned long return_ip;
	int err = 0;

	frame = get_sigframe(ka, regs, sizeof(*frame));

	if (!access_ok(VERIFY_WRITE, frame, sizeof(*frame)))
		goto give_sigsegv;

	err |= setup_sigcontext(&frame->sc, regs, set->sig[0]);
	if (err)
		goto give_sigsegv;

	if (_NSIG_WORDS > 1) {
		err |= __copy_to_user(frame->extramask, &set->sig[1],
				      sizeof(frame->extramask));
	}
	if (err)
		goto give_sigsegv;

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	if (ka->sa.sa_flags & SA_RESTORER) {
		return_ip = (unsigned long)ka->sa.sa_restorer;
	} else {
		/* trampoline - the desired return ip is the retcode itself */
		return_ip = (unsigned long)&frame->retcode;
		/* This is movu.w __NR_sigreturn, r9; break 13; */
		err |= __put_user(0x9c5f,         (short *)(frame->retcode+0));
		err |= __put_user(__NR_sigreturn, (short *)(frame->retcode+2));
		err |= __put_user(0xe93d,         (short *)(frame->retcode+4));
	}

	if (err)
		goto give_sigsegv;

	/* Set up registers for signal handler */

	regs->irp = (unsigned long) ka->sa.sa_handler;  /* what we enter NOW   */
	regs->srp = return_ip;                          /* what we enter LATER */
	regs->r10 = sig;                                /* first argument is signo */

	/* actually move the usp to reflect the stacked frame */

	wrusp((unsigned long)frame);

	return;

give_sigsegv:
	if (sig == SIGSEGV)
		ka->sa.sa_handler = SIG_DFL;
	force_sig(SIGSEGV, current);
}

static void setup_rt_frame(int sig, struct k_sigaction *ka, siginfo_t *info,
			   sigset_t *set, struct pt_regs * regs)
{
	struct rt_sigframe *frame;
	unsigned long return_ip;
	int err = 0;

	frame = get_sigframe(ka, regs, sizeof(*frame));

	if (!access_ok(VERIFY_WRITE, frame, sizeof(*frame)))
		goto give_sigsegv;

	err |= __put_user(&frame->info, &frame->pinfo);
	err |= __put_user(&frame->uc, &frame->puc);
	err |= copy_siginfo_to_user(&frame->info, info);
	if (err)
		goto give_sigsegv;

	/* Clear all the bits of the ucontext we don't use.  */
        err |= __clear_user(&frame->uc, offsetof(struct ucontext, uc_mcontext));

	err |= setup_sigcontext(&frame->uc.uc_mcontext, regs, set->sig[0]);

	err |= __copy_to_user(&frame->uc.uc_sigmask, set, sizeof(*set));

	if (err)
		goto give_sigsegv;

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	if (ka->sa.sa_flags & SA_RESTORER) {
		return_ip = (unsigned long)ka->sa.sa_restorer;
	} else {
		/* trampoline - the desired return ip is the retcode itself */
		return_ip = (unsigned long)&frame->retcode;
		/* This is movu.w __NR_rt_sigreturn, r9; break 13; */
		err |= __put_user(0x9c5f,            (short *)(frame->retcode+0));
		err |= __put_user(__NR_rt_sigreturn, (short *)(frame->retcode+2));
		err |= __put_user(0xe93d,            (short *)(frame->retcode+4));
	}

	if (err)
		goto give_sigsegv;

	/* TODO what is the current->exec_domain stuff and invmap ? */

	/* Set up registers for signal handler */

	regs->irp = (unsigned long) ka->sa.sa_handler;  /* what we enter NOW   */
	regs->srp = return_ip;                          /* what we enter LATER */
	regs->r10 = sig;                                /* first argument is signo */
        regs->r11 = (unsigned long) &frame->info;       /* second argument is (siginfo_t *) */
        regs->r12 = 0;                                  /* third argument is unused */

	/* actually move the usp to reflect the stacked frame */

	wrusp((unsigned long)frame);

	return;

give_sigsegv:
	if (sig == SIGSEGV)
		ka->sa.sa_handler = SIG_DFL;
	force_sig(SIGSEGV, current);
}

/*
 * OK, we're invoking a handler
 */	

static inline void
handle_signal(int canrestart, unsigned long sig,
	      siginfo_t *info, sigset_t *oldset, struct pt_regs * regs)
{
	struct k_sigaction *ka = &current->sig->action[sig-1];

	/* Are we from a system call? */
	if (canrestart) {
		/* If so, check system call restarting.. */
		switch (regs->r10) {
			case -ERESTARTNOHAND:
				/* ERESTARTNOHAND means that the syscall should only be
				   restarted if there was no handler for the signal, and since
				   we only get here if there is a handler, we don't restart */
				regs->r10 = -EINTR;
				break;

			case -ERESTARTSYS:
				/* ERESTARTSYS means to restart the syscall if there is no
				   handler or the handler was registered with SA_RESTART */
				if (!(ka->sa.sa_flags & SA_RESTART)) {
					regs->r10 = -EINTR;
					break;
				}
			/* fallthrough */
			case -ERESTARTNOINTR:
				/* ERESTARTNOINTR means that the syscall should be called again
				   after the signal handler returns. */
				RESTART_CRIS_SYS(regs);
		}
	}

	/* Set up the stack frame */
	if (ka->sa.sa_flags & SA_SIGINFO)
		setup_rt_frame(sig, ka, info, oldset, regs);
	else
		setup_frame(sig, ka, oldset, regs);

	if (ka->sa.sa_flags & SA_ONESHOT)
		ka->sa.sa_handler = SIG_DFL;

	if (!(ka->sa.sa_flags & SA_NODEFER)) {
		spin_lock_irq(&current->sigmask_lock);
		sigorsets(&current->blocked,&current->blocked,&ka->sa.sa_mask);
		sigaddset(&current->blocked,sig);
		recalc_sigpending();
		spin_unlock_irq(&current->sigmask_lock);
	}
}

/*
 * Note that 'init' is a special process: it doesn't get signals it doesn't
 * want to handle. Thus you cannot kill init even with a SIGKILL even by
 * mistake.
 *
 * Also note that the regs structure given here as an argument, is the latest
 * pushed pt_regs. It may or may not be the same as the first pushed registers
 * when the initial usermode->kernelmode transition took place. Therefore
 * we can use user_mode(regs) to see if we came directly from kernel or user
 * mode below.
 */

int do_signal(int canrestart, sigset_t *oldset, struct pt_regs *regs)
{
	siginfo_t info;
	int signr;

	/*
	 * We want the common case to go fast, which
	 * is why we may in certain cases get here from
	 * kernel mode. Just return without doing anything
	 * if so.
	 */
	if (!user_mode(regs))
		return 1;

	if (!oldset)
		oldset = &current->blocked;

	signr = get_signal_to_deliver(&info, regs, NULL);
	if (signr > 0) {
		/* Whee!  Actually deliver the signal.  */
		handle_signal(canrestart, signr, &info, oldset, regs);
		return 1;
	}

	/* Did we come from a system call? */
	if (canrestart) {
		/* Restart the system call - no handlers present */
		if (regs->r10 == -ERESTARTNOHAND ||
		    regs->r10 == -ERESTARTSYS ||
		    regs->r10 == -ERESTARTNOINTR) {
			RESTART_CRIS_SYS(regs);
		}
	}
	return 0;
}
