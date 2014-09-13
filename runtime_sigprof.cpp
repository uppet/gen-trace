#define __STDC_FORMAT_MACROS
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <signal.h>
#include <sys/time.h>
#include <sys/syscall.h>

#include <new>

#ifndef CTRACE_FILE_NAME
#define CTRACE_FILE_NAME "/sdcard/trace.json"
#endif // CTRACE_FILE_NAME
#define CRASH()                                                               \
  do                                                                          \
    {                                                                         \
      (*(int *)0xeadbaddc = 0);                                               \
    }                                                                         \
  while (0)

#ifdef CTRACE_ENABLE_STAT
int stat_find_miss = 0;
#endif // CTRACE_ENABLE_STAT
namespace
{
pthread_key_t thread_info_key;
FILE *file_to_write;
static const uint64_t invalid_time = static_cast<uint64_t> (-1);
static const int frequency = 1000;
static const int ticks = 1;
static const int max_idle_times = 1000;
static const int update_clock_times = 100;

int pipes[2];
#ifdef __ARM_EABI__

struct sigcontext
{
  unsigned long trap_no;
  unsigned long error_code;
  unsigned long oldmask;
  unsigned long arm_r0;
  unsigned long arm_r1;
  unsigned long arm_r2;
  unsigned long arm_r3;
  unsigned long arm_r4;
  unsigned long arm_r5;
  unsigned long arm_r6;
  unsigned long arm_r7;
  unsigned long arm_r8;
  unsigned long arm_r9;
  unsigned long arm_r10;
  unsigned long arm_fp;
  unsigned long arm_ip;
  unsigned long arm_sp;
  unsigned long arm_lr;
  unsigned long arm_pc;
  unsigned long arm_cpsr;
  unsigned long fault_address;
};

#endif

struct ucontext
{
  unsigned long uc_flags;
  struct ucontext *uc_link;
  stack_t uc_stack;
  struct sigcontext uc_mcontext;
  sigset_t uc_sigmask; /* mask last for extensibility */
};

struct CTraceStruct
{
  uint64_t start_time_;
  uint64_t min_end_time_;
  const char *name_;
  CTraceStruct (const char *);
};

struct ThreadInfo
{
  static const int MAX_STACK = frequency;
  int pid_;
  int tid_;
  CTraceStruct *stack_[MAX_STACK];
  int stack_end_;
  uint64_t current_time_;
  int idle_times_;
  int clock_update_count_;
  bool blocked_;
  ThreadInfo ();
  void Clear ();
  void SetBlocked ();
  static ThreadInfo *New ();
  static ThreadInfo *Find ();
  static ThreadInfo *Find (const int);
};
static const int MAX_THREADS = 100;
char info_store_char[MAX_THREADS * sizeof (ThreadInfo)];

void
ThreadInfo::Clear ()
{
  tid_ = 0;
}

void
ThreadInfo::SetBlocked ()
{
  blocked_ = true;
  clock_update_count_ = 0;
}

ThreadInfo *
ThreadInfo::Find (const int tid)
{
  ThreadInfo *info_store = reinterpret_cast<ThreadInfo *> (info_store_char);
  int hash_index = tid % MAX_THREADS;
  for (int i = 0; i < MAX_THREADS; ++i)
    {
      if (info_store[hash_index].tid_ == tid)
        {
          return &info_store[hash_index];
        }
      hash_index++;
#ifdef CTRACE_ENABLE_STAT
      __sync_add_and_fetch (&stat_find_miss, 1);
#endif // CTRACE_ENABLE_STAT
      if (hash_index >= MAX_THREADS)
        hash_index = 0;
    }
  return NULL;
}

ThreadInfo *
ThreadInfo::Find ()
{
  const int tid = syscall (__NR_gettid, 0);
  return ThreadInfo::Find (tid);
}

ThreadInfo *
ThreadInfo::New ()
{
  ThreadInfo *free_thread_info = NULL;
  ThreadInfo *info_store = reinterpret_cast<ThreadInfo *> (info_store_char);
  int hash_index = syscall (__NR_gettid, 0) % MAX_THREADS;
  for (int i = 0; i < MAX_THREADS; ++i)
    {
      if (info_store[hash_index].tid_ == 0)
        {
          if (!__sync_bool_compare_and_swap (&info_store[hash_index].pid_, 0,
                                             -1))
            {
              goto __continue;
            }
          free_thread_info = &info_store[hash_index];
          break;
        }
    __continue:
      hash_index++;
      if (hash_index >= MAX_THREADS)
        hash_index = 0;
    }
  if (free_thread_info == NULL)
    CRASH ();
  pthread_setspecific (thread_info_key, free_thread_info);
  return new (free_thread_info) ThreadInfo ();
}

ThreadInfo::ThreadInfo ()
{
  pid_ = getpid ();
  tid_ = syscall (__NR_gettid, 0);
  stack_end_ = 0;
  idle_times_ = 0;
  blocked_ = true;
  clock_update_count_ = 0;
}

CTraceStruct::CTraceStruct (const char *name)
{
  start_time_ = invalid_time;
  min_end_time_ = invalid_time;
  name_ = name;
}

ThreadInfo *
_get_thread_info ()
{
  ThreadInfo *tinfo = ThreadInfo::Find ();
  if (tinfo)
    return tinfo;
  tinfo = ThreadInfo::New ();
  return tinfo;
}

uint64_t
GetTimesFromClock ()
{
  static const int64_t kMillisecondsPerSecond = 1000;
  static const int64_t kMicrosecondsPerMillisecond = 1000;
  static const int64_t kMicrosecondsPerSecond = kMicrosecondsPerMillisecond
                                                * kMillisecondsPerSecond;
  static const int64_t kNanosecondsPerMicrosecond = 1000;

  struct timespec ts_thread;
  clock_gettime (CLOCK_MONOTONIC, &ts_thread);
  return (static_cast<uint64_t> (ts_thread.tv_sec) * kMicrosecondsPerSecond)
         + (static_cast<uint64_t> (ts_thread.tv_nsec)
            / kNanosecondsPerMicrosecond);
}

ThreadInfo *
GetThreadInfo ()
{
  ThreadInfo *tinfo = _get_thread_info ();
  if (tinfo->blocked_)
    {
      tinfo->current_time_ = GetTimesFromClock ();
      sigset_t unblock_set;
      sigemptyset (&unblock_set);
      sigaddset (&unblock_set, SIGPROF);
      sigprocmask (SIG_UNBLOCK, &unblock_set, 0);
      tinfo->blocked_ = false;
    }
  return tinfo;
}

void
delete_thread_info (void *tinfo)
{
  static_cast<ThreadInfo *> (tinfo)->Clear ();
}

void
myhandler (int, siginfo_t *, void *context)
{
  int tid = syscall (__NR_gettid, 0);
  // we don't use GetThreadInfo , because
  // it make no sense to deal
  // with the thread without this structure
  // created in __start_ctrace__.
  ThreadInfo *tinfo = ThreadInfo::Find (tid);
  if (!tinfo)
    {
      // block this signal if it does not belong to
      // the profiling threads.
      sigaddset (&static_cast<ucontext *> (context)->uc_sigmask, SIGPROF);
      return;
    }
  uint64_t old_time = tinfo->current_time_;
  uint64_t &current_time_thread = tinfo->current_time_;
  current_time_thread += ticks * frequency;

  if (tinfo->stack_end_ >= ThreadInfo::MAX_STACK)
    {
      CRASH ();
    }
  for (int i = 0; i < tinfo->stack_end_; ++i, old_time += ticks)
    {
      CTraceStruct *cur = tinfo->stack_[i];
      if (cur->start_time_ != invalid_time)
        continue;
      cur->start_time_ = old_time;
    }
  if (tinfo->stack_end_ != 0)
    {
      tinfo->stack_[tinfo->stack_end_ - 1]->min_end_time_ = current_time_thread
                                                            + ticks;
    }
  else
    {
      tinfo->idle_times_++;
      if (tinfo->idle_times_ >= max_idle_times)
        {
          // will block SIGPROF
          sigaddset (&static_cast<ucontext *> (context)->uc_sigmask, SIGPROF);
          tinfo->SetBlocked ();
        }
    }
  // try update clock
  if (tinfo->clock_update_count_++ > update_clock_times)
    {
      tinfo->clock_update_count_ = 0;
      current_time_thread = GetTimesFromClock ();
    }
}

void *writer_thread (void *);

struct Initializer
{
  Initializer ()
  {
    pthread_key_create (&thread_info_key, delete_thread_info);
    struct sigaction myaction = { 0 };
    struct itimerval timer;
    myaction.sa_sigaction = myhandler;
    myaction.sa_flags = SA_SIGINFO;
    sigaction (SIGPROF, &myaction, NULL);

    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = frequency;
    timer.it_interval = timer.it_value;
    setitimer (ITIMER_PROF, &timer, NULL);
    file_to_write = fopen (CTRACE_FILE_NAME, "w");
    fprintf (file_to_write, "{\"traceEvents\": [");
    pipe (pipes);
    pthread_t my_writer_thread;
    pthread_create (&my_writer_thread, NULL, writer_thread, NULL);
  }

