// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *               2016 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <pthread.h>
#include "gtest/gtest.h"
#ifndef GTEST_IS_THREADSAFE
#error "!GTEST_IS_THREADSAFE"
#endif

#include "include/cephfs/libcephfs.h"
#include <errno.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/xattr.h>

#include <stdlib.h>
#include <semaphore.h>
#include <time.h>
#include <sys/mman.h>

#ifdef __linux__
#include <limits.h>
#endif

// Startup common: create and mount ceph fs
#define STARTUP_CEPH() do {				\
    ASSERT_EQ(0, ceph_create(&cmount, NULL));		\
    ASSERT_EQ(0, ceph_conf_read_file(cmount, NULL));	\
    ASSERT_EQ(0, ceph_conf_parse_env(cmount, NULL));	\
    ASSERT_EQ(0, ceph_mount(cmount, NULL));		\
  } while(0)

// Cleanup common: unmount and release ceph fs
#define CLEANUP_CEPH() do {			\
    ASSERT_EQ(0, ceph_unmount(cmount));		\
    ASSERT_EQ(0, ceph_release(cmount));		\
  } while(0)

static const mode_t fileMode = S_IRWXU | S_IRWXG | S_IRWXO;

// Default wait time for normal and "slow" operations
// (5" should be enough in case of network congestion)
static const long waitMs = 10;
static const long waitSlowMs = 5000;

#if 0
// Get the absolute struct timespec reference from now + 'ms' milliseconds
static const struct timespec* abstime(struct timespec &ts, long ms) {
  if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
    abort();
  }
  ts.tv_nsec += ms * 1000000;
  ts.tv_sec += ts.tv_nsec / 1000000000;
  ts.tv_nsec %= 1000000000;
  return &ts;
}
#endif

/* Basic locking */

