/*
** $Id: lparser.c,v 2.42.1.3 2007/12/28 15:32:23 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/


#include <string.h>

#define lparser_c
#define LUA_CORE

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"



#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)

#define getlocvar(fs, i)	((fs)->f->locvars[(fs)->actvar[i]])

#define luaY_checklimit(fs,v,l,m)	if ((v)>(l)) errorlimit(fs,l,m)


/*
** nodes for block list (list of active blocks)
*/
typedef struct BlockCnt {
  struct BlockCnt *previous;  /* chain */
  int breaklist;  /* list of jumps out of this loop */
  lu_byte nactvar;  /* # active locals outside the breakable structure */
  lu_byte upval;  /* true if some variable in the block is an upvalue */
  lu_byte isbreakable;  /* true if `block' is a loop */
} BlockCnt;



/*
** prototypes for recursive non-terminal functions
*/
static void chunk (LexState *ls);
static void expr (LexState *ls, expdesc *v);


static void anchor_token (LexState *ls) {
  if (ls->t.token == TK_NAME || ls->t.token == TK_STRING) {
    TString *ts = ls->t.seminfo.ts;
    luaX_newstring(ls, getstr(ts), ts->tsv.len);
  }
}


static void error_expected (LexState *ls, int token) {
  luaX_syntaxerror(ls,
      luaO_pushfstring(ls->L, LUA_QS " expected", luaX_token2str(ls, token)));
}


static void errorlimit (FuncState *fs, int limit, const char *what) {
  const char *msg = (fs->f->linedefined == 0) ?
    luaO_pushfstring(fs->L, "main function has more than %d %s", limit, what) :
    luaO_pushfstring(fs->L, "function at line %d has more than %d %s",
                            fs->f->linedefined, limit, what);
  luaX_lexerror(fs->ls, msg, 0);
}


static int testnext (LexState *ls, int c) {
  if (ls->t.token == c) {
    luaX_next(ls);
    return 1;
  }
  else return 0;
}


static void check (LexState *ls, int c) {
  if (ls->t.token != c)
    error_expected(ls, c);
}

static void checknext (LexState *ls, int c) {
  check(ls, c);
  LUAI_ERRORCHECK()
  luaX_next(ls);
}


#define check_condition(ls,c,msg)	{ if (!(c)) luaX_syntaxerror(ls, msg); }



static void check_match (LexState *ls, int what, int who, int where) {
  if (!testnext(ls, what)) {
	LUAI_ERRORCHECK()
    if (where == ls->linenumber) {
      error_expected(ls, what);
      LUAI_ERRORCHECK()
    }
    else {
      luaX_syntaxerror(ls, luaO_pushfstring(ls->L,
             LUA_QS " expected (to close " LUA_QS " at line %d)",
              luaX_token2str(ls, what), luaX_token2str(ls, who), where));
    }
  }
}


static TString *str_checkname (LexState *ls) {
  TString *ts;
  check(ls, TK_NAME);
  ts = ls->t.seminfo.ts;
  luaX_next(ls);
  return ts;
}


static void init_exp (expdesc *e, expkind k, int i) {
  e->f = e->t = NO_JUMP;
  e->k = k;
  e->u.s.info = i;
}


static void codestring (LexState *ls, expdesc *e, TString *s) {
  init_exp(e, VK, luaK_stringK(ls->fs, s));
}


static void checkname(LexState *ls, expdesc *e) {
  codestring(ls, e, str_checkname(ls));
}


static int registerlocalvar (LexState *ls, TString *varname) {
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int oldsize = f->sizelocvars;
  luaM_growvector(ls->L, f->locvars, fs->nlocvars, f->sizelocvars,
                  LocVar, SHRT_MAX, "too many local variables");
  LUAI_ERRORCHECK(0)
  while (oldsize < f->sizelocvars) f->locvars[oldsize++].varname = NULL;
  f->locvars[fs->nlocvars].varname = varname;
  luaC_objbarrier(ls->L, f, varname);
  LUAI_ERRORCHECK(0)
  return fs->nlocvars++;
}


#define new_localvarliteral(ls,v,n) \
  new_localvar(ls, luaX_newstring(ls, "" v, (sizeof(v)/sizeof(char))-1), n)


static void new_localvar (LexState *ls, TString *name, int n) {
  FuncState *fs = ls->fs;
  luaY_checklimit(fs, fs->nactvar+n+1, LUAI_MAXVARS, "local variables");
  LUAI_ERRORCHECK()
  fs->actvar[fs->nactvar+n] = cast(unsigned short, registerlocalvar(ls, name));
}


static void adjustlocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  fs->nactvar = cast_byte(fs->nactvar + nvars);
  LUAI_ERRORCHECK()
  for (; nvars; nvars--) {
    getlocvar(fs, fs->nactvar - nvars).startpc = fs->pc;
    LUAI_ERRORCHECK()
  }
}


static void removevars (LexState *ls, int tolevel) {
  FuncState *fs = ls->fs;
  while (fs->nactvar > tolevel) {
    getlocvar(fs, --fs->nactvar).endpc = fs->pc;
    LUAI_ERRORCHECK()
  }
}


static int indexupvalue (FuncState *fs, TString *name, expdesc *v) {
  int i;
  Proto *f = fs->f;
  int oldsize = f->sizeupvalues;
  for (i=0; i<f->nups; i++) {
    if (fs->upvalues[i].k == v->k && fs->upvalues[i].info == v->u.s.info) {
      lua_assert(f->upvalues[i] == name);
      return i;
    }
  }
  /* new one */
  luaY_checklimit(fs, f->nups + 1, LUAI_MAXUPVALUES, "upvalues");
  LUAI_ERRORCHECK(0)
  luaM_growvector(fs->L, f->upvalues, f->nups, f->sizeupvalues,
                  TString *, MAX_INT, "");
  LUAI_ERRORCHECK(0)
  while (oldsize < f->sizeupvalues) f->upvalues[oldsize++] = NULL;
  f->upvalues[f->nups] = name;
  luaC_objbarrier(fs->L, f, name);
  LUAI_ERRORCHECK(0)
  lua_assert(v->k == VLOCAL || v->k == VUPVAL);
  LUAI_ERRORCHECK(0)
  fs->upvalues[f->nups].k = cast_byte(v->k);
  fs->upvalues[f->nups].info = cast_byte(v->u.s.info);
  return f->nups++;
}


static int searchvar (FuncState *fs, TString *n) {
  int i;
  for (i=fs->nactvar-1; i >= 0; i--) {
    if (n == getlocvar(fs, i).varname)
      return i;
    LUAI_ERRORCHECK()
  }
  return -1;  /* not found */
}


static void markupval (FuncState *fs, int level) {
  BlockCnt *bl = fs->bl;
  while (bl && bl->nactvar > level) bl = bl->previous;
  if (bl) bl->upval = 1;
}


