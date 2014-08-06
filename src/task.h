/* -*- Mode: C++; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#ifndef RR_TASK_H_
#define RR_TASK_H_

#include <memory>

#include "preload/syscall_buffer.h"

#include "event.h"
#include "registers.h"
#include "trace.h"
#include "util.h"
#include "vm.h"

class AutoRemoteSyscalls;
class Session;
class RecordSession;
class ReplaySession;
struct Sighandlers;
class Task;
struct TaskGroup;

/** Add the signal |_sig| to |_set|. */
#define SIGSET_ADD(_set, _sig)			\
	((_set) | (1 << ((_sig) - 1)))

struct syscallbuf_hdr;
struct syscallbuf_record;

/* (There are various GNU and BSD extensions that define this, but
 * it's not worth the bother to sort those out.) */
typedef void (*sig_handler_t)(int);

/* We need to complement sigsets in order to update the Task blocked
 * set, but POSIX doesn't appear to define a convenient helper.  So we
 * define our own linux-compatible sig_set_t and use bit operators to
 * manipulate sigsets. */
typedef uint64_t sig_set_t;
static_assert(_NSIG / 8 == sizeof(sig_set_t), "Update sig_set_t for _NSIG.");

/**
 * The kernel SYS_sigaction ABI is different from the libc API; this
 * is the kernel layout.  We see these at SYS_sigaction traps.
 */
struct kernel_sigaction {
	sig_handler_t k_sa_handler;
	unsigned long sa_flags;
	void (*sa_restorer) (void);
	sigset_t sa_mask;
};

/**
 * Tracks a group of tasks with an associated ID, set from the
 * original "thread group leader", the child of |fork()| which became
 * the ancestor of all other threads in the group.  Each constituent
 * task must own a reference to this.
 */
struct TaskGroup : public HasTaskSet {
	friend class Session;

	typedef std::shared_ptr<TaskGroup> shr_ptr;

	/** See |Task::destabilize_task_group()|. */
	void destabilize();

	const pid_t tgid;
	const pid_t real_tgid;

	int exit_code;

private:
	TaskGroup(pid_t tgid, pid_t real_tgid);

	TaskGroup(const TaskGroup&) = delete;
	TaskGroup operator=(const TaskGroup&) = delete;
};

enum CloneFlags {
	/**
	 * The child gets a semantic copy of all parent resources (and
	 * becomes a new task group).  This is the semantics of the
	 * fork() syscall.
	 */
	CLONE_SHARE_NOTHING = 0,
	/**
	 * Child will share the table of signal dispositions with its
	 * parent.
	 */
	CLONE_SHARE_SIGHANDLERS = 1 << 0,
	/** Child will join its parent's task group. */
	CLONE_SHARE_TASK_GROUP = 1 << 1,
	/** Child will share its parent's address space. */
	CLONE_SHARE_VM = 1 << 2,
	/** Kernel will clear and notify tid futex on task exit. */
	CLONE_CLEARTID = 1 << 3,
	// Set the thread area to what's specified by the |tls| arg.
	CLONE_SET_TLS = 1 << 4,
};

/**
 * Enumeration of ways to resume execution.  See the ptrace manual for
 * details of the semantics of these.
 *
 * We define a new datatype because the PTRACE_SYSEMU* requests aren't
 * part of the official ptrace API, and we want to use a strong type
 * for these resume requests to ensure callers don't confuse their
 * arguments.
 */
enum ResumeRequest {
	RESUME_CONT = PTRACE_CONT,
	RESUME_SINGLESTEP = PTRACE_SINGLESTEP,
	RESUME_SYSCALL = PTRACE_SYSCALL,
	RESUME_SYSEMU = PTRACE_SYSEMU,
	RESUME_SYSEMU_SINGLESTEP = PTRACE_SYSEMU_SINGLESTEP,
};
enum WaitRequest {
	// After resuming, blocking-waitpid() until tracee status
	// changes.
	RESUME_WAIT,
	// Don't wait after resuming.
	RESUME_NONBLOCKING
};

enum { SHARE_DESCHED_EVENT_FD = 1, DONT_SHARE_DESCHED_EVENT_FD = 0 };

enum DestroyBufferFlags {
	DESTROY_SCRATCH = 1 << 0,
	DESTROY_SYSCALLBUF = 1 << 1,
};

/**
 * A "task" is a task in the linux usage: the unit of scheduling.  (OS
 * people sometimes call this a "thread control block".)  Multiple
 * tasks may share the same address space and file descriptors, in
 * which case they're commonly called "threads".  Or two tasks may
 * have their own address spaces and file descriptors, in which case
 * they're called "processes".  Both look the same to rr (on linux),
 * so no distinction is made here.
 */
class Task {
	friend class Session;
	friend class RecordSession;
	friend class ReplaySession;
public:
	typedef std::vector<WatchConfig> DebugRegs;

	~Task();

	/**
	 * Return true iff this is at an execution state where
	 * resuming execution may lead to the restart of an
	 * interrupted syscall.
	 *
	 * For example, if a signal without a user handler is about to
	 * be delivered to this just after a syscall interruption,
	 * then delivering the signal may restart the first syscall
	 * and this method will return true.
	 */
	bool at_may_restart_syscall() const;

