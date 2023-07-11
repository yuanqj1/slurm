/*****************************************************************************\
 *  jobs.c - Slurm REST API jobs http operations handlers
 *****************************************************************************
 *  Copyright (C) 2019-2020 SchedMD LLC.
 *  Written by Nathan Rini <nate@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "src/common/env.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/parse_time.h"
#include "src/common/read_config.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/interfaces/serializer.h"

#include "src/slurmrestd/operations.h"

#include "api.h"
#include "structs.h"

static int _op_handler_jobs(openapi_ctxt_t *ctxt)
{
	openapi_job_info_query_t query = {0};
	job_info_msg_t *job_info_ptr = NULL;
	int rc;

	if (ctxt->method != HTTP_REQUEST_GET) {
		return resp_error(ctxt, (rc = ESLURM_REST_INVALID_QUERY),
				  __func__,
				  "Unsupported HTTP method requested: %s",
				  get_http_method_string(ctxt->method));
	}

	if (DATA_PARSE(ctxt->parser, OPENAPI_JOB_INFO_QUERY, query, ctxt->query,
		       ctxt->parent_path)) {
		return resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
				  "Rejecting request. Failure parsing query.");
	}

	if (!query.show_flags)
		query.show_flags = SHOW_ALL | SHOW_DETAIL;

	rc = slurm_load_jobs(query.update_time, &job_info_ptr,
			     query.show_flags);

	if (rc == SLURM_NO_CHANGE_IN_DATA) {
		char ts[32] = {0};
		slurm_make_time_str(&query.update_time, ts, sizeof(ts));
		rc = SLURM_SUCCESS;
		resp_warn(ctxt, __func__,
			  "No job changes since update_time[%ld]=%s",
			  query.update_time, ts);
	} else if (rc) {
		resp_error(ctxt, rc, "slurm_load_jobs()",
			   "Unable to query jobs");
	} else {
		DUMP_OPENAPI_RESP_SINGLE(OPENAPI_JOB_INFO_RESP, job_info_ptr,
					 ctxt);
	}

	slurm_free_job_info_msg(job_info_ptr);
	return rc;
}

static void _handle_job_get(ctxt_t *ctxt, slurm_selected_step_t *job_id)
{
	openapi_job_info_query_t query = {0};
	int rc = SLURM_SUCCESS;
	job_info_msg_t *job_info_ptr = NULL;
	uint32_t id;

	if (DATA_PARSE(ctxt->parser, OPENAPI_JOB_INFO_QUERY, query, ctxt->query,
		       ctxt->parent_path)) {
		resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			   "Rejecting request. Failure parsing query.");
		return;
	}

	if (job_id->het_job_offset != NO_VAL)
		id = job_id->step_id.job_id + job_id->het_job_offset;
	else
		id = job_id->step_id.job_id;

	if (job_id->array_task_id != NO_VAL)
		resp_warn(ctxt, __func__, "Job array Ids are not currently supported for job searches. Showing all jobs in array instead.");
	if (job_id->step_id.step_id != NO_VAL)
		resp_warn(ctxt, __func__,
			  "Job steps are not supported for job searches. Showing whole job instead.");

	if (!query.show_flags)
		query.show_flags = SHOW_ALL | SHOW_DETAIL;

	if ((rc = slurm_load_job(&job_info_ptr, id, query.show_flags))) {
		char *id = NULL;

		fmt_job_id_string(job_id, &id);
		resp_error(ctxt, rc, __func__, "Unable to query JobId=%s",
			   id);

		xfree(id);
	} else {
		DUMP_OPENAPI_RESP_SINGLE(OPENAPI_JOB_INFO_RESP, job_info_ptr,
					 ctxt);
	}

	slurm_free_job_info_msg(job_info_ptr);
}

static void _handle_job_delete(ctxt_t *ctxt, slurm_selected_step_t *job_id)
{
	openapi_job_info_delete_query_t query = {0};

	if (DATA_PARSE(ctxt->parser, OPENAPI_JOB_INFO_DELETE_QUERY, query,
		       ctxt->query, ctxt->parent_path))
		return;

	if (!query.signal)
		query.signal = SIGKILL;

	if (!query.flags)
		query.flags = KILL_FULL_JOB;

	if (slurm_kill_job(job_id->step_id.job_id, query.signal, query.flags)) {
		/* Already signaled jobs are considered a success here */
		if (errno == ESLURM_ALREADY_DONE) {
			resp_warn(ctxt, __func__,
				  "Job was already sent signal %s",
				  strsignal(query.signal));
			return;
		}

		resp_error(ctxt, errno, "slurm_kill_job2()",
			   "unable to send signal %s to JobId=%u",
			   strsignal(query.signal), job_id->step_id.job_id);
	}
}

