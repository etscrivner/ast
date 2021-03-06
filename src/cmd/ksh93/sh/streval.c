/***********************************************************************
 *                                                                      *
 *               This software is part of the ast package               *
 *          Copyright (c) 1982-2014 AT&T Intellectual Property          *
 *                      and is licensed under the                       *
 *                 Eclipse Public License, Version 1.0                  *
 *                    by AT&T Intellectual Property                     *
 *                                                                      *
 *                A copy of the License is available at                 *
 *          http://www.eclipse.org/org/documents/epl-v10.html           *
 *         (with md5 checksum b35adb5213ca9657e911e9befb180842)         *
 *                                                                      *
 *              Information and Software Systems Research               *
 *                            AT&T Research                             *
 *                           Florham Park NJ                            *
 *                                                                      *
 *                    David Korn <dgkorn@gmail.com>                     *
 *                                                                      *
 ***********************************************************************/
//
// D. G. Korn
// AT&T Labs
//
// arithmetic expression evaluator
//
// This version compiles the expression onto a stack and has a separate executor.
//
#include "config_ast.h"  // IWYU pragma: keep

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "ast.h"
#include "ast_float.h"
#include "defs.h"
#include "error.h"
#include "name.h"
#include "sfio.h"
#include "stk.h"
#include "streval.h"

#ifndef ERROR_dictionary
#define ERROR_dictionary(s) (s)
#endif

#define MAXLEVEL 1024
#define SMALL_STACK 12

//
// The following are used with tokenbits() macro.
//
#define T_OP 0x3f       // mask for operator number
#define T_BINARY 0x40   // binary operators
#define T_NOFLOAT 0x80  // non floating point operator
#define A_LVALUE (2 * MAXPREC + 2)

#define pow2size(x) \
    ((x) <= 2 ? 2 : (x) <= 4 ? 4 : (x) <= 8 ? 8 : (x) <= 16 ? 16 : (x) <= 32 ? 32 : 64)
#define round(x, size) (((x) + (size)-1) & ~((size)-1))
#define stkpush(stk, v, val, type)                                 \
    ((((v)->offset = round(stktell(stk), pow2size(sizeof(type)))), \
      stkseek(stk, (v)->offset + sizeof(type)), *((type *)stkptr(stk, (v)->offset)) = (val)))
#define stkoffset(stk, v) ((v)->offset)
#define roundptr(ep, cp, type) \
    (((unsigned char *)(ep)) + round(cp - ((unsigned char *)(ep)), pow2size(sizeof(type))))

static int level;

typedef struct vars {  // vars stacked per invocation
    Shell_t *shp;
    const char *expr;     // current expression
    const char *nextchr;  // next char in current expression
    const char *errchr;   // next char after error
    const char *errstr;   // error string
    struct lval errmsg;   // error message text
    int offset;           // offset for pushchr macro
    int staksize;         // current stack size needed
    int stakmaxsize;      // maximum stack size needed
    unsigned char paren;  // parenthesis level
    char infun;           // incremented by comma inside function
    int emode;
    Sfdouble_t (*convert)(const char **, struct lval *, int, Sfdouble_t);
} vars_t;

typedef Sfdouble_t (*Math_f)(Sfdouble_t, ...);
typedef Sfdouble_t (*Math_1f_f)(Sfdouble_t);
typedef int (*Math_1i_f)(Sfdouble_t);
typedef Sfdouble_t (*Math_2f_f)(Sfdouble_t, Sfdouble_t);
typedef Sfdouble_t (*Math_2v_f)(int, Sfdouble_t, Sfdouble_t);
typedef Sfdouble_t (*Math_2f_i)(Sfdouble_t, int);
typedef int (*Math_2i_f)(Sfdouble_t, Sfdouble_t);
typedef Sfdouble_t (*Math_3f_f)(Sfdouble_t, Sfdouble_t, Sfdouble_t);
typedef int (*Math_3i_f)(Sfdouble_t, Sfdouble_t, Sfdouble_t);

#define getchr(vp) (*(vp)->nextchr++)
#define nxtchr(vp) ((vp)->nextchr++)
#define peekchr(vp) (*(vp)->nextchr)
#define ungetchr(vp) ((vp)->nextchr--)

