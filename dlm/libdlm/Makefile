TARGET= libdlm

LIBDIRT=$(TARGET).a \
	$(TARGET)_lt.a \
	$(TARGET).so.${SOMAJOR}.${SOMINOR} \
	$(TARGET)_lt.so.${SOMAJOR}.${SOMINOR}

LIBSYMT=$(TARGET).so \
	$(TARGET)_lt.so \
	$(TARGET).so.$(SOMAJOR) \
	$(TARGET)_lt.so.$(SOMAJOR)

INCDIRT=$(TARGET).h

UDEVT=51-dlm.rules

PKGCONF=$(TARGET).pc $(TARGET)_lt.pc

include ../../make/defines.mk

SHAREDLIB=$(TARGET).so.${SOMAJOR}.${SOMINOR} $(TARGET)_lt.so.${SOMAJOR}.${SOMINOR}
STATICLIB=$(TARGET).a $(TARGET)_lt.a

all: $(STATICLIB) $(SHAREDLIB) $(PKGCONF)

include $(OBJDIR)/make/cobj.mk
include $(OBJDIR)/make/clean.mk
include $(OBJDIR)/make/install.mk
include $(OBJDIR)/make/uninstall.mk

CFLAGS += -fPIC
CFLAGS += -I$(S)
CFLAGS += -I${incdir}
CFLAGS += -I$(KERNEL_SRC)/include

REENT_CFLAGS += -D_REENTRANT

LDFLAGS += -L${libdir}

PTHREAD_LDFLAGS += -lpthread 

$(TARGET).a: $(TARGET).o
	${AR} cru $@ $^
	${RANLIB} $@

$(TARGET)_lt.a: $(TARGET)_lt.o
	${AR} cru $@ $^
	${RANLIB} $@

$(TARGET).so.${SOMAJOR}.${SOMINOR}: $(TARGET).o
	$(CC) -shared -o $@ -Wl,-soname=$(TARGET).so.$(SOMAJOR) $< $(PTHREAD_LDFLAGS) $(LDFLAGS)
	ln -sf $(TARGET).so.$(SOMAJOR).$(SOMINOR) $(TARGET).so
	ln -sf $(TARGET).so.$(SOMAJOR).$(SOMINOR) $(TARGET).so.$(SOMAJOR)

$(TARGET)_lt.so.${SOMAJOR}.${SOMINOR}: $(TARGET)_lt.o
	$(CC) -shared -o $@ -Wl,-soname=$(TARGET)_lt.so.$(SOMAJOR) $< $(LDFLAGS)
	ln -sf $(TARGET)_lt.so.$(SOMAJOR).$(SOMINOR) $(TARGET)_lt.so
	ln -sf $(TARGET)_lt.so.$(SOMAJOR).$(SOMINOR) $(TARGET)_lt.so.$(SOMAJOR)

$(TARGET).pc: $(S)/$(TARGET).pc.in
	cat $(S)/$(TARGET).pc.in | \
	sed \
		-e 's#@PREFIX@#${prefix}#g' \
		-e 's#@LIBDIR@#${libdir}#g' \
		-e 's#@INCDIR@#${incdir}#g' \
		-e 's#@VERSION@#${RELEASE_VERSION}#g' \
	> $@

$(TARGET)_lt.pc: $(S)/$(TARGET)_lt.pc.in
	cat $(S)/$(TARGET)_lt.pc.in | \
	sed \
		-e 's#@PREFIX@#${prefix}#g' \
		-e 's#@LIBDIR@#${libdir}#g' \
		-e 's#@INCDIR@#${incdir}#g' \
		-e 's#@VERSION@#${RELEASE_VERSION}#g' \
	> $@

clean: generalclean

-include $(TARGET).d
-include $(TARGET)_lt.d
