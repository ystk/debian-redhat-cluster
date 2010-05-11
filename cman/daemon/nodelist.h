/* These just make the access a little neater */
static inline int objdb_get_string(OBJDB_API *corosync, hdb_handle_t object_service_handle,
				   const char *key, char **value)
{
	int res;

	*value = NULL;
	if ( !(res = corosync->object_key_get(object_service_handle,
					      key,
					      strlen(key),
					      (void *)value,
					      NULL))) {
		if (*value)
			return 0;
	}
	return -1;
}

static inline void objdb_get_int(OBJDB_API *corosync, hdb_handle_t object_service_handle,
				 const char *key, unsigned int *intvalue, unsigned int default_value)
{
	char *value = NULL;

	*intvalue = default_value;

	if (!corosync->object_key_get(object_service_handle, key, strlen(key),
				 (void *)&value, NULL)) {
		if (value) {
			*intvalue = atoi(value);
		}
	}
}


/* Helper functions for navigating the nodes list */
static inline hdb_handle_t nodeslist_init(OBJDB_API *corosync,
					  hdb_handle_t cluster_parent_handle,
					  hdb_handle_t *find_handle)
{
	hdb_handle_t object_handle;
	hdb_handle_t find_handle1;
	hdb_handle_t find_handle2;

	corosync->object_find_create(cluster_parent_handle,"clusternodes", strlen("clusternodes"), &find_handle1);
	if (corosync->object_find_next(find_handle1, &object_handle) == 0)
	{
		hdb_handle_t nodes_handle;
		corosync->object_find_destroy(find_handle1);

		corosync->object_find_create(object_handle,"clusternode", strlen("clusternode"), &find_handle2);

		if (corosync->object_find_next(find_handle2, &nodes_handle) == 0)
		{
			*find_handle = find_handle2;
			return nodes_handle;
		}
	}
	return 0;
}

static inline hdb_handle_t nodeslist_next(OBJDB_API *corosync, hdb_handle_t find_handle)
{
        hdb_handle_t nodes_handle;

	if (corosync->object_find_next(find_handle, &nodes_handle) == 0)
		return nodes_handle;
	else
		return 0;
}

static inline hdb_handle_t nodelist_byname(OBJDB_API *corosync,
					   hdb_handle_t cluster_parent_handle,
					   char *name)
{
	char *nodename;
	hdb_handle_t nodes_handle;
	hdb_handle_t find_handle = 0;

	nodes_handle = nodeslist_init(corosync, cluster_parent_handle, &find_handle);
	while (nodes_handle) {
		if (objdb_get_string(corosync, nodes_handle, "name", &nodename)) {
			nodes_handle = nodeslist_next(corosync, find_handle);
			continue;
		}
		if (strcmp(nodename, name) == 0)
			return nodes_handle;

		nodes_handle = nodeslist_next(corosync, find_handle);
	}
	corosync->object_find_destroy(find_handle);

	return 0;
}
