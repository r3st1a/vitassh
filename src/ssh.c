#include "ssh.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "log.h"
#include "net.h"

#define SSH_TIMEOUT_CONNECT 15000
#define SSH_TIMEOUT_HANDSHAKE 20000
#define SSH_TIMEOUT_CHANNEL 15000
/* Auth gets its own limit: no keepalives are sent before the shell is up, so a
 * server that goes quiet mid-login is only noticed by this. */
#define SSH_TIMEOUT_AUTH 15000
#define SSH_KEEPALIVE_EVERY_MS 15000
#define SSH_KEEPALIVE_INTERVAL_S 30

/* Enough rope to fat-finger a password, not enough to trip a fail2ban. */
#define SSH_MAX_AUTH_ATTEMPTS 3

/* --- helpers ------------------------------------------------------------ */

static SshEvent fail(Ssh *s, const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;

static SshEvent fail(Ssh *s, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(s->message, sizeof(s->message), fmt, ap);
	va_end(ap);

	log_printf("ssh: %s", s->message);
	s->state = SSH_FAILED;
	return SSH_EVENT_FAILED;
}

static SshEvent fail_libssh2(Ssh *s, const char *what, int rc)
{
	char *detail = NULL;
	int detail_len = 0;

	if (s->session != NULL)
		libssh2_session_last_error(s->session, &detail, &detail_len, 0);

	if (detail != NULL && detail_len > 0)
		return fail(s, "%s: %.*s (%d)", what, detail_len, detail, rc);

	return fail(s, "%s failed (%d)", what, rc);
}

static bool timed_out(const Ssh *s, uint64_t limit_ms)
{
	return now_ms() - s->state_since > limit_ms;
}

static void hex_encode(char *dst, size_t cap, const unsigned char *src, size_t len)
{
	static const char digits[] = "0123456789abcdef";
	size_t i;

	if (cap == 0) return;
	if (len * 2 + 1 > cap) len = (cap - 1) / 2;

	for (i = 0; i < len; i++) {
		dst[i * 2] = digits[src[i] >> 4];
		dst[i * 2 + 1] = digits[src[i] & 0x0F];
	}
	dst[len * 2] = '\0';
}

/*
 * Answers every keyboard-interactive prompt with the password. Servers use this
 * mechanism for plain "Password:" prompts, and there is nothing else we could
 * answer with, so this is what gets PAM-backed servers working.
 */
static void kbdint_callback(const char *name, int name_len,
                            const char *instruction, int instruction_len,
                            int num_prompts,
                            const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                            LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                            void **abstract)
{
	Ssh *s = (abstract != NULL) ? (Ssh *)*abstract : NULL;
	int i;

	(void)name;
	(void)name_len;
	(void)instruction;
	(void)instruction_len;

	if (s == NULL) return;

	for (i = 0; i < num_prompts; i++) {
		size_t len = strlen(s->password);
		char *copy = malloc(len + 1);

		/* libssh2 frees these with the session allocator, which is malloc. */
		if (copy == NULL) continue;
		memcpy(copy, s->password, len + 1);

		responses[i].text = copy;
		responses[i].length = (unsigned int)len;
	}
}

/* --- lifecycle ---------------------------------------------------------- */

void ssh_init(Ssh *s)
{
	memset(s, 0, sizeof(*s));
	s->sock = -1;
	s->cols = TERM_COLS;
	s->rows = TERM_ROWS;
	s->state = SSH_IDLE;
}

void ssh_stop(Ssh *s)
{
	/* Closing the socket first means libssh2's best-effort disconnect fails
	 * immediately instead of trying to write to a dead peer on the way out. */
	net_close(s->sock);
	s->sock = -1;

	if (s->session != NULL) {
		libssh2_session_set_blocking(s->session, 1);
		if (s->channel != NULL) {
			libssh2_channel_free(s->channel);
			s->channel = NULL;
		}
		libssh2_session_disconnect_ex(s->session, SSH_DISCONNECT_BY_APPLICATION,
		                              "bye", "");
		libssh2_session_free(s->session);
		s->session = NULL;
	}
	s->channel = NULL;

	/* Every per-connection flag has to be cleared here, not just in ssh_start:
	 * a reconnect goes through ssh_stop first, and a stale password_asked would
	 * otherwise leave the new connection waiting for a password forever. */
	s->out_len = 0;
	s->password_set = false;
	s->password_asked = false;
	s->hostkey_accepted = false;
	s->hostkey_asked = false;
	s->methods_asked = false;
	s->method = SSH_AUTH_NONE;
	s->auth_attempts = 0;
	s->state = SSH_IDLE;
	s->message[0] = '\0';
	s->fingerprint[0] = '\0';
	memset(s->password, 0, sizeof(s->password));
}

int ssh_start(Ssh *s, const HostEntry *host)
{
	int rc;

	ssh_stop(s);

	s->host = *host;
	s->state_since = now_ms();
	s->last_keepalive = s->state_since;
	s->sent_cols = -1;
	s->sent_rows = -1;
	s->message[0] = '\0';
	s->fingerprint[0] = '\0';

	if (!net_link_up()) {
		s->state = SSH_FAILED;
		snprintf(s->message, sizeof(s->message), "no network connection");
		return NET_ERR_NO_LINK;
	}

	rc = net_connect_start(host->host, host->port, &s->sock);
	if (rc != NET_OK) {
		s->state = SSH_FAILED;
		snprintf(s->message, sizeof(s->message), "%s: %s", host->host,
		         net_strerror(rc));
		log_printf("ssh: %s", s->message);
		return rc;
	}

	log_printf("ssh: starting %s@%s:%d", host->user, host->host, host->port);
	s->state = SSH_CONNECT;
	return NET_OK;
}

/* --- per-state work ----------------------------------------------------- */

static SshEvent do_connect(Ssh *s)
{
	int rc = net_connect_poll(s->sock);

	if (rc < 0) return fail(s, "could not connect to %s:%d", s->host.host, s->host.port);
	if (rc == 0) {
		if (timed_out(s, SSH_TIMEOUT_CONNECT))
			return fail(s, "connection to %s:%d timed out", s->host.host, s->host.port);
		return SSH_EVENT_NONE;
	}

	log_printf("ssh: tcp connected");

	s->session = libssh2_session_init_ex(NULL, NULL, NULL, s);
	if (s->session == NULL) return fail(s, "out of memory for the ssh session");

	libssh2_session_set_blocking(s->session, 0);
	libssh2_session_set_timeout(s->session, 0);
	libssh2_keepalive_config(s->session, 1, SSH_KEEPALIVE_INTERVAL_S);

	s->state = SSH_HANDSHAKE;
	s->state_since = now_ms();
	return SSH_EVENT_NONE;
}

static SshEvent do_handshake(Ssh *s)
{
	const unsigned char *hash;
	int rc = libssh2_session_handshake(s->session, s->sock);

	if (rc == LIBSSH2_ERROR_EAGAIN) {
		if (timed_out(s, SSH_TIMEOUT_HANDSHAKE)) return fail(s, "ssh handshake timed out");
		return SSH_EVENT_NONE;
	}
	if (rc != 0) return fail_libssh2(s, "ssh handshake", rc);

	hash = (const unsigned char *)libssh2_hostkey_hash(s->session,
	                                                   LIBSSH2_HOSTKEY_HASH_SHA256);
	if (hash != NULL)
		hex_encode(s->fingerprint, sizeof(s->fingerprint), hash, 32);
	else
		snprintf(s->fingerprint, sizeof(s->fingerprint), "(unavailable)");

	log_printf("ssh: handshake done, host key %s", s->fingerprint);

	s->state = SSH_HOSTKEY;
	s->state_since = now_ms();
	return SSH_EVENT_NONE;
}

static SshEvent do_channel(Ssh *s)
{
	s->channel = libssh2_channel_open_session(s->session);
	if (s->channel == NULL) {
		int rc = libssh2_session_last_errno(s->session);

		if (rc == LIBSSH2_ERROR_EAGAIN) {
			if (timed_out(s, SSH_TIMEOUT_CHANNEL)) return fail(s, "opening a channel timed out");
			return SSH_EVENT_NONE;
		}
		return fail_libssh2(s, "opening a channel", rc);
	}

	s->state = SSH_PTY;
	s->state_since = now_ms();
	return SSH_EVENT_NONE;
}

static SshEvent do_pty(Ssh *s)
{
	int rc = libssh2_channel_request_pty_ex(s->channel, "xterm", 5, NULL, 0,
	                                        s->cols, s->rows, 0, 0);

	if (rc == LIBSSH2_ERROR_EAGAIN) {
		if (timed_out(s, SSH_TIMEOUT_CHANNEL)) return fail(s, "requesting a pty timed out");
		return SSH_EVENT_NONE;
	}
	if (rc != 0) return fail_libssh2(s, "requesting a pty", rc);

	s->sent_cols = s->cols;
	s->sent_rows = s->rows;
	log_printf("ssh: pty %dx%d", s->cols, s->rows);

	s->state = SSH_SHELL;
	s->state_since = now_ms();
	return SSH_EVENT_NONE;
}

static SshEvent do_shell(Ssh *s)
{
	int rc = libssh2_channel_shell(s->channel);

	if (rc == LIBSSH2_ERROR_EAGAIN) {
		if (timed_out(s, SSH_TIMEOUT_CHANNEL)) return fail(s, "starting the shell timed out");
		return SSH_EVENT_NONE;
	}
	if (rc != 0) return fail_libssh2(s, "starting the shell", rc);

	log_printf("ssh: shell running");
	s->state = SSH_RUNNING;
	s->state_since = now_ms();
	return SSH_EVENT_RUNNING;
}

static void flush_output(Ssh *s)
{
	while (s->out_len > 0) {
		ssize_t n = libssh2_channel_write_ex(s->channel, 0, s->out, s->out_len);

		if (n == LIBSSH2_ERROR_EAGAIN) return;
		if (n <= 0) {
			log_printf("ssh: write failed (%d), dropping %u bytes", (int)n,
			           (unsigned)s->out_len);
			s->out_len = 0;
			return;
		}

		memmove(s->out, s->out + n, s->out_len - (size_t)n);
		s->out_len -= (size_t)n;
	}
}

static size_t drain(Ssh *s, Term *t, VtParser *vt, int stream, size_t budget)
{
	char buf[4096];
	size_t total = 0;

	while (total < budget) {
		ssize_t n = libssh2_channel_read_ex(s->channel, stream, buf, sizeof(buf));

		if (n > 0) {
			vt_feed(vt, t, (const uint8_t *)buf, (size_t)n);
			total += (size_t)n;
			continue;
		}
		if (n == LIBSSH2_ERROR_EAGAIN) break;
		if (n < 0) {
			/* A hard read error ends the session; the eof check below will
			 * notice on the next tick. */
			log_printf("ssh: read stream %d failed (%d)", stream, (int)n);
			break;
		}
		break; /* 0 bytes: nothing more this frame */
	}
	return total;
}

static SshEvent do_running(Ssh *s, Term *t, VtParser *vt)
{
	char reply[VT_OUTPUT_CAP];
	size_t got;
	size_t n;
	int seconds = 0;

	flush_output(s);

	got = drain(s, t, vt, 0, SSH_READ_BUDGET);
	got += drain(s, t, vt, 1, SSH_READ_BUDGET);

	/* DSR/DA replies the parser owes the remote. */
	n = vt_take_output(vt, reply, sizeof(reply));
	if (n > 0) ssh_send(s, reply, n);

	if (now_ms() - s->last_keepalive > SSH_KEEPALIVE_EVERY_MS) {
		s->last_keepalive = now_ms();
		if (libssh2_keepalive_send(s->session, &seconds) < 0) {
			log_printf("ssh: keepalive failed");
			return fail(s, "the connection stopped responding");
		}
	}

	if (libssh2_channel_eof(s->channel) == 1) {
		log_printf("ssh: remote closed the session");
		s->state = SSH_CLOSED;
		return SSH_EVENT_CLOSED;
	}

	return got > 0 ? SSH_EVENT_DATA : SSH_EVENT_NONE;
}

/* --- the state machine -------------------------------------------------- */

SshEvent ssh_tick(Ssh *s, Term *t, VtParser *vt)
{
	switch (s->state) {
	case SSH_IDLE:
	case SSH_CLOSED:
	case SSH_FAILED:
		return SSH_EVENT_NONE;

	case SSH_CONNECT:
		return do_connect(s);

	case SSH_HANDSHAKE:
		return do_handshake(s);

	case SSH_HOSTKEY:
		if (!s->hostkey_accepted) {
			if (!s->hostkey_asked) {
				s->hostkey_asked = true;
				return SSH_EVENT_NEED_HOSTKEY;
			}
			return SSH_EVENT_NONE;
		}
		s->state = SSH_AUTH;
		s->state_since = now_ms();
		return SSH_EVENT_NONE;

	case SSH_AUTH: {
		const char *user = s->host.user;
		int rc;

		/* A pure bit test with no side effects, so calling it every frame is
		 * free and cannot clobber the error code read below. It catches
		 * success reached by any path, including the "none" case. */
		if (libssh2_userauth_authenticated(s->session)) {
			s->state = SSH_CHANNEL;
			s->state_since = now_ms();
			return SSH_EVENT_NONE;
		}

		/*
		 * Ask which methods the server offers exactly once per connection, and
		 * never while an authentication attempt is in flight. libssh2 resets
		 * its list state after every completed call, so a second call puts a
		 * fresh "none" request on the wire -- and that request's reply set
		 * (SUCCESS/FAILURE/BANNER) overlaps the password attempt's, so it
		 * steals the answer the attempt is waiting for. That is what made a
		 * correct password fail with "asking which authentication methods are
		 * offered failed (0)": the server had already said yes.
		 */
		if (!s->methods_asked) {
			char *methods = libssh2_userauth_list(s->session, user,
			                                      (unsigned)strlen(user));

			if (methods == NULL) {
				/*
				 * A server that lets this user in with "none" answers the probe
				 * with USERAUTH_SUCCESS: libssh2 marks the session authenticated
				 * and still returns NULL, with error code 0. That is a login,
				 * not a failure, so it is checked before rc.
				 */
				if (libssh2_userauth_authenticated(s->session)) {
					s->methods_asked = true;
					log_printf("ssh: server accepted \"none\" authentication");
					s->state = SSH_CHANNEL;
					s->state_since = now_ms();
					return SSH_EVENT_NONE;
				}

				rc = libssh2_session_last_errno(s->session);
				if (rc == LIBSSH2_ERROR_EAGAIN) {
					if (timed_out(s, SSH_TIMEOUT_AUTH))
						return fail(s, "%s did not answer the authentication "
						                "method query", s->host.host);
					return SSH_EVENT_NONE;
				}
				/* Every other failure leaves libssh2's list state mid-flight
				 * and unusable; calling it again would resend the probe, so
				 * this is terminal. */
				return fail_libssh2(s, "asking which authentication methods are offered", rc);
			}

			if (strstr(methods, "password") != NULL)
				s->method = SSH_AUTH_PASSWORD;
			else if (strstr(methods, "keyboard-interactive") != NULL)
				s->method = SSH_AUTH_KEYBOARD;
			else
				/* Name what the server does offer: on a console this log line
				 * is the only diagnosis available. */
				return fail(s, "%s offers no password authentication (offers: %.80s)",
				            s->host.host, methods);

			s->methods_asked = true;
			log_printf("ssh: methods \"%s\", using %s", methods,
			           s->method == SSH_AUTH_PASSWORD ? "password" : "keyboard-interactive");
		}

		/*
		 * Only now can a password be wanted, so a server that offers none fails
		 * above without ever raising a keyboard. On a retry this is reached with
		 * methods_asked already set, so there is no second probe.
		 */
		if (!s->password_set) {
			if (!s->password_asked) {
				s->password_asked = true;
				return SSH_EVENT_NEED_PASSWORD;
			}
			return SSH_EVENT_NONE;
		}

		if (s->method == SSH_AUTH_PASSWORD)
			rc = libssh2_userauth_password_ex(s->session, user,
			                                  (unsigned)strlen(user), s->password,
			                                  (unsigned)strlen(s->password), NULL);
		else
			rc = libssh2_userauth_keyboard_interactive_ex(s->session, user,
			                                              (unsigned)strlen(user),
			                                              kbdint_callback);

		if (rc == LIBSSH2_ERROR_EAGAIN) {
			if (timed_out(s, SSH_TIMEOUT_AUTH)) return fail(s, "authentication timed out");
			return SSH_EVENT_NONE;
		}
		if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED) {
			/*
			 * A mistyped password should not cost the user the whole session,
			 * so drop back to asking rather than tearing the connection down.
			 * The method stays picked: re-querying it would be another round
			 * trip for an answer we already have.
			 */
			s->auth_attempts++;
			snprintf(s->message, sizeof(s->message),
			         "Authentication failed for %s@%s", user, s->host.host);
			log_printf("ssh: %s (attempt %d)", s->message, s->auth_attempts);

			if (s->auth_attempts >= SSH_MAX_AUTH_ATTEMPTS)
				return fail(s, "%s, giving up", s->message);

			memset(s->password, 0, sizeof(s->password));
			s->password_set = false;
			s->password_asked = false;
			s->state_since = now_ms();
			return SSH_EVENT_NONE;
		}
		if (rc != 0) return fail_libssh2(s, "authentication", rc);

		s->state = SSH_CHANNEL;
		s->state_since = now_ms();
		return SSH_EVENT_NONE;
	}

	case SSH_CHANNEL:
		return do_channel(s);

	case SSH_PTY:
		return do_pty(s);

	case SSH_SHELL:
		return do_shell(s);

	case SSH_RUNNING:
		return do_running(s, t, vt);
	}

	return SSH_EVENT_NONE;
}

