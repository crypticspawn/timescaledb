#include <postgres.h>

#include <miscadmin.h>
#include <postmaster/bgworker.h>
#include <storage/ipc.h>
#include <storage/latch.h>
#include <storage/lwlock.h>
#include <storage/proc.h>
#include <storage/shmem.h>
#include <utils/jsonb.h>
#include <utils/timestamp.h>
#include <utils/snapmgr.h>
#include <utils/memutils.h>
#include <utils/builtins.h>
#include <access/xact.h>
#include <pgstat.h>
#include <signal.h>

#include "extension.h"
#include "log.h"
#include "bgw/scheduler.h"
#include "bgw/job.h"
#include "bgw/job_stat.h"
#include "timer_mock.h"
#include "params.h"


TS_FUNCTION_INFO_V1(ts_bgw_db_scheduler_test_run_and_wait_for_scheduler_finish);
TS_FUNCTION_INFO_V1(ts_bgw_db_scheduler_test_run);
TS_FUNCTION_INFO_V1(ts_bgw_db_scheduler_test_wait_for_scheduler_finish);
TS_FUNCTION_INFO_V1(ts_bgw_db_scheduler_test_main);
TS_FUNCTION_INFO_V1(ts_bgw_job_execute_test);

typedef enum TestJobType
{
	TEST_JOB_TYPE_JOB_1 = 0,
	TEST_JOB_TYPE_JOB_2_ERROR,
	TEST_JOB_TYPE_JOB_3_LONG,
	_MAX_TEST_JOB_TYPE
} TestJobType;

const char *test_job_type_names[_MAX_TEST_JOB_TYPE] = {
	[TEST_JOB_TYPE_JOB_1] = "bgw_test_job_1",
	[TEST_JOB_TYPE_JOB_2_ERROR] = "bgw_test_job_2_error",
	[TEST_JOB_TYPE_JOB_3_LONG] = "bgw_test_job_3_long",
};

static char *
serialize_test_parameters(int32 ttl)
{
	JsonbValue *result;
	JsonbValue	ttl_value;
	JsonbParseState *parseState = NULL;
	Jsonb	   *jb;
	StringInfo	jtext = makeStringInfo();

	ttl_value.type = jbvNumeric;
	ttl_value.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(ttl)));

	result = pushJsonbValue(&parseState, WJB_BEGIN_ARRAY, NULL);

	result = pushJsonbValue(&parseState, WJB_ELEM, &ttl_value);

	result = pushJsonbValue(&parseState, WJB_END_ARRAY, NULL);

	jb = JsonbValueToJsonb(result);
	(void) JsonbToCString(jtext, &jb->root, VARSIZE(jb));
	Assert(jtext->len < BGW_EXTRALEN);

	return jtext->data;
}

static void
deserialize_test_parameters(char *params, int32 *ttl)
{
	Jsonb	   *jb = (Jsonb *) DatumGetPointer(DirectFunctionCall1(jsonb_in, CStringGetDatum(params)));
	JsonbValue *ttl_v = getIthJsonbValueFromContainer(&jb->root, 0);
	Numeric		ttl_numeric;

	Assert(ttl_v->type == jbvNumeric);

	ttl_numeric = ttl_v->val.numeric;
	*ttl = DatumGetInt32(DirectFunctionCall1(numeric_int4, NumericGetDatum(ttl_numeric)));
}

extern Datum
ts_bgw_db_scheduler_test_main(PG_FUNCTION_ARGS)
{
	Oid			db_oid = DatumGetObjectId(MyBgworkerEntry->bgw_main_arg);
	int32		ttl;

	BackgroundWorkerBlockSignals();
	/* Setup any signal handlers here */
	BackgroundWorkerUnblockSignals();
	bgw_scheduler_setup_callbacks();

	deserialize_test_parameters(MyBgworkerEntry->bgw_extra, &ttl);

	elog(WARNING, "running a test in the background: db=%d ttl=%d", db_oid, ttl);

	BackgroundWorkerInitializeConnectionByOid(db_oid, InvalidOid);

	StartTransactionCommand();
	params_get();
	CommitTransactionCommand();
	bgw_log_set_application_name("DB Scheduler");
	register_emit_log_hook();

	timer_set(&mock_timer);

	bgw_job_set_job_entrypoint_function_name("ts_bgw_job_execute_test");

	pgstat_report_appname("DB Scheduler Test");

	bgw_scheduler_process(ttl, timer_mock_register_bgw_handle);

	PG_RETURN_VOID();
}