	/**
	 * Continue according to the semantics implied by the helper's
	 * name.  See the ptrace manual for details of semantics.  If
	 * |sig| is nonzero, it's delivered to this as part of the
	 * resume request.
	 *
	 * By default, wait for status to change after resuming,
	 * before returning.  Return true if successful, false if
	 * interrupted.  Don't wait for status change in the
	 * "_nonblocking()" variants.
	 */
	bool cont(int sig=0) {
		return resume_execution(RESUME_CONT, RESUME_WAIT, sig);
	}
	void cont_nonblocking(int sig=0) {
		resume_execution(RESUME_CONT, RESUME_NONBLOCKING, sig);
	}
	bool cont_singlestep(int sig=0) {
		return resume_execution(RESUME_SINGLESTEP, RESUME_WAIT, sig);
	}
	bool cont_syscall(int sig=0) {
		return resume_execution(RESUME_SYSCALL, RESUME_WAIT, sig);
	}
	void cont_syscall_nonblocking(int sig=0) {
		resume_execution(RESUME_SYSCALL, RESUME_NONBLOCKING, sig);
	}
	bool cont_sysemu(int sig=0) {
		return resume_execution(RESUME_SYSEMU, RESUME_WAIT, sig);
	}
	bool cont_sysemu_singlestep(int sig=0) {
		return resume_execution(RESUME_SYSEMU_SINGLESTEP,
					RESUME_WAIT, sig);
	}

	/**
	 * This must be in an emulated syscall, entered through
	 * |cont_sysemu()| or |cont_sysemu_singlestep()|, but that's
	 * not checked.  If so, step over the system call instruction
	 * to "exit" the emulated syscall.
	 *
	 * This operation is (assumed to be) idempotent:
	 * |finish_emulated_syscall()| can be called any number of
	 * times without changing state other than
	 * emulated-syscall-exited-ness.  Checkpointing relies on this
	 * assumption.
	 */
	void finish_emulated_syscall();

	/**
	 * Shortcut to the single |pending_event->desched.rec| when
	 * there's one desched event on the stack, and NULL otherwise.
	 * Exists just so that clients don't need to dig around in the
	 * event stack to find this record.
	 */
	const struct syscallbuf_record* desched_rec() const;

	/**
	 * Mark the members of this task's group as "unstable",
	 * meaning that even though a task may look runnable, it
	 * actually might not be.  (And so |waitpid(-1)| should be
	 * used to schedule the next task.)
	 *
	 * This is needed to handle the peculiarities of mass Task
	 * death at exit_group() and upon receiving core-dumping
	 * signals.  The reason it's needed is easier to understand if
	 * you keep in mind that the "main loop" of ptrace tracers is
	 * /supposed/ to look like
	 *
	 *   while (true) {
	 *     int tid = waitpid(-1, ...);
	 *     // do something with tid
	 *     ptrace(tid, PTRACE_SYSCALL, ...);
	 *   }
	 *
	 * That is, the tracer is supposed to let the kernel schedule
	 * threads and then respond to notifications generated by the
	 * kernel.
	 *
	 * Obviously this isn't how rr's recorder loop looks, because,
	 * among other things, rr has to serialize thread execution.
	 * Normally this isn't much of a problem.  However, mass task
	 * death is an exception.  What happens at a mass task death
	 * is a sequence of events like the following
	 *
	 *  1. A task calls exit_group() or is sent a core-dumping
	 *     signal.
	 *  2. rr receives a PTRACE_EVENT_EXIT notification for the
	 *     task.
	 *  3. rr detaches from the dying/dead task.
	 *  4. Successive calls to waitpid(-1) generate additional
	 *     PTRACE_EVENT_EXIT notifications for each also-dead task
	 *     in the original task's thread group.  Repeat (2) / (3)
	 *     for each notified task.
	 *
	 * So why destabilization?  After (2), rr can't block on the
	 * task shutting down (|waitpid(tid)|), because the kernel
	 * harvests the LWPs of the dying task group in an unknown
	 * order (which we shouldn't assume, even if we could guess
	 * it).  If rr blocks on the task harvest, it will (usually)
	 * deadlock.
	 *
	 * And because rr doesn't know the order of tasks that will be
	 * reaped, it doesn't know which of the dying tasks to
	 * "schedule".  If it guesses and blocks on another task in
	 * the group's status-change, it will (usually) deadlock.
	 *
	 * So destabilizing a task group, from rr's perspective, means
	 * handing scheduling control back to the kernel and not
	 * trying to harvest tasks before detaching from them.
	 *
	 * NB: an invariant of rr scheduling is that all process
	 * status changes happen as a result of rr resuming the
	 * execution of a task.  This is required to keep tracees in
	 * known states, preventing events from happening "behind rr's
	 * back".  However, destabilizing a task group means that
	 * these kinds of changes are possible, in theory.
	 *
	 * Currently, instability is a one-way street; it's only used
	 * needed for death signals and exit_group().
	 */
	void destabilize_task_group();

	/**
	 * Dump attributes of this process, including pending events,
	 * to |out|, which defaults to LOG_FILE.
	 */
	void dump(FILE* out = NULL) const;

	/**
	 * Called after the first exec in a session, when the session first
	 * enters a consistent state. Prior to that, the task state
	 * can vary based on how rr set up the child process. We have to flush
	 * out any state that might have been affected by that.
	 */
	void flush_inconsistent_state();

	/**
	 * Return total number of rbcs ever executed by this task.
	 * Updates rbc count from the current performance counter values if
	 * necessary.
	 */
	int64_t rbc_count();

	/**
	 * Set rbc count to 'count'.
	 */
	void set_rbc_count(int64_t count);

	/**
	 * Return the exe path passed to the most recent (successful)
	 * execve call.
	 */
	const std::string& exec_file() const { return execve_file; }

	/**
	 * Return true if this exited because of a SYS_exit/exit_group
	 * call.
	 */
	bool exited() const { return WIFEXITED(wait_status); }

	/** Return the event at the top of this's stack. */
	Event& ev() {
		return pending_events.back();
	}
	const Event& ev() const {
		return pending_events.back();
	}

