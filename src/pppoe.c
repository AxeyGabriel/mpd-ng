
/*
 * pppoe.c
 *
 * Written by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1995-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 */

#include "ppp.h"
#include "pppoe.h"
#include "ngfunc.h"
#include "log.h"
#include "util.h"

#include <paths.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netgraph/ng_message.h>
#include <netgraph/ng_pppoe.h>
#include <netgraph/ng_ether.h>
#include <netgraph/ng_tee.h>
#include <netgraph.h>

#include <sys/param.h>
#include <sys/linker.h>

/*
 * DEFINITIONS
 */

#define PPPOE_MTU		1492	/* allow room for PPPoE overhead */
#define PPPOE_MRU		1492

#define PPPOE_CONNECT_TIMEOUT	9

#define ETHER_DEFAULT_HOOK	NG_ETHER_HOOK_ORPHAN

#ifndef SMALL_SYSTEM
#define PPPOE_MAXPARENTIFS	4096
#else
#define PPPOE_MAXPARENTIFS	32
#endif

#define MAX_PATH		64	/* XXX should be NG_PATHSIZ */
#define MAX_SESSION		64	/* max length of PPPoE session name */

#ifndef PTT_MAX_PAYL			/* PPP-Max-Payload (RFC4638) */
#if BYTE_ORDER == BIG_ENDIAN
#define PTT_MAX_PAYL		(0x0120)
#else
#define PTT_MAX_PAYL		(0x2001)
#endif
#endif

/* https://tools.ietf.org/html/rfc4937 */
#if BYTE_ORDER == BIG_ENDIAN
#define MPD_PTT_CREDITS		(0x0106)
#define MPD_PTT_METRICS		(0x0107)
#define MPD_PTT_SEQ_NUMBER	(0x0108)
#define MPD_PTT_HURL		(0x0111)
#define MPD_PTT_MOTM		(0x0112)
#define MPD_PTT_IP_ROUTE_ADD	(0x0121)
#else
#define MPD_PTT_CREDITS		(0x0601)
#define MPD_PTT_METRICS		(0x0701)
#define MPD_PTT_SEQ_NUMBER	(0x0801)
#define MPD_PTT_HURL		(0x1101)
#define MPD_PTT_MOTM		(0x1201)
#define MPD_PTT_IP_ROUTE_ADD	(0x2101)
#endif

/* Per link private info */
struct pppoeinfo {
	char		iface[IFNAMSIZ];	/* PPPoE interface name */
	char		path[MAX_PATH];		/* PPPoE node path */
	char		hook[NG_HOOKSIZ];	/* hook on that node */
	char		session[MAX_SESSION];	/* session name */
	char		acname[PPPOE_SERVICE_NAME_SIZE];	/* AC name */
	uint16_t	max_payload;		/* PPP-Max-Payload (RFC4638) */
	int		mac_format;		/* MAC address format */
	u_char		peeraddr[6];		/* Peer MAC address */
	char		real_session[MAX_SESSION]; /* real session name */
	char		agent_cid[64];		/* Agent Circuit ID */
	char		agent_rid[64];		/* Agent Remote ID */
	u_char		incoming;		/* incoming vs. outgoing */
	u_char		opened;			/* PPPoE opened by phys */
	u_char		mp_reply;		/* PPP-Max-Payload reply from server */
	struct optinfo	options;
	struct PppoeIf  *PIf;			/* pointer on parent ng_pppoe info */
	struct PppoeList *list;
	struct pppTimer	connectTimer;		/* connection timeout timer */
};
typedef struct pppoeinfo	*PppoeInfo;

static u_char gNgEtherLoaded = FALSE;

/* Set menu options */
enum {
	SET_IFACE,
	SET_SESSION,
	SET_ACNAME,
	SET_MAX_PAYLOAD,
	SET_MAC_FORMAT
};

/* MAC format options */
enum {
    MAC_UNFORMATTED = 0,
    MAC_UNIX_LIKE,
    MAC_CISCO_LIKE,
    MAC_IETF
};

/*
   Invariants:
   ----------

   PPPOE_DOWN
	- ng_pppoe(4) node does not exist
	- pe->csock == -1
	- Connect timeout timer is not running

   PPPOE_CONNECTING
	- ng_pppoe(4) node exists and is connected to ether and ppp nodes
	- pe->csock != -1
	- Listening for control messages rec'd on pe->csock
	- Connect timeout timer is running
	- NGM_PPPOE_CONNECT has been sent to the ng_pppoe(4) node, and
	    no response has been received yet

   PPPOE_UP
	- ng_pppoe(4) node exists and is connected to ether and ppp nodes
	- pe->csock != -1
	- Listening for control messages rec'd on pe->csock
	- Connect timeout timer is not running
	- NGM_PPPOE_CONNECT has been sent to the ng_pppoe(4) node, and
	    a NGM_PPPOE_SUCCESS has been received
*/

/*
 * INTERNAL FUNCTIONS
 */

static int	PppoeInit(Link l);
static int	PppoeInst(Link l, Link lt);
static void	PppoeOpen(Link l);
static void	PppoeClose(Link l);
static void	PppoeShutdown(Link l);
static int	PppoePeerMacAddr(Link l, void *buf, size_t buf_len);
static int	PppoePeerIface(Link l, void *buf, size_t buf_len);
static int	PppoeCallingNum(Link l, void *buf, size_t buf_len);
static int	PppoeCalledNum(Link l, void *buf, size_t buf_len);
static int	PppoeSelfName(Link l, void *buf, size_t buf_len);
static int	PppoePeerName(Link l, void *buf, size_t buf_len);
static u_short	PppoeGetMtu(Link l, int conf);
static u_short	PppoeGetMru(Link l, int conf);
static void	PppoeCtrlReadEvent(int type, void *arg);
static void	PppoeConnectTimeout(void *arg);
static void	PppoeStat(Context ctx);
static int	PppoeSetCommand(Context ctx, int ac, const char *const av[], const void *arg);
static int	PppoeOriginated(Link l);
static int	PppoeIsSync(Link l);
static void	PppoeGetNode(Link l);
static void	PppoeReleaseNode(Link l);
static int 	PppoeListen(Link l);
static int 	PppoeUnListen(Link l);
static void	PppoeNodeUpdate(Link l);
static void	PppoeListenEvent(int type, void *arg);
static int 	CreatePppoeNode(struct PppoeIf *PIf, const char *iface, const char *path, const char *hook);

static void	PppoeDoClose(Link l);

/*
 * GLOBAL VARIABLES
 */

const struct phystype gPppoePhysType = {
    .name		= "pppoe",
    .descr		= "PPP over Ethernet",
    .mtu		= PPPOE_MTU,
    .mru		= PPPOE_MRU,
    .tmpl		= 1,
    .init		= PppoeInit,
    .inst		= PppoeInst,
    .open		= PppoeOpen,
    .close		= PppoeClose,
    .update		= PppoeNodeUpdate,
    .shutdown		= PppoeShutdown,
    .showstat		= PppoeStat,
    .originate		= PppoeOriginated,
    .issync		= PppoeIsSync,
    .peeraddr		= PppoePeerMacAddr,
    .peermacaddr	= PppoePeerMacAddr,
    .peeriface		= PppoePeerIface,
    .callingnum		= PppoeCallingNum,
    .callednum		= PppoeCalledNum,
    .selfname		= PppoeSelfName,
    .peername		= PppoePeerName,
    .getmtu		= PppoeGetMtu,
    .getmru		= PppoeGetMru
};

const struct cmdtab PppoeSetCmds[] = {
      { "iface {name}",		"Set ethernet interface to use",
	  PppoeSetCommand, NULL, 2, (void *)SET_IFACE },
      { "service {name}",	"Set PPPoE session name",
	  PppoeSetCommand, NULL, 2, (void *)SET_SESSION },
      { "acname {name}",	"Set PPPoE access concentrator name",
	  PppoeSetCommand, NULL, 2, (void *)SET_ACNAME },
#ifdef NGM_PPPOE_SETMAXP_COOKIE
      { "max-payload {size}",	"Set PPP-Max-Payload tag",
	  PppoeSetCommand, NULL, 2, (void *)SET_MAX_PAYLOAD },
#endif
      { "mac-format {format}",	"Set RADIUS attribute 31 MAC format",
	  PppoeSetCommand, NULL, 2, (void *)SET_MAC_FORMAT },
      { NULL, NULL, NULL, NULL, 0, NULL }
};

