TARGET= libaislock

include ../../make/defines.mk
include $(OBJDIR)/make/libs.mk
include $(OBJDIR)/make/cobj.mk
include $(OBJDIR)/make/clean.mk
include $(OBJDIR)/make/install.mk
include $(OBJDIR)/make/uninstall.mk

CFLAGS += -fPIC
CFLAGS += -I${dlmincdir}
CFLAGS += -I${incdir}

LDFLAGS += -L${dlmlibdir} -ldlm
LDFLAGS += -lpthread