	/**
	 * Sets the priority to 'value', updating the map-by-priority.
	 * Small priority values mean higher priority.
	 */
	void set_priority(int value);

	/**
	 * Stat |fd| in the context of this task's fd table, returning
	 * the result in |buf|.  The name of the referent file is
	 * returned in |buf|, of max size |buf_num_bytes|.  Return
	 * true on success, false on error.
	 */
	bool fdstat(int fd, struct stat* st, char* buf, size_t buf_num_bytes);

	/**
	 * Force the wait status of this to |status|, as if
	 * |wait()/try_wait()| had returned it.
	 */
	void force_status(int status) { wait_status = status; }

	/**
	 * Wait for |futex| in this address space to have the value
	 * |val|.
	 *
	 * WARNING: this implementation semi-busy-waits for the value
	 * change.  This must only be used in contexts where the futex
	 * will change "soon".
	 */
	void futex_wait(void* futex, uint32_t val);

	/**
	 * Return the message associated with the current ptrace
	 * event, f.e. the new child's pid at PTRACE_EVENT_CLONE.
	 */
	unsigned long get_ptrace_eventmsg();

	/**
	 * Return through |si| the siginfo at the signal-stop of this.
	 * Not meaningful unless this is actually at a signal stop.
	 */
	void get_siginfo(siginfo_t* si);

	/**
	 * Set the siginfo for the signal-stop of this.
	 */
	void set_siginfo(const siginfo_t& si);

	/**
	 * Return the trace we're either recording to (|ofstream()|)
	 * or replaying from (|ifstream()|).
	 */
	TraceIfstream& ifstream();
	TraceOfstream& ofstream();

	/**
	 * Initialize tracee buffers in this, i.e., implement
	 * RRCALL_init_syscall_buffer.  This task must be at the point
	 * of *exit from* the rrcall.  Registers will be updated with
	 * the return value from the rrcall, which is also returned
	 * from this call.  |map_hint| suggests where to map the
	 * region; see |init_syscallbuf_buffer()|.
	 *
	 * Pass SHARE_DESCHED_EVENT_FD to additionally share that fd.
	 */
	void* init_buffers(void* map_hint, int share_desched_fd);

	/**
	 * Destroy in the tracee task the buffer(s) specified by the
	 * DestroyBufferFlags mask |which|.  This task must already be
	 * at a state in which remote syscalls can be executed; if
	 * it's not, results are undefined.
	 */
	void destroy_buffers(int which);

	/** Return the current $ip of this. */
	void* ip() { return (void*)regs().ip(); }

	/**
	 * Return true if this is at an arm-desched-event syscall.
	 */
	bool is_arm_desched_event_syscall();

	/**
	 * Return true if this is at an arm-desched-event or
	 * disarm-desched-event syscall.
	 */
	bool is_desched_event_syscall();

	/**
	 * Return true if this is at a disarm-desched-event syscall.
	 */
	bool is_disarm_desched_event_syscall();

	/**
	 * return True when this is just before a syscall trap
	 * instruction for a traced syscall made by the syscallbuf
	 * code.  Callers may assume |is_in_syscallbuf()| is implied
	 * by this.
	 */
	bool is_entering_traced_syscall() {
		// |int $0x80| is |5d 80|, so |2| comes from
		// |sizeof(int $0x80)|.
		void* next_ip = (byte*)ip() + 2;
		return next_ip == traced_syscall_ip;
	}

	/**
	 * Return true if this is within the syscallbuf library.  This
	 * *does not* imply that $ip is at a buffered syscall; see
	 * below.
	 */
	bool is_in_syscallbuf() {
		return (syscallbuf_lib_start <= ip()
			&& ip() < syscallbuf_lib_end);
	}

	/**
	 * Return true when this at a traced syscall made by the
	 * syscallbuf code.  Callers may assume |is_in_syscallbuf()|
	 * is implied by this.
	 */
	bool is_traced_syscall() {
		return ip() == traced_syscall_ip;
	}

	/**
	 * Return true when this is at an untraced syscall, i.e. one
	 * initiated by a function in the syscallbuf.  Callers may
	 * assume |is_in_syscallbuf()| is implied by this.
	 */
	bool is_untraced_syscall() {
		return ip() == untraced_syscall_ip;
	}

	/**
	 * Return true if this task is most likely entering or exiting
	 * a syscall.
	 *
	 * No false negatives are known to be possible (i.e. if this
	 * task really is replaying a syscall, true will be returned),
	 * however false positives are possible.  Callers should
	 * therefore use this return value conservatively.
	 */
	bool is_probably_replaying_syscall();

	/**
	 * Return true if |ptrace_event()| is the trace event
	 * generated by the syscallbuf seccomp-bpf when a traced
	 * syscall is entered.
	 */
	bool is_ptrace_seccomp_event() const;

	/** Return true iff |sig| is blocked for this. */
	bool is_sig_blocked(int sig) const;

	/**
	 * Return true iff |sig| is SIG_IGN, or it's SIG_DFL and the
	 * default disposition is "ignore".
	 */
	bool is_sig_ignored(int sig) const;

	/**
	 * Return true if the current state of this looks like the
	 * interrupted syscall at the top of our event stack, if there
	 * is one.
	 */
	bool is_syscall_restart();

	/** Dump all pending events to the INFO log. */
	void log_pending_events() const;

	/**
	 * Return nonzero if |t| may not be immediately runnable,
	 * i.e., resuming execution and then |waitpid()|'ing may block
	 * for an unbounded amount of time.  When the task is in this
	 * state, the tracer must await a |waitpid()| notification
	 * that the task is no longer possibly-blocked before resuming
	 * its execution.
	 */
	bool may_be_blocked() const;

