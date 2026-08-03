// The six builtins and the static table that maps a name to one of them.
// Nothing here ever exits the process: exit only raises sh->want_exit and the
// REPL decides when to leave. Every failure is one line on stderr plus status.

#define _POSIX_C_SOURCE 200809L

#include "builtin.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../alloc/alloc.h"

// getcwd needs a caller supplied buffer. A path longer than this fails with
// ERANGE and is reported like any other cd failure.
#define CWD_MAX 4096

// Status a shell returns when exit is handed something that is not a number.
#define EXIT_BAD_ARG 2

static void bi_err(const char *name, const char *msg) {
    fprintf(stderr, "nullsh: %s: %s\n", name, msg);
}

// The two part form: "nullsh: cd: /nope: No such file or directory".
static void bi_err_arg(const char *name, const char *arg, const char *msg) {
    fprintf(stderr, "nullsh: %s: %s: %s\n", name, arg, msg);
}

// Caller releases the copy with nsh_free.
static char *dup_str(const char *s) {
    size_t size = strlen(s) + 1;
    char *copy = nsh_malloc(size);
    memcpy(copy, s, size);
    return copy;
}

static bool is_name_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

// NAME must match [A-Za-z_][A-Za-z0-9_]*. The length is explicit so the name
// half of NAME=VALUE can be checked without copying it out first.
static bool valid_name(const char *s, size_t len) {
    if (len == 0 || !is_name_start(s[0])) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        if (!is_name_char(s[i])) {
            return false;
        }
    }
    return true;
}

static int bi_cd(Shell *sh, int argc, char **argv) {
    (void)sh;
    if (argc > 2) {
        bi_err("cd", "too many arguments");
        return 1;
    }

    const char *target = NULL;
    bool announce = false;
    if (argc < 2) {
        target = getenv("HOME");
        if (target == NULL) {
            bi_err("cd", "HOME not set");
            return 1;
        }
    } else if (strcmp(argv[1], "-") == 0) {
        target = getenv("OLDPWD");
        if (target == NULL) {
            bi_err("cd", "OLDPWD not set");
            return 1;
        }
        announce = true;
    } else {
        target = argv[1];
    }

    // PWD wins over getcwd so a directory reached through a symlink is
    // remembered the way the user typed it. The snapshot is a copy because the
    // setenv calls below can invalidate any pointer into the environment, and
    // target itself may point at the old OLDPWD.
    char buf[CWD_MAX];
    const char *pwd = getenv("PWD");
    if (pwd == NULL) {
        pwd = getcwd(buf, sizeof buf);
    }
    char *prev = (pwd == NULL) ? NULL : dup_str(pwd);

    if (chdir(target) != 0) {
        bi_err_arg("cd", target, strerror(errno));
        nsh_free(prev);
        return 1;
    }

    if (getcwd(buf, sizeof buf) == NULL) {
        bi_err("cd", strerror(errno));
        nsh_free(prev);
        return 1;
    }

    int status = 0;
    if (prev != NULL && setenv("OLDPWD", prev, 1) != 0) {
        bi_err("cd", strerror(errno));
        status = 1;
    }
    nsh_free(prev);
    if (setenv("PWD", buf, 1) != 0) {
        bi_err("cd", strerror(errno));
        status = 1;
    }
    if (announce) {
        printf("%s\n", buf);
    }
    return status;
}

static int bi_exit(Shell *sh, int argc, char **argv) {
    if (argc > 2) {
        // The one arg shape that leaves the shell running.
        bi_err("exit", "too many arguments");
        return 1;
    }

    if (argc == 2) {
        char *end = NULL;
        long value = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0') {
            // bash still leaves on a bad argument, it just leaves with 2.
            bi_err("exit", "numeric argument required");
            sh->exit_code = EXIT_BAD_ARG;
            sh->want_exit = true;
            return EXIT_BAD_ARG;
        }
        // A status is one byte on the wire, so 256 exits 0 and -1 exits 255.
        sh->exit_code = (int)(value & 0xFF);
    } else {
        sh->exit_code = sh->last_status;
    }

    sh->want_exit = true;
    return sh->exit_code;
}

static int bi_help(Shell *sh, int argc, char **argv) {
    (void)sh;
    (void)argc;
    (void)argv;
    fputs("nullsh builtins:\n"
          "  cd [dir]         change directory, no argument means $HOME,\n"
          "                   a single - means $OLDPWD\n"
          "  exit [status]    leave the shell, default is the last status\n"
          "  export NAME=VAL  set a variable for the shell and its children\n"
          "  help             print this list\n"
          "  history          print the command history, oldest first\n"
          "  unset NAME       remove a variable from the environment\n",
          stdout);
    return 0;
}

static int bi_export(Shell *sh, int argc, char **argv) {
    (void)sh;
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *eq = strchr(arg, '=');
        size_t name_len = (eq == NULL) ? strlen(arg) : (size_t)(eq - arg);

        if (!valid_name(arg, name_len)) {
            // bash reports the bad word and keeps going with the rest.
            bi_err_arg("export", arg, "not a valid identifier");
            status = 1;
            continue;
        }
        if (eq == NULL) {
            // NAME alone would promote a shell variable to the environment.
            // nullsh keeps no separate variable table, so a name that already
            // has a value is exported and one that does not has nothing to
            // export. Either way there is nothing left to do.
            continue;
        }

        char *name = nsh_malloc(name_len + 1);
        memcpy(name, arg, name_len);
        name[name_len] = '\0';
        if (setenv(name, eq + 1, 1) != 0) {
            bi_err_arg("export", arg, strerror(errno));
            status = 1;
        }
        nsh_free(name);
    }
    return status;
}

static int bi_unset(Shell *sh, int argc, char **argv) {
    (void)sh;
    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!valid_name(arg, strlen(arg))) {
            bi_err_arg("unset", arg, "not a valid identifier");
            status = 1;
            continue;
        }
        // Removing a name that was never set is success, not an error.
        if (unsetenv(arg) != 0) {
            bi_err_arg("unset", arg, strerror(errno));
            status = 1;
        }
    }
    return status;
}

static int bi_history(Shell *sh, int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        bi_err("history", "too many arguments");
        return 1;
    }
    size_t count = history_count(&sh->history);
    for (size_t i = 0; i < count; i++) {
        const char *line = history_get(&sh->history, i);
        if (line != NULL) {
            printf("%5zu  %s\n", i + 1, line);
        }
    }
    return 0;
}

typedef struct {
    const char *name;
    BuiltinFn fn;
} BuiltinEntry;

static const BuiltinEntry BUILTINS[] = {
    {"cd", bi_cd},         {"exit", bi_exit},       {"export", bi_export},
    {"help", bi_help},     {"history", bi_history}, {"unset", bi_unset},
};

BuiltinFn builtin_lookup(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++) {
        if (strcmp(BUILTINS[i].name, name) == 0) {
            return BUILTINS[i].fn;
        }
    }
    return NULL;
}
