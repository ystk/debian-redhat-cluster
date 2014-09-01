#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <getopt.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <libxml/tree.h>

#include "editconf.h"

#define MAX_NODES 256

const char *prog_name = "ccs_tool";

#define die(fmt, args...) \
do { \
	fprintf(stderr, "%s: ", prog_name); \
	fprintf(stderr, fmt "\n", ##args); \
	exit(EXIT_FAILURE); \
} while (0)


#define INT_TO_CHAR(x, str) \
	if (str && atoi((const char *)str)) \
		x = '*'; \
	else \
		x = ' ';

struct option_info
{
	const char *name;
	const char *altname;
	const char *votes;
	const char *nodeid;
	const char *mcast_addr;
	const char *ip_addr;
	const char *fence_type;
	const char *autostart;
	const char *exclusive;
	const char *recovery;
	const char *fs;
	const char *domain;
	const char *script;
	const char *mountpoint;
	const char *type;
	const char *device;
	const char *options;
	const char *configfile;
	const char *outputfile;
	const char **failover_nodes;
	int  do_delete;
	int  force_fsck;
	int  force_unmount;
	int  self_fence;
	int  ordered;
	int  restricted;
};

static void config_usage(int rw)
{
	fprintf(stderr, " -c --configfile    Name of configuration file (" DEFAULT_CONFIG_DIR "/" DEFAULT_CONFIG_FILE ")\n");
	if (rw)
	{
		fprintf(stderr, " -o --outputfile    Name of output file (defaults to same as --configfile)\n");
	}
}

static void help_usage(void)
{
	fprintf(stderr, " -h --help          Display this help text\n");
}

static void list_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [options]\n", prog_name, name);
	fprintf(stderr, " -v --verbose       Print all properties of the item\n");
	config_usage(0);
	help_usage();

	exit(0);
}

static void create_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [-2] <clustername>\n", prog_name, name);
	fprintf(stderr, " -2                 Create a 2-node cman cluster config file\n");
	fprintf(stderr, " -n <num>           Create skeleton entries for <num> nodes\n");
	fprintf(stderr, " -f <device>        Add a fence device to the node skeletons\n");
	config_usage(0);
	help_usage();
	fprintf(stderr, "\n"
	  "Note that \"create\" on its own will not create a valid configuration file.\n"
	  "Fence agents and nodes will need to be added to it before handing it over\n"
	  "to cman.\n"
	  "\n"
	  "eg:\n"
	  "  ccs_tool create MyCluster\n"
	  "  ccs_tool addfence apc fence_apc ipaddr=apc.domain.net user=apc password=apc\n"
	  "  ccs_tool addnode node1 -n 1 -f apc port=1\n"
	  "  ccs_tool addnode node2 -n 2 -f apc port=2\n"
	  "  ccs_tool addnode node3 -n 3 -f apc port=3\n"
	  "  ccs_tool addnode node4 -n 4 -f apc port=4\n"
          "\n");
	fprintf(stderr, "If you add -n <numbner> to the command then %s will add skeleton entries for\n", name);
	fprintf(stderr, "that many nodes. This file WILL NEED EDITTING MANUALLY before it can be used\n");
	fprintf(stderr, "by cman.\n");

	exit(0);
}

static void addfence_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [options] <name> <agent> [param=value]\n", prog_name, name);
	config_usage(1);
	help_usage();

	exit(0);
}

static void delfence_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [options] <name>\n", prog_name, name);
	config_usage(1);
	help_usage();
	fprintf(stderr, "\n");
	fprintf(stderr, "%s will allow you to remove a fence device that is in use by nodes.\n", name);
	fprintf(stderr, "This is to allow changes to be made, but be aware that it may produce an\n");
	fprintf(stderr, "invalid configuration file if you don't add it back in again.\n");

	exit(0);
}

static void delnode_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [options] <name>\n", prog_name, name);
	config_usage(1);
	help_usage();

	exit(0);
}

static void addscript_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [options] <name> <path_to_script>\n",
			prog_name, name);
	config_usage(1);
	help_usage();

	exit(0);
}

static void addip_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [options] <IP_address>\n",
			prog_name, name);
	config_usage(1);
	help_usage();

	exit(0);
}

static void addfs_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [options] <name> <device> <mountpoint>\n",
			prog_name, name);
	fprintf(stderr, " -t --type          Type of the filesystem (ext3, ext4, etc.)\n");
	fprintf(stderr, "                    Default type is ext3.\n");
	fprintf(stderr, " -p --options       Mount options\n");
	fprintf(stderr, " -k --force_fsck    Force fsck before mount\n");
	fprintf(stderr, " -u --force_unmount Call umount with force flag\n");
	fprintf(stderr, " -s --self_fence    Use 'self_fence' feature\n");
	config_usage(1);
	help_usage();

	exit(0);
}

static void addfdomain_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [options] <name> <node1> ... <nodeN>\n",
			prog_name, name);
	fprintf(stderr, " -p --ordered       Allows you to specify a preference order\n");
	fprintf(stderr, "                    among the members of a failover domain\n");
	fprintf(stderr, " -r --restricted    Allows you to restrict the members that can\n");
	fprintf(stderr, "                    run a particular cluster service.\n");
	config_usage(1);
	help_usage();

	exit(0);
}

static void addnodeid_usage(const char *name)
{
	fprintf(stderr, "Add node IDs to all nodes in the config file that don't have them.\n");
	fprintf(stderr, "Nodes with IDs will not be afftected, so you can run this as many times\n");
	fprintf(stderr, "as you like without doing any harm.\n");
	fprintf(stderr, "It will optionally add a multicast address to the cluster config too.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Usage: %s %s [options] <name>\n", prog_name, name);
	fprintf(stderr, " -n --nodeid        Nodeid to start with (default 1)\n");
	fprintf(stderr, " -m --multicast     Set or change the multicast address\n");
	fprintf(stderr, " -v --verbose       Print nodeids that are assigned\n");
	config_usage(1);
	help_usage();

	exit(0);
}

static void addnode_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [options] <nodename> [<fencearg>=<value>]...\n", prog_name, name);
	fprintf(stderr, " -n --nodeid        Nodeid (required)\n");
	fprintf(stderr, " -v --votes         Number of votes for this node (default 1)\n");
	fprintf(stderr, " -a --altname       Alternative name/interface for multihomed hosts\n");
	fprintf(stderr, " -f --fence_type    Type of fencing to use\n");
	config_usage(1);
	help_usage();

	fprintf(stderr, "\n");
	fprintf(stderr, "Examples:\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Add a new node to default configuration file:\n");
	fprintf(stderr, "  %s %s newnode1 -n 1 -f manual ipaddr=newnode\n", prog_name, name);
	fprintf(stderr, "\n");
	fprintf(stderr, "Add a new node and dump config file to stdout rather than save it\n");
	fprintf(stderr, "  %s %s newnode2 -n 2 -f apc -o- newnode.temp.net port=1\n", prog_name, name);

	exit(0);
}

static void addservice_usage(const char *name)
{
	fprintf(stderr, "Usage: %s %s [options] <servicename>\n", prog_name, name);
	fprintf(stderr, " -a --autostart       Start the service on boot\n");
	fprintf(stderr, " -d --domain          Failover domain for the service\n");
	fprintf(stderr, " -x --exclusive       Do not run other services on the same server\n");
	fprintf(stderr, " -r --recovery        Recovery policy\n");
	fprintf(stderr, " -f --fs              Filesystem resource for the service\n");
	fprintf(stderr, " -s --script          Script resource for the service\n");
	fprintf(stderr, " -i --ip              IP address resource for the service\n");
	config_usage(1);
	help_usage();

	exit(0);
}

/* Is it really ?
 * Actually, we don't check that this is a valid multicast address(!),
 * merely that it is a valid IP[46] address.
 */
static int valid_mcast_addr(char *mcast)
{
        struct addrinfo *ainfo;
        struct addrinfo ahints;
	int ret;

        memset(&ahints, 0, sizeof(ahints));

        ret = getaddrinfo(mcast, NULL, &ahints, &ainfo);
	if (ret) {
		freeaddrinfo(ainfo);
		return 0;
	}
	return 1;
}