/* 
 * INTERNAL VARIABLES 
 */

struct PppoeList {
    char	session[MAX_SESSION];
    int		refs;
    SLIST_ENTRY(PppoeList)	next;
};

struct PppoeIf {
    char	ifnodepath[MAX_PATH];
    ng_ID_t	node_id;		/* pppoe node id */
    int		refs;
    int		csock;                  /* netgraph Control socket */
    int		dsock;                  /* netgraph Data socket */
    EventRef	ctrlEvent;		/* listen for ctrl messages */
    EventRef	dataEvent;		/* listen for data messages */
    SLIST_HEAD(, PppoeList) list;
};

static struct PppoeIf PppoeIfs[PPPOE_MAXPARENTIFS];

struct tagname {
    int		tag;
    const char	*name;
};

static const struct tagname tag2str[] = {
    { PTT_EOL, "End-Of-List" },
    { PTT_SRV_NAME, "Service-Name" },
    { PTT_AC_NAME, "AC-Name" },
    { PTT_HOST_UNIQ, "Host-Uniq" },
    { PTT_AC_COOKIE, "AC-Cookie" },
    { PTT_VENDOR, "Vendor-Specific" },
    { PTT_RELAY_SID, "Relay-Session-Id" },
    { PTT_MAX_PAYL, "PPP-Max-Payload" },
    { PTT_SRV_ERR, "Service-Name-Error" },
    { PTT_SYS_ERR, "AC-System-Error" },
    { PTT_GEN_ERR, "Generic-Error" },
    /* RFC 4937 */
    { MPD_PTT_CREDITS, "Credits" },
    { MPD_PTT_METRICS, "Metrics" },
    { MPD_PTT_SEQ_NUMBER, "Sequence Number" },
    { MPD_PTT_HURL, "HURL" },
    { MPD_PTT_MOTM, "MOTM" },
    { MPD_PTT_IP_ROUTE_ADD, "IP_Route_Add" },
    { 0, "UNKNOWN" }
};
#define NUM_TAG_NAMES	(sizeof(tag2str) / sizeof(*tag2str))


/*
 * PppoeInit()
 *
 * Initialize device-specific data in physical layer info
 */
static int
PppoeInit(Link l)
{
	PppoeInfo pe;

	/* Allocate private struct */
	pe = (PppoeInfo)(l->info = Malloc(MB_PHYS, sizeof(*pe)));
	pe->incoming = 0;
	pe->opened = 0;
	snprintf(pe->iface, sizeof(pe->iface), "undefined");
	snprintf(pe->path, sizeof(pe->path), "undefined:");
	snprintf(pe->hook, sizeof(pe->hook), "undefined");
	snprintf(pe->session, sizeof(pe->session), "*");
	memset(pe->peeraddr, 0x00, ETHER_ADDR_LEN);
	strlcpy(pe->real_session, pe->session, sizeof(pe->real_session));
	pe->agent_cid[0] = 0;
	pe->agent_rid[0] = 0;
	pe->PIf = NULL;
	pe->max_payload = 0;
	pe->mac_format = MAC_UNFORMATTED;
	pe->mp_reply = 0;

	/* Done */
	return(0);
}

/*
 * PppoeInst()
 *
 * Instantiate device
 */
static int
PppoeInst(Link l, Link lt)
{
	PppoeInfo pi;
	l->info = Mdup(MB_PHYS, lt->info, sizeof(struct pppoeinfo));
	pi = (PppoeInfo)l->info;
	if (pi->PIf)
	    pi->PIf->refs++;
	if (pi->list)
	    pi->list->refs++;

	/* Done */
	return(0);
}

/*
 * PppoeOpen()
 */
static void
PppoeOpen(Link l)
{
	PppoeInfo pe = (PppoeInfo)l->info;
	struct ngm_connect	cn;
	union {
	    u_char buf[sizeof(struct ngpppoe_init_data) + MAX_SESSION];
	    struct ngpppoe_init_data	poeid;
	} u;
	struct ngpppoe_init_data *const idata = &u.poeid;
	char path[NG_PATHSIZ];
	char session_hook[NG_HOOKSIZ];

	pe->opened=1;

	Disable(&l->conf.options, LINK_CONF_ACFCOMP);	/* RFC 2516 */
	Deny(&l->conf.options, LINK_CONF_ACFCOMP);	/* RFC 2516 */

	snprintf(session_hook, sizeof(session_hook), "mpd%d-%d", 
	    gPid, l->id);
	
	if (pe->incoming == 1) {
		Log(LG_PHYS2, ("[%s] PppoeOpen() on incoming call", l->name));

		/* Path to the ng_tee node */
		snprintf(path, sizeof(path), "[%x]:%s", 
		    pe->PIf->node_id, session_hook);

		/* Connect ng_tee(4) node to the ng_ppp(4) node. */
		memset(&cn, 0, sizeof(cn));
		if (!PhysGetUpperHook(l, cn.path, cn.peerhook)) {
		    Log(LG_PHYS, ("[%s] PPPoE: can't get upper hook", l->name));
		    goto fail3;
		}
		snprintf(cn.ourhook, sizeof(cn.ourhook), "right");
		if (NgSendMsg(pe->PIf->csock, path, NGM_GENERIC_COOKIE, NGM_CONNECT, 
		    &cn, sizeof(cn)) < 0) {
			Perror("[%s] PPPoE: can't connect \"%s\"->\"%s\" and \"%s\"->\"%s\"",
	    		    l->name, path, cn.ourhook, cn.path, cn.peerhook);
			goto fail3;
		}

		/* Shutdown ng_tee node */
		if (NgFuncShutdownNode(pe->PIf->csock, l->name, path) < 0) {
			Perror("[%s] PPPoE: Shutdown ng_tee node %s error",
			    l->name, path);
		}

		if (l->state==PHYS_STATE_READY) {
		    TimerStop(&pe->connectTimer);
		    l->state = PHYS_STATE_UP;
		    PhysUp(l);
		}
		return;
	}

	/* Sanity check. */
	if (l->state != PHYS_STATE_DOWN) {
		Log(LG_PHYS, ("[%s] PPPoE allready active", l->name));
		return;
	};

	/* Create PPPoE node if necessary. */
	PppoeGetNode(l);

	if (!pe->PIf) {
	    Log(LG_ERR, ("[%s] PPPoE node for link is not initialized",
	        l->name));
	    goto fail;
	}

	/* Connect our ng_ppp(4) node link hook to the ng_pppoe(4) node. */
	strlcpy(cn.ourhook, session_hook, sizeof(cn.ourhook));
	snprintf(path, sizeof(path), "[%x]:", pe->PIf->node_id);

	if (!PhysGetUpperHook(l, cn.path, cn.peerhook)) {
	    Log(LG_PHYS, ("[%s] PPPoE: can't get upper hook", l->name));
	    goto fail2;
	}
	
	if (NgSendMsg(pe->PIf->csock, path, NGM_GENERIC_COOKIE, NGM_CONNECT, 
	    &cn, sizeof(cn)) < 0) {
		Perror("[%s] PPPoE: can't connect \"%s\"->\"%s\" and \"%s\"->\"%s\"",
    		    l->name, path, cn.ourhook, cn.path, cn.peerhook);
		goto fail2;
	}
	
#ifdef NGM_PPPOE_SETMAXP_COOKIE
	if (pe->max_payload > 0)
	    Log(LG_PHYS, ("[%s] PPPoE: Set PPP-Max-Payload to '%u'",
		l->name, pe->max_payload));
	/* Tell the PPPoE node to set PPP-Max-Payload value (unset if 0). */
	if (NgSendMsg(pe->PIf->csock, path, NGM_PPPOE_COOKIE, NGM_PPPOE_SETMAXP,
	    &pe->max_payload, sizeof(uint16_t)) < 0) {
		Perror("[%s] PPPoE can't set PPP-Max-Payload value", l->name);
		goto fail2;
	}
#endif

	Log(LG_PHYS, ("[%s] PPPoE: Connecting to '%s'", l->name, pe->session));
	
	/* Tell the PPPoE node to try to connect to a server. */
	memset(idata, 0, sizeof(struct ngpppoe_init_data));
	strlcpy(idata->hook, session_hook, sizeof(idata->hook));
	idata->data_len = strlen(pe->session);
	strncpy(idata->data, pe->session, MAX_SESSION);
	if (NgSendMsg(pe->PIf->csock, path, NGM_PPPOE_COOKIE, NGM_PPPOE_CONNECT,
	    idata, sizeof(*idata) + idata->data_len) < 0) {
		Perror("[%s] PPPoE can't request connection to server", l->name);
		goto fail2;
	}

	/* Set a timer to limit connection time. */
	TimerInit(&pe->connectTimer, "PPPoE-connect",
	    PPPOE_CONNECT_TIMEOUT * SECONDS, PppoeConnectTimeout, l);
	TimerStart(&pe->connectTimer);

	/* OK */
	l->state = PHYS_STATE_CONNECTING;
	strlcpy(pe->real_session, pe->session, sizeof(pe->real_session));
	pe->agent_cid[0] = 0;
	pe->agent_rid[0] = 0;
	pe->mp_reply = 0;
	return;

fail3:
	snprintf(path, sizeof(path), "[%x]:", pe->PIf->node_id);
fail2:
	NgFuncDisconnect(pe->PIf->csock, l->name, path, session_hook);
fail:	
	PhysDown(l, STR_ERROR, NULL);
	return;
}