static int singlevaraux (FuncState *fs, TString *n, expdesc *var, int base) {
  if (fs == NULL) {  /* no more levels? */
    init_exp(var, VGLOBAL, NO_REG);  /* default is global variable */
    return VGLOBAL;
  }
  else {
    int v = searchvar(fs, n);  /* look up at current level */
    LUAI_ERRORCHECK(0)
    if (v >= 0) {
      init_exp(var, VLOCAL, v);
      LUAI_ERRORCHECK(0)
      if (!base)
        markupval(fs, v);  /* local will be used as an upval */
      return VLOCAL;
    }
    else {  /* not found at current level; try upper one */
      if (singlevaraux(fs->prev, n, var, 0) == VGLOBAL)
        return VGLOBAL;
      LUAI_ERRORCHECK(0);
      var->u.s.info = indexupvalue(fs, n, var);  /* else was LOCAL or UPVAL */
      var->k = VUPVAL;  /* upvalue in this level */
      return VUPVAL;
    }
  }
}


static void singlevar (LexState *ls, expdesc *var) {
  TString *varname = str_checkname(ls);
  FuncState *fs = ls->fs;
  if (singlevaraux(fs, varname, var, 1) == VGLOBAL) {
	LUAI_ERRORCHECK()
    var->u.s.info = luaK_stringK(fs, varname);  /* info points to global name */
  }
}


static void adjust_assign (LexState *ls, int nvars, int nexps, expdesc *e) {
  FuncState *fs = ls->fs;
  int extra = nvars - nexps;
  if (hasmultret(e->k)) {
    extra++;  /* includes call itself */
    if (extra < 0) extra = 0;
    luaK_setreturns(fs, e, extra);  /* last exp. provides the difference */
    LUAI_ERRORCHECK()
    if (extra > 1) luaK_reserveregs(fs, extra-1);
    LUAI_ERRORCHECK()
  }
  else {
    if (e->k != VVOID) luaK_exp2nextreg(fs, e);  /* close last expression */
    LUAI_ERRORCHECK()
    if (extra > 0) {
      int reg = fs->freereg;
      luaK_reserveregs(fs, extra);
      LUAI_ERRORCHECK()
      luaK_nil(fs, reg, extra);
    }
  }
}


static void enterlevel (LexState *ls) {
  if (++ls->L->nCcalls > LUAI_MAXCCALLS)
	luaX_lexerror(ls, "chunk has too many syntax levels", 0);
}


#define leavelevel(ls)	((ls)->L->nCcalls--)


static void enterblock (FuncState *fs, BlockCnt *bl, lu_byte isbreakable) {
  bl->breaklist = NO_JUMP;
  bl->isbreakable = isbreakable;
  bl->nactvar = fs->nactvar;
  bl->upval = 0;
  bl->previous = fs->bl;
  fs->bl = bl;
  lua_assert(fs->freereg == fs->nactvar);
}


static void leaveblock (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  fs->bl = bl->previous;
  removevars(fs->ls, bl->nactvar);
  LUAI_ERRORCHECK()
  if (bl->upval)
    luaK_codeABC(fs, OP_CLOSE, bl->nactvar, 0, 0);
  LUAI_ERRORCHECK()
  /* a block either controls scope or breaks (never both) */
  lua_assert(!bl->isbreakable || !bl->upval);
  LUAI_ERRORCHECK()
  lua_assert(bl->nactvar == fs->nactvar);
  LUAI_ERRORCHECK()
  fs->freereg = fs->nactvar;  /* free registers */
  luaK_patchtohere(fs, bl->breaklist);
}


static void pushclosure (LexState *ls, FuncState *func, expdesc *v) {
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int oldsize = f->sizep;
  int i;
  luaM_growvector(ls->L, f->p, fs->np, f->sizep, Proto *,
                  MAXARG_Bx, "constant table overflow");
  LUAI_ERRORCHECK()
  while (oldsize < f->sizep) f->p[oldsize++] = NULL;
  f->p[fs->np++] = func->f;
  luaC_objbarrier(ls->L, f, func->f);
  LUAI_ERRORCHECK()
  init_exp(v, VRELOCABLE, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np-1));
  LUAI_ERRORCHECK()
  for (i=0; i<func->f->nups; i++) {
    OpCode o = (func->upvalues[i].k == VLOCAL) ? OP_MOVE : OP_GETUPVAL;
    luaK_codeABC(fs, o, 0, func->upvalues[i].info, 0);
    LUAI_ERRORCHECK()
  }
}


static void open_func (LexState *ls, FuncState *fs) {
  lua_State *L = ls->L;
  Proto *f = luaF_newproto(L);
  LUAI_ERRORCHECK()
  fs->f = f;
  fs->prev = ls->fs;  /* linked list of funcstates */
  fs->ls = ls;
  fs->L = L;
  ls->fs = fs;
  fs->pc = 0;
  fs->lasttarget = -1;
  fs->jpc = NO_JUMP;
  fs->freereg = 0;
  fs->nk = 0;
  fs->np = 0;
  fs->nlocvars = 0;
  fs->nactvar = 0;
  fs->bl = NULL;
  f->source = ls->source;
  f->maxstacksize = 2;  /* registers 0/1 are always valid */
  fs->h = luaH_new(L, 0, 0);
  LUAI_ERRORCHECK()
  /* anchor table of constants and prototype (to avoid being collected) */
  sethvalue2s(L, L->top, fs->h);
  LUAI_ERRORCHECK()
  incr_top(L);
  LUAI_ERRORCHECK()
  setptvalue2s(L, L->top, f);
  LUAI_ERRORCHECK()
  incr_top(L);
}


static void close_func (LexState *ls) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  removevars(ls, 0);
  LUAI_ERRORCHECK()
  luaK_ret(fs, 0, 0);  /* final return */
  LUAI_ERRORCHECK()
  luaM_reallocvector(L, f->code, f->sizecode, fs->pc, Instruction);
  LUAI_ERRORCHECK()
  f->sizecode = fs->pc;
  luaM_reallocvector(L, f->lineinfo, f->sizelineinfo, fs->pc, int);
  LUAI_ERRORCHECK()
  f->sizelineinfo = fs->pc;
  luaM_reallocvector(L, f->k, f->sizek, fs->nk, TValue);
  LUAI_ERRORCHECK()
  f->sizek = fs->nk;
  luaM_reallocvector(L, f->p, f->sizep, fs->np, Proto *);
  LUAI_ERRORCHECK()
  f->sizep = fs->np;
  luaM_reallocvector(L, f->locvars, f->sizelocvars, fs->nlocvars, LocVar);
  LUAI_ERRORCHECK()
  f->sizelocvars = fs->nlocvars;
  luaM_reallocvector(L, f->upvalues, f->sizeupvalues, f->nups, TString *);
  LUAI_ERRORCHECK()
  f->sizeupvalues = f->nups;
  lua_assert(luaG_checkcode(f));
  LUAI_ERRORCHECK()
  lua_assert(fs->bl == NULL);
  LUAI_ERRORCHECK()
  ls->fs = fs->prev;
  L->top -= 2;  /* remove table and prototype from the stack */
  /* last token read was anchored in defunct function; must reanchor it */
  if (fs) anchor_token(ls);
}