static void save_file(xmlDoc *doc, struct option_info *ninfo)
{
	char tmpconffile[strlen(ninfo->outputfile)+5];
	char oldconffile[strlen(ninfo->outputfile)+5];
	int using_stdout = 0;
	mode_t old_mode;
	int ret;

	old_mode = umask(026);

	if (strcmp(ninfo->outputfile, "-") == 0)
		using_stdout = 1;

	/*
	 * Save it to a temp file before moving the old one out of the way
	 */
	if (!using_stdout)
	{
		snprintf(tmpconffile, sizeof(tmpconffile), "%s.tmp", ninfo->outputfile);
		snprintf(oldconffile, sizeof(oldconffile), "%s.old", ninfo->outputfile);
	}
	else
	{
		strcpy(tmpconffile, ninfo->outputfile);
	}

	xmlKeepBlanksDefault(0);
	ret = xmlSaveFormatFile(tmpconffile, doc, 1);
	if (ret == -1)
		die("Error writing new config file %s", ninfo->outputfile);

	if (!using_stdout)
	{
		if (rename(ninfo->outputfile, oldconffile) == -1 && errno != ENOENT)
			die("Can't move old config file out of the way\n");

		if (rename(tmpconffile, ninfo->outputfile))
		{
			perror("Error renaming new file to its real filename");

			/* Drat, that failed, try to put the old one back */
			if (rename(oldconffile, ninfo->outputfile))
				die("Can't move old config fileback in place - clean up after me please\n");
		}
	}

	/* free the document */
	xmlFreeDoc(doc);

	umask(old_mode);
}

static void validate_int_arg(char argopt, char *arg)
{
	char *tmp;
	int val;

	val = strtol(arg, &tmp, 10);
	if (tmp == arg || tmp != arg + strlen(arg))
		die("argument to %c (%s) is not an integer", argopt, arg);

	if (val < 0)
		die("argument to %c cannot be negative", argopt);
}

/* Get the config_version string from the file */
static xmlChar *find_version(xmlNode *root)
{
	if (xmlHasProp(root, BAD_CAST "config_version"))
	{
		xmlChar *ver;

		ver = xmlGetProp(root, BAD_CAST "config_version");
		return ver;
	}
	return NULL;
}

/* Get the cluster name string from the file */
static xmlChar *cluster_name(xmlNode *root)
{
	if (xmlHasProp(root, BAD_CAST "name"))
	{
		xmlChar *ver;

		ver = xmlGetProp(root, BAD_CAST "name");
		return ver;
	}
	return NULL;
}

static void increment_version(xmlNode *root_element)
{
	int ver;
	unsigned char *version_string;
	char newver[32];

	/* Increment version */
	version_string = find_version(root_element);
	if (!version_string)
		die("Can't find \"config_version\" in config file\n");

	ver = atoi((char *)version_string);
	snprintf(newver, sizeof(newver), "%d", ++ver);
	xmlSetProp(root_element, BAD_CAST "config_version", BAD_CAST newver);
}

static void _xmlSetIntProp(xmlNode *element, const char *property, const int value)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", value);
	xmlSetProp(element, BAD_CAST property, BAD_CAST buf);
}

static xmlNode *findnode(xmlNode *root, const char *name)
{
	xmlNode *cur_node;

	for (cur_node = root->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE && strcmp((char *)cur_node->name, name)==0)
		{
			return cur_node;
		}
	}
	return NULL;
}

/* Return the fence type name (& node) for a cluster node */
static xmlChar *get_fence_type(xmlNode *clusternode, xmlNode **fencenode)
{
	xmlNode *f;

	f = findnode(clusternode, "fence");
	if (f)
	{
		f = findnode(f, "method");
		if (f)
		{
			f = findnode(f, "device");
			*fencenode = f;
			return xmlGetProp(f, BAD_CAST "name");
		}
	}
	return NULL;
}

/* Check the fence type exists under <fencedevices> */
static xmlNode *valid_fence_type(xmlNode *root, const char *fencetype)
{
	xmlNode *devs;
	xmlNode *cur_node;

	devs = findnode(root, "fencedevices");
	if (!devs)
		return NULL;

	for (cur_node = devs->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE && strcmp((char *)cur_node->name, "fencedevice") == 0)
		{
			xmlChar *name = xmlGetProp(cur_node, BAD_CAST "name");
			if (strcmp((char *)name, fencetype) == 0)
				return cur_node;
		}
	}
	return NULL;
}

/* Check the nodeid is not already in use by another node */
static xmlNode *get_by_nodeid(xmlNode *root, int nodeid)
{
	xmlNode *cnodes;
	xmlNode *cur_node;

	cnodes = findnode(root, "clusternodes");
	if (!cnodes)
		return NULL;

	for (cur_node = cnodes->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE && strcmp((char *)cur_node->name, "clusternode") == 0)
		{
			xmlChar *idstring = xmlGetProp(cur_node, BAD_CAST "nodeid");
			if (idstring && atoi((char *)idstring) == nodeid)
				return cur_node;
		}
	}
	return NULL;
}


/* Get the multicast address node.
 */
static xmlNode *find_multicast_addr(xmlNode *clusternodes)
{
	xmlNode *clnode = findnode(clusternodes, "cman");
	if (clnode)
	{
		xmlNode *mcast = findnode(clnode, "multicast");
		return mcast;
	}
	return NULL;
}

static xmlNode *do_find_node(xmlNode *root, const char *nodename,
	const char *elem_name, const char *attrib_name)
{
	xmlNode *cur_node;

	for (cur_node = root->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE &&
			strcmp((char *)cur_node->name, elem_name) == 0)
		{
			xmlChar *name = xmlGetProp(cur_node, BAD_CAST attrib_name);
			if (name && strcmp((char *)name, nodename) == 0)
				return cur_node;
		}
	}
	return NULL;
}

static xmlNode *do_find_resource_ref(xmlNode *root, const char *name,
		const char *res_type)
{
	xmlNode *cur_node;
	xmlNode *res = NULL;

	for (cur_node = root->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE &&
			strcmp((char *)cur_node->name, "service") == 0)
		{
			res = do_find_node(cur_node, name, res_type, "ref");
			if (res)
				break;
		}
	}

	return res;
}

static xmlNode *find_node(xmlNode *clusternodes, const char *nodename)
{
	return do_find_node(clusternodes, nodename, "clusternode", "name");
}

static xmlNode *find_service(xmlNode *root, const char *servicename)
{
	return do_find_node(root, servicename, "service", "name");
}

static xmlNode *find_fs_resource(xmlNode *root, const char *name)
{
	return do_find_node(root, name, "fs", "name");
}

static xmlNode *find_fdomain_resource(xmlNode *root, const char *name)
{
	return do_find_node(root, name, "failoverdomain", "name");
}

static xmlNode *find_script_resource(xmlNode *root, const char *name)
{
	return do_find_node(root, name, "script", "name");
}

static xmlNode *find_script_ref(xmlNode *root, const char *name)
{
	return do_find_resource_ref(root, name, "script");
}

static xmlNode *find_ip_resource(xmlNode *root, const char *name)
{
	return do_find_node(root, name, "ip", "address");
}

static xmlNode *find_ip_ref(xmlNode *root, const char *name)
{
	return do_find_resource_ref(root, name, "ip");
}

static xmlNode *find_fs_ref(xmlNode *root, const char *name)
{
	return do_find_resource_ref(root, name, "fs");
}

static xmlNode *find_fdomain_ref(xmlNode *root, const char *name)
{
	xmlNode *cur_node;

	for (cur_node = root->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE &&
			strcmp((char *)cur_node->name, "service") == 0)
		{
			xmlChar *domain = xmlGetProp(cur_node, BAD_CAST "domain");
			if (domain && strcmp(name, (char *)domain) == 0)
				return cur_node;
		}
	}

	return NULL;
}

/* Print name=value pairs for a (n XML) node.
 * "ignore" is a string to ignore if present as a property (probably already printed on the main line)
 */
static int print_properties(xmlNode *node, const char *prefix, const char *ignore, const char *ignore2)
{
	xmlAttr *attr;
	int done_prefix = 0;

	for (attr = node->properties; attr; attr = attr->next)
	{
		/* Don't print "name=" */
		if (strcmp((char *)attr->name, "name") &&
		    strcmp((char *)attr->name, ignore) &&
		    strcmp((char *)attr->name, ignore2)
			)
		{
			if (!done_prefix)
			{
				done_prefix = 1;
				printf("%s", prefix);
			}
			printf(" %s=%s", attr->name, xmlGetProp(node, attr->name));
		}
	}
	if (done_prefix)
		printf("\n");
	return done_prefix;
}

