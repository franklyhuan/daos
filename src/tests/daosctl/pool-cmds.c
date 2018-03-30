/**
 * (C) Copyright 2018 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */

/* generic */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mpi.h>
#include <stdint.h>
#include <inttypes.h>
#include <argp.h>

/* daos specific */
#include <daos.h>
#include <daos_api.h>
#include <daos_mgmt.h>
#include <daos/common.h>
#include <gurt/common.h>

/**
 * This file contains the pool related user commands.
 *
 * For each command there are 3 items of interest: a structure that
 * contains the arguments for the command; a callback function that
 * takes the arguments from argp and puts them in the structure; a
 * function that sends the arguments to the DAOS API and handles the
 * reply.  All commands share the same structure and callback function
 * at present.
 */

struct pool_cmd_options {
	char          *server_group;
	char          *uuid;
	char          *server_list;
	char          *target_list;
	unsigned int  force;
	unsigned int  mode;
	unsigned int  uid;
	unsigned int  gid;
	uint64_t      size;
	uint32_t      replica_count;
	unsigned int  verbose;
};

static int
parse_rank_list(char *str_rank_list, d_rank_list_t *num_rank_list)
{
	/* note the presumption that no more than 1000, it just throws
	 * them away if there are, that should be be plenty for
	 * any currently imaginable situation.
	 */
	const uint32_t MAX = 1000;
	uint32_t rank_list[MAX];
	int i = 0;
	char *token;

	token = strtok(str_rank_list, ",");
	while (token != NULL && i < MAX) {
		rank_list[i++] = atoi(token);
		token = strtok(NULL, ",");
	}
	if (i >= MAX)
		printf("rank list exceeded maximum, threw some away");
	num_rank_list->rl_nr = i;
	num_rank_list->rl_ranks = (d_rank_t *)calloc(
		num_rank_list->rl_nr, sizeof(d_rank_t));
	if (num_rank_list->rl_ranks == NULL) {
		printf("failed allocating pool service rank list");
		return -1;
	}
	for (i = 0; i < num_rank_list->rl_nr; i++)
		num_rank_list->rl_ranks[i] = rank_list[i];
	return 0;
}

static int
parse_size(uint64_t *size, char *arg)
{
	char *unit;
	*size = strtoul(arg, &unit, 0);

	switch (*unit) {
	case '\0':
		break;
	case 'k':
	case 'K':
		*size <<= 10;
		break;
	case 'm':
	case 'M':
		*size <<= 20;
		break;
	case 'g':
	case 'G':
		*size <<= 30;
		break;
	}
	return 0;
}

/**
 * Callback function for create poolthat works with argp to put
 * all the arguments into a structure.
 */
static int
parse_pool_args_cb(int key, char *arg,
		   struct argp_state *state)
{
	struct pool_cmd_options *options = state->input;

	switch (key) {

	case 'f':
		options->force = 1;
		break;
	case 'g':
		options->gid = atoi(arg);
		break;
	case 'i':
		options->uuid = arg;
		break;
	case 'l':
		options->server_list = arg;
		break;
	case 'm':
		options->mode = atoi(arg);
		break;
	case 'r':
		options->replica_count = atoi(arg);
		break;
	case 's':
		options->server_group = arg;
		break;
	case 't':
		options->target_list = arg;
		break;
	case 'u':
		options->uid = atoi(arg);
		break;
	case 'v':
		options->verbose = 1;
		break;
	case 'z':
		parse_size(&(options->size), arg);
		break;
	}
	return 0;
}

/**
 * Process a create pool command.
 */
int
cmd_create_pool(int argc, const char **argv, void *ctx)
{
	int           rc = -ENXIO;
	uuid_t        uuid;
	d_rank_list_t svc = {NULL, 0};

	struct argp_option options[] = {
		{"server-group",   's',    "SERVER-GROUP",     0,
		 "ID of the server group that is to manage the new pool"},
		{"uid",            'u',    "UID",              0,
		 "User ID that is to own the new pool"},
		{"gid",            'g',    "GID",              0,
		 "Group ID that is to own the new pool"},
		{"mode",           'm',    "mode",             0,
		 "Mode defines the operations allowed on the pool"},
		{"size",           'z',    "size",             0,
		 "Size of the pool in bytes or with k/m/g appended (e.g. 10g)"},
		{"replicas",       'r',   "REPLICAS",           0,
		 "number of service replicas"},
		{"verbose",          'v',   0,                0,
		 "Verbose triggers additional results text to be output."},
		{0}
	};
	struct argp argp = {options, parse_pool_args_cb};
	struct pool_cmd_options cp_options = {"daos_server", NULL, "0", "0",
				      0, 0700, 0, 0, 1024*1024*1024, 1, 0};

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments conform
	 * to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &cp_options);

	svc.rl_nr = cp_options.replica_count;
	D_ALLOC_ARRAY(svc.rl_ranks, svc.rl_nr);

	if (svc.rl_ranks == NULL) {
		printf("failed allocating rank list");
		return -1;
	}
	*(svc.rl_ranks) = 0;

	rc = daos_pool_create(cp_options.mode, cp_options.uid,
			      cp_options.gid, cp_options.server_group,
				   NULL, "rubbish", cp_options.size, &svc,
				   uuid, NULL);

	if (rc) {
		printf("Pool create fail, result: %s\n", d_errstr(rc));
	} else {
		char uuid_str[100];

		uuid_unparse(uuid, uuid_str);
		printf("%s\n", uuid_str);

		if (cp_options.verbose) {
			int i;

			printf("Server ranks: ");
			for (i = 0; i < svc.rl_nr; i++)
				printf(" %u\n", svc.rl_ranks[i]);
		}
	}

	if (svc.rl_ranks != NULL)
		free(svc.rl_ranks);
	return rc;
}

