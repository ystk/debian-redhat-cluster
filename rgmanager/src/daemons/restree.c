#include <libxml/parser.h>
#include <libxml/xmlmemory.h>
#include <libxml/xpath.h>
#include <ccs.h>
#include <rg_locks.h>
#include <stdlib.h>
#include <stdio.h>
#include <resgroup.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <list.h>
#include <restart_counter.h>
#include <reslist.h>
#include <pthread.h>
#include <logging.h>
#include <assert.h>

/* XXX from resrules.c */
int _res_op(resource_node_t **tree, resource_t *first, char *type,
	    void * __attribute__((unused))ret, int op);
static inline int
_res_op_internal(resource_node_t **tree, resource_t *first,
		 char *type, void *__attribute__((unused))ret, int realop,
		 resource_node_t *node);
static inline int _res_op_internal(resource_node_t **tree, resource_t *first,
		 char *type, void *__attribute__((unused))ret, int realop,
		 resource_node_t *node);

/* XXX from reslist.c */
void * act_dup(resource_act_t *acts);
time_t get_time(char *action, int depth, resource_node_t *node);



const char *ocf_errors[] = {
	"success",				// 0
	"generic error",			// 1
	"invalid argument(s)",			// 2
	"function not implemented",		// 3
	"insufficient privileges",		// 4
	"program not installed",		// 5
	"program not configured",		// 6
	"not running",
	NULL
};


/* XXX MEGA HACK */
#ifdef NO_CCS
static int _no_op_mode_ = 0;
void _no_op_mode(int arg);
void
_no_op_mode(int arg)
{
	_no_op_mode_ = arg;
}
#endif


/**
   ocf_strerror
 */
static const char *
ocf_strerror(int ret)
{
	if (ret >= 0 && ret < OCF_RA_MAX)
		return ocf_errors[ret];

	return "unspecified";
}


/**
   Destroys an environment variable array.

   @param env		Environment var to kill
   @see			build_env
 */
static void
kill_env(char **env)
{
	int x;

	for (x = 0; env[x]; x++)
		free(env[x]);
	free(env);
}


/**
   Adds OCF environment variables to a preallocated  environment var array

   @param res		Resource w/ additional variable stuff
   @param args		Preallocated environment variable array
   @see			build_env
 */
static void
add_ocf_stuff(resource_t *res, char **env, int depth, int refcnt, int timeout)
{
	char ver[10];
	char *minor, *val;
	size_t n;

	if (!res->r_rule->rr_version)
		strncpy(ver, OCF_API_VERSION, sizeof(ver)-1);
	else 
		strncpy(ver, res->r_rule->rr_version, sizeof(ver)-1);

	minor = strchr(ver, '.');
	if (minor) {
		*minor = 0;
		minor++;
	} else
		minor = (char *)"0";
		
	/*
	   Store the OCF major version
	 */
	n = strlen(OCF_RA_VERSION_MAJOR_STR) + strlen(ver) + 2;
	val = malloc(n);
	if (!val)
		return;
	snprintf(val, n, "%s=%s", OCF_RA_VERSION_MAJOR_STR, ver);
	*env = val; env++;

	/*
	   Store the OCF minor version
	 */
	n = strlen(OCF_RA_VERSION_MINOR_STR) + strlen(minor) + 2;
	val = malloc(n);
	if (!val)
		return;
	snprintf(val, n, "%s=%s", OCF_RA_VERSION_MINOR_STR, minor);
	*env = val; env++;

	/*
	   Store the OCF root
	 */
	n = strlen(OCF_ROOT_STR) + strlen(OCF_ROOT) + 2;
	val = malloc(n);
	if (!val)
		return;
	snprintf(val, n, "%s=%s", OCF_ROOT_STR, OCF_ROOT);
	*env = val; env++;

	/*
	   Store the OCF Resource Instance (primary attr)
	 */
	n = strlen(OCF_RESOURCE_INSTANCE_STR) +
		strlen(res->r_rule->rr_type) + 1 +
		strlen(res->r_attrs[0].ra_value) + 2;
	val = malloc(n);
	if (!val)
		return;
	snprintf(val, n, "%s=%s:%s", OCF_RESOURCE_INSTANCE_STR,
		 res->r_rule->rr_type,
		 res->r_attrs[0].ra_value);
	*env = val; env++;

	/*
	   Store the OCF Resource Type
	 */
	n = strlen(OCF_RESOURCE_TYPE_STR) +
		strlen(res->r_rule->rr_type) + 2;
	val = malloc(n);
	if (!val)
		return;
	snprintf(val, n, "%s=%s", OCF_RESOURCE_TYPE_STR, 
		 res->r_rule->rr_type);
	*env = val; env++;

	/*
	   Store the OCF Check Level (0 for now)
	 */
	snprintf(ver, sizeof(ver), "%d", depth);
	n = strlen(OCF_CHECK_LEVEL_STR) + strlen(ver) + 2;
	val = malloc(n);
	if (!val)
		return;
	snprintf(val, n, "%s=%s", OCF_CHECK_LEVEL_STR, ver);
	*env = val; env++;

	/*
	   Store the resource local refcnt (0 for now)
	 */
	snprintf(ver, sizeof(ver), "%d", refcnt);
	n = strlen(OCF_REFCNT_STR) + strlen(ver) + 2;
	val = malloc(n);
	if (!val)
		return;
	snprintf(val, n, "%s=%s", OCF_REFCNT_STR, ver);
	*env = val; env++;

	/*
	   Store the resource action timeout
	 */
	snprintf(ver, sizeof(ver), "%d", timeout);
	n = strlen(OCF_TIMEOUT_STR) + strlen(ver) + 2;
	val = malloc(n);
	if (!val)
		return;
	snprintf(val, n, "%s=%s", OCF_TIMEOUT_STR, ver);
	*env = val; env++;
}


/**
   Allocate and fill an environment variable array.

   @param node		Node in resource tree to use for parameters
   @param depth		Depth (status/monitor/etc.)
   @return		Newly allocated environment array or NULL if
   			one could not be formed.
   @see			kill_env res_exec add_ocf_stuff
 */