Proto *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff, const char *name) {
  struct LexState lexstate;
  struct FuncState funcstate;
  lexstate.buff = buff;
  luaX_setinput(L, &lexstate, z, luaS_new(L, name));
  LUAI_ERRORCHECK(NULL)
  open_func(&lexstate, &funcstate);
  LUAI_ERRORCHECK(NULL)
  funcstate.f->is_vararg = VARARG_ISVARARG;  /* main func. is always vararg */
  luaX_next(&lexstate);  /* read first token */
  LUAI_ERRORCHECK(NULL)
  chunk(&lexstate);
  LUAI_ERRORCHECK(NULL)
  check(&lexstate, TK_EOS);
  LUAI_ERRORCHECK(NULL)
  close_func(&lexstate);
  LUAI_ERRORCHECK(NULL)
  lua_assert(funcstate.prev == NULL);
  LUAI_ERRORCHECK(NULL)
  lua_assert(funcstate.f->nups == 0);
  LUAI_ERRORCHECK(NULL)
  lua_assert(lexstate.fs == NULL);
  return funcstate.f;
}



/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/


static void field (LexState *ls, expdesc *v) {
  /* field -> ['.' | ':'] NAME */
  FuncState *fs = ls->fs;
  expdesc key;
  luaK_exp2anyreg(fs, v);
  LUAI_ERRORCHECK()
  luaX_next(ls);  /* skip the dot or colon */
  LUAI_ERRORCHECK()
  checkname(ls, &key);
  LUAI_ERRORCHECK()
  luaK_indexed(fs, v, &key);
}