/* Add name=value pairs from the commandline as properties to a node */
static void add_fence_args(xmlNode *fencenode, int argc, char **argv, int optindex)
{
	int i;

	for (i = optindex; i<argc; i++)
	{
		char *prop;
		char *value;
		char *equals;

		prop = strdup(argv[i]);
		// FIXME: handle failed strdup
		equals = strchr(prop, '=');
		if (!equals)
			die("option '%s' is not opt=value pair\n", prop);

		value = equals+1;
		*equals = '\0';

		/* "name" is used for the fence type itself, so this is just
		 *  to protect the user from their own stupidity
		 */
		if (strcmp(prop, "name") == 0)
			die("Can't use \"name\" as a fence argument name\n");

		xmlSetProp(fencenode, BAD_CAST prop, BAD_CAST value);
		free(prop);
	}
}

static void add_clusternode(xmlNode *root_element, struct option_info *ninfo,
			    int argc, char **argv, int optindex)
{
	xmlNode *clusternodes;
	xmlNode *newnode;

	xmlNode *newfence;
	xmlNode *newfencemethod;
	xmlNode *newfencedevice;

	clusternodes = findnode(root_element, "clusternodes");
	if (!clusternodes)
		die("Can't find \"clusternodes\" in %s\n", ninfo->configfile);

	/* Don't allow duplicate node names */
	if (find_node(clusternodes, ninfo->name))
		die("node %s already exists in %s\n", ninfo->name, ninfo->configfile);

	/* Check for duplicate node ID */
	if (!ninfo->nodeid)
		die("nodeid not specified\n");

	if (get_by_nodeid(root_element, atoi((char *)ninfo->nodeid)))
		die("nodeid %s already in use\n", ninfo->nodeid);

        /* Don't allow random fence types */
	if (ninfo->fence_type && !valid_fence_type(root_element, ninfo->fence_type))
		die("fence type '%s' not known\n", ninfo->fence_type);

	/* Add the new node */
	newnode = xmlNewNode(NULL, BAD_CAST "clusternode");
	xmlSetProp(newnode, BAD_CAST "name", BAD_CAST ninfo->name);
	xmlSetProp(newnode, BAD_CAST "votes", BAD_CAST ninfo->votes);
	xmlSetProp(newnode, BAD_CAST "nodeid", BAD_CAST ninfo->nodeid);
	xmlAddChild(clusternodes, newnode);

	if (ninfo->altname)
	{
		xmlNode *altnode;

		altnode = xmlNewNode(NULL, BAD_CAST "altname");
		xmlSetProp(altnode, BAD_CAST "name", BAD_CAST ninfo->altname);
		xmlAddChild(newnode, altnode);
	}

	/* Add the fence attributes */
	if (ninfo->fence_type)
	{
		newfence = xmlNewNode(NULL, BAD_CAST "fence");
		newfencemethod = xmlNewNode(NULL, BAD_CAST "method");
		xmlSetProp(newfencemethod, BAD_CAST "name", BAD_CAST "single");

		newfencedevice = xmlNewNode(NULL, BAD_CAST "device");
		xmlSetProp(newfencedevice, BAD_CAST "name", BAD_CAST ninfo->fence_type);

		/* Add name=value options */
		add_fence_args(newfencedevice, argc, argv, optindex+1);

		xmlAddChild(newnode, newfence);
		xmlAddChild(newfence, newfencemethod);
		xmlAddChild(newfencemethod, newfencedevice);
	}
}

static void add_clusterservice(xmlNode *root_element, struct option_info *ninfo,
			    int argc, char **argv, int optindex)
{
	xmlNode *rm;
	xmlNode *rs;
	xmlNode *fdomains;
	xmlNode *newnode;

	xmlNode *newfs = NULL;
	xmlNode *newfsscript;
	xmlNode *newfsip;

	rm = findnode(root_element, "rm");
	if (!rm)
		die("Can't find \"rm\" in %s\n", ninfo->configfile);

	/* Don't allow duplicate service names */
	if (find_service(rm, ninfo->name))
		die("service %s already exists in %s\n", ninfo->name,
				ninfo->configfile);

	rs = findnode(rm, "resources");
	fdomains = findnode(rm, "failoverdomains");
	if (ninfo->fs && (!rs || !find_fs_resource(rs, ninfo->fs)))
		die("fs resource %s doesn't exist in %s\n", ninfo->fs,
				ninfo->configfile);
	if (ninfo->script && (!rs || !find_script_resource(rs, ninfo->script)))
		die("script resource %s doesn't exist in %s\n", ninfo->script,
				ninfo->configfile);
	if (ninfo->ip_addr && (!rs || !find_ip_resource(rs, ninfo->ip_addr)))
		die("ip resource %s doesn't exist in %s\n", ninfo->ip_addr,
				ninfo->configfile);
	if (ninfo->domain && (!fdomains || !find_fdomain_resource(fdomains, ninfo->domain)))
		die("failover domain %s doesn't exist in %s\n", ninfo->domain,
				ninfo->configfile);

	/* Add the new service */
	newnode = xmlNewNode(NULL, BAD_CAST "service");
	xmlSetProp(newnode, BAD_CAST "name", BAD_CAST ninfo->name);
	xmlSetProp(newnode, BAD_CAST "autostart", BAD_CAST ninfo->autostart);
	if (ninfo->domain)
		xmlSetProp(newnode, BAD_CAST "domain", BAD_CAST ninfo->domain);
	if (ninfo->exclusive)
		xmlSetProp(newnode, BAD_CAST "exclusive",
				BAD_CAST ninfo->exclusive);
	xmlSetProp(newnode, BAD_CAST "recovery", BAD_CAST ninfo->recovery);
	xmlAddChild(rm, newnode);

	/* Add the fs reference */
	if (ninfo->fs)
	{
		newfs = xmlNewNode(NULL, BAD_CAST "fs");
		xmlSetProp(newfs, BAD_CAST "ref", BAD_CAST ninfo->fs);
		xmlAddChild(newnode, newfs);
	}

	/* Add the script reference */
	if (ninfo->script)
	{
		newfsscript = xmlNewNode(NULL, BAD_CAST "script");
		xmlSetProp(newfsscript, BAD_CAST "ref", BAD_CAST ninfo->script);
		if (newfs)
			xmlAddChild(newfs, newfsscript);
		else
			xmlAddChild(newnode, newfsscript);
	}

	/* Add the ip reference */
	if (ninfo->ip_addr)
	{
		newfsip = xmlNewNode(NULL, BAD_CAST "ip");
		xmlSetProp(newfsip, BAD_CAST "ref", BAD_CAST
				ninfo->ip_addr);
		if (newfs)
			xmlAddChild(newfs, newfsip);
		else
			xmlAddChild(newnode, newfsip);
	}
}

static void add_clusterfs(xmlNode *root_element, struct option_info *ninfo,
			    int argc, char **argv, int optindex)
{
	xmlNode *rm;
	xmlNode *rs;
	xmlNode *node;

	rm = findnode(root_element, "rm");
	if (!rm)
		die("Can't find \"rm\" in %s\n", ninfo->configfile);

	rs = findnode(rm, "resources");
	if (!rs)
		die("Can't find \"resources\" %s\n", ninfo->configfile);

	/* Check it doesn't already exist */
	if (find_fs_resource(rs, ninfo->name))
		die("fs %s already exists\n", ninfo->name);

	/* Add the new fs resource */
	node = xmlNewNode(NULL, BAD_CAST "fs");
	xmlSetProp(node, BAD_CAST "device", BAD_CAST ninfo->device);
	_xmlSetIntProp(node, "force_fsck", ninfo->force_fsck);
	_xmlSetIntProp(node, "force_unmount", ninfo->force_unmount);
	xmlSetProp(node, BAD_CAST "fstype", BAD_CAST ninfo->type);
	xmlSetProp(node, BAD_CAST "mountpoint", BAD_CAST ninfo->mountpoint);
	xmlSetProp(node, BAD_CAST "name", BAD_CAST ninfo->name);
	xmlSetProp(node, BAD_CAST "options", (ninfo->options) ?
					BAD_CAST ninfo->options : BAD_CAST "");
	_xmlSetIntProp(node, "self_fence", ninfo->self_fence);
	xmlAddChild(rs, node);
}

