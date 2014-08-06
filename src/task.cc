/* -*- Mode: C++; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

//#define DEBUGTAG "Task"

#include "task.h"

#include <errno.h>
#include <linux/net.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <elf.h>

#include <limits>
#include <set>

#include "preload/syscall_buffer.h"

#include "hpc.h"
#include "log.h"
#include "remote_syscalls.h"
#include "session.h"
#include "syscalls.h"
#include "util.h"

#define NUM_X86_DEBUG_REGS 8
#define NUM_X86_WATCHPOINTS 4

#define RR_PTRACE_O_EXITKILL (1 << 20)

using namespace std;

/**
 * Format into |path| the path name at which syscallbuf shmem for
 * tracee |tid| will be allocated (briefly, until unlinked).
 */
template<size_t N>
void format_syscallbuf_shmem_path(pid_t tid, char (&path)[N])
{
	static int nonce;
	snprintf(path, N - 1, SYSCALLBUF_SHMEM_NAME_PREFIX "%d-%d",
		 tid, nonce++);
}

// TODO: merge other dups of this function.
static void write_socketcall_args(Task* t,
				  struct socketcall_args* child_args_vec,
				  long arg1, long arg2, long arg3)
{
	struct socketcall_args args = { { arg1, arg2, arg3 } };
	t->write_mem(child_args_vec, args);
}

/**
 * Stores the table of signal dispositions and metadata for an
 * arbitrary set of tasks.  Each of those tasks must own one one of
 * the |refcount|s while they still refer to this.
 */
struct Sighandler {
	Sighandler() : sa(), resethand() { }
	Sighandler(const struct kernel_sigaction& sa)
		: sa(sa), resethand(sa.sa_flags & SA_RESETHAND) { }

	bool ignored(int sig) const {
		return (SIG_IGN == sa.k_sa_handler ||
			(SIG_DFL == sa.k_sa_handler
			 && IGNORE == default_action(sig)));
	}
	bool is_default() const {
		return SIG_DFL == sa.k_sa_handler && !resethand;
	}
	bool is_user_handler() const {
		static_assert((void*)1 == SIG_IGN, "");
		return (uintptr_t)sa.k_sa_handler & ~(uintptr_t)SIG_IGN;
	}

	kernel_sigaction sa;
	bool resethand;
};
struct Sighandlers {
	typedef shared_ptr<Sighandlers> shr_ptr;

	shr_ptr clone() const {
		shr_ptr s(new Sighandlers());
		// NB: depends on the fact that Sighandler is for all
		// intents and purposes a POD type, though not
		// technically.
		memcpy(s->handlers, handlers, sizeof(handlers));
		return s;
	}

	Sighandler& get(int sig) {
		assert_valid(sig);
		return handlers[sig];
	}
	const Sighandler& get(int sig) const {
		assert_valid(sig);
		return handlers[sig];
	}

	void init_from_current_process() {
		for (int i = 0; i < ssize_t(ALEN(handlers)); ++i) {
			Sighandler& h = handlers[i];
			struct sigaction act;
			if (-1 == sigaction(i, NULL, &act)) {
				/* EINVAL means we're querying an
				 * unused signal number. */
				assert(EINVAL == errno);
				assert(h.is_default());
				continue;
			}
			struct kernel_sigaction ka;
			ka.k_sa_handler = act.sa_handler;
			ka.sa_flags = act.sa_flags;
			ka.sa_restorer = act.sa_restorer;
			ka.sa_mask = act.sa_mask;
			h = Sighandler(ka);
		}
	}

	/**
	 * For each signal in |table| such that is_user_handler() is
	 * true, reset the disposition of that signal to SIG_DFL, and
	 * clear the resethand flag if it's set.  SIG_IGN signals are
	 * not modified.
	 *
	 * (After an exec() call copies the original sighandler table,
	 * this is the operation required by POSIX to initialize that
	 * table copy.)
	 */
	void reset_user_handlers() {
		for (int i = 0; i < ssize_t(ALEN(handlers)); ++i) {
			Sighandler& h = handlers[i];
			// If the handler was a user handler, reset to
			// default.  If it was SIG_IGN or SIG_DFL,
			// leave it alone.
			if (h.is_user_handler()) {
				handlers[i] = Sighandler();
			}
		}
	}

	static void assert_valid(int sig) {
		assert(0 < sig && sig < ssize_t(ALEN(handlers)));
	}

	static shr_ptr create() {
		return shr_ptr(new Sighandlers());
	}

	Sighandler handlers[_NSIG];

private:
	Sighandlers() { }
	Sighandlers(const Sighandlers&);
	Sighandlers operator=(const Sighandlers&);
};

void
TaskGroup::destabilize()
{
	LOG(debug) <<"destabilizing task group "<< tgid;
	for (auto it = task_set().begin(); it != task_set().end(); ++it) {
		Task* t = *it;
		t->unstable = 1;
		LOG(debug) <<"  destabilized task "<< t->tid;
	}
}

TaskGroup::TaskGroup(pid_t tgid, pid_t real_tgid)
	: tgid(tgid)
	, real_tgid(real_tgid)
	, exit_code(-1)
{
	LOG(debug) <<"creating new task group "<< tgid <<" (real tgid:"
		   << real_tgid <<")";
}

Task::Task(pid_t _tid, pid_t _rec_tid, int _priority)
	: thread_time(1)
	, switchable(), pseudo_blocked(), succ_event_counter(), unstable()
	, priority(_priority)
	, scratch_ptr(), scratch_size()
	, flushed_syscallbuf()
	, delay_syscallbuf_reset(), delay_syscallbuf_flush()
	  // These will be initialized when the syscall buffer is.
	, desched_fd(-1), desched_fd_child(-1)
	, seccomp_bpf_enabled()
	, child_sig(), stepped_into_syscall()
	, hpc()
	, tid(_tid), rec_tid(_rec_tid > 0 ? _rec_tid : _tid)
	, untraced_syscall_ip(), syscallbuf_lib_start(), syscallbuf_lib_end()
	, syscallbuf_hdr(), num_syscallbuf_bytes(), syscallbuf_child()
	, blocked_sigs()
	, prname("???")
	, rbcs(0)
	, registers_known(false), extra_registers_known(false)
	, robust_futex_list(), robust_futex_list_len()
	, session_record(nullptr), session_replay(nullptr)
	, stashed_si(), stashed_wait_status()
	, thread_area(), thread_area_valid()
	, tid_futex()
	, top_of_stack()
	, wait_status()
{
	if (RECORD != rr_flags()->option) {
		// This flag isn't meaningful outside recording.
		// Suppress output related to it outside recording.
		switchable = 1;
	}

	push_event(Event(EV_SENTINEL, NO_EXEC_INFO));

	init_hpc(this);
}

Task::~Task()
{
	LOG(debug) <<"task "<< tid <<" (rec:"<< rec_tid <<") is dying ...";

	assert(this == session().find_task(rec_tid));
	// We expect tasks to usually exit by a call to exit() or
	// exit_group(), so it's not helpful to warn about that.
	if (EV_SENTINEL != ev().type()
	    && (pending_events.size() > 2
		|| !(ev().type() == EV_SYSCALL
		     && (SYS_exit == ev().Syscall().no
			 || SYS_exit_group == ev().Syscall().no)))) {
		LOG(warn) << tid <<" still has pending events.  From top down:";
		log_pending_events();
	}

	session().on_destroy(this);
	tg->erase_task(this);
	as->erase_task(this);

	destroy_hpc(this);
	destroy_local_buffers();

	// We need the mem_fd in detach_and_reap().
	detach_and_reap();

	LOG(debug) <<"  dead";
}

bool
Task::at_may_restart_syscall() const
{
	ssize_t depth = pending_events.size();
	const Event* prev_ev =
		depth > 2 ? &pending_events[depth - 2] : nullptr;
	return EV_SYSCALL_INTERRUPTION == ev().type()
		|| (EV_SIGNAL_DELIVERY == ev().type()
		    && prev_ev && EV_SYSCALL_INTERRUPTION == prev_ev->type());
}

void
Task::finish_emulated_syscall()
{
	// XXX verify that this can't be interrupted by a breakpoint trap
	Registers r = regs();
	void* ip = (void*)r.ip();
	bool known_idempotent_insn_after_syscall = (is_traced_syscall()
						    || is_untraced_syscall());

	// We're about to single-step the tracee at its $ip just past
	// the syscall insn, then back up the $ip to where it started.
	// This is problematic because it will execute the insn at the
	// current $ip twice.  If that insns isn't idempotent, then
	// replay will create side effects that diverge from
	// recording.
	//
	// To prevent that, we insert a breakpoint trap at the current
	// $ip.  We can execute that without creating side effects.
	// After the single-step, we remove the breakpoint, which
	// restores the original insn at the $ip.
	//
	// Syscalls made from the syscallbuf are known to execute an
	// idempotent insn after the syscall trap (restore register
	// from stack), so we don't have to pay this expense.
	if (!known_idempotent_insn_after_syscall) {
		vm()->set_breakpoint(ip, TRAP_BKPT_INTERNAL);
	}
	cont_sysemu_singlestep();

	if (!known_idempotent_insn_after_syscall) {
		// The breakpoint should raise SIGTRAP, but we can also see
		// any of the host of replay-ignored signals.
		ASSERT(this, (pending_sig() == SIGTRAP
			      || is_ignored_replay_signal(pending_sig())))
			<< "PENDING SIG IS "<< signalname(pending_sig());
		vm()->remove_breakpoint(ip, TRAP_BKPT_INTERNAL);
	}
	set_regs(r);
	force_status(0);
}

