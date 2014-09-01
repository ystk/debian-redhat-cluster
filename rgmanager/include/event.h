#ifndef _EVENT_H
#define _EVENT_H

/* 128 is a bit big, but it should be okay */
typedef struct __rge_q {
	char rg_name[128];
	uint32_t rg_state;
	uint32_t pad1;
	int rg_owner;
	int rg_last_owner;
} group_event_t;

typedef struct __ne_q {
	int ne_local;
	int ne_nodeid;
	int ne_state;
	int ne_clean;
} node_event_t;

typedef struct __cfg_q {
	int cfg_version;	/* Not used or handled for now */
	int cfg_oldversion;
} config_event_t;

typedef struct __user_q {
	char u_name[128];
	msgctx_t *u_ctx;
	int u_request;
	int u_arg1;
	int u_arg2;
	int u_target;		/* Node ID */
} user_event_t;

typedef enum {
	EVENT_NONE=0,
	EVENT_CONFIG,
	EVENT_NODE,
	EVENT_RG,
	EVENT_USER
} event_type_t;

/* Data that's distributed which indicates which
   node is the event master */
typedef struct __rgm {
	uint32_t m_magic;
	uint32_t m_nodeid;
	uint64_t m_master_time;
	uint8_t  m_reserved[112];
} event_master_t;

#define swab_event_master_t(ptr) \
{\
	swab32((ptr)->m_nodeid);\
	swab32((ptr)->m_magic);\
	swab64((ptr)->m_master_time);\
}

/* Just a magic # to help us ensure we've got good
   date from VF */
#define EVENT_MASTER_MAGIC 0xfabab0de

/* Event structure - internal to the event subsystem; use
   the queueing functions below which allocate this struct
   and pass it to the event handler */
typedef struct _event {
	/* Not used dynamically - part of config info */
	list_head();
	char *ev_name;
	char *ev_script;
	char *ev_script_file;
	int ev_prio; 
	int ev_pad;
	/* --- end config part */
	int ev_type;		/* config & generated by rgmanager*/
	int ev_transaction;
	union {
		group_event_t group;
		node_event_t node;
		config_event_t config;
		user_event_t user;
	} ev;
} event_t;

#define EVENT_PRIO_COUNT 100

typedef struct _event_table {
	int max_prio;
	int pad;
	event_t *entries[0];
} event_table_t;


int construct_events(int ccsfd, event_table_t **);
void deconstruct_events(event_table_t **);
void print_events(event_table_t *);
void dump_events(FILE *fp, event_table_t *);

/* Does the event match a configured event? */
int event_match(event_t *pattern, event_t *actual);

/* Event queueing functions. */
void node_event_q(int local, int nodeID, int state, int clean);
void rg_event_q(char *name, uint32_t state, int owner, int last);
void user_event_q(char *svc, int request, int arg1, int arg2,
		  int target, msgctx_t *ctx);
void config_event_q(void);

/* Call this to see if there's a master. */
int event_master_info_cached(event_master_t *);

/* Call this to get the node ID of the current 
   master *or* become the master if none exists */
int event_master(void);

/* Setup */
int central_events_enabled(void);
void set_central_events(int flag);
int slang_process_event(event_table_t *event_table, event_t *ev);

/* For distributed events. */
void set_transition_throttling(int nsecs);
int get_transition_throttling(void);
void broadcast_event(const char *svcName, uint32_t state, int owner, int last);

/* Simplified service start. */
int service_op_start(char *svcName, int *target_list, int target_list_len,
		     int *new_owner);
int service_op_stop(char *svcName, int do_disable, int event_type);
int service_op_convalesce(const char *svcName);
int service_op_migrate(char *svcName, int target_node);


/* Non-central event processing */
void node_event(int local, int nodeID, int nodeStatus, int clean);

int32_t master_event_callback(char *key, uint64_t viewno, void *data,
			      uint32_t datalen);

int node_has_fencing(int nodeid);
int fence_domain_joined(void);
int get_service_state_internal(const char *svcName, rg_state_t *svcStatus);

#endif
