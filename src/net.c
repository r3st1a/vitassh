#include "net.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"

/* Handed to sceNetInit and owned by the network stack from then on. */
static char net_pool[1 * 1024 * 1024] __attribute__((aligned(4096)));

static bool net_ready;
static bool netctl_ready;

int net_init(void)
{
	SceNetInitParam param;
	int rc;

	rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	if (rc < 0) {
		log_printf("net: sceSysmoduleLoadModule(NET) = 0x%08X", rc);
		return NET_ERR_MODULE;
	}

	memset(&param, 0, sizeof(param));
	param.memory = net_pool;
	param.size = (int)sizeof(net_pool);
	param.flags = 0;

	rc = sceNetInit(&param);
	if (rc < 0) {
		log_printf("net: sceNetInit = 0x%08X", rc);
		return NET_ERR_MODULE;
	}
	net_ready = true;

	/* NetCtl is only used to wait for a link, so a failure here is survivable:
	 * the connect attempt will simply tell us there is no route. */
	rc = sceNetCtlInit();
	if (rc < 0) {
		log_printf("net: sceNetCtlInit = 0x%08X", rc);
	} else {
		netctl_ready = true;
	}

	log_printf("net: up");
	return NET_OK;
}

void net_term(void)
{
	if (netctl_ready) {
		sceNetCtlTerm();
		netctl_ready = false;
	}
	if (net_ready) {
		sceNetTerm();
		net_ready = false;
	}
}

bool net_link_up(void)
{
	int state = 0;

	if (!netctl_ready) return true; /* no way to tell; let connect decide */

	if (sceNetCtlInetGetState(&state) < 0) return true;
	return state == SCE_NETCTL_STATE_CONNECTED;
}

/* Fills in an IPv4 address, preferring a literal so the common case skips the
 * blocking resolver entirely. */
static int resolve(const char *host, struct sockaddr_in *addr)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	int rc;

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;

	if (inet_pton(AF_INET, host, &addr->sin_addr) == 1) return NET_OK;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	log_printf("net: resolving %s", host);
	rc = getaddrinfo(host, NULL, &hints, &res);
	if (rc != 0 || res == NULL) {
		log_printf("net: getaddrinfo = %d", rc);
		if (res != NULL) freeaddrinfo(res);
		return NET_ERR_RESOLVE;
	}

	memcpy(addr, res->ai_addr, sizeof(*addr));
	freeaddrinfo(res);
	return NET_OK;
}

int net_connect_start(const char *host, int port, int *sock_out)
{
	struct sockaddr_in addr;
	int sock;
	int one = 1;
	int rc;

	*sock_out = -1;

	if (!net_ready) return NET_ERR_MODULE;

	rc = resolve(host, &addr);
	if (rc != NET_OK) return rc;

	addr.sin_port = htons((uint16_t)port);

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		log_printf("net: socket = %d", sock);
		return NET_ERR_SOCKET;
	}

	/* SO_NONBLOCK is the Vita spelling; plain fcntl does not reach the socket
	 * layer here. */
	if (setsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one)) < 0)
		log_printf("net: SO_NONBLOCK failed, connect may block");

	/*
	 * A non-blocking connect reports "still in progress" as EINPROGRESS on a
	 * real Vita, but Vita3K's emulated socket layer returns EAGAIN. Accept
	 * both (EAGAIN is EWOULDBLOCK) instead of closing a socket that is in
	 * fact coming up.
	 */
	rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0 && errno != EINPROGRESS && errno != EALREADY &&
	    errno != EAGAIN && errno != EWOULDBLOCK) {
		log_printf("net: connect = %d errno %d", rc, errno);
		close(sock);
		return NET_ERR_CONNECT;
	}

	log_printf("net: connecting to %s:%d", host, port);
	*sock_out = sock;
	return NET_OK;
}

int net_connect_poll(int sock)
{
	fd_set wfds;
	fd_set efds;
	struct timeval tv;
	int err = 0;
	socklen_t len = sizeof(err);
	int rc;

	if (sock < 0) return -1;

	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	FD_SET(sock, &wfds);
	FD_SET(sock, &efds);

	tv.tv_sec = 0;
	tv.tv_usec = 0;

	rc = select(sock + 1, NULL, &wfds, &efds, &tv);
	if (rc < 0) return -1;
	if (rc == 0) return 0; /* still in progress */
	if (FD_ISSET(sock, &efds)) return -1;

	if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return -1;
	if (err != 0) {
		log_printf("net: connect failed, SO_ERROR %d", err);
		return -1;
	}
	return 1;
}

void net_close(int sock)
{
	if (sock >= 0) close(sock);
}

const char *net_strerror(int code)
{
	switch (code) {
	case NET_OK:
		return "ok";
	case NET_ERR_MODULE:
		return "network stack unavailable";
	case NET_ERR_NO_LINK:
		return "no network connection";
	case NET_ERR_RESOLVE:
		return "could not resolve host";
	case NET_ERR_SOCKET:
		return "could not open socket";
	case NET_ERR_CONNECT:
		return "connection refused";
	default:
		return "network error";
	}
}
