TARGET = fence_tool

SBINDIRT=$(TARGET)

all: depends ${TARGET}

include ../../make/defines.mk
include $(OBJDIR)/make/cobj.mk
include $(OBJDIR)/make/clean.mk
include $(OBJDIR)/make/install.mk
include $(OBJDIR)/make/uninstall.mk

OBJS=fence_tool.o

CFLAGS += -D_FILE_OFFSET_BITS=64
CFLAGS += -I${ccsincdir} -I${cmanincdir} -I${fencedincdir}
CFLAGS += -I$(S)/../include
CFLAGS += -I${incdir} 

LDFLAGS += -L${ccslibdir} -L${cmanlibdir} -lccs -lcman
LDFLAGS += -L${fencedlibdir} -lfenced
LDFLAGS += -L${libdir}

${TARGET}: ${OBJS}
	$(CC) -o $@ $^ $(LDFLAGS)

depends:
	$(MAKE) -C ../libfenced

clean: generalclean

-include $(OBJS:.o=.d)