static void add_clusterfdomain(xmlNode *root_element, struct option_info *ninfo,
			    int argc, char **argv, int optindex)
{
	xmlNode *rm;
	xmlNode *fdomains;
	xmlNode *node;
	xmlNode *cn;
	int i = 0;

	rm = findnode(root_element, "rm");
	if (!rm)
		die("Can't find \"rm\" in %s\n", ninfo->configfile);

	fdomains = findnode(rm, "failoverdomains");
	if (!fdomains)
		die("Can't find \"failoverdomains\" %s\n", ninfo->configfile);

	cn = findnode(root_element, "clusternodes");
	if (!cn)
		die("Can't find \"clusternodes\" in %s\n", ninfo->configfile);

	/* Check it doesn't already exist */
	if (find_fdomain_resource(fdomains, ninfo->name))
		die("failover domain %s already exists\n", ninfo->name);

	/* Check that nodes are defined */
	while (ninfo->failover_nodes[i]) {
		if (!find_node(cn, ninfo->failover_nodes[i]))
			die("Can't find node %s in %s.\n",
				ninfo->failover_nodes[i], ninfo->configfile);
		i++;
	}

	/* Add the new failover domain */
	node = xmlNewNode(NULL, BAD_CAST "failoverdomain");
	xmlSetProp(node, BAD_CAST "name", BAD_CAST ninfo->name);
	_xmlSetIntProp(node, "ordered", ninfo->ordered);
	_xmlSetIntProp(node, "restricted", ninfo->restricted);

	i = 0;
	while (ninfo->failover_nodes[i]) {
		xmlNode *fnode = xmlNewNode(NULL, BAD_CAST "failoverdomainnode");
		xmlSetProp(fnode, BAD_CAST "name", BAD_CAST ninfo->failover_nodes[i]);
		_xmlSetIntProp(fnode, "priority", i + 1);
		xmlAddChild(node, fnode);
		i++;
	}
	xmlAddChild(fdomains, node);
}

static xmlDoc *open_configfile(struct option_info *ninfo)
{
	xmlDoc *doc;

	/* Init libxml */
	xmlInitParser();
	LIBXML_TEST_VERSION;

	if (!ninfo->configfile)
		ninfo->configfile = DEFAULT_CONFIG_DIR "/" DEFAULT_CONFIG_FILE;
	if (!ninfo->outputfile)
		ninfo->outputfile = ninfo->configfile;

	/* Load XML document */
	doc = xmlParseFile(ninfo->configfile);
	if (doc == NULL)
		die("Error: unable to parse requested configuration file\n");

	return doc;

}

static void del_clusternode(xmlNode *root_element, struct option_info *ninfo)
{
	xmlNode *clusternodes;
	xmlNode *oldnode;

	clusternodes = findnode(root_element, "clusternodes");
	if (!clusternodes)
	{
		fprintf(stderr, "Can't find \"clusternodes\" in %s\n", ninfo->configfile);
		exit(1);
	}

	oldnode = find_node(clusternodes, ninfo->name);
	if (!oldnode)
	{
		fprintf(stderr, "node %s does not exist in %s\n", ninfo->name, ninfo->configfile);
		exit(1);
	}

	xmlUnlinkNode(oldnode);
}

static void del_clusterservice(xmlNode *root_element, struct option_info *ninfo)
{
	xmlNode *rm;
	xmlNode *oldnode;

	rm = findnode(root_element, "rm");
	if (!rm)
	{
		fprintf(stderr, "Can't find \"rm\" in %s\n", ninfo->configfile);
		exit(1);
	}

	oldnode = find_service(rm, ninfo->name);
	if (!oldnode)
	{
		fprintf(stderr, "service %s does not exist in %s\n", ninfo->name, ninfo->configfile);
		exit(1);
	}

	xmlUnlinkNode(oldnode);
}

static void del_clusterscript(xmlNode *root_element, struct option_info *ninfo)
{
	xmlNode *rm, *rs;
	xmlNode *node;

	rm = findnode(root_element, "rm");
	if (!rm)
	{
		fprintf(stderr, "Can't find \"rm\" in %s\n", ninfo->configfile);
		exit(1);
	}

	rs = findnode(rm, "resources");
	if (!rs)
	{
		fprintf(stderr, "Can't find \"resources\" in %s\n", ninfo->configfile);
		exit(1);
	}

	/* Check that not used */
	node = find_script_ref(rm, ninfo->name);
	if (node)
	{
		fprintf(stderr, "Script %s is referenced in service in %s,"
			" please remove reference first.\n", ninfo->name,
			ninfo->configfile);
		exit(1);
	}

	node = find_script_resource(rs, ninfo->name);
	if (!node)
	{
		fprintf(stderr, "Script %s does not exist in %s\n", ninfo->name, ninfo->configfile);
		exit(1);
	}

	xmlUnlinkNode(node);
}

static void del_clusterip(xmlNode *root_element, struct option_info *ninfo)
{
	xmlNode *rm, *rs;
	xmlNode *node;

	rm = findnode(root_element, "rm");
	if (!rm)
	{
		fprintf(stderr, "Can't find \"rm\" in %s\n", ninfo->configfile);
		exit(1);
	}

	rs = findnode(rm, "resources");
	if (!rs)
	{
		fprintf(stderr, "Can't find \"resources\" in %s\n", ninfo->configfile);
		exit(1);
	}

	/* Check that not used */
	node = find_ip_ref(rm, ninfo->name);
	if (node)
	{
		fprintf(stderr, "IP %s is referenced in service in %s,"
			" please remove reference first.\n", ninfo->name,
			ninfo->configfile);
		exit(1);
	}

	node = find_ip_resource(rs, ninfo->name);
	if (!node)
	{
		fprintf(stderr, "IP %s does not exist in %s\n", ninfo->name, ninfo->configfile);
		exit(1);
	}

	xmlUnlinkNode(node);
}

static void del_clusterfs(xmlNode *root_element, struct option_info *ninfo)
{
	xmlNode *rm, *rs;
	xmlNode *node;

	rm = findnode(root_element, "rm");
	if (!rm)
	{
		fprintf(stderr, "Can't find \"rm\" in %s\n", ninfo->configfile);
		exit(1);
	}

	rs = findnode(rm, "resources");
	if (!rs)
	{
		fprintf(stderr, "Can't find \"resources\" in %s\n", ninfo->configfile);
		exit(1);
	}

	/* Check that not used */
	node = find_fs_ref(rm, ninfo->name);
	if (node)
	{
		fprintf(stderr, "fs %s is referenced in service in %s,"
			" please remove reference first.\n", ninfo->name,
			ninfo->configfile);
		exit(1);
	}

	node = find_fs_resource(rs, ninfo->name);
	if (!node)
	{
		fprintf(stderr, "fs %s does not exist in %s\n", ninfo->name, ninfo->configfile);
		exit(1);
	}

	xmlUnlinkNode(node);
}

static void del_clusterfdomain(xmlNode *root_element, struct option_info *ninfo)
{
	xmlNode *rm, *fdomains;
	xmlNode *node;

	rm = findnode(root_element, "rm");
	if (!rm)
	{
		fprintf(stderr, "Can't find \"rm\" in %s\n", ninfo->configfile);
		exit(1);
	}

	fdomains = findnode(rm, "failoverdomains");
	if (!fdomains)
	{
		fprintf(stderr, "Can't find \"failoverdomains\" in %s\n", ninfo->configfile);
		exit(1);
	}

	/* Check that not used */
	node = find_fdomain_ref(rm, ninfo->name);
	if (node)
	{
		fprintf(stderr, "failover domain %s is referenced in service in %s,"
			" please remove reference first.\n", ninfo->name,
			ninfo->configfile);
		exit(1);
	}

	node = find_fdomain_resource(fdomains, ninfo->name);
	if (!node)
	{
		fprintf(stderr, "failover domain %s does not exist in %s\n", ninfo->name, ninfo->configfile);
		exit(1);
	}

	xmlUnlinkNode(node);
}

struct option addnode_options[] =
{
      { "votes", required_argument, NULL, 'v'},
      { "nodeid", required_argument, NULL, 'n'},
      { "altname", required_argument, NULL, 'a'},
      { "fence_type", required_argument, NULL, 'f'},
      { "outputfile", required_argument, NULL, 'o'},
      { "configfile", required_argument, NULL, 'c'},
      { NULL, 0, NULL, 0 },
};

struct option addfs_options[] =
{
      { "type", required_argument, NULL, 't'},
      { "options", required_argument, NULL, 'p'},
      { "outputfile", required_argument, NULL, 'o'},
      { "configfile", required_argument, NULL, 'c'},
      { "force_fsck", no_argument, NULL, 'k'},
      { "force_unmount", no_argument, NULL, 'u'},
      { "self_fence", no_argument, NULL, 's'},
      { NULL, 0, NULL, 0 },
};

struct option addfdomain_options[] =
{
      { "outputfile", required_argument, NULL, 'o'},
      { "configfile", required_argument, NULL, 'c'},
      { "ordered", no_argument, NULL, 'p'},
      { "restricted", no_argument, NULL, 'r'},
      { NULL, 0, NULL, 0 },
};

