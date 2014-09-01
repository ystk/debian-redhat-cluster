include ../../../make/defines.mk

TARGET1 = ccs_config_dump
TARGET2 = ccs_config_validate
TARGET3 = ccs_update_schema
TARGET4 = cluster.rng

SBINDIRT = $(TARGET1) $(TARGET2) $(TARGET3)
SHAREDIRSYMT = $(TARGET4)
RELAXNGDIRT = cluster.rng.in.head cluster.rng.in.tail

all: $(TARGET1) $(TARGET2) $(TARGET3) $(TARGET4)

include $(OBJDIR)/make/cobj.mk
include $(OBJDIR)/make/clean.mk
include $(OBJDIR)/make/install.mk
include $(OBJDIR)/make/uninstall.mk

OBJS =	ccs_config_dump.o

CFLAGS += -I${corosyncincdir}
CFLAGS += -I${incdir}

LDFLAGS += -L${corosynclibdir} -lconfdb
LDFLAGS += -L${libdir}

${TARGET1}: ${OBJS}
	$(CC) -o $@ $^ $(LDFLAGS)

${TARGET2}: $(S)/${TARGET2}.in
	cat $(S)/$(TARGET2).in | sed \
		-e 's#@SBINDIR@#${sbindir}#g' \
		-e 's#@SHAREDIR@#${sharedir}#g' \
		-e 's#@VERSION@#${RELEASE_VERSION}#g' \
	> $(TARGET2)

${TARGET3}: $(S)/${TARGET3}.in
	cat $(S)/$(TARGET3).in | sed \
		-e 's#@SBINDIR@#${sbindir}#g' \
		-e 's#@SHAREDIR@#${sharedir}#g' \
		-e 's#@VERSION@#${RELEASE_VERSION}#g' \
	> $(TARGET3)

${TARGET4}:
	ln -sf /var/lib/cluster/cluster.rng ${TARGET4}

clean: generalclean
