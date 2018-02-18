#include <signal.h>
#include <thread.h>
#include <current.h>

__sigfunc default_sighandlers[NSIG+1] = {
	NULL,        // first signal number starts at 1
	_sigfn_term, // SIGHUP
	_sigfn_term, // SIGINT
	_sigfn_core, // SIGQUIT
	_sigfn_core, // SIGILL
	_sigfn_core, // SIGTRAP
	_sigfn_core, // SIGABRT
	_sigfn_term, // SIGEMT
	_sigfn_core, // SIGFPE
	_sigfn_term, // SIGKILL
	_sigfn_core, // SIGBUS,
	_sigfn_core, // SIGSEGV
	_sigfn_core, // SIGSYS
	_sigfn_term, // SIGPIPE
	_sigfn_term, // SIGALRM
	_sigfn_term, // SIGTERM
	_sigfn_ign,  // SIGURG
	_sigfn_stop, // SIGSTOP
	_sigfn_stop, // SIGTSTP
	_sigfn_cont, // SIGCONT
	_sigfn_ign,  // SIGCHLD
	_sigfn_stop, // SIGTTIN
	_sigfn_stop, // SIGTTOU
	_sigfn_ign, // SIGIO
	_sigfn_core, // SIGXCPU
	_sigfn_core, // SIGXFSZ
	_sigfn_term, // SIGVTALRM
	_sigfn_term, // SIGPROF
	_sigfn_ign, // SIGWINCH
	_sigfn_term, // SIGINFO
	_sigfn_term, // SIGUSR1
	_sigfn_term, // SIGUSR2
	_sigfn_ign, // SIGPWR
};

void _sigfn_term(int signo) {
  (void)signo;
  DEBUG(DB_SIG, "Exiting curthread (%d) due to signal [%s]\n", (int)curthread->t_pid, sys_signame[signo]);
  thread_exit(1);
}
void _sigfn_core(int signo) {
  _sigfn_term(signo);
}
void _sigfn_ign(int signo) {
  (void)signo;
  // do nothing
}
void _sigfn_stop(int signo) {
  DEBUG(DB_SIG, "Stopping curthread (%d) due to signal [%s]\n", (int)curthread->t_pid, sys_signame[signo]);
  thread_stop();
}
void _sigfn_cont(int signo) {
  (void)signo;
  DEBUG(DB_SIG, "Continuing curthread (%d) due to signal [%s]\n", (int)curthread->t_pid, sys_signame[signo]);
  curthread->t_is_stopped = false;
}
