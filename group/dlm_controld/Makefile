include ../../make/defines.mk

TARGET = dlm_controld
SBINDIRT = ${TARGET}

all: depends ${TARGET}

include $(OBJDIR)/make/cobj.mk
include $(OBJDIR)/make/clean.mk
include $(OBJDIR)/make/install.mk
include $(OBJDIR)/make/uninstall.mk

OBJS=	action.o \
	cpg.o \
	crc.o \
	deadlock.o \
	main.o \
	netlink.o \
	plock.o \
	group.o \
	config.o \
	member_cman.o \
	logging.o

CFLAGS += -I${ccsincdir} -I${cmanincdir} -I${logtincdir} 
CFLAGS += -I${dlmincdir} -I${dlmcontrolincdir}
CFLAGS += -I${corosyncincdir} -I${openaisincdir}
CFLAGS += -I${fencedincdir}
CFLAGS += -I${KERNEL_SRC}/include/
CFLAGS += -I$(S)/../lib/ -I$(S)/../include/
CFLAGS += -I${incdir}

LDFLAGS += -L${logtlibdir} -llogthread
LDFLAGS += -L${ccslibdir} -L${cmanlibdir} -lccs -lcman
LDFLAGS += -L${dlmlibdir} -L${fencedlibdir} -ldlm -lfenced
LDFLAGS += -L${openaislibdir} -lSaCkpt
LDFLAGS += -L${corosynclibdir} -lcpg -lconfdb
LDFLAGS += -lpthread
LDFLAGS += -L../lib -lgroup
LDFLAGS += -L${libdir}

LDDEPS += ../lib/libgroup.a

${TARGET}: ${OBJS} ${LDDEPS}
	$(CC) -o $@ $^ $(LDFLAGS)

depends:
	$(MAKE) -C ../lib all

clean: generalclean

-include $(OBJS:.o=.d)
