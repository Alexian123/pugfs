#include "shell.h"
#include "disk_img.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXEC_CONTINUE        1
#define EXEC_SUCCESS         0
#define EXEC_EXIT           -100
#define EXEC_ERR_GENERAL    -2

#define MAX_INPUT_LEN  (PATH_MAX_LEN + 32)

#define STREQ(s1, s2) (strcmp((s1), (s2)) == 0)

#define MAX_LIST_ENTRIES 100

static char list_buffer[MAX_LIST_ENTRIES * PUGFS_FILENAME_BYTES] = "";

static char working_dir[PATH_MAX_LEN + 1] = "0:/";
static char input_buffer[MAX_INPUT_LEN] = "";

static int _shell_get_input();
static int _shell_execute(PUGFS *fs);
static int _shell_execute_no_args(PUGFS *fs, const char *cmd);
static int _shell_execute_1_arg(PUGFS *fs, const char *cmd, const char *arg1);
static int _shell_execute_2_args(PUGFS *fs, const char *cmd, const char *arg1, const char *arg2);
static int _shell_execute_3_args(PUGFS *fs, const char *cmd, const char *arg1, const char *arg2, const char *arg3);
static void _shell_print_help();
static PATH* _shell_parse_path(const char *path);

void shell_start(PUGFS *fs, DISK_IMG *img) {
    if (!fs || !img) {
        fprintf(stderr, "Failed to start shell!\nExiting...\n");
        exit(EXIT_FAILURE);
    }

    int res = 0;
    for (;;) {
        res = _shell_get_input();

        if (res == -2) {
            printf("Error: command too long.\n");
            continue;
        }

        if (res == -1) {
            if (feof(stdin)) {
                printf("\nGoodbye!\n");
                break;
            }

            if (ferror(stdin)) {
                perror("stdin");
                break;
            }
        }

        res = _shell_execute(fs);

        if (res == EXEC_EXIT) {
            printf("\nGoodbye!\n");
            break;
        }

        if (res < 0) {
            printf("[%d]", res);
        }
    }

    pugfs_destroy(fs);
    disk_img_close(img);
}

static int _shell_get_input() {
    memset(input_buffer, 0, MAX_INPUT_LEN);
    printf("(pugfs %s)> ", working_dir);
    fflush(stdout);

    if (!fgets(input_buffer, MAX_INPUT_LEN, stdin)) {
        // EOF or input error
        return -1;
    }

    // check if the entire line was read
    size_t len = strlen(input_buffer);

    if (len > 0 && input_buffer[len - 1] == '\n') {
        input_buffer[len - 1] = '\0';
    } else {
        // no newline: input might be too long
        int c;
        while ((c = getchar()) != '\n' && c != EOF);

        if (len == MAX_INPUT_LEN - 1)
            return -2; // input too long
    }

    return 0;
}

static int _shell_execute(PUGFS *fs) {
    char *command = strtok(input_buffer, " \t");
    if (!command) return EXEC_SUCCESS;
    char *arg1 = strtok(NULL, " \t");
    char *arg2 = strtok(NULL, " \t");
    char *arg3 = strtok(NULL, " \t");
    char *arg4 = strtok(NULL, " \t");
    int res = 0;

    // 0 args commands
    res = _shell_execute_no_args(fs, command);
    if (res <= 0) return res;

    // 1 arg commands
    res = _shell_execute_1_arg(fs, command, arg1);
    if (res <= 0) return res;

    // 2 args commands
    res = _shell_execute_2_args(fs, command, arg1, arg2);
    if (res <= 0) return res;
    
    // 3 args commands
    res = _shell_execute_3_args(fs, command, arg1, arg2, arg3);
    if (res <= 0) return res;

    if (res == EXEC_CONTINUE) {
        printf("Unknown command: %s\nType 'help' for a list of available commands\n", command);
    }

    return res;
}


static int _shell_execute_no_args(PUGFS *fs, const char *cmd) {
    if (STREQ(cmd, "exit")) {
        return EXEC_EXIT;
    } else if (STREQ(cmd, "help")) {
        _shell_print_help();
        return EXIT_SUCCESS;
    }
    return EXEC_CONTINUE;
}

