#include <unistd.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

void set_hostname() {
    FILE *f = fopen("/etc/hostname", "r");
    if (!f) return;
    char name[64] = {0};
    fgets(name, sizeof(name), f);
    fclose(f);
    name[strcspn(name, "\n")] = 0;
    sethostname(name, strlen(name));
}

void handle_poweroff(int sig) {
    printf("\nShutting down OnOS...\n");
    sync();
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    reboot(RB_POWER_OFF);
}

void handle_halt(int sig) {
    printf("\nHalting OnOS...\n");
    sync();
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    reboot(RB_HALT_SYSTEM);
}

void handle_reboot(int sig) {
    printf("\nRebooting OnOS...\n");
    sync();
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    reboot(RB_AUTOBOOT);
}

int main() {
    // Core virtual filesystems
    if (mount("none", "/proc", "proc", 0, NULL) != 0)
        perror("mount /proc failed");
    if (mount("none", "/sys", "sysfs", 0, NULL) != 0)
        perror("mount /sys failed");
    if (mount("none", "/dev", "devtmpfs", 0, NULL) != 0)
        perror("mount /dev failed");

    // Runtime directories OpenRC needs
    if (mount("none", "/run", "tmpfs", 0, "mode=0755") != 0)
        perror("mount /run failed");
    if (mount("none", "/tmp", "tmpfs", 0, "mode=1777") != 0)
        perror("mount /tmp failed");

    // Create dirs OpenRC expects inside /run
    mkdir("/run/openrc", 0755);
    mkdir("/run/lock", 0755);
    mkdir("/dev/pts", 0755);
    mount("none", "/dev/pts", "devpts", 0, NULL);
    mount("none", "/dev/shm", "tmpfs", 0, "mode=1777");

    setsid();

    int tty = open("/dev/tty1", O_RDWR);
    if (tty < 0) {
        perror("open /dev/tty1 failed");
    } else {
        ioctl(tty, TIOCSCTTY, 1);
        dup2(tty, STDIN_FILENO);
        dup2(tty, STDOUT_FILENO);
        dup2(tty, STDERR_FILENO);
        if (tty > 2) close(tty);
    }

    signal(SIGTERM, handle_poweroff);
    signal(SIGUSR1, handle_halt);
    signal(SIGUSR2, handle_poweroff);
    signal(SIGINT,  handle_reboot);

    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/root", 1);
    setenv("TERM", "linux", 1);

    set_hostname();

    printf("Welcome to OnOS!\n");

    char *argv[] = {"/sbin/openrc-init", NULL};
    execv("/sbin/openrc-init", argv);
    perror("execv /sbin/openrc-init failed");
    for (;;) {}
}
