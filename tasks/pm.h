/*
 * pm.h
 *
 *  Created on: 30.12.2012
 *      Author: pascal
 */

#ifndef PM_H_
#define PM_H_

#include "isr.h"
#include "stdint.h"
#include "vmm.h"
#include "list.h"
#include "hashmap.h"
#include "lock.h"
#include <bits/types.h>

typedef _pid_t pid_t;

typedef struct{
		uint64_t mmx[6];
		uint128_t ymm[16][2];		//YMM-Register sind 256-Bit breit
}__attribute__((aligned(16)))ihs_extended_t;

typedef enum{
	PM_BLOCKED, PM_RUNNING, PM_TERMINATED
}pm_status_t;

typedef struct process_t{
		context_t *Context;
		pid_t PID;
		struct process_t *parent;
		char *cmd;
		pm_status_t Status;
		list_t threads, terminated_childs, waiting_threads, waiting_threads_pid;
		hashmap_t *streams;
		void *nextThreadStack;
		lock_t lock;
		int exit_status;
}process_t;

extern process_t *currentProcess;			//Aktueller Prozess

void pm_Init(void);
process_t *pm_InitTask(process_t *parent, void *entry, char* cmd, const char **env, const char *stdin, const char *stdout, const char *stderr);
void pm_DestroyTask(process_t *process);
void pm_ExitTask(int code);
void pm_BlockTask(process_t *process);
void pm_ActivateTask(process_t *process);
process_t *pm_getTask(pid_t PID);
pid_t pm_WaitChild(pid_t pid, int *status);

//syscalls
void pm_syscall_exit(int status);
pid_t pm_syscall_wait(pid_t pid, int *status);

#endif /* PM_H_ */