static void _job_post_update(ctxt_t *ctxt, job_desc_msg_t *job, char *script,
			     slurm_selected_step_t *job_id)
{
	job_array_resp_msg_t *resp = NULL;

	if (job_id->step_id.job_id != NO_VAL)
		job->job_id = job_id->step_id.job_id;

	if (job_id->het_job_offset != NO_VAL)
		job->het_job_offset = job_id->het_job_offset;

	if (slurm_update_job2(job, &resp)) {
		resp_error(ctxt, errno, "slurm_update_job2()",
			   "Job update requested failed");
		goto cleanup;
	} else if (resp) {
		job_post_response_t r = {
			.results = resp,
			.job_id = resp->job_array_id[0],
			.step_id = NULL, /* not provided by RPC */
			.job_submit_user_msg = NULL, /* not provided by RPC */
		};
		DATA_DUMP(ctxt->parser, OPENAPI_JOB_POST_RESPONSE, r,
			  ctxt->resp);
	}

cleanup:
	slurm_free_job_array_resp(resp);
}

static void _job_submit_rc(ctxt_t *ctxt, submit_response_msg_t *resp,
			   const char *src)
{
	int rc = resp->error_code;

	if (!rc)
		return;

	if (rc == ESLURM_JOB_HELD) {
		/* Job submitted with held state is not an error */
		resp_warn(ctxt, "slurm_submit_batch_job()",
			"%s", slurm_strerror(rc));
		return;
	}

	resp_error(ctxt, rc, src, NULL);
}

static void _job_post_submit(ctxt_t *ctxt, job_desc_msg_t *job, char *script)
{
	submit_response_msg_t *resp = NULL;

	if (script) {
		xfree(job->script);
		job->script = xstrdup(script);
	}

	if (slurm_submit_batch_job(job, &resp)) {
		resp_error(ctxt, errno, "slurm_submit_batch_job()",
			   "Batch job submission failed");
		goto cleanup;
	}

	debug3("%s:[%s] job submitted -> job_id:%d step_id:%d rc:%d message:%s",
	       __func__, ctxt->id, resp->job_id, resp->step_id,
	       resp->error_code, resp->job_submit_user_msg);

	if (resp) {
		job_submit_response_t r = {
			.result = resp,
			.job_id = resp->job_id,
			.step_id = resp->step_id,
			.job_submit_user_msg = resp->job_submit_user_msg,
		};
		DATA_DUMP(ctxt->parser, OPENAPI_JOB_SUBMIT_RESPONSE, r,
			  ctxt->resp);
	}

	_job_submit_rc(ctxt, resp, "slurm_submit_batch_job()");

cleanup:
	slurm_free_submit_response_response_msg(resp);
}

static void _job_post_het_submit(ctxt_t *ctxt, list_t *jobs, char *script)
{
	submit_response_msg_t *resp = NULL;

	if (!jobs || !list_count(jobs)) {
		resp_error(ctxt, errno, __func__,
			   "Refusing HET job submission without any components");
		goto cleanup;
	}

	if (list_count(jobs) > MAX_HET_JOB_COMPONENTS) {
		resp_error(ctxt, errno, __func__,
			   "Refusing HET job submission too many components: %d > %u",
			   list_count(jobs), MAX_HET_JOB_COMPONENTS);
		goto cleanup;
	}

	if (script) {
		job_desc_msg_t *j = list_peek(jobs);

		if (!j->script)
			j->script = xstrdup(script);
	}

	if (slurm_submit_batch_het_job(jobs, &resp)) {
		resp_error(ctxt, errno, "slurm_submit_batch_het_job()",
			   "HET job submission failed");
		goto cleanup;
	}

	debug3("%s:[%s] HET job submitted -> job_id:%d step_id:%d rc:%d message:%s",
	       __func__, ctxt->id, resp->job_id, resp->step_id,
	       resp->error_code, resp->job_submit_user_msg);

	if (resp) {
		job_submit_response_t r = {
			.result = resp,
			.job_id = resp->job_id,
			.step_id = resp->step_id,
			.job_submit_user_msg = resp->job_submit_user_msg,
		};
		DATA_DUMP(ctxt->parser, OPENAPI_JOB_SUBMIT_RESPONSE, r,
			  ctxt->resp);
	}

	_job_submit_rc(ctxt, resp, "slurm_submit_batch_het_job()");

cleanup:
	slurm_free_submit_response_response_msg(resp);
}