const struct syscallbuf_record*
Task::desched_rec() const
{
	return (ev().is_syscall_event() ? ev().Syscall().desched_rec :
		(EV_DESCHED == ev().type()) ? ev().Desched().rec : nullptr);
}

/**
 * In recording, Task is notified of a destabilizing signal event
 * *after* it's been recorded, at the next trace-event-time.  In
 * replay though we're notified at the occurrence of the signal.  So
 * to display the same event time in logging across record/replay,
 * apply this offset.
 */
static int signal_delivery_event_offset()
{
	return RECORD == rr_flags()->option ? -1 : 0;
}

void
Task::destabilize_task_group()
{
	// Only print this helper warning if there's (probably) a
	// human around to see it.  This is done to avoid polluting
	// output from tests.
	if (EV_SIGNAL_DELIVERY == ev().type() && !probably_not_interactive()) {
		printf("[rr.%d] Warning: task %d (process %d) dying from fatal signal %s.\n",
		       trace_time() + signal_delivery_event_offset(),
		       rec_tid, tgid(), signalname(ev().Signal().no));
	}

	tg->destabilize();
}

void
Task::dump(FILE* out) const
{
	out = out ? out : stderr;
	fprintf(out, "  %s(tid:%d rec_tid:%d status:0x%x%s%s)<%p>\n",
		prname.c_str(), tid, rec_tid, wait_status,
		switchable ? "" : " UNSWITCHABLE",
		unstable ? " UNSTABLE" : "",
		this);
	if (RECORD == rr_flags()->option) {
		// TODO pending events are currently only meaningful
		// during recording.  We should change that
		// eventually, to have more informative output.
		log_pending_events();
	}
}

void
Task::set_priority(int value)
{
	if (priority == value) {
		// don't mess with task order
		return;
	}
	session().update_task_priority(this, value);
}

bool
Task::fdstat(int fd, struct stat* st, char* buf, size_t buf_num_bytes)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path) - 1, "/proc/%d/fd/%d", tid, fd);
	ScopedOpen backing_fd(path, O_RDONLY);
	if (0 > backing_fd) {
		return false;
	}
	ssize_t nbytes = readlink(path, buf, buf_num_bytes);
	if (0 > nbytes) {
		return false;
	}
	buf[nbytes] = '\0';
	return 0 == fstat(backing_fd, st);
}

void
Task::futex_wait(void* futex, uint32_t val)
{
	static_assert(sizeof(val) == sizeof(long),
		      "Sorry, need to implement Task::read_int().");
	// Wait for *sync_addr == sync_val.  This implementation isn't
	// pretty, but it's pretty much the best we can do with
	// available kernel tools.
	//
	// TODO: find clever way to avoid busy-waiting.
	while (val != uint32_t(read_word(futex))) {
		// Try to give our scheduling slot to the kernel
		// thread that's going to write sync_addr.
		sched_yield();
	}
}

unsigned long
Task::get_ptrace_eventmsg()
{
	unsigned long msg;
	xptrace(PTRACE_GETEVENTMSG, nullptr, &msg);
	return msg;
}

void
Task::get_siginfo(siginfo_t* si)
{
	xptrace(PTRACE_GETSIGINFO, nullptr, si);
}

void
Task::set_siginfo(const siginfo_t& si)
{
	xptrace(PTRACE_SETSIGINFO, nullptr, (void*)&si);
}

TraceIfstream&
Task::ifstream()
{
	return session_replay->ifstream();
}

TraceOfstream&
Task::ofstream()
{
	return session_record->ofstream();
}

void*
Task::init_buffers(void* map_hint, int share_desched_fd)
{
	// NB: the tracee can't be interrupted with a signal while
	// we're processing the rrcall, because it's masked off all
	// signals.
	AutoRemoteSyscalls remote(this);

	// Arguments to the rrcall.
	void* child_args = (void*)remote.regs().arg1();
	struct rrcall_init_buffers_params args;
	read_mem(child_args, &args);

	ASSERT(this,
	       rr_flags()->use_syscall_buffer == !!args.syscallbuf_enabled)
		<<"Tracee things syscallbuf is "
		<< (args.syscallbuf_enabled ? "en" : "dis")
		<<"abled, but tracer thinks "
		<< (rr_flags()->use_syscall_buffer ? "en" : "dis") <<"abled";

	void* child_map_addr = nullptr;
	if (args.syscallbuf_enabled) {
		traced_syscall_ip = args.traced_syscall_ip;
		untraced_syscall_ip = args.untraced_syscall_ip;
		args.syscallbuf_ptr = child_map_addr =
				      init_syscall_buffer(remote, map_hint);
		init_desched_fd(remote, &args, share_desched_fd);
		// Zero out the child buffers we may have used here.
		// They may contain "real" fds, which in general will
		// not be the same across record/replay.
		write_socketcall_args(this, args.args_vec, 0, 0, 0);
		write_mem(args.fdptr, 0);
	} else {
		args.syscallbuf_ptr = nullptr;
	}
	
	// Return the mapped buffers to the child.
	write_mem(child_args, args);

	// The tracee doesn't need this addr returned, because it's
	// already written to the inout |args| param, but we stash it
	// away in the return value slot so that we can easily check
	// that we map the segment at the same addr during replay.
	remote.regs().set_syscall_result((uintptr_t)child_map_addr);
	syscallbuf_hdr->locked = is_desched_sig_blocked();

	return child_map_addr;
}

void
Task::destroy_buffers(int which)
{
	AutoRemoteSyscalls remote(this);
	if (DESTROY_SCRATCH & which) {
		remote.syscall(SYS_munmap, scratch_ptr, scratch_size);
		vm()->unmap(scratch_ptr, scratch_size);
	}
	if ((DESTROY_SYSCALLBUF & which) && syscallbuf_child) {
		remote.syscall(SYS_munmap,
			       syscallbuf_child, num_syscallbuf_bytes);
		vm()->unmap(syscallbuf_child, num_syscallbuf_bytes);
		remote.syscall(SYS_close, desched_fd_child);
	}
}

bool
Task::is_arm_desched_event_syscall()
{
	return (is_desched_event_syscall()
		&& PERF_EVENT_IOC_ENABLE == regs().arg2());
}

bool
Task::is_desched_event_syscall()
{
	return (SYS_ioctl == regs().original_syscallno()
		&& (desched_fd_child == (int)regs().arg1_signed()
		    || desched_fd_child == REPLAY_DESCHED_EVENT_FD));
}

bool
Task::is_disarm_desched_event_syscall()
{
	return (is_desched_event_syscall()
		&& PERF_EVENT_IOC_DISABLE == regs().arg2());
}

bool
Task::is_probably_replaying_syscall()
{
	assert(session_replay);
	// If the tracee is at our syscall entry points, we know for
	// sure it's entering/exiting/just-exited a syscall.
	return (is_traced_syscall() || is_untraced_syscall()
		// Otherwise, we assume that if original_syscallno() has been
		// saved from syscallno(), then the tracee is in a syscall.
		// This seems to be the heuristic used by the linux
		// kernel for /proc/PID/syscall.
		|| regs().original_syscallno() >= 0);
}

bool
Task::is_ptrace_seccomp_event() const
{
	int event = ptrace_event();
	return (PTRACE_EVENT_SECCOMP_OBSOLETE == event ||
		PTRACE_EVENT_SECCOMP == event);
}

bool
Task::is_sig_blocked(int sig) const
{
	int sig_bit = sig - 1;
	return (blocked_sigs >> sig_bit) & 1;
}

bool
Task::is_sig_ignored(int sig) const
{
	return sighandlers->get(sig).ignored(sig);
}

bool
Task::is_syscall_restart()
{
	int syscallno = regs().original_syscallno();
	bool must_restart = (SYS_restart_syscall == syscallno);
	bool is_restart = false;
	const Registers* old_regs;

	LOG(debug) <<"  is syscall interruption of recorded " << ev()
		   <<"? (now "<< syscallname(syscallno) <<")";

	if (EV_SYSCALL_INTERRUPTION != ev().type()) {
		goto done;
	}

	old_regs = &ev().Syscall().regs;
	/* It's possible for the tracee to resume after a sighandler
	 * with a fresh syscall that happens to be the same as the one
	 * that was interrupted.  So we check here if the args are the
	 * same.
	 *
	 * Of course, it's possible (but less likely) for the tracee
	 * to incidentally resume with a fresh syscall that just
	 * happens to have the same *arguments* too.  But in that
	 * case, we would usually set up scratch buffers etc the same
	 * was as for the original interrupted syscall, so we just
	 * save a step here.
	 *
	 * TODO: it's possible for arg structures to be mutated
	 * between the original call and restarted call in such a way
	 * that it might change the scratch allocation decisions. */
	if (SYS_restart_syscall == syscallno) {
		must_restart = true;
		syscallno = ev().Syscall().no;
		LOG(debug) <<"  (SYS_restart_syscall)";
	}
	if (ev().Syscall().no != syscallno) {
		LOG(debug) <<"  interrupted %s"<< ev() <<" != "
			   << syscallname(syscallno);
		goto done;
	}
	if (!(old_regs->arg1() == regs().arg1()
	      && old_regs->arg2() == regs().arg2()
	      && old_regs->arg3() == regs().arg3()
	      && old_regs->arg4() == regs().arg4()
	      && old_regs->arg5() == regs().arg5()
	      && old_regs->arg6() == regs().arg6())) {
		LOG(debug) <<"  regs different at interrupted "
			   << syscallname(syscallno);
		goto done;
	}
	is_restart = true;

done:
	ASSERT(this, !must_restart || is_restart)
		<<"Must restart %s"<< syscallname(syscallno) <<" but won't";
	if (is_restart) {
		LOG(debug) <<"  restart of " << syscallname(syscallno);
	}
	return is_restart;
}