// This is used to convert characters to math expression tokens.
static inline int getop(int c) {
    if (c < sizeof(strval_states)) return strval_states[(c)];
    switch (c) {
        case '|': {
            return A_OR;
        }
        case '^': {
            return A_XOR;
        }
        case '~': {
            return A_TILDE;
        }
        default: { return A_REG; }
    }
}

#define seterror(v, msg) _seterror(v, ERROR_dictionary(msg))
#define ERROR(vp, msg) return seterror((vp), msg)

//
// Set error message string and return 0.
//
static_fn int _seterror(vars_t *vp, const char *msg) {
    if (!vp->errmsg.value) vp->errmsg.value = (char *)msg;
    vp->errchr = vp->nextchr;
    vp->nextchr = "";
    level = 0;
    return 0;
}

static_fn void arith_error(const char *message, const char *expr, int mode) {
    level = 0;
    mode = (mode & 3) != 0;
    errormsg(SH_DICT, ERROR_exit(mode), message, expr);
}

#if _ast_no_um2fm
static_fn Sfdouble_t U2F(Sfulong_t u) {
    Sflong_t s = u;
    Sfdouble_t f;

    if (s >= 0) return s;
    s = u / 2;
    f = s;
    f *= 2;
    if (u & 1) f++;
    return f;
}
#else
#define U2F(x) x
#endif

static_fn void array_args(Shell_t *shp, char *tp, int n) {
    while (n--) {
        if (tp[n] == 5) {
            Namval_t *np = nv_namptr(shp->mathnodes, n);
            nv_offattr(np, NV_LDOUBLE);
        }
    }
}

