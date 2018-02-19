MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = hal-imp io memory processing pipeline-imp util#Add more here... 

noinst_LTLIBRARIES = libxdpd_driver_gnu_linux_dpdk_ng_src.la

libxdpd_driver_gnu_linux_dpdk_ng_src_la_SOURCES = bg_taskmanager.cc config.cc config_rss.c
libxdpd_driver_gnu_linux_dpdk_ng_src_la_LIBADD = \
		hal-imp/libxdpd_driver_gnu_linux_dpdk_ng_src_hal_imp.la\
		io/libxdpd_driver_gnu_linux_dpdk_ng_src_io.la\
		pipeline-imp/libxdpd_driver_gnu_linux_dpdk_ng_src_pipeline_imp.la\
		memory/libxdpd_driver_gnu_linux_dpdk_ng_src_memory.la\
		processing/libxdpd_driver_gnu_linux_dpdk_ng_src_processing.la\
		util/libxdpd_driver_gnu_linux_dpdk_ng_src_util.la\
		-lrte_eal\
		-lrte_eventdev\
		-lrte_ethdev\
		-lrte_hash\
		-lrte_kni\
		-lrte_lpm\
		-lrte_mbuf\
		-lrte_mempool\
		-lrte_power\
		-lrte_ring\
		-lrte_timer\
		-lrte_eal\
		-lrte_kvargs\
		-ldl