static void yindex (LexState *ls, expdesc *v) {
  /* index -> '[' expr ']' */
  luaX_next(ls);  /* skip the '[' */
  LUAI_ERRORCHECK()
  expr(ls, v);
  LUAI_ERRORCHECK()
  luaK_exp2val(ls->fs, v);
  LUAI_ERRORCHECK()
  checknext(ls, ']');
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/


struct ConsControl {
  expdesc v;  /* last list item read */
  expdesc *t;  /* table descriptor */
  int nh;  /* total number of `record' elements */
  int na;  /* total number of array elements */
  int tostore;  /* number of array elements pending to be stored */
};


static void recfield (LexState *ls, struct ConsControl *cc) {
  /* recfield -> (NAME | `['exp1`]') = exp1 */
  FuncState *fs = ls->fs;
  int reg = ls->fs->freereg;
  expdesc key, val;
  int rkkey;
  if (ls->t.token == TK_NAME) {
    luaY_checklimit(fs, cc->nh, MAX_INT, "items in a constructor");
    LUAI_ERRORCHECK()
    checkname(ls, &key);
  }
  else  /* ls->t.token == '[' */
    yindex(ls, &key);
  LUAI_ERRORCHECK()
  cc->nh++;
  checknext(ls, '=');
  LUAI_ERRORCHECK()
  rkkey = luaK_exp2RK(fs, &key);
  LUAI_ERRORCHECK()
  expr(ls, &val);
  LUAI_ERRORCHECK()
  luaK_codeABC(fs, OP_SETTABLE, cc->t->u.s.info, rkkey, luaK_exp2RK(fs, &val));
  LUAI_ERRORCHECK()
  fs->freereg = reg;  /* free registers */
}


static void closelistfield (FuncState *fs, struct ConsControl *cc) {
  if (cc->v.k == VVOID) return;  /* there is no list item */
  luaK_exp2nextreg(fs, &cc->v);
  LUAI_ERRORCHECK()
  cc->v.k = VVOID;
  if (cc->tostore == LFIELDS_PER_FLUSH) {
    luaK_setlist(fs, cc->t->u.s.info, cc->na, cc->tostore);  /* flush */
    LUAI_ERRORCHECK()
    cc->tostore = 0;  /* no more items pending */
  }
}


static void lastlistfield (FuncState *fs, struct ConsControl *cc) {
  if (cc->tostore == 0) return;
  if (hasmultret(cc->v.k)) {
    luaK_setmultret(fs, &cc->v);
    LUAI_ERRORCHECK()
    luaK_setlist(fs, cc->t->u.s.info, cc->na, LUA_MULTRET);
    LUAI_ERRORCHECK()
    cc->na--;  /* do not count last expression (unknown number of elements) */
  }
  else {
    if (cc->v.k != VVOID)
      luaK_exp2nextreg(fs, &cc->v);
    LUAI_ERRORCHECK()
    luaK_setlist(fs, cc->t->u.s.info, cc->na, cc->tostore);
  }
}


static void listfield (LexState *ls, struct ConsControl *cc) {
  expr(ls, &cc->v);
  luaY_checklimit(ls->fs, cc->na, MAX_INT, "items in a constructor");
  LUAI_ERRORCHECK()
  cc->na++;
  cc->tostore++;
}


static void constructor (LexState *ls, expdesc *t) {
  /* constructor -> ?? */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int pc = luaK_codeABC(fs, OP_NEWTABLE, 0, 0, 0);
  LUAI_ERRORCHECK()
  struct ConsControl cc;
  cc.na = cc.nh = cc.tostore = 0;
  cc.t = t;
  init_exp(t, VRELOCABLE, pc);
  LUAI_ERRORCHECK()
  init_exp(&cc.v, VVOID, 0);  /* no value (yet) */
  LUAI_ERRORCHECK()
  luaK_exp2nextreg(ls->fs, t);  /* fix it at stack top (for gc) */
  LUAI_ERRORCHECK()
  checknext(ls, '{');
  LUAI_ERRORCHECK()
  do {
    lua_assert(cc.v.k == VVOID || cc.tostore > 0);
    LUAI_ERRORCHECK()
    if (ls->t.token == '}') break;
    closelistfield(fs, &cc);
    LUAI_ERRORCHECK()
    switch(ls->t.token) {
      case TK_NAME: {  /* may be listfields or recfields */
        luaX_lookahead(ls);
        LUAI_ERRORCHECK()
        if (ls->lookahead.token != '=')  /* expression? */
          listfield(ls, &cc);
        else
          recfield(ls, &cc);
        LUAI_ERRORCHECK()
        break;
      }
      case '[': {  /* constructor_item -> recfield */
        recfield(ls, &cc);
        LUAI_ERRORCHECK()
        break;
      }
      default: {  /* constructor_part -> listfield */
        listfield(ls, &cc);
        LUAI_ERRORCHECK()
        break;
      }
    }
  } while (testnext(ls, ',') || testnext(ls, ';'));
  check_match(ls, '}', '{', line);
  LUAI_ERRORCHECK()
  lastlistfield(fs, &cc);
  LUAI_ERRORCHECK()
  SETARG_B(fs->f->code[pc], luaO_int2fb(cc.na)); /* set initial array size */
  LUAI_ERRORCHECK()
  SETARG_C(fs->f->code[pc], luaO_int2fb(cc.nh));  /* set initial table size */
}

/* }====================================================================== */



static void parlist (LexState *ls) {
  /* parlist -> [ param { `,' param } ] */
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int nparams = 0;
  f->is_vararg = 0;
  if (ls->t.token != ')') {  /* is `parlist' not empty? */
    do {
      LUAI_ERRORCHECK()
      switch (ls->t.token) {
        case TK_NAME: {  /* param -> NAME */
          new_localvar(ls, str_checkname(ls), nparams++);
          LUAI_ERRORCHECK()
          break;
        }
        case TK_DOTS: {  /* param -> `...' */
          luaX_next(ls);
          LUAI_ERRORCHECK()
#if defined(LUA_COMPAT_VARARG)
          /* use `arg' as default name */
          new_localvarliteral(ls, "arg", nparams++);
          LUAI_ERRORCHECK()
          f->is_vararg = VARARG_HASARG | VARARG_NEEDSARG;
#endif
          f->is_vararg |= VARARG_ISVARARG;
          break;
        }
        default:
          luaX_syntaxerror(ls, "<name> or " LUA_QL("...") " expected");
          LUAI_ERRORCHECK()
      }
    } while (!f->is_vararg && testnext(ls, ','));
  }
  adjustlocalvars(ls, nparams);
  LUAI_ERRORCHECK()
  f->numparams = cast_byte(fs->nactvar - (f->is_vararg & VARARG_HASARG));
  LUAI_ERRORCHECK()
  luaK_reserveregs(fs, fs->nactvar);  /* reserve register for parameters */
}


static void body (LexState *ls, expdesc *e, int needself, int line) {
  /* body ->  `(' parlist `)' chunk END */
  FuncState new_fs;
  open_func(ls, &new_fs);
  LUAI_ERRORCHECK()
  new_fs.f->linedefined = line;
  checknext(ls, '(');
  LUAI_ERRORCHECK()
  if (needself) {
    new_localvarliteral(ls, "self", 0);
    LUAI_ERRORCHECK()
    adjustlocalvars(ls, 1);
    LUAI_ERRORCHECK()
  }
  parlist(ls);
  LUAI_ERRORCHECK()
  checknext(ls, ')');
  LUAI_ERRORCHECK()
  chunk(ls);
  LUAI_ERRORCHECK()
  new_fs.f->lastlinedefined = ls->linenumber;
  check_match(ls, TK_END, TK_FUNCTION, line);
  LUAI_ERRORCHECK()
  close_func(ls);
  LUAI_ERRORCHECK()
  pushclosure(ls, &new_fs, e);
}


static int explist1 (LexState *ls, expdesc *v) {
  /* explist1 -> expr { `,' expr } */
  int n = 1;  /* at least one expression */
  expr(ls, v);
  LUAI_ERRORCHECK(0)
  while (testnext(ls, ',')) {
    luaK_exp2nextreg(ls->fs, v);
    LUAI_ERRORCHECK(0)
    expr(ls, v);
    LUAI_ERRORCHECK(0)
    n++;
  }
  return n;
}


static void funcargs (LexState *ls, expdesc *f) {
  FuncState *fs = ls->fs;
  expdesc args;
  int base, nparams;
  int line = ls->linenumber;
  switch (ls->t.token) {
    case '(': {  /* funcargs -> `(' [ explist1 ] `)' */
      if (line != ls->lastline)
        luaX_syntaxerror(ls,"ambiguous syntax (function call x new statement)");
      LUAI_ERRORCHECK()
      luaX_next(ls);
      LUAI_ERRORCHECK()
      if (ls->t.token == ')')  /* arg list is empty? */
        args.k = VVOID;
      else {
        explist1(ls, &args);
        LUAI_ERRORCHECK()
        luaK_setmultret(fs, &args);
        LUAI_ERRORCHECK()
      }
      check_match(ls, ')', '(', line);
      LUAI_ERRORCHECK()
      break;
    }
    case '{': {  /* funcargs -> constructor */
      constructor(ls, &args);
      LUAI_ERRORCHECK()
      break;
    }
    case TK_STRING: {  /* funcargs -> STRING */
      codestring(ls, &args, ls->t.seminfo.ts);
      LUAI_ERRORCHECK()
      luaX_next(ls);  /* must use `seminfo' before `next' */
      LUAI_ERRORCHECK()
      break;
    }
    default: {
      luaX_syntaxerror(ls, "function arguments expected");
      return;
    }
  }
  LUAI_ERRORCHECK()
  lua_assert(f->k == VNONRELOC);
  LUAI_ERRORCHECK()
  base = f->u.s.info;  /* base register for call */
  if (hasmultret(args.k)) {
	LUAI_ERRORCHECK()
    nparams = LUA_MULTRET;  /* open call */
  }
  else {
    if (args.k != VVOID)
      luaK_exp2nextreg(fs, &args);  /* close last argument */
    LUAI_ERRORCHECK()
    nparams = fs->freereg - (base+1);
  }
  LUAI_ERRORCHECK()
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams+1, 2));
  LUAI_ERRORCHECK()
  luaK_fixline(fs, line);
  LUAI_ERRORCHECK()
  fs->freereg = base+1;  /* call remove function and arguments and leaves
                            (unless changed) one result */
}




/*
** {======================================================================
** Expression parsing
** =======================================================================
*/


static void prefixexp (LexState *ls, expdesc *v) {
  /* prefixexp -> NAME | '(' expr ')' */
  switch (ls->t.token) {
    case '(': {
      int line = ls->linenumber;
      luaX_next(ls);
      LUAI_ERRORCHECK()
      expr(ls, v);
      LUAI_ERRORCHECK()
      check_match(ls, ')', '(', line);
      LUAI_ERRORCHECK()
      luaK_dischargevars(ls->fs, v);
      LUAI_ERRORCHECK()
      return;
    }
    case TK_NAME: {
      singlevar(ls, v);
      return;
    }
    default: {
      luaX_syntaxerror(ls, "unexpected symbol");
      return;
    }
  }
}