/*
 * PppoeConnectTimeout()
 */
static void
PppoeConnectTimeout(void *arg)
{
	const Link l = (Link)arg;

	/* Cancel connection. */
	Log(LG_PHYS, ("[%s] PPPoE connection timeout after %d seconds",
	    l->name, PPPOE_CONNECT_TIMEOUT));
	PppoeDoClose(l);
	PhysDown(l, STR_CON_FAILED0, NULL);
}

/*
 * PppoeClose()
 */
static void
PppoeClose(Link l)
{
	const PppoeInfo pe = (PppoeInfo)l->info;

	pe->opened = 0;
	if (l->state == PHYS_STATE_DOWN)
		return;
	PppoeDoClose(l);
	PhysDown(l, STR_MANUALLY, NULL);
}

/*
 * PppoeShutdown()
 *
 * Shut everything down and go to the PHYS_STATE_DOWN state.
 */
static void
PppoeShutdown(Link l)
{
	PppoeDoClose(l);
	PppoeUnListen(l);
	PppoeReleaseNode(l);
	Freee(l->info);
}

/*
 * PppoeDoClose()
 *
 * Shut everything down and go to the PHYS_STATE_DOWN state.
 */
static void
PppoeDoClose(Link l)
{
	const PppoeInfo pi = (PppoeInfo)l->info;
	char path[NG_PATHSIZ];
	char session_hook[NG_HOOKSIZ];

	if (l->state == PHYS_STATE_DOWN)
		return;

	snprintf(path, sizeof(path), "[%x]:", pi->PIf->node_id);
	snprintf(session_hook, sizeof(session_hook), "mpd%d-%d",
	    gPid, l->id);
	NgFuncDisconnect(pi->PIf->csock, l->name, path, session_hook);

	TimerStop(&pi->connectTimer);
	l->state = PHYS_STATE_DOWN;
	pi->incoming = 0;
	memset(pi->peeraddr, 0x00, ETHER_ADDR_LEN);
	pi->real_session[0] = 0;
	pi->agent_cid[0] = 0;
	pi->agent_rid[0] = 0;
	pi->mp_reply = 0;
}

/*
 * PppoeCtrlReadEvent()
 *
 * Receive an incoming control message from the PPPoE node
 */
static void
PppoeCtrlReadEvent(int type, void *arg)
{
	union {
#ifdef NGM_PPPOE_SETMAXP_COOKIE
	    u_char buf[sizeof(struct ng_mesg) + sizeof(struct ngpppoe_maxp)];
#else
	    u_char buf[sizeof(struct ng_mesg) + sizeof(struct ngpppoe_sts)];
#endif
	    struct ng_mesg resp;
	} u;
	char path[NG_PATHSIZ];
	Link l = NULL;
	PppoeInfo pi = NULL;
	
	struct PppoeIf  *PIf = (struct PppoeIf*)arg;

	(void)type;
	/* Read control message. */
	if (NgRecvMsg(PIf->csock, &u.resp, sizeof(u), path) < 0) {
		Perror("PPPoE: error reading message from \"%s\"", path);
		return;
	}
	if (u.resp.header.typecookie != NGM_PPPOE_COOKIE) {
		Log(LG_ERR, ("PPPoE: rec'd cookie %lu from \"%s\"",
		    (u_long)u.resp.header.typecookie, path));
		return;
	}

	switch (u.resp.header.cmd) {
	    case NGM_PPPOE_SUCCESS:
	    case NGM_PPPOE_FAIL:
	    case NGM_PPPOE_CLOSE:
#ifdef NGM_PPPOE_SETMAXP_COOKIE
	    case NGM_PPPOE_SETMAXP:
#endif
	    {
		char	ppphook[NG_HOOKSIZ];
		char	*linkname, *rest;
		int	id;

		/* Check hook name prefix */
		linkname = ((struct ngpppoe_sts *)u.resp.data)->hook;
		if (strncmp(linkname, "listen-", 7) == 0)
		    return;	/* We do not need them */
		snprintf(ppphook, NG_HOOKSIZ, "mpd%d-", gPid);
		if (strncmp(linkname, ppphook, strlen(ppphook))) {
		    Log(LG_ERR, ("PPPoE: message %d from unknown hook \"%s\"",
			u.resp.header.cmd, ((struct ngpppoe_sts *)u.resp.data)->hook));
		    return;
		}
		linkname += strlen(ppphook);
		id = strtol(linkname, &rest, 10);
		if (rest[0] != 0 ||
		  !gLinks[id] ||
		  gLinks[id]->type != &gPppoePhysType ||
		  PIf != ((PppoeInfo)gLinks[id]->info)->PIf) {
		    Log((u.resp.header.cmd == NGM_PPPOE_SUCCESS)?LG_ERR:LG_PHYS,
			("PPPoE: message %d from unexisting link \"%s\"",
			    u.resp.header.cmd, linkname));
		    return;
		}
		
		l = gLinks[id];
		pi = (PppoeInfo)l->info;

		if (l->state == PHYS_STATE_DOWN) {
		    if (u.resp.header.cmd != NGM_PPPOE_CLOSE) 
			Log(LG_PHYS, ("[%s] PPPoE: message %d in DOWN state",
			    l->name, u.resp.header.cmd));
		    return;
		}
	    }
	}

	/* Decode message. */
	switch (u.resp.header.cmd) {
	    case NGM_PPPOE_SESSIONID: /* XXX: I do not know what to do with this? */
		Log(LG_PHYS3, ("PPPoE: rec'd SESSIONID %u from \"%s\"",
		  ntohs((uint16_t)u.resp.data), path));
		break;
	    case NGM_PPPOE_SUCCESS:
		Log(LG_PHYS, ("[%s] PPPoE: connection successful", l->name));
		if (pi->opened) {
		    TimerStop(&pi->connectTimer);
		    l->state = PHYS_STATE_UP;
		    PhysUp(l);
		} else {
		    l->state = PHYS_STATE_READY;
		}
		break;
	    case NGM_PPPOE_FAIL:
		Log(LG_PHYS, ("[%s] PPPoE: connection failed", l->name));
		PppoeDoClose(l);
		PhysDown(l, STR_CON_FAILED0, NULL);
		break;
	    case NGM_PPPOE_CLOSE:
		Log(LG_PHYS, ("[%s] PPPoE: connection closed", l->name));
		PppoeDoClose(l);
		PhysDown(l, STR_DROPPED, NULL);
		break;
	    case NGM_PPPOE_ACNAME:
		Log(LG_PHYS, ("PPPoE: rec'd ACNAME \"%s\"",
		  ((struct ngpppoe_sts *)u.resp.data)->hook));
		break;
#ifdef NGM_PPPOE_SETMAXP_COOKIE
	    case NGM_PPPOE_SETMAXP:
	    {
		struct ngpppoe_maxp *maxp;
		
		maxp = ((struct ngpppoe_maxp *)(void *)u.resp.data);
		Log(LG_PHYS, ("[%s] PPPoE: rec'd PPP-Max-Payload '%u'",
		  l->name, maxp->data));
		if (pi->max_payload > 0) {
		    if (pi->max_payload == maxp->data)
			pi->mp_reply = 1;
		    else
			Log(LG_PHYS,
			  ("[%s] PPPoE: sent and returned values are not equal",
			  l->name));
		} else
		    Log(LG_PHYS, ("[%s] PPPoE: server sent tag PPP-Max-Payload"
		      " without request from the client",
		      l->name));
		break;
	    }
#endif
#ifdef NGM_PPPOE_PADM_COOKIE
	    case NGM_PPPOE_HURL:
		Log(LG_PHYS, ("PPPoE: rec'd HURL \"%s\"",
		  ((struct ngpppoe_padm *)u.resp.data)->msg));
		break;
	    case NGM_PPPOE_MOTM:
		Log(LG_PHYS, ("PPPoE: rec'd MOTM \"%s\"",
		  ((struct ngpppoe_padm *)u.resp.data)->msg));
		break;
#endif
	    default:
		Log(LG_PHYS, ("PPPoE: rec'd command %lu from \"%s\"",
		    (u_long)u.resp.header.cmd, path));
		break;
	}
}

