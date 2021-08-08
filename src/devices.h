
/*
 * devices.h
 * 
 * Rewritten by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1998-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 */

#if !defined(_WANT_DEVICE_TYPES) && !defined(_WANT_DEVICE_CMDS)
#ifdef PHYSTYPE_MODEM
#include "modem.h"
#endif
#ifdef PHYSTYPE_NG_SOCKET
#include "ng.h"
#endif
#ifdef PHYSTYPE_TCP
#include "tcp.h"
#endif
#ifdef PHYSTYPE_UDP
#include "udp.h"
#endif
#ifdef PHYSTYPE_PPTP
#include "pptp.h"
#endif
#ifdef PHYSTYPE_L2TP
#include "l2tp.h"
#endif
#ifdef PHYSTYPE_PPPOE
#include "pppoe.h"
#endif
#endif

#ifdef _WANT_DEVICE_CMDS
#ifdef PHYSTYPE_MODEM
    { "modem ...",			"Modem specific stuff",
	CMD_SUBMENU, AdmitDev, 2, ModemSetCmds },
#endif
#ifdef PHYSTYPE_NG_SOCKET
    { "ng ...",				"Netgraph specific stuff",
	CMD_SUBMENU, AdmitDev, 2, NgSetCmds },
#endif
#ifdef PHYSTYPE_TCP
    { "tcp ...",			"TCP specific stuff",
	CMD_SUBMENU, AdmitDev, 2, TcpSetCmds },
#endif
#ifdef PHYSTYPE_UDP
    { "udp ...",			"UDP specific stuff",
	CMD_SUBMENU, AdmitDev, 2, UdpSetCmds },
#endif
#ifdef PHYSTYPE_PPTP
    { "pptp ...",			"PPTP specific stuff",
	CMD_SUBMENU, AdmitDev, 2, PptpSetCmds },
#endif
#ifdef PHYSTYPE_L2TP
    { "l2tp ...",			"L2TP specific stuff",
	CMD_SUBMENU, AdmitDev, 2, L2tpSetCmds },
#endif
#ifdef PHYSTYPE_PPPOE
    { "pppoe ...",			"PPPoE specific stuff",
	CMD_SUBMENU, AdmitDev, 2, PppoeSetCmds },
#endif
#endif

#ifdef _WANT_DEVICE_TYPES
#ifdef PHYSTYPE_MODEM
    &gModemPhysType,
#endif
#ifdef PHYSTYPE_NG_SOCKET
    &gNgPhysType,
#endif
#ifdef PHYSTYPE_TCP
    &gTcpPhysType,
#endif
#ifdef PHYSTYPE_UDP
    &gUdpPhysType,
#endif
#ifdef PHYSTYPE_PPTP
    &gPptpPhysType,
#endif
#ifdef PHYSTYPE_L2TP
    &gL2tpPhysType,
#endif
#ifdef PHYSTYPE_PPPOE
    &gPppoePhysType,
#endif
#endif