static void primaryexp (LexState *ls, expdesc *v) {
  /* primaryexp ->
        prefixexp { `.' NAME | `[' exp `]' | `:' NAME funcargs | funcargs } */
  FuncState *fs = ls->fs;
  prefixexp(ls, v);
  LUAI_ERRORCHECK()
  for (;;) {
    switch (ls->t.token) {
      case '.': {  /* field */
        field(ls, v);
        LUAI_ERRORCHECK()
        break;
      }
      case '[': {  /* `[' exp1 `]' */
        expdesc key;
        luaK_exp2anyreg(fs, v);
        LUAI_ERRORCHECK()
        yindex(ls, &key);
        LUAI_ERRORCHECK()
        luaK_indexed(fs, v, &key);
        LUAI_ERRORCHECK()
        break;
      }
      case ':': {  /* `:' NAME funcargs */
        expdesc key;
        luaX_next(ls);
        LUAI_ERRORCHECK()
        checkname(ls, &key);
        LUAI_ERRORCHECK()
        luaK_self(fs, v, &key);
        LUAI_ERRORCHECK()
        funcargs(ls, v);
        LUAI_ERRORCHECK()
        break;
      }
      case '(': case TK_STRING: case '{': {  /* funcargs */
        luaK_exp2nextreg(fs, v);
        LUAI_ERRORCHECK()
        funcargs(ls, v);
        LUAI_ERRORCHECK()
        break;
      }
      default: return;
    }
    LUAI_ERRORCHECK()
  }
}


static void simpleexp (LexState *ls, expdesc *v) {
  /* simpleexp -> NUMBER | STRING | NIL | true | false | ... |
                  constructor | FUNCTION body | primaryexp */
  switch (ls->t.token) {
    case TK_NUMBER: {
      init_exp(v, VKNUM, 0);
      LUAI_ERRORCHECK()
      v->u.nval = ls->t.seminfo.r;
      break;
    }
    case TK_STRING: {
      codestring(ls, v, ls->t.seminfo.ts);
      break;
    }
    case TK_NIL: {
      init_exp(v, VNIL, 0);
      break;
    }
    case TK_TRUE: {
      init_exp(v, VTRUE, 0);
      break;
    }
    case TK_FALSE: {
      init_exp(v, VFALSE, 0);
      break;
    }
    case TK_DOTS: {  /* vararg */
      FuncState *fs = ls->fs;
      check_condition(ls, fs->f->is_vararg,
                      "cannot use " LUA_QL("...") " outside a vararg function");
      LUAI_ERRORCHECK()
      fs->f->is_vararg &= ~VARARG_NEEDSARG;  /* don't need 'arg' */
      init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, 1, 0));
      break;
    }
    case '{': {  /* constructor */
      constructor(ls, v);
      return;
    }
    case TK_FUNCTION: {
      luaX_next(ls);
      LUAI_ERRORCHECK()
      body(ls, v, 0, ls->linenumber);
      return;
    }
    default: {
      primaryexp(ls, v);
      return;
    }
  }
  LUAI_ERRORCHECK()
  luaX_next(ls);
}


static UnOpr getunopr (int op) {
  switch (op) {
    case TK_NOT: return OPR_NOT;
    case '-': return OPR_MINUS;
    case '#': return OPR_LEN;
    default: return OPR_NOUNOPR;
  }
}


static BinOpr getbinopr (int op) {
  switch (op) {
    case '+': return OPR_ADD;
    case '-': return OPR_SUB;
    case '*': return OPR_MUL;
    case '/': return OPR_DIV;
    case '%': return OPR_MOD;
    case '^': return OPR_POW;
    case TK_CONCAT: return OPR_CONCAT;
    case TK_NE: return OPR_NE;
    case TK_EQ: return OPR_EQ;
    case '<': return OPR_LT;
    case TK_LE: return OPR_LE;
    case '>': return OPR_GT;
    case TK_GE: return OPR_GE;
    case TK_AND: return OPR_AND;
    case TK_OR: return OPR_OR;
    default: return OPR_NOBINOPR;
  }
}


static const struct {
  lu_byte left;  /* left priority for each binary operator */
  lu_byte right; /* right priority */
} priority[] = {  /* ORDER OPR */
   {6, 6}, {6, 6}, {7, 7}, {7, 7}, {7, 7},  /* `+' `-' `/' `%' */
   {10, 9}, {5, 4},                 /* power and concat (right associative) */
   {3, 3}, {3, 3},                  /* equality and inequality */
   {3, 3}, {3, 3}, {3, 3}, {3, 3},  /* order */
   {2, 2}, {1, 1}                   /* logical (and/or) */
};

#define UNARY_PRIORITY	8  /* priority for unary operators */


/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where `binop' is any binary operator with a priority higher than `limit'
*/
static BinOpr subexpr (LexState *ls, expdesc *v, unsigned int limit) {
  BinOpr op;
  UnOpr uop;
  enterlevel(ls);
  LUAI_ERRORCHECK(op)
  uop = getunopr(ls->t.token);
  LUAI_ERRORCHECK(op)
  if (uop != OPR_NOUNOPR) {
    luaX_next(ls);
    LUAI_ERRORCHECK(op)
    subexpr(ls, v, UNARY_PRIORITY);
    LUAI_ERRORCHECK(op)
    luaK_prefix(ls->fs, uop, v);
  }
  else simpleexp(ls, v);
  LUAI_ERRORCHECK(op)
  /* expand while operators have priorities higher than `limit' */
  op = getbinopr(ls->t.token);
  LUAI_ERRORCHECK(op)
  while (op != OPR_NOBINOPR && priority[op].left > limit) {
    expdesc v2;
    BinOpr nextop;
    luaX_next(ls);
    LUAI_ERRORCHECK(op)
    luaK_infix(ls->fs, op, v);
    LUAI_ERRORCHECK(op)
    /* read sub-expression with higher priority */
    nextop = subexpr(ls, &v2, priority[op].right);
    LUAI_ERRORCHECK(op)
    luaK_posfix(ls->fs, op, v, &v2);
    LUAI_ERRORCHECK(op)
    op = nextop;
  }
  leavelevel(ls);
  return op;  /* return first untreated operator */
}


static void expr (LexState *ls, expdesc *v) {
  subexpr(ls, v, 0);
}

/* }==================================================================== */



/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/


static int block_follow (int token) {
  switch (token) {
    case TK_ELSE: case TK_ELSEIF: case TK_END:
    case TK_UNTIL: case TK_EOS:
      return 1;
    default: return 0;
  }
}


static void block (LexState *ls) {
  /* block -> chunk */
  FuncState *fs = ls->fs;
  BlockCnt bl;
  enterblock(fs, &bl, 0);
  LUAI_ERRORCHECK()
  chunk(ls);
  LUAI_ERRORCHECK()
  lua_assert(bl.breaklist == NO_JUMP);
  LUAI_ERRORCHECK()
  leaveblock(fs);
}


/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
struct LHS_assign {
  struct LHS_assign *prev;
  expdesc v;  /* variable (global, local, upvalue, or indexed) */
};