TEST(LibCephFS, BasicRecordLocking) {
  struct ceph_mount_info *cmount = NULL;
  STARTUP_CEPH();

  char c_file[1024];
  sprintf(c_file, "recordlock_test_%d", getpid());
  Fh *fh = NULL;
  Inode *root = NULL, *inode = NULL;
  struct stat attr;
  int rc;
  struct flock lock1, lock2;

  // Get the root inode
  rc = ceph_ll_lookup_root(cmount, &root);
  ASSERT_EQ(rc, 0); 

  // Get the inode and Fh corresponding to c_file
  rc = ceph_ll_create(cmount, root, c_file, fileMode, O_RDWR | O_CREAT, &attr,
  		      &inode, &fh, 0, 0);
  ASSERT_EQ(rc, 0); 

  // write lock twice
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, 42, false));

  lock2.l_type = F_WRLCK;
  lock2.l_whence = SEEK_SET;
  lock2.l_start = 0;
  lock2.l_len = 1024;
  lock2.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock2, 43, false));

  // Now try a conflicting read lock
  lock2.l_type = F_RDLCK;
  lock2.l_whence = SEEK_SET;
  lock2.l_start = 100;
  lock2.l_len = 100;
  lock2.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock2, 43, false));

  // Now do a getlk
  ASSERT_EQ(0, ceph_ll_getlk(cmount, fh, &lock2, 43));
  ASSERT_EQ(lock2.l_type, F_WRLCK);
  ASSERT_EQ(lock2.l_start, 0);
  ASSERT_EQ(lock2.l_len, 1024);
  ASSERT_EQ(lock2.l_pid, getpid());

  // Extend the range of the write lock
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 1024;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, 42, false));

  // Now do a getlk
  lock2.l_type = F_RDLCK;
  lock2.l_whence = SEEK_SET;
  lock2.l_start = 100;
  lock2.l_len = 100;
  lock2.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_getlk(cmount, fh, &lock2, 43));
  ASSERT_EQ(lock2.l_type, F_WRLCK);
  ASSERT_EQ(lock2.l_start, 0);
  ASSERT_EQ(lock2.l_len, 2048);
  ASSERT_EQ(lock2.l_pid, getpid());

  // Now release part of the range
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 512;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, 42, false));

  // Now do a getlk to check 1st part
  lock2.l_type = F_RDLCK;
  lock2.l_whence = SEEK_SET;
  lock2.l_start = 100;
  lock2.l_len = 100;
  lock2.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_getlk(cmount, fh, &lock2, 43));
  ASSERT_EQ(lock2.l_type, F_WRLCK);
  ASSERT_EQ(lock2.l_start, 0);
  ASSERT_EQ(lock2.l_len, 512);
  ASSERT_EQ(lock2.l_pid, getpid());

  // Now do a getlk to check 2nd part
  lock2.l_type = F_RDLCK;
  lock2.l_whence = SEEK_SET;
  lock2.l_start = 2000;
  lock2.l_len = 100;
  lock2.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_getlk(cmount, fh, &lock2, 43));
  ASSERT_EQ(lock2.l_type, F_WRLCK);
  ASSERT_EQ(lock2.l_start, 1536);
  ASSERT_EQ(lock2.l_len, 512);
  ASSERT_EQ(lock2.l_pid, getpid());

  // Now do a getlk to check released part
  lock2.l_type = F_RDLCK;
  lock2.l_whence = SEEK_SET;
  lock2.l_start = 512;
  lock2.l_len = 1024;
  lock2.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_getlk(cmount, fh, &lock2, 43));
  ASSERT_EQ(lock2.l_type, F_UNLCK);
  ASSERT_EQ(lock2.l_start, 512);
  ASSERT_EQ(lock2.l_len, 1024);
  ASSERT_EQ(lock2.l_pid, getpid());

  // Now downgrade the 1st part of the lock
  lock1.l_type = F_RDLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 512;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, 42, false));

  // Now do a getlk to check 1st part
  lock2.l_type = F_WRLCK;
  lock2.l_whence = SEEK_SET;
  lock2.l_start = 100;
  lock2.l_len = 100;
  lock2.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_getlk(cmount, fh, &lock2, 43));
  ASSERT_EQ(lock2.l_type, F_RDLCK);
  ASSERT_EQ(lock2.l_start, 0);
  ASSERT_EQ(lock2.l_len, 512);
  ASSERT_EQ(lock2.l_pid, getpid());

  // Now upgrade the 1st part of the lock
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 512;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, 42, false));

  // Now do a getlk to check 1st part
  lock2.l_type = F_WRLCK;
  lock2.l_whence = SEEK_SET;
  lock2.l_start = 100;
  lock2.l_len = 100;
  lock2.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_getlk(cmount, fh, &lock2, 43));
  ASSERT_EQ(lock2.l_type, F_WRLCK);
  ASSERT_EQ(lock2.l_start, 0);
  ASSERT_EQ(lock2.l_len, 512);
  ASSERT_EQ(lock2.l_pid, getpid());

  ASSERT_EQ(0, ceph_ll_close(cmount, fh));
  ASSERT_EQ(0, ceph_unlink(cmount, c_file));
  CLEANUP_CEPH();
}

#if 0
/* Locking in different threads */

// Used by ConcurrentLocking test
struct str_ConcurrentLocking {
  const char *file;
  struct ceph_mount_info *cmount;  // !NULL if shared
  sem_t sem[2];
  sem_t semReply[2];
  void sem_init(int pshared) {
    ASSERT_EQ(0, ::sem_init(&sem[0], pshared, 0));
    ASSERT_EQ(0, ::sem_init(&sem[1], pshared, 0));
    ASSERT_EQ(0, ::sem_init(&semReply[0], pshared, 0));
    ASSERT_EQ(0, ::sem_init(&semReply[1], pshared, 0));
  }
  void sem_destroy() {
    ASSERT_EQ(0, ::sem_destroy(&sem[0]));
    ASSERT_EQ(0, ::sem_destroy(&sem[1]));
    ASSERT_EQ(0, ::sem_destroy(&semReply[0]));
    ASSERT_EQ(0, ::sem_destroy(&semReply[1]));
  }
};

// Wakeup main (for (N) steps)
#define PING_MAIN(n) ASSERT_EQ(0, sem_post(&s.sem[n%2]))
// Wait for main to wake us up (for (RN) steps)
#define WAIT_MAIN(n) \
  ASSERT_EQ(0, sem_timedwait(&s.semReply[n%2], abstime(ts, waitSlowMs)))