void
Task::log_pending_events() const
{
	ssize_t depth = pending_events.size();

	assert(depth > 0);
	if (1 == depth) {
		LOG(info) <<"(no pending events)";
		return;
	}

	/* The event at depth 0 is the placeholder event, which isn't
	 * useful to log.  Skip it. */
	for (auto it = pending_events.rbegin(); it != pending_events.rend();
	     ++it) {
		it->log();
	}
}

bool
Task::may_be_blocked() const
{
	return (EV_SYSCALL == ev().type()
		&& PROCESSING_SYSCALL == ev().Syscall().state)
		|| (EV_SIGNAL_DELIVERY == ev().type()
		    && ev().Signal().delivered);
}

void
Task::maybe_update_vm(int syscallno, int state)
{
	// We have to use the recorded_regs during replay because they
	// have the return value set in syscall_result().  We may not have
	// advanced regs() to that point yet.
	const Registers& r = RECORD == rr_flags()->option ?
		regs() : current_trace_frame().recorded_regs;

	if (STATE_SYSCALL_EXIT != state
	    || (SYSCALL_FAILED(r.syscall_result_signed()) &&
	        SYS_mprotect != syscallno)) {
		return;
	}
	switch (syscallno) {
	case SYS_brk: {
		void* addr = reinterpret_cast<void*>(r.arg1());
		if (!addr) {
			// A brk() update of NULL is observed with
			// libc, which apparently is its means of
			// finding out the initial brk().  We can
			// ignore that for the purposes of updating
			// our address space.
			return;
		}
		return vm()->brk(addr);
	}
	case SYS_mmap2: {
		LOG(debug) <<"(mmap2 will receive / has received direct processing)";
		return;
	}
	case SYS_mprotect: {
		void* addr = reinterpret_cast<void*>(r.arg1());
		size_t num_bytes = r.arg2();
		int prot = r.arg3_signed();
		return vm()->protect(addr, num_bytes, prot);
	}
	case SYS_mremap: {
		if (SYSCALL_FAILED(r.syscall_result_signed()) &&
		    -ENOMEM != r.syscall_result_signed()) {
			return;
		}
		void* old_addr = reinterpret_cast<void*>(r.arg1());
		size_t old_num_bytes = r.arg2();
		void* new_addr = reinterpret_cast<void*>(r.syscall_result());
		size_t new_num_bytes = r.arg3();
		return vm()->remap(old_addr, old_num_bytes,
				   new_addr, new_num_bytes);
	}
	case SYS_munmap: {
		void* addr = reinterpret_cast<void*>(r.arg1());
		size_t num_bytes = r.arg2();
		return vm()->unmap(addr, num_bytes);
	}
	}
}

void
Task::move_ip_before_breakpoint()
{
	// TODO: assert that this is at a breakpoint trap.
	Registers r = regs();
	r.set_ip(r.ip() - sizeof(AddressSpace::breakpoint_insn));
	set_regs(r);
}

static string prname_from_exe_image(const string& e)
{
	size_t last_slash = e.rfind('/');
	string basename =
		(last_slash != e.npos) ? e.substr(last_slash + 1) : e;
	return basename.substr(0, 15);
}

void
Task::pre_exec()
{
	execve_file = read_c_str((void*)regs().arg1());
	if (execve_file[0] != '/') {
		char buf[PATH_MAX];
		snprintf(buf, sizeof(buf),
			 "/proc/%d/cwd/%s", real_tgid(),
			 execve_file.c_str());
		execve_file = buf;
	}
	char abspath[PATH_MAX];
	if (realpath(execve_file.c_str(), abspath)) {
		execve_file = abspath;
	}
}

void
Task::post_exec()
{
	sighandlers = sighandlers->clone();
	sighandlers->reset_user_handlers();
	as->erase_task(this);
	assert(execve_file.size() > 0);
	auto a = session().create_vm(this, execve_file);
	a->is_replacing(*as);
	as.swap(a);
	// XXX should we re-create our TaskGroup here too?
	prname = prname_from_exe_image(as->exe_image());
}

void
Task::record_current_event()
{
	record_event(ev());
}

static bool record_extra_regs(const Event& ev)
{
	switch (ev.type()) {
	case EV_SYSCALL:
		// sigreturn/rt_sigreturn restores register state
		return (ev.Syscall().no == SYS_rt_sigreturn ||
		        ev.Syscall().no == SYS_sigreturn)
		       && ev.Syscall().state == EXITING_SYSCALL;
	case EV_SIGNAL_HANDLER:
		// entering a signal handler seems to clear FP/SSE regs,
		// so record these effects.
		return true;
	default:
		return false;
	}
}

void
Task::record_event(const Event& ev)
{
	maybe_flush_syscallbuf();

	struct trace_frame frame;
	frame.global_time = ofstream().time();
	frame.thread_time = thread_time++;
	frame.tid = tid;
	frame.ev = ev.encode();
	if (ev.has_exec_info()) {
		rbcs += read_rbc(hpc);
		frame.rbc = rbcs;
#ifdef HPC_ENABLE_EXTRA_PERF_COUNTERS
		frame.hw_interrupts = read_hw_int(hpc);
		frame.page_faults = read_page_faults(hpc);
		frame.insts = read_insts(hpc);


		frame->cs = read_cs(hpc);


#endif
		frame.recorded_regs = regs();
		if (record_extra_regs(ev)) {
			frame.recorded_extra_regs = extra_regs();
		}
	}

	if (should_dump_memory(this, frame)) {
		dump_process_memory(this, frame.global_time, "rec");
	}
	if (should_checksum(this, frame)) {
		checksum_process_memory(this, frame.global_time);
	}

	ofstream() << frame;
	if (ev.has_exec_info()) {
		reset_hpc(this, rr_flags()->max_rbc);
	}
}

void
Task::flush_inconsistent_state()
{
	rbcs = 0;
}

int64_t
Task::rbc_count()
{
	int64_t hpc_rbcs = read_rbc(hpc);
	if (hpc_rbcs > 0) {
		rbcs += hpc_rbcs;
		reset_hpc(this, 0);
	}
	return rbcs;
}

void
Task::set_rbc_count(int64_t count)
{
	rbcs = count;
}

void
Task::record_local(void* addr, ssize_t num_bytes, const void* data)
{
	maybe_flush_syscallbuf();

	struct raw_data buf;
	buf.addr = addr;
	buf.data.assign((const byte*)data, (const byte*)data + num_bytes);
	buf.ev = ev().encode();
	buf.global_time = ofstream().time();
	ofstream() << buf;
}

void
Task::record_remote(void* addr, ssize_t num_bytes)
{
	// We shouldn't be recording a scratch address.
	ASSERT(this, !addr || addr != scratch_ptr);

	maybe_flush_syscallbuf();

	struct raw_data buf;
	buf.addr = addr;
	buf.ev = ev().encode();
	buf.global_time = ofstream().time();
 	if (addr && num_bytes > 0) {
		buf.data.resize(num_bytes);
		read_bytes_helper(addr, buf.data.size(), buf.data.data());
	}
	ofstream() << buf;
}

void
Task::record_remote_str(void* str)
{
	maybe_flush_syscallbuf();

	string s = read_c_str(str);
	struct raw_data buf;
	buf.addr = str;
	// Record the \0 byte.
	buf.data.assign(s.c_str(), s.c_str() + s.size() + 1);
	buf.ev = ev().encode();
	buf.global_time = ofstream().time();
	ofstream() << buf;
}

string
Task::read_c_str(void* child_addr)
{
	// XXX handle invalid C strings
	string str;
	while (true) {
		// We're only guaranteed that [child_addr,
		// end_of_page) is mapped.
		void* end_of_page = ceil_page_size((byte*)child_addr + 1);
		ssize_t nbytes = (byte*)end_of_page - (byte*)child_addr;
		char buf[nbytes];

		read_bytes_helper(child_addr, nbytes,
				  reinterpret_cast<byte*>(buf));
		for (int i = 0; i < nbytes; ++i) {
			if ('\0' == buf[i]) {
				return str;
			}
			str += buf[i];
		}
		child_addr = end_of_page;
	}
}

intptr_t
Task::read_word(void* child_addr)
{
	long word;
	read_mem(child_addr, &word);
	return word;
}

size_t
Task::get_reg(uint8_t* buf, int regname, bool* defined)
{
	size_t num_bytes = regs().read_register(buf, regname, defined);
	if (!*defined) {
		num_bytes = extra_regs().read_register(buf, regname, defined);
	}
	return num_bytes;
}

const Registers&
Task::regs()
{
	if (!registers_known) {
		LOG(debug) <<"  (refreshing register cache)";
		xptrace(PTRACE_GETREGS, nullptr, &registers);
		registers_known = true;
	}
	return registers;
}