	/**
	 * If |syscallno| at |state| changes our VM mapping, then
	 * update the cache for that change.  The exception is mmap()
	 * calls: they're complicated enough to be handled separately.
	 * Client code should call |t->vm()->map(...)| directly.
	 */
	void maybe_update_vm(int syscallno, int state);

	/**
	 * Assuming ip() is just past a breakpoint instruction, adjust
	 * ip() backwards to point at that breakpoint insn.
	 */
	void move_ip_before_breakpoint();

	/**
	 * Return the "task name"; i.e. what |prctl(PR_GET_NAME)| or
	 * /proc/tid/comm would say that the task's name is.
	 */
	const std::string& name() const { return prname; }

	/**
	 * Return the signal that's pending for this as of the last
	 * call to |wait()/try_wait()|.  The signal 0 means "no
	 * signals'.
	 */
	int pending_sig() const {
		return pending_sig_from_status(wait_status);
	}

	/**
	 * Call this method when this task has entered an |execve()|
	 * call.
	 */
	void pre_exec();

	/**
	 * Call this after an |execve()| syscall finishes.  Emulate
	 * resource updates induced by the exec.
	 */
	void post_exec();

	/**
	 * Return the ptrace event as of the last call to
	 * |wait()/try_wait()|.
	 */
	int ptrace_event() const {
		return ptrace_event_from_status(wait_status);
	}

	/**
	 * Manage pending events.  |push_event()| pushes the given
	 * event onto the top of the event stack.  The |pop_*()|
	 * helpers pop the event at top of the stack, which must be of
	 * the specified type.
	 */
	void push_event(const Event& ev) {
		pending_events.push_back(ev);
	}
	void pop_event(EventType expected_type) {
		assert(pending_events.back().type() == expected_type);
		pending_events.pop_back();
	}
	void pop_noop() {
		pop_event(EV_NOOP);
	}
	void pop_desched() {
		pop_event(EV_DESCHED);
	}
	void pop_signal_delivery() {
		pop_event(EV_SIGNAL_DELIVERY);
	}
	void pop_signal_handler() {
		pop_event(EV_SIGNAL_HANDLER);
	}
	void pop_syscall() {
		pop_event(EV_SYSCALL);
	}
	void pop_syscall_interruption() {
		pop_event(EV_SYSCALL_INTERRUPTION);
	}

	/**
	 * Read |N| bytes from |child_addr| into |buf|, or don't
	 * return.
	 */
	template<size_t N>
	void read_bytes(void* child_addr, byte (&buf)[N]) {
		return read_bytes_helper(child_addr, N, buf);
	}

	/**
	 * Record an event on behalf of this.  Record the registers of
	 * this (and other relevant execution state) so that it can be
	 * used or verified during replay, if that state is available
	 * and meaningful at this's current execution point.
	 * |record_current_event()| record |this->ev()|, and
	 * |record_event()| records the specified event.
	 */
	void record_current_event();
	void record_event(const Event& ev);

	/**
	 * Save tracee data to the trace.  |addr| is the address in
	 * the address space of this task.  The |record_local*()|
	 * variants record data that's already been read from this,
	 * and the |record_remote*()| variants read the data and then
	 * record it.
	 */
	void record_local(void* addr, ssize_t num_bytes, const void* buf);
	void record_remote(void* addr, ssize_t num_bytes);
	void record_remote_str(void* str);

	/**
	 * Attempt to find the value of |regname| (a DebuggerRegister
	 * name) in this task, and if so (i) write it to |buf|; (ii)
	 * set |*defined = true|; (iii) return the size of written
	 * data.  If |*defined == false|, the value of |buf| is
	 * meaningless.
	 *
	 * This helper can fetch the values of both general-purpose
	 * and "extra" registers.
	 *
	 * NB: |buf| must be large enough to hold the largest register
	 * value that can be named by |regname|.
	 *
	 * TODO: nicer API.
	 */
	size_t get_reg(uint8_t* buf, int regname, bool* defined);

	/** Return the current regs of this. */
	const Registers& regs();

	/** Return the extra registers of this. */
	const ExtraRegisters& extra_regs();

	/** Return the current arch of this. This can change due to exec(). */
	supported_arch arch() { return regs().arch(); }

	/**
	 * Return the debug status, which is a bitfield comprising
	 * |DebugStatus| bits (see above).
	 */
	uintptr_t debug_status();

	/**
	 * Return the address of the watchpoint programmed at slot
	 * |i|.
	 */
	void* watchpoint_addr(size_t i);

	/** Return the current $sp of this. */
	void* sp() { return (void*)regs().esp; }

	/**
	 * Read |val| from |child_addr|.
	 */
	template<typename T>
	void read_mem(void* child_addr, T* val) {
		return read_bytes_helper(child_addr, sizeof(*val),
					 reinterpret_cast<byte*>(val));
	}

	/**
	 * Read and return the C string located at |child_addr| in
	 * this address space.
	 */
	std::string read_c_str(void* child_addr);

	/**
	 * Return the word at |child_addr| in this address space.
	 *
	 * NB: doesn't use the ptrace API, so safe to use even when
	 * the tracee isn't at a trace-stop.
	 */
	intptr_t read_word(void* child_addr);

	/**
	 * Copy |num_bytes| from |src| to |dst| in the address space
	 * of this.
	 */
	void remote_memcpy(void* dst, const void* src, size_t num_bytes);

