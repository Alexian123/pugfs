#ifndef __PATH_H
#define __PATH_H

#include <stdbool.h>

#define PATH_MAX_LEN                128 // bytes
#define PATH_MAX_TOKEN_LEN          16  // bytes
#define PATH_ROOT_DELIMITER         ':' // after drive number
#define PATH_TOKEN_DELIMITER        '/'

typedef struct path_root PATH;
typedef struct path_token PATH_TOKEN;

bool path_is_valid(const char *path);
PATH* path_parse(const char *path, const char *crt_dir_path);
void path_destroy(PATH *p);
int path_to_string(PATH *p, char *out);

/*
     1: equal
     0: not equal.
    -1: Invalid arguments.
*/
int path_equals(PATH *p1, PATH *p2);

/*
     1: p1 is a child (or descendant) of p2.
     0: It isn't.
    -1: Invalid arguments.
*/
int path_is_child_of(PATH *p1, PATH *p2);

int path_get_drive_number(PATH *p);
PATH_TOKEN* path_get_first_token(PATH *p);
PATH_TOKEN* path_get_next_token(PATH_TOKEN *pt);
int path_pop_last_token(PATH *p, char *out_token_text);    // remove the last and copy its text
const char* path_get_token_text(PATH_TOKEN *pt);

#endif