#include "3d.h"
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#ifndef TARGET_WIN32
#include <pthread.h>
#else
#include <windows.h>
#include <synchapi.h>
#include <processthreadsapi.h>
#endif

#ifndef TARGET_WIN32
#define LOCK_CORE(core) pthread_mutex_lock(&cores[core].mutex);
#else
#define LOCK_CORE(core) WaitForSingleObject(cores[core].mutex,INFINITY);
#endif
#ifndef TARGET_WIN32
#define UNLOCK_CORE(core) pthread_mutex_unlock(&cores[core].mutex);
#else
#define UNLOCK_CORE(core) ReleaseMutex(cores[core].mutex);
#endif
typedef struct CThread {
    void *Fs;
    uint64_t sleep_until;
    int8_t locked;
    int8_t dead;
    int8_t to_kill;
    int8_t in_yield;
    int64_t *wait_for_ptr;
    int64_t wait_for_ptr_expected;
    int64_t wait_for_ptr_mask;
    int64_t core;
    void *stack;
    char *cur_dir;
    char cur_drv;
    ctx_t ctx;
} CThread;
typedef vec_t(CThread*) vec_CThread_t;
typedef struct CCore {
	vec_CThread_t *threads;
	vec_CThread_t *dead_threads;
	#ifndef TARGET_WIN32
	pthread_t pt;
	pthread_mutex_t mutex;
	#else
	HANDLE pt;
	HANDLE mutex;
	#endif
	int flop;
	int ready;
	void *gs;
	//
	//This is used to tell the core which thread to interupt
	// Once it is intereputed,it's RIP register is put to __interupt_to
	//
	CThread *__interupt_thread;
	void *__interupt_to;
	
} CCore;
static __thread int core_num;
static int core_cnt; 
CCore cores[64];
static void MakeContext(ctx_t *ctx,void *stack,void(*fptr)(void*),void *data) {
    ctx->rip=fptr;
    #ifdef TARGET_WIN32
    ctx->rcx=data;
    #else
    ctx->rdi=data;
    #endif
    ctx->rsp=stack+MSize(stack)-8*8;//Include area for home registers and minimum of 4 stack items
}
#include <sys/time.h>
static __thread void *Fs;
void *GetFs() {
    loop:
    if(!Fs) {
        Fs=TD_MALLOC(2048); //Is acutally 1198 but it could change
        goto loop;
    }
    return Fs;
}
typedef struct {
    void *fp;
    void *data;
    void *fs;
    struct CThread *thd;
} CPair;
static __thread CThread *cur_thrd;
static __thread vec_CThread_t threads;
static __thread vec_CThread_t dead_threads;
void __FreeThread(CThread *t) {
	if(!t) return;
    PoopAllocFreeTaskMem(t->Fs);
}
void __Exit() {
    vec_remove(&threads,cur_thrd);
    vec_push(&dead_threads,cur_thrd);
    cur_thrd->dead=1;
    __Yield();
}
static void __SetThreadPtr2(CThread *t,void *ptr) {
	if(!t) return;
	t->in_yield=0;
    t->ctx.rip=ptr;
}
void __SetThreadPtr(CThread *t,void *ptr) {
	LOCK_CORE(t->core);
	cores[t->core].__interupt_thread=t;
	cores[t->core].__interupt_to=ptr;
	pthread_kill(cores[t->core].pt,SIGUSR2);
	UNLOCK_CORE(t->core);
}
void __KillThread(CThread *t) {
	if(!t) return;
    CHash **ex=map_get(&TOSLoader,"Exit");
    if(t->to_kill) return;
    t->to_kill=1;
    if(cur_thrd==t) {
        if(ex)
            FFI_CALL_TOS_0(ex[0]->val);
        __Exit();
    } else {
        __SetThreadPtr(t,ex[0]->val);
    }
}
#ifndef TARGET_WIN32
static void SigUsr2(int sig) {
	sigset_t set;
	pthread_sigmask(0,NULL,&set);
	sigdelset(&set,SIGUSR2);
	pthread_sigmask(SIG_SETMASK,&set,NULL);
	LOCK_CORE(core_num);
	if(cores[core_num].__interupt_thread==cur_thrd) {
		UNLOCK_CORE(core_num);
		FFI_CALL_TOS_0(cores[core_num].__interupt_to);
	} else {
		__SetThreadPtr2(cores[core_num].__interupt_thread,cores[core_num].__interupt_to);
	}
	UNLOCK_CORE(core_num);
}
#endif
static int64_t __SpawnFFI(CPair *p) {
    CHash **ex;
    VFsThrdInit();
    FFI_CALL_TOS_1(p->fp,p->data);
    if(ex=map_get(&TOSLoader,"Exit")) {
        FFI_CALL_TOS_0(ex[0]->val);
    }
    __Exit();
}
CThread *__MPSpawn(int core,void *fs,void *fp,void *data,char *name) {
    CThread *thd=TD_MALLOC(sizeof(CThread));
    CPair *p=TD_MALLOC(sizeof(CPair));
    p->fp=fp,p->data=data,p->fs=fs,p->thd=thd;
    signal(SIGSEGV,FualtCB);
    #ifndef TARGET_WIN32
    signal(SIGBUS,FualtCB);
    #endif
    thd->Fs=p->fs;
    thd->cur_dir=cur_dir;
    thd->cur_drv=cur_drv;
    thd->core=core;
    GetContext(&thd->ctx);
    if(!thd->dead) {
		thd->stack=TD_MALLOC(2000000); //aprx 2Mb
        MakeContext(&thd->ctx,thd->stack,&__SpawnFFI,p);
        while(!atomic_load(&cores[core].ready))
			sched_yield();
		LOCK_CORE(core);
        vec_push(cores[core].threads,thd);
        UNLOCK_CORE(core);
    }
    return thd;
}
CThread *__Spawn(void *fs,void *fp,void *data,char *name) {
	return __MPSpawn(core_num,fs,fp,data,name);
} 
void __AwakeThread(CThread *t) {
	if(!t) return;
    t->locked=0;
}
void __Suspend(CThread *t) {
	if(!t) return;
    t->locked=3;
    if(t==cur_thrd) __Yield();
}
void __Sleep(int64_t t) {
    if(cur_thrd)
        cur_thrd->sleep_until=t+SDL_GetTicks();
    __Yield();
}
void __SleepUntilValue(int64_t *ptr,int64_t mask,int64_t expected) {
    if(cur_thrd) {
        cur_thrd->wait_for_ptr=ptr;
        cur_thrd->wait_for_ptr_expected=expected;
        cur_thrd->wait_for_ptr_mask=mask;
        cur_thrd->locked=2;
    }
    __Yield();
}
static int ThrdIsReady(CThread *thr) {
	if(thr->dead||thr->locked) return 0;
	if(thr->sleep_until&&(thr->sleep_until>=SDL_GetTicks()))
		return 0;
	return 1;
}
void __Yield() {
    enter:;
    #ifndef TARGET_WIN32
    sched_yield();
    #endif
    ctx_t old;
    GetContext(&old);
    int64_t i;
    LOCK_CORE(core_num);
    for(i=0;i!=threads.length;i++)
        if(threads.data[i]->to_kill) {
            if(cur_thrd!=threads.data[i]) {
                __KillThread(threads.data[i]);
                threads.data[i]->to_kill=0;
            }
        } else if(threads.data[i]->wait_for_ptr) {
            int64_t mask=threads.data[i]->wait_for_ptr_mask;
            if((threads.data[i]->wait_for_ptr_expected&mask)==(mask&atomic_load(threads.data[i]->wait_for_ptr))) {
                threads.data[i]->locked=1;
            } else {
                threads.data[i]->wait_for_ptr=NULL;
                threads.data[i]->locked=0;
            }
        }
    rem:
    for(i=0;i!=dead_threads.length;i++) {
		if(cur_thrd!=dead_threads.data[i]) {
            __FreeThread(dead_threads.data[i]);
			TD_FREE(dead_threads.data[i]->stack);
			TD_FREE(dead_threads.data[i]);
			vec_remove(&dead_threads,dead_threads.data[i]);
            goto rem;
		}
	}
    loop:;
    int64_t ms=SDL_GetTicks(),min_sleep=-1,t,most_overdue_i=-1;
    int64_t dont_sleep=0;
    int64_t idx,idx2=threads.length-1,idx3=-1;
    vec_find(&threads,cur_thrd,idx);
    if(idx==-1) {
        idx=0;
        idx2=threads.length-1;
        if(idx2==-1) {
			UNLOCK_CORE(core_num);
			return;
		}
        goto ent;
    }
    threads.data[idx]->in_yield=1;
    GetContext(&threads.data[idx]->ctx);
    //idx doesnt change but the phyiscall index of the CAN CHANGE,SO RECOMPUTE 
    vec_find(&threads,cur_thrd,idx);
    ms=SDL_GetTicks();
    if(ThrdIsReady(threads.data[idx])) {
		if(!(cores[core_num].flop^=1)) {
			threads.data[idx]->in_yield=0;
			UNLOCK_CORE(core_num);
			return;
		}
	}
    check:
    for(idx2=(idx+1)%threads.length;idx!=idx2;idx2=(1+idx2)%threads.length) {
        if(threads.data[idx2]->locked||threads.data[idx2]->dead) {
            continue;
        } else if(threads.data[idx2]->sleep_until) {
			t=threads.data[idx2]->sleep_until-ms;
			if(t<=0) {
				if(most_overdue_i==-1)
					most_overdue_i=idx2;
				else if(threads.data[most_overdue_i]->sleep_until>threads.data[idx2]->sleep_until)
					most_overdue_i=idx2;			
			}
            if(min_sleep==-1)
				min_sleep=t;
            else if(min_sleep>0)
                min_sleep=(min_sleep<t)?min_sleep:t;
            if(min_sleep<0)
                min_sleep=-1;
            if(threads.data[idx2]->sleep_until<=ms) {
				threads.data[idx2]->sleep_until=0;
				goto pass;
            }
        } else {
			if(idx3==-1) idx3=idx2;
			continue;
			pass:
            if(ThrdIsReady(threads.data[idx2])) {
				threads.data[idx]->cur_drv=cur_drv;
				threads.data[idx]->cur_dir=cur_dir;
				threads.data[idx]->Fs=Fs;
				//
				ent:
				cur_thrd=threads.data[idx2];
				Fs=threads.data[idx2]->Fs;
				cur_dir=threads.data[idx2]->cur_dir;
				cur_drv=threads.data[idx2]->cur_drv;
				if(!threads.data[idx2]->in_yield)
					UNLOCK_CORE(core_num);
				SetContext(&threads.data[idx2]->ctx);
			}
			break;
        }
    }
    if(most_overdue_i!=-1) {
		idx2=most_overdue_i;
		goto pass;
	} else if(idx3!=-1) {
		idx2=idx3;
		goto pass;
    }
    if(min_sleep>0&&!dont_sleep) {//We skipped over the current thread when looking for canidates to swap too
		SDL_Delay((int64_t)min_sleep);
    }
    goto loop;
}
void __AwaitThread(CThread *t) {
	if(!t) return;
    for(;;) {
        if(t->dead) {
            break;
        } else
            __Yield();
    }
}
int InitThreadsForCore() {
    vec_init(&threads);
    vec_init(&dead_threads);
    VFsThrdInit(); 
    int num=atomic_fetch_add(&core_cnt,1);
    core_num=num;
    cores[num].pt=pthread_self();
    cores[num].mutex=PTHREAD_MUTEX_INITIALIZER;
    cores[num].gs=PoopMAlloc(512); //Should be enough
    cores[num].threads=&threads;
    cores[num].dead_threads=&dead_threads;
    atomic_fetch_add(&cores[num].ready,1);
    return num;
}
static void Loop(void*ul) {
		//Make a dummy "thread" to yield out to if we have no other threads 
		CThread *thd=TD_MALLOC(sizeof(CThread));
		InitThreadsForCore();
		vec_push(&threads,thd);
		GetContext(&thd->ctx);
		for(;;) {
			SDL_Delay(50);
			__Yield();
		}
}
void SpawnCore() {
	pthread_t dummy;
	#ifndef TARGET_WIN32
	pthread_create(&dummy,NULL,&Loop,NULL);
	signal(SIGUSR2,SigUsr2);
	#else
	CreateThread(NULL,0,&Loop,NULL,0,NULL);
	#endif
}
int CoreNum() {
	return core_num;
}
void *GetGs() {
	return cores[core_num].gs;
}