/* --- caller-facing bits ------------------------------------------------- */

void ssh_provide_password(Ssh *s, const char *password)
{
	snprintf(s->password, sizeof(s->password), "%s", password != NULL ? password : "");
	s->password_set = true;
	s->state_since = now_ms();
}

void ssh_accept_hostkey(Ssh *s, bool accept)
{
	if (!accept) {
		fail(s, "host key for %s was rejected", s->host.host);
		return;
	}
	s->hostkey_accepted = true;
	s->state_since = now_ms();
}

void ssh_send(Ssh *s, const void *data, size_t len)
{
	size_t room = sizeof(s->out) - s->out_len;

	if (len > room) {
		log_printf("ssh: send buffer full, dropping %u bytes", (unsigned)(len - room));
		len = room;
	}
	if (len == 0) return;

	memcpy(s->out + s->out_len, data, len);
	s->out_len += len;
}

void ssh_send_str(Ssh *s, const char *str)
{
	if (str != NULL) ssh_send(s, str, strlen(str));
}

void ssh_resize(Ssh *s, int cols, int rows)
{
	if (s->channel == NULL) return;
	if (cols == s->sent_cols && rows == s->sent_rows) return;

	s->cols = cols;
	s->rows = rows;

	if (libssh2_channel_request_pty_size_ex(s->channel, cols, rows, 0, 0) == 0) {
		s->sent_cols = cols;
		s->sent_rows = rows;
		log_printf("ssh: resized to %dx%d", cols, rows);
	}
}

bool ssh_running(const Ssh *s)
{
	return s->state == SSH_RUNNING;
}

bool ssh_needs_input(const Ssh *s)
{
	return s->state == SSH_HOSTKEY || s->state == SSH_AUTH;
}

const char *ssh_state_name(SshState state)
{
	switch (state) {
	case SSH_IDLE:
		return "idle";
	case SSH_CONNECT:
		return "connecting";
	case SSH_HANDSHAKE:
		return "handshaking";
	case SSH_HOSTKEY:
		return "host key";
	case SSH_AUTH:
		return "authenticating";
	case SSH_CHANNEL:
		return "opening channel";
	case SSH_PTY:
		return "requesting pty";
	case SSH_SHELL:
		return "starting shell";
	case SSH_RUNNING:
		return "connected";
	case SSH_CLOSED:
		return "disconnected";
	case SSH_FAILED:
		return "failed";
	}
	return "?";
}