// Wakeup worker (for (RN) steps)
#define PING_WORKER(n) ASSERT_EQ(0, sem_post(&s.semReply[n%2]))
// Wait for worker to wake us up (for (N) steps)
#define WAIT_WORKER(n) \
  ASSERT_EQ(0, sem_timedwait(&s.sem[n%2], abstime(ts, waitSlowMs)))
// Worker shall not wake us up (for (N) steps)
#define NOT_WAIT_WORKER(n) \
  ASSERT_EQ(-1, sem_timedwait(&s.sem[n%2], abstime(ts, waitMs)))

// Do twice an operation
#define TWICE(EXPR) do {			\
    EXPR;					\
    EXPR;					\
  } while(0)

/* Locking in different threads */

// Used by ConcurrentLocking test
static void thread_ConcurrentLocking(str_ConcurrentLocking& s) {
  struct ceph_mount_info *const cmount = s.cmount;
  Fh *fh = NULL;
  Inode *root = NULL, *inode = NULL;
  struct stat attr;
  struct flock lock1;
  int rc;

  // Get the root inode
  rc = ceph_ll_lookup_root(cmount, &root);
  ASSERT_EQ(rc, 0); 

  // Get the inode and Fh corresponding to c_file
  rc = ceph_ll_create(cmount, root, c_file, fileMode, O_RDWR | O_CREAT, &attr,
  		      &inode, &fh, 0, 0);
  ASSERT_EQ(rc, 0); 

  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));

  PING_MAIN(1); // (1)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), true));
  PING_MAIN(2); // (2)

  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
  PING_MAIN(3); // (3)

  lock1.l_type = F_RDLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), true));
  PING_MAIN(4); // (4)

  WAIT_MAIN(1); // (R1)
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
  PING_MAIN(5); // (5)

  WAIT_MAIN(2); // (R2)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), true));
  PING_MAIN(6); // (6)

  WAIT_MAIN(3); // (R3)
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
  PING_MAIN(7); // (7)
}

// Used by ConcurrentLocking test
static void* thread_ConcurrentLocking_(void *arg) {
  str_ConcurrentLocking *const s =
    reinterpret_cast<str_ConcurrentLocking*>(arg);
  thread_ConcurrentLocking(*s);
  return NULL;
}

TEST(LibCephFS, ConcurrentLocking) {
  const pid_t mypid = getpid();
  struct ceph_mount_info *cmount;
  STARTUP_CEPH();

  char c_file[1024];
  sprintf(c_file, "recordlock_test_%d", mypid);
  Fh *fh = NULL;
  Inode *root = NULL, *inode = NULL;
  struct stat attr;

  // Lock
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), true));

  // Start locker thread
  pthread_t thread;
  struct timespec ts;
  str_ConcurrentLocking s = { c_file, cmount };
  s.sem_init(0);
  ASSERT_EQ(0, pthread_create(&thread, NULL, thread_ConcurrentLocking_, &s));
  // Synchronization point with thread (failure: thread is dead)
  WAIT_WORKER(1); // (1)

  // Shall not have lock immediately
  NOT_WAIT_WORKER(2); // (2)

  // Unlock
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));

  // Shall have lock
  // Synchronization point with thread (failure: thread is dead)
  WAIT_WORKER(2); // (2)

  // Synchronization point with thread (failure: thread is dead)
  WAIT_WORKER(3); // (3)

  // Wait for thread to share lock
  WAIT_WORKER(4); // (4)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
  lock1.l_type = F_RDLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));

  // Wake up thread to unlock shared lock
  PING_WORKER(1); // (R1)
  WAIT_WORKER(5); // (5)

  // Now we can lock exclusively
  // Upgrade to exclusive lock (as per POSIX)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), true));

  // Wake up thread to lock shared lock
  PING_WORKER(2); // (R2)

  // Shall not have lock immediately
  NOT_WAIT_WORKER(6); // (6)

  // Release lock ; thread will get it
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
  WAIT_WORKER(6); // (6)

  // We no longer have the lock
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
  lock1.l_type = F_RDLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));

  // Wake up thread to unlock exclusive lock
  PING_WORKER(3); // (R3)
  WAIT_WORKER(7); // (7)

  // We can lock it again
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));

  // Cleanup
  void *retval = (void*) (uintptr_t) -1;
  ASSERT_EQ(0, pthread_join(thread, &retval));
  ASSERT_EQ(NULL, retval);
  s.sem_destroy();
  ASSERT_EQ(0, ceph_close(cmount, fd));
  ASSERT_EQ(0, ceph_unlink(cmount, c_file));
  CLEANUP_CEPH();
}