static void _job_post(ctxt_t *ctxt, slurm_selected_step_t *job_id)
{
	job_submit_request_t req = {0};

	if ((slurm_conf.debug_flags & DEBUG_FLAG_NET_RAW) && ctxt->query) {
		char *buffer = NULL;

		serialize_g_data_to_string(&buffer, NULL, ctxt->query,
					   MIME_TYPE_JSON, SER_FLAGS_COMPACT);

		log_flag(NET_RAW, "%s:[%s] job POST: %s",
		       __func__, ctxt->id, buffer);

		xfree(buffer);
	}

	if (!ctxt->query) {
		resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			   "unexpected empty query for job");
		return;
	}

	if (DATA_PARSE(ctxt->parser, JOB_SUBMIT_REQ, req, ctxt->query,
		       ctxt->parent_path))
		return;

	if (!req.script || !req.script[0]) {
		if (!job_id || (job_id->step_id.job_id == NO_VAL))
			resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
				   "Populated \"script\" field is required for job submission");
		else
			resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
				   "Populated \"script\" field is required for JobId=%u update",
				   job_id->step_id.job_id);
		return;
	}
	if (req.job && req.jobs) {
		resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			   "Specify only one \"job\" or \"jobs\" fields but never both");
		return;
	}
	if (!req.job && !req.jobs) {
		resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			   "Specifing either \"job\" or \"jobs\" fields are required to job %s",
			   (!job_id || (job_id->step_id.job_id == NO_VAL) ?
			    " update" : "submission"));
		return;
	}
	if (job_id && req.jobs) {
		resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			   "Specify only \"job\" field for updating an existing job");
		return;
	}

	if (req.job) {
		if (job_id)
			_job_post_update(ctxt, req.job, req.script, job_id);
		else
			_job_post_submit(ctxt, req.job, req.script);
	} else {
		_job_post_het_submit(ctxt, req.jobs, req.script);
	}

	slurm_free_job_desc_msg(req.job);
	FREE_NULL_LIST(req.jobs);
	xfree(req.script);
}

static int _op_handler_job(openapi_ctxt_t *ctxt)
{
	openapi_job_info_param_t params = {{ 0 }};
	slurm_selected_step_t *job_id;

	if (DATA_PARSE(ctxt->parser, OPENAPI_JOB_INFO_PARAM, params,
		       ctxt->parameters, ctxt->parent_path)) {
		return resp_error(
			ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			"Rejecting request. Failure parsing parameters");
	}

	job_id = &params.job_id;

	if ((job_id->step_id.job_id == NO_VAL) ||
	    (job_id->step_id.job_id <= 0) ||
	    (job_id->step_id.job_id >= MAX_JOB_ID)) {
		return resp_error(ctxt, ESLURM_INVALID_JOB_ID, __func__,
				  "Invalid JobID=%u rejected",
				  job_id->step_id.job_id);
	}

	if (ctxt->method == HTTP_REQUEST_GET) {
		_handle_job_get(ctxt, job_id);
	} else if (ctxt->method == HTTP_REQUEST_DELETE) {
		_handle_job_delete(ctxt, job_id);
	} else if (ctxt->method == HTTP_REQUEST_POST) {
		_job_post(ctxt, job_id);
	} else {
		return resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
				  "Unsupported HTTP method requested: %s",
				  get_http_method_string(ctxt->method));
	}

	return SLURM_SUCCESS;
}

static int _op_handler_submit_job(openapi_ctxt_t *ctxt)
{
	if (ctxt->method == HTTP_REQUEST_POST) {
		_job_post(ctxt, NULL);
	} else {
		resp_error(ctxt, ESLURM_REST_INVALID_QUERY, __func__,
			   "Unsupported HTTP method requested: %s",
			   get_http_method_string(ctxt->method));
	}

	return ctxt->rc;
}

extern void init_op_jobs(void)
{
	bind_handler("/slurm/{data_parser}/job/submit", _op_handler_submit_job);
	bind_handler("/slurm/{data_parser}/jobs/", _op_handler_jobs);
	bind_handler("/slurm/{data_parser}/job/{job_id}", _op_handler_job);
}

extern void destroy_op_jobs(void)
{
	unbind_operation_ctxt_handler(_op_handler_submit_job);
	unbind_operation_ctxt_handler(_op_handler_job);
	unbind_operation_ctxt_handler(_op_handler_jobs);
}