static unsigned int xsave_area_size = 0;

const ExtraRegisters&
Task::extra_regs()
{
	if (!extra_registers_known) {
		LOG(debug) <<"  (refreshing extra-register cache)";

		if (xsave_area_size == 0) {
			// We'll use the largest possible area all the time
			// even when it might not be needed. Simpler that way.
			unsigned int eax, ecx, edx;
			cpuid(CPUID_GETXSAVE, 0, &eax, &ecx, &edx);
			xsave_area_size = ecx;
		}
		extra_registers.data.resize(xsave_area_size);
		struct iovec vec = { extra_registers.data.data(), xsave_area_size };
		xptrace(PTRACE_GETREGSET, (void*)NT_X86_XSTATE, &vec);
		ASSERT(this, vec.iov_len == xsave_area_size)
			<<"Didn't get enough register data; expected "
			<< xsave_area_size <<" but got "<< vec.iov_len;
		extra_registers_known = true;
	}
	return extra_registers;
}

static ssize_t dr_user_word_offset(size_t i)
{
	assert(i < NUM_X86_DEBUG_REGS);
	return offsetof(struct user, u_debugreg[0]) + sizeof(void*) * i;
}

uintptr_t
Task::debug_status()
{
	return fallible_ptrace(PTRACE_PEEKUSER, (void*)dr_user_word_offset(6),
			       nullptr);
}

void*
Task::watchpoint_addr(size_t i)
{
	assert(i < NUM_X86_WATCHPOINTS);
	return (void*)fallible_ptrace(PTRACE_PEEKUSER,
				      (void*)dr_user_word_offset(i),
				      nullptr);
}

void
Task::remote_memcpy(void* dst, const void* src, size_t num_bytes)
{
	// XXX this could be more efficient
	byte buf[num_bytes];
	read_bytes_helper((void*)src, num_bytes, buf);
	write_bytes_helper(dst, num_bytes, buf);
}

bool
Task::resume_execution(ResumeRequest how, WaitRequest wait_how, int sig,
		       int64_t rbc_period)
{
	// Ensure no rbcs are lost when we reset_hpc below.
	rbc_count();
	if (session_replay) {
		reset_hpc(this, rbc_period);
	} else {
		assert(rbc_period == 0);
	}
	LOG(debug) <<"resuming execution with "<< ptrace_req_name(how);
	xptrace(how, nullptr, (void*)(uintptr_t)sig);
	registers_known = false;
	extra_registers_known = false;
	if (RESUME_NONBLOCKING == wait_how) {
		return true;
	}
	return wait();
}

Session&
Task::session()
{
	if (session_record) {
		return *session_record;
	}
	return *session_replay;
}

RecordSession&
Task::record_session()
{
	return *session_record;
}

ReplaySession&
Task::replay_session()
{
	return *session_replay;
}

const struct trace_frame&
Task::current_trace_frame()
{
	return replay_session().current_trace_frame();
}

ssize_t
Task::set_data_from_trace()
{
	struct raw_data buf;
	ifstream() >> buf;
	if (buf.addr && buf.data.size() > 0) {
		write_bytes_helper(buf.addr, buf.data.size(), buf.data.data());
	}
	return buf.data.size();
}

void
Task::set_return_value_from_trace()
{
	Registers r = regs();
	r.set_syscall_result(current_trace_frame().recorded_regs.syscall_result());
	set_regs(r);
}

void
Task::set_regs(const Registers& regs)
{
	registers = regs;
	xptrace(PTRACE_SETREGS, nullptr, &registers);
	registers_known = true;
}

void
Task::set_extra_regs(const ExtraRegisters& regs)
{
	ASSERT(this, !regs.empty())
		<<"Trying to set empty ExtraRegisters";
	extra_registers = regs;
	struct iovec vec = { extra_registers.data.data(), extra_registers.data.size() };
	xptrace(PTRACE_SETREGSET, (void*)NT_X86_XSTATE, &vec);
	extra_registers_known = true;
}

enum WatchBytesX86 {
    BYTES_1 = 0x00, BYTES_2 = 0x01, BYTES_4 = 0x03, BYTES_8 = 0x02
};
static WatchBytesX86 num_bytes_to_dr_len(size_t num_bytes)
{
	switch (num_bytes) {
	case 1:
		return BYTES_1;
	case 2:
		return BYTES_2;
	case 4:
		return BYTES_4;
	case 8:
		return BYTES_8;
	default:
		FATAL() <<"Unsupported breakpoint size "<< num_bytes;
		return WatchBytesX86(-1); // not reached
	}
}

bool
Task::set_debug_regs(const DebugRegs& regs)
{
	struct DebugControl {
		uintptr_t packed() { return *(uintptr_t*)this; }

		uintptr_t dr0_local : 1;
		uintptr_t dr0_global : 1;
		uintptr_t dr1_local : 1;
		uintptr_t dr1_global : 1;
		uintptr_t dr2_local : 1;
		uintptr_t dr2_global : 1;
		uintptr_t dr3_local : 1;
		uintptr_t dr3_global : 1;

		uintptr_t ignored : 8;

		WatchType dr0_type : 2;
		WatchBytesX86 dr0_len : 2;
		WatchType dr1_type : 2;
		WatchBytesX86 dr1_len : 2;
		WatchType dr2_type : 2;
		WatchBytesX86 dr2_len : 2;
		WatchType dr3_type : 2;
		WatchBytesX86 dr3_len : 2;
	} dr7 = { 0 };
	static_assert(sizeof(DebugControl) == sizeof(uintptr_t),
		      "Can't pack DebugControl");

	// Reset the debug status since we're about to change the set
	// of programmed watchpoints.
	xptrace(PTRACE_POKEUSER, (void*)dr_user_word_offset(6), 0);
	// Ensure that we clear the programmed watchpoints in case
	// enabling one of them fails.  We guarantee atomicity to the
	// caller.
	xptrace(PTRACE_POKEUSER, (void*)dr_user_word_offset(7), 0);
	if (regs.size() > NUM_X86_WATCHPOINTS) {
		return false;
	}

	size_t dr = 0;
	for (auto reg : regs) {
		if (fallible_ptrace(PTRACE_POKEUSER,
				    (void*)dr_user_word_offset(dr),
				    reg.addr)) {
			return false;
		}
		switch (dr++) {
#define CASE_ENABLE_DR(_dr7, _i, _reg)					\
			case _i:					\
				_dr7.dr## _i ##_local = 1;		\
				_dr7.dr## _i ##_type = _reg.type;	\
				_dr7.dr## _i ##_len = num_bytes_to_dr_len(_reg.num_bytes); \
				break
		CASE_ENABLE_DR(dr7, 0, reg);
		CASE_ENABLE_DR(dr7, 1, reg);
		CASE_ENABLE_DR(dr7, 2, reg);
		CASE_ENABLE_DR(dr7, 3, reg);
#undef CASE_ENABLE_DR
		default:
			FATAL() <<"There's no debug register "<< dr;
		}
	}
	return 0 == fallible_ptrace(PTRACE_POKEUSER,
				    (void*)dr_user_word_offset(7),
				    (void*)dr7.packed());
}

void
Task::set_thread_area(void* tls)
{
	read_mem(tls, &thread_area);
	thread_area_valid = true;
}

const struct user_desc*
Task::tls() const
{
	return thread_area_valid ? &thread_area : nullptr;
}

void
Task::set_tid_addr(void* tid_addr)
{
	LOG(debug) <<"updating cleartid futex to "<< tid_addr;
	tid_futex = tid_addr;
}

void
Task::signal_delivered(int sig)
{
	Sighandler& h = sighandlers->get(sig);
	if (h.resethand) {
		h = Sighandler();
	}
}

sig_handler_t
Task::signal_disposition(int sig) const
{
	return sighandlers->get(sig).sa.k_sa_handler;
}

bool
Task::signal_has_user_handler(int sig) const
{
	return sighandlers->get(sig).is_user_handler();
}

const kernel_sigaction&
Task::signal_action(int sig) const
{
	return sighandlers->get(sig).sa;
}

void
Task::stash_sig()
{
	assert(pending_sig());
	ASSERT(this, !has_stashed_sig())
		<< "Tried to stash "<< signalname(pending_sig()) <<" when "
		<< signalname(stashed_si.si_signo) <<" was already stashed.";
	stashed_wait_status = wait_status;
	get_siginfo(&stashed_si);
}

const siginfo_t&
Task::pop_stash_sig()
{
	assert(has_stashed_sig());
	force_status(stashed_wait_status);
	stashed_wait_status = 0;
	return stashed_si;
}

const string&
Task::trace_dir() const
{
	return trace_fstream().dir();
}

uint32_t
Task::trace_time() const
{
	return trace_fstream().time();
}

void
Task::update_prname(void* child_addr)
{
	struct { char chars[16]; } name;
	read_mem(child_addr, &name);
	name.chars[sizeof(name.chars) - 1] = '\0';
	prname = name.chars;
}

void
Task::update_sigaction(const Registers& regs)
{
	int sig = regs.arg1_signed();
	void* new_sigaction = (void*)regs.arg2();
	if (0 == regs.syscall_result() && new_sigaction) {
		// A new sighandler was installed.  Update our
		// sighandler table.
		// TODO: discard attempts to handle or ignore signals
		// that can't be by POSIX
		struct kernel_sigaction sa;
		read_mem(new_sigaction, &sa);
		sighandlers->get(sig) = Sighandler(sa);
	}
}