/*
 * PppoeStat()
 */
void
PppoeStat(Context ctx)
{
	const PppoeInfo pe = (PppoeInfo)ctx->lnk->info;
	char	buf[32];

	switch (pe->mac_format) {
	    case MAC_UNFORMATTED:
		sprintf(buf, "unformatted");
		break;
	    case MAC_UNIX_LIKE:
		sprintf(buf, "unix-like");
		break;
	    case MAC_CISCO_LIKE:
		sprintf(buf, "cisco-like");
		break;
	    case MAC_IETF:
		sprintf(buf, "ietf");
		break;
	    default:
		sprintf(buf, "unknown");
		break;
	}

	Printf("PPPoE configuration:\r\n");
	Printf("\tIface Name   : %s\r\n", pe->iface);
	Printf("\tIface Node   : %s\r\n", pe->path);
	Printf("\tIface Hook   : %s\r\n", pe->hook);
	Printf("\tSession      : %s\r\n", pe->session);
#ifdef NGM_PPPOE_SETMAXP_COOKIE
	Printf("\tMax-Payload  : %u\r\n", pe->max_payload);
#endif
	Printf("\tMAC format   : %s\r\n", buf);
	Printf("PPPoE status:\r\n");
	if (ctx->lnk->state != PHYS_STATE_DOWN) {
	    Printf("\tOpened       : %s\r\n", (pe->opened?"YES":"NO"));
	    Printf("\tIncoming     : %s\r\n", (pe->incoming?"YES":"NO"));
	    PppoePeerMacAddr(ctx->lnk, buf, sizeof(buf));
	    Printf("\tCurrent peer : %s\r\n", buf);
	    Printf("\tSession      : %s\r\n", pe->real_session);
	    Printf("\tMax-Payload  : %s\r\n", (pe->mp_reply?"YES":"NO"));
	    Printf("\tCircuit-ID   : %s\r\n", pe->agent_cid);
	    Printf("\tRemote-ID    : %s\r\n", pe->agent_rid);
	}
}

/*
 * PppoeOriginated()
 */
static int
PppoeOriginated(Link l)
{
	PppoeInfo      const pppoe = (PppoeInfo)l->info;

	return (pppoe->incoming ? LINK_ORIGINATE_REMOTE : LINK_ORIGINATE_LOCAL);
}

/*
 * PppoeIsSync()
 */
static int
PppoeIsSync(Link l)
{
	(void)l;
	return (1);
}

static int
PppoePeerMacAddr(Link l, void *buf, size_t buf_len)
{
	PppoeInfo	const pppoe = (PppoeInfo)l->info;

	if (buf_len < 18)
		return (1);
	ether_ntoa_r((struct ether_addr *)pppoe->peeraddr, buf);
	return (0);
}

static int
PppoePeerIface(Link l, void *buf, size_t buf_len)
{
	PppoeInfo	const pppoe = (PppoeInfo)l->info;

	strlcpy(buf, pppoe->iface, buf_len);
	return (0);
}

static int
PppoeCallingNum(Link l, void *buf, size_t buf_len)
{
	PppoeInfo	const pppoe = (PppoeInfo)l->info;

	if (pppoe->incoming) {
	    switch (pppoe->mac_format) {
		case MAC_UNFORMATTED:
		    snprintf(buf, buf_len, "%02x%02x%02x%02x%02x%02x",
		    pppoe->peeraddr[0], pppoe->peeraddr[1], pppoe->peeraddr[2],
		    pppoe->peeraddr[3], pppoe->peeraddr[4], pppoe->peeraddr[5]);
		    break;
		case MAC_UNIX_LIKE:
		    ether_ntoa_r((struct ether_addr *)pppoe->peeraddr, buf);
		    break;
		case MAC_CISCO_LIKE:
		    snprintf(buf, buf_len, "%02x%02x.%02x%02x.%02x%02x",
		    pppoe->peeraddr[0], pppoe->peeraddr[1], pppoe->peeraddr[2],
		    pppoe->peeraddr[3], pppoe->peeraddr[4], pppoe->peeraddr[5]);
		    break;
		case MAC_IETF:
		    snprintf(buf, buf_len, "%02x-%02x-%02x-%02x-%02x-%02x",
		    pppoe->peeraddr[0], pppoe->peeraddr[1], pppoe->peeraddr[2],
		    pppoe->peeraddr[3], pppoe->peeraddr[4], pppoe->peeraddr[5]);
		    break;
		default:
		    sprintf(buf, "unknown");
		    return(-1);
		    break;
	    }
	} else {
	    strlcpy(buf, pppoe->real_session, buf_len);
	}

	return (0);
}

static int
PppoeCalledNum(Link l, void *buf, size_t buf_len)
{
	PppoeInfo	const pppoe = (PppoeInfo)l->info;

	if (!pppoe->incoming) {
	    switch (pppoe->mac_format) {
		case MAC_UNFORMATTED:
		    snprintf(buf, buf_len, "%02x%02x%02x%02x%02x%02x",
		    pppoe->peeraddr[0], pppoe->peeraddr[1], pppoe->peeraddr[2],
		    pppoe->peeraddr[3], pppoe->peeraddr[4], pppoe->peeraddr[5]);
		    break;
		case MAC_UNIX_LIKE:
		    ether_ntoa_r((struct ether_addr *)pppoe->peeraddr, buf);
		    break;
		case MAC_CISCO_LIKE:
		    snprintf(buf, buf_len, "%02x%02x.%02x%02x.%02x%02x",
		    pppoe->peeraddr[0], pppoe->peeraddr[1], pppoe->peeraddr[2],
		    pppoe->peeraddr[3], pppoe->peeraddr[4], pppoe->peeraddr[5]);
		    break;
		case MAC_IETF:
		    snprintf(buf, buf_len, "%02x-%02x-%02x-%02x-%02x-%02x",
		    pppoe->peeraddr[0], pppoe->peeraddr[1], pppoe->peeraddr[2],
		    pppoe->peeraddr[3], pppoe->peeraddr[4], pppoe->peeraddr[5]);
		    break;
		default:
		    sprintf(buf, "unknown");
		    return(-1);
		    break;
	    }
	} else {
	    strlcpy(buf, pppoe->real_session, buf_len);
	}

	return (0);
}

static int
PppoeSelfName(Link l, void *buf, size_t buf_len)
{
	PppoeInfo	const pppoe = (PppoeInfo)l->info;

	strlcpy(buf, pppoe->agent_cid, buf_len);

	return (0);
}