extern Datum
ts_bgw_db_scheduler_test_run_and_wait_for_scheduler_finish(PG_FUNCTION_ARGS)
{
	char	   *params = serialize_test_parameters(PG_GETARG_INT32(0));
	BackgroundWorkerHandle *worker_handle;
	pid_t		pid;

	worker_handle = bgw_start_worker("ts_bgw_db_scheduler_test_main", "ts_bgw_db_scheduler_test_main", params);

	Assert(BGWH_STARTED == WaitForBackgroundWorkerStartup(worker_handle, &pid));
	Assert(BGWH_STOPPED == WaitForBackgroundWorkerShutdown(worker_handle));

	PG_RETURN_VOID();
}

static BackgroundWorkerHandle *current_handle = NULL;

extern Datum
ts_bgw_db_scheduler_test_run(PG_FUNCTION_ARGS)
{
	char	   *params = serialize_test_parameters(PG_GETARG_INT32(0));
	pid_t		pid;
	MemoryContext old_ctx;

	old_ctx = MemoryContextSwitchTo(TopMemoryContext);
	current_handle = bgw_start_worker("ts_bgw_db_scheduler_test_main", "ts_bgw_db_scheduler_test_main", params);
	MemoryContextSwitchTo(old_ctx);


	Assert(BGWH_STARTED == WaitForBackgroundWorkerStartup(current_handle, &pid));

	PG_RETURN_VOID();
}

extern Datum
ts_bgw_db_scheduler_test_wait_for_scheduler_finish(PG_FUNCTION_ARGS)
{
	Assert(BGWH_STOPPED == WaitForBackgroundWorkerShutdown(current_handle));
	PG_RETURN_VOID();
}
static bool
test_job_1()
{
	StartTransactionCommand();
	elog(WARNING, "Execute job 1");

	CommitTransactionCommand();
	return true;
}

static bool
test_job_2_error()
{
	StartTransactionCommand();
	elog(WARNING, "Before error job 2");

	elog(ERROR, "Error job 2");

	elog(WARNING, "After error job 2");

	CommitTransactionCommand();
	return true;
}

static pqsigfunc prev_signal_func = NULL;

static void
log_terminate_signal(SIGNAL_ARGS)
{
	elog(WARNING, "Job got term signal");

	if (prev_signal_func != NULL)
		prev_signal_func(postgres_signal_arg);
}

static bool
test_job_3_long()
{
	BackgroundWorkerBlockSignals();

	/*
	 * Only set prev_signal_func once to prevent it from being set to
	 * log_terminate_signal.
	 */
	if (prev_signal_func == NULL)
		prev_signal_func = pqsignal(SIGTERM, log_terminate_signal);
	/* Setup any signal handlers here */
	BackgroundWorkerUnblockSignals();

	elog(WARNING, "Before sleep job 3");

	DirectFunctionCall1(pg_sleep, Float8GetDatum(0.5L));

	elog(WARNING, "After sleep job 3");
	return true;
}

static TestJobType
get_test_job_type_from_name(Name job_type_name)
{
	int			i;

	for (i = 0; i < _MAX_TEST_JOB_TYPE; i++)
	{
		if (namestrcmp(job_type_name, test_job_type_names[i]) == 0)
			return i;
	}
	return _MAX_TEST_JOB_TYPE;
}

static bool
test_job_dispatcher(BgwJob *job)
{
	register_emit_log_hook();
	bgw_log_set_application_name(NameStr(job->fd.application_name));

	StartTransactionCommand();
	params_get();
	CommitTransactionCommand();

	switch (get_test_job_type_from_name(&job->fd.job_type))
	{
		case TEST_JOB_TYPE_JOB_1:
			return test_job_1();
		case TEST_JOB_TYPE_JOB_2_ERROR:
			return test_job_2_error();
		case TEST_JOB_TYPE_JOB_3_LONG:
			return test_job_3_long();
		case _MAX_TEST_JOB_TYPE:
			elog(ERROR, "unrecognized test job type: %s", NameStr(job->fd.job_type));
	}
	return false;
}

Datum
ts_bgw_job_execute_test(PG_FUNCTION_ARGS)
{
	timer_set(&mock_timer);
	bgw_job_set_unknown_job_type_hook(test_job_dispatcher);

	return ts_bgw_job_entrypoint(fcinfo);
}
