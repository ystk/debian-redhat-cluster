TARGET= mkfs.gfs

FORCESBINT= $(TARGET)

all: depends ${TARGET}

include ../../make/defines.mk
include $(OBJDIR)/make/cobj.mk
include $(OBJDIR)/make/clean.mk
include $(OBJDIR)/make/install.mk
include $(OBJDIR)/make/uninstall.mk

OBJS=	device_geometry.o \
	fs_geometry.o \
	locking.o \
	main.o \
	ondisk.o \
	structures.o

CFLAGS += -DHELPER_PROGRAM -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE
CFLAGS += -I${gfskincdir}
CFLAGS += -I$(S)/../include -I$(S)/../libgfs
CFLAGS += -I${incdir}

LDFLAGS += -L../libgfs -lgfs
LDFLAGS += -L${libdir}

LDDEPS += ../libgfs/libgfs.a

${TARGET}: ${OBJS} ${LDDEPS}
	$(CC) -o $@ $^ $(LDFLAGS)

depends:
	$(MAKE) -C ../libgfs all

clean: generalclean

-include $(OBJS:.o=.d)