/*
** check whether, in an assignment to a local variable, the local variable
** is needed in a previous assignment (to a table). If so, save original
** local value in a safe place and use this safe copy in the previous
** assignment.
*/
static void check_conflict (LexState *ls, struct LHS_assign *lh, expdesc *v) {
  FuncState *fs = ls->fs;
  int extra = fs->freereg;  /* eventual position to save local variable */
  int conflict = 0;
  for (; lh; lh = lh->prev) {
    if (lh->v.k == VINDEXED) {
      if (lh->v.u.s.info == v->u.s.info) {  /* conflict? */
        conflict = 1;
        lh->v.u.s.info = extra;  /* previous assignment will use safe copy */
      }
      if (lh->v.u.s.aux == v->u.s.info) {  /* conflict? */
        conflict = 1;
        lh->v.u.s.aux = extra;  /* previous assignment will use safe copy */
      }
    }
  }
  if (conflict) {
    luaK_codeABC(fs, OP_MOVE, fs->freereg, v->u.s.info, 0);  /* make copy */
    LUAI_ERRORCHECK()
    luaK_reserveregs(fs, 1);
  }
}


static void assignment (LexState *ls, struct LHS_assign *lh, int nvars) {
  expdesc e;
  check_condition(ls, VLOCAL <= lh->v.k && lh->v.k <= VINDEXED,
                      "syntax error");
  LUAI_ERRORCHECK()
  if (testnext(ls, ',')) {  /* assignment -> `,' primaryexp assignment */
	LUAI_ERRORCHECK()
    struct LHS_assign nv;
    nv.prev = lh;
    primaryexp(ls, &nv.v);
    LUAI_ERRORCHECK()
    if (nv.v.k == VLOCAL)
      check_conflict(ls, lh, &nv.v);
    LUAI_ERRORCHECK()
    luaY_checklimit(ls->fs, nvars, LUAI_MAXCCALLS - ls->L->nCcalls,
                    "variables in assignment");
    LUAI_ERRORCHECK()
    assignment(ls, &nv, nvars+1);
    LUAI_ERRORCHECK()
  }
  else {  /* assignment -> `=' explist1 */
    int nexps;
    checknext(ls, '=');
    LUAI_ERRORCHECK()
    nexps = explist1(ls, &e);
    LUAI_ERRORCHECK()
    if (nexps != nvars) {
      adjust_assign(ls, nvars, nexps, &e);
      LUAI_ERRORCHECK()
      if (nexps > nvars)
        ls->fs->freereg -= nexps - nvars;  /* remove extra values */
    }
    else {
      luaK_setoneret(ls->fs, &e);  /* close last expression */
      LUAI_ERRORCHECK()
      luaK_storevar(ls->fs, &lh->v, &e);
      return;  /* avoid default */
    }
  }
  init_exp(&e, VNONRELOC, ls->fs->freereg-1);  /* default assignment */
  LUAI_ERRORCHECK()
  luaK_storevar(ls->fs, &lh->v, &e);
}


static int cond (LexState *ls) {
  /* cond -> exp */
  expdesc v;
  expr(ls, &v);  /* read condition */
  LUAI_ERRORCHECK(0)
  if (v.k == VNIL) v.k = VFALSE;  /* `falses' are all equal here */
  luaK_goiftrue(ls->fs, &v);
  return v.f;
}


static void breakstat (LexState *ls) {
  FuncState *fs = ls->fs;
  BlockCnt *bl = fs->bl;
  int upval = 0;
  while (bl && !bl->isbreakable) {
    upval |= bl->upval;
    bl = bl->previous;
  }
  if (!bl)
    luaX_syntaxerror(ls, "no loop to break");
  if (upval)
    luaK_codeABC(fs, OP_CLOSE, bl->nactvar, 0, 0);
  LUAI_ERRORCHECK()
  luaK_concat(fs, &bl->breaklist, luaK_jump(fs));
}


static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE cond DO block END */
  FuncState *fs = ls->fs;
  int whileinit;
  int condexit;
  BlockCnt bl;
  luaX_next(ls);  /* skip WHILE */
  LUAI_ERRORCHECK()
  whileinit = luaK_getlabel(fs);
  LUAI_ERRORCHECK()
  condexit = cond(ls);
  LUAI_ERRORCHECK()
  enterblock(fs, &bl, 1);
  LUAI_ERRORCHECK()
  checknext(ls, TK_DO);
  LUAI_ERRORCHECK()
  block(ls);
  LUAI_ERRORCHECK()
  luaK_patchlist(fs, luaK_jump(fs), whileinit);
  LUAI_ERRORCHECK()
  check_match(ls, TK_END, TK_WHILE, line);
  LUAI_ERRORCHECK()
  leaveblock(fs);
  LUAI_ERRORCHECK()
  luaK_patchtohere(fs, condexit);  /* false conditions finish the loop */
}


static void repeatstat (LexState *ls, int line) {
  /* repeatstat -> REPEAT block UNTIL cond */
  int condexit;
  FuncState *fs = ls->fs;
  int repeat_init = luaK_getlabel(fs);
  LUAI_ERRORCHECK()
  BlockCnt bl1, bl2;
  enterblock(fs, &bl1, 1);  /* loop block */
  LUAI_ERRORCHECK()
  enterblock(fs, &bl2, 0);  /* scope block */
  LUAI_ERRORCHECK()
  luaX_next(ls);  /* skip REPEAT */
  LUAI_ERRORCHECK()
  chunk(ls);
  LUAI_ERRORCHECK()
  check_match(ls, TK_UNTIL, TK_REPEAT, line);
  LUAI_ERRORCHECK()
  condexit = cond(ls);  /* read condition (inside scope block) */
  LUAI_ERRORCHECK()
  if (!bl2.upval) {  /* no upvalues? */
    leaveblock(fs);  /* finish scope */
    LUAI_ERRORCHECK()
    luaK_patchlist(ls->fs, condexit, repeat_init);  /* close the loop */
  }
  else {  /* complete semantics when there are upvalues */
    breakstat(ls);  /* if condition then break */
    LUAI_ERRORCHECK()
    luaK_patchtohere(ls->fs, condexit);  /* else... */
    LUAI_ERRORCHECK()
    leaveblock(fs);  /* finish scope... */
    LUAI_ERRORCHECK()
    luaK_patchlist(ls->fs, luaK_jump(fs), repeat_init);  /* and repeat */
  }
  LUAI_ERRORCHECK()
  leaveblock(fs);  /* finish loop */
}


static int exp1 (LexState *ls) {
  expdesc e;
  int k;
  expr(ls, &e);
  LUAI_ERRORCHECK(0)
  k = e.k;
  luaK_exp2nextreg(ls->fs, &e);
  return k;
}