  ~Initializer ()
  {
    fclose (file_to_write);
    close (pipes[0]);
    close (pipes[1]);
  }
};

Initializer __init__;

struct Record
{
  int pid_;
  int tid_;
  uint64_t start_time_;
  uint64_t dur_;
  const char *name_;
};

void
record_this (CTraceStruct *c, ThreadInfo *tinfo)
{
  Record *r = static_cast<Record *> (malloc (sizeof (Record)));
  if (!r)
    CRASH ();
  r->pid_ = tinfo->pid_;
  r->tid_ = tinfo->tid_;
  r->start_time_ = c->start_time_;
  r->name_ = c->name_;
  r->dur_ = c->min_end_time_ - c->start_time_;
  while (true)
    {
      int written_bytes = write (pipes[1], &r, sizeof (Record *));
      if (written_bytes != sizeof (Record *))
        {
          if (errno == EINTR)
            continue;
          CRASH ();
        }
      break;
    }
}

void *
writer_thread (void *)
{
  pthread_setname_np (pthread_self (), "writer_thread");
  int fd = pipes[0];

  while (true)
    {
      Record *current;

      int read_bytes = read (fd, &current, sizeof (Record *));
      if (read_bytes != sizeof (Record *))
        {
          if (errno == EINTR)
            continue;
          CRASH ();
        }

      static bool needComma = false;
      if (!needComma)
        {
          needComma = true;
        }
      else
        {
          fprintf (file_to_write, ", ");
        }

      fprintf (file_to_write,
               "{\"cat\":\"%s\", \"pid\":%d, \"tid\":%d, \"ts\":%" PRIu64 ", "
               "\"ph\":\"X\", \"name\":\"%s\", \"dur\": %" PRIu64 "}",
               "profile", current->pid_, current->tid_, current->start_time_,
               current->name_, current->dur_);
      static int flushCount = 0;
      if (flushCount++ == 5)
        {
          fflush (file_to_write);
          flushCount = 0;
        }
      free (current);
    }
}
}

extern "C" {
extern void __start_ctrace__ (void *c, const char *name);
extern void __end_ctrace__ (CTraceStruct *c, const char *name);
}

void
__start_ctrace__ (void *c, const char *name)
{
  if (file_to_write == 0)
    return;
  CTraceStruct *cs = new (c) CTraceStruct (name);
  ThreadInfo *tinfo = GetThreadInfo ();
  if (tinfo->stack_end_ < ThreadInfo::MAX_STACK)
    {
      tinfo->stack_[tinfo->stack_end_] = cs;
    }
  tinfo->stack_end_++;
}

void
__end_ctrace__ (CTraceStruct *c, const char *name)
{
  if (file_to_write == 0)
    return;
  ThreadInfo *tinfo = GetThreadInfo ();
  tinfo->stack_end_--;
  if (tinfo->stack_end_ < ThreadInfo::MAX_STACK)
    {
      if (c->start_time_ != invalid_time)
        {
          // we should record this
          record_this (c, tinfo);
          if (tinfo->stack_end_ != 0)
            {
              // propagate the back's mini end time
              tinfo->stack_[tinfo->stack_end_ - 1]->min_end_time_
                  = c->min_end_time_ + ticks;
              tinfo->current_time_ += ticks;
            }
        }
    }
}