static int _shell_execute_1_arg(PUGFS *fs, const char *cmd, const char *arg1) {
    if (STREQ(cmd, "touch")) {
        if (!arg1) {
            printf("Usage: touch <filepath>\n");
            return EXEC_ERR_GENERAL;
        } else {
            PATH *p = _shell_parse_path(arg1);
            if (!p) return EXEC_ERR_GENERAL;
            int res = pugfs_touch(fs, p);
            path_destroy(p);
            return res;
        }
    } else if (STREQ(cmd, "del")) {
        if (!arg1) {
            printf("Usage: del <filepath>\n");
            return EXEC_ERR_GENERAL;
        } else {
            PATH *p = _shell_parse_path(arg1);
            if (!p) return EXEC_ERR_GENERAL;
            int res =  pugfs_delete(fs, p);
            path_destroy(p);
            return res;
        }
    } else if (STREQ(cmd, "mkdir")) {
        if (!arg1) {
            printf("Usage: mkdir <dirpath>\n");
            return EXEC_ERR_GENERAL;
        } else {
            PATH *p = _shell_parse_path(arg1);
            if (!p) return EXEC_ERR_GENERAL;
            int res = pugfs_mkdir(fs, p);
            path_destroy(p);
            return res;
        }
    } else if (STREQ(cmd, "rmdir")) {
        if (!arg1) {
            printf("Usage: rmdir <dirpath>\n");
            return EXEC_ERR_GENERAL;
        } else {
            PATH *p = _shell_parse_path(arg1);
            if (!p) return EXEC_ERR_GENERAL;
            int res = pugfs_rmdir(fs, p);
            path_destroy(p);
            return res;
        }
    } else if (STREQ(cmd, "ls")) {
        if (!arg1) {
            arg1 = working_dir;
        }

        PATH *p = _shell_parse_path(arg1);
        if (!p) return EXEC_ERR_GENERAL;

        int res = pugfs_lsdir(fs, p, list_buffer, MAX_LIST_ENTRIES);
        if (res < 0) {
            path_destroy(p);
            return res;
        }

        // print the entries
        for (int i = 0; i < res * PUGFS_FILENAME_BYTES; i += PUGFS_FILENAME_BYTES) {
            printf("\t%s\n", list_buffer + i);
        }

        path_destroy(p);
        return EXIT_SUCCESS;
    } else if (STREQ(cmd, "cd")) {
        if (!arg1) {
            return EXIT_SUCCESS;
        }
        strncpy(working_dir, arg1, PATH_MAX_LEN);
        return EXIT_SUCCESS;
    }
    return EXEC_CONTINUE;
}

static int _shell_execute_2_args(PUGFS *fs, const char *cmd, const char *arg1, const char *arg2) {
    if (STREQ(cmd, "trunc")) {
        if (!arg1 || !arg2) {
            printf("Usage: trunc <filepath> <newsize>\n");
            return EXEC_ERR_GENERAL;
        } else {
            uint32_t new_size = 0;
            sscanf(arg2, "%u", &new_size);
            PATH *p = _shell_parse_path(arg1);
            if (!p) return EXEC_ERR_GENERAL;
            int res = pugfs_trunc(fs, p, new_size);
            path_destroy(p);
            return res;
        }
    }
    return EXEC_CONTINUE;
}

static int _shell_execute_3_args(PUGFS *fs, const char *cmd, const char *arg1, const char *arg2, const char *arg3) {
    if (STREQ(cmd, "mv")) {
        if (!arg1 || !arg2) {
            printf("Usage: mv <src> <dst> [-f]\n");
            return EXEC_ERR_GENERAL;
        } else {
            PATH *psrc = _shell_parse_path(arg1);
            if (!psrc) return EXEC_ERR_GENERAL;
            PATH *pdst = _shell_parse_path(arg2);
            if (!pdst) {
                path_destroy(psrc);
                return EXEC_ERR_GENERAL;
            }

            int res = EXEC_ERR_GENERAL;
            if (!arg3) {
                res = pugfs_move(fs, psrc, pdst, 0);
            } else if (STREQ(arg3, "-f")) {
                res = pugfs_move(fs, psrc, pdst, 1);
            } else {
                printf("Usage: mv <src> <dst> [-f]\n");
            }

            path_destroy(psrc);
            path_destroy(pdst);
            return res;
        }
    } else if (STREQ(cmd, "cp")) {
        if (!arg1 || !arg2) {
            printf("Usage: cp <src> <dst> [-f]\n");
            return EXEC_ERR_GENERAL;
        } else {
            PATH *psrc = _shell_parse_path(arg1);
            if (!psrc) return EXEC_ERR_GENERAL;
            PATH *pdst = _shell_parse_path(arg2);
            if (!pdst) {
                path_destroy(psrc);
                return EXEC_ERR_GENERAL;
            }

            int res = EXEC_ERR_GENERAL;
            if (!arg3) {
                res = pugfs_copy(fs, psrc, pdst, 0);
            } else if (STREQ(arg3, "-f")) {
                res = pugfs_copy(fs, psrc, pdst, 1);
            } else {
                printf("Usage: cp <src> <dst> [-f]\n");
            }

            path_destroy(psrc);
            path_destroy(pdst);
            return res;
        }
    }
    return EXEC_CONTINUE;
}

static void _shell_print_help() {
    printf("Avaliable commands:\n");
    printf("exit\ntouch\ndel\nmkdir\nrmdir\nls\ntrunc\nmv\ncp\ncd\n\n");
}

static PATH* _shell_parse_path(const char *path) {
    if (!path_is_valid(path)) return NULL;
    return path_parse(path, NULL);
}