static int
PppoePeerName(Link l, void *buf, size_t buf_len)
{
	PppoeInfo	const pppoe = (PppoeInfo)l->info;

	strlcpy(buf, pppoe->agent_rid, buf_len);

	return (0);
}

static u_short
PppoeGetMtu(Link l, int conf)
{
	PppoeInfo	const pppoe = (PppoeInfo)l->info;

	if (pppoe->max_payload > 0 && pppoe->mp_reply > 0)
	    return (pppoe->max_payload);
	else
	    if (conf == 0)
		return (l->type->mtu);
	    else
		return (l->conf.mtu);
}

static u_short
PppoeGetMru(Link l, int conf)
{
	PppoeInfo	const pppoe = (PppoeInfo)l->info;

	if (pppoe->max_payload > 0 && pppoe->mp_reply > 0)
	    return (pppoe->max_payload);
	else
	    if (conf == 0)
		return (l->type->mru);
	    else
		return (l->conf.mru);
}

static int 
CreatePppoeNode(struct PppoeIf *PIf, const char *iface, const char *path, const char *hook)
{
        union {
		u_char          buf[sizeof(struct ng_mesg) + 2048];
		struct ng_mesg  reply;
	} u;
	struct ng_mesg *resp;
	const struct hooklist *hlist;
	const struct nodeinfo *ninfo;
	uint32_t f;

	/* Make sure interface is up. */
	if (IfaceSetFlag(iface, IFF_UP) != 0) {
		Perror("[%s] PPPoE: can't bring up interface", iface);
		return (0);
	}
	/* Create a new netgraph node */
	if (NgMkSockNode(NULL, &PIf->csock, &PIf->dsock) < 0) {
		Perror("[%s] PPPoE: can't create ctrl socket", iface);
		return (0);
	}
	(void)fcntl(PIf->csock, F_SETFD, 1);
	(void)fcntl(PIf->dsock, F_SETFD, 1);

	/* Check if NG_ETHER_NODE_TYPE is available. */
	if (gNgEtherLoaded == FALSE) {
		const struct typelist *tlist;

		/* Ask for a list of available node types. */
		if (NgSendMsg(PIf->csock, "", NGM_GENERIC_COOKIE, NGM_LISTTYPES,
		    NULL, 0) < 0) {
			Perror("[%s] PPPoE: Cannot send a netgraph message",
			    iface);
			close(PIf->csock);
			close(PIf->dsock);
			return (0);
		}

		/* Get response. */
		resp = &u.reply;
		if (NgRecvMsg(PIf->csock, resp, sizeof(u.buf), NULL) <= 0) {
			Perror("[%s] PPPoE: Cannot get netgraph response",
			    iface);
			close(PIf->csock);
			close(PIf->dsock);
			return (0);
		}

		/* Look for NG_ETHER_NODE_TYPE. */
		tlist = (const struct typelist*)(void *)resp->data;
		for (f = 0; f < tlist->numtypes; f++)
			if (strncmp(tlist->typeinfo[f].type_name,
			    NG_ETHER_NODE_TYPE,
			    sizeof(NG_ETHER_NODE_TYPE) - 1) == 0)
				gNgEtherLoaded = TRUE;

		/* If not found try to load ng_ether and repeat the check. */
		if (gNgEtherLoaded == FALSE && (kldload("ng_ether") < 0)) {
			Perror("PPPoE: Cannot load ng_ether");
			close(PIf->csock);
			close(PIf->dsock);
			assert (0);
		}
		gNgEtherLoaded = TRUE;
	}

	/*
	 * Ask for a list of hooks attached to the "ether" node. This node
	 * should magically exist as a way of hooking stuff onto an ethernet
	 * device.
	 */
	if (NgSendMsg(PIf->csock, path, NGM_GENERIC_COOKIE, NGM_LISTHOOKS,
	    NULL, 0) < 0) {
		Perror("[%s] Cannot send a netgraph message: %s", iface, path);
		close(PIf->csock);
		close(PIf->dsock);
		return (0);
	}

	/* Get our list back. */
	resp = &u.reply;
	if (NgRecvMsg(PIf->csock, resp, sizeof(u.buf), NULL) <= 0) {
		Perror("[%s] Cannot get netgraph response", iface);
		close(PIf->csock);
		close(PIf->dsock);
		return (0);
	}

	hlist = (const struct hooklist *)(void *)resp->data;
	ninfo = &hlist->nodeinfo;

	/* Make sure we've got the right type of node. */
	if (strncmp(ninfo->type, NG_ETHER_NODE_TYPE,
	    sizeof(NG_ETHER_NODE_TYPE) - 1)) {
		Log(LG_ERR, ("[%s] Unexpected node type ``%s'' (wanted ``"
		    NG_ETHER_NODE_TYPE "'') on %s",
		    iface, ninfo->type, path));
		close(PIf->csock);
		close(PIf->dsock);
		return (0);
	}

	/* Look for a hook already attached. */
	for (f = 0; f < ninfo->hooks; f++) {
		const struct linkinfo *nlink = &hlist->link[f];

		/* Search for "orphans" hook. */
		if (strcmp(nlink->ourhook, NG_ETHER_HOOK_ORPHAN) &&
		    strcmp(nlink->ourhook, NG_ETHER_HOOK_DIVERT))
			continue;

		/*
		 * Something is using the data coming out of this ``ether''
		 * node. If it's a PPPoE node, we use that node, otherwise
		 * we complain that someone else is using the node.
		 */
		if (strcmp(nlink->nodeinfo.type, NG_PPPOE_NODE_TYPE)) {
			Log(LG_ERR, ("%s Node type ``%s'' is currently "
			    " using orphan hook\n",
			    path, nlink->nodeinfo.type));
			close(PIf->csock);
			close(PIf->dsock);
			return (0);
		}
		PIf->node_id = nlink->nodeinfo.id;
		break;
	}

	if (f == ninfo->hooks) {
		struct ngm_mkpeer mp;
		char	path2[NG_PATHSIZ];

		/* Create new PPPoE node. */
		memset(&mp, 0, sizeof(mp));
		strcpy(mp.type, NG_PPPOE_NODE_TYPE);
		strlcpy(mp.ourhook, hook, sizeof(mp.ourhook));
		strcpy(mp.peerhook, NG_PPPOE_HOOK_ETHERNET);
		if (NgSendMsg(PIf->csock, path, NGM_GENERIC_COOKIE, NGM_MKPEER, &mp,
		    sizeof(mp)) < 0) {
			Perror("[%s] can't create %s peer to %s,%s",
			    iface, NG_PPPOE_NODE_TYPE, path, hook);
			close(PIf->csock);
			close(PIf->dsock);
			return (0);
		}

		snprintf(path2, sizeof(path2), "%s%s", path, hook);
		/* Get pppoe node ID */
		if ((PIf->node_id = NgGetNodeID(PIf->csock, path2)) == 0) {
			Perror("[%s] Cannot get %s node id", iface,
			    NG_PPPOE_NODE_TYPE);
			close(PIf->csock);
			close(PIf->dsock);
			return (0);
		};
	};

	/* Register an event listening to the control and data sockets. */
	EventRegister(&(PIf->ctrlEvent), EVENT_READ, PIf->csock,
	    EVENT_RECURRING, PppoeCtrlReadEvent, PIf);
	EventRegister(&(PIf->dataEvent), EVENT_READ, PIf->dsock,
	    EVENT_RECURRING, PppoeListenEvent, PIf);

	return (1);
}

/*
 * Look for a tag of a specific type.
 * Don't trust any length the other end says,
 * but assume we already sanity checked ph->length.
 */
static const struct pppoe_tag*
get_tag(const struct pppoe_hdr* ph, uint16_t idx)
{
	const char *const end = ((const char *)(ph + 1))
	            + ntohs(ph->length);
	const struct pppoe_tag *pt = (const void *)(ph + 1);
	const char *ptn;

	/*
	 * Keep processing tags while a tag header will still fit.
	 */
	while((const char*)(pt + 1) <= end) {
		/*
		 * If the tag data would go past the end of the packet, abort.
		 */
		ptn = (((const char *)(pt + 1)) + ntohs(pt->tag_len));
		if (ptn > end)
			return (NULL);
		if (pt->tag_type == idx)
			return (pt);

		pt = (const struct pppoe_tag*)ptn;
	}

	return (NULL);
}