static void forbody (LexState *ls, int base, int line, int nvars, int isnum) {
  /* forbody -> DO block */
  BlockCnt bl;
  FuncState *fs = ls->fs;
  int prep, endfor;
  adjustlocalvars(ls, 3);  /* control variables */
  LUAI_ERRORCHECK()
  checknext(ls, TK_DO);
  LUAI_ERRORCHECK()
  prep = isnum ? luaK_codeAsBx(fs, OP_FORPREP, base, NO_JUMP) : luaK_jump(fs);
  LUAI_ERRORCHECK()
  enterblock(fs, &bl, 0);  /* scope for declared variables */
  LUAI_ERRORCHECK()
  adjustlocalvars(ls, nvars);
  LUAI_ERRORCHECK()
  luaK_reserveregs(fs, nvars);
  LUAI_ERRORCHECK()
  block(ls);
  LUAI_ERRORCHECK()
  leaveblock(fs);  /* end of scope for declared variables */
  luaK_patchtohere(fs, prep);
  LUAI_ERRORCHECK()
  endfor = (isnum) ? luaK_codeAsBx(fs, OP_FORLOOP, base, NO_JUMP) :
                     luaK_codeABC(fs, OP_TFORLOOP, base, 0, nvars);
  LUAI_ERRORCHECK()
  luaK_fixline(fs, line);  /* pretend that `OP_FOR' starts the loop */
  LUAI_ERRORCHECK()
  luaK_patchlist(fs, (isnum ? endfor : luaK_jump(fs)), prep + 1);
}


static void fornum (LexState *ls, TString *varname, int line) {
  /* fornum -> NAME = exp1,exp1[,exp1] forbody */
  FuncState *fs = ls->fs;
  int base = fs->freereg;
  new_localvarliteral(ls, "(for index)", 0);
  LUAI_ERRORCHECK()
  new_localvarliteral(ls, "(for limit)", 1);
  LUAI_ERRORCHECK()
  new_localvarliteral(ls, "(for step)", 2);
  LUAI_ERRORCHECK()
  new_localvar(ls, varname, 3);
  LUAI_ERRORCHECK()
  checknext(ls, '=');
  LUAI_ERRORCHECK()
  exp1(ls);  /* initial value */
  LUAI_ERRORCHECK()
  checknext(ls, ',');
  LUAI_ERRORCHECK()
  exp1(ls);  /* limit */
  LUAI_ERRORCHECK()
  if (testnext(ls, ','))
  {
	LUAI_ERRORCHECK()
    exp1(ls);  /* optional step */
  }
  else {  /* default step = 1 */
    luaK_codeABx(fs, OP_LOADK, fs->freereg, luaK_numberK(fs, 1));
    LUAI_ERRORCHECK()
    luaK_reserveregs(fs, 1);
  }
  LUAI_ERRORCHECK()
  forbody(ls, base, line, 1, 1);
}


static void forlist (LexState *ls, TString *indexname) {
  /* forlist -> NAME {,NAME} IN explist1 forbody */
  FuncState *fs = ls->fs;
  expdesc e;
  int nvars = 0;
  int line;
  int base = fs->freereg;
  /* create control variables */
  new_localvarliteral(ls, "(for generator)", nvars++);
  LUAI_ERRORCHECK()
  new_localvarliteral(ls, "(for state)", nvars++);
  LUAI_ERRORCHECK()
  new_localvarliteral(ls, "(for control)", nvars++);
  LUAI_ERRORCHECK()
  /* create declared variables */
  new_localvar(ls, indexname, nvars++);
  LUAI_ERRORCHECK()
  while (testnext(ls, ','))
    new_localvar(ls, str_checkname(ls), nvars++);
  LUAI_ERRORCHECK()
  checknext(ls, TK_IN);
  LUAI_ERRORCHECK()
  line = ls->linenumber;
  adjust_assign(ls, 3, explist1(ls, &e), &e);
  LUAI_ERRORCHECK()
  luaK_checkstack(fs, 3);  /* extra space to call generator */
  LUAI_ERRORCHECK()
  forbody(ls, base, line, nvars - 3, 0);
}


static void forstat (LexState *ls, int line) {
  /* forstat -> FOR (fornum | forlist) END */
  FuncState *fs = ls->fs;
  TString *varname;
  BlockCnt bl;
  enterblock(fs, &bl, 1);  /* scope for loop and control variables */
  LUAI_ERRORCHECK()
  luaX_next(ls);  /* skip `for' */
  LUAI_ERRORCHECK()
  varname = str_checkname(ls);  /* first variable name */
  LUAI_ERRORCHECK()
  switch (ls->t.token) {
    case '=': fornum(ls, varname, line); break;
    case ',': case TK_IN: forlist(ls, varname); break;
    default: luaX_syntaxerror(ls, LUA_QL("=") " or " LUA_QL("in") " expected");
  }
  LUAI_ERRORCHECK()
  check_match(ls, TK_END, TK_FOR, line);
  LUAI_ERRORCHECK()
  leaveblock(fs);  /* loop scope (`break' jumps to this point) */
}


static int test_then_block (LexState *ls) {
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  int condexit;
  luaX_next(ls);  /* skip IF or ELSEIF */
  LUAI_ERRORCHECK(0)
  condexit = cond(ls);
  LUAI_ERRORCHECK(0)
  checknext(ls, TK_THEN);
  LUAI_ERRORCHECK(0)
  block(ls);  /* `then' part */
  LUAI_ERRORCHECK(0)
  return condexit;
}


static void ifstat (LexState *ls, int line) {
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  FuncState *fs = ls->fs;
  int flist;
  int escapelist = NO_JUMP;
  flist = test_then_block(ls);  /* IF cond THEN block */
  LUAI_ERRORCHECK()
  while (ls->t.token == TK_ELSEIF) {
    luaK_concat(fs, &escapelist, luaK_jump(fs));
    LUAI_ERRORCHECK()
    luaK_patchtohere(fs, flist);
    LUAI_ERRORCHECK()
    flist = test_then_block(ls);  /* ELSEIF cond THEN block */
  }
  LUAI_ERRORCHECK()
  if (ls->t.token == TK_ELSE) {
    luaK_concat(fs, &escapelist, luaK_jump(fs));
    LUAI_ERRORCHECK()
    luaK_patchtohere(fs, flist);
    LUAI_ERRORCHECK()
    luaX_next(ls);  /* skip ELSE (after patch, for correct line info) */
    LUAI_ERRORCHECK()
    block(ls);  /* `else' part */
  }
  else
    luaK_concat(fs, &escapelist, flist);
  LUAI_ERRORCHECK()
  luaK_patchtohere(fs, escapelist);
  LUAI_ERRORCHECK()
  check_match(ls, TK_END, TK_IF, line);
}


static void localfunc (LexState *ls) {
  expdesc v, b;
  FuncState *fs = ls->fs;
  new_localvar(ls, str_checkname(ls), 0);
  LUAI_ERRORCHECK()
  init_exp(&v, VLOCAL, fs->freereg);
  LUAI_ERRORCHECK()
  luaK_reserveregs(fs, 1);
  LUAI_ERRORCHECK()
  adjustlocalvars(ls, 1);
  LUAI_ERRORCHECK()
  body(ls, &b, 0, ls->linenumber);
  LUAI_ERRORCHECK()
  luaK_storevar(fs, &v, &b);
  LUAI_ERRORCHECK()
  /* debug information will only see the variable after this point! */
  getlocvar(fs, fs->nactvar - 1).startpc = fs->pc;
}