Sfdouble_t arith_exec(Arith_t *ep) {
    Sfdouble_t num = 0, *dp, *sp;
    unsigned char *cp = ep->code;
    int c, type = 0;
    char *tp;
    Sfdouble_t d, small_stack[SMALL_STACK + 1], arg[9];
    const char *ptr = "";
    char *lastval = NULL;
    int lastsub = 0;
    struct lval node;
    Shell_t *shp = ep->shp;

    memset(&node, 0, sizeof(node));
    node.shp = shp;
    node.emode = ep->emode;
    node.expr = ep->expr;
    node.elen = ep->elen;
    if (level++ >= MAXLEVEL) {
        arith_error(e_recursive, ep->expr, ep->emode);
        return 0;
    }
    if (ep->staksize < SMALL_STACK) {
        sp = small_stack;
    } else {
        sp = stkalloc(shp->stk, ep->staksize * (sizeof(Sfdouble_t) + 1));
    }
    tp = (char *)(sp + ep->staksize);
    tp--, sp--;
    while ((c = *cp++)) {
        if (c & T_NOFLOAT) {
            if (type || ((c & T_BINARY) && (c & T_OP) != A_MOD && tp[-1] == 1)) {
                arith_error(e_incompatible, ep->expr, ep->emode);
            }
        }
        switch (c & T_OP) {
            case A_JMP:
            case A_JMPZ:
            case A_JMPNZ: {
                c &= T_OP;
                cp = roundptr(ep, cp, short);
                if ((c == A_JMPZ && num) || (c == A_JMPNZ && !num)) {
                    cp += sizeof(short);
                } else {
                    cp = (unsigned char *)ep + *((short *)cp);
                }
                continue;
            }
            case A_NOTNOT: {
                num = (num != 0);
                type = 0;
                break;
            }
            case A_PLUSPLUS: {
                node.nosub = -1;
                (*ep->fun)(&ptr, &node, ASSIGN, num + 1);
                break;
            }
            case A_MINUSMINUS: {
                node.nosub = -1;
                (*ep->fun)(&ptr, &node, ASSIGN, num - 1);
                break;
            }
            case A_INCR: {
                num = num + 1;
                node.nosub = -1;
                num = (*ep->fun)(&ptr, &node, ASSIGN, num);
                break;
            }
            case A_DECR: {
                num = num - 1;
                node.nosub = -1;
                num = (*ep->fun)(&ptr, &node, ASSIGN, num);
                break;
            }
            case A_SWAP: {
                num = sp[-1];
                sp[-1] = *sp;
                type = tp[-1];
                tp[-1] = *tp;
                break;
            }
            case A_POP: {
                sp--;
                continue;
            }
            case A_ASSIGNOP1: {
                node.emode |= ARITH_ASSIGNOP;
            }
            // TODO: Determine if this should be FALLTHRU.
            // FALLTHRU
            case A_PUSHV: {
                cp = roundptr(ep, cp, Sfdouble_t *);
                dp = *((Sfdouble_t **)cp);
                cp += sizeof(Sfdouble_t *);
                c = *(short *)cp;
                cp += sizeof(short);
                lastval = node.value = (char *)dp;
                node.flag = c;
                if (node.flag) lastval = NULL;
                node.isfloat = 0;
                node.level = level;
                node.nosub = 0;
                node.nextop = *cp;
                if (node.nextop == A_JMP) {
                    node.nextop = ((unsigned char *)ep)[*((short *)roundptr(ep, cp + 1, short))];
                }
                num = (*ep->fun)(&ptr, &node, VALUE, num);
                if (lastval) lastval = node.ovalue;
                if (node.emode & ARITH_ASSIGNOP) {
                    lastsub = node.nosub;
                    node.nosub = 0;
                    node.emode &= ~ARITH_ASSIGNOP;
                }
                if (node.value != (char *)dp) arith_error(node.value, ptr, ep->emode);
                *++sp = num;
                type = node.isfloat;
                if ((d = num) > LDBL_LLONG_MAX && num <= LDBL_ULLONG_MAX) {
                    type = TYPE_U;
                    d -= LDBL_LLONG_MAX;
                }
                if ((Sflong_t)d != d) type = TYPE_LD;
                *++tp = type;
                c = 0;
                break;
            }
            case A_ENUM: {
                node.eflag = 1;
                continue;
            }
            case A_ASSIGNOP: {
                node.nosub = lastsub;
            }
            // TODO: Determine if this should be FALLTHRU.
            // FALLTHRU
            case A_STORE: {
                cp = roundptr(ep, cp, Sfdouble_t *);
                dp = *((Sfdouble_t **)cp);
                cp += sizeof(Sfdouble_t *);
                c = *(short *)cp;
                if (c < 0) c = 0;
                cp += sizeof(short);
                node.value = (char *)dp;
                node.flag = c;
                if (lastval) node.eflag = 1;
                node.ptr = NULL;
                num = (*ep->fun)(&ptr, &node, ASSIGN, num);
                if (lastval && node.ptr) {
                    Sfdouble_t r;
                    node.flag = 0;
                    node.value = lastval;
                    r = (*ep->fun)(&ptr, &node, VALUE, num);
                    if (r != num) {
                        node.flag = c;
                        node.value = (char *)dp;
                        num = (*ep->fun)(&ptr, &node, ASSIGN, r);
                    }

                } else if (lastval && num == 0 && sh_isoption(shp, SH_NOUNSET) &&
                           nv_isnull((Namval_t *)lastval)) {
                    arith_error((char *)ERROR_dictionary(e_notset), nv_name((Namval_t *)lastval),
                                3);
                }
                lastval = NULL;
                c = 0;
                break;
            }
            case A_PUSHF: {
                cp = roundptr(ep, cp, Math_f);
                *++sp = (Sfdouble_t)(cp - ep->code);
                cp += sizeof(Math_f);
                *++tp = *cp++;
                node.userfn = 0;
                if (*tp > 1) node.userfn = 1;
                *tp &= 1;
                continue;
            }
            case A_PUSHN: {
                cp = roundptr(ep, cp, Sfdouble_t);
                num = *((Sfdouble_t *)cp);
                cp += sizeof(Sfdouble_t);
                *++sp = num;
                *++tp = type = *cp++;
                break;
            }
            case A_NOT: {
                type = 0;
                num = !num;
                break;
            }
            case A_UMINUS: {
                num = -num;
                break;
            }
            case A_TILDE: {
                num = ~((Sflong_t)(num));
                break;
            }
            case A_PLUS: {
                num += sp[-1];
                break;
            }
            case A_MINUS: {
                num = sp[-1] - num;
                break;
            }
            case A_TIMES: {
                num *= sp[-1];
                break;
            }
            case A_POW: {
                extern Math_f sh_mathstdfun(const char *, size_t, short *);
                static Math_2f_f powfn;
                if (!powfn) {
                    powfn = (Math_2f_f)sh_mathstdfun("pow", 3, NULL);
                    if (!powfn) powfn = (Math_2f_f)pow;
                }
                num = powfn(sp[-1], num);
                break;
            }
            case A_MOD: {
                if (!(Sflong_t)num) arith_error(e_divzero, ep->expr, ep->emode);
                if (type == TYPE_U || tp[-1] == TYPE_U) {
                    num = U2F((Sfulong_t)(sp[-1]) % (Sfulong_t)(num));
                } else {
                    num = (Sflong_t)(sp[-1]) % (Sflong_t)(num);
                }
                break;
            }
            case A_DIV: {
                if (type || tp[-1]) {
                    num = sp[-1] / num;
                    type = type > tp[-1] ? type : tp[-1];
                } else if ((Sfulong_t)(num < 0 ? -num : num) == 0) {
                    arith_error(e_divzero, ep->expr, ep->emode);
                } else if (type == TYPE_U || tp[-1] == TYPE_U) {
                    num = U2F((Sfulong_t)(sp[-1]) / (Sfulong_t)(num));
                } else {
                    Sfdouble_t x = floorl(sp[-1]);
                    Sfdouble_t y = floorl(num);
                    num = floorl(x / y);
                }
                break;
            }
            case A_LSHIFT: {
                if ((long)num >= CHAR_BIT * sizeof(Sfulong_t)) {
                    num = 0;
                } else if (tp[-1] == TYPE_U) {
                    num = U2F((Sfulong_t)(sp[-1]) << (long)(num));
                } else {
                    num = (Sflong_t)(sp[-1]) << (long)(num);
                }
                break;
            }
            case A_RSHIFT: {
                if ((long)num >= CHAR_BIT * sizeof(Sfulong_t)) {
                    num = 0;
                } else if (tp[-1] == TYPE_U) {
                    num = U2F((Sfulong_t)(sp[-1]) >> (long)(num));
                } else {
                    num = (Sflong_t)(sp[-1]) >> (long)(num);
                }
                break;
            }
            case A_XOR: {
                if (type == TYPE_U || tp[-1] == TYPE_U) {
                    num = U2F((Sfulong_t)(sp[-1]) ^ (Sfulong_t)(num));
                } else {
                    num = (Sflong_t)(sp[-1]) ^ (Sflong_t)(num);
                }
                break;
            }
            case A_OR: {
                if (type == TYPE_U || tp[-1] == TYPE_U) {
                    num = U2F((Sfulong_t)(sp[-1]) | (Sfulong_t)(num));
                } else {
                    num = (Sflong_t)(sp[-1]) | (Sflong_t)(num);
                }
                break;
            }
            case A_AND: {
                if (type == TYPE_U || tp[-1] == TYPE_U) {
                    num = U2F((Sfulong_t)(sp[-1]) & (Sfulong_t)(num));
                } else {
                    num = (Sflong_t)(sp[-1]) & (Sflong_t)(num);
                }
                break;
            }
            case A_EQ: {
                num = (sp[-1] == num);
                type = 0;
                break;
            }
            case A_NEQ: {
                num = (sp[-1] != num);
                type = 0;
                break;
            }
            case A_LE: {
                num = (sp[-1] <= num);
                type = 0;
                break;
            }
            case A_GE: {
                num = (sp[-1] >= num);
                type = 0;
                break;
            }
            case A_GT: {
                num = (sp[-1] > num);
                type = 0;
                break;
            }
            case A_LT: {
                num = (sp[-1] < num);
                type = 0;
                break;
            }
            case A_CALL1F: {
                sp--, tp--;
                Math_1f_f funx = *(Math_1f_f *)(ep->code + (int)(*sp));
                type = *tp;
                if (c & T_BINARY) {
                    c &= ~T_BINARY;
                    arg[0] = num;
                    arg[1] = 0;
                    array_args(shp, tp + 1, 1);
                    num = sh_mathfun(shp, funx, 1, arg);
                    node.userfn = 0;
                    break;
                }
                num = (*funx)(num);
                break;
            }
            case A_CALL1I: {
                sp--, tp--;
                type = *tp;
                Math_1i_f funx = *((Math_1i_f *)(ep->code + (int)(*sp)));
                num = (*funx)(num);
                break;
            }
            case A_CALL2F: {
                sp -= 2, tp -= 2;
                type = *tp;
                if (c & T_BINARY) {
                    c &= ~T_BINARY;
                    arg[0] = sp[1];
                    arg[1] = num;
                    arg[2] = 0;
                    array_args(shp, tp + 1, 2);
                    Math_2f_i funx = *(Math_2f_i *)(ep->code + (int)(*sp));
                    num = sh_mathfun(shp, funx, 2, arg);
                    node.userfn = 0;
                    break;
                }
                if (c & T_NOFLOAT) {
                    Math_2f_i funx = *(Math_2f_i *)(ep->code + (int)(*sp));
                    num = (*funx)(sp[1], (int)num);
                } else {
                    Math_2f_f funx = *(Math_2f_f *)(ep->code + (int)(*sp));
                    num = (*funx)(sp[1], num);
                }
                break;
            }
            case A_CALL2V: {
                sp -= 2, tp -= 2;
                type = tp[1];
                Math_2v_f funx = *(Math_2v_f *)(ep->code + (int)(*sp));
                num = (*funx)(type - 1, sp[1], num);
                break;
            }
            case A_CALL2I: {
                sp -= 2, tp -= 2;
                type = *tp;
                Math_2i_f funx = *(Math_2i_f *)(ep->code + (int)(*sp));
                num = (*funx)(sp[1], num);
                break;
            }
            case A_CALL3F: {
                sp -= 3, tp -= 3;
                Math_3f_f funx = *(Math_3f_f *)(ep->code + (int)(*sp));
                type = *tp;
                if (c & T_BINARY) {
                    c &= ~T_BINARY;
                    arg[0] = sp[1];
                    arg[1] = sp[2];
                    arg[2] = num;
                    arg[3] = 0;
                    array_args(shp, tp + 1, 3);
                    num = sh_mathfun(shp, funx, 3, arg);
                    node.userfn = 0;
                    break;
                }
                num = (*funx)(sp[1], sp[2], num);
                break;
            }
        }
        if (c) lastval = NULL;
        if (c & T_BINARY) {
            node.ptr = NULL;
            sp--, tp--;
            if (*tp > type) type = *tp;
        }
        *sp = num;
        *tp = type;
    }
    if (level > 0) level--;
    if (type == 0 && !num) num = 0;
    return num;
}