static char **
build_env(resource_node_t *node, int depth, int refcnt, int timeout)
{
	resource_t *res = node->rn_resource;
	char **env;
	char *val;
	int x, attrs, n;

	for (attrs = 0; res->r_attrs && res->r_attrs[attrs].ra_name; attrs++);
	attrs += 9; /*
		   Leave space for:
		   OCF_RA_VERSION_MAJOR
		   OCF_RA_VERSION_MINOR
		   OCF_ROOT
		   OCF_RESOURCE_INSTANCE
		   OCF_RESOURCE_TYPE
		   OCF_CHECK_LEVEL
		   OCF_RESKEY_RGMANAGER_meta_refcnt
		   OCF_RESKEY_RGMANAGER_meta_timeout
		   (null terminator)
		 */

	env = malloc(sizeof(char *) * attrs);
	if (!env)
		return NULL;

	memset(env, 0, sizeof(char *) * attrs);

	/* Reset */
	attrs = 0;
	for (x = 0; res->r_attrs && res->r_attrs[x].ra_name; x++) {

		val = (char *)attr_value(node, res->r_attrs[x].ra_name);
		if (!val)
			continue;

		/* Strlen of both + '=' + 'OCF_RESKEY' + '\0' terminator' */
		n = strlen(res->r_attrs[x].ra_name) + strlen(val) + 2 +
			strlen(OCF_RES_PREFIX);

		env[attrs] = malloc(n);
		if (!env[attrs]) {
			kill_env(env);
			return NULL;
		}

		/* Prepend so we don't conflict with normal shell vars */
		snprintf(env[attrs], n, "%s%s=%s", OCF_RES_PREFIX,
			 res->r_attrs[x].ra_name, val);

#if 0
		/* Don't uppercase; OCF-spec */
		for (n = 0; env[x][n] != '='; n++)
			env[x][n] &= ~0x20; /* Convert to uppercase */
#endif
		++attrs;
	}

	add_ocf_stuff(res, &env[attrs], depth, refcnt, timeout);

	return env;
}


/**
   Set all signal handlers to default for exec of a script.
   ONLY do this after a fork().
 */
static void
restore_signals(void)
{
	sigset_t set;
	int x;

	for (x = 1; x < _NSIG; x++)
		signal(x, SIG_DFL);

	sigfillset(&set);
	sigprocmask(SIG_UNBLOCK, &set, NULL);
}


/** Find the index for a given operation / depth in a resource node */
static int
res_act_index(resource_node_t *node, const char *op_str, int depth)
{
	int x = 0;
	resource_act_t *act;

	for (x = 0; node->rn_actions[x].ra_name; x++) {
		act = &node->rn_actions[x];
		if (depth != act->ra_depth)
			continue;
		if (strcasecmp(act->ra_name, op_str))
			continue;
		return x;
	}

	return -1;
}


/**
   Execute a resource-specific agent for a resource node in the tree.

   @param node		Resource tree node we're dealing with
   @param op		Operation to perform (stop/start/etc.)
   @param depth		OCF Check level/depth
   @return		Return value of script.
   @see			build_env
 */
