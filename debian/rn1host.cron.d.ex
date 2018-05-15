#
# Regular cron jobs for the rn1host package
#
0 4	* * *	root	[ -x /usr/bin/rn1host_maintenance ] && /usr/bin/rn1host_maintenance