//
// This returns operator tokens or A_REG or A_NUM.
//
static_fn int gettok(vars_t *vp) {
    int c, op;

    vp->errchr = vp->nextchr;
    while (true) {
        c = getchr(vp);
        op = getop(c);
        switch (op) {
            case 0: {
                vp->errchr = vp->nextchr;
                break;
            }
            case A_EOF: {
                vp->nextchr--;
                return op;
            }
            case A_COMMA: {
                if (vp->shp->decomma && (c = peekchr(vp)) >= '0' && c <= '9') {
                    op = A_DIG;
                    ungetchr(vp);
                }
                return op;
            }
            case A_DOT: {
                if ((c = peekchr(vp)) >= '0' && c <= '9') {
                    op = A_DIG;
                } else {
                    op = A_REG;
                }
                ungetchr(vp);
                return op;
            }
            case A_DIG:
            case A_REG:
            case A_LIT: {
                ungetchr(vp);
                return op;
            }
            case A_QUEST: {
                if (peekchr(vp) == ':') {
                    nxtchr(vp);
                    op = A_QCOLON;
                }
                return op;
            }
            case A_LT:
            case A_GT: {
                if (peekchr(vp) == c) {
                    nxtchr(vp);
                    op -= 2;
                    return op;
                }
            }
            // FALLTHRU
            case A_NOT:
            case A_COLON: {
                c = '=';
            }
            // FALLTHRU
            case A_ASSIGN:
            case A_TIMES:
            case A_PLUS:
            case A_MINUS:
            case A_OR:
            case A_AND: {
                if (peekchr(vp) == c) {
                    nxtchr(vp);
                    op--;
                }
                return op;
            }
            // This handles all the tokens that don't need special fixups; e.g., A_LPAR.
            default: { return op; }
        }
    }
}