static const struct pppoe_tag*
get_vs_tag(const struct pppoe_hdr* ph, uint32_t idx)
{
	const char *const end = ((const char *)(ph + 1))
	            + ntohs(ph->length);
	const struct pppoe_tag *pt = (const void *)(ph + 1);
	const char *ptn;

	/*
	 * Keep processing tags while a tag header will still fit.
	 */
	while((const char*)(pt + 1) <= end) {
		/*
		 * If the tag data would go past the end of the packet, abort.
		 */
		ptn = (((const char *)(pt + 1)) + ntohs(pt->tag_len));
		if (ptn > end)
			return (NULL);
		if (pt->tag_type == PTT_VENDOR &&
		    ntohs(pt->tag_len) >= 4 &&
		    *(const uint32_t*)(const void *)(pt + 1) == idx)
			return (pt);

		pt = (const struct pppoe_tag*)ptn;
	}

	return (NULL);
}

static void
print_tags(const struct pppoe_hdr* ph)
{
	const char *const end = ((const char *)(ph + 1))
	            + ntohs(ph->length);
	const struct pppoe_tag *pt = (const void *)(ph + 1);
	const char *ptn;
	const void *v;
	char buf[1024], tag[32];
	size_t len, k;

	/*
	 * Keep processing tags while a tag header will still fit.
	 */
	while((const char*)(pt + 1) <= end) {
		/*
		 * If the tag data would go past the end of the packet, abort.
		 */
		v = pt + 1;
		ptn = (((const char *)(pt + 1)) + ntohs(pt->tag_len));
		if (ptn > end)
			return;
		len = ntohs(pt->tag_len);
		buf[0] = 0;
		switch (pt->tag_type) {
		    case PTT_EOL:
			if (len != 0)
			    sprintf(buf, "TAG_LENGTH is not zero!");
			break;
		    case PTT_SRV_NAME:
			if (len >= sizeof(buf))
			    len = sizeof(buf)-1;
			memcpy(buf, pt + 1, len);
			buf[len] = 0;
			if (len == 0)
			    sprintf(buf, "Any service is acceptable");
			break;
		    case PTT_AC_NAME:
			if (len >= sizeof(buf))
			    len = sizeof(buf)-1;
			memcpy(buf, pt + 1, len);
			buf[len] = 0;
			break;
		    case PTT_HOST_UNIQ:
		    case PTT_AC_COOKIE:
		    case PTT_RELAY_SID:
			snprintf(buf, sizeof(buf), "0x%s", Bin2Hex(v, len));
			break;
		    case PTT_VENDOR:
			if (len >= 4) {
			    if ((const uint8_t)*(const uint8_t*)v != 0) {
				snprintf(buf, sizeof(buf),
				    "First byte of VENDOR is not zero! 0x%s",
				    Bin2Hex(v, len));
			    } else {
				snprintf(buf, sizeof(buf), "0x%s 0x%s",
				Bin2Hex(v, 4),
				Bin2Hex((const uint8_t*)v + 4, len - 4));
			    }
			} else {
			    sprintf(buf, "TAG_LENGTH must be >= 4 !");
			}
			break;
		    case PTT_MAX_PAYL:
			if (len != 2) {
			    sprintf(buf, "TAG_LENGTH is not 2!");
			} else {
			    sprintf(buf, "%u", *(const uint16_t*)(const void *)(pt + 1));
			}
			break;
		    case PTT_SRV_ERR:
			if (len > 0 && (const char *)(pt + 1)+4 !=0) {
			    if (len >= sizeof(buf))
				len = sizeof(buf)-1;
			    memcpy(buf, pt + 1, len);
			    buf[len] = 0;
			}
			break;
		    case PTT_SYS_ERR:
		    case PTT_GEN_ERR:
			if (len >= sizeof(buf))
			    len = sizeof(buf)-1;
			memcpy(buf, pt + 1, len);
			buf[len] = 0;
			break;
		    case MPD_PTT_CREDITS:
		    case MPD_PTT_METRICS:
		    case MPD_PTT_SEQ_NUMBER:
		    case MPD_PTT_HURL:
		    case MPD_PTT_MOTM:
		    case MPD_PTT_IP_ROUTE_ADD:
			sprintf(buf, "Not implemented");
			break;
		    default:
			sprintf(buf, "0x%04x", pt->tag_type);
			break;
		}
		/* First check our stat list for known tags */
		for (k = 0; k < NUM_TAG_NAMES; k++) {
		    if (pt->tag_type == tag2str[k].tag) {
			sprintf(tag, "%s", tag2str[k].name);
			break;
		    }
		}
		Log(LG_PHYS3, ("TAG: %s, Value: %s", tag, buf));
		pt = (const struct pppoe_tag*)ptn;
	}
}

