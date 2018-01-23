#ifndef _CONSOLE_LOCK_H_
#define _CONSOLE_LOCK_H_
inline void console_lock_bootstrap(void);
#if 1

#include <synch.h>
struct lock *console_lock;

inline void console_lock_bootstrap(void) {
  console_lock = lock_create("console lock");
}
#define DEBUG_CONSOLE_LOCK(fd)\
  do {\
    if (((fd) == 1 || (fd) == 2) && console_lock != NULL) {\
      lock_acquire(console_lock);\
    } else { (void)0; }\
  } while(0)
#define DEBUG_CONSOLE_UNLOCK() (console_lock && lock_do_i_hold(console_lock)) ? lock_release(console_lock) : (void)0
#else
inline void console_lock_bootstrap(void) {}
#define DEBUG_CONSOLE_LOCK(fd) (void)0
#define DEBUG_CONSOLE_UNLOCK() (void)0
#endif
#endif
