/*
 * Where the app keeps its files on the device. One place so the config and the
 * log cannot drift apart.
 */
#ifndef VITASSH_PATHS_H
#define VITASSH_PATHS_H

#define VITASSH_DIR "ux0:data/vitassh"
#define VITASSH_HOSTS VITASSH_DIR "/hosts.txt"
#define VITASSH_LOG VITASSH_DIR "/log.txt"
/*
 * Saved passwords, encrypted. Never written in the clear: see src/vault.c.
 * An unreadable one is moved aside under this name plus ".bad"; vault.c works
 * that out from the path it is handed, so there is only one spelling of it.
 */
#define VITASSH_SECRETS VITASSH_DIR "/secrets.dat"

#endif /* VITASSH_PATHS_H */