void
Task::update_sigmask(const Registers& regs)
{
	int how = regs.arg1_signed();
	void* setp = (void*)regs.arg2();

	if (SYSCALL_FAILED(regs.syscall_result_signed()) || !setp) {
		return;
	}

	ASSERT(this, (!syscallbuf_hdr || !syscallbuf_hdr->locked
		      || is_desched_sig_blocked()))
	       <<"syscallbuf is locked but SIGSYS isn't blocked";

	sig_set_t set;
	read_mem(setp, &set);

	// Update the blocked signals per |how|.
	switch (how) {
	case SIG_BLOCK:
		blocked_sigs |= set;
		break;
	case SIG_UNBLOCK:
		blocked_sigs &= ~set;
		break;
	case SIG_SETMASK:
		blocked_sigs = set;
		break;
	default:
		FATAL() <<"Unknown sigmask manipulator "<< how;
	}

	// In the syscallbuf, we rely on SIGSYS being raised when
	// tracees are descheduled in blocked syscalls.  But
	// unfortunately, if tracees block SIGSYS, then we don't get
	// notification of the pending signal and deadlock.  If we did
	// get those notifications, this code would be unnecessary.
	//
	// So we lock the syscallbuf while the desched signal is
	// blocked, which prevents the tracee from attempting a
	// buffered call.
	if (syscallbuf_hdr) {
		syscallbuf_hdr->locked = is_desched_sig_blocked();
	}
}

// The Task currently being wait()d on, or nullptr.
// |waiter_was_interrupted| if a PTRACE_INTERRUPT had to be applied to
// |waiter| to get it to stop.
static Task* waiter;
static bool waiter_was_interrupted;

static bool is_signal_triggered_by_ptrace_interrupt(int sig)
{
	switch (sig) {
	case SIGTRAP:
	// We sometimes see SIGSTOP at interrupts, though the
	// docs don't mention that.
	case SIGSTOP:
	// We sometimes see 0 too...
	case 0:
		return true;
	default:
		return false;
	}
}

bool
Task::wait()
{
	LOG(debug) <<"going into blocking waitpid("<< tid <<") ...";
	ASSERT(this, !unstable)
		<<"Don't wait for unstable tasks";

	// We only need this during recording.  If tracees go runaway
	// during replay, something else is at fault.
	bool enable_wait_interrupt = (RECORD == rr_flags()->option);
	if (enable_wait_interrupt) {
		waiter = this;
		// Where does the 3 seconds come from?  No especially
		// good reason.  We want this to be pretty high,
		// because it's a last-ditch recovery mechanism, not a
		// primary thread scheduler.  Though in theory the
		// PTRACE_INTERRUPT's shouldn't interfere with other
		// events, that's hard to test thoroughly so try to
		// avoid it.
		alarm(3);

		// Set the wait_status to a sentinel value so that we
		// can hopefully recognize race conditions in the
		// SIGALRM handler.
		wait_status = -1;
	}
	pid_t ret = waitpid(tid, &wait_status, __WALL);
	if (enable_wait_interrupt) {
		waiter = nullptr;
		alarm(0);
	}

	if (0 > ret && EINTR == errno) {
		LOG(debug) <<"  waitpid("<< tid <<") interrupted!";
		return false;
	}
	LOG(debug) <<"  waitpid("<< tid <<") returns "<< ret <<"; status "
		   << HEX(wait_status);
	ASSERT(this, tid == ret)
		<<"waitpid("<< tid <<") failed with "<< ret;;
	// If some other ptrace-stop happened to race with our
	// PTRACE_INTERRUPT, then let the other event win.  We only
	// want to interrupt tracees stuck running in userspace.
	if (waiter_was_interrupted && PTRACE_EVENT_STOP == ptrace_event()
	    && is_signal_triggered_by_ptrace_interrupt(WSTOPSIG(wait_status))) {
		LOG(warn) <<"Forced to PTRACE_INTERRUPT tracee";
		stashed_wait_status = wait_status =
				      (HPC_TIME_SLICE_SIGNAL << 8) | 0x7f;
		memset(&stashed_si, 0, sizeof(stashed_si));
		stashed_si.si_signo = HPC_TIME_SLICE_SIGNAL;
		stashed_si.si_fd = hpc->rbc.fd;
		stashed_si.si_code = POLL_IN;
		// Starve the runaway task of CPU time.  It just got
		// the equivalent of hundreds of time slices.
		succ_event_counter = numeric_limits<int>::max() / 2;
	} else if (waiter_was_interrupted) {
		LOG(warn) <<"  PTRACE_INTERRUPT raced with another event "
			  << HEX(wait_status);
	}
	waiter_was_interrupted = false;
	return true;
}

bool
Task::try_wait()
{
	pid_t ret = waitpid(tid, &wait_status, WNOHANG | __WALL | WSTOPPED);
	LOG(debug) <<"waitpid("<< tid <<", NOHANG) returns "<< ret
		   <<", status "<< HEX(wait_status);
	ASSERT(this, 0 <= ret)
		<<"waitpid("<< tid <<", NOHANG) failed with "<< ret;
	return ret == tid;
}

/**
 * Prepare this process and its ancestors for recording/replay by
 * preventing direct access to sources of nondeterminism, and ensuring
 * that rr bugs don't adversely affect the underlying system.
 */
static void set_up_process(void)
{
	int orig_pers;

	/* TODO tracees can probably undo some of the setup below
	 * ... */

	/* Disable address space layout randomization, for obvious
	 * reasons, and ensure that the layout is otherwise well-known
	 * ("COMPAT").  For not-understood reasons, "COMPAT" layouts
	 * have been observed in certain recording situations but not
	 * in replay, which causes divergence. */
	if (0 > (orig_pers = personality(0xffffffff))) {
		FATAL() <<"error getting personaity";
	}
	if (0 > personality(orig_pers | ADDR_NO_RANDOMIZE |
			    ADDR_COMPAT_LAYOUT)) {
		FATAL() <<"error disabling randomization";
	}
	/* Trap to the rr process if a 'rdtsc' instruction is issued.
	 * That allows rr to record the tsc and replay it
	 * deterministically. */
	if (0 > prctl(PR_SET_TSC, PR_TSC_SIGSEGV, 0, 0, 0)) {
		FATAL() <<"error setting up prctl";
	}
	// If the rr process dies, prevent runaway tracee processes
	// from dragging down the underlying system.
	//
	// TODO: this isn't inherited across fork().
	if (0 > prctl(PR_SET_PDEATHSIG, SIGKILL)) {
		FATAL() <<"Couldn't set parent-death signal";
	}
}

/*static*/int
Task::pending_sig_from_status(int status)
{
	if (status == 0) {
		return 0;
	}
	int sig = stop_sig_from_status(status);
	switch (sig) {
	case (SIGTRAP | 0x80):
		/* We ask for PTRACE_O_TRACESYSGOOD, so this was a
		 * trap for a syscall.  Pretend like it wasn't a
		 * signal. */
		return 0;
	case SIGTRAP:
		/* For a "normal" SIGTRAP, it's a ptrace trap if
		 * there's a ptrace event.  If so, pretend like we
		 * didn't get a signal.  Otherwise it was a genuine
		 * TRAP signal raised by something else (most likely a
		 * debugger breakpoint). */
		return ptrace_event_from_status(status) ? 0 : SIGTRAP;
	default:
		/* XXX do we really get the high bit set on some
		 * SEGVs? */
		return sig & ~0x80;
	}
}

Task*
Task::clone(int flags, void* stack, void* tls, void* cleartid_addr,
	    pid_t new_tid, pid_t new_rec_tid,
	    Session* other_session)
{
	auto& sess = other_session ? *other_session : session();
	Task* t = new Task(new_tid, new_rec_tid, priority);

	t->session_record = session_record;
	t->session_replay = session_replay;
	t->syscallbuf_lib_start = syscallbuf_lib_start;
	t->syscallbuf_lib_end = syscallbuf_lib_end;
	t->blocked_sigs = blocked_sigs;
	if (CLONE_SHARE_SIGHANDLERS & flags) {
		t->sighandlers = sighandlers;
	} else {
		auto sh = Sighandlers::create();
		t->sighandlers.swap(sh);
	}
	if (CLONE_SHARE_TASK_GROUP & flags) {
		t->tg = tg;
		tg->insert_task(t);
	} else {
		auto g = sess.create_tg(t);
		t->tg.swap(g);
	}
	if (CLONE_SHARE_VM & flags) {
		t->as = as;
	} else {
		t->as = sess.clone(as);
	}
	if (stack) {
		const Mapping& m = 
			t->as->mapping_of((byte*)stack - page_size(),
					  page_size()).first;
		LOG(debug) <<"mapping stack for "<< new_tid <<" at "<< m;
		t->as->map(m.start, m.num_bytes(), m.prot, m.flags,
			   m.offset, MappableResource::stack(new_tid));
		t->top_of_stack = stack;
	}
	// Clone children, both thread and fork, inherit the parent
	// prname.
	t->prname = prname;
	if (CLONE_CLEARTID & flags) {
		LOG(debug) <<"cleartid futex is "<< cleartid_addr;
		assert(cleartid_addr);
		t->tid_futex = cleartid_addr;
	} else {
		LOG(debug) <<"(clone child not enabling CLEARTID)";
	}

	// wait() before trying to do anything that might need to
	// use ptrace to access memory
	t->wait();

	t->open_mem_fd_if_needed();
	if (CLONE_SET_TLS & flags) {
		t->set_thread_area(tls);
	}

	t->as->insert_task(t);
	return t;
}

