/*
 * libssh2 driven as a non-blocking state machine.
 *
 * Everything happens on the render thread: libssh2_session_set_blocking(0) plus
 * a non-blocking socket means every call here returns promptly, and the frame
 * loop calls ssh_tick() once per vblank. A background thread would be the
 * obvious alternative, but the bundled libcrypto is a no-threads build, so the
 * only safe way to touch OpenSSL is from one thread.
 */
#ifndef VITASSH_SSH_H
#define VITASSH_SSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libssh2.h>

#include "config.h"
#include "term.h"
#include "vt.h"

typedef enum {
	SSH_IDLE,
	SSH_CONNECT,
	SSH_HANDSHAKE,
	SSH_HOSTKEY,
	SSH_AUTH,
	SSH_CHANNEL,
	SSH_PTY,
	SSH_SHELL,
	SSH_RUNNING,
	SSH_CLOSED,
	SSH_FAILED,
} SshState;

/* What the state machine needs from the caller after a tick. */
typedef enum {
	SSH_EVENT_NONE,
	SSH_EVENT_NEED_PASSWORD, /* call ssh_provide_password() */
	SSH_EVENT_NEED_HOSTKEY,  /* call ssh_accept_hostkey() */
	SSH_EVENT_RUNNING,
	SSH_EVENT_DATA, /* the terminal changed */
	SSH_EVENT_CLOSED,
	SSH_EVENT_FAILED,
} SshEvent;

enum {
	SSH_AUTH_NONE,
	SSH_AUTH_PASSWORD,
	SSH_AUTH_KEYBOARD,
};

#define SSH_SEND_CAP 8192
#define SSH_READ_BUDGET 65536 /* bytes drained per frame */

typedef struct {
	SshState state;

	HostEntry host;
	int sock;
	LIBSSH2_SESSION *session;
	LIBSSH2_CHANNEL *channel;

	char password[128];
	bool password_set;
	bool hostkey_accepted;

	bool password_asked;
	bool hostkey_asked;
	/*
	 * The method list is queried once per connection and never again: libssh2
	 * resets its internal list state after every completed call, so a second
	 * call would put another "none" request on the wire, and that request
	 * competes with the password attempt for the same incoming packets.
	 */
	bool methods_asked;
	int method;
	int auth_attempts; /* failed password attempts on this connection */

	char fingerprint[80]; /* SHA-256 of the host key, hex */
	char message[192];    /* shown on the error screen */

	uint64_t state_since;
	uint64_t last_keepalive;

	int cols, rows;
	int sent_cols, sent_rows;

	char out[SSH_SEND_CAP];
	size_t out_len;
} Ssh;

void ssh_init(Ssh *s);

/*
 * Opens the socket and starts connecting. Returns NET_OK, or a NET_ERR_* if the
 * connection could not even be started (the error screen shows s->message).
 */
int ssh_start(Ssh *s, const HostEntry *host);

/* Advances the machine one frame and feeds whatever arrived to the terminal. */
SshEvent ssh_tick(Ssh *s, Term *t, VtParser *vt);

void ssh_provide_password(Ssh *s, const char *password);
void ssh_accept_hostkey(Ssh *s, bool accept);

/* Queues bytes for the remote. Never blocks; on overflow the bytes are dropped
 * and logged rather than blocking the frame. */
void ssh_send(Ssh *s, const void *data, size_t len);
void ssh_send_str(Ssh *s, const char *str);

/* Tells the remote the window changed, if it actually did. */
void ssh_resize(Ssh *s, int cols, int rows);

void ssh_stop(Ssh *s);

bool ssh_running(const Ssh *s);
bool ssh_needs_input(const Ssh *s);
const char *ssh_state_name(SshState state);

#endif /* VITASSH_SSH_H */