TEST(LibCephFS, ThreesomeLocking) {
  const pid_t mypid = getpid();
  struct ceph_mount_info *cmount;
  STARTUP_CEPH();

  char c_file[1024];
  sprintf(c_file, "recordlock_test_%d", mypid);
  Fh *fh = NULL;
  Inode *root = NULL, *inode = NULL;
  struct stat attr;

  // Lock
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), true));

  // Start locker thread
  pthread_t thread[2];
  struct timespec ts;
  str_ConcurrentLocking s = { c_file, cmount };
  s.sem_init(0);
  ASSERT_EQ(0, pthread_create(&thread[0], NULL, thread_ConcurrentLocking_, &s));
  ASSERT_EQ(0, pthread_create(&thread[1], NULL, thread_ConcurrentLocking_, &s));
  // Synchronization point with thread (failure: thread is dead)
  TWICE(WAIT_WORKER(1)); // (1)

  // Shall not have lock immediately
  NOT_WAIT_WORKER(2); // (2)

  // Unlock
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));

  // Shall have lock
  TWICE(// Synchronization point with thread (failure: thread is dead)
	WAIT_WORKER(2); // (2)
	
	// Synchronization point with thread (failure: thread is dead)
	WAIT_WORKER(3)); // (3)
  
  // Wait for thread to share lock
  TWICE(WAIT_WORKER(4)); // (4)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
  lock1.l_type = F_RDLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));

  // Wake up thread to unlock shared lock
  TWICE(PING_WORKER(1); // (R1)
	WAIT_WORKER(5)); // (5)

  // Now we can lock exclusively
  // Upgrade to exclusive lock (as per POSIX)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), true));

  TWICE(  // Wake up thread to lock shared lock
	PING_WORKER(2); // (R2)
	
	// Shall not have lock immediately
	NOT_WAIT_WORKER(6)); // (6)
  
  // Release lock ; thread will get it
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
  TWICE(WAIT_WORKER(6); // (6)
	
	// We no longer have the lock
	lock1.l_type = F_WRLCK;
	lock1.l_whence = SEEK_SET;
	lock1.l_start = 0;
	lock1.l_len = 1024;
	lock1.l_pid = getpid();
	ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
	lock1.l_type = F_RDLCK;
	lock1.l_whence = SEEK_SET;
	lock1.l_start = 0;
	lock1.l_len = 1024;
	lock1.l_pid = getpid();
	ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
	
	// Wake up thread to unlock exclusive lock
	PING_WORKER(3); // (R3)
	WAIT_WORKER(7); // (7)
	);
  
  // We can lock it again
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, pthread_self(), false));

  // Cleanup
  void *retval = (void*) (uintptr_t) -1;
  ASSERT_EQ(0, pthread_join(thread[0], &retval));
  ASSERT_EQ(NULL, retval);
  ASSERT_EQ(0, pthread_join(thread[1], &retval));
  ASSERT_EQ(NULL, retval);
  s.sem_destroy();
  ASSERT_EQ(0, ceph_close(cmount, fd));
  ASSERT_EQ(0, ceph_unlink(cmount, c_file));
  CLEANUP_CEPH();
}

/* Locking in different processes */

#define PROCESS_SLOW_MS() \
  static const long waitMs = 100; \
  (void) waitMs

