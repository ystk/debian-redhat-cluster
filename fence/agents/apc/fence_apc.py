#!/usr/bin/python

#####
##
## The Following Agent Has Been Tested On:
##
##  Model       Firmware
## +---------------------------------------------+
##  AP7951	AOS v2.7.0, PDU APP v2.7.3
##  AP7941      AOS v3.5.7, PDU APP v3.5.6
##  AP9606	AOS v2.5.4, PDU APP v2.7.3
##
## @note: ssh is very slow on AP79XX devices protocol (1) and 
##        cipher (des/blowfish) have to be defined
#####

import sys, re, pexpect, exceptions
sys.path.append("@FENCEAGENTSLIBDIR@")
from fencing import *

#BEGIN_VERSION_GENERATION
RELEASE_VERSION="New APC Agent - test release on steroids"
REDHAT_COPYRIGHT=""
BUILD_DATE="March, 2008"
#END_VERSION_GENERATION

def get_power_status(conn, options):
	exp_result = 0
	outlets = {}
	try:
		conn.send("1\r\n")
		conn.log_expect(options, options["-c"], int(options["-Y"]))

		version = 0
		admin = 0
		switch = 0;

		if (None != re.compile('.* MasterSwitch plus.*', re.IGNORECASE | re.S).match(conn.before)):
			switch = 1;
			if (None != re.compile('.* MasterSwitch plus 2', re.IGNORECASE | re.S).match(conn.before)):
				if (0 == options.has_key("-s")):
					fail_usage("Failed: You have to enter physical switch number")
			else:
				if (0 == options.has_key("-s")):
					options["-s"] = "1"

		if (None == re.compile('.*Outlet Management.*', re.IGNORECASE | re.S).match(conn.before)):
			version = 2
		else:
			version = 3

		if (None == re.compile('.*Outlet Control/Configuration.*', re.IGNORECASE | re.S).match(conn.before)):
			admin = 0
		else:
			admin = 1

		if switch == 0:
			if version == 2:
				if admin == 0:
					conn.send("2\r\n")
				else:
					conn.send("3\r\n")
			else:
				conn.send("2\r\n")
				conn.log_expect(options, options["-c"], int(options["-Y"]))
				conn.send("1\r\n")
		else:
			conn.send(options["-s"]+"\r\n")
			
		while True:
			exp_result = conn.log_expect(options, [ options["-c"],  "Press <ENTER>" ], int(options["-Y"]))
			lines = conn.before.split("\n");
			show_re = re.compile('^\s*(\d+)- (.*?)\s+(ON|OFF)\s*')
			for x in lines:
				res = show_re.search(x)
				if (res != None):
					outlets[res.group(1)] = (res.group(2), res.group(3))
			conn.send("\r\n")
			if exp_result == 0:
				break
		conn.send(chr(03))		
		conn.log_expect(options, "- Logout", int(options["-Y"]))
		conn.log_expect(options, options["-c"], int(options["-Y"]))
	except pexpect.EOF:
		fail(EC_CONNECTION_LOST)
	except pexpect.TIMEOUT:
		fail(EC_TIMED_OUT)

	if ["list", "monitor"].count(options["-o"]) == 1:
		return outlets
	else:
		try:
			(_, status) = outlets[options["-n"]]
			return status.lower().strip()
		except KeyError:
			fail(EC_STATUS)

