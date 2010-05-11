#!/bin/bash

#
# Dummy OCF script for resource group
#

# Grab nfs lock tricks if available
export NFS_TRICKS=1
if [ -f "$(dirname $0)/svclib_nfslock" ]; then
	. $(dirname $0)/svclib_nfslock
	NFS_TRICKS=0
fi

meta_data()
{
    cat <<EOT
<?xml version="1.0"?>
<resource-agent version="rgmanager 2.0" name="service">
    <version>1.0</version>

    <longdesc lang="en">
        This defines a collection of resources, known as a resource
        group or cluster service.
    </longdesc>
    <shortdesc lang="en">
        Defines a services.
    </shortdesc>

    <parameters>
        <parameter name="name" unique="1" required="1" primary="1">
            <longdesc lang="en">
                This is the name of the resource group.
            </longdesc>
            <shortdesc lang="en">
                Name
            </shortdesc>
            <content type="string"/>
        </parameter>
    
        <parameter name="domain" reconfig="1">
            <longdesc lang="en">
                Fail over domains define lists of cluster members
                to try in the event that a resource group fails.
            </longdesc>
            <shortdesc lang="en">
                Fail over Domain
            </shortdesc>
            <content type="string"/>
        </parameter>

        <parameter name="autostart" reconfig="1">
            <longdesc lang="en">
	    	If set to yes, this resource group will automatically be started
		after the cluster forms a quorum.  If set to no, this resource
		group will start in the 'disabled' state after the cluster forms
		a quorum.
            </longdesc>
            <shortdesc lang="en">
	    	Automatic start after quorum formation
            </shortdesc>
            <content type="boolean" default="1"/>
        </parameter>

        <parameter name="hardrecovery" reconfig="1">
            <longdesc lang="en">
	    	If set to yes, the last owner will reboot if this resource
		group fails to stop cleanly, thus allowing the resource
		group to fail over to another node.  Use with caution; a
		badly-behaved resource could cause the entire cluster to
		reboot.  This should never be enabled if the automatic
		start feature is used.
            </longdesc>
            <shortdesc lang="en">
	    	Reboot if stop phase fails
            </shortdesc>
            <content type="boolean" default="0"/>
        </parameter>

        <parameter name="exclusive" reconfig="1">
            <longdesc lang="en">
	    	If set, this resource group will only relocate to
		nodes which have no other resource groups running in the
		event of a failure.  If no empty nodes are available,
		this resource group will not be restarted after a failure.
		Additionally, resource groups will not automatically
		relocate to the node running this resource group.  This
		option can be overridden by manual start and/or relocate
		operations.
            </longdesc>
            <shortdesc lang="en">
	        Exclusive resource group
            </shortdesc>
            <content type="boolean" default="0"/>
        </parameter>

	<parameter name="nfslock">
	    <longdesc lang="en">
	    	Enable NFS lock workarounds.  When used with a compatible
		HA-callout program like clunfslock, this could be used
		to provide NFS lock failover, but at significant cost to
		other services on the machine.  This requires a compatible
		version of nfs-utils and manual configuration of rpc.statd;
		see 'man rpc.statd' to see if your version supports
		the -H parameter.
	    </longdesc>
	    <shortdesc lang="en">
	        Enable NFS lock workarounds
	    </shortdesc>
	    <content type="boolean" default="0"/>
	</parameter>

	<parameter name="nfs_client_cache">
            <longdesc lang="en">
	   	On systems with large numbers of exports, a performance
		problem in the exportfs command can cause inordinately long
		status check times for services with lots of mounted
		NFS clients.  This occurs because exportfs does DNS queries
		on all clients in the export list.

		Setting this option to '1' will enable caching of the export
		list returned from the exportfs command on a per-service
		basis.  The cache will last for 30 seconds before expiring
		instead of being generated each time an nfsclient resource
		is called.
            </longdesc>
            <shortdesc lang="en">
	    	Enable exportfs list caching
            </shortdesc>
	    <content type="integer" default="0"/>
	</parameter>

                
        <parameter name="recovery" reconfig="1">
            <longdesc lang="en">
	        This currently has three possible options: "restart" tries
		to restart failed parts of this resource group locally before
		attempting to relocate (default); "relocate" does not bother
		trying to restart the service locally; "disable" disables
		the resource group if any component fails.  Note that
		any resource with a valid "recover" operation which can be
		recovered without a restart will be.
            </longdesc>
            <shortdesc lang="en">
	    	Failure recovery policy
            </shortdesc>
            <content type="string" default="restart"/>
        </parameter>

        <parameter name="depend">
            <longdesc lang="en">
		Top-level service this depends on, in "service:name" format.
            </longdesc>
            <shortdesc lang="en">
		Service dependency; will not start without the specified
		service running.
            </shortdesc>
            <content type="string"/>
        </parameter>

        <parameter name="depend_mode">
            <longdesc lang="en">
	    	Dependency mode
            </longdesc>
            <shortdesc lang="en">
		Service dependency mode.
		hard - This service is stopped/started if its dependency
		       is stopped/started
		soft - This service only depends on the other service for
		       initial startip.  If the other service stops, this
		       service is not stopped.
            </shortdesc>
            <content type="string" default="hard"/>
        </parameter>

        <parameter name="max_restarts">
            <longdesc lang="en">
	    	Maximum restarts for this service.
            </longdesc>
            <shortdesc lang="en">
	    	Maximum restarts for this service.
            </shortdesc>
            <content type="string" default="0"/>
        </parameter>

        <parameter name="restart_expire_time">
            <longdesc lang="en">
	    	Restart expiration time
            </longdesc>
            <shortdesc lang="en">
	    	Restart expiration time.  A restart is forgotten
		after this time.  When combined with the max_restarts
		option, this lets administrators specify a threshold
		for when to fail over services.  If max_restarts
		is exceeded in this given expiration time, the service
		is relocated instead of restarted again.
            </shortdesc>
            <content type="string" default="0"/>
        </parameter>

	<parameter name="priority">
	    <longdesc lang="en">
		Priority for the service.  In a failover scenario, this
		indicates the ordering of the service (1 is processed
		first, 2 is processed second, etc.).  This overrides the
		order presented in cluster.conf.  This option only has
		an effect if central processing within rgmanager is turned
		on.
	    </longdesc>
	    <shortdesc lang="en">
		Service priority
	    </shortdesc>
	    <content type="integer" default="0"/>
	</parameter>

    </parameters>

    <actions>
        <action name="start" timeout="5"/>
        <action name="stop" timeout="5"/>
	
	<!-- No-ops.  Groups are abstract resource types. 
        <action name="status" timeout="5" interval="1h"/>
        <action name="monitor" timeout="5" interval="1h"/>
 -->

        <action name="reconfig" timeout="5"/>
        <action name="recover" timeout="5"/>
        <action name="reload" timeout="5"/>
        <action name="meta-data" timeout="5"/>
        <action name="validate-all" timeout="5"/>
    </actions>
    
    <special tag="rgmanager">
        <attributes maxinstances="1"/>
        <child type="lvm" start="1" stop="9"/>
        <child type="fs" start="2" stop="8"/>
        <child type="clusterfs" start="3" stop="7"/>
        <child type="netfs" start="4" stop="6"/>
	<child type="nfsexport" start="5" stop="5"/>

	<child type="nfsclient" start="6" stop="4"/>

        <child type="ip" start="7" stop="2"/>
        <child type="smb" start="8" stop="3"/>
        <child type="script" start="9" stop="1"/>
    </special>
</resource-agent>
EOT
}


#
# A Resource group is abstract, but the OCF RA API doesn't allow for abstract
# resources, so here it is.
#
case $1 in
	start)
		#
		# XXX If this is set, we kill lockd.  If there is no
		# child IP address, then clients will NOT get the reclaim
		# notification.
		#
		if [ $NFS_TRICKS -eq 0 ]; then
			if [ "$OCF_RESKEY_nfslock" = "yes" ] || \
	   		   [ "$OCF_RESKEY_nfslock" = "1" ]; then
				pkill -KILL -x lockd
			fi
		fi
		exit 0
		;;
	stop)
		exit 0
		;;
	recover|restart)
		exit 0
		;;
	status|monitor)
		exit 0
		;;
	reload)
		exit 0
		;;
	meta-data)
		meta_data
		exit 0
		;;
	validate-all)
		exit 0
		;;
	reconfig)
		exit 0
		;;
	*)
		exit 0
		;;
esac