// Used by ConcurrentLocking test
static void process_ConcurrentLocking(str_ConcurrentLocking& s) {
  const pid_t mypid = getpid();
  PROCESS_SLOW_MS();

  struct ceph_mount_info *cmount = NULL;
  struct timespec ts;
  Fh *fh = NULL;
  Inode *root = NULL, *inode = NULL;
  struct stat attr;
  int rc;

  STARTUP_CEPH();
  s.cmount = cmount;

  // Get the root inode
  rc = ceph_ll_lookup_root(cmount, &root);
  ASSERT_EQ(rc, 0); 

  // Get the inode and Fh corresponding to c_file
  rc = ceph_ll_create(cmount, root, c_file, fileMode, O_RDWR | O_CREAT, &attr,
  		      &inode, &fh, 0, 0);
  ASSERT_EQ(rc, 0); 

  WAIT_MAIN(1); // (R1)

  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  PING_MAIN(1); // (1)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, true));
  PING_MAIN(2); // (2)

  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  PING_MAIN(3); // (3)

  lock1.l_type = F_RDLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, true));
  PING_MAIN(4); // (4)

  WAIT_MAIN(2); // (R2)
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  PING_MAIN(5); // (5)

  WAIT_MAIN(3); // (R3)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, true));
  PING_MAIN(6); // (6)

  WAIT_MAIN(4); // (R4)
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  PING_MAIN(7); // (7)

  CLEANUP_CEPH();

  s.sem_destroy();
  exit(EXIT_SUCCESS);
}

TEST(LibCephFS, InterProcessLocking) {
  PROCESS_SLOW_MS();
  // Process synchronization
  char c_file[1024];
  const pid_t mypid = getpid();
  sprintf(c_file, "recordlock_test_%d", mypid);
  Fh *fh = NULL;
  Inode *root = NULL, *inode = NULL;
  struct stat attr;

  // Note: the semaphores MUST be on a shared memory segment
  str_ConcurrentLocking *const shs =
    reinterpret_cast<str_ConcurrentLocking*>
    (mmap(0, sizeof(*shs), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
	  -1, 0));
  str_ConcurrentLocking &s = *shs;
  s.file = c_file;
  s.sem_init(1);

  // Start locker process
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    process_ConcurrentLocking(s);
    exit(EXIT_FAILURE);
  }

  struct timespec ts;
  struct ceph_mount_info *cmount;
  STARTUP_CEPH();

  const int fd = ceph_open(cmount, c_file, O_RDWR | O_CREAT, fileMode);
  ASSERT_GE(fd, 0); 

  // Lock
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, true));

  // Synchronization point with process (failure: process is dead)
  PING_WORKER(1); // (R1)
  WAIT_WORKER(1); // (1)

  // Shall not have lock immediately
  NOT_WAIT_WORKER(2); // (2)

  // Unlock
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));

  // Shall have lock
  // Synchronization point with process (failure: process is dead)
  WAIT_WORKER(2); // (2)

  // Synchronization point with process (failure: process is dead)
  WAIT_WORKER(3); // (3)

  // Wait for process to share lock
  WAIT_WORKER(4); // (4)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  lock1.l_type = F_RDLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));

  // Wake up process to unlock shared lock
  PING_WORKER(2); // (R2)
  WAIT_WORKER(5); // (5)

  // Now we can lock exclusively
  // Upgrade to exclusive lock (as per POSIX)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, true));

  // Wake up process to lock shared lock
  PING_WORKER(3); // (R3)

  // Shall not have lock immediately
  NOT_WAIT_WORKER(6); // (6)

  // Release lock ; process will get it
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  WAIT_WORKER(6); // (6)

  // We no longer have the lock
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  lock1.l_type = F_RDLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));

  // Wake up process to unlock exclusive lock
  PING_WORKER(4); // (R4)
  WAIT_WORKER(7); // (7)

  // We can lock it again
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));

  // Wait pid
  int status;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_EQ(EXIT_SUCCESS, status);

  // Cleanup
  s.sem_destroy();
  ASSERT_EQ(0, munmap(shs, sizeof(*shs)));
  ASSERT_EQ(0, ceph_close(cmount, fd));
  ASSERT_EQ(0, ceph_unlink(cmount, c_file));
  CLEANUP_CEPH();
}

