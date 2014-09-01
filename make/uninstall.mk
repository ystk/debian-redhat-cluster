uninstall:
ifdef LIBDIRT
	${UNINSTALL} ${LIBDIRT} ${libdir}
endif
ifdef LIBSYMT
	${UNINSTALL} ${LIBSYMT} ${libdir}
endif
ifdef INCDIRT
	${UNINSTALL} ${INCDIRT} ${incdir}
endif
ifdef FORCESBINT
	${UNINSTALL} ${FORCESBINT} ${DESTDIR}/sbin
endif
ifdef SBINDIRT
	${UNINSTALL} ${SBINDIRT} ${sbindir}
endif
ifdef SBINSYMT
	${UNINSTALL} ${SBINSYMT} ${sbindir}
endif
ifdef LCRSOT
	${UNINSTALL} ${LCRSOT} ${libexecdir}/lcrso
endif
ifdef INITDT
	${UNINSTALL} ${INITDT} ${initddir}
endif
ifdef UDEVT
	${UNINSTALL} ${UDEVT} ${DESTDIR}/lib/udev/rules.d
endif
ifdef DOCS
	${UNINSTALL} ${DOCS} ${docdir}
endif
ifdef LOGRORATED
	${UNINSTALL} ${LOGRORATED} ${logrotatedir}
endif
ifdef NOTIFYD
	${UNINSTALL} ${NOTIFYD} ${notifyddir}
endif
ifdef PKGCONF
	${UNINSTALL} ${PKGCONF} ${pkgconfigdir}
endif
ifdef SHAREDIRTEX
	${UNINSTALL} ${SHAREDIRTEX} ${sharedir}
endif
ifdef SHAREDIRT
	${UNINSTALL} ${SHAREDIRT} ${sharedir}
endif
ifdef SHAREDIRSYMT
	${UNINSTALL} ${SHAREDIRSYMT} ${sharedir}
endif
ifdef RELAXNGDIRT
	${UNINSTALL} ${RELAXNGDIRT} ${sharedir}/relaxng
endif
ifdef MANTARGET
	set -e; \
	for i in ${MANTARGET}; do \
		p=`echo $$i | sed -e 's#.*\.##g'`; \
		${UNINSTALL} $$i ${mandir}/man$$p; \
	done
endif