static void
PppoeListenEvent(int type, void *arg)
{
	int			k, sz;
	struct PppoeIf		*PIf = (struct PppoeIf *)(arg);
	char			rhook[NG_HOOKSIZ];
	unsigned char		response[1024];

	char			path[NG_PATHSIZ];
	char			path1[NG_PATHSIZ];
	char			session_hook[NG_HOOKSIZ];
	char			*session;
	char			real_session[MAX_SESSION];
	char			agent_cid[64];
	char			agent_rid[64];
	struct ngm_connect      cn;
	struct ngm_mkpeer 	mp;
	Link 			l = NULL;
	PppoeInfo		pi = NULL;
	const struct pppoe_full_hdr	*wh;
	const struct pppoe_hdr	*ph;
	const struct pppoe_tag  *tag;

	union {
	    u_char buf[sizeof(struct ngpppoe_init_data) + MAX_SESSION];
	    struct ngpppoe_init_data poeid;
	} u;
	struct ngpppoe_init_data *const idata = &u.poeid;

	(void)type;
	switch (sz = NgRecvData(PIf->dsock, response, sizeof(response), rhook)) {
          case -1:
	    Log(LG_ERR, ("NgRecvData: %d", sz));
            return;
          case 0:
            Log(LG_ERR, ("NgRecvData: socket closed"));
            return;
        }

	if (strncmp(rhook, "listen-", 7)) {
		Log(LG_ERR, ("PPPoE: data from unknown hook \"%s\"", rhook));
		return;
	}

	session = rhook + 7;

	if ((size_t)sz < sizeof(struct pppoe_full_hdr)) {
		Log(LG_PHYS, ("Incoming truncated PPPoE connection request via %s for "
		    "service \"%s\"", PIf->ifnodepath, session));
		return;
	}

	wh = (struct pppoe_full_hdr *)response;
	ph = &wh->ph;
	if ((tag = get_tag(ph, PTT_SRV_NAME))) {
	    size_t len = ntohs(tag->tag_len);
	    if (len >= sizeof(real_session))
		len = sizeof(real_session)-1;
	    memcpy(real_session, tag + 1, len);
	    real_session[len] = 0;
	} else {
	    strlcpy(real_session, session, sizeof(real_session));
	}
	bzero(agent_cid, sizeof(agent_cid));
	bzero(agent_rid, sizeof(agent_rid));
	if ((tag = get_vs_tag(ph, htonl(0x00000DE9)))) {
	    size_t len = ntohs(tag->tag_len) - 4, pos = 0;
	    const char *b = (const char *)(tag + 1) + 4;
	    while (pos + 1 <= len) {
		size_t len1 = b[pos + 1];
		if (len1 > len - pos - 2)
		    break;
		if (len1 >= sizeof(agent_rid))
		    len1 = sizeof(agent_rid) - 1;
		switch (b[pos]) {
		    case 1:
			strncpy(agent_cid, &b[pos + 2], len1);
			break;
		    case 2:
			strncpy(agent_rid, &b[pos + 2], len1);
			break;
		}
		pos += 2 + len1;
	    }
	}

	Log(LG_PHYS, ("Incoming PPPoE connection request via %s for "
	    "service \"%s\" from %s", PIf->ifnodepath, real_session,
	    ether_ntoa((const struct ether_addr *)&wh->eh.ether_shost)));

	if (gLogOptions & LG_PHYS3)
	    print_tags(ph);

	if (gShutdownInProgress) {
		Log(LG_PHYS, ("Shutdown sequence in progress, ignoring request."));
		return;
	}

	if (OVERLOAD()) {
		Log(LG_PHYS, ("Daemon overloaded, ignoring request."));
		return;
	}

	/* Examine all PPPoE links. */
	for (k = 0; k < gNumLinks; k++) {
		Link l2;
	        PppoeInfo pi2;

		if (!gLinks[k] || gLinks[k]->type != &gPppoePhysType)
			continue;

		l2 = gLinks[k];
		pi2 = (PppoeInfo)l2->info;

		if ((!PhysIsBusy(l2)) &&
		    (pi2->PIf == PIf) &&
		    (strcmp(pi2->session, session) == 0) &&
		    Enabled(&l2->conf.options, LINK_CONF_INCOMING)) {
			l = l2;
			break;
		}
	}
	
	if (l != NULL && l->tmpl)
	    l = LinkInst(l, NULL, 0, 0);

	if (l == NULL) {
		Log(LG_PHYS, ("No free PPPoE link with requested parameters "
		    "was found"));
		return;
	}
	pi = (PppoeInfo)l->info;
	
	Log(LG_PHYS, ("[%s] Accepting PPPoE connection", l->name));

	/* Path to the ng_pppoe */
	snprintf(path, sizeof(path), "[%x]:", PIf->node_id);

	/* Name of ng_pppoe session hook */
	snprintf(session_hook, sizeof(session_hook), "mpd%d-%d",
	    gPid, l->id);
		
	/* Create ng_tee(4) node and connect it to ng_pppoe(4). */
	memset(&mp, 0, sizeof(mp));
	strcpy(mp.type, NG_TEE_NODE_TYPE);
	strlcpy(mp.ourhook, session_hook, sizeof(mp.ourhook));
	snprintf(mp.peerhook, sizeof(mp.peerhook), "left");
	if (NgSendMsg(pi->PIf->csock, path, NGM_GENERIC_COOKIE, NGM_MKPEER,
	    &mp, sizeof(mp)) < 0) {
		Perror("[%s] PPPoE: can't create %s peer to %s,%s",
		    l->name, NG_TEE_NODE_TYPE, path, "left");
		goto close_socket;
	}

	/* Path to the ng_tee */
	snprintf(path1, sizeof(path), "%s%s", path, session_hook);

	/* Connect our socket node link hook to the ng_tee(4) node. */
	memset(&cn, 0, sizeof(cn));
	strlcpy(cn.ourhook, l->name, sizeof(cn.ourhook));
	strlcpy(cn.path, path1, sizeof(cn.path));
	strcpy(cn.peerhook, "left2right");
	if (NgSendMsg(pi->PIf->csock, ".:", NGM_GENERIC_COOKIE, NGM_CONNECT,
	    &cn, sizeof(cn)) < 0) {
		Perror("[%s] PPPoE: can't connect \"%s\"->\"%s\" and \"%s\"->\"%s\"",
		    l->name, ".:", cn.ourhook, cn.path, cn.peerhook);
		goto shutdown_tee;
	}

	/* Put the PPPoE node into OFFER mode. */
	memset(idata, 0, sizeof(*idata));
	strlcpy(idata->hook, session_hook, sizeof(idata->hook));
	if (pi->acname[0] != 0) {
		strlcpy(idata->data, pi->acname, MAX_SESSION);
	} else {
		if (gethostname(idata->data, MAX_SESSION) == -1) {
			Log(LG_ERR, ("[%s] PPPoE: gethostname() failed",
			    l->name));
			idata->data[0] = 0;
		}
		if (idata->data[0] == 0)
			strlcpy(idata->data, "NONAME", MAX_SESSION);
	}
	idata->data_len=strlen(idata->data);

	if (NgSendMsg(pi->PIf->csock, path, NGM_PPPOE_COOKIE, NGM_PPPOE_OFFER,
	    idata, sizeof(*idata) + idata->data_len) < 0) {
		Perror("[%s] PPPoE: can't send NGM_PPPOE_OFFER to %s,%s ",
		    l->name, path, idata->hook);
		goto shutdown_tee;
	}

	memset(idata, 0, sizeof(*idata));
	strlcpy(idata->hook, session_hook, sizeof(idata->hook));
	idata->data_len = strlen(pi->session);
	strncpy(idata->data, pi->session, MAX_SESSION);

	if (NgSendMsg(pi->PIf->csock, path, NGM_PPPOE_COOKIE,
	    NGM_PPPOE_SERVICE, idata,
	    sizeof(*idata) + idata->data_len) < 0) {
		Perror("[%s] PPPoE: can't send NGM_PPPOE_SERVICE to %s,%s",
		    l->name, path, idata->hook);
		goto shutdown_tee;
	}

	/* And send our request data to the waiting node. */
	if (NgSendData(pi->PIf->dsock, l->name, response, sz) == -1) {
		Perror("[%s] PPPoE: Cannot send original request", l->name);
		goto shutdown_tee;
	}
		
	if (NgFuncDisconnect(pi->PIf->csock, l->name, ".:", l->name) < 0) {
		Perror("[%s] PPPoE: can't remove hook %s", l->name, l->name);
		goto shutdown_tee;
    	}
	l->state = PHYS_STATE_CONNECTING;
	pi->incoming = 1;
	/* Record the peer's MAC address */
	memcpy(pi->peeraddr, wh->eh.ether_shost, 6);
	strlcpy(pi->real_session, real_session, sizeof(pi->real_session));
	strlcpy(pi->agent_cid, agent_cid, sizeof(pi->agent_cid));
	strlcpy(pi->agent_rid, agent_rid, sizeof(pi->agent_rid));

	Log(LG_PHYS2, ("[%s] PPPoE response sent", l->name));

	/* Set a timer to limit connection time. */
	TimerInit(&pi->connectTimer, "PPPoE-connect",
	    PPPOE_CONNECT_TIMEOUT * SECONDS, PppoeConnectTimeout, l);
	TimerStart(&pi->connectTimer);

	PhysIncoming(l);
	return;

shutdown_tee:
	if (NgFuncShutdownNode(pi->PIf->csock, l->name, path1) < 0) {
	    Perror("[%s] Shutdown ng_tee node %s error", l->name, path1);
	};

close_socket:
	Log(LG_PHYS, ("[%s] PPPoE connection not accepted due to error",
	    l->name));

	/* If link is not static - shutdown it. */
	if (!l->stay)
	    LinkShutdown(l);
}

/*
 * PppoeGetNode()
 */

static void
PppoeGetNode(Link l)
{
    int i, j = -1, free = -1;
    PppoeInfo pi = (PppoeInfo)l->info;

    if (pi->PIf) // Do this only once for interface
	return;

    if (!strcmp(pi->path, "undefined:")) {
    	    Log(LG_ERR, ("[%s] PPPoE: Skipping link \"%s\" with undefined "
	        "interface", l->name, l->name));
	    return;
    }

    for (i = 0; i < PPPOE_MAXPARENTIFS; i++) {
	if (PppoeIfs[i].ifnodepath[0] == 0) {
	    free = i;
	} else if (strcmp(PppoeIfs[i].ifnodepath, pi->path) == 0) {
    	    j = i;
	    break;
	}
    }
    if (j == -1) {
	if (free == -1) {
		Log(LG_ERR, ("[%s] PPPoE: Too many different parent interfaces! ", 
		    l->name));
		return;
	}
	if (CreatePppoeNode(&PppoeIfs[free], pi->iface, pi->path, pi->hook)) {
		strlcpy(PppoeIfs[free].ifnodepath,
		    pi->path,
		    sizeof(PppoeIfs[free].ifnodepath));
		PppoeIfs[free].refs = 1;
		pi->PIf = &PppoeIfs[free];
	} else {
		Log(LG_ERR, ("[%s] PPPoE: Error creating ng_pppoe "
		    "node on %s", l->name, pi->path));
		return;
	}
    } else {
	PppoeIfs[j].refs++;
        pi->PIf = &PppoeIfs[j];
    }
}

