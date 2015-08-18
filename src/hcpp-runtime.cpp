/* Copyright (c) 2015, Rice University

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1.  Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
2.  Redistributions in binary form must reproduce the above
     copyright notice, this list of conditions and the following
     disclaimer in the documentation and/or other materials provided
     with the distribution.
3.  Neither the name of Rice University
     nor the names of its contributors may be used to endorse or
     promote products derived from this software without specific
     prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

/*
 * hcpp-runtime.cpp
 *
 *      Author: Vivek Kumar (vivekk@rice.edu)
 *      Acknowledgments: https://wiki.rice.edu/confluence/display/HABANERO/People
 */

#include "hcpp-internal.h"
#include <pthread.h>
#include "hcpp-atomics.h"
#include <sys/time.h>

namespace hcpp {

using namespace std;

static double benchmark_start_time_stats = 0;
pthread_key_t wskey;
pthread_once_t selfKeyInitialized = PTHREAD_ONCE_INIT;

#ifdef CRT_COMM_WORKER
semiConcDeque_t * comm_worker_out_deque;
#endif

static finish_t*	root_finish;

hc_context* 		crt_context;
hc_options* 		crt_options;

asyncAnyInfo*  asyncAnyInfo_forWorker;
commWorkerAsyncAny_infoStruct_t *commWorkerAsyncAny_infoStruct;

void log_(const char * file, int line, hc_workerState * ws, const char * format, ...) {
	va_list l;
	FILE * f = stderr;
	if (ws != NULL) {
		fprintf(f, "[worker: %d (%s:%d)] ", ws->id, file, line);
	} else {
		fprintf(f, "[%s:%d] ", file, line);
	}
	va_start(l, format);
	vfprintf(f, format, l);
	fflush(f);
	va_end(l);
}

// Statistics
int total_push_outd;
int* total_push_ind;
int* total_steals;

inline void increment_async_counter(int wid) {
	total_push_ind[wid]++;
}

inline void increment_steals_counter(int wid) {
	total_steals[wid]++;
}

inline void increment_asyncComm_counter() {
	total_push_outd++;
}

// One global finish scope

static void initializeKey() {
	pthread_key_create(&wskey, NULL);
}

void set_current_worker(int wid) {
	pthread_setspecific(wskey, crt_context->workers[wid]);
	if(getenv("CRT_BIND_THREADS")) {
		bind_thread(wid, NULL, 0);
	}
}

inline int get_current_worker() {
	return ((hc_workerState*)pthread_getspecific(wskey))->id;
}

//FWD declaration for pthread_create
void * worker_routine(void * args);

void crt_global_init(bool HPT) {
	// Build queues
	crt_context->done = 1;
	if(!HPT) {
		crt_context->nproc = crt_options->nproc;
		crt_context->nworkers = crt_options->nworkers;
		crt_context->options = crt_options;

		place_t * root = (place_t*) malloc(sizeof(place_t));
		root->id = 0;
		root->nnext = NULL;
		root->child = NULL;
		root->parent = NULL;
		root->type = MEM_PLACE;
		root->ndeques = crt_context->nworkers;
		crt_context->hpt = root;
		crt_context->places = &crt_context->hpt;
		crt_context->nplaces = 1;
		crt_context->workers = (hc_workerState**) malloc(sizeof(hc_workerState*) * crt_options->nworkers);
		HASSERT(crt_context->workers);
		hc_workerState * cur_ws = NULL;
		for(int i=0; i<crt_options->nworkers; i++) {
			crt_context->workers[i] = new hc_workerState;
			HASSERT(crt_context->workers[i]);
			crt_context->workers[i]->context = crt_context;
			crt_context->workers[i]->id = i;
			crt_context->workers[i]->pl = root;
			crt_context->workers[i]->hpt_path = NULL;
			crt_context->workers[i]->nnext = NULL;
			if (i == 0) {
				cur_ws = crt_context->workers[i];
			} else {
				cur_ws->nnext = crt_context->workers[i];
				cur_ws = crt_context->workers[i];
			}
		}
		root->workers = crt_context->workers[0];
	}
	else {
		crt_context->hpt = readhpt(&crt_context->places, &crt_context->nplaces, &crt_context->nproc, &crt_context->workers, &crt_context->nworkers);
		for (int i=0; i<crt_context->nworkers; i++) {
			hc_workerState * ws = crt_context->workers[i];
			ws->context = crt_context;
		}
	}

	total_push_outd = 0;
	total_steals = new int[crt_context->nworkers];
	total_push_ind = new int[crt_context->nworkers];
	for(int i=0; i<crt_context->nworkers; i++) {
		total_steals[i] = 0;
		total_push_ind[i] = 0;
	}

#ifdef CRT_COMM_WORKER
	comm_worker_out_deque = new semiConcDeque_t;
	HASSERT(comm_worker_out_deque);
	semiConcDequeInit(comm_worker_out_deque, NULL);
#endif

	asyncAnyInfo_forWorker = (asyncAnyInfo*) malloc(sizeof(asyncAnyInfo) * crt_context->nworkers);
	for(int i=0; i<crt_context->nworkers; i++) {
		asyncAnyInfo_forWorker[i].asyncAny_pushed = 0;
		asyncAnyInfo_forWorker[i].asyncAny_stolen = 0;
	}
	commWorkerAsyncAny_infoStruct = new commWorkerAsyncAny_infoStruct_t;
	commWorkerAsyncAny_infoStruct->ptr_to_outgoingAsyncAny = NULL;
	commWorkerAsyncAny_infoStruct->initiatePackingOfAsyncAny = false;
}

void crt_createWorkerThreads(int nb_workers) {
	/* setting current thread as worker 0 */
	// Launch the worker threads
	pthread_once(&selfKeyInitialized, initializeKey);
	// Start workers
	for(int i=1;i<nb_workers;i++) {
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_create(&crt_context->workers[i]->t, &attr, &worker_routine, &crt_context->workers[i]->id);
	}
	set_current_worker(0);
}

void display_runtime() {
	cout << "---------CRT_RUNTIME_INFO-----------" << endl;
	printf(">>> CRT_WORKERS\t\t= %s\n",getenv("CRT_WORKERS"));
	printf(">>> CRT_HPT_FILE\t= %s\n",getenv("CRT_HPT_FILE"));
	printf(">>> CRT_BIND_THREADS\t= %s\n",getenv("CRT_BIND_THREADS"));
	if(getenv("CRT_WORKERS") && getenv("CRT_BIND_THREADS")) {
		printf("WARNING: CRT_BIND_THREADS assign cores in round robin. E.g., setting CRT_WORKERS=12 on 2-socket node, each with 12 cores, will assign both HCUPC++ places on same socket\n");
	}
	printf(">>> CRT_STATS\t\t= %s\n",getenv("CRT_STATS"));
	cout << "----------------------------------------" << endl;
}

void crt_entrypoint(bool HPT) {
	if(getenv("CRT_STATS")) {
		display_runtime();
	}

	srand(0);

	crt_options = new hc_options;
	HASSERT(crt_options);
	crt_context = new hc_context;
	HASSERT(crt_context);

	if(!HPT) {
		char* workers_env = getenv("CRT_WORKERS");
		int workers = 1;
		if(!workers_env) {
			std::cout << "CRT: WARNING -- Number of workers not set. Please set using env CRT_WORKERS" << std::endl;
		}
		else {
			workers = atoi(workers_env);
		}
		HASSERT(workers > 0);
		crt_options->nworkers = workers;
		crt_options->nproc = workers;
	}

	crt_global_init(HPT);

#ifdef __USE_HC_MM__
	const char* mm_alloc_batch_size = getenv("CRT_MM_ALLOCBATCHSIZE");
	const int mm_alloc_batch_size_int = mm_alloc_batch_size ? atoi(mm_alloc_batch_size) : HC_MM_ALLOC_BATCH_SIZE;
	crt_options->alloc_batch_size = mm_alloc_batch_size_int;
	hc_mm_init(crt_context);
#endif

	hc_hpt_init(crt_context);
#if TODO
	hc_hpt_dev_init(crt_context);
#endif
	/* Create key to store per thread worker_state */
	if (pthread_key_create(&wskey, NULL) != 0) {
		log_die("Cannot create wskey for worker-specific data");
	}

	/* set pthread's concurrency. Doesn't seem to do much on Linux */
	pthread_setconcurrency(crt_context->nworkers);

	/* Create all worker threads  */
	crt_createWorkerThreads(crt_context->nworkers);

	// allocate root finish
	root_finish = new finish_t;
	current_ws()->current_finish = root_finish;
	start_finish();
}

void crt_join_workers(int nb_workers) {
	// Join the workers
	crt_context->done = 0;
	for(int i=1;i< nb_workers; i++) {
		pthread_join(crt_context->workers[i]->t, NULL);
	}
}

void crt_cleanup() {
	hc_hpt_dev_cleanup(crt_context);
	hc_hpt_cleanup_1(crt_context); /* cleanup deques (allocated by hc mm) */
#ifdef USE_HC_MM
	hc_mm_cleanup(crt_context);
#endif
	hc_hpt_cleanup_2(crt_context); /* cleanup the HPT, places, and workers (allocated by malloc) */
	pthread_key_delete(wskey);

	free(crt_context);
	free(crt_options);
	free(total_steals);
	free(total_push_ind);
	free(asyncAnyInfo_forWorker);
}

inline void check_in_finish(finish_t * finish) {
	hc_atomic_inc(&(finish->counter));
}

inline void check_out_finish(finish_t * finish) {
	hc_atomic_dec(&(finish->counter));
}

inline void execute_task(task_t* task) {
	finish_t* current_finish = task->get_current_finish();
	current_ws()->current_finish = current_finish;

	(task->_fp)(task->_args);
	check_out_finish(current_finish);
	HC_FREE(task);
}

inline void rt_schedule_async(task_t* async_task, int comm_task) {
	if(comm_task) {
#ifdef CRT_COMM_WORKER
		// push on comm_worker out_deq
		semiConcDequeLockedPush(comm_worker_out_deque, (task_t*) async_task);
#endif
	}
	else {
		// push on worker deq
		if(!dequePush(&(crt_context->workers[get_current_worker()]->current->deque), (task_t*) async_task)) {
			// TODO: deque is full, so execute in place
			printf("WARNING: deque full, local executino\n");
			execute_task((task_t*) async_task);
		}
	}
}

void try_schedule_async(task_t * async_task, int comm_task) {
    if (is_eligible_to_schedule(async_task)) {
        rt_schedule_async(async_task, comm_task);
    }
}

void spawn(task_t * task) {
	// get current worker
	hc_workerState* ws = current_ws();
	check_in_finish(ws->current_finish);
	task->set_current_finish(ws->current_finish);
	try_schedule_async(task, 0);
#ifdef HC_COMM_WORKER_STATS
	const int wid = get_current_worker();
	increment_async_counter(wid);
#endif

}

void spawn_commTask(task_t * task) {
#ifdef CRT_COMM_WORKER
	hc_workerState* ws = current_ws();
	check_in_finish(ws->current_finish);
	task->set_current_finish(ws->current_finish);
	try_schedule_async(task, 1);
#else
	assert(false);
#endif
}

void spawn_asyncAnyTask(task_t* task) {
	// get current worker
	hc_workerState* ws = current_ws();
	const int wid = get_current_worker();
	check_in_finish(ws->current_finish);
	task->set_current_finish(ws->current_finish);
	try_schedule_async(task, 0);
	asyncAnyInfo_forWorker[wid].asyncAny_pushed++;
#ifdef HC_COMM_WORKER_STATS
	increment_async_counter(wid);
#endif
}

inline void slave_worker_finishHelper_routine(finish_t* finish) {
	hc_workerState* ws = current_ws();
	int wid = ws->id;

	while(finish->counter > 0) {
		// try to pop
		task_t* task = hpt_pop_task(ws);
		if (!task) {
			while(finish->counter > 0) {
				// try to steal
				task = hpt_steal_task(ws);
				if (task) {
#ifdef HC_COMM_WORKER_STATS
					increment_steals_counter(wid);
#endif
					break;
				}
			}
		}
		if(task) {
			execute_task(task);
		}
	}
}

#ifdef CRT_COMM_WORKER
inline void master_worker_routine(finish_t* finish) {
	semiConcDeque_t *deque = comm_worker_out_deque;
	while(finish->counter > 0) {
		// try to pop
		task_t* task = semiConcDequeNonLockedPop(deque);
		// Comm worker cannot steal
		if(task) {
#ifdef HC_COMM_WORKER_STATS
			increment_asyncComm_counter();
#endif
			execute_task(task);
		}
	}
}
#endif

void* worker_routine(void * args) {
	int wid = *((int *) args);
	set_current_worker(wid);

	hc_workerState* ws = current_ws();

	while(crt_context->done) {
		task_t* task = hpt_pop_task(ws);
		if (!task) {
			while(crt_context->done) {
				// try to steal
				task = hpt_steal_task(ws);
				if (task) {
#ifdef HC_COMM_WORKER_STATS
					increment_steals_counter(wid);
#endif
					break;
				}
			}
		}

		if(task) {
			execute_task(task);
		}
	}

	return NULL;
}

void teardown() {

}

inline void help_finish(finish_t * finish) {
#ifdef CRT_COMM_WORKER
	if(current_ws()->id == 0) {
		master_worker_routine(finish);
	}
	else {
		slave_worker_finishHelper_routine(finish);
	}
#else
	slave_worker_finishHelper_routine(finish);
#endif
}

/*
 * =================== INTERFACE TO USER FUNCTIONS ==========================
 */

volatile int* start_finish_special() {
	hc_workerState* ws = current_ws();
	finish_t * finish = (finish_t*) HC_MALLOC(sizeof(finish_t));
	finish->counter = 0;
	finish->parent = ws->current_finish;
	if(finish->parent) {
		check_in_finish(finish->parent);
	}
	ws->current_finish = finish;
	return &(finish->counter);
}

void start_finish() {
	hc_workerState* ws = current_ws();
	finish_t * finish = (finish_t*) HC_MALLOC(sizeof(finish_t));
	finish->counter = 0;
	finish->parent = ws->current_finish;
	if(finish->parent) {
		check_in_finish(finish->parent);
	}
	ws->current_finish = finish;
}

void end_finish() {
	hc_workerState* ws =current_ws();
	finish_t* current_finish = ws->current_finish;

	if (current_finish->counter > 0) {
		help_finish(current_finish);
	}
	HASSERT(current_finish->counter == 0);

	if(current_finish->parent) {
		check_out_finish(current_finish->parent);
	}

	ws->current_finish = current_finish->parent;
	HC_FREE(current_finish);
}

void finish(std::function<void()> lambda) {
	start_finish();
	lambda();
	end_finish();
}

/*
 * snapshot of total asyncAny tasks currently available with all computation workers
 */
int totalAsyncAnyAvailable() {
	int total_asyncany = 0;
	for(int i=1; i<crt_context->nworkers; i++) {
		total_asyncany += (asyncAnyInfo_forWorker[i].asyncAny_pushed - asyncAnyInfo_forWorker[i].asyncAny_stolen);
	}
	return total_asyncany;
}

int totalPendingLocalAsyncs() {
	/*
	 * snapshot of all pending tasks at all workers
	 */
#if 1
	return current_ws()->current_finish->counter;
#else
	int pending_tasks = 0;
	for(int i=0; i<crt_context->nworkers; i++) {
		hc_workerState* ws = crt_context->workers[i];
		const finish_t* ws_curr_f_i = ws->current_finish;
		if(ws_curr_f_i) {
			bool found = false;
			for(int j=0; j<i; j++) {
				const finish_t* ws_curr_f_j = crt_context->workers[j]->current_finish;
				if(ws_curr_f_j && ws_curr_f_j == ws_curr_f_i) {
					found = true;
					break;
				}
			}
			if(!found) pending_tasks += ws_curr_f_i->counter;
		}
	}

	return pending_tasks;
#endif
}

int numWorkers() {
	return crt_context->nworkers;
}

int get_hc_wid() {
	return get_current_worker();
}

void gather_commWorker_Stats(int* push_outd, int* push_ind, int* steal_ind) {
	int asyncPush=0, steals=0, asyncCommPush=total_push_outd;
	for(int i=0; i<numWorkers(); i++) {
		asyncPush += total_push_ind[i];
		steals += total_steals[i];
	}
	*push_outd = asyncCommPush;
	*push_ind = asyncPush;
	*steal_ind = steals;
}

double mysecond() {
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec + ((double) tv.tv_usec / 1000000);
}

void runtime_statistics(double duration) {
	int asyncPush=0, steals=0, asyncCommPush=total_push_outd;
	for(int i=0; i<numWorkers(); i++) {
		asyncPush += total_push_ind[i];
		steals += total_steals[i];
	}

	printf("============================ MMTk Statistics Totals ============================\n");
	printf("time.mu\ttotalPushOutDeq\ttotalPushInDeq\ttotalStealsInDeq\n");
	printf("%.3f\t%d\t%d\t%d\n",duration,asyncCommPush,asyncPush,steals);
	printf("Total time: %.3f ms\n",duration);
	printf("------------------------------ End MMTk Statistics -----------------------------\n");
	printf("===== TEST PASSED in %.3f msec =====\n",duration);
}

void showStatsHeader() {
	cout << endl;
	cout << "-----" << endl;
	cout << "mkdir timedrun fake" << endl;
	cout << endl;
	cout << "-----" << endl;
	benchmark_start_time_stats = mysecond();
}

void showStatsFooter() {
	double end = mysecond();
	HASSERT(benchmark_start_time_stats != 0);
	double dur = (end-benchmark_start_time_stats)*1000;
	runtime_statistics(dur);
}

void init(int * argc, char ** argv) {
	if(getenv("CRT_STATS")) {
		showStatsHeader();
	}
	// get the total number of workers from the env
	const bool HPT = getenv("CRT_HPT_FILE") != NULL;
	crt_entrypoint(HPT);
}

void finalize() {
	end_finish();
	free(root_finish);

	if(getenv("CRT_STATS")) {
		showStatsFooter();
	}

	crt_join_workers(crt_context->nworkers);
	crt_cleanup();
}

}