Task*
Task::os_fork_into(Session* session)
{
	AutoRemoteSyscalls remote(this);
	Task* child = os_clone(this, session, remote, rec_tid,
			       // Most likely, we'll be setting up a
			       // CLEARTID futex.  That's not done
			       // here, but rather later in
			       // |copy_state()|.
			       //
			       // We also don't use any of the SETTID
			       // flags because that earlier work will
			       // be copied by fork()ing the address
			       // space.
			       SIGCHLD);
	// When we forked ourselves, the child inherited the setup we
	// did to make the clone() call.  So we have to "finish" the
	// remote calls (i.e. undo fudged state) in the child too,
	// even though we never made any syscalls there.
	remote.restore_state_to(child);
	return child;
}

Task*
Task::os_clone_into(Task* task_leader, AutoRemoteSyscalls& remote)
{
	return os_clone(task_leader, &task_leader->session(), remote,
			rec_tid,
			// We don't actually /need/ to specify the
			// SIGHAND/SYSVMEM flags because those things
			// are emulated in the tracee.  But we use the
			// same flags as glibc to be on the safe side
			// wrt kernel bugs.
			//
			// We don't pass CLONE_SETTLS here *only*
			// because we'll do it later in
			// |copy_state()|.
			//
			// See |os_fork_into()| above for discussion
			// of the CTID flags.
			(CLONE_VM | CLONE_FS | CLONE_FILES |
			 CLONE_SIGHAND | CLONE_THREAD |
			 CLONE_SYSVSEM),
			stack());
}

void
Task::copy_state(Task* from)
{
	long err;
	set_regs(from->regs());
	{
		AutoRemoteSyscalls remote(this);
		{
			char prname[16];
			strncpy(prname, from->name().c_str(), sizeof(prname));
			AutoRestoreMem remote_prname(remote,
						     (const byte*)prname,
						     sizeof(prname));
			LOG(debug) <<"    setting name to "<< prname;
			err = remote.syscall(SYS_prctl, PR_SET_NAME,
					     static_cast<void*>(remote_prname));
			ASSERT(this, 0 == err);
			update_prname(remote_prname);
		}

		if (from->robust_list()) {
			set_robust_list(from->robust_list(),
					from->robust_list_len());
			LOG(debug) <<"    setting robust-list "
				   << this->robust_list()
				   <<" (size "<< this->robust_list_len() <<")";
			err = remote.syscall(SYS_set_robust_list,
					     this->robust_list(),
					     this->robust_list_len());
			ASSERT(this, 0 == err);
		}

		if (const struct user_desc* tls = from->tls()) {
			AutoRestoreMem remote_tls(remote, (const byte*)tls,
						  sizeof(*tls));
			LOG(debug) <<"    setting tls "<< remote_tls;
			err = remote.syscall(SYS_set_thread_area,
					     static_cast<void*>(remote_tls));
			ASSERT(this, 0 == err);
			set_thread_area(remote_tls);
		}

		if (void* ctid = from->tid_addr()) {
			err = remote.syscall(SYS_set_tid_address, ctid);
			ASSERT(this, tid == err);
		}

		if (from->syscallbuf_child) {
			// All these fields are preserved by the fork.
			traced_syscall_ip = from->traced_syscall_ip;
			untraced_syscall_ip = from->untraced_syscall_ip;
			syscallbuf_child = from->syscallbuf_child;
			num_syscallbuf_bytes = from->num_syscallbuf_bytes;
			desched_fd_child = from->desched_fd_child;

			// The syscallbuf is mapped as a shared
			// segment between rr and the tracee.  So we
			// have to unmap it, create a copy, and then
			// re-map the copy in rr and the tracee.
			void* map_hint = from->syscallbuf_child;
			destroy_buffers(DESTROY_SYSCALLBUF);
			destroy_local_buffers();

			syscallbuf_child = init_syscall_buffer(remote,
							       map_hint);
			ASSERT(this,
			       from->syscallbuf_child == syscallbuf_child);
			// Ensure the copied syscallbuf has the same contents
			// as the old one, for consistency checking.
			memcpy(syscallbuf_hdr, from->syscallbuf_hdr,
			       num_syscallbuf_bytes);
		}
	}
	// The scratch buffer (for now) is merely a private mapping in
	// the remote task.  The CoW copy made by fork()'ing the
	// address space has the semantics we want.  It's not used in
	// replay anyway.
	scratch_ptr = from->scratch_ptr;
	scratch_size = from->scratch_size;

	// Whatever |from|'s last wait status was is what ours would
	// have been.
	wait_status = from->wait_status;

	// These are only metadata that have been inferred from the
	// series of syscalls made by the trace so far.
	blocked_sigs = from->blocked_sigs;
	pending_events = from->pending_events;

	rbcs = from->rbc_count();
	tid_futex = from->tid_futex;

	reset_hpc(this, 0);
}

void
Task::destroy_local_buffers()
{
	close(desched_fd);
	munmap(syscallbuf_hdr, num_syscallbuf_bytes);
}

void
Task::detach_and_reap()
{
	// child_mem_fd needs to be valid since we won't be able to open
	// it for futex_wait below after we've detached.
	ASSERT(this, as->mem_fd() >= 0);

	// XXX: why do we detach before harvesting?
	fallible_ptrace(PTRACE_DETACH, nullptr, nullptr);
	if (unstable) {
		// In addition to problems described in the long
		// comment at the prototype of this function, unstable
		// exits may result in the kernel *not* clearing the
		// futex, for example for fatal signals.  So we would
		// deadlock waiting on the futex.
		LOG(warn) << tid <<" is unstable; not blocking on its termination";
		return;
	}

	LOG(debug) <<"Joining with exiting "<< tid <<" ...";
	while (true) {
		int err = waitpid(tid, &wait_status, __WALL);
		if (-1 == err && ECHILD == errno) {
			LOG(debug) <<" ... ECHILD";
			break;
		} else if (-1 == err) {
			assert(EINTR == errno);
		}
		if (err == tid && (exited() || signaled())) {
			LOG(debug) <<" ... exited with status "
				   << HEX(wait_status);
			break;
		} else if (err == tid) {
			assert(PTRACE_EVENT_EXIT == ptrace_event());
		}
	}

	if (tid_futex && as->task_set().size() > 0) {
		// clone()'d tasks can have a pid_t* |ctid| argument
		// that's written with the new task's pid.  That
		// pointer can also be used as a futex: when the task
		// dies, the original ctid value is cleared and a
		// FUTEX_WAKE is done on the address. So
		// pthread_join() is basically a standard futex wait
		// loop.
		LOG(debug) <<"  waiting for tid futex "<< tid_futex
			   <<" to be cleared ...";
		futex_wait(tid_futex, 0);
	} else if(tid_futex) {
		// There are no other live tasks in this address
		// space, which means the address space just died
		// along with our exit.  So we can't read the futex.
		LOG(debug) <<"  (can't futex_wait last task in vm)";
	}
}

long
Task::fallible_ptrace(int request, void* addr, void* data)
{
	return ptrace(__ptrace_request(request), tid, addr, data);
}

void
Task::open_mem_fd()
{
	if (as->mem_fd() >= 0) {
		close(as->mem_fd());
		// Use ptrace to read/write during open_mem_fd
		as->set_mem_fd(-1);
	}

	// We could try opening /proc/<pid>/mem directly first and
	// only do this dance if that fails. But it's simpler to
	// always take this path, and gives better test coverage.
	static const char path[] = "/proc/self/mem";

	AutoRemoteSyscalls remote(this);
	long remote_fd;
	{
		AutoRestoreMem remote_path(remote,
					   (const byte*)path, sizeof(path));
		remote_fd = remote.syscall(SYS_open,
					   static_cast<void*>(remote_path), O_RDWR);
		assert(remote_fd >= 0);
	}

	as->set_mem_fd(retrieve_fd(remote, remote_fd));
	assert(as->mem_fd() >= 0);

	long err = remote.syscall(SYS_close, remote_fd);
	assert(!err);
}

void
Task::open_mem_fd_if_needed()
{
	if (as->mem_fd() < 0) {
		open_mem_fd();
	}
}

