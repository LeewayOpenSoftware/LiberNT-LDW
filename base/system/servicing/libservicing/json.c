/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Minimal strict JSON reader (RFC 8259 subset: no comments, no trailing commas)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 24

typedef struct _P
{
    const char *s;
    size_t n;
    size_t i;
    int depth;
} P;

static void SkipWs(P *p)
{
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r'))
        p->i++;
}

static LN_JSON *NewNode(LN_JSON_TYPE t)
{
    LN_JSON *n = calloc(1, sizeof(*n));

    if (n)
        n->type = t;
    return n;
}

void LnJsonFree(LN_JSON *n)
{
    while (n)
    {
        LN_JSON *next = n->next;

        LnJsonFree(n->child);
        free(n->str);
        free(n->key);
        free(n);
        n = next;
    }
}

static int Hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++)
    {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static void PutUtf8(LN_BUF *b, unsigned cp)
{
    char t[4];

    if (cp < 0x80) { t[0] = (char)cp; LnBufAppend(b, t, 1); }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 63)); LnBufAppend(b, t, 2); }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 63)); t[2] = (char)(0x80 | (cp & 63)); LnBufAppend(b, t, 3); }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 63)); t[2] = (char)(0x80 | ((cp >> 6) & 63)); t[3] = (char)(0x80 | (cp & 63)); LnBufAppend(b, t, 4); }
}

static DWORD ParseString(P *p, char **out)
{
    LN_BUF b;

    LnBufInit(&b);
    p->i++;                                 /* opening quote */
    for (;;)
    {
        unsigned char c;

        if (p->i >= p->n)
            goto bad;
        c = (unsigned char)p->s[p->i++];
        if (c == '"')
            break;
        if (c < 0x20)
            goto bad;
        if (c == '\\')
        {
            char e;

            if (p->i >= p->n)
                goto bad;
            e = p->s[p->i++];
            switch (e)
            {
                case '"': LnBufAppend(&b, "\"", 1); break;
                case '\\': LnBufAppend(&b, "\\", 1); break;
                case '/': LnBufAppend(&b, "/", 1); break;
                case 'b': LnBufAppend(&b, "\b", 1); break;
                case 'f': LnBufAppend(&b, "\f", 1); break;
                case 'n': LnBufAppend(&b, "\n", 1); break;
                case 'r': LnBufAppend(&b, "\r", 1); break;
                case 't': LnBufAppend(&b, "\t", 1); break;
                case 'u':
                {
                    unsigned cp, lo;

                    if (p->i + 4 > p->n || !Hex4(p->s + p->i, &cp))
                        goto bad;
                    p->i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00)
                    {
                        if (p->i + 6 > p->n || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u' ||
                            !Hex4(p->s + p->i + 2, &lo) || lo < 0xDC00 || lo > 0xDFFF)
                            goto bad;
                        p->i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    else if (cp >= 0xDC00 && cp <= 0xDFFF)
                        goto bad;
                    if (cp == 0)
                        goto bad;               /* NUL would truncate paths */
                    PutUtf8(&b, cp);
                    break;
                }
                default:
                    goto bad;
            }
        }
        else
            LnBufAppend(&b, (const char *)&c, 1);
    }
    if (!b.data && LnBufAppend(&b, "", 0))
        goto bad;
    *out = b.data;
    return ERROR_SUCCESS;
bad:
    LnBufFree(&b);
    return ERROR_INVALID_DATA;
}

static DWORD ParseValue(P *p, LN_JSON **out);

static DWORD ParseContainer(P *p, LN_JSON **out, BOOL isObject)
{
    LN_JSON *node = NewNode(isObject ? LnJsonObject : LnJsonArray);
    LN_JSON **tail;
    char close = isObject ? '}' : ']';

    if (!node)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (++p->depth > JSON_MAX_DEPTH)
        goto bad;
    tail = &node->child;
    p->i++;
    SkipWs(p);
    if (p->i < p->n && p->s[p->i] == close)
    {
        p->i++;
        p->depth--;
        *out = node;
        return ERROR_SUCCESS;
    }
    for (;;)
    {
        LN_JSON *val = NULL;
        char *key = NULL;
        DWORD err;

        SkipWs(p);
        if (isObject)
        {
            if (p->i >= p->n || p->s[p->i] != '"' || ParseString(p, &key))
                goto bad;
            SkipWs(p);
            if (p->i >= p->n || p->s[p->i++] != ':')
            {
                free(key);
                goto bad;
            }
            SkipWs(p);
        }
        err = ParseValue(p, &val);
        if (err)
        {
            free(key);
            LnJsonFree(node);
            return err;
        }
        if (isObject)
            val->key = key;
        *tail = val;
        tail = &val->next;
        SkipWs(p);
        if (p->i >= p->n)
            goto bad;
        if (p->s[p->i] == ',')
        {
            p->i++;
            continue;
        }
        if (p->s[p->i] == close)
        {
            p->i++;
            break;
        }
        goto bad;
    }
    p->depth--;
    *out = node;
    return ERROR_SUCCESS;
bad:
    LnJsonFree(node);
    return ERROR_INVALID_DATA;
}

