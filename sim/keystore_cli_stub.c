/*
 * keystore_cli_stub.c — stand-in for the provisioning CLI (keystore_cli.c)
 * when the build excludes the secure store. -1 = "no CLI command
 * consumed", so the sim proceeds to the normal SDL loop.
 */

int keystore_cli_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    return -1;
}