int
res_exec(resource_node_t *node, int op, const char *arg, int depth)
{
	int childpid, pid;
	int ret = 0;
	int act_index;
	int inc = node->rn_resource->r_incarnations;
	time_t sleeptime = 0, timeout = 0;
	char **env = NULL;
	resource_t *res = node->rn_resource;
	const char *op_str = agent_op_str(op);
	char fullpath[2048];

	if (!res->r_rule->rr_agent)
		return 0;

	/* Get the action index for later */
	act_index = res_act_index(node, op_str, depth);

	/* This shouldn't happen, but execing an action for which 
	   we have an incorrect depth or no status action does not
	   indicate a problem.  This allows people writing resource
	   agents to write agents which have no status/monitor function
	   at their option, in violation of the OCF RA API specification. */
	if (act_index < 0)
		return 0;

	if (!(node->rn_flags & RF_ENFORCE_TIMEOUTS))
		timeout = node->rn_actions[act_index].ra_timeout;

	/* rgmanager ref counts are designed to track *other* incarnations
	   on the host.  So, if we're started/failed, the RA should not count
	   this incarnation */
	if (inc && (node->rn_state == RES_STARTED ||
		    node->rn_state == RES_FAILED))
		--inc;

#ifdef DEBUG
	env = build_env(node, depth, inc, (int)timeout);
	if (!env)
		return -errno;
#endif

#ifdef NO_CCS
	if (_no_op_mode_) {
		printf("[%s] %s:%s\n", op_str, res->r_rule->rr_type,
		       res->r_attrs->ra_value);
		return 0;
	}
#endif

	childpid = fork();
	if (childpid < 0)
		return -errno;

	if (!childpid) {
		/* Child */ 
#if 0
		printf("Exec of script %s, action %s type %s\n",
			res->r_rule->rr_agent, agent_op_str(op),
			res->r_rule->rr_type);
#endif

#ifndef DEBUG
		env = build_env(node, depth, inc, (int)timeout);
#endif

		if (!env)
			exit(-ENOMEM);

		if (res->r_rule->rr_agent[0] != '/')
			snprintf(fullpath, sizeof(fullpath), "%s/%s",
				 RESOURCE_ROOTDIR, res->r_rule->rr_agent);
		else
			snprintf(fullpath, sizeof(fullpath), "%s",
				 res->r_rule->rr_agent);

		restore_signals();
		setpgrp();

		if (arg)
			execle(fullpath, fullpath, op_str, arg, NULL, env);
		else
			execle(fullpath, fullpath, op_str, NULL, env);
	}

#ifdef DEBUG
	kill_env(env);
#endif

	if (node->rn_flags & RF_ENFORCE_TIMEOUTS)
		sleeptime = node->rn_actions[act_index].ra_timeout;

	if (sleeptime > 0) {

		/* There's a better way to do this, but this is easy and
		   doesn't introduce signal woes */
		while (sleeptime) {
			pid = waitpid(childpid, &ret, WNOHANG);

			if (pid == childpid)
				break;
			sleep(1);
			--sleeptime;
		}

		if (pid != childpid && sleeptime == 0) {

			logt_print(LOG_ERR,
			       "%s on %s:%s timed out after %d seconds\n",
			       op_str, res->r_rule->rr_type,
			       res->r_attrs->ra_value,
			       (int)node->rn_actions[act_index].ra_timeout);
			
			/* This can't be guaranteed to kill even the child
			   process if the child is in disk-wait :( */
			kill(childpid, SIGKILL);
			sleep(1);
			pid = waitpid(childpid, &ret, WNOHANG);
			if (pid == 0) {
				logt_print(LOG_ERR,
				       "Task %s PID %d did not exit "
				       "after SIGKILL\n",
				       op_str, childpid);
			}

			/* Always an error if we time out */
			return 1;
		}
	} else {
		do {
			pid = waitpid(childpid, &ret, 0);
			if ((pid < 0) && (errno == EINTR))
				continue;
		} while (0);
	}

	if (WIFEXITED(ret)) {

		ret = WEXITSTATUS(ret);

#ifndef NO_CCS
		if ((op == RS_STATUS &&
		     node->rn_state == RES_STARTED && ret) ||
		    (op != RS_STATUS && ret)) {
#else
		if (ret) {
#endif
			logt_print(LOG_NOTICE,
			       "%s on %s \"%s\" returned %d (%s)\n",
			       op_str, res->r_rule->rr_type,
			       res->r_attrs->ra_value, ret,
			       ocf_strerror(ret));
		}

		return ret;
	}

	if (!WIFSIGNALED(ret))
		assert(0);

	return -EFAULT;
}


static inline void
assign_restart_policy(resource_t *curres, resource_node_t *parent,
		      resource_node_t *node, int ccsfd, char *base)
{
	char *val;
	int max_restarts = 0;
	time_t restart_expire_time = 0;
	char tok[1024];

	if (!curres || !node)
		return;
	node->rn_restart_counter = NULL;
	if (parent &&
	    !(node->rn_flags & RF_INDEPENDENT))
		return;

	if (node->rn_flags & RF_INDEPENDENT) {
		/* per-resource-node failures / expire times */
		snprintf(tok, sizeof(tok), "%s/@__max_restarts", base);
#ifndef NO_CCS
		if (ccs_get(ccsfd, tok, &val) == 0) {
#else
		if (conf_get(tok, &val) == 0) {
#endif
			max_restarts = atoi(val);
			if (max_restarts <= 0)
				max_restarts = 0;
			free(val);
		}
	
		snprintf(tok, sizeof(tok), "%s/@__restart_expire_time", base);
#ifndef NO_CCS
		if (ccs_get(ccsfd, tok, &val) == 0) {
#else
		if (conf_get(tok, &val) == 0) {
#endif
			restart_expire_time = (time_t)expand_time(val);
			if ((int64_t)restart_expire_time <= 0)
				restart_expire_time = 0;
			free(val);
		}

		if (restart_expire_time == 0 || max_restarts == 0)
			return;
		goto out_assign;
	}

	val = (char *)res_attr_value(curres, "max_restarts");
	if (!val)
		return;
	max_restarts = atoi(val);
	if (max_restarts <= 0)
		return;
	val = (char *)res_attr_value(curres, "restart_expire_time");
	if (val) {
		restart_expire_time = (time_t)expand_time(val);
		if ((int64_t)restart_expire_time < 0)
			return;
	}

out_assign:
	node->rn_restart_counter = restart_init(restart_expire_time,
						max_restarts);
}


static inline int
do_load_resource(int ccsfd, char *base,
	         resource_rule_t *rule,
	         resource_node_t **tree,
		 resource_t **reslist,
		 resource_node_t *parent,
		 resource_node_t **newnode)
{
	char tok[512];
	char *ref;
	resource_node_t *node;
	resource_t *curres;
	time_t failure_expire = 0;
	int max_failures = 0;

	snprintf(tok, sizeof(tok), "%s/@ref", base);

#ifndef NO_CCS
	if (ccs_get(ccsfd, tok, &ref) != 0) {
#else
	if (conf_get(tok, &ref) != 0) {
#endif
		/* There wasn't an existing resource. See if there
		   is one defined inline */
		curres = load_resource(ccsfd, rule, base);
		if (!curres) {
			/* No ref and no new one inline == 
			   no more of the selected type */
			return 1;
		}

	       	if (store_resource(reslist, curres) != 0) {
	 		printf("Error storing %s resource\n",
	 		       curres->r_rule->rr_type);
	 		destroy_resource(curres);
			return -1;
	 	}

		curres->r_flags = RF_INLINE;

	} else {

		curres = find_resource_by_ref(reslist, rule->rr_type,
						      ref);
		if (!curres) {
			printf("Error: Reference to nonexistent "
			       "resource %s (type %s)\n", ref,
			       rule->rr_type);
			free(ref);
			return -1;
		}

		if (curres->r_flags & RF_INLINE) {
			printf("Error: Reference to inlined "
			       "resource %s (type %s) is illegal\n",
			       ref, rule->rr_type);
			free(ref);
			return -1;
		}
		free(ref);
	}

	/* Load it if its max refs hasn't been exceeded */
	if (rule->rr_maxrefs && (curres->r_refs >= rule->rr_maxrefs)){
		printf("Warning: Max references exceeded for resource"
		       " %s (type %s)\n", curres->r_attrs[0].ra_name,
		       rule->rr_type);
		return -1;
	}

	node = malloc(sizeof(*node));
	if (!node)
		return -1;

	memset(node, 0, sizeof(*node));

	//printf("New resource tree node: %s:%s \n", curres->r_rule->rr_type,curres->r_attrs->ra_value);

	node->rn_child = NULL;
	node->rn_parent = parent;
	node->rn_resource = curres;
	node->rn_state = RES_STOPPED;
	node->rn_flags = 0;
	node->rn_actions = (resource_act_t *)act_dup(curres->r_actions);


	if (parent) {
		/* Independent subtree / non-critical for top-level is
		 * not useful and can interfere with restart thresholds for
		 * non critical resources */
		snprintf(tok, sizeof(tok), "%s/@__independent_subtree", base);
#ifndef NO_CCS
		if (ccs_get(ccsfd, tok, &ref) == 0) {
#else
		if (conf_get(tok, &ref) == 0) {
#endif
			if (atoi(ref) == 1 || strcasecmp(ref, "yes") == 0)
				node->rn_flags |= RF_INDEPENDENT;
			if (atoi(ref) == 2 || strcasecmp(ref, "non-critical") == 0) {
				curres->r_flags |= RF_NON_CRITICAL;
			}
			free(ref);
		}
	}

	snprintf(tok, sizeof(tok), "%s/@__enforce_timeouts", base);
#ifndef NO_CCS
	if (ccs_get(ccsfd, tok, &ref) == 0) {
#else
	if (conf_get(tok, &ref) == 0) {
#endif
		if (atoi(ref) > 0 || strcasecmp(ref, "yes") == 0)
			node->rn_flags |= RF_ENFORCE_TIMEOUTS;
		free(ref);
	}

	/* per-resource-node failures / expire times */
	snprintf(tok, sizeof(tok), "%s/@__max_failures", base);
#ifndef NO_CCS
	if (ccs_get(ccsfd, tok, &ref) == 0) {
#else
	if (conf_get(tok, &ref) == 0) {
#endif
		max_failures = atoi(ref);
		if (max_failures < 0)
			max_failures = 0;
		free(ref);
	}

	snprintf(tok, sizeof(tok), "%s/@__failure_expire_time", base);
#ifndef NO_CCS
	if (ccs_get(ccsfd, tok, &ref) == 0) {
#else
	if (conf_get(tok, &ref) == 0) {
#endif
		failure_expire = (time_t)expand_time(ref);
		if ((int64_t)failure_expire < 0)
			failure_expire = 0;
		free(ref);
	}

	if (max_failures && failure_expire) {
		node->rn_failure_counter = restart_init(failure_expire,
							max_failures);
	}

	curres->r_refs++;

	if (curres->r_refs > 1 &&
	    (curres->r_flags & RF_NON_CRITICAL)) {
		res_build_name(tok, sizeof(tok), curres);
		printf("Non-critical flag for %s is being cleared due to multiple references.\n", tok);
		curres->r_flags &= ~RF_NON_CRITICAL;
	}

	if (curres->r_flags & RF_NON_CRITICAL) {
		/* Independent subtree is implied if a
		 * resource is non-critical
		 */
		node->rn_flags |= RF_NON_CRITICAL | RF_INDEPENDENT;

	}

	assign_restart_policy(curres, parent, node, ccsfd, base);

	*newnode = node;

	list_insert(tree, node);

	return 0;
}


/**
   Build the resource tree.  If a new resource is defined inline, add it to
   the resource list.  All rules, however, must have already been read in.

   @param ccsfd		File descriptor connected to CCS
   @param tree		Tree to modify/insert on to
   @param parent	Parent node, if one exists.
   @param rule		Rule surrounding the new node
   @param rulelist	List of all rules allowed in the tree.
   @param reslist	List of all currently defined resources
   @param base		Base CCS path.
   @see			destroy_resource_tree
 */
#define RFL_FOUND 0x1
#define RFL_FORBID 0x2
static int
build_tree(int ccsfd, resource_node_t **tree,
	   resource_node_t *parent,
	   resource_rule_t *rule,
	   resource_rule_t **rulelist,
	   resource_t **reslist, char *base)
{
	char tok[512];
	resource_rule_t *childrule;
	resource_node_t *node = NULL;
	char *ref;
	char *tmp;
	int ccount = 0, x = 0, y = 0, flags = 0;

	//printf("DESCEND: %s / %s\n", rule?rule->rr_type:"(none)", base);

	/* Pass 1: typed / defined children */
	for (y = 0; rule && rule->rr_childtypes &&
     	     rule->rr_childtypes[y].rc_name; y++) {
		
	
		flags = 0;
		list_for(rulelist, childrule, x) {
			if (strcmp(rule->rr_childtypes[y].rc_name,
				   childrule->rr_type))
				continue;

			flags |= RFL_FOUND;

			if (rule->rr_childtypes[y].rc_forbid)
				flags |= RFL_FORBID;

			break;
		}

		if (flags & RFL_FORBID)
			/* Allow all *but* forbidden */
			continue;

		if (!(flags & RFL_FOUND))
			/* Not found?  Wait for pass 2 */
			continue;

		//printf("looking for %s %s @ %s\n",
			//rule->rr_childtypes[y].rc_name,
			//childrule->rr_type, base);
		for (x = 1; ; x++) {

			/* Search for base/type[x]/@ref - reference an existing
			   	resource */
			snprintf(tok, sizeof(tok), "%s/%s[%d]", base,
				 childrule->rr_type, x);

			flags = 1;
			switch(do_load_resource(ccsfd, tok, childrule, tree,
						reslist, parent, &node)) {
			case -1:
				continue;
			case 1:
				/* 1 == no more */
				//printf("No resource found @ %s\n", tok);
				flags = 0;
				break;
			case 0:
				break;
			}
			if (!flags)
				break;

			/* Got a child :: bump count */
			snprintf(tok, sizeof(tok), "%s/%s[%d]", base,
				 childrule->rr_type, x);

			/* Kaboom */
			build_tree(ccsfd, &node->rn_child, node, childrule,
				   rulelist, reslist, tok);

		}
	}


	/* Pass 2: untyped children */
	for (ccount=1; ; ccount++) {
		snprintf(tok, sizeof(tok), "%s/child::*[%d]", base, ccount);

#ifndef NO_CCS
		if (ccs_get(ccsfd, tok, &ref) != 0) {
#else
		if (conf_get(tok, &ref) != 0) {
#endif
			/* End of the line. */
			//printf("End of the line: %s\n", tok);
			break;
		}

		tmp = strchr(ref, '=');
		if (tmp) {
			*tmp = 0;
		} else {
			/* no = sign... bad */
			free(ref);
			continue;
		}

		/* Find the resource rule */
		flags = 0;
		list_for(rulelist, childrule, x) {
			if (!strcasecmp(childrule->rr_type, ref)) {
				/* Ok, matching rule found */
				flags = 1;
				break;
			}
		}
		/* No resource rule matching the child?  Press on... */
		if (!flags) {
			free(ref);
			continue;
		}

		flags = 0;
		/* Don't descend on anything we should have already picked
		   up on in the above loop */
		for (y = 0; rule && rule->rr_childtypes &&
		     rule->rr_childtypes[y].rc_name; y++) {
			/* SKIP defined child types of any type */
			if (strcmp(rule->rr_childtypes[y].rc_name, ref))
				continue;
			if (rule->rr_childtypes[y].rc_flags == 0) {
				/* 2 = defined as a real child */
				flags = 2;
				break;
			}

			flags = 1;
			break;
		}

		free(ref);
		if (flags == 2)
			continue;

		x = 1;
		switch(do_load_resource(ccsfd, tok, childrule, tree,
				        reslist, parent, &node)) {
		case -1:
			continue;
		case 1:
			/* no more found */
			x = 0;
			printf("No resource found @ %s\n", tok);
			break;
		case 0:
			/* another is found */
			break;
		}
		if (!x) /* no more found */
			break;

		/* childrule = rule set of this child at this point */
		/* tok = set above; if we got this far, we're all set */
		/* Kaboom */

		build_tree(ccsfd, &node->rn_child, node, childrule,
			   rulelist, reslist, tok);
	}

	//printf("ASCEND: %s / %s\n", rule?rule->rr_type:"(none)", base);
	return 0;
}


/**
   Set up to call build_tree.  Hides the nastiness from the user.

   @param ccsfd		File descriptor connected to CCS
   @param tree		Tree pointer.  Should start as a pointer to NULL.
   @param rulelist	List of all rules allowed
   @param reslist	List of all currently defined resources
   @return 		0
   @see			build_tree destroy_resource_tree
 */
int
build_resource_tree(int ccsfd, resource_node_t **tree,
		    resource_rule_t **rulelist,
		    resource_t **reslist)
{
	resource_node_t *root = NULL;
	char tok[512];

	snprintf(tok, sizeof(tok), "%s", RESOURCE_TREE_ROOT);

	/* Find and build the list of root nodes */
	build_tree(ccsfd, &root, NULL, NULL/*curr*/, rulelist, reslist, tok);

	if (root)
		*tree = root;

	return 0;
}


/**
   Deconstruct a resource tree.

   @param tree		Tree to obliterate.
   @see			build_resource_tree
 */
void
destroy_resource_tree(resource_node_t **tree)
{
	resource_node_t *node = NULL;

	while ((node = *tree)) {
		if ((*tree)->rn_child)
			destroy_resource_tree(&(*tree)->rn_child);

		list_remove(tree, node);

		if (node->rn_restart_counter) {
			restart_cleanup(node->rn_restart_counter);
		}

		if (node->rn_failure_counter) {
			restart_cleanup(node->rn_failure_counter);
		}

		if(node->rn_actions){
			free(node->rn_actions);
		}
		free(node);
	}
}


static void
_print_resource_tree(FILE *fp, resource_node_t **tree, int level)
{
	resource_node_t *node;
	int x, y;

	list_do(tree, node) {
		for (x = 0; x < level; x++)
			fprintf(fp, "  ");

		fprintf(fp, "%s", node->rn_resource->r_rule->rr_type);
		if (node->rn_flags) {
			fprintf(fp, " [ ");
			if (node->rn_flags & RF_INLINE)
				fprintf(fp, "INLINE ");
			if (node->rn_flags & RF_NEEDSTOP)
				fprintf(fp, "NEEDSTOP ");
			if (node->rn_flags & RF_NEEDSTART)
				fprintf(fp, "NEEDSTART ");
			if (node->rn_flags & RF_COMMON)
				fprintf(fp, "COMMON ");
			if (node->rn_flags & RF_INDEPENDENT)
				fprintf(fp, "INDEPENDENT ");
			if (node->rn_flags & RF_RECONFIG)
				fprintf(fp, "RECONFIG ");
			if (node->rn_flags & RF_INIT)
				fprintf(fp, "INIT ");
			if (node->rn_flags & RF_DESTROY)
				fprintf(fp, "DESTROY ");
			if (node->rn_flags & RF_ENFORCE_TIMEOUTS)
				fprintf(fp, "ENFORCE-TIMEOUTS ");
			if (node->rn_flags & RF_NON_CRITICAL)
				fprintf(fp, "NON-CRITICAL ");
			fprintf(fp, "]");
		}

		fprintf(fp, " (S%d) {\n", node->rn_state);

		for (x = 0; node->rn_resource->r_attrs &&
		     node->rn_resource->r_attrs[x].ra_value; x++) {
			for (y = 0; y < level+1; y++)
				fprintf(fp, "  ");
			fprintf(fp, "%s = \"%s\";\n",
				node->rn_resource->r_attrs[x].ra_name,
 				attr_value(node,
					   node->rn_resource->r_attrs[x].ra_name)
			       );
		}

		_print_resource_tree(fp, &node->rn_child, level + 1);

		for (x = 0; x < level; x++)
			fprintf(fp, "  ");
		fprintf(fp, "}\n");
	} while (!list_done(tree, node));
}


void
print_resource_tree(resource_node_t **tree)
{
	_print_resource_tree(stdout, tree, 0);
}


void
dump_resource_tree(FILE *fp, resource_node_t **tree)
{
	_print_resource_tree(fp, tree, 0);
}


static inline int
_do_child_levels(resource_node_t **tree, resource_t *first, void *ret,
		 int op)
{
	resource_node_t *node = *tree;
	resource_t *res = node->rn_resource;
	resource_rule_t *rule = res->r_rule;
	int l, lev, x, rv = 0;

	for (l = 1; l <= RESOURCE_MAX_LEVELS; l++) {

		for (x = 0; rule->rr_childtypes &&
		     rule->rr_childtypes[x].rc_name; x++) {

			if(op == RS_STOP)
				lev = rule->rr_childtypes[x].rc_stoplevel;
			else
				lev = rule->rr_childtypes[x].rc_startlevel;

			if (!lev || lev != l)
				continue;

#if 0
			printf("%s children of %s type %s (level %d)\n",
			       agent_op_str(op),
			       node->rn_resource->r_rule->rr_type,
			       rule->rr_childtypes[x].rc_name, l);
#endif

			/* Do op on all children at our level */
			rv |= _res_op(&node->rn_child, first,
			     	     rule->rr_childtypes[x].rc_name, 
		     		     ret, op);

			if (rv & SFL_FAILURE && op != RS_STOP)
				return rv;
		}

		if (rv != 0 && op != RS_STOP)
			return rv;
	}

	return rv;
}


static inline int
_xx_child_internal(resource_node_t *node, resource_t *first,
		   resource_node_t *child, void *ret, int op)
{
	int x;
	resource_rule_t *rule = node->rn_resource->r_rule;

	for (x = 0; rule->rr_childtypes &&
     	     rule->rr_childtypes[x].rc_name; x++) {
		if (!strcmp(child->rn_resource->r_rule->rr_type,
			    rule->rr_childtypes[x].rc_name)) {
			if (rule->rr_childtypes[x].rc_startlevel ||
			    rule->rr_childtypes[x].rc_stoplevel) {
				return 0;
			}
		}
	}

	return _res_op_internal(&child, first,
	 		       child->rn_resource->r_rule->rr_type,
			       ret, op, child);
}


static inline int
_do_child_default_level(resource_node_t **tree, resource_t *first,
			void *ret, int op)
{
	resource_node_t *node = *tree, *child;
	int y, rv = 0;

	if (op == RS_START || op == RS_STATUS) {
		list_for(&node->rn_child, child, y) {
			rv |= _xx_child_internal(node, first, child, ret, op);

			if (rv & SFL_FAILURE)
				return rv;
		}
	} else {
		list_for_rev(&node->rn_child, child, y) {
			rv |= _xx_child_internal(node, first, child, ret, op);
		}
	}

	return rv;
}





/**
   Nasty codependent function.  Perform an operation by numerical level
   at some point in the tree.  This allows indirectly-dependent resources
   (such as IP addresses and user scripts) to have ordering without requiring
   a direct dependency.

   @param tree		Resource tree to search/perform operations on
   @param first		Resource we're looking to perform the operation on,
   			if one exists.
   @param ret		Unused, but will be used to store status information
   			such as resources consumed, etc, in the future.
   @param op		Operation to perform if either first is found,
   			or no first is declared (in which case, all nodes
			in the subtree).
   @see			_res_op res_exec
 */
static int
_res_op_by_level(resource_node_t **tree, resource_t *first, void *ret,
		 int op)
{
	resource_node_t *node = *tree;
	resource_t *res = node->rn_resource;
	resource_rule_t *rule = res->r_rule;
	int rv = 0;

	if (!rule->rr_childtypes)
		return _res_op(&node->rn_child, first, NULL, ret, op);

	if (op == RS_START || op == RS_STATUS) {
		rv |= _do_child_levels(tree, first, ret, op);
	       	if (rv & SFL_FAILURE)
			return rv;

		/* Start default level after specified ones */
		rv |= _do_child_default_level(tree, first, ret, op);

	} /* stop */ else {

		rv |= _do_child_default_level(tree, first, ret, op);
		rv |= _do_child_levels(tree, first, ret, op);
	}

	return rv;
}


static void
mark_nodes(resource_node_t *node, int state, int setflags, int clearflags)
{
	int x;
	resource_node_t *child;

	list_for(&node->rn_child, child, x) {
		mark_nodes(child, state, setflags,
			   clearflags);
	}

	if (state >= RES_STOPPED)
		node->rn_state = state;
	node->rn_flags |= (setflags);
	node->rn_flags &= ~(clearflags);
}


/**
   Do a status on a resource node.  This takes into account the last time the
   status operation was run and selects the highest possible resource depth
   to use given the elapsed time.
  */
static int
do_status(resource_node_t *node)
{
	int x = 0, idx = -1;
	int has_recover = 0;
	time_t delta = 0, now = 0;

	if (node->rn_state == RES_DISABLED)
		return 0;

	now = time(NULL);

	for (; node->rn_actions[x].ra_name; x++) {
		if (!has_recover &&
		    !strcmp(node->rn_actions[x].ra_name, "recover")) {
			has_recover = 1;
			continue;
		}

		if (strcmp(node->rn_actions[x].ra_name, "status"))
			continue;

		delta = now - node->rn_actions[x].ra_last;

		/*
		printf("%s:%s %s level %d interval = %d  elapsed = %d\n",
			node->rn_resource->r_rule->rr_type,
			node->rn_resource->r_attrs->ra_value,
			node->rn_actions[x].ra_name, node->rn_actions[x].ra_depth,
			(int)node->rn_actions[x].ra_interval, (int)delta);
		 */

		/* Ok, it's a 'status' action. See if enough time has
		   elapsed for a given type of status action */
		if (delta < node->rn_actions[x].ra_interval ||
		    !node->rn_actions[x].ra_interval)
			continue;

		if (idx == -1 ||
		    node->rn_actions[x].ra_depth > node->rn_actions[idx].ra_depth)
			idx = x;
	}

	/* No check levels ready at the moment. */
	/* Cap status check children if configured to do so */
	if (idx == -1 || rg_inc_children() < 0) {
		if (node->rn_checked)
			return node->rn_last_status;
		return 0;
	}

	x = res_exec(node, RS_STATUS, NULL, node->rn_actions[idx].ra_depth);
	rg_dec_children();

	/* Record status check result *after* the status check has
	 * completed. */
	node->rn_actions[idx].ra_last = time(NULL);

	/* If we have not exceeded our failure count threshold, then fudge
	 * the status check this round */
	if (x && node->rn_failure_counter) {
		if (!restart_threshold_exceeded(node->rn_failure_counter)) {
			x = 0;
			restart_add(node->rn_failure_counter);
		}
	}

	node->rn_last_status = x;
	node->rn_last_depth = node->rn_actions[idx].ra_depth;
	node->rn_checked = 1;

	if (x == 0)
		return 0;

	if (!has_recover)
		return x;

	/* Strange/failed status. Try to recover inline. */
	if ((x = res_exec(node, RS_RECOVER, NULL, 0)) == 0)
		return 0;

	return x;
}


static void
set_time(const char *action, int depth, resource_node_t *node)
{
	time_t now;
	int x = 0;

	time(&now);

	for (; node->rn_actions[x].ra_name; x++) {

		if (strcmp(node->rn_actions[x].ra_name, action) ||
	    	    node->rn_actions[x].ra_depth != depth)
			continue;

		node->rn_actions[x].ra_last = now;
		break;
	}
}


time_t
get_time(char *action, int depth, resource_node_t *node)
{
	int x = 0;

	for (; node->rn_actions[x].ra_name; x++) {

		if (strcmp(node->rn_actions[x].ra_name, action) ||
	    	    node->rn_actions[x].ra_depth != depth)
			continue;

		return node->rn_actions[x].ra_last;
	}

	return (time_t)0;
}


static void
clear_checks(resource_node_t *node)
{
	time_t now;
	int x = 0;

	now = get_time((char *)"start", 0, node);

	for (; node->rn_actions[x].ra_name; x++) {

		if (strcmp(node->rn_actions[x].ra_name, "monitor") &&
		    strcmp(node->rn_actions[x].ra_name, "status"))
			continue;

		node->rn_actions[x].ra_last = now;
	}

	restart_clear(node->rn_failure_counter);

	node->rn_checked = 0;
	node->rn_last_status = 0;
	node->rn_last_depth = 0;
}


/**
   Nasty codependent function.  Perform an operation by type for all siblings
   at some point in the tree.  This allows indirectly-dependent resources
   (such as IP addresses and user scripts) to have ordering without requiring
   a direct dependency.

   @param tree		Resource tree to search/perform operations on
   @param first		Resource we're looking to perform the operation on,
   			if one exists.
   @param type		Type to look for.
   @param ret		Unused, but will be used to store status information
   			such as resources consumed, etc, in the future.
   @param realop	Operation to perform if either first is found,
   			or no first is declared (in which case, all nodes
			in the subtree).
   @see			_res_op_by_level res_exec
 */
static inline int
_res_op_internal(resource_node_t __attribute__ ((unused)) **tree,
		 resource_t *first,
		 char *type, void *__attribute__((unused))ret, int realop,
		 resource_node_t *node)
{
	int rv = 0, me, op, rte = 0;

	/* Restore default operation. */
	op = realop;

	/* If we're starting by type, do that funky thing. */
	if (type && strlen(type) &&
	    strcmp(node->rn_resource->r_rule->rr_type, type))
		return 0;

	/* If the resource is found, all nodes in the subtree must
	   have the operation performed as well. */
	me = !first || (node->rn_resource == first);

	//printf("begin %s: %s %s [0x%x]\n", res_ops[op],
	       //node->rn_resource->r_rule->rr_type,
	       //primary_attr_value(node->rn_resource),
	       //node->rn_flags);

	if (me) {
		/*
		   If we've been marked as a node which
		   needs to be started or stopped, clear
		   that flag and start/stop this resource
		   and all resource babies.

		   Otherwise, don't do anything; look for
		   children with RF_NEEDSTART and
		   RF_NEEDSTOP flags.

		   CONDSTART and CONDSTOP are no-ops if
		   the appropriate flag is not set.
		 */
		if ((op == RS_CONVALESCE) &&
		    node->rn_state == RES_DISABLED) {
			printf("Node %s:%s - Convalesce\n",
			       node->rn_resource->r_rule->rr_type,
			       primary_attr_value(node->rn_resource));
			mark_nodes(node, RES_STOPPED, RF_NEEDSTART, 0);
			op = RS_START;
		}

	       	if ((op == RS_CONDSTART) &&
		    (node->rn_flags & RF_NEEDSTART)) {
			printf("Node %s:%s - CONDSTART\n",
			       node->rn_resource->r_rule->rr_type,
			       primary_attr_value(node->rn_resource));
			op = RS_START;
		}

		if ((op == RS_CONDSTOP) &&
		    (node->rn_flags & RF_NEEDSTOP)) {
			printf("Node %s:%s - CONDSTOP\n",
			       node->rn_resource->r_rule->rr_type,
			       primary_attr_value(node->rn_resource));
			op = RS_STOP;
		}
	}

	/* Start starts before children */
	if (me && (op == RS_START)) {

		if (node->rn_state == RES_DISABLED)
			/* Nothing to do - children are also disabled */
			return 0;

		if ((realop == RS_START || realop == RS_CONVALESCE) &&
		     node->rn_flags & RF_INDEPENDENT)
			restart_clear(node->rn_restart_counter);

		pthread_mutex_lock(&node->rn_resource->r_mutex);

		if (node->rn_flags & RF_RECONFIG &&
		    realop == RS_CONDSTART) {
			rv = res_exec(node, RS_RECONFIG, NULL, 0);
			op = realop; /* reset to CONDSTART */
		} else {
			rv = res_exec(node, op, NULL, 0);
		}
		node->rn_flags &= ~(RF_NEEDSTART | RF_RECONFIG);
		if (rv != 0) {
			node->rn_state = RES_FAILED;
			pthread_mutex_unlock(&node->rn_resource->r_mutex);
			return SFL_FAILURE;
		}

		set_time("start", 0, node);
		clear_checks(node);

		if (node->rn_state != RES_STARTED) {
			++node->rn_resource->r_incarnations;
			node->rn_state = RES_STARTED;
		}
		pthread_mutex_unlock(&node->rn_resource->r_mutex);

	} else if (me && (op == RS_STATUS || op == RS_STATUS_INQUIRY)) {

		/* Special quick-check for status inquiry */
		if (op == RS_STATUS_INQUIRY) {
			if (res_exec(node, RS_STATUS, NULL, 0) != 0)
				return SFL_FAILURE;

			/* XXX: A migratable service (the only place this
			 * check can be used) cannot have child dependencies
			 * anyway, so this is a short-circuit. */
			return 0;
		}

		/* Check status before children*/
		rv = do_status(node);
		if (rv != 0) {
			/*
			   If this node's status has failed, all of its
			   dependent children are failed, whether or not this
			   node is independent or not.
			 */

			/* If we're an independent subtree, return a flag
			   stating that this section is recoverable apart
			   from siblings in the resource tree.  All child
			   resources of this node must be restarted,
			   but siblings of this node are not affected. */
			if (node->rn_flags & RF_INDEPENDENT) {
				
				rte = restart_threshold_exceeded(node->rn_restart_counter);
				if ((node->rn_flags & RF_NON_CRITICAL) && (rte ||
				    !node->rn_restart_counter)) {
					mark_nodes(node, RES_FAILED,
						   RF_NEEDSTOP | RF_QUIESCE, 0);
					restart_clear(node->rn_restart_counter);
					return SFL_RECOVERABLE|SFL_PARTIAL;
				} else if (!rte ||
					   !node->rn_restart_counter) {
					restart_add(node->rn_restart_counter);
					mark_nodes(node, RES_FAILED,
						   RF_NEEDSTART | RF_NEEDSTOP, 0);
					return SFL_RECOVERABLE;
				} else {
					restart_clear(node->rn_restart_counter);
					return SFL_FAILURE;
				}
			}

			return SFL_FAILURE;
		}

		if (node->rn_state != RES_DISABLED) {
			node->rn_state = RES_STARTED;
			node->rn_flags &= ~RF_NEEDSTOP;
		}
	}

       if (node->rn_child) {
                rv |= _res_op_by_level(&node, me?NULL:first, ret, op);

               /* If one or more child resources are failed and at least one
		  of them is not an independent subtree then let's check if
		  if we are an independent subtree.  If so, mark ourself
		  and all our children as failed and return a flag stating
		  that this section is recoverable apart from siblings in
		  the resource tree. */
		if (op == RS_STATUS && (rv & SFL_FAILURE) &&
		    (node->rn_flags & RF_INDEPENDENT)) {

			rte = restart_threshold_exceeded(node->rn_restart_counter);

			rv = SFL_RECOVERABLE;
			if ((node->rn_flags & RF_NON_CRITICAL) && (rte || 
			    !node->rn_restart_counter)) {
				/* if non-critical, just stop */
				mark_nodes(node, RES_FAILED, RF_NEEDSTOP | RF_QUIESCE, 0);

				rv |= SFL_PARTIAL;
			} else if (!rte || !node->rn_restart_counter) {
				restart_add(node->rn_restart_counter);
				mark_nodes(node, RES_FAILED,
					   RF_NEEDSTOP | RF_NEEDSTART, 0);
			} else {
				restart_clear(node->rn_restart_counter);
				rv = SFL_FAILURE;
			}
		}
	}
 			
	/* Stop should occur after children have stopped */
	if (me && (op == RS_STOP)) {

		/* Decrease incarnations so the script knows how many *other*
		   incarnations there are. */
		pthread_mutex_lock(&node->rn_resource->r_mutex);

		node->rn_flags &= ~RF_NEEDSTOP;
		rv |= res_exec(node, op, NULL, 0);

		if (node->rn_flags & (RF_NEEDSTART|RF_QUIESCE) &&
		    node->rn_flags & RF_INDEPENDENT &&
		    node->rn_flags & RF_NON_CRITICAL) {
			/* Non-critical resources = do not fail 
			 * service if the resource fails to stop
			 */
			if (rv & SFL_FAILURE) {
				logt_print(LOG_WARNING, "Failed to stop subtree %s:%s"
					   " during non-critical recovery "
					   "operation\n",
					   node->rn_resource->r_rule->rr_type,
					   primary_attr_value(
						node->rn_resource));
				rv = SFL_PARTIAL;
				node->rn_flags |= RF_QUIESCE;
			}
		}

		if (rv == 0 && (node->rn_state == RES_STARTED ||
				node->rn_state == RES_FAILED)) {
			assert(node->rn_resource->r_incarnations >= 0);
			if (node->rn_resource->r_incarnations > 0)
				--node->rn_resource->r_incarnations;
		} else if (rv != 0 && (node->rn_state != RES_DISABLED &&
			   !(node->rn_flags & RF_QUIESCE))) {
			node->rn_state = RES_FAILED;
			pthread_mutex_unlock(&node->rn_resource->r_mutex);
			return SFL_FAILURE;
		}
		pthread_mutex_unlock(&node->rn_resource->r_mutex);

		if (node->rn_flags & RF_QUIESCE) {
			mark_nodes(node, RES_DISABLED, 0,
				   RF_NEEDSTART|RF_QUIESCE);
		} else {
			node->rn_state = RES_STOPPED;
		}
	}

	//printf("end %s: %s %s\n", res_ops[op],
	       //node->rn_resource->r_rule->rr_type,
	       //primary_attr_value(node->rn_resource));
	
	return rv;
}


/**
   Nasty codependent function.  Perform an operation by type for all siblings
   at some point in the tree.  This allows indirectly-dependent resources
   (such as IP addresses and user scripts) to have ordering without requiring
   a direct dependency.

   @param tree		Resource tree to search/perform operations on
   @param first		Resource we're looking to perform the operation on,
   			if one exists.
   @param type		Type to look for.
   @param ret		Unused, but will be used to store status information
   			such as resources consumed, etc, in the future.
   @param realop	Operation to perform if either first is found,
   			or no first is declared (in which case, all nodes
			in the subtree).
   @see			_res_op_by_level res_exec
 */
int
_res_op(resource_node_t **tree, resource_t *first,
	char *type, void * __attribute__((unused))ret, int realop)
{
  	resource_node_t *node;
 	int count = 0, rv = 0;
 	
 	if (realop == RS_STOP) {
 		list_for_rev(tree, node, count) {
 			rv |= _res_op_internal(tree, first, type, ret, realop,
 					       node);
 		}
 	} else {
 		list_for(tree, node, count) {
 			rv |= _res_op_internal(tree, first, type, ret, realop,
 					       node);

			/* If we hit a problem during a 'status' op in an
			   independent subtree, rv will have the
			   SFL_RECOVERABLE bit set, but not SFL_FAILURE.
			   If we ever hit SFL_FAILURE during a status
			   operation, we're *DONE* - even if the subtree
			   is flagged w/ indy-subtree */
			  
 			if (rv & SFL_FAILURE) 
 				return rv;
 		}
 	}

	return rv;
}

/**
   Start all occurrences of a resource in a tree

   @param tree		Tree to search for our resource.
   @param res		Resource to start/stop
   @param ret		Unused
 */
int
res_start(resource_node_t **tree, resource_t *res, void *ret)
{
	return _res_op(tree, res, NULL, ret, RS_START);
}


/**
   Start all occurrences of a resource in a tree

   @param tree		Tree to search for our resource.
   @param res		Resource to start/stop
   @param ret		Unused
 */
int
res_condstop(resource_node_t **tree, resource_t *res, void *ret)
{
	return _res_op(tree, res, NULL, ret, RS_CONDSTOP);
}


/**
   Repair/fix/convalesce all occurrences of a resource in a tree

   @param tree		Tree to search for our resource.
   @param res		Resource to start/stop
   @param ret		Unused
 */
int
res_convalesce(resource_node_t **tree, resource_t *res, void *ret)
{
	return _res_op(tree, res, NULL, ret, RS_CONVALESCE);
}


/**
   Start all occurrences of a resource in a tree

   @param tree		Tree to search for our resource.
   @param res		Resource to start/stop
   @param ret		Unused
 */
int
res_condstart(resource_node_t **tree, resource_t *res, void *ret)
{
	return _res_op(tree, res, NULL, ret, RS_CONDSTART);
}


/**
   Stop all occurrences of a resource in a tree

   @param tree		Tree to search for our resource.
   @param res		Resource to start/stop
   @param ret		Unused
 */
int
res_stop(resource_node_t **tree, resource_t *res, void *ret)
{
	return _res_op(tree, res, NULL, ret, RS_STOP);
}


/**
   Check status of all occurrences of a resource in a tree

   @param tree		Tree to search for our resource.
   @param res		Resource to start/stop
   @param ret		Unused
 */
int
res_status(resource_node_t **tree, resource_t *res, void *ret)
{
	return _res_op(tree, res, NULL, ret, RS_STATUS);
}


/**
   Check status of all occurrences of a resource in a tree

   @param tree		Tree to search for our resource.
   @param res		Resource to start/stop
   @param ret		Unused
 */
int
res_status_inquiry(resource_node_t **tree, resource_t *res, void *ret)
{
	return _res_op(tree, res, NULL, ret, RS_STATUS_INQUIRY);
}


/**
   Find the delta of two resource lists and flag the resources which need
   to be restarted/stopped/started. 
  */
int
resource_delta(resource_t **leftres, resource_t **rightres)
{
	resource_t *lc, *rc;
	int ret;

	list_do(leftres, lc) {
		rc = find_resource_by_ref(rightres, lc->r_rule->rr_type,
					  primary_attr_value(lc));

		/* No restart.  It's gone. */
		if (!rc) {
			lc->r_flags |= RF_NEEDSTOP;
			continue;
		}

		/* Ok, see if the resource is the same */
		ret = rescmp(lc, rc);
		if (ret	== 0) {
			rc->r_flags |= RF_COMMON;
			continue;
		}

		if (ret == 2) {
			/* return of 2 from rescmp means
			   the two resources differ only 
			   by reconfigurable bits */
			/* Do nothing on condstop phase;
			   do a "reconfig" instead of 
			   "start" on conststart phase */
			rc->r_flags |= RF_COMMON;
			rc->r_flags |= RF_NEEDSTART;
			rc->r_flags |= RF_RECONFIG;
			continue;
		}

		rc->r_flags |= RF_COMMON;

		/* Resource has changed.  Flag it. */
		lc->r_flags |= RF_NEEDSTOP;
		rc->r_flags |= RF_NEEDSTART;

	} while (!list_done(leftres, lc));

	/* Ok, if we weren't existing, flag as a needstart. */
	list_do(rightres, rc) {
		if (rc->r_flags & RF_COMMON)
			rc->r_flags &= ~RF_COMMON;
		else
			rc->r_flags |= RF_NEEDSTART;
	} while (!list_done(rightres, rc));
	/* Easy part is done. */
	return 0;
}


/**
   Part 2 of online mods:  Tree delta.  Ow.  It hurts.  It hurts.
   We need to do this because resources can be moved from one RG
   to another with nothing changing (not even refcnt!).
  */
int
resource_tree_delta(resource_node_t **ltree, resource_node_t **rtree)
{
	resource_node_t *ln, *rn;
	int rc;

	list_do(ltree, ln) {
		/*
		   Ok.  Run down the left tree looking for obsolete resources.
		   (e.g. those that need to be stopped)

		   If it's obsolete, continue.  All children will need to be
		   restarted too, so we don't need to compare the children
		   of this node.
		 */
		if (ln->rn_resource->r_flags & RF_NEEDSTOP) {
			ln->rn_flags |= RF_NEEDSTOP;
			continue;
		}

		/*
		   Ok.  This particular node wasn't flagged. 
		 */
		list_do(rtree, rn) {

			rc = rescmp(ln->rn_resource, rn->rn_resource);

			/* Wildly different resource? */
			if (rc <= -1)
				continue;

			/*
			   If it needs to be started (e.g. it's been altered
			   or is new), then we don't really care about its
			   children.
			 */

			if (rn->rn_resource->r_flags & RF_NEEDSTART) {
				rn->rn_flags |= RF_NEEDSTART;
				if ((rn->rn_resource->r_flags & RF_RECONFIG) == 0)
					continue;
			}

			if (rc == 0 || rc == 2) {
				if (rc == 2)
					rn->rn_flags |= RF_NEEDSTART | RF_RECONFIG;

				if (rc == 0) 
					rn->rn_state = ln->rn_state;

				/* Ok, same resource.  Recurse. */
				ln->rn_flags |= RF_COMMON;
				rn->rn_flags |= RF_COMMON;
				resource_tree_delta(&ln->rn_child,
						    &rn->rn_child);
			}

		} while (!list_done(rtree, rn));

		if (ln->rn_flags & RF_COMMON)
			ln->rn_flags &= ~RF_COMMON;
		else 
			ln->rn_flags |= RF_NEEDSTOP;

	} while (!list_done(ltree, ln));

	/*
	   See if we need to start anything.  Stuff which wasn't flagged
	   as COMMON needs to be started.

	   As always, if we have to start a node, everything below it
	   must also be started.
	 */
	list_do(rtree, rn) {
		if (rn->rn_flags & RF_COMMON)
			rn->rn_flags &= ~RF_COMMON;
		else
			rn->rn_flags |= RF_NEEDSTART;
	} while (!list_done(rtree, rn));

	return 0;
}
