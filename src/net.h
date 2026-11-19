/*
 * Vita networking: module bring-up, link polling, DNS, non-blocking connect.
 *
 * sceNetInit() needs a buffer that lives as long as the stack does, so it is a
 * static here rather than something the caller owns.
 */
#ifndef VITASSH_NET_H
#define VITASSH_NET_H

#include <stdbool.h>
#include <stdint.h>

enum {
	NET_OK = 0,
	NET_ERR_MODULE, /* the net sysmodule or sceNetInit failed */
	NET_ERR_NO_LINK,
	NET_ERR_RESOLVE,
	NET_ERR_SOCKET,
	NET_ERR_CONNECT,
};

/* Brings up the net sysmodule and sceNet. Returns NET_OK or a NET_ERR_*. */
int net_init(void);

void net_term(void);

/* Polls NetCtl. False until the console holds an IP address. */
bool net_link_up(void);

/*
 * Opens a non-blocking socket to host:port. On NET_OK *sock_out is a connected-
 * pending socket that must then be polled with net_connect_poll().
 */
int net_connect_start(const char *host, int port, int *sock_out);

/* 0 = still connecting, 1 = connected, -1 = the connect attempt failed. */
int net_connect_poll(int sock);

void net_close(int sock);

const char *net_strerror(int code);

#endif /* VITASSH_NET_H */
