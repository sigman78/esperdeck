/*
 * keystore_cli_stub.c — stand-in for the provisioning CLI (keystore_cli.c)
 * when the secure store is excluded from the build. -1 = "no CLI command
 * consumed", so the sim proceeds to the normal SDL loop.
 */

int keystore_cli_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    return -1;
}