TEST(LibCephFS, ThreesomeInterProcessLocking) {
  PROCESS_SLOW_MS();
  // Process synchronization
  char c_file[1024];
  const pid_t mypid = getpid();
  sprintf(c_file, "recordlock_test_%d", mypid);
  Fh *fh = NULL;
  Inode *root = NULL, *inode = NULL;
  struct stat attr;

  // Note: the semaphores MUST be on a shared memory segment
  str_ConcurrentLocking *const shs =
    reinterpret_cast<str_ConcurrentLocking*>
    (mmap(0, sizeof(*shs), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
	  -1, 0));
  str_ConcurrentLocking &s = *shs;
  s.file = c_file;
  s.sem_init(1);

  // Start locker processes
  pid_t pid[2];
  pid[0] = fork();
  ASSERT_GE(pid[0], 0);
  if (pid[0] == 0) {
    process_ConcurrentLocking(s);
    exit(EXIT_FAILURE);
  }
  pid[1] = fork();
  ASSERT_GE(pid[1], 0);
  if (pid[1] == 0) {
    process_ConcurrentLocking(s);
    exit(EXIT_FAILURE);
  }

  struct timespec ts;
  struct ceph_mount_info *cmount;
  STARTUP_CEPH();

  const int fd = ceph_open(cmount, c_file, O_RDWR | O_CREAT, fileMode);
  ASSERT_GE(fd, 0); 

  // Lock
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, true));

  // Synchronization point with process (failure: process is dead)
  TWICE(PING_WORKER(1)); // (R1)
  TWICE(WAIT_WORKER(1)); // (1)

  // Shall not have lock immediately
  NOT_WAIT_WORKER(2); // (2)

  // Unlock
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));

  // Shall have lock
  TWICE(// Synchronization point with process (failure: process is dead)
	WAIT_WORKER(2); // (2)
	
	// Synchronization point with process (failure: process is dead)
	WAIT_WORKER(3)); // (3)
  
  // Wait for process to share lock
  TWICE(WAIT_WORKER(4)); // (4)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(-EAGAIN, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  lock1.l_type = F_RDLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));

  // Wake up process to unlock shared lock
  TWICE(PING_WORKER(2); // (R2)
	WAIT_WORKER(5)); // (5)

  // Now we can lock exclusively
  // Upgrade to exclusive lock (as per POSIX)
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, true));

  TWICE(  // Wake up process to lock shared lock
	PING_WORKER(3); // (R3)
	
	// Shall not have lock immediately
	NOT_WAIT_WORKER(6)); // (6)
  
  // Release lock ; process will get it
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  TWICE(WAIT_WORKER(6); // (6)
	
	// We no longer have the lock
	ASSERT_EQ(-EWOULDBLOCK,
		  ceph_flock(cmount, fd, LOCK_EX | LOCK_NB, mypid));
	ASSERT_EQ(-EWOULDBLOCK,
		  ceph_flock(cmount, fd, LOCK_SH | LOCK_NB, mypid));
	
	// Wake up process to unlock exclusive lock
	PING_WORKER(4); // (R4)
	WAIT_WORKER(7); // (7)
	);
  
  // We can lock it again
  lock1.l_type = F_WRLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));
  lock1.l_type = F_UNLCK;
  lock1.l_whence = SEEK_SET;
  lock1.l_start = 0;
  lock1.l_len = 1024;
  lock1.l_pid = getpid();
  ASSERT_EQ(0, ceph_ll_setlk(cmount, fh, &lock1, mypid, false));

  // Wait pids
  int status;
  ASSERT_EQ(pid[0], waitpid(pid[0], &status, 0));
  ASSERT_EQ(EXIT_SUCCESS, status);
  ASSERT_EQ(pid[1], waitpid(pid[1], &status, 0));
  ASSERT_EQ(EXIT_SUCCESS, status);

  // Cleanup
  s.sem_destroy();
  ASSERT_EQ(0, munmap(shs, sizeof(*shs)));
  ASSERT_EQ(0, ceph_close(cmount, fd));
  ASSERT_EQ(0, ceph_unlink(cmount, c_file));
  CLEANUP_CEPH();
}
#endif