def set_power_status(conn, options):
	action = {
		'on' : "1",
		'off': "2"
	}[options["-o"]]

	try:
		conn.send("1\r\n")
		conn.log_expect(options, options["-c"], int(options["-Y"]))

		version = 0
		admin2 = 0
		admin3 = 0
		switch = 0

		if (None != re.compile('.* MasterSwitch plus.*', re.IGNORECASE | re.S).match(conn.before)):
			switch = 1;
			## MasterSwitch has different schema for on/off actions
			action = {
				'on' : "1",
				'off': "3"
			}[options["-o"]]
			if (None != re.compile('.* MasterSwitch plus 2', re.IGNORECASE | re.S).match(conn.before)):
				if (0 == options.has_key("-s")):
					fail_usage("Failed: You have to enter physical switch number")
			else:
				if (0 == options.has_key("-s")):
					options["-s"] = 1

		if (None == re.compile('.*Outlet Management.*', re.IGNORECASE | re.S).match(conn.before)):
			version = 2
		else:
			version = 3

		if (None == re.compile('.*Outlet Control/Configuration.*', re.IGNORECASE | re.S).match(conn.before)):
			admin2 = 0
		else:
			admin2 = 1

		if switch == 0:
			if version == 2:
				if admin2 == 0:
					conn.send("2\r\n")
				else:
					conn.send("3\r\n")
			else:
				conn.send("2\r\n")
				conn.log_expect(options, options["-c"], int(options["-Y"]))
				if (None == re.compile('.*2- Outlet Restriction.*', re.IGNORECASE | re.S).match(conn.before)):
					admin3 = 0
				else:
					admin3 = 1
				conn.send("1\r\n")
		else:
			conn.send(options["-s"] + "\r\n")

		while 1 == conn.log_expect(options, [ options["-c"],  "Press <ENTER>" ], int(options["-Y"])):
			conn.send("\r\n")
		conn.send(options["-n"]+"\r\n")
		conn.log_expect(options, options["-c"], int(options["-Y"]))

		if switch == 0:
			if admin2 == 1:
				conn.send("1\r\n")
				conn.log_expect(options, options["-c"], int(options["-Y"]))
			if admin3 == 1:
				conn.send("1\r\n")
				conn.log_expect(options, options["-c"], int(options["-Y"]))
		else:
			conn.send("1\r\n")
			conn.log_expect(options, options["-c"], int(options["-Y"]))
			
		conn.send(action+"\r\n")
		conn.log_expect(options, "Enter 'YES' to continue or <ENTER> to cancel :", int(options["-Y"]))
		conn.send("YES\r\n")
		conn.log_expect(options, "Press <ENTER> to continue...", int(options["-Y"]))
		conn.send("\r\n")
		conn.log_expect(options, options["-c"], int(options["-Y"]))
		conn.send(chr(03))
		conn.log_expect(options, "- Logout", int(options["-Y"]))
		conn.log_expect(options, options["-c"], int(options["-Y"]))
	except pexpect.EOF:
		fail(EC_CONNECTION_LOST)
	except pexpect.TIMEOUT:
		fail(EC_TIMED_OUT)

def main():
	device_opt = [  "help", "version", "agent", "quiet", "verbose", "debug",
			"action", "ipaddr", "login", "passwd", "passwd_script",
			"secure", "port", "identity_file", "switch", "test", "separator",
			"inet4_only", "inet6_only", "ipport",
			"power_timeout", "shell_timeout", "login_timeout", "power_wait" ]

	atexit.register(atexit_handler)

	options = check_input(device_opt, process_input(device_opt))

	## 
	## Fence agent specific defaults
	#####
	options["ssh_options"] = "-1 -c blowfish"

	if 0 == options.has_key("-c"):
		options["-c"] = "\n>"

	docs = { }
	docs["shortdesc"] = "Fence agent for APC over telnet/ssh"
	docs["longdesc"] = "fence_apc is an I/O Fencing agent \
which can be used with the APC network power switch. It logs into device \
via telnet/ssh  and reboots a specified outlet. Lengthy telnet/ssh connections \
should be avoided while a GFS cluster  is  running  because  the  connection \
will block any necessary fencing actions."
	docs["vendorurl"] = "http://www.apc.com"
	show_docs(options, docs)

	## Support for -n [switch]:[plug] notation that was used before
	if (options.has_key("-n") == 1) and (-1 != options["-n"].find(":")):
		(switch, plug) = options["-n"].split(":", 1)
		options["-s"] = switch;
		options["-n"] = plug;

	##
	## Operate the fencing device
	####
	conn = fence_login(options)
	result = fence_action(conn, options, set_power_status, get_power_status, get_power_status)

	##
	## Logout from system
	##
	## In some special unspecified cases it is possible that 
	## connection will be closed before we run close(). This is not 
	## a problem because everything is checked before.
	######
	try:
		conn.sendline("4")
		conn.close()
	except exceptions.OSError:
		pass
	except pexpect.ExceptionPexpect:
		pass
	
	sys.exit(result)

if __name__ == "__main__":
	main()