/**
 * Function to process a destroy pool command.
 */
int
cmd_destroy_pool(int argc, const char **argv, void *ctx)
{
	uuid_t uuid;
	int    rc;

	struct argp_option options[] = {
		{"server-group",   's',   "SERVER-GROUP",   0,
		 "ID of the server group that manages the pool"},
		{"uuid",           'i',   "UUID",           0,
		 "ID of the pool that is to be destroyed"},
		{"force",          'f',   0,                0,
		 "Force pool destruction regardless of current state."},
		{0}
	};
	struct argp argp = {options, parse_pool_args_cb};

	struct pool_cmd_options dp_options = {"daos_server", NULL, "0", "0",
				      0, 0700, 0, 0, 1024*1024*1024, 1, 0};

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments conform
	 * to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &dp_options);

	printf("destroy_pool uuid:%s server:%s force:%i\n", dp_options.uuid,
	       dp_options.server_group, dp_options.force);

	rc = uuid_parse(dp_options.uuid, uuid);

	rc = daos_pool_destroy(uuid, dp_options.server_group,
			       (int)dp_options.force, NULL);

	if (rc)
		printf("<<<daosctl>>> Pool destroy result: %d\n", rc);
	else
		printf("<<<daosctl>>> Pool destroyed.\n");

	fflush(stdout);

	return rc;
}

/**
 * Function to process an exclude target operation.
 */
int
cmd_exclude_target(int argc, const char **argv, void *ctx)
{
	uuid_t uuid;
	int    rc;
	d_rank_list_t pool_service_list = {NULL, 0};
	d_rank_list_t pool_target_list = {NULL, 0};

	struct argp_option options[] = {
		{"server-group",   's',   "SERVER-GROUP",     0,
		 "ID of the server group that manages the pool"},
		{"uuid",           'i',   "UUID",             0,
		 "ID of the pool that is to be destroyed"},
		{"servers",        'l',   "server rank-list", 0,
		 "pool service ranks, comma separated, no spaces e.g. -l 1,2"},
		{"targets",        't',   "target rank-list", 0,
		 "pool target list, comma separated, no spaces e.g. -l 1,2"},
		{0}
	};
	struct argp argp = {options, parse_pool_args_cb};
	struct pool_cmd_options et_options = {"daos_server", NULL, "0", "0", 0,
					      0, 0, 0, 0, 0};

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments conform
	 * to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &et_options);

	/* turn the uuid string, into a uuid byte array */
	rc = uuid_parse(et_options.uuid, uuid);

	/* turn the list of pool service nodes into a rank list */
	rc = parse_rank_list(et_options.server_list,
			     &pool_service_list);
	if (rc < 0)
		/* TODO do a better job with failure return */
		return rc;

	/* put the ascii list of target ranks into the right format */
	rc = parse_rank_list(et_options.target_list,
			     &pool_target_list);
	if (rc < 0)
		/* TODO do a better job with failure return */
		return rc;

	rc = daos_pool_exclude(uuid, et_options.server_group,
			       &pool_service_list,
			       &pool_target_list, NULL);

	if (rc)
		printf("Target exclude failed result: %d\n", rc);
	else
		printf("Target excluded.\n");
	fflush(stdout);

	return rc;
}


/**
 * Function to process an evict pool command which kicks out clients
 * that are attached to a pool.
 */