/*
 * PppoeReleaseNode()
 */

static void
PppoeReleaseNode(Link l)
{
    PppoeInfo pi = (PppoeInfo)l->info;

    if (!pi->PIf) // Do this only once for interface
	return;

    pi->PIf->refs--;
    if (pi->PIf->refs == 0) {
	pi->PIf->ifnodepath[0] = 0;
	pi->PIf->node_id = 0;
	EventUnRegister(&pi->PIf->ctrlEvent);
	EventUnRegister(&pi->PIf->dataEvent);
	close(pi->PIf->csock);
	pi->PIf->csock = -1;
	close(pi->PIf->dsock);
	pi->PIf->dsock = -1;
    }

    pi->PIf = NULL;
}

static int 
PppoeListen(Link l)
{
	PppoeInfo pi = (PppoeInfo)l->info;
	struct PppoeIf *PIf = pi->PIf;
	struct PppoeList *pl;
	union {
	    u_char buf[sizeof(struct ngpppoe_init_data) + MAX_SESSION];
	    struct ngpppoe_init_data	poeid;
	} u;
	struct ngpppoe_init_data *const idata = &u.poeid;
	char path[NG_PATHSIZ];
	struct ngm_connect	cn;

	if (pi->list || !pi->PIf)
	    return(1);	/* Do this only once */

	SLIST_FOREACH(pl, &pi->PIf->list, next) {
	    if (strcmp(pl->session, pi->session) == 0)
		break;
	}
	if (pl) {
	    pl->refs++;
	    pi->list = pl;
	    return (1);
	}
	
	pl = Malloc(MB_PHYS, sizeof(*pl));
	strlcpy(pl->session, pi->session, sizeof(pl->session));
	pl->refs = 1;
	pi->list = pl;
	SLIST_INSERT_HEAD(&pi->PIf->list, pl, next);

	snprintf(path, sizeof(path), "[%x]:", PIf->node_id);
	
	/* Connect our socket node link hook to the ng_pppoe(4) node. */
	memset(&cn, 0, sizeof(cn));
	strcpy(cn.path, path);
	snprintf(cn.ourhook, sizeof(cn.peerhook), "listen-%s", pi->session);
	strcpy(cn.peerhook, cn.ourhook);

	if (NgSendMsg(PIf->csock, ".:", NGM_GENERIC_COOKIE, NGM_CONNECT, &cn,
	    sizeof(cn)) < 0) {
		Perror("PPPoE: Can't connect \"%s\"->\"%s\" and \"%s\"->\"%s\"",
		    ".:", cn.ourhook, cn.path, cn.peerhook);
		return(0);
	}

	/* Tell the PPPoE node to be a server. */
	memset(idata, 0, sizeof(*idata));
	snprintf(idata->hook, sizeof(idata->hook), "listen-%s", pi->session);
	idata->data_len = strlen(pi->session);
	strncpy(idata->data, pi->session, MAX_SESSION);

	if (NgSendMsg(PIf->csock, path, NGM_PPPOE_COOKIE, NGM_PPPOE_LISTEN,
	    idata, sizeof(*idata) + idata->data_len) < 0) {
		Perror("PPPoE: Can't send NGM_PPPOE_LISTEN to %s hook %s",
		     path, idata->hook);
		return (0);
	}

	Log(LG_PHYS, ("PPPoE: waiting for connection on %s, service \"%s\"",
		PIf->ifnodepath, idata->data));
	    
	return (1);
}

static int 
PppoeUnListen(Link l)
{
	PppoeInfo pi = (PppoeInfo)l->info;
	struct PppoeIf *PIf = pi->PIf;
	char path[NG_PATHSIZ];
	char session_hook[NG_HOOKSIZ];

	if (!pi->list)
	    return(1);	/* Do this only once */

	pi->list->refs--;
	
	if (pi->list->refs == 0) {
	
	    snprintf(path, sizeof(path), "[%x]:", pi->PIf->node_id);
	    snprintf(session_hook, sizeof(session_hook), "listen-%s", pi->list->session);
	    NgFuncDisconnect(pi->PIf->csock, l->name, path, session_hook);

	    Log(LG_PHYS, ("PPPoE: stop waiting for connection on %s, service \"%s\"",
		PIf->ifnodepath, pi->list->session));
		
	    SLIST_REMOVE(&PIf->list, pi->list, PppoeList, next);
	    Freee(pi->list);
	}
	    
	pi->list = NULL;
	return (1);
}

/*
 * PppoeNodeUpdate()
 */

static void
PppoeNodeUpdate(Link l)
{
    PppoeInfo pi = (PppoeInfo)l->info;
    if (!pi->list) {
	if (Enabled(&l->conf.options, LINK_CONF_INCOMING)) {
	    PppoeGetNode(l);
	    PppoeListen(l);
	}
    } else {
	if (!Enabled(&l->conf.options, LINK_CONF_INCOMING)) {
	    PppoeUnListen(l);
	    if (l->state == PHYS_STATE_DOWN)
		PppoeReleaseNode(l);
	}
    }
}

/*
 * PppoeSetCommand()
 */
 
static int
PppoeSetCommand(Context ctx, int ac, const char *const av[], const void *arg)
{
	const PppoeInfo pi = (PppoeInfo) ctx->lnk->info;
	const char *hookname = ETHER_DEFAULT_HOOK;
	int i;
#ifdef NGM_PPPOE_SETMAXP_COOKIE
	int ap;
#endif
	switch ((intptr_t)arg) {
	case SET_IFACE:
		switch (ac) {
		case 2:
			hookname = av[1];
			/* fall through */
		case 1:
			strlcpy(pi->iface, av[0], sizeof(pi->iface));
			strlcpy(pi->path, pi->iface, sizeof(pi->path) - 1);
			for (i = 0; i < sizeof(pi->path) - 1; i++) {
				if (pi->path[i] == '.' || pi->path[i] == ':')
					pi->path[i] = '_';
				else if (pi->path[i] == '\0') {
					pi->path[i] = ':';
					pi->path[i + 1] = '\0';
					break;
				}
			}
			strlcpy(pi->hook, hookname, sizeof(pi->hook));
			break;
		default:
			return(-1);
		}
		if (pi->list) {
		    PppoeUnListen(ctx->lnk);
		    PppoeReleaseNode(ctx->lnk);
		    PppoeGetNode(ctx->lnk);
		    PppoeListen(ctx->lnk);
		}
		break;
	case SET_SESSION:
		if (ac != 1)
			return(-1);
		strlcpy(pi->session, av[0], sizeof(pi->session));
		if (pi->list) {
		    PppoeUnListen(ctx->lnk);
		    PppoeListen(ctx->lnk);
		}
		break;
	case SET_ACNAME:
		if (ac != 1)
			return(-1);
		strlcpy(pi->acname, av[0], sizeof(pi->acname));
		break;
#ifdef NGM_PPPOE_SETMAXP_COOKIE
	case SET_MAX_PAYLOAD:
		if (ac != 1)
			return(-1);
		ap = atoi(av[0]);
		if (ap < PPPOE_MRU || ap > ETHER_MAX_LEN - 8)
			Error("PPP-Max-Payload value \"%s\"", av[0]);
		pi->max_payload = ap;
		break;
#endif
	case SET_MAC_FORMAT:
		if (ac != 1)
			return(-1);
		if (strcmp(av[0], "unformatted") == 0) {
		    pi->mac_format = MAC_UNFORMATTED;
		} else if (strcmp(av[0], "unix-like") == 0) {
		    pi->mac_format = MAC_UNIX_LIKE;
		} else if (strcmp(av[0], "cisco-like") == 0) {
		    pi->mac_format = MAC_CISCO_LIKE;
		} else if (strcmp(av[0], "ietf") == 0) {
		    pi->mac_format = MAC_IETF;
		} else {
		    Error("Incorrect PPPoE mac-format \"%s\"", av[0]);
		}
		break;
	default:
		assert(0);
	}
	return(0);
}