	/**
	 * Resume execution |how|, deliverying |sig| if nonzero.
	 * After resuming, |wait_how|. In replay, reset hpcs and
	 * request an rbc period of rbc_period.
	 *
	 * You probably want to use one of the cont*() helpers above,
	 * and not this.
	 */
	bool resume_execution(ResumeRequest how, WaitRequest wait_how,
			      int sig = 0, int64_t rbc_period = 0);

	/** Return the session this is part of. */
	Session& session();
	RecordSession& record_session();
	ReplaySession& replay_session();
	ReplaySession* replay_session_ptr() {
		return session_replay;
	}

	const struct trace_frame& current_trace_frame();

	/** Restore the next chunk of saved data from the trace to this. */
	ssize_t set_data_from_trace();

	/**
	 * Set the syscall-return-value register of this to what was
	 * saved in the current trace frame.
	 */
	void set_return_value_from_trace();

	/** Set the tracee's registers to |regs|. */
	void set_regs(const Registers& regs);

	/** Set the tracee's extra registers to |regs|. */
	void set_extra_regs(const ExtraRegisters& regs);

	/**
	 * Program the debug registers to the vector of watchpoint
	 * configurations in |reg| (also updating the debug control
	 * register appropriately).  Return true if all registers were
	 * successfully programmed, false otherwise.  Any time false
	 * is returned, the caller is guaranteed that no watchpoint
	 * has been enabled; either all of |regs| is enabled and true
	 * is returned, or none are and false is returned.
	 */
	bool set_debug_regs(const DebugRegs& regs);

	/**
	 * Update the futex robust list head pointer to |list| (which
	 * is of size |len|).
	 */
	void set_robust_list(void* list, size_t len) {
		robust_futex_list = list;
		robust_futex_list_len = len;
	}
	void* robust_list() const { return robust_futex_list; }
	size_t robust_list_len() const { return robust_futex_list_len; }

	/** Update the thread area to |addr|. */
	void set_thread_area(void* tls);
	const struct user_desc* tls() const;

	/** Update the clear-tid futex to |tid_addr|. */
	void set_tid_addr(void* tid_addr);
	void* tid_addr() const { return tid_futex; }

	/**
	 * Call this after |sig| is delivered to this task.  Emulate
	 * sighandler updates induced by the signal delivery.
	 */
	void signal_delivered(int sig);

	/** Return true if this died because of a signal. */
	bool signaled() const {	return WIFSIGNALED(wait_status); }

	/** Return the disposition of |sig|. */
	sig_handler_t signal_disposition(int sig) const;

	/**
	 * Return true if the disposition of |sig| in |table| isn't
	 * SIG_IGN or SIG_DFL, that is, if a user sighandler will be
	 * invoked when |sig| is received.
	 */
	bool signal_has_user_handler(int sig) const;

	/** Return |sig|'s current sigaction. */
	const kernel_sigaction& signal_action(int sig) const;

	/**
	 * Return the |stack| argument passed to |clone()|, i.e. the
	 * top of this thread's stack if it's a thread.
	 *
	 * This IS NOT the stack pointer.  Call |sp()| if you want
	 * that.
	 */
	void* stack() const { return top_of_stack; }

	/**
	 * Stashed-signal API: if a signal becomes pending at an
	 * awkward time, but could be handled "soon", call
	 * |stash_sig()| to stash the current pending-signal state.
	 *
	 * |has_stashed_sig()| obviously returns true if |stash_sig()|
	 * has been called successfully.
	 *
	 * |pop_stash_sig()| restores the (relevant) state of this
	 * Task to what was saved in |stash_sig()|, and returns the
	 * saved siginfo.  After this call, |has_stashed_sig()| is
	 * false.
	 *
	 * NB: |get_siginfo()| will always return the "real" siginfo,
	 * regardless of stash popped-ness state.  Callers must ensure
	 * they do the right thing with the popped siginfo.
	 */
	void stash_sig();
	bool has_stashed_sig() const { return stashed_wait_status; }
	const siginfo_t& pop_stash_sig();

	/**
	 * Return the status of this as of the last successful
	 * wait()/try_wait() call.
	 */
	int status() const { return wait_status; }

	/**
	 * Return true if this is at a signal-stop.  If so,
	 * |stop_sig()| returns the signal that stopped us.
	 */
	bool stopped() const { return stopped_from_status(wait_status); }
	int stop_sig() const { return stop_sig_from_status(wait_status); }

	/** Return the task group this belongs to. */
	TaskGroup::shr_ptr task_group() { return tg; }

	/** Return the id of this task's recorded thread group. */
	pid_t tgid() const { return tg->tgid; }
	/** Return id of real OS task group. */
	pid_t real_tgid() const { return tg->real_tgid; }

	/** Return the dir of the trace we're using. */
	const std::string& trace_dir() const;

	/**
	 * Get the current "time" measured as ticks on recording trace
	 * events.  |task_time()| returns that "time" wrt this task
	 * only.
	 */
	uint32_t trace_time() const;
	uint32_t task_time() const { return thread_time; }

	/**
	 * Call this after the tracee successfully makes a
	 * |prctl(PR_SET_NAME)| call to change the task name to the
	 * string pointed at in the tracee's address space by
	 * |child_addr|.
	 */
	void update_prname(void* child_addr);

 	/**
	 * Call this when SYS_sigaction is finishing with |regs|.
	 */
	void update_sigaction(const Registers& regs);

	/**
	 * Call this when the tracee is about to complete a
	 * SYS_rt_sigprocmask syscall with |regs|.
	 */
	void update_sigmask(const Registers& regs);

	/**
	 * Call this before recording events or data.  Records
	 * syscallbuf data and flushes the buffer, if there's buffered
	 * data.
	 */
	void maybe_flush_syscallbuf();

	/**
	 * Return the virtual memory mapping (address space) of this
	 * task.
	 */
	AddressSpace::shr_ptr vm() { return as; }

