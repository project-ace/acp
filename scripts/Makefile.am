#
# Copyright (c) 2012-2014 ACE Project
# $COPYRIGHT$
#

bin_SCRIPTS = acprun
acprun: acprun.in
	( cd $(top_builddir) && ./config.status --file=${subdir}/acprun:${subdir}/acprun.in )
CLEANFILES = acprun
EXTRA_DIST = acprun.in