void*
Task::init_syscall_buffer(AutoRemoteSyscalls& remote, void* map_hint)
{
	// Create the segment we'll share with the tracee.
	char shmem_name[PATH_MAX];
	format_syscallbuf_shmem_path(tid, shmem_name);
	int shmem_fd = create_shmem_segment(shmem_name,
					    SYSCALLBUF_BUFFER_SIZE);
	// Map the shmem fd in the child.
	int child_shmem_fd;
	{
		char proc_path[PATH_MAX];
		snprintf(proc_path, sizeof(proc_path), "/proc/%d/fd/%d",
			 getpid(), shmem_fd);
		AutoRestoreMem child_path(remote, proc_path);
		child_shmem_fd = remote.syscall(SYS_open,
						static_cast<void*>(child_path),
						O_RDWR, 0600);
		if (0 > child_shmem_fd) {
			errno = -child_shmem_fd;
			FATAL() <<"Failed to open("<< proc_path <<") in tracee";
		}
	}

	// Map the segment in ours and the tracee's address spaces.
	void* map_addr;
	num_syscallbuf_bytes = SYSCALLBUF_BUFFER_SIZE;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_SHARED;
	off64_t offset_pages = 0;
	if ((void*)-1 == (map_addr = mmap(nullptr, num_syscallbuf_bytes,
					  prot, flags,
					  shmem_fd, offset_pages))) {
		FATAL() <<"Failed to mmap shmem region";
	}
	void* child_map_addr = (byte*)remote.syscall(SYS_mmap2,
						     map_hint,
						     num_syscallbuf_bytes,
						     prot, flags,
						     child_shmem_fd,
						     offset_pages);
	syscallbuf_child = child_map_addr;
	syscallbuf_hdr = (struct syscallbuf_hdr*)map_addr;
	// No entries to begin with.
	memset(syscallbuf_hdr, 0, sizeof(*syscallbuf_hdr));

	vm()->map(child_map_addr, num_syscallbuf_bytes,
		  prot, flags, page_size() * offset_pages,
		  MappableResource::syscallbuf(rec_tid, shmem_fd,
					       shmem_name));

	close(shmem_fd);
	remote.syscall(SYS_close, child_shmem_fd);

	return child_map_addr;
}

/**
 * Block until receiving an fd the other side of |sock| sent us, then
 * return the fd (valid in this address space).  Optionally return the
 * remote fd number that was shared to us in |remote_fdno|.
 *
 * XXX replace this with receive_fd().
 */
static int recv_fd(int sock, int* remote_fdno)
{
	struct msghdr msg;
	int fd, remote_fd;
	struct iovec data;
	struct cmsghdr* cmsg;
	char cmsgbuf[CMSG_SPACE(sizeof(int))];

	memset(&msg, 0, sizeof(msg));

	data.iov_base = &remote_fd;
	data.iov_len = sizeof(remote_fd);
	msg.msg_iov = &data;
	msg.msg_iovlen = 1;

	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	if (0 >= recvmsg(sock, &msg, 0)) {
		FATAL() <<"Failed to receive fd";
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	assert(cmsg->cmsg_level == SOL_SOCKET
	       && cmsg->cmsg_type == SCM_RIGHTS);

	fd = *(int*)CMSG_DATA(cmsg);
	if (remote_fdno) {
		*remote_fdno = remote_fd;
	}

	return fd;
}

void
Task::init_desched_fd(AutoRemoteSyscalls& remote,
		      struct rrcall_init_buffers_params* args,
		      int share_desched_fd)
{
	// NB: this implementation could be vastly simplfied if we
	// were able to share the tracee's desched fd through its
	// /proc/fd entry.  However, as of linux 3.13.9, this doesn't
	// work.  So instead we do the SCM_RIGHTS dance.
	if (!share_desched_fd) {
		desched_fd_child = REPLAY_DESCHED_EVENT_FD;
		return;
	}

	// NB: the sockaddr prepared by the child uses the recorded
	// tid, so always must here. */
	struct sockaddr_un addr;
	prepare_syscallbuf_socket_addr(&addr, rec_tid);

	// Bind the server socket, but don't start listening yet.
	int listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (::bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr))) {
		FATAL() <<"Failed to bind listen socket";
	}
	if (listen(listen_sock, 1)) {
		FATAL() <<"Failed to mark listening for listen socket";
	}

	// Initiate tracee connect(), but don't wait for it to
	// finish.
	write_socketcall_args(this, args->args_vec, AF_UNIX, SOCK_STREAM, 0);
	int child_sock = remote.syscall(SYS_socketcall,
					SYS_SOCKET, args->args_vec);
	if (0 > child_sock) {
		errno = -child_sock;
		FATAL() <<"Failed to create child socket";
	}
	write_socketcall_args(this, args->args_vec, child_sock,
			      (uintptr_t)args->sockaddr,
			      sizeof(*args->sockaddr));

	Registers callregs = remote.regs();
	callregs.set_arg1(SYS_CONNECT);
	callregs.set_arg2((uintptr_t)args->args_vec);
	remote.syscall_helper(AutoRemoteSyscalls::DONT_WAIT, SYS_socketcall,
			      callregs);
	// Now the child is waiting for us to accept it.

	// Accept the child's connection and finish its syscall.
	//
	// XXX could be really anal and check credentials of
	// connecting endpoint ...
	int sock = accept(listen_sock, NULL, NULL);
	int child_ret;
	if ((child_ret = remote.wait_syscall(SYS_socketcall))) {
		errno = -child_ret;
		FATAL() <<"Failed to connect() in tracee";
	}
	// Socket name not needed anymore.
	unlink(addr.sun_path);

	// Pull the puppet strings to have the child share its desched
	// counter with us.  Similarly to above, we DONT_WAIT on the
	// call to finish, since it's likely not defined whether the
	// sendmsg() may block on our recvmsg()ing what the tracee
	// sent us (in which case we would deadlock with the tracee).
	write_socketcall_args(this, args->args_vec, child_sock,
			      (uintptr_t)args->msg, 0);
	callregs = remote.regs();
	callregs.set_arg1(SYS_SENDMSG);
	callregs.set_arg2((uintptr_t)args->args_vec);
	remote.syscall_helper(AutoRemoteSyscalls::DONT_WAIT, SYS_socketcall,
			      callregs);
	// Child may be waiting on our recvmsg().

	// Read the shared fd and finish the child's syscall.
	desched_fd = recv_fd(sock, &desched_fd_child);
	if (0 >= remote.wait_syscall(SYS_socketcall)) {
		errno = -child_ret;
		FATAL() <<"Failed to sendmsg() in tracee";
	}

	// Socket magic is now done.
	close(listen_sock);
	close(sock);
	remote.syscall(SYS_close, child_sock);
}


bool
Task::is_desched_sig_blocked()
{
	return is_sig_blocked(SYSCALLBUF_DESCHED_SIGNAL);
}

/** Send |sig| to task |tid| within group |tgid|. */
static int sys_tgkill(pid_t tgid, pid_t tid, int sig)
{
	return syscall(SYS_tgkill, tgid, tid, SIGKILL);
}

void
Task::kill()
{
	LOG(debug) <<"sending SIGKILL to "<< tid <<" ...";
	sys_tgkill(real_tgid(), tid, SIGKILL);

	if (!unstable) {
		wait();

		if (WIFSIGNALED(wait_status)) {
			assert(SIGKILL == WTERMSIG(wait_status));
			// The task is already dead and reaped, so skip any
			// waitpid()'ing during cleanup.
			unstable = 1;
		}
	}

	// Don't attempt to synchonize on the cleartid futex.  We
	// won't be able to reliably read it, and it's pointless
	// anyway.
	tid_futex = nullptr;
}

void
Task::maybe_flush_syscallbuf()
{
	if (EV_SYSCALLBUF_FLUSH == ev().type()) {
		// Already flushing.
		return;
	}
	if (!syscallbuf_hdr
	    || 0 == syscallbuf_hdr->num_rec_bytes 
	    || delay_syscallbuf_flush) {
		// No syscallbuf or no records.  No flushing to do.
		return;
	}
	// Write the entire buffer in one shot without parsing it,
	// because replay will take care of that.
	push_event(Event(EV_SYSCALLBUF_FLUSH, NO_EXEC_INFO));
	record_local(syscallbuf_child,
		     // Record the header for consistency checking.
		     syscallbuf_hdr->num_rec_bytes + sizeof(*syscallbuf_hdr),
		     syscallbuf_hdr);
	record_current_event();
	pop_event(EV_SYSCALLBUF_FLUSH);

	// Reset header.
	assert(!syscallbuf_hdr->abort_commit);
	if (!delay_syscallbuf_reset) {
		syscallbuf_hdr->num_rec_bytes = 0;
	}
	flushed_syscallbuf = 1;
}

static off64_t to_offset(void* addr)
{
	off64_t offset = (uintptr_t)addr;
	assert(offset <= off64_t(numeric_limits<unsigned long>::max()));
	return offset;
}

ssize_t
Task::read_bytes_ptrace(void* addr, ssize_t buf_size, byte* buf)
{
	ssize_t nread = 0;
	// XXX 64-bit porting issue
	int word_size = sizeof(long);
	errno = 0;
	// Only read aligned words. This ensures we can always read the last
	// byte before an unmapped region.
	while (nread < buf_size) {
		uintptr_t start = uintptr_t(addr) + nread;
		uintptr_t start_word = start & ~(word_size - 1);
		uintptr_t end_word = start_word + word_size;
		uintptr_t length =
			std::min(end_word - start, uintptr_t(buf_size - nread));

		long v = fallible_ptrace(PTRACE_PEEKDATA,
			reinterpret_cast<void*>(start_word), NULL);
		if (errno) {
			break;
		}
		memcpy(buf + nread,
		       reinterpret_cast<byte*>(&v) + (start - start_word),
		       length);
		nread += length;
	}

	return nread;
}