	/**
	 * Block until the status of this changes.  Return true if
	 * successful, false if interrupted, and don't return at all
	 * on errors.
	 */
	bool wait();
	/**
	 * Return true if the status of this has changed, but don't
	 * block.
	 */
	bool try_wait();

	/**
	 * Write |N| bytes from |buf| to |child_addr|, or don't
	 * return.
	 */
	template<size_t N>
	void write_bytes(void* child_addr, const byte (&buf)[N]) {
		return write_bytes_helper(child_addr, N, buf);
	}

	/**
	 * Write |val| to |child_addr|.
	 *
	 * NB: doesn't use the ptrace API, so safe to use even when
	 * the tracee isn't at a trace-stop.
	 */
	template<typename T>
	void write_mem(void* child_addr, const T& val) {
		return write_bytes_helper(child_addr, sizeof(val),
					  reinterpret_cast<const byte*>(&val));
	}
	/**
	 * This is not the helper you're looking for.  See above: you
	 * probably accidentally wrote |write_mem(addr, &foo)| when
	 * you meant |write_mem(addr, foo)|.
	 */
	template<typename T>
	void write_mem(void* child_addr, const T* val) = delete;

	/**
	 * Don't use these helpers directly; use the safer and more
	 * convenient variants above.
	 *
	 * Read/write the number of bytes that the template wrapper
	 * inferred.
	 */
	ssize_t read_bytes_fallible(void* addr, ssize_t buf_size,
				    byte* buf);
	void read_bytes_helper(void* addr, ssize_t buf_size, byte* buf);
	void write_bytes_helper(void* addr,
				 ssize_t buf_size, const byte* buf);

	/** See |pending_sig()| above. */
	static int pending_sig_from_status(int status);
	/** See |ptrace_event()| above. */
	static int ptrace_event_from_status(int status) {
		return (0xFF0000 & status) >> 16;
	}
	/** See |stopped()| and |stop_sig()| above. */
	static bool stopped_from_status(int status) {
		return WIFSTOPPED(status);
	}
	static int stop_sig_from_status(int status) {
		assert(stopped_from_status(status));
		return WSTOPSIG(status);
	}

	/**
	 * Call this when performing a clone syscall in this task. Returns
	 * true if the call completed, false if it was interrupted and
	 * needs to be resumed.
	 */
	bool clone_syscall_is_complete();

	/**
	 * Open /proc/[tid]/mem fd for our AddressSpace, closing the old one
	 * first.
	 * This never fails. If necessary we force the tracee to open the file
	 * itself and smuggle the fd back to us.
	 */
	void open_mem_fd();

	/**
	 * Calls open_mem_fd if this task's AddressSpace doesn't already have one.
	 */
	void open_mem_fd_if_needed();

	/**
	 * Return the name of the given syscall.
	 */
	const char* syscallname(int syscallno) const;

	/* State only used during recording. */

	/* The running count of events that have been recorded for
	 * this task.  Starts at "1" to match with "global_time". */
	int thread_time;

	/* Whether switching away from this task is allowed in its
	 * current state.  Some operations must be completed
	 * atomically and aren't switchable. */
	int switchable;
	/* Nonzero when this is switchable for semantic purposes, but
	 * definitely isn't blocked on ony resource.  In that case,
	 * it's safe for the scheduler to do a blocking waitpid on
	 * this if our scheduling slot is open. */
	int pseudo_blocked;
	/* Number of times this context has been scheduled in a row,
	 * which approximately corresponds to the number of events
	 * it's processed in succession.  The scheduler maintains this
	 * state and uses it to make scheduling decisions. */
	int succ_event_counter;
	/* Nonzero when any assumptions made about the status of this
	 * process have been invalidated, and must be re-established
	 * with a waitpid() call. */
	int unstable;

	/* Task 'nice' value set by setpriority(2).
	   We use this to drive scheduling decisions. rr's scheduler is
	   deliberately simple and unfair; a task never runs as long as there's
	   another runnable task with a lower nice value. */
	int priority;

	/* Imagine that task A passes buffer |b| to the read()
	 * syscall.  Imagine that, after A is switched out for task B,
	 * task B then writes to |b|.  Then B is switched out for A.
	 * Since rr doesn't schedule the kernel code, the result is
	 * nondeterministic.  To avoid that class of replay
	 * divergence, we "redirect" (in)outparams passed to may-block
	 * syscalls, to "scratch memory".  The kernel writes to
	 * scratch deterministically, and when A (in the example
	 * above) exits its read() syscall, rr copies the scratch data
	 * back to the original buffers, serializing A and B in the
	 * example above.
	 *
	 * Syscalls can "nest" due to signal handlers.  If a syscall A
	 * is interrupted by a signal, and the sighandler calls B,
	 * then we can have scratch buffers set up for args of both A
	 * and B.  In linux, B won't actually re-enter A; A is exited
	 * with a "will-restart" error code and its args are saved for
	 * when (or if) it's restarted after the signal.  But that
	 * doesn't really matter wrt scratch space.  (TODO: in the
	 * future, we may be able to use that fact to simplify
	 * things.)
	 *
	 * Because of nesting, at first blush it seems we should push
	 * scratch allocations onto a stack and pop them as syscalls
	 * (or restarts thereof) complete.  But under a critical
	 * assumption, we can actually skip that.  The critical
	 * assumption is that the kernel writes its (in)outparams
	 * atomically wrt signal interruptions, and only writes them
	 * on successful exit.  Each syscall will complete in stack
	 * order, and it's invariant that the syscall processors must
	 * only write back to user buffers *only* the data that was
	 * written by the kernel.  So as long as the atomicity
	 * assumption holds, the completion of syscalls higher in the
	 * event stack may overwrite scratch space, but the completion
	 * of each syscall will overwrite those overwrites again, and
	 * that over-overwritten data is exactly and only what we'll
	 * write back to the tracee.
	 *
	 * |scratch_ptr| points at the mapped address in the child,
	 * and |size| is the total available space. */
	void* scratch_ptr;
	ssize_t scratch_size;

