#include "path.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DIGIT_CHAR_TO_NUM(c)    ((c) - '0')
#define DIGIT_NUM_TO_CHAR(n)    ((n) + '0')

struct path_token
{
    char text[PATH_MAX_TOKEN_LEN];
    int index;
};

struct path
{
    int drive_no;
    struct path_token tokens[PATH_MAX_TOKENS];
    int length;
};

static int _path_parse_tokens(PATH *p, const char *path) {
    if (!p || !path) return -1;

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

        // check the length & number of tokens
        if (len >= PATH_MAX_TOKEN_LEN || p->length >= PATH_MAX_TOKENS) {
            return -1;
        }

        // copy token text and append null terminator
        int curr_index = p->length;
        struct path_token *token = &p->tokens[curr_index];
        memcpy(token->text, path, len);
        token->text[len] = '\0';
        token->index = curr_index;

        p->length++;
        path += len;
    }

    return 0;
}

int path_is_valid(const char *path) {
    if (!path) return 0;

    size_t len = strnlen(path, PATH_MAX_LEN);
    if (len >= PATH_MAX_LEN) return 0;

    return len >= 3 && isdigit((unsigned char)path[0]) && path[1] == PATH_ROOT_DELIMITER && path[2] == PATH_TOKEN_DELIMITER;
}

PATH* path_parse(const char *path) {
    if (!path_is_valid(path)) return NULL;

    // extract drive number
    int drive = DIGIT_CHAR_TO_NUM(path[0]);
    path += 3; // Skip "0:/"

    // allocate memory for max path
    PATH *p = (PATH*)malloc(sizeof(struct path));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->drive_no = drive;

    int res = _path_parse_tokens(p, path);
    if (res != 0) {
        path_destroy(p);
        return NULL;
    }

    return p;
}

PATH* path_parse_relative(PATH *root, const char *path) {
    if (!root || !path || root->length < 0) return NULL;

    PATH *p = (PATH*)malloc(sizeof(*root));
    if (!p) return NULL;

    memcpy(p, root, sizeof(*root));

    int res = _path_parse_tokens(p, path);
    if (res != 0) {
        path_destroy(p);
        return NULL;
    }
    
    return p;
}

PATH* path_get_parent_path(PATH *p) {
    if (!p || p->length <= 0) return NULL;

    PATH *parent = (PATH*)malloc(sizeof(*p));
    if (!parent) return NULL;

    memcpy(parent, p, sizeof(*p));
    --(parent->length); 

    return parent;
}

void path_destroy(PATH *p) {
    if (!p) return;
    free(p);
}

int path_to_string(PATH *p, char *out) {
    if (!p || !out) return -1;

    memset(out, 0, PATH_MAX_LEN);

    // "0:/"
    out[0] = DIGIT_NUM_TO_CHAR(p->drive_no);
    out[1] = PATH_ROOT_DELIMITER;
    out[2] = PATH_TOKEN_DELIMITER;

    size_t pos = 3;
    for (int i = 0; i < p->length; ++i) {
        struct path_token *tok = &(p->tokens[i]);
        size_t len = strnlen(tok->text, PATH_MAX_TOKEN_LEN);

        // reserve one byte for '\0'
        if (len >= PATH_MAX_TOKEN_LEN || pos + len >= PATH_MAX_LEN) {
            return -1;
        }

        memcpy(out + pos, tok->text, len);
        pos += len;

        // add separator between tokens
        if (i < p->length - 1) {
            if (pos + 1 >= PATH_MAX_LEN) {
                return -1;
            }
            out[pos++] = PATH_TOKEN_DELIMITER;
        }
    }

    out[pos] = '\0';
    return 0;
}

int path_equals(PATH *p1, PATH *p2) {
    if (!p1 || !p2) return -1;

    if (p1->drive_no != p2->drive_no || p1->length != p2->length)
        return 0;

    for (int i = 0; i < p1->length; ++i) {
        if (strcmp(p1->tokens[i].text, p2->tokens[i].text) != 0)
            return 0;
    }

    return 1;
}

int path_is_child_of(PATH *p1, PATH *p2) {
    if (!p1 || !p2) return -1;

    if (p1->drive_no != p2->drive_no) return 0;

    // p1 cannot be a child of p2 if it has fewer or equal components
    if (p1->length <= p2->length) return 0;

    for (int i = 0; i < p2->length; ++i) {
        PATH_TOKEN *t1 = &(p1->tokens[i]);
        PATH_TOKEN *t2 = &(p2->tokens[i]);
        if (strcmp(t1->text, t2->text) != 0)
            return 0;
    }

    return 1;
}

int path_get_drive_number(PATH *p) {
    if (!p) return -1;
    return p->drive_no;
}

int path_get_number_of_tokens(PATH *p) {
    if (!p) return -1;
    return p->length;
}

PATH_TOKEN* path_get_token_at(PATH *p, int index) {
    if (!p || index < 0 || index >= p->length)
        return NULL;
    return &(p->tokens[index]);
}

PATH_TOKEN* path_get_first_token(PATH *p) {
    if (!p || p->length <= 0)
        return NULL;
    return &(p->tokens[0]);
}

PATH_TOKEN* path_get_last_token(PATH *p) {
    if (!p || p->length <= 0)
        return NULL;
    return &(p->tokens[p->length - 1]);
}

PATH_TOKEN* path_get_next_token(PATH *p, PATH_TOKEN *pt) {
    if (!p || !pt || p->length <= 0)
        return NULL;

    if (pt->index < 0 || pt->index >= p->length)
        return NULL;

    if (pt != &p->tokens[pt->index])
        return NULL;

    if (pt->index + 1 >= p->length)
        return NULL;

    return &p->tokens[pt->index + 1];
}

const char* path_get_token_text(PATH_TOKEN *pt) {
    return pt ? pt->text : NULL;
}

char* path_copy_token_text(PATH_TOKEN *pt, char *out) {
    if (!pt || !out) return NULL;
    return strncpy(out, pt->text, PATH_MAX_TOKEN_LEN);
}