struct option commonw_options[] =
{
      { "outputfile", required_argument, NULL, 'o'},
      { "configfile", required_argument, NULL, 'c'},
      { NULL, 0, NULL, 0 },
};

struct option addnodeid_options[] =
{
      { "outputfile", required_argument, NULL, 'o'},
      { "configfile", required_argument, NULL, 'c'},
      { "multicast", required_argument, NULL, 'm'},
      { "nodeid", no_argument, NULL, 'n'},
      { "verbose", no_argument, NULL, 'v'},
      { NULL, 0, NULL, 0 },
};

struct option list_options[] =
{
      { "configfile", required_argument, NULL, 'c'},
      { "verbose", no_argument, NULL, 'v'},
      { NULL, 0, NULL, 0 },
};

struct option create_options[] =
{
      { "configfile", required_argument, NULL, 'c'},
      { "nodes", required_argument, NULL, 'n'},
      { "fence", required_argument, NULL, 'f'},
      { "verbose", no_argument, NULL, 'v'},
      { NULL, 0, NULL, 0 },
};

struct option addservice_options[] =
{
      { "autostart", required_argument, NULL, 'a'},
      { "domain", required_argument, NULL, 'd'},
      { "exclusive", required_argument, NULL, 'x'},
      { "recovery", required_argument, NULL, 'r'},
      { "fs", required_argument, NULL, 'f'},
      { "script", required_argument, NULL, 's'},
      { "ip", required_argument, NULL, 'i'},
      { "outputfile", required_argument, NULL, 'o'},
      { "configfile", required_argument, NULL, 'c'},
      { NULL, 0, NULL, 0 },
};

static int parse_commonw_options(int argc, char **argv,
	struct option_info *ninfo)
{
	int opt;

	memset(ninfo, 0, sizeof(*ninfo));

	while ( (opt = getopt_long(argc, argv, "o:c:h?", commonw_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'c':
			ninfo->configfile = strdup(optarg);
			break;

		case 'o':
			ninfo->outputfile = strdup(optarg);
			break;

		case '?':
		default:
			return 1;
		}
	}
	return 0;
}

static int next_nodeid(int startid, int *nodeids, int nodecount)
{
	int i;
	int nextid = startid;

retry:
	for (i=0; i<nodecount; i++)
	{
		if (nodeids[i] == nextid)
		{
			nextid++;
			goto retry;
		}
	}

	return nextid;
}

void add_nodeids(int argc, char **argv)
{
	struct option_info ninfo;
	unsigned char *nodenames[MAX_NODES];
	xmlDoc *doc;
	xmlNode *root_element;
	xmlNode *clusternodes;
	xmlNode *cur_node;
	xmlNode *mcast;
	int  verbose = 0;
	int  opt;
	int  i;
	int  nodenumbers[MAX_NODES];
	int  nodeidx;
	int  totalnodes;
	int  nextid;

	memset(nodenames, 0, sizeof(nodenames));
	memset(nodenumbers, 0, sizeof(nodenumbers));
	memset(&ninfo, 0, sizeof(ninfo));
	ninfo.nodeid = "1";

	while ( (opt = getopt_long(argc, argv, "n:o:c:m:vh?", addnodeid_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'n':
			validate_int_arg(opt, optarg);
			ninfo.nodeid = strdup(optarg);
			break;

		case 'c':
			ninfo.configfile = strdup(optarg);
			break;

		case 'o':
			ninfo.outputfile = strdup(optarg);
			break;

		case 'm':
			if (!valid_mcast_addr(optarg)) {
				fprintf(stderr, "%s is not a valid multicast address\n", optarg);
				return;
			}
			ninfo.mcast_addr = strdup(optarg);
			break;

		case 'v':
			verbose++;
			break;

		case '?':
		default:
			addnodeid_usage(argv[0]);
		}
	}

	doc = open_configfile(&ninfo);

	root_element = xmlDocGetRootElement(doc);

	increment_version(root_element);

	/* Warn if the cluster doesn't have a multicast address */
	mcast = find_multicast_addr(root_element);
	if (!mcast & !ninfo.mcast_addr) {
		fprintf(stderr, "\nWARNING: The cluster does not have a multicast address.\n");
		fprintf(stderr, "A default will be assigned a run-time which might not suit your installation\n\n");
	}

	if (ninfo.mcast_addr) {
		if (!mcast) {
			xmlNode *cman = xmlNewNode(NULL, BAD_CAST "cman");
			mcast = xmlNewNode(NULL, BAD_CAST "multicast");

			xmlAddChild(cman, mcast);
			xmlAddChild(root_element, cman);
		}
		xmlSetProp(mcast, BAD_CAST "addr", BAD_CAST ninfo.mcast_addr);
	}

	/* Get a list of nodes that /do/ have nodeids so we don't generate
	   any duplicates */
	nodeidx=0;
	clusternodes = findnode(root_element, "clusternodes");
	if (!clusternodes)
		die("Can't find \"clusternodes\" in %s\n", ninfo.configfile);


	for (cur_node = clusternodes->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE && strcmp((char *)cur_node->name, "clusternode") == 0)
		{
			xmlChar *name   = xmlGetProp(cur_node, BAD_CAST "name");
			xmlChar *nodeid = xmlGetProp(cur_node, BAD_CAST "nodeid");
			nodenames[nodeidx]  = name;
			if (nodeid)
				nodenumbers[nodeidx] = atoi((char*)nodeid);
			nodeidx++;
		}
	}
	totalnodes = nodeidx;

	/* Loop round nodes adding nodeIDs where they don't exist. */
	nextid = next_nodeid(atoi(ninfo.nodeid), nodenumbers, totalnodes);
	for (i=0; i<totalnodes; i++)
	{
		if (nodenumbers[i] == 0)
		{
			nodenumbers[i] = nextid;
			nextid = next_nodeid(nextid, nodenumbers, totalnodes);
			if (verbose)
				fprintf(stderr, "Node %s now has id %d\n", nodenames[i], nodenumbers[i]);
		}
	}

	/* Now write them into the tree */
	nodeidx = 0;
	for (cur_node = clusternodes->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE && strcmp((char *)cur_node->name, "clusternode") == 0)
		{
			char tmp[80];
			xmlChar *name = xmlGetProp(cur_node, BAD_CAST "name");

			assert(strcmp((char*)nodenames[nodeidx], (char*)name) == 0);

			sprintf(tmp, "%d", nodenumbers[nodeidx]);
			xmlSetProp(cur_node, BAD_CAST "nodeid", BAD_CAST tmp);
			nodeidx++;
		}
	}


	/* Write it out */
	save_file(doc, &ninfo);

	/* Shutdown libxml */
	xmlCleanupParser();
}