ssize_t
Task::write_bytes_ptrace(void* addr, ssize_t buf_size, const byte* buf)
{
	ssize_t nwritten = 0;
	// XXX 64-bit porting issue
	unsigned int word_size = sizeof(long);
	errno = 0;
	// Only write aligned words. This ensures we can always write the last
	// byte before an unmapped region.
	while (nwritten < buf_size) {
		uintptr_t start = uintptr_t(addr) + nwritten;
		uintptr_t start_word = start & ~(word_size - 1);
		uintptr_t end_word = start_word + word_size;
		uintptr_t length = std::min(end_word - start, uintptr_t(buf_size - nwritten));

		long v;
		if (length < word_size) {
			v = fallible_ptrace(PTRACE_PEEKDATA, (void*)start_word, NULL);
			if (errno) {
				break;
			}
		}
		memcpy(reinterpret_cast<byte*>(&v) + (start - start_word),
		       buf + nwritten,
		       length);
		fallible_ptrace(PTRACE_POKEDATA,
			reinterpret_cast<void*>(start_word),
			reinterpret_cast<void*>(v));
		nwritten += length;
	}

	return nwritten;
}

ssize_t
Task::read_bytes_fallible(void* addr, ssize_t buf_size, byte* buf)
{
	ASSERT(this, buf_size >= 0) <<"Invalid buf_size "<< buf_size;
	if (0 == buf_size) {
		return 0;
	}

	if (as->mem_fd() < 0) {
		return read_bytes_ptrace(addr, buf_size, buf);
	}

	errno = 0;
	ssize_t nread = pread64(as->mem_fd(), buf, buf_size, to_offset(addr));
	// We open the mem_fd just after being notified of
	// exec(), when the Task is created.  Trying to read from that
	// fd seems to return 0 with errno 0.  Reopening the mem fd
	// allows the pwrite to succeed.  It seems that the first mem
	// fd we open, very early in exec, refers to some resource
	// that's different than the one we see after reopening the
	// fd, after exec.
	if (0 == nread && 0 == errno) {
		open_mem_fd();
		return read_bytes_fallible(addr, buf_size, buf);
	}
	return nread;
}

void
Task::read_bytes_helper(void* addr, ssize_t buf_size, byte* buf)
{
	ssize_t nread = read_bytes_fallible(addr, buf_size, buf);
	ASSERT(this, nread == buf_size)
		<<"Should have read "<< buf_size <<" bytes from "<< addr
		<<", but only read "<< nread;
}

void
Task::write_bytes_helper(void* addr, ssize_t buf_size, const byte* buf)
{
	ASSERT(this, buf_size >= 0) <<"Invalid buf_size "<< buf_size;
	if (0 == buf_size) {
		return;
	}

	if (as->mem_fd() < 0) {
		write_bytes_ptrace(addr, buf_size, buf);
		return;
	}

	errno = 0;
	ssize_t nwritten = pwrite64(as->mem_fd(), buf, buf_size,
				    to_offset(addr));
	// See comment in read_bytes_helper().
	if (0 == nwritten && 0 == errno) {
		open_mem_fd();
		return write_bytes_helper(addr, buf_size, buf);
	}
	ASSERT(this, nwritten == buf_size)
		<<"Should have written "<< buf_size <<" bytes to "<< addr
		<<", but only wrote "<< nwritten;
}

TraceFstream&
Task::trace_fstream()
{
	if (session_record) {
		return session_record->ofstream();
	}
	return session_replay->ifstream();
}

const TraceFstream&
Task::trace_fstream() const
{
	if (session_record) {
		return session_record->ofstream();
	}
	return session_replay->ifstream();
}

void
Task::xptrace(int request, void* addr, void* data)
{
	long ret = fallible_ptrace(request, addr, data);
	ASSERT(this, 0 == ret)
		<< "ptrace("<< ptrace_req_name(request) <<", "<< tid
		<<", addr="<< addr <<", data="<< data <<") failed";
}

/*static*/ void
Task::handle_runaway(int sig)
{
	LOG(debug) <<"SIGALRM fired; runaway tracee";
	if (!waiter || -1 != waiter->wait_status) {
		LOG(debug) <<"  ... false alarm, race condition";
		return;
	}
	waiter->xptrace(PTRACE_INTERRUPT, nullptr, nullptr);
	waiter_was_interrupted = true;
}

bool
Task::clone_syscall_is_complete()
{
	int event = ptrace_event();
	if (PTRACE_EVENT_CLONE == event || PTRACE_EVENT_FORK == event) {
		return true;
	}
	ASSERT(this, !event)
		<<"Unexpected ptrace event "<< ptrace_event_name(event);

	// EAGAIN has been observed here. Theoretically that should only
	// happen when "too many processes are already running", but who
	// knows...
	// XXX why would ENOSYS happen here?
	intptr_t result = regs().syscall_result_signed();
	ASSERT(this, SYSCALL_MAY_RESTART(result)
			|| -ENOSYS == result || -EAGAIN == result)
		<<"Unexpected task status "<< HEX(status())
		<<" (syscall result:"<< regs().syscall_result_signed() <<")";
	return false;
}

/*static*/ Task*
Task::os_clone(Task* parent, Session* session, AutoRemoteSyscalls& remote,
	       pid_t rec_child_tid, unsigned base_flags, void* stack,
	       void* ptid, void* tls, void* ctid)
{
	// NB: reference the glibc i386 clone.S implementation for
	// placement of clone syscall args.  The man page is incorrect
	// and the linux source is confusing.
	remote.syscall(SYS_clone, base_flags, stack, ptid, tls, ctid);
	while (!parent->clone_syscall_is_complete()) {
		parent->cont_syscall();
	}

	parent->cont_syscall();
	long new_tid = parent->regs().syscall_result_signed();
	if (0 > new_tid) {
		FATAL() <<"Failed to clone("<< parent->tid <<") -> "<< new_tid
			<< ": "<< strerror(-new_tid);
	}
	Task* child = parent->clone(clone_flags_to_task_flags(base_flags),
				    stack, tls, ctid,
				    new_tid, rec_child_tid, session);
	return child;
}

/*static*/ Task*
Task::spawn(const struct args_env& ae, Session& session, pid_t rec_tid)
{
	assert(session.tasks().size() == 0);

	pid_t tid = fork();
	if (0 == tid) {
		// Set current working directory to the cwd used during
		// recording. The main effect of this is to resolve relative
		// paths in the following execvpe correctly during replay.
		chdir(ae.cwd.c_str());
		set_up_process();
		// The preceding code must run before sending SIGSTOP here,
		// since after SIGSTOP replay emulates almost all syscalls, but
		// we need the above syscalls to run "for real".

		// Signal to tracer that we're configured.
		::kill(getpid(), SIGSTOP);

		// We do a small amount of dummy work here to retire
		// some branches in order to ensure that the rbc is
		// non-zero.  The tracer can then check the rbc value
		// at the first ptrace-trap to see if it seems to be
		// working.
		int start = rand() % 5;
		int num_its = start + 5;
		int sum = 0;
		for (int i = start; i < num_its; ++i) {
			sum += i;
		}
		syscall(SYS_write, -1, &sum, sizeof(sum));

		EnvironmentBugDetector::run_detection_code();

		execvpe(ae.exe_image.c_str(), ae.argv.data(), ae.envp.data());
		FATAL() <<"Failed to exec '"<< ae.exe_image.c_str() <<"'";
	}

	signal(SIGALRM, Task::handle_runaway);

	Task* t = new Task(tid, rec_tid, 0);
	// The very first task we fork inherits the signal
	// dispositions of the current OS process (which should all be
	// default at this point, but ...).  From there on, new tasks
	// will transitively inherit from this first task.
	auto sh = Sighandlers::create();
	sh->init_from_current_process();
	t->sighandlers.swap(sh);
	// Don't use the POSIX wrapper, because it doesn't necessarily
	// read the entire sigset tracked by the kernel.
	if (::syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL,
		      &t->blocked_sigs, sizeof(t->blocked_sigs))) {
		FATAL() <<"Failed to read blocked signals";
	}
	auto g = session.create_tg(t);
	t->tg.swap(g);
	auto as = session.create_vm(t, ae.exe_image);
	t->as.swap(as);

	// Sync with the child process.
	int options = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK |
		PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE |
		PTRACE_O_TRACEEXEC | PTRACE_O_TRACEVFORKDONE |
		PTRACE_O_TRACEEXIT | PTRACE_O_TRACESECCOMP |
		RR_PTRACE_O_EXITKILL;
	int ret = t->fallible_ptrace(PTRACE_SEIZE, nullptr, (void*)options);
	if (ret < 0 && errno == EINVAL) {
		// PTRACE_O_EXITKILL was added in kernel 3.8, and we only need
		// it for more robust cleanup, so tolerate not having it.
		options &= ~RR_PTRACE_O_EXITKILL;
		ret = t->fallible_ptrace(PTRACE_SEIZE, nullptr, (void*)options);
	}
	ASSERT(t, !ret) <<"PTRACE_SEIZE failed for tid %d"<< t->tid;

	// PTRACE_SEIZE is fundamentally racy by design.  We depend on
	// stopping the tracee at a known location, so raciness is
	// bad.  To resolve the race condition, we just keep running
	// the tracee until it reaches the known-safe starting point.
	//
	// Alternatively, it would be possible to remove the
	// requirement of the tracing beginning from a known point.
	while (true) {
		t->wait();
		if (SIGSTOP == t->stop_sig()) {
			break;
		}
		t->cont_nonblocking();
	}
	t->force_status(0);
	t->open_mem_fd();
	return t;
}

const char*
Task::syscallname(int syscall) const
{
	return ::syscallname(syscall, x86);
}