int
cmd_evict_pool(int argc, const char **argv, void *ctx)
{
	uuid_t uuid;
	int rc;

	struct pool_cmd_options ep_options = {"daos_server", NULL, "0",
					       "0", 0, 0, 0, 0, 0, 0, 0};
	d_rank_list_t svc;
	uint32_t rl_ranks = 1;
	struct argp_option options[] = {
		{"server-group",   's',   "SERVER-GROUP",   0,
		 "ID of the server group that manages the pool"},
		{"uuid",           'i',   "UUID",           0,
		 "ID of the pool to evict"},
		{0}
	};
	struct argp argp = {options, parse_pool_args_cb};

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments
	 * conform to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &ep_options);

	/* TODO make this a parameter */
	svc.rl_nr = 1;
	svc.rl_ranks = &rl_ranks;

	rc = uuid_parse(ep_options.uuid, uuid);

	rc = daos_pool_evict(uuid, ep_options.server_group, &svc, NULL);

	if (rc)
		printf("Client pool eviction failed with: %d\n",
		       rc);
	else
		printf("Clients evicted from pool successfuly.\n");

	fflush(stdout);

	return rc;
}

/**
 * Function to get current pool status.
 */
int
cmd_query_pool_status(int argc, const char **argv, void *ctx)
{
	uuid_t uuid;
	int rc;
	unsigned int flag = DAOS_PC_RO;
	daos_pool_info_t  info;
	daos_handle_t poh;
	struct pool_cmd_options qp_options = {"daos_server", NULL, NULL, NULL,
					      0, 0, 0, 0, 0, 0, 0};
	d_rank_list_t pool_service_list = {NULL, 0};

	struct argp_option options[] = {
		{"server-group",   's',   "SERVER-GROUP",   0,
		 "ID of the server group that manages the pool"},
		{"uuid",           'i',   "UUID",           0,
		 "ID of the pool to query"},
		{"uid",            'u',    "UID",              0,
		 "User ID that owns the new pool"},
		{"gid",            'g',    "GID",              0,
		 "Group ID that owns the new pool"},
		{"server",         'l',   "server rank list",    0,
		 "mpi rank of the pool service leader"},
		{0}
	};
	struct argp argp = {options, parse_pool_args_cb};

	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments
	 * conform to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &qp_options);

	rc = uuid_parse(qp_options.uuid, uuid);

	rc = parse_rank_list(qp_options.server_list,
			     &pool_service_list);

	printf("server group is %s\n", qp_options.server_group);
	printf("rank %u\n", pool_service_list.rl_ranks[0]);

	rc = daos_pool_connect(uuid, qp_options.server_group,
			       &pool_service_list,
			       flag, &poh, &info, NULL);

	if (rc) {
		printf("<<<daosctl>>> Pool connect fail, result: %d\n",
		       rc);
		return 1;
	}
	printf("target count: %i\n", info.pi_ntargets);
	printf("disabled targets: %i\n", info.pi_ndisabled);
	printf("pool map version: %i\n", info.pi_rebuild_st.rs_version);
	printf("rebuild error: %i\n", info.pi_rebuild_st.rs_errno);
	printf("rebuild done: %i\n", info.pi_rebuild_st.rs_done);
	printf("objects rebuilt: %" PRIu64 "\n", info.pi_rebuild_st.rs_obj_nr);
	printf("record rebuilt: %" PRIu64 "\n", info.pi_rebuild_st.rs_rec_nr);

	fflush(stdout);

	return rc;
}

int
cmd_kill_server(int argc, const char **argv, void *ctx)
{
	int rc;
	struct pool_cmd_options ep_options = {"daos_server", NULL, NULL, NULL,
					      0, 0, 0, 0, 0, 0, 0};
	struct argp_option options[] = {
		{"server-group",   's',   "SERVER-GROUP",   0,
		 "ID of the server group that manages the pool"},
		{"server",         'l',   "SERVER-LIST",    0,
		 "mpi rank of the server to kill"},
		{"force",          'f',    0,               0,
		 "Abrupt shutdown, no cleanup."},
		{0}
	};
	struct argp argp = {options, parse_pool_args_cb};

	d_rank_list_t pool_service_list = {NULL, 0};


	/* adjust the arguments to skip over the command */
	argv++;
	argc--;

	/* once the command is removed the remaining arguments
	 * conform to GNU standards and can be parsed with argp
	 */
	argp_parse(&argp, argc, (char **restrict)argv, 0, 0, &ep_options);

	rc = parse_rank_list(ep_options.server_list,
			     &pool_service_list);


	/* There should be a single rank in the list,
	 * relying on that without checking TODO
	 */
	rc = daos_mgmt_svc_rip(ep_options.server_group,
			       pool_service_list.rl_ranks[0],
			       ep_options.force, NULL);

	if (rc)
		printf("Server %s kill failed with: '%s'\n",
		       ep_options.server_list, d_errstr(rc));
	else
		printf("Server %s killed successfuly.\n",
		       ep_options.server_list);

	fflush(stdout);

	return rc;
}