void add_node(int argc, char **argv)
{
	struct option_info ninfo;
	int opt;
	xmlDoc *doc;
	xmlNode *root_element;

	memset(&ninfo, 0, sizeof(ninfo));
	ninfo.votes = "1";

	while ( (opt = getopt_long(argc, argv, "v:n:a:f:o:c:h?", addnode_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'v':
			validate_int_arg(opt, optarg);
			ninfo.votes = optarg;
			break;

		case 'n':
			validate_int_arg(opt, optarg);
			ninfo.nodeid = optarg;
			break;

		case 'a':
			ninfo.altname = strdup(optarg);
			break;

		case 'f':
			ninfo.fence_type = strdup(optarg);
			break;

		case 'c':
			ninfo.configfile = strdup(optarg);
			break;

		case 'o':
			ninfo.outputfile = strdup(optarg);
			break;

		case '?':
		default:
			addnode_usage(argv[0]);
		}
	}

	/* Get node name parameter */
	if (optind < argc)
		ninfo.name = strdup(argv[optind]);
	else
		addnode_usage(argv[0]);

	doc = open_configfile(&ninfo);

	root_element = xmlDocGetRootElement(doc);

	increment_version(root_element);

	add_clusternode(root_element, &ninfo, argc, argv, optind);

	/* Write it out */
	save_file(doc, &ninfo);
	/* Shutdown libxml */
	xmlCleanupParser();

}

void del_node(int argc, char **argv)
{
	struct option_info ninfo;
	xmlDoc *doc;
	xmlNode *root_element;

	if (parse_commonw_options(argc, argv, &ninfo))
		delnode_usage(argv[0]);

	/* Get node name parameter */
	if (optind < argc)
		ninfo.name = strdup(argv[optind]);
	else
		delnode_usage(argv[0]);

	doc = open_configfile(&ninfo);

	root_element = xmlDocGetRootElement(doc);

	increment_version(root_element);

	if (!strcmp(argv[0], "delnode"))
		del_clusternode(root_element, &ninfo);
	else if (!strcmp(argv[0], "delservice"))
		del_clusterservice(root_element, &ninfo);
	else if (!strcmp(argv[0], "delscript"))
		del_clusterscript(root_element, &ninfo);
	else if (!strcmp(argv[0], "delip"))
		del_clusterip(root_element, &ninfo);
	else if (!strcmp(argv[0], "delfs"))
		del_clusterfs(root_element, &ninfo);
	else if (!strcmp(argv[0], "delfdomain"))
		del_clusterfdomain(root_element, &ninfo);

	/* Write it out */
	save_file(doc, &ninfo);
}

void list_nodes(int argc, char **argv)
{
	xmlNode *cur_node;
	xmlNode *root_element;
	xmlNode *clusternodes;
	xmlNode *fencenode = NULL;
	xmlDocPtr doc;
	xmlNode *mcast;
	struct option_info ninfo;
	int opt;
	int verbose = 0;

	memset(&ninfo, 0, sizeof(ninfo));

	while ( (opt = getopt_long(argc, argv, "c:vh?", list_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'c':
			ninfo.configfile = strdup(optarg);
			break;
		case 'v':
			verbose++;
			break;
		case '?':
		default:
			list_usage(argv[0]);
		}
	}
	doc = open_configfile(&ninfo);

	root_element = xmlDocGetRootElement(doc);


	printf("\nCluster name: %s, config_version: %s\n\n",
	       (char *)cluster_name(root_element),
	       (char *)find_version(root_element));

	clusternodes = findnode(root_element, "clusternodes");
	if (!clusternodes)
		die("Can't find \"clusternodes\" in %s\n", ninfo.configfile);

	mcast = find_multicast_addr(root_element);
	if (mcast)
		printf("Multicast address for cluster: %s\n\n", xmlGetProp(mcast, BAD_CAST "addr"));

	printf("Nodename                        Votes Nodeid Fencetype\n");
	for (cur_node = clusternodes->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE && strcmp((char *)cur_node->name, "clusternode") == 0)
		{
			xmlChar *name   = xmlGetProp(cur_node, BAD_CAST "name");
			xmlChar *votes  = xmlGetProp(cur_node, BAD_CAST "votes");
			xmlChar *nodeid = xmlGetProp(cur_node, BAD_CAST "nodeid");
			xmlChar *ftype  = get_fence_type(cur_node, &fencenode);

			if (!nodeid)
				nodeid=(unsigned char *)"0";
			if (!votes)
				votes = (unsigned char *)"1";

			printf("%-32s %3d  %3d    %s\n", name, atoi((char *)votes),
			       atoi((char *)nodeid),
			       ftype?ftype:(xmlChar *)"");
			if (verbose)
			{
				xmlNode *a = findnode(cur_node, "altname");
				if (a)
				{
					printf(" altname %s=%s", "name", xmlGetProp(a, BAD_CAST "name"));
					if (!print_properties(a, "","",""))
						printf("\n");
				}
				print_properties(cur_node, "  Node properties: ", "votes", "nodeid");
				print_properties(fencenode, "  Fence properties: ", "agent", "");
			}

		}
	}
}

void add_service(int argc, char **argv)
{
	struct option_info ninfo;
	int opt;
	xmlDoc *doc;
	xmlNode *root_element;

	memset(&ninfo, 0, sizeof(ninfo));
	ninfo.autostart = "1";
	ninfo.recovery = "relocate";

	while ( (opt = getopt_long(argc, argv, "a:d:x:r:f:o:c:s:i:h?", addservice_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'a':
			validate_int_arg(opt, optarg);
			ninfo.autostart = optarg;
			break;

		case 'd':
			ninfo.domain = strdup(optarg);
			break;

		case 'x':
			validate_int_arg(opt, optarg);
			ninfo.exclusive = optarg;
			break;

		case 'r':
			ninfo.recovery = strdup(optarg);
			break;

		case 'f':
			ninfo.fs = strdup(optarg);
			break;

		case 's':
			ninfo.script = strdup(optarg);
			break;

		case 'i':
			ninfo.ip_addr = strdup(optarg);
			break;

		case 'c':
			ninfo.configfile = strdup(optarg);
			break;

		case 'o':
			ninfo.outputfile = strdup(optarg);
			break;

		case '?':
		default:
			addservice_usage(argv[0]);
		}
	}

	/* Get service name parameter */
	if (optind < argc)
		ninfo.name = strdup(argv[optind]);
	else
		addservice_usage(argv[0]);


	doc = open_configfile(&ninfo);

	root_element = xmlDocGetRootElement(doc);

	increment_version(root_element);

	add_clusterservice(root_element, &ninfo, argc, argv, optind);

	/* Write it out */
	save_file(doc, &ninfo);
	/* Shutdown libxml */
	xmlCleanupParser();

}

void list_services(int argc, char **argv)
{
	xmlNode *cur_service;
	xmlNode *root_element;
	xmlNode *rm;
	xmlDocPtr doc;
	struct option_info ninfo;
	int opt;
	int verbose = 0;

	memset(&ninfo, 0, sizeof(ninfo));

	while ( (opt = getopt_long(argc, argv, "c:vh?", list_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'c':
			ninfo.configfile = strdup(optarg);
			break;
		case 'v':
			verbose++;
			break;
		case '?':
		default:
			list_usage(argv[0]);
		}
	}
	doc = open_configfile(&ninfo);

	root_element = xmlDocGetRootElement(doc);


	printf("\nCluster name: %s, config_version: %s\n\n",
	       (char *)cluster_name(root_element),
	       (char *)find_version(root_element));

	rm = findnode(root_element, "rm");
	if (!rm)
		die("Can't find \"rm\" in %s\n", ninfo.configfile);

	printf("Name                             Autostart Exclusive Recovery\n");
	for (cur_service = rm->children; cur_service;
			cur_service = cur_service->next)
	{
		xmlChar *name, *autostart, *exclusive, *recovery;

		if (!cur_service->type == XML_ELEMENT_NODE ||
			strcmp((char *)cur_service->name, "service") != 0)
			continue;

		name = xmlGetProp(cur_service, BAD_CAST "name");
		autostart = xmlGetProp(cur_service, BAD_CAST "autostart");
		exclusive = xmlGetProp(cur_service, BAD_CAST "exclusive");
		recovery = xmlGetProp(cur_service, BAD_CAST "recovery");

		if (!autostart)
			autostart = (unsigned char *)"0";
		if (!exclusive)
			exclusive = (unsigned char *)"0";
		if (!recovery)
			recovery = (unsigned char *)"-";

		printf("%-32s       %3d       %3d %s\n", name,
			atoi((char *)autostart), atoi((char *)exclusive),
			(char *)recovery);
	}
}

void add_script(int argc, char **argv)
{
	struct option_info ninfo;
	xmlDoc *doc;
	xmlNode *root_element;
	xmlNode *rm, *rs, *node;
	char *name;
	char *sc_file;

	if (parse_commonw_options(argc, argv, &ninfo))
		addscript_usage(argv[0]);

	if (argc - optind < 2)
		addscript_usage(argv[0]);

	doc = open_configfile(&ninfo);

	root_element = xmlDocGetRootElement(doc);

	increment_version(root_element);

	rm = findnode(root_element, "rm");
	if (!rm)
		die("Can't find \"rm\" %s\n", ninfo.configfile);

	rs = findnode(rm, "resources");
	if (!rs)
		die("Can't find \"resources\" %s\n", ninfo.configfile);

	/* First param is the script name - check it doesn't already exist */
	name = argv[optind++];
	if (find_script_resource(rs, name))
		die("Script %s already exists\n", name);
	sc_file = argv[optind++];

	/* Add it */
	node = xmlNewNode(NULL, BAD_CAST "script");
	xmlSetProp(node, BAD_CAST "file", BAD_CAST sc_file);
	xmlSetProp(node, BAD_CAST "name", BAD_CAST name);
	xmlAddChild(rs, node);

	/* Write it out */
	save_file(doc, &ninfo);

	/* Shutdown libxml */
	xmlCleanupParser();
}

void add_ip(int argc, char **argv)
{
	struct option_info ninfo;
	xmlDoc *doc;
	xmlNode *root_element;
	xmlNode *rm, *rs, *node;

	if (parse_commonw_options(argc, argv, &ninfo))
		addip_usage(argv[0]);

	if (optind < argc)
		ninfo.ip_addr = strdup(argv[optind]);
	else
		addip_usage(argv[0]);

	doc = open_configfile(&ninfo);

	root_element = xmlDocGetRootElement(doc);

	increment_version(root_element);

	rm = findnode(root_element, "rm");
	if (!rm)
		die("Can't find \"rm\" %s\n", ninfo.configfile);

	rs = findnode(rm, "resources");
	if (!rs)
		die("Can't find \"resources\" %s\n", ninfo.configfile);

	/* Check it doesn't already exist */
	if (find_ip_resource(rs, ninfo.ip_addr))
		die("IP %s already exists\n", ninfo.ip_addr);

	/* Add it */
	node = xmlNewNode(NULL, BAD_CAST "ip");
	xmlSetProp(node, BAD_CAST "address", BAD_CAST ninfo.ip_addr);
	xmlSetProp(node, BAD_CAST "monitor_link", BAD_CAST "1");
	xmlAddChild(rs, node);

	/* Write it out */
	save_file(doc, &ninfo);

	/* Shutdown libxml */
	xmlCleanupParser();
}

void add_fs(int argc, char **argv)
{
	struct option_info ninfo;
	xmlDoc *doc;
	xmlNode *root_element;
	int opt;

	memset(&ninfo, 0, sizeof(ninfo));

	while ( (opt = getopt_long(argc, argv, "t:p:o:c:h?kus", addfs_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 't':
			ninfo.type = strdup(optarg);
			break;

		case 'p':
			ninfo.options = strdup(optarg);
			break;

		case 'c':
			ninfo.configfile = strdup(optarg);
			break;

		case 'o':
			ninfo.outputfile = strdup(optarg);
			break;

		case 'k':
			ninfo.force_fsck = 1;
			break;

		case 'u':
			ninfo.force_unmount = 1;
			break;

		case 's':
			ninfo.self_fence = 1;
			break;

		case '?':
		default:
			addfs_usage(argv[0]);
		}
	}

	if (optind < argc - 2) {
		ninfo.name = strdup(argv[optind]);
		ninfo.device = strdup(argv[optind + 1]);
		ninfo.mountpoint = strdup(argv[optind + 2]);
	} else
		addfs_usage(argv[0]);

	if (!ninfo.type)
		ninfo.type = "ext3";

	doc = open_configfile(&ninfo);

	root_element = xmlDocGetRootElement(doc);

	increment_version(root_element);

	add_clusterfs(root_element, &ninfo, argc, argv, optind);

	/* Write it out */
	save_file(doc, &ninfo);
	/* Shutdown libxml */
	xmlCleanupParser();
}

void add_fdomain(int argc, char **argv)
{
	struct option_info ninfo;
	xmlDoc *doc;
	xmlNode *root_element;
	int opt, i;

	memset(&ninfo, 0, sizeof(ninfo));

	while ( (opt = getopt_long(argc, argv, "pro:c:h?", addfdomain_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'p':
			ninfo.ordered = 1;
			break;

		case 'r':
			ninfo.restricted = 1;
			break;

		case 'c':
			ninfo.configfile = strdup(optarg);
			break;

		case 'o':
			ninfo.outputfile = strdup(optarg);
			break;

		case '?':
		default:
			addfdomain_usage(argv[0]);
		}
	}

	if (optind < argc - 1) {
		ninfo.name = strdup(argv[optind]);
		ninfo.failover_nodes = (const char **)malloc(sizeof(char *) * (argc - optind));
		for (i = 0; i < argc - optind - 1; i++)
			ninfo.failover_nodes[i] = strdup(argv[i + optind + 1]);
		ninfo.failover_nodes[i] = NULL;
	} else
		addfdomain_usage(argv[0]);

	doc = open_configfile(&ninfo);

	root_element = xmlDocGetRootElement(doc);

	increment_version(root_element);

	add_clusterfdomain(root_element, &ninfo, argc, argv, optind);

	/* Write it out */
	save_file(doc, &ninfo);
	/* Shutdown libxml */
	xmlCleanupParser();
}

void create_skeleton(int argc, char **argv)
{
	char *fencename = NULL;
	char *clustername;
	struct option_info ninfo;
	struct stat st;
	FILE *outfile;
	int i;
	int twonode = 0;
	int numnodes=0;
	int opt;

	memset(&ninfo, 0, sizeof(ninfo));

	while ( (opt = getopt_long(argc, argv, "c:2hn:f:?", create_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'c':
			ninfo.outputfile = strdup(optarg);
			break;

		case '2':
			twonode = 1;
			numnodes = 2;
			break;
		case 'n':
			numnodes = atoi(optarg);
			break;
		case 'f':
			fencename = strdup(optarg);
			break;

		case '?':
		default:
			create_usage(argv[0]);
		}
	}
	if (!ninfo.outputfile)
		ninfo.outputfile = DEFAULT_CONFIG_DIR "/" DEFAULT_CONFIG_FILE;
	ninfo.configfile = "-";

	if (argc - optind < 1)
		create_usage(argv[0]);

	clustername = argv[optind];

	if (stat(ninfo.outputfile, &st) == 0)
		die("%s already exists", ninfo.outputfile);

	/* Init libxml */
	outfile = fopen(ninfo.outputfile, "w+");
	if (!outfile) {
		perror(" Can't open output file");
		return;
	}

	fprintf(outfile, "<?xml version=\"1.0\"?>\n");
	fprintf(outfile, "<cluster name=\"%s\" config_version=\"1\">\n", clustername);
	fprintf(outfile, "\n");
	if (twonode) {
		fprintf(outfile, "  <cman two_node=\"1\" expected_votes=\"1\"/>\n");
	}

	fprintf(outfile, "  <clusternodes>\n");
	for (i=1; i <= numnodes; i++) {
		fprintf(outfile, "    <clusternode name=\"NEEDNAME-%02d\" votes=\"1\" nodeid=\"%d\">\n", i, i);
		fprintf(outfile, "      <fence>\n");
		fprintf(outfile, "        <method name=\"single\">\n");
		if (fencename) {
			fprintf(outfile, "          <device name=\"fence1\" ADDARGS/>\n");
		}
		fprintf(outfile, "        </method>\n");
		fprintf(outfile, "      </fence>\n");
		fprintf(outfile, "    </clusternode>\n");
	}
	fprintf(outfile, "  </clusternodes>\n");
	fprintf(outfile, "\n");
	fprintf(outfile, "  <fencedevices>\n");
	if (fencename) {
		fprintf(outfile, "    <fencedevice name=\"fence1\" agent=\"%s\" ADDARGS/>\n", fencename);
	}
	fprintf(outfile, "  </fencedevices>\n");
	fprintf(outfile, "\n");
	fprintf(outfile, "  <rm>\n");
	fprintf(outfile, "    <failoverdomains/>\n");
	fprintf(outfile, "    <resources/>\n");
	fprintf(outfile, "  </rm>\n");
	fprintf(outfile, "</cluster>\n");

	fclose(outfile);
}

void add_fence(int argc, char **argv)
{
	xmlNode *root_element;
	xmlNode *fencedevices;
	xmlNode *fencenode = NULL;
	xmlDocPtr doc;
	char *fencename;
	char *agentname;
	struct option_info ninfo;

	if (parse_commonw_options(argc, argv, &ninfo))
		addfence_usage(argv[0]);

	if (argc - optind < 2)
		addfence_usage(argv[0]);

	doc = open_configfile(&ninfo);
	root_element = xmlDocGetRootElement(doc);

	increment_version(root_element);

	fencedevices = findnode(root_element, "fencedevices");
	if (!fencedevices)
		die("Can't find \"fencedevices\" %s\n", ninfo.configfile);

	/* First param is the fence name - check it doesn't already exist */
	fencename = argv[optind++];

	if (valid_fence_type(root_element, fencename))
		die("fence type %s already exists\n", fencename);

	agentname = argv[optind++];

	/* Add it */
	fencenode = xmlNewNode(NULL, BAD_CAST "fencedevice");
	xmlSetProp(fencenode, BAD_CAST "name", BAD_CAST fencename);
	xmlSetProp(fencenode, BAD_CAST "agent", BAD_CAST agentname);

	/* Add name=value options */
	add_fence_args(fencenode, argc, argv, optind);

	xmlAddChild(fencedevices, fencenode);

	save_file(doc, &ninfo);
}

void del_fence(int argc, char **argv)
{
	xmlNode *root_element;
	xmlNode *fencedevices;
	xmlNode *fencenode;
	xmlDocPtr doc;
	char *fencename;
	struct option_info ninfo;

	if (parse_commonw_options(argc, argv, &ninfo))
		delfence_usage(argv[0]);

	if (argc - optind < 1)
		delfence_usage(argv[0]);

	fencename = argv[optind];

	doc = open_configfile(&ninfo);
	root_element = xmlDocGetRootElement(doc);
	increment_version(root_element);

	fencedevices = findnode(root_element, "fencedevices");
	if (!fencedevices)
		die("Can't find \"fencedevices\" in %s\n", ninfo.configfile);

	fencenode = valid_fence_type(root_element, fencename);
	if (!fencenode)
		die("fence type %s does not exist\n", fencename);

	xmlUnlinkNode(fencenode);

	save_file(doc, &ninfo);
}

void list_fences(int argc, char **argv)
{
	xmlNode *cur_node;
	xmlNode *root_element;
	xmlNode *fencedevices;
	xmlDocPtr doc;
	struct option_info ninfo;
	int opt;
	int verbose=0;

	memset(&ninfo, 0, sizeof(ninfo));

	while ( (opt = getopt_long(argc, argv, "c:hv?", list_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'c':
			ninfo.configfile = strdup(optarg);
			break;
		case 'v':
			verbose++;
			break;
		case '?':
		default:
			list_usage(argv[0]);
		}
	}
	doc = open_configfile(&ninfo);
	root_element = xmlDocGetRootElement(doc);

	fencedevices = findnode(root_element, "fencedevices");
	if (!fencedevices)
		die("Can't find \"fencedevices\" in %s\n", ninfo.configfile);


	printf("Name             Agent\n");
	for (cur_node = fencedevices->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE && strcmp((char *)cur_node->name, "fencedevice") == 0)
		{
			xmlChar *name  = xmlGetProp(cur_node, BAD_CAST "name");
			xmlChar *agent = xmlGetProp(cur_node, BAD_CAST "agent");

			printf("%-16s %s\n", name, agent);
			if (verbose)
				print_properties(cur_node, "  Properties: ", "agent", "");
		}
	}
}

void list_scripts(int argc, char **argv)
{
	xmlNode *cur_node;
	xmlNode *root_element;
	xmlNode *rm, *rs;
	xmlDocPtr doc;
	struct option_info ninfo;
	int opt;
	int verbose=0;

	memset(&ninfo, 0, sizeof(ninfo));

	while ( (opt = getopt_long(argc, argv, "c:hv?", list_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'c':
			ninfo.configfile = strdup(optarg);
			break;
		case 'v':
			verbose++;
			break;
		case '?':
		default:
			list_usage(argv[0]);
		}
	}
	doc = open_configfile(&ninfo);
	root_element = xmlDocGetRootElement(doc);

	rm = findnode(root_element, "rm");
	if (!rm)
		die("Can't find \"rm\" in %s\n", ninfo.configfile);

	rs = findnode(rm, "resources");
	if (!rs)
		die("Can't find \"resources\" in %s\n", ninfo.configfile);

	printf("Name             Path\n");
	for (cur_node = rs->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE && strcmp((char *)cur_node->name, "script") == 0)
		{
			xmlChar *name  = xmlGetProp(cur_node, BAD_CAST "name");
			xmlChar *path = xmlGetProp(cur_node, BAD_CAST "file");

			printf("%-16s %s\n", name, path);
		}
	}
}

void list_ips(int argc, char **argv)
{
	xmlNode *cur_node;
	xmlNode *root_element;
	xmlNode *rm, *rs;
	xmlDocPtr doc;
	struct option_info ninfo;
	int opt;
	int verbose=0;

	memset(&ninfo, 0, sizeof(ninfo));

	while ( (opt = getopt_long(argc, argv, "c:hv?", list_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'c':
			ninfo.configfile = strdup(optarg);
			break;
		case 'v':
			verbose++;
			break;
		case '?':
		default:
			list_usage(argv[0]);
		}
	}
	doc = open_configfile(&ninfo);
	root_element = xmlDocGetRootElement(doc);

	rm = findnode(root_element, "rm");
	if (!rm)
		die("Can't find \"rm\" in %s\n", ninfo.configfile);

	rs = findnode(rm, "resources");
	if (!rs)
		die("Can't find \"resources\" in %s\n", ninfo.configfile);

	printf("IP\n");
	for (cur_node = rs->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE &&
			strcmp((char *)cur_node->name, "ip") == 0)
		{
			xmlChar *ip  = xmlGetProp(cur_node, BAD_CAST "address");

			printf("%s\n", ip);
		}
	}
}

void list_fs(int argc, char **argv)
{
	xmlNode *cur_node;
	xmlNode *root_element;
	xmlNode *rm, *rs;
	xmlDocPtr doc;
	struct option_info ninfo;
	int opt;
	int verbose=0;

	memset(&ninfo, 0, sizeof(ninfo));

	while ( (opt = getopt_long(argc, argv, "c:hv?", list_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'c':
			ninfo.configfile = strdup(optarg);
			break;
		case 'v':
			verbose++;
			break;
		case '?':
		default:
			list_usage(argv[0]);
		}
	}
	doc = open_configfile(&ninfo);
	root_element = xmlDocGetRootElement(doc);

	rm = findnode(root_element, "rm");
	if (!rm)
		die("Can't find \"rm\" in %s\n", ninfo.configfile);

	rs = findnode(rm, "resources");
	if (!rs)
		die("Can't find \"resources\" in %s\n", ninfo.configfile);

	printf("Name             Type  FUS Device              Mountpoint\n");
	for (cur_node = rs->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE &&
			strcmp((char *)cur_node->name, "fs") == 0)
		{
			xmlChar *name  = xmlGetProp(cur_node, BAD_CAST "name");
			xmlChar *type  = xmlGetProp(cur_node, BAD_CAST "fstype");
			xmlChar *force_fsck  = xmlGetProp(cur_node,
						BAD_CAST "force_fsck");
			xmlChar *force_unmount  = xmlGetProp(cur_node,
						BAD_CAST "force_unmount");
			xmlChar *self_fence  = xmlGetProp(cur_node,
						BAD_CAST "self_fence");
			xmlChar *device  = xmlGetProp(cur_node,
						BAD_CAST "device");
			xmlChar *mnt  = xmlGetProp(cur_node,
						BAD_CAST "mountpoint");

			char f, u, s;
			INT_TO_CHAR(f, force_fsck)
			INT_TO_CHAR(u, force_unmount)
			INT_TO_CHAR(s, self_fence)
			printf("%-16.16s %-5.5s %c%c%c %-19.19s %s\n", name, type, f, u,
					s, device, mnt);
		}
	}
}

void list_fdomains(int argc, char **argv)
{
	xmlNode *cur_node, *fnode;
	xmlNode *root_element;
	xmlNode *rm, *fdomains;
	xmlDocPtr doc;
	struct option_info ninfo;
	int opt;
	int verbose=0;

	memset(&ninfo, 0, sizeof(ninfo));

	while ( (opt = getopt_long(argc, argv, "c:hv?", list_options, NULL)) != EOF)
	{
		switch(opt)
		{
		case 'c':
			ninfo.configfile = strdup(optarg);
			break;
		case 'v':
			verbose++;
			break;
		case '?':
		default:
			list_usage(argv[0]);
		}
	}
	doc = open_configfile(&ninfo);
	root_element = xmlDocGetRootElement(doc);

	rm = findnode(root_element, "rm");
	if (!rm)
		die("Can't find \"rm\" in %s\n", ninfo.configfile);

	fdomains = findnode(rm, "failoverdomains");
	if (!fdomains)
		die("Can't find \"failoverdomains\" in %s\n", ninfo.configfile);

	printf("Name             OR Nodes\n");
	for (cur_node = fdomains->children; cur_node; cur_node = cur_node->next)
	{
		if (cur_node->type == XML_ELEMENT_NODE &&
			strcmp((char *)cur_node->name, "failoverdomain") == 0)
		{
			xmlChar *name  = xmlGetProp(cur_node, BAD_CAST "name");
			xmlChar *ordered  = xmlGetProp(cur_node, BAD_CAST "ordered");
			xmlChar *restricted = xmlGetProp(cur_node, BAD_CAST "restricted");
			char o, r;
			int first_node = 1;

			INT_TO_CHAR(o, ordered)
			INT_TO_CHAR(r, restricted)
			printf("%-16.16s %c%c ", name, o, r);
			for (fnode = cur_node->children; fnode; fnode = fnode->next)
				if (fnode->type == XML_ELEMENT_NODE &&
					strcmp((char *)fnode->name, "failoverdomainnode") == 0)
				{
					xmlChar *fname  = xmlGetProp(fnode, BAD_CAST "name");
					if (first_node) {
						printf("%s", fname);
						first_node = 0;
					} else
						printf(",%s", fname);
				}
			printf("\n");
		}
	}
}
