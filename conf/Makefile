# $Id: Makefile.conf 2169 2014-04-14 12:41:31Z dmitryluhtionov $

PREFIX?=	/usr/local
BINDIR?=	/etc/mpd5

BINOWN!=	/usr/bin/id -n -u
BINGRP!=	/usr/bin/id -n -g

MPD_SAMPLES+=	mpd.conf.sample
MPD_SAMPLES+=	mpd.secret.sample
MPD_SAMPLES+=	mpd.script.sample
NOOBJ=
NO_OBJ=

all clean cleandir depend:

afterinstall:
	[ -d ${DESTDIR}${PREFIX}${BINDIR} ] || mkdir ${DESTDIR}${PREFIX}${BINDIR}
	${INSTALL} -c -o ${BINOWN} -g ${BINGRP} -m ${SHAREMODE} ${MPD_SAMPLES} \
		${DESTDIR}${PREFIX}${BINDIR}

.include <bsd.prog.mk>
