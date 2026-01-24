/*
 * jsmn.h
 * A minimalistic JSON parser in C.
 *
 * Source: https://github.com/zserge/jsmn
 *
 * Copyright (c) 2010 Serge Zaitsev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef JSMN_H
#define JSMN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSMN_UNDEFINED = 0,
    JSMN_OBJECT = 1,
    JSMN_ARRAY = 2,
    JSMN_STRING = 3,
    JSMN_PRIMITIVE = 4
} jsmntype_t;

typedef enum {
    JSMN_ERROR_NOMEM = -1,
    JSMN_ERROR_INVAL = -2,
    JSMN_ERROR_PART = -3
} jsmnerr_t;

typedef struct {
    jsmntype_t type;
    int start;
    int end;
    int size;
    int parent;
} jsmntok_t;

typedef struct {
    unsigned int pos;
    unsigned int toknext;
    int toksuper;
} jsmn_parser;

static void jsmn_init(jsmn_parser *parser) {
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens, size_t num_tokens) {
    if (parser->toknext >= num_tokens) return NULL;
    jsmntok_t *tok = &tokens[parser->toknext++];
    tok->start = tok->end = -1;
    tok->size = 0;
    tok->parent = -1;
    tok->type = JSMN_UNDEFINED;
    return tok;
}

static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type, int start, int end) {
    token->type = type;
    token->start = start;
    token->end = end;
    token->size = 0;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, size_t num_tokens) {
    int start = (int)parser->pos;
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        if (c == '\t' || c == '\r' || c == '\n' || c == ' ' || c == ',' || c == ']' || c == '}') {
            break;
        }
        if (c < 32 || c >= 127) {
            parser->pos = (unsigned int)start;
            return JSMN_ERROR_INVAL;
        }
    }
    if (tokens) {
        jsmntok_t *tok = jsmn_alloc_token(parser, tokens, num_tokens);
        if (!tok) {
            parser->pos = (unsigned int)start;
            return JSMN_ERROR_NOMEM;
        }
        jsmn_fill_token(tok, JSMN_PRIMITIVE, start, (int)parser->pos);
        tok->parent = parser->toksuper;
    }
    parser->pos--;
    return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, size_t num_tokens) {
    int start = (int)parser->pos;
    parser->pos++;
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        if (c == '\"') {
            if (tokens) {
                jsmntok_t *tok = jsmn_alloc_token(parser, tokens, num_tokens);
                if (!tok) {
                    parser->pos = (unsigned int)start;
                    return JSMN_ERROR_NOMEM;
                }
                jsmn_fill_token(tok, JSMN_STRING, start + 1, (int)parser->pos);
                tok->parent = parser->toksuper;
            }
            return 0;
        }
        if (c == '\\' && parser->pos + 1 < len) {
            parser->pos++;
            continue;
        }
        if ((unsigned char)c < 32) {
            parser->pos = (unsigned int)start;
            return JSMN_ERROR_INVAL;
        }
    }
    parser->pos = (unsigned int)start;
    return JSMN_ERROR_PART;
}

static int jsmn_parse(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens, unsigned int num_tokens) {
    int count = (int)parser->toknext;
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        jsmntok_t *tok;
        switch (c) {
            case '{':
            case '[':
                count++;
                if (tokens) {
                    tok = jsmn_alloc_token(parser, tokens, num_tokens);
                    if (!tok) return JSMN_ERROR_NOMEM;
                    tok->type = (c == '{') ? JSMN_OBJECT : JSMN_ARRAY;
                    tok->start = (int)parser->pos;
                    tok->parent = parser->toksuper;
                    if (parser->toksuper != -1) tokens[parser->toksuper].size++;
                    parser->toksuper = (int)parser->toknext - 1;
                }
                break;
            case '}':
            case ']':
                if (tokens) {
                    jsmntype_t type = (c == '}') ? JSMN_OBJECT : JSMN_ARRAY;
                    int i = (int)parser->toknext - 1;
                    for (;; i--) {
                        if (i < 0) return JSMN_ERROR_INVAL;
                        tok = &tokens[i];
                        if (tok->start != -1 && tok->end == -1) {
                            if (tok->type != type) return JSMN_ERROR_INVAL;
                            tok->end = (int)parser->pos + 1;
                            parser->toksuper = tok->parent;
                            break;
                        }
                    }
                }
                break;
            case '\"': {
                int r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
                if (r < 0) return r;
                count++;
                if (parser->toksuper != -1 && tokens) tokens[parser->toksuper].size++;
                break;
            }
            case '\t': case '\r': case '\n': case ' ':
            case ':': case ',':
                break;
            default: {
                int r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
                if (r < 0) return r;
                count++;
                if (parser->toksuper != -1 && tokens) tokens[parser->toksuper].size++;
                break;
            }
        }
    }

    if (tokens) {
        for (int i = (int)parser->toknext - 1; i >= 0; i--) {
            if (tokens[i].start != -1 && tokens[i].end == -1) return JSMN_ERROR_PART;
        }
    }
    return count;
}

#ifdef __cplusplus
}
#endif

#endif
