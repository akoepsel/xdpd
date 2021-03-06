AC_INIT(xdpd - DPDK GNU/Linux driver, m4_esyscmd_s(cat VERSION), xdpd@bisdn.de, xdpd_gnu_linux_dpdk_fm)

AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])
AC_CONFIG_MACRO_DIR([m4])
AC_GNU_SOURCE

AC_PROG_INSTALL
AC_PROG_CC
AC_PROG_CXX
AC_PROG_LD
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AUTOCONF_ENV="SHELL=/bin/sh"
LT_INIT
link_all_deplibs=yes
link_all_deplibs_CXX=yes
AC_ENABLE_STATIC
LIBTOOL="$LIBTOOL --preserve-dup-deps"
AC_SUBST(LIBTOOL)dnl

# default flags
CFLAGS="-fgnu89-inline -Werror -Wall $CFLAGS"
CXXFLAGS="-Werror -Wall $CXXFLAGS"
LDFLAGS=" -Wl,-export-dynamic"

AC_DEFINE(__STDC_FORMAT_MACROS)
AC_DEFINE(__STDC_LIMIT_MACROS)

#Driver environment. Must go first
m4_include([../../../../config/driver.m4])

#ROFL
m4_include([../../../../config/rofl.m4])

# DPDK
WITH_DPDK="yes"
m4_include([../../../../config/dpdk.m4])

# Debug
m4_include([../../../../config/debug.m4])

# Check for profiling mode
m4_include([../../../../config/profiling.m4])

# TODO: remove this once the patch is accepted
#AC_CHECK_LIB(rte_kni, rte_kni_init, [AC_DEFINE(DPDK_PATCHED_KNI, 1)], [echo "";AC_WARN([DPDK does not contain the KNI memzone pool patch! NF ports will still work, but the number of NF ports that can be allocated could be limited and fail. You can safely ignore this warning if you are not going to be using NF ports.]); sleep 3], [-lrte_eal -lethdev -lrte_hash -lrte_kni -lrte_lpm -lrte_malloc -lrte_mbuf -lrte_mempool -lrte_power -lrte_ring -lrte_timer -lrte_eal -lrte_kvargs -ldl -lpthread -lrt])

AC_CONFIG_FILES([
	Makefile
	src/Makefile
	src/hal-imp/Makefile
	src/hal-imp/openflow/openflow1x/Makefile
	src/io/Makefile
	src/io/packet_classifiers/Makefile
	src/io/packet_classifiers/c_types_pktclassifier/Makefile
	src/processing/Makefile
	src/pipeline-imp/Makefile
	src/util/Makefile
])
AC_OUTPUT
