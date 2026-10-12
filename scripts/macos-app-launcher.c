// The executable of mep.app (`just install-app`): starts the installed mep
// binary through a login shell and waits for it.
//
// Why not a shell script that execs mep: macOS grants privacy permissions
// (Accessibility, which exec-gui window embedding needs; Screen Recording)
// to the *responsible* app -- the bundle LaunchServices started -- checked
// against that bundle's code signature. A script bundle has no signature
// of its own and its process turns into a different program on exec, so
// the "mep" switched on in System Settings never matched the running mep:
// the permission prompt came back on every launch while the setting showed
// it allowed. This launcher is a real, signed executable that stays alive
// as mep's parent, so mep -- its child -- is covered by the bundle's
// permission, the way a program started from a terminal is covered by the
// terminal's. Signed with the stable "mep Local Code Signing" certificate,
// the grant also survives reinstalls.
//
// MEP_BINARY is the mep to run, fixed at compile time (-DMEP_BINARY=...).
// The login shell supplies the PATH a Finder-started app lacks (no
// ~/.local/bin or Homebrew), which terminals, runners and LSPs rely on.

#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MEP_BINARY
#error "build with -DMEP_BINARY=\"/path/to/mep\""
#endif

extern char **environ;

int main(int argc, char **argv) {
    const char *shell = getenv("SHELL");
    if (shell == NULL || *shell == '\0') shell = "/bin/zsh";
    // shell -lc 'exec "$0" "$@"' MEP_BINARY args...
    char **child_argv = calloc((size_t)argc + 4, sizeof *child_argv);
    if (child_argv == NULL) return 1;
    child_argv[0] = (char *)shell;
    child_argv[1] = "-lc";
    child_argv[2] = "exec \"$0\" \"$@\"";
    child_argv[3] = MEP_BINARY;
    for (int i = 1; i < argc; ++i) child_argv[3 + i] = argv[i];
    pid_t pid;
    if (posix_spawn(&pid, shell, NULL, NULL, child_argv, environ) != 0) return 127;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