//
// Evaluate a subexpression with precedence.
//
static_fn bool expr(vars_t *vp, int precedence) {
    int c, op;
    int invalid, wasop = 0;
    struct lval lvalue, assignop;
    const char *pos;
    Sfdouble_t d;
    Shell_t *shp = vp->shp;

    lvalue.value = NULL;
    lvalue.nargs = 0;
    lvalue.fun = NULL;
    lvalue.shp = shp;
again:
    op = gettok(vp);
    c = 2 * MAXPREC + 1;
    switch (op) {
        case A_PLUS: {
            goto again;
        }
        case A_EOF: {
            if (precedence > 2) ERROR(vp, e_moretokens);
            return true;
        }
        case A_MINUS: {
            op = A_UMINUS;
            goto common;
        }
        case A_NOT: {
            goto common;
        }
        case A_MINUSMINUS: {
            c = A_LVALUE;
            op = A_DECR | T_NOFLOAT;
            goto common;
        }
        case A_PLUSPLUS: {
            c = A_LVALUE;
            op = A_INCR | T_NOFLOAT;
        }
        // FALLTHRU
        case A_TILDE: {
            op |= T_NOFLOAT;
        common:
            if (!expr(vp, c)) return false;
            sfputc(shp->stk, op);
            break;
        }
        default: {
            vp->nextchr = vp->errchr;
            wasop = 1;
        }
    }
    invalid = wasop;
    while (1) {
        assignop.value = 0;
        op = gettok(vp);
        if (op == A_DIG || op == A_REG || op == A_LIT) {
            if (!wasop) ERROR(vp, e_synbad);
            goto number;
        }
        if (wasop++ && op != A_LPAR) ERROR(vp, e_synbad);
        // Check for assignment operation.
        if (peekchr(vp) == '=' && !(strval_precedence[op] & NOASSIGN)) {
            if ((!lvalue.value || precedence > 3)) ERROR(vp, e_notlvalue);
            if (precedence == 3) precedence = 2;
            assignop = lvalue;
            nxtchr(vp);
            c = 3;
        } else {
            c = (strval_precedence[op] & PRECMASK);
            if (c == MAXPREC || op == A_POW) c++;
            c *= 2;
        }
        // From here on c is the new precedence level.
        if (lvalue.value && (op != A_ASSIGN)) {
            if (vp->staksize++ >= vp->stakmaxsize) vp->stakmaxsize = vp->staksize;
            if (op == A_EQ || op == A_NEQ) sfputc(shp->stk, A_ENUM);
            sfputc(stkstd, assignop.value ? A_ASSIGNOP1 : A_PUSHV);
            stkpush(shp->stk, vp, lvalue.value, char *);
            if (lvalue.flag < 0) lvalue.flag = 0;
            stkpush(shp->stk, vp, lvalue.flag, short);
            if (vp->nextchr == 0) ERROR(vp, e_badnum);
            if (!(strval_precedence[op] & SEQPOINT)) lvalue.value = NULL;
            invalid = 0;
        } else if (precedence == A_LVALUE) {
            ERROR(vp, e_notlvalue);
        }
        if (invalid && op > A_ASSIGN) ERROR(vp, e_synbad);
        if (precedence >= c) goto done;
        if (strval_precedence[op] & RASSOC) c--;
        if ((c < (2 * MAXPREC + 1)) && !(strval_precedence[op] & SEQPOINT)) {
            wasop = 0;
            if (!expr(vp, c)) return false;
        }
        switch (op) {
            case A_RPAR: {
                if (!vp->paren) ERROR(vp, e_paren);
                if (invalid) ERROR(vp, e_synbad);
                goto done;
            }
            case A_COMMA: {
                wasop = 0;
                if (vp->infun) {
                    vp->infun++;
                } else {
                    sfputc(shp->stk, A_POP);
                    vp->staksize--;
                }
                if (!expr(vp, c)) {
                    stkseek(shp->stk, stktell(shp->stk) - 1);
                    return false;
                }
                lvalue.value = NULL;
                break;
            }
            case A_LPAR: {
                int infun = vp->infun;
                int userfun = 0;
                Sfdouble_t (*fun)(Sfdouble_t, ...);
                int nargs = lvalue.nargs;
                if (nargs < 0 && (nargs & 070) == 070) nargs = -nargs;
                fun = lvalue.fun;
                lvalue.fun = NULL;
                if (fun) {
                    if (vp->staksize++ >= vp->stakmaxsize) vp->stakmaxsize = vp->staksize;
                    vp->infun = 1;
                    if ((int)lvalue.nargs < 0) {
                        userfun = T_BINARY;
                    } else if ((int)lvalue.nargs & 040) {
                        userfun = T_NOFLOAT;
                    }
                    sfputc(shp->stk, A_PUSHF);
                    stkpush(shp->stk, vp, fun, Math_f);
                    sfputc(shp->stk, 1 + (userfun == T_BINARY));
                } else {
                    vp->infun = 0;
                }
                if (!invalid) ERROR(vp, e_synbad);
                vp->paren++;
                if (!expr(vp, 1)) return false;
                vp->paren--;
                if (fun) {
                    int x = (nargs & 010) ? 2 : -1;
                    int call = A_CALL1F;
                    if (nargs & 0100) call = A_CALL1V;
                    nargs &= 7;
                    if (vp->infun != nargs) ERROR(vp, e_argcount);
                    if ((vp->staksize += nargs) >= vp->stakmaxsize) {
                        vp->stakmaxsize = vp->staksize + nargs;
                    }
                    sfputc(shp->stk, call + userfun + nargs + x);
                    vp->staksize -= nargs;
                }
                vp->infun = infun;
                if (gettok(vp) != A_RPAR) ERROR(vp, e_paren);
                wasop = 0;
                break;
            }
            case A_PLUSPLUS:
            case A_MINUSMINUS: {
                wasop = 0;
                op |= T_NOFLOAT;
            }
            // FALLTHRU
            case A_ASSIGN: {
                if (!lvalue.value) ERROR(vp, e_notlvalue);
                if (op == A_ASSIGN) {
                    sfputc(shp->stk, A_STORE);
                    stkpush(shp->stk, vp, lvalue.value, char *);
                    stkpush(shp->stk, vp, lvalue.flag, short);
                    vp->staksize--;
                } else {
                    sfputc(shp->stk, op);
                }
                lvalue.value = NULL;
                break;
            }
            case A_QUEST: {
                int offset1, offset2;
                sfputc(shp->stk, A_JMPZ);
                stkpush(shp->stk, vp, 0, short);
                offset1 = stkoffset(shp->stk, vp);
                sfputc(shp->stk, A_POP);
                if (!expr(vp, 1)) return false;
                if (gettok(vp) != A_COLON) ERROR(vp, e_questcolon);
                sfputc(shp->stk, A_JMP);
                stkpush(shp->stk, vp, 0, short);
                offset2 = stkoffset(shp->stk, vp);
                *((short *)stkptr(shp->stk, offset1)) = stktell(shp->stk);
                sfputc(shp->stk, A_POP);
                if (!expr(vp, 3)) return false;
                *((short *)stkptr(shp->stk, offset2)) = stktell(shp->stk);
                lvalue.value = NULL;
                wasop = 0;
                break;
            }
            case A_COLON: {
                ERROR(vp, e_badcolon);
                break;
            }
            case A_QCOLON:
            case A_ANDAND:
            case A_OROR: {
                int offset;
                if (op == A_ANDAND) {
                    op = A_JMPZ;
                } else {
                    op = A_JMPNZ;
                }
                sfputc(shp->stk, op);
                stkpush(shp->stk, vp, 0, short);
                offset = stkoffset(shp->stk, vp);
                sfputc(shp->stk, A_POP);
                if (!expr(vp, c)) return false;
                *((short *)stkptr(shp->stk, offset)) = stktell(shp->stk);
                if (op != A_QCOLON) sfputc(shp->stk, A_NOTNOT);
                lvalue.value = NULL;
                wasop = 0;
                break;
            }
            case A_AND:
            case A_OR:
            case A_XOR:
            case A_LSHIFT:
            case A_RSHIFT:
            case A_MOD:
                op |= T_NOFLOAT;
            // FALLTHRU
            case A_PLUS:
            case A_MINUS:
            case A_TIMES:
            case A_DIV:
            case A_EQ:
            case A_NEQ:
            case A_LT:
            case A_LE:
            case A_GT:
            case A_GE:
            case A_POW: {
                sfputc(shp->stk, op | T_BINARY);
                vp->staksize--;
                break;
            }
            case A_NOT:
            case A_TILDE:
            default: {
                ERROR(vp, e_synbad);
            number:
                wasop = 0;
                if (*vp->nextchr == 'L' && vp->nextchr[1] == '\'') {
                    vp->nextchr++;
                    op = A_LIT;
                }
                pos = vp->nextchr;
                lvalue.isfloat = 0;
                lvalue.expr = vp->expr;
                lvalue.emode = vp->emode;
                if (op == A_LIT) {
                    // Character constants.
                    if (pos[1] == '\\' && pos[2] == '\'' && pos[3] != '\'') {
                        d = '\\';
                        vp->nextchr += 2;
                    } else {
                        d = chresc(pos + 1, (char **)&vp->nextchr);
                    }
                    // posix allows the trailing ' to be optional.
                    if (*vp->nextchr == '\'') vp->nextchr++;
                } else {
                    d = (*vp->convert)(&vp->nextchr, &lvalue, LOOKUP, 0);
                }
                if (vp->nextchr == pos) {
                    vp->errmsg.value = lvalue.value;
                    if (vp->errmsg.value) vp->errstr = pos;
                    ERROR(vp, op == A_LIT ? e_charconst : e_synbad);
                }
#if 0
                        if(op==A_DIG || op==A_LIT)
#else
                if (op == A_DIG || op == A_LIT || lvalue.isfloat == TYPE_LD)
#endif
                {
                    sfputc(shp->stk, A_PUSHN);
                    if (vp->staksize++ >= vp->stakmaxsize) vp->stakmaxsize = vp->staksize;
                    stkpush(shp->stk, vp, d, Sfdouble_t);
                    sfputc(shp->stk, lvalue.isfloat);
                }

                // Check for function call.
                if (lvalue.fun) continue;
                break;
            }
        }
        invalid = 0;
        if (assignop.value) {
            if (vp->staksize++ >= vp->stakmaxsize) vp->stakmaxsize = vp->staksize;
            if (assignop.flag < 0) assignop.flag = 0;
            sfputc(shp->stk, c & 1 ? A_ASSIGNOP : A_STORE);
            stkpush(shp->stk, vp, assignop.value, char *);
            stkpush(shp->stk, vp, assignop.flag, short);
        }
    }

done:
    vp->nextchr = vp->errchr;
    return true;
}