	/* Nonzero after the trace recorder has flushed the
	 * syscallbuf.  When this happens, the recorder must prepare a
	 * "reset" of the buffer, to zero the record count, at the
	 * next available slow (taking |desched| into
	 * consideration). */
	int flushed_syscallbuf;
	/* This bit is set when code wants to prevent the syscall
	 * record buffer from being reset when it normally would be.
	 * Currently, the desched'd syscall code uses this. */
	int delay_syscallbuf_reset;
	/* This bit is set when code wants the syscallbuf to be
	 * "synthetically empty": even if the record counter is
	 * nonzero, it should not be flushed.  Currently, the
	 * desched'd syscall code uses this along with
	 * |delay_syscallbuf_reset| above to keep the syscallbuf
	 * intact during possibly many "reentrant" events. */
	int delay_syscallbuf_flush;

	/* The child's desched counter event fd number, and our local
	 * dup. */
	int desched_fd, desched_fd_child;
	/* True when the tracee has started using the syscallbuf, and
	 * the tracer will start receiving PTRACE_SECCOMP events for
	 * traced syscalls.  We don't make any attempt to guess at the
	 * OS's process/thread semantics; this flag goes on the first
	 * time rr sees a PTRACE_SECCOMP event from the task.
	 *
	 * NB: there must always be at least one traced syscall before
	 * any untraced ones; that's the magic "rrcall" the tracee
	 * uses to initialize its syscallbuf. */
	int seccomp_bpf_enabled;

	/* State used only during replay. */

	int child_sig;
	// True when this has been forced to enter a syscall with
	// PTRACE_SYSCALL when instead we wanted to use
	// PTRACE_SINGLESTEP.  See replayer.cc.
	bool stepped_into_syscall;

	/* State used during both recording and replay. */

	struct hpc_context* hpc;

	/* This is always the "real" tid of the tracee. */
	pid_t tid;
	/* This is always the recorded tid of the tracee.  During
	 * recording, it's synonymous with |tid|, and during replay
	 * it's the tid that was recorded. */
	pid_t rec_tid;

	/* The instruction pointer from which traced syscalls made by
	 * the syscallbuf will originate. */
	void* traced_syscall_ip;
	/* The instruction pointer from which untraced syscalls will
	 * originate, used to determine whether a syscall is being
	 * made by the syscallbuf wrappers or not. */
	void* untraced_syscall_ip;
	/* Start and end of the mapping of the syscallbuf code
	 * section, used to determine whether a tracee's $ip is in the
	 * lib. */
	void* syscallbuf_lib_start;
	void* syscallbuf_lib_end;
	/* Points at rr's mapping of the (shared) syscall buffer. */
	struct syscallbuf_hdr* syscallbuf_hdr;
	size_t num_syscallbuf_bytes;
	/* Points at the tracee's mapping of the buffer. */
	void* syscallbuf_child;

	/* The value of arg1 passed to the last execve syscall in this task. */
	uintptr_t exec_saved_arg1;

private:
	Task(pid_t tid, pid_t rec_tid, int priority);

	/**
	 * Return a new Task cloned from |p|.  |flags| are a set of
	 * CloneFlags (see above) that determine which resources are
	 * shared or copied to the new child.  |new_tid| is the tid
	 * assigned to the new task by the kernel.  |new_rec_tid| is
	 * only relevant to replay, and is the pid that was assigned
	 * to the task during recording.
	 */
	Task* clone(int flags, void* stack, void* tls, void* cleartid_addr,
		    pid_t new_tid, pid_t new_rec_tid = -1,
		    Session* other_session = nullptr);

	/**
	 * Make this task look like an identical copy of |from| in
	 * every way relevant to replay.  This task should have been
	 * created by calling |from->os_clone()| or |from->os_fork()|,
	 * and if it wasn't results are undefined.
	 *
	 * Some task state must be copied into this by injecting and
	 * running syscalls in this task.  Other state is metadata
	 * that can simply be copied over in local memory.
	 */
	void copy_state(Task* from);

	/**
	 * Destroy tracer-side state of this (as opposed to remote,
	 * tracee-side state).
	 */
	void destroy_local_buffers();

	/**
	 * Detach this from rr and try hard to ensure any operations
	 * related to it have completed by the time this function
	 * returns.
	 *
	 * Warning: called by destructor.
	 */
	void detach_and_reap();

	/**
	 * Make the ptrace |request| with |addr| and |data|, return
	 * the ptrace return value.
	 */
	long fallible_ptrace(int request, void* addr, void* data);

	/**
	 * Read tracee memory using PTRACE_PEEKDATA calls. Slow, only use
	 * as fallback. Returns number of bytes actually read.
	 */
	ssize_t read_bytes_ptrace(void* addr, ssize_t buf_size, byte* buf);

	/**
	 * Write tracee memory using PTRACE_POKEDATA calls. Slow, only use
	 * as fallback. Returns number of bytes actually written.
	 */
	ssize_t write_bytes_ptrace(void* addr, ssize_t buf_size, const byte* buf);

	/**
	 * Map the syscallbuffer for this, shared with this process.
	 * |map_hint| is the address where the syscallbuf is expected
	 * to be mapped --- and this is asserted --- or nullptr if
	 * there are no expectations.
	 */
	void* init_syscall_buffer(AutoRemoteSyscalls& remote, void* map_hint);