static void localstat (LexState *ls) {
  /* stat -> LOCAL NAME {`,' NAME} [`=' explist1] */
  int nvars = 0;
  int nexps;
  expdesc e;
  do {
	LUAI_ERRORCHECK()
    new_localvar(ls, str_checkname(ls), nvars++);
	LUAI_ERRORCHECK()
  } while (testnext(ls, ','));
  if (testnext(ls, '=')) {
	LUAI_ERRORCHECK()
    nexps = explist1(ls, &e);
  }
  else {
    e.k = VVOID;
    nexps = 0;
  }
  LUAI_ERRORCHECK()
  adjust_assign(ls, nvars, nexps, &e);
  LUAI_ERRORCHECK()
  adjustlocalvars(ls, nvars);
}


static int funcname (LexState *ls, expdesc *v) {
  /* funcname -> NAME {field} [`:' NAME] */
  int needself = 0;
  singlevar(ls, v);
  LUAI_ERRORCHECK(0)
  while (ls->t.token == '.') {
    field(ls, v);
    LUAI_ERRORCHECK(0)
  }
  if (ls->t.token == ':') {
    needself = 1;
    field(ls, v);
    LUAI_ERRORCHECK(0)
  }
  return needself;
}


static void funcstat (LexState *ls, int line) {
  /* funcstat -> FUNCTION funcname body */
  int needself;
  expdesc v, b;
  luaX_next(ls);  /* skip FUNCTION */
  LUAI_ERRORCHECK()
  needself = funcname(ls, &v);
  LUAI_ERRORCHECK()
  body(ls, &b, needself, line);
  LUAI_ERRORCHECK()
  luaK_storevar(ls->fs, &v, &b);
  LUAI_ERRORCHECK()
  luaK_fixline(ls->fs, line);  /* definition `happens' in the first line */
}


static void exprstat (LexState *ls) {
  /* stat -> func | assignment */
  FuncState *fs = ls->fs;
  struct LHS_assign v;
  primaryexp(ls, &v.v);
  LUAI_ERRORCHECK()
  if (v.v.k == VCALL)  /* stat -> func */
    SETARG_C(getcode(fs, &v.v), 1);  /* call statement uses no results */
  else {  /* stat -> assignment */
    v.prev = NULL;
    assignment(ls, &v, 1);
  }
  LUAI_ERRORCHECK()
}


static void retstat (LexState *ls) {
  /* stat -> RETURN explist */
  FuncState *fs = ls->fs;
  expdesc e;
  int first, nret;  /* registers with returned values */
  luaX_next(ls);  /* skip RETURN */
  LUAI_ERRORCHECK()
  if (block_follow(ls->t.token) || ls->t.token == ';') {
	LUAI_ERRORCHECK()
    first = nret = 0;  /* return no values */
  } else {
    nret = explist1(ls, &e);  /* optional return values */
    if (hasmultret(e.k)) {
      LUAI_ERRORCHECK()
      luaK_setmultret(fs, &e);
      LUAI_ERRORCHECK()
      if (e.k == VCALL && nret == 1) {  /* tail call? */
        SET_OPCODE(getcode(fs,&e), OP_TAILCALL);
        LUAI_ERRORCHECK()
        lua_assert(GETARG_A(getcode(fs,&e)) == fs->nactvar);
        LUAI_ERRORCHECK()
      }
      first = fs->nactvar;
      nret = LUA_MULTRET;  /* return all values */
    }
    else {
      if (nret == 1)  /* only one single value? */
        first = luaK_exp2anyreg(fs, &e);
        LUAI_ERRORCHECK()
      else {
        luaK_exp2nextreg(fs, &e);  /* values must go to the `stack' */
        LUAI_ERRORCHECK()
        first = fs->nactvar;  /* return all `active' values */
        lua_assert(nret == fs->freereg - first);
        LUAI_ERRORCHECK()
      }
    }
  }
  LUAI_ERRORCHECK()
  luaK_ret(fs, first, nret);
}


static int statement (LexState *ls) {
  LUAI_ERRORCHECK(0)
  int line = ls->linenumber;  /* may be needed for error messages */
  switch (ls->t.token) {
    case TK_IF: {  /* stat -> ifstat */
      ifstat(ls, line);
      return 0;
    }
    case TK_WHILE: {  /* stat -> whilestat */
      whilestat(ls, line);
      return 0;
    }
    case TK_DO: {  /* stat -> DO block END */
      luaX_next(ls);  /* skip DO */
      LUAI_ERRORCHECK(0)
      block(ls);
      LUAI_ERRORCHECK(0)
      check_match(ls, TK_END, TK_DO, line);
      return 0;
    }
    case TK_FOR: {  /* stat -> forstat */
      forstat(ls, line);
      return 0;
    }
    case TK_REPEAT: {  /* stat -> repeatstat */
      repeatstat(ls, line);
      return 0;
    }
    case TK_FUNCTION: {
      funcstat(ls, line);  /* stat -> funcstat */
      return 0;
    }
    case TK_LOCAL: {  /* stat -> localstat */
      luaX_next(ls);  /* skip LOCAL */
      LUAI_ERRORCHECK(0)
      if (testnext(ls, TK_FUNCTION)) { /* local function? */
        LUAI_ERRORCHECK(0)
        localfunc(ls);
      }
      else {
        LUAI_ERRORCHECK(0)
        localstat(ls);
      }
      return 0;
    }
    case TK_RETURN: {  /* stat -> retstat */
      retstat(ls);
      return 1;  /* must be last statement */
    }
    case TK_BREAK: {  /* stat -> breakstat */
      luaX_next(ls);  /* skip BREAK */
      LUAI_ERRORCHECK(0)
      breakstat(ls);
      return 1;  /* must be last statement */
    }
    default: {
      exprstat(ls);
      return 0;  /* to avoid warnings */
    }
  }
}


static void chunk (LexState *ls) {
  /* chunk -> { stat [`;'] } */
  int islast = 0;
  enterlevel(ls);
  LUAI_ERRORCHECK()
  while (!islast && !block_follow(ls->t.token)) {
	LUAI_ERRORCHECK()
    islast = statement(ls);
    LUAI_ERRORCHECK()
    testnext(ls, ';');
    LUAI_ERRORCHECK()
    lua_assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
               ls->fs->freereg >= ls->fs->nactvar);
    LUAI_ERRORCHECK()
    ls->fs->freereg = ls->fs->nactvar;  /* free registers */
  }
  leavelevel(ls);
}

/* }====================================================================== */