static DWORD ParseValue(P *p, LN_JSON **out)
{
    LN_JSON *n;

    SkipWs(p);
    if (p->i >= p->n)
        return ERROR_INVALID_DATA;
    switch (p->s[p->i])
    {
        case '{': return ParseContainer(p, out, TRUE);
        case '[': return ParseContainer(p, out, FALSE);
        case '"':
        {
            char *s;
            DWORD err = ParseString(p, &s);

            if (err)
                return err;
            n = NewNode(LnJsonString);
            if (!n)
            {
                free(s);
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            n->str = s;
            *out = n;
            return ERROR_SUCCESS;
        }
        case 't':
        case 'f':
        case 'n':
        {
            int isTrue = !strncmp(p->s + p->i, "true", 4) && p->i + 4 <= p->n;
            int isFalse = !strncmp(p->s + p->i, "false", 5) && p->i + 5 <= p->n;
            int isNull = !strncmp(p->s + p->i, "null", 4) && p->i + 4 <= p->n;

            if (!isTrue && !isFalse && !isNull)
                return ERROR_INVALID_DATA;
            n = NewNode(isNull ? LnJsonNull : LnJsonBool);
            if (!n)
                return ERROR_NOT_ENOUGH_MEMORY;
            n->boolean = isTrue;
            p->i += isFalse ? 5 : 4;
            *out = n;
            return ERROR_SUCCESS;
        }
        default:
        {
            long long v = 0;
            int neg = 0, digits = 0;

            if (p->s[p->i] == '-')
            {
                neg = 1;
                p->i++;
            }
            while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
            {
                if (v > (0x7fffffffffffffffLL - 9) / 10)
                    return ERROR_INVALID_DATA;
                v = v * 10 + (p->s[p->i] - '0');
                p->i++;
                digits++;
            }
            if (!digits || (p->i < p->n && (p->s[p->i] == '.' || p->s[p->i] == 'e' || p->s[p->i] == 'E')))
                return ERROR_INVALID_DATA;   /* integers only */
            n = NewNode(LnJsonNumber);
            if (!n)
                return ERROR_NOT_ENOUGH_MEMORY;
            n->num = neg ? -v : v;
            *out = n;
            return ERROR_SUCCESS;
        }
    }
}

DWORD LnJsonParse(const char *text, size_t len, LN_JSON **root)
{
    P p;
    DWORD err;

    *root = NULL;
    p.s = text;
    p.n = len;
    p.i = 0;
    p.depth = 0;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        p.i = 3;                              /* tolerate a UTF-8 BOM */
    err = ParseValue(&p, root);
    if (err)
        return err;
    SkipWs(&p);
    if (p.i != p.n)
    {
        LnJsonFree(*root);
        *root = NULL;
        return ERROR_INVALID_DATA;
    }
    return ERROR_SUCCESS;
}

const LN_JSON *LnJsonGet(const LN_JSON *obj, const char *key)
{
    const LN_JSON *c;

    if (!obj || obj->type != LnJsonObject)
        return NULL;
    for (c = obj->child; c; c = c->next)
    {
        if (c->key && !strcmp(c->key, key))
            return c;
    }
    return NULL;
}

const char *LnJsonGetString(const LN_JSON *obj, const char *key)
{
    const LN_JSON *n = LnJsonGet(obj, key);

    return (n && n->type == LnJsonString) ? n->str : NULL;
}

BOOL LnJsonGetInt(const LN_JSON *obj, const char *key, long long *out)
{
    const LN_JSON *n = LnJsonGet(obj, key);

    if (!n || n->type != LnJsonNumber)
        return FALSE;
    *out = n->num;
    return TRUE;
}

BOOL LnJsonGetBool(const LN_JSON *obj, const char *key, BOOL *out)
{
    const LN_JSON *n = LnJsonGet(obj, key);

    if (!n || n->type != LnJsonBool)
        return FALSE;
    *out = n->boolean;
    return TRUE;
}
