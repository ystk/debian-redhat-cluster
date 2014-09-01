include make/defines.mk

REALSUBDIRS = common config cman dlm fence/libfenced group \
	      fence rgmanager bindings doc contrib

SUBDIRS = $(filter-out \
	  $(if ${without_common},common) \
	  $(if ${without_config},config) \
	  $(if ${without_cman},cman) \
	  $(if ${without_dlm},dlm) \
	  $(if ${without_fence},fence/libfenced) \
	  $(if ${without_group},group) \
	  $(if ${without_fence},fence) \
	  $(if ${without_rgmanager},rgmanager) \
	  $(if ${without_bindings},bindings) \
	  , $(REALSUBDIRS))

all: ${SUBDIRS}

${SUBDIRS}:
	${MAKE} -C $@ all

# Dependencies

common:
config:
cman: common config
dlm: config
fence/libfenced:
group: cman dlm fence/libfenced
fence: group
rgmanager: cman dlm
bindings: cman
contrib: dlm

oldconfig:
	@if [ -f $(OBJDIR)/.configure.sh ]; then \
		sh $(OBJDIR)/.configure.sh; \
	else \
		echo "Unable to find old configuration data"; \
	fi

install:
	set -e && for i in ${SUBDIRS}; do ${MAKE} -C $$i $@; done
	install -d ${notifyddir}
	install -d ${logdir}
	install -d ${DESTDIR}/var/lib/cluster
	install -d ${DESTDIR}/var/run/cluster

uninstall:
	set -e && for i in ${SUBDIRS}; do ${MAKE} -C $$i $@; done
	rmdir ${notifyddir} || :;
	rmdir ${logdir} || :;
	rmdir ${DESTDIR}/var/lib/cluster || :;
	rmdir ${DESTDIR}/var/run/cluster || :;

clean:
	set -e && for i in ${REALSUBDIRS}; do \
		contrib_code=1 \
		${MAKE} -C $$i $@;\
	done

distclean: clean
	rm -f make/defines.mk
	rm -f .configure.sh
	rm -f *tar.gz
	rm -rf build

.PHONY: ${REALSUBDIRS}