Arith_t *arith_compile(Shell_t *shp, const char *string, char **last,
                       Sfdouble_t (*fun)(const char **, struct lval *, int, Sfdouble_t),
                       int emode) {
    vars_t cur;
    Arith_t *ep;
    int offset;
    int nounset = sh_isoption(shp, SH_NOUNSET);

    memset(&cur, 0, sizeof(cur));
    cur.shp = shp;
    cur.expr = cur.nextchr = string;
    cur.convert = fun;
    cur.emode = emode;
    cur.errmsg.value = 0;
    cur.errmsg.emode = emode;
    stkseek(shp->stk, sizeof(Arith_t));
    if (nounset) sh_offoption(shp, SH_NOUNSET);
    if (!expr(&cur, 0) && cur.errmsg.value) {
        if (cur.errstr) string = cur.errstr;
        if ((*fun)(&string, &cur.errmsg, MESSAGE, 0) < 0) {
            stkseek(shp->stk, 0);
            *last = (char *)Empty;
            if (nounset) sh_onoption(shp, SH_NOUNSET);
            return NULL;
        }
        cur.nextchr = cur.errchr;
    }
    sfputc(shp->stk, 0);
    offset = stktell(shp->stk);
    ep = (Arith_t *)stkfreeze(shp->stk, 0);
    ep->shp = shp;
    ep->expr = string;
    ep->elen = (short)strlen(string);
    ep->code = (unsigned char *)(ep + 1);
    ep->fun = fun;
    ep->emode = emode;
    ep->size = offset - sizeof(Arith_t);
    ep->staksize = cur.stakmaxsize + 1;
    if (last) *last = (char *)(cur.nextchr);
    if (nounset) sh_onoption(shp, SH_NOUNSET);
    return ep;
}

//
// Evaluate an integer arithmetic expression in s.
//
// (Sfdouble_t)(*convert)(char** end, struct lval* string, int type, Sfdouble_t value)
//     is a user supplied conversion routine that is called when unknown
//     chars are encountered.
// *end points to the part to be converted and must be adjusted by convert to point to the next
// non-converted character; if typ is MESSAGE then string points to an error message string.
//
// NOTE: (*convert)() may call strval().
//
Sfdouble_t strval(Shell_t *shp, const char *s, char **end,
                  Sfdouble_t (*conv)(const char **, struct lval *, int, Sfdouble_t), int emode) {
    Arith_t *ep;
    Sfdouble_t d;
    char *sp = NULL;
    int offset;

    offset = stktell(shp->stk);
    if (offset) sp = stkfreeze(shp->stk, 1);
    ep = arith_compile(shp, s, end, conv, emode);
    ep->emode = emode;
    d = arith_exec(ep);
    stkset(shp->stk, sp ? sp : (char *)ep, offset);
    return d;
}