	/**
	 * Share the desched-event fd that this task has already
	 * opened to this process when |share_desched_fd|.
	 */
	void init_desched_fd(AutoRemoteSyscalls& remote,
			     struct rrcall_init_buffers_params* args,
			     int share_desched_fd);

	/**
	 * True if this has blocked delivery of the desched signal.
	 */
	bool is_desched_sig_blocked();

	/**
	 * Destroy the OS task backing this by sending it SIGKILL and
	 * ensuring it was delivered.  After |kill()|, the only
	 * meaningful thing that can be done with this task is to
	 * delete it.
	 */
	void kill();

	/**
	 * Make the OS-level calls to create a new fork or clone that
	 * will eventually be a copy of this task and return that Task
	 * metadata.  These methods are used in concert with
	 * |Task::copy_state()| to create task copies during
	 * checkpointing.
	 *
	 * For |os_fork_into()|, |session| will be tracking the
	 * returned fork child.
	 *
	 * For |os_clone_into()|, |task_leader| is the "main thread"
	 * in the process into which the copy of this task will be
	 * created.  |task_leader| will perform the actual OS calls to
	 * create the new child.
	 */
	Task* os_fork_into(Session* session);
	Task* os_clone_into(Task* task_leader, AutoRemoteSyscalls& remote);

	/**
	 * Return the trace fstream that we're using, whether in
	 * recording or replay.
	 */
	TraceFstream& trace_fstream();
	const TraceFstream& trace_fstream() const;

	/**
	 * Like |fallible_ptrace()| but infallible: except either the
	 * request succeeds, or this doesn't return.
	 */
	void xptrace(int request, void* addr, void* data);

	/**
	 * The rbc interrupt has failed to stop the Task currently
	 * being |wait()|ed, so the alarm() we programmed has fired.
	 * PTRACE_INTERRUPT the runaway tracee.
	 */
	static void handle_runaway(int sig);

	/**
	 * Make the OS-level calls to clone |parent| into |session|
	 * and return the resulting Task metadata for that new
	 * process.  This is as opposed to |Task::clone()|, which only
	 * attaches Task metadata to an /existing/ process.
	 *
	 * The new clone will be tracked in |session|.  The other
	 * arguments are as for |Task::clone()| above.
	 */
	static Task* os_clone(Task* parent, Session* session,
			      AutoRemoteSyscalls& remote,
			      pid_t rec_child_tid,
			      unsigned base_flags,
			      void* stack = nullptr,
			      void* ptid = nullptr,
			      void* tls = nullptr,
			      void* ctid = nullptr);

	/** Fork and exec a task to run |ae|, with |rec_tid|. */
	static Task* spawn(const struct args_env& ae, Session& session,
			   pid_t rec_tid = -1);

	// The address space of this task.
	std::shared_ptr<AddressSpace> as;
	// The set of signals that are currently blocked.
	sig_set_t blocked_sigs;
	// The exe-file argument passed to the most recent execve call
	// made by this task.
	std::string execve_file;
	// The current stack of events being processed.  (We use a
	// deque instead of a stack because we need to iterate the
	// events.)
	std::deque<Event> pending_events;
	// Task's OS name.
	std::string prname;
	// Count of all rbcs seen by this task since tracees became
	// consistent.
	int64_t rbcs;


	int64_t irc;


	// When |registers_known|, these are our child registers.
	// When execution is resumed, we no longer know what the child
	// registers are so the flag is unset.  The next time the
	// registers are read after a trace-stop, we actually make the
	// ptrace call to update the cache, and set the "known" bit
	// back to true.  Manually setting the registers also updates
	// this cached value and set the "known" flag.
	Registers registers;
	bool registers_known;
	// When |extra_registers_known|, we have saved our extra registers.
	ExtraRegisters extra_registers;
	bool extra_registers_known;
	// Futex list passed to |set_robust_list()|.  We could keep a
	// strong type for this list head and read it if we wanted to,
	// but for now we only need to remember its address / size at
	// the time of the most recent set_robust_list() call.
	void* robust_futex_list;
	size_t robust_futex_list_len;
	// The record or replay session we're part of.
	RecordSession* session_record;
	ReplaySession* session_replay;
	// Points to the signal-hander table of this task.  If this
	// task is a non-fork clone child, then the table will be
	// shared with all its "thread" siblings.  Any updates made to
	// that shared table are immediately visible to all sibling
	// threads.
	//
	// fork children always get their own copies of the table.
	// And if this task exec()s, the table is copied and stripped
	// of user sighandlers (see below). */
	std::shared_ptr<Sighandlers> sighandlers;
	// Stashed signal-delivery state, ready to be delivered at
	// next opportunity.  |stashed_si| is only meaningful when
	// |stashed_wait_status| is nonzero.
	siginfo_t stashed_si;
	int stashed_wait_status;
	// The task group this belongs to.
	std::shared_ptr<TaskGroup> tg;
	// Contents of the |tls| argument passed to |clone()| and
	// |set_thread_area()|, when |thread_area_valid| is true.
	struct user_desc thread_area;
	bool thread_area_valid;
	// The memory cell the kernel will clear and notify on exit,
	// if our clone parent requested it.
	void* tid_futex;
	// The |stack| argument passed to |clone()|, which for
	// "threads" is the top of the user-allocated stack.
	void* top_of_stack;
	// The most recent status of this task as returned by
	// waitpid().
	int wait_status;

	Task(Task&) = delete;
	Task operator=(Task&) = delete;
};

#endif /* RR_TASK_H_ */
