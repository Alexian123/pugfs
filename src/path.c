#include "path.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DIGIT_CHAR_TO_NUM(c)    ((c) - '0')
#define DIGIT_NUM_TO_CHAR(n)    ((n) + '0')

struct path_root
{
    int drive_no;
    struct path_token *first;
};

struct path_token
{
    char text[PATH_MAX_TOKEN_LEN];
    struct path_token *next;
};

bool path_is_valid(const char *path) {
    if (!path) return false;

    size_t len = strnlen(path, PATH_MAX_LEN);
    if (len >= PATH_MAX_LEN) return false;

    return len >= 3 && isdigit(path[0]) && path[1] == PATH_ROOT_DELIMITER && path[2] == PATH_TOKEN_DELIMITER;
}

PATH* path_parse(const char *path, const char *crt_dir_path) {
    (void)crt_dir_path; // Relative paths not implemented yet.
    if (!path_is_valid(path)) return NULL;

    // extract drive number
    int drive = DIGIT_CHAR_TO_NUM(path[0]);
    path += 3; // Skip "0:/"

    // create root node
    PATH *root = malloc(sizeof(struct path_root));
    if (!root) return NULL;
    root->drive_no = drive;
    root->first = NULL;

    // parse tokens
    struct path_token *last = NULL;
    while (*path != '\0') {
        // skip repeated delimiters
        while (*path == PATH_TOKEN_DELIMITER) path++;

        // end
        if (*path == '\0') break;

        // find the length of the next token
        size_t len = 0;
        while (path[len] != '\0' && path[len] != PATH_TOKEN_DELIMITER) {
            ++len;
        }

        // check the length
        if (len >= PATH_MAX_TOKEN_LEN) {
            path_destroy(root);
            return NULL;
        }

        // create token
        struct path_token *token = malloc(sizeof(struct path_token));
        memset(token, 0, sizeof(struct path_token));
        if (!token) {
            path_destroy(root);
            return NULL;
        }

        // copy token text and append null terminator
        memcpy(token->text, path, len);
        token->text[len] = '\0';
        token->next = NULL;

        // append to linked list if necessary
        if (last) {
            last->next = token;
        } else {
            root->first = token;
        }

        last = token;
        path += len;
    }

    return root;
}

void path_destroy(PATH *p)
{
    if (!p) return;
    struct path_token *tok = p->first;
    while (tok) {
        struct path_token *next = tok->next;
        free(tok);
        tok = next;
    }
    free(p);
}

char* path_to_string(PATH *p) {
    if (!p) return NULL;

    char *str = malloc(PATH_MAX_LEN);
    memset(str, 0, PATH_MAX_LEN);
    if (!str) return NULL;

    // "0:/"
    str[0] = DIGIT_NUM_TO_CHAR(p->drive_no);
    str[1] = PATH_ROOT_DELIMITER;
    str[2] = PATH_TOKEN_DELIMITER;

    size_t pos = 3;
    struct path_token *tok = p->first;

    while (tok) {
        size_t len = strnlen(tok->text, PATH_MAX_TOKEN_LEN);

        // reserve one byte for '\0'
        if (len >= PATH_MAX_TOKEN_LEN || pos + len >= PATH_MAX_LEN) {
            free(str);
            return NULL;
        }

        memcpy(str + pos, tok->text, len);
        pos += len;

        tok = tok->next;

        // add separator between tokens
        if (tok) {
            if (pos + 1 >= PATH_MAX_LEN) {
                free(str);
                return NULL;
            }
            str[pos++] = PATH_TOKEN_DELIMITER;
        }
    }

    str[pos] = '\0';
    return str;
}

int path_get_drive_number(PATH *p) {
    return p->drive_no;
}

PATH_TOKEN* path_get_first_token(PATH *p) {
    return p->first;
}

PATH_TOKEN* path_get_next_token(PATH_TOKEN *pt) {
    return pt->next;
}

const char* path_get_token_text(PATH_TOKEN *pt) {
    return pt->text;
}