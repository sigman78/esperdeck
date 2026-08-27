/*
 * keystore_cli.h — sim-only keystore provisioning CLI (docs/storage_auth.md,
 * provisioning door 1: PC-side wrapping before the build creates the
 * littlefs image).
 */

#ifndef KEYSTORE_CLI_H
#define KEYSTORE_CLI_H

/**
 * If argv contains a keystore command, run it against sim_storage/ and
 * return the process exit code (0 ok, 1 error). Returns -1 when no
 * keystore flag is present — the caller proceeds to the normal GUI.
 */
int keystore_cli_main(int argc, char **argv);

#endif /* KEYSTORE_CLI_H */
