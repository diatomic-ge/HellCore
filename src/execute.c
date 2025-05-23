/******************************************************************************
  Copyright (c) 1992, 1995, 1996 Xerox Corporation.  All rights reserved.
  Portions of this code were written by Stephen White, aka ghond.
  Use and copying of this software and preparation of derivative works based
  upon this software are permitted.  Any distribution of this software or
  derivative works must comply with all applicable United States export
  control laws.  This software is made available AS IS, and Xerox Corporation
  makes no warranty about the software, its performance or its conformity to
  any specification.  Any person obtaining a copy of this software is requested
  to send their name and post office or electronic mail address to:
    Pavel Curtis
    Xerox PARC
    3333 Coyote Hill Rd.
    Palo Alto, CA 94304
    Pavel@Xerox.Com
 *****************************************************************************/

#include "my-string.h"
#include "my-time.h"

#include <errno.h>
#include <signal.h>
#include <sys/time.h>

#include "config.h"
#include "db.h"
#include "db_io.h"
#include "decompile.h"
#include "eval_env.h"
#include "eval_vm.h"
#include "exceptions.h"
#include "execute.h"
#include "functions.h"
#include "hash.h"
#include "list.h"
#include "log.h"
#include "numbers.h"
#include "opcode.h"
#include "options.h"
#include "parse_cmd.h"
#include "profiler.h"
#include "server.h"
#include "storage.h"
#include "streams.h"
#include "structures.h"
#include "sym_table.h"
#include "tasks.h"
#include "timers.h"
#include "utils.h"
#include "waif.h"
#include "version.h"

/* the following globals are the guts of the virtual machine: */
static activation *activ_stack = 0;
static int max_stack_size = 0;
static unsigned top_activ_stack;	/* points to top-of-stack
					   (last-occupied-slot),
					   not next-empty-slot */
static int root_activ_vector;	/* root_activ_vector == MAIN_VECTOR
				   iff root activation is main
				   vector */

/* these globals are not part of the vm because they get re-initialized
   after a suspend */
static int ticks_remaining;
static int task_was_suspended;
int task_timed_out;
static int interpreter_is_running = 0;
static int task_killed;
static Timer_ID task_alarm_id;
static task_kind current_task_kind;

static const char *handler_verb_name;	/* For in-DB traceback handling */
static Var handler_verb_args;

/* profiling */
static int do_verb_profiling = 0;

/* macros to ease indexing into activation stack */
#define RUN_ACTIV     activ_stack[top_activ_stack]
#define CALLER_ACTIV  activ_stack[top_activ_stack - 1]

static struct itimerval itimer_set;
static struct itimerval itimer_now;
volatile sig_atomic_t itimer_secs;
static int proftimer_on = 0;

void 
proftimer_sighandler(int sig)
{
    itimer_secs++;
}

inline static void
start_proftimer()
{
    int result;

    itimer_set.it_interval.tv_sec = 1;
    itimer_set.it_interval.tv_usec = 0;
    itimer_set.it_value.tv_sec = 1;
    itimer_set.it_value.tv_usec = 0;
    itimer_secs = 0;
    signal(SIGPROF, proftimer_sighandler);
    result = setitimer(ITIMER_PROF, &itimer_set, NULL);
    if (result == -1)
	panic(strerror(errno));

    proftimer_on = 1;
}

inline static void
stop_proftimer()
{
    int result;

    itimer_set.it_value.tv_sec = 0;
    itimer_set.it_value.tv_usec = 0;
    itimer_set.it_interval.tv_sec = 0;  
    itimer_set.it_interval.tv_usec = 0;
    signal(SIGPROF, SIG_IGN);
    result = setitimer(ITIMER_PROF, &itimer_set, NULL);
    if (result == -1)
        panic(strerror(errno));

    proftimer_on = 0;
}

inline static void
reset_proftimer()
{
    start_proftimer();
}

inline static int
read_proftimer()
{
    int result;
    result = getitimer(ITIMER_PROF, &itimer_now);

    if (result == -1)
	panic(strerror(errno));

    int e_secs, e_msecs;
    e_secs = itimer_now.it_interval.tv_sec - itimer_now.it_value.tv_sec;
    e_msecs = (e_secs * 1000) + (itimer_now.it_interval.tv_usec
	- itimer_now.it_value.tv_usec)/1000; 

    return ((e_msecs > 0) ? e_msecs : 0);
}

inline static void
log_activation_for_profile(const char* context)
{
    if (!do_verb_profiling)
	return;

    if (current_task_kind == TASK_FORKED) {
	profile_log("%d,%d,%s,#%d,%s,#%d\n", 
	    current_task_id, root_activ_vector, context, 
	    RUN_ACTIV.vloc, RUN_ACTIV.verbname, RUN_ACTIV.this);
    } else {
	profile_log("%d,MAIN,%s,#%d,%s,#%d\n", 
	    current_task_id, context, 
	    RUN_ACTIV.vloc, RUN_ACTIV.verbname, RUN_ACTIV.this);
    }
}

/**** error handling ****/

typedef enum {			/* Reasons for executing a FINALLY handler */
    /* These constants are stored in the DB, so don't change the order... */
    FIN_FALL_THRU, FIN_RAISE, FIN_UNCAUGHT, FIN_RETURN,
    FIN_ABORT,			/* This doesn't actually get you into a FINALLY... */
    FIN_EXIT
} Finally_Reason;

/*
 * Keep a pool of the common size rt_stacks around to avoid beating up on
 * malloc.  This doesn't really need tuning.  Most rt_stacks will be less
 * than size 10.  I rounded up to a size which won't waste a lot of space
 * with a powers-of-two malloc (while leaving some room for mymalloc
 * overhead, if any).
 */
static Var *rt_stack_quick;
#define RT_STACK_QUICKSIZE	15

static void
alloc_rt_stack(activation * a, int size)
{
    Var *res;

    if (size <= RT_STACK_QUICKSIZE && rt_stack_quick) {
	res = rt_stack_quick;
	rt_stack_quick = rt_stack_quick[0].v.list;
    } else {
	res = mymalloc(MAX(size, RT_STACK_QUICKSIZE) * sizeof(Var), M_RT_STACK);
    }
    a->base_rt_stack = a->top_rt_stack = res;
    a->rt_stack_size = size;
}

static void
free_rt_stack(activation * a)
{
    Var *stack = a->base_rt_stack;

    if (a->rt_stack_size <= RT_STACK_QUICKSIZE) {
	stack[0].v.list = rt_stack_quick;
	rt_stack_quick = stack;
    } else
	myfree(stack, M_RT_STACK);
}

void
print_error_backtrace(const char *msg, void (*output) (const char *))
{
    int t;
    Stream *str;

    if (!interpreter_is_running)
	return;
    str = new_stream(100);
    for (t = top_activ_stack; t >= 0; t--) {
	if (t != top_activ_stack)
	    stream_printf(str, "... called from ");
	stream_printf(str, "#%d:%s", activ_stack[t].vloc,
		      activ_stack[t].verbname);
	if (activ_stack[t].vloc != activ_stack[t].this)
	    stream_printf(str, " (this == #%d)", activ_stack[t].this);

	stream_printf(str, ", line %d",
		      find_line_number(activ_stack[t].prog,
				       (t == 0 ? root_activ_vector
					: MAIN_VECTOR),
				       activ_stack[t].error_pc));
	if (t == top_activ_stack)
	    stream_printf(str, ":  %s", msg);
	output(reset_stream(str));
	if (t > 0 && activ_stack[t].bi_func_pc) {
	    stream_printf(str, "... called from built-in function %s()",
			  name_func_by_num(activ_stack[t].bi_func_id));
	    output(reset_stream(str));
	}
    }
    output("(End of traceback)");
    free_stream(str);
}

void
output_to_log(const char *line)
{
    oklog("%s\n", line);
}

static Var backtrace_list;

static void
output_to_list(const char *line)
{
    Var str;

    str.type = TYPE_STR;
    str.v.str = str_dup(line);
    backtrace_list = listappend(backtrace_list, str);
}

static Var
error_backtrace_list(const char *msg)
{
    backtrace_list = new_list(0);
    print_error_backtrace(msg, output_to_list);
    return backtrace_list;
}

static enum error
suspend_task(package p)
{
    vm the_vm = new_vm(current_task_id, top_activ_stack + 1);
    int i;
    enum error e;

    /* save cputime */
    RUN_ACTIV.cputime += read_proftimer();
    the_vm->max_stack_size = max_stack_size;
    the_vm->top_activ_stack = top_activ_stack;
    the_vm->root_activ_vector = root_activ_vector;
    the_vm->func_id = 0;	/* shouldn't need func_id; */
    for (i = 0; i <= top_activ_stack; i++)
	the_vm->activ_stack[i] = activ_stack[i];

    e = (*p.u.susp.proc) (the_vm, p.u.susp.data);
    if (e != E_NONE)
	free_vm(the_vm, 0);
    return e;
}

static int raise_error(package p, enum outcome *outcome);

static int
unwind_stack(Finally_Reason why, Var value, enum outcome *outcome)
{
    /* Returns true iff the entire stack was unwound and the interpreter
     * should stop, in which case *outcome is the correct outcome to return. */
    Var code = (why == FIN_RAISE ? value.v.list[1] : zero);
    static int saved_cputime;

    saved_cputime = 0;

    for (;;) {			/* loop over activations */
	activation *a = &(activ_stack[top_activ_stack]);
	void *bi_func_data = 0;
	int bi_func_pc;
	unsigned bi_func_id = 0;
	Objid player;
        int saved_cputime;
	Var v, *goal = a->base_rt_stack;

	if (why == FIN_EXIT)
	    goal += value.v.list[1].v.num;
	while (a->top_rt_stack > goal) {	/* loop over rt stack */
	    a->top_rt_stack--;
	    v = *(a->top_rt_stack);
	    if (why != FIN_ABORT && v.type == TYPE_FINALLY) {
		/* FINALLY handler */
		a->pc = v.v.num;
		v.type = TYPE_INT;
		v.v.num = why;
		*(a->top_rt_stack++) = v;
		*(a->top_rt_stack++) = value;
		return 0;
	    } else if (why == FIN_RAISE && v.type == TYPE_CATCH) {
		/* TRY-EXCEPT or `expr ! ...' handler */
		Var *new_top = a->top_rt_stack - 2 * v.v.num;
		Var *vv;
		int found = 0;

		for (vv = new_top; vv < a->top_rt_stack; vv += 2) {
		    if (!found && (vv->type != TYPE_LIST
				   || ismember(code, *vv, 0))) {
			found = 1;
			v = *(vv + 1);
			if (v.type != TYPE_INT)
			    panic("Non-numeric PC value on stack!");
			a->pc = v.v.num;
		    }
		    free_var(*vv);
		}

		a->top_rt_stack = new_top;
		if (found) {
		    *(a->top_rt_stack++) = value;
		    return 0;
		}
	    } else
		free_var(v);
	}
	if (why == FIN_EXIT) {
	    a->pc = value.v.list[2].v.num;
	    free_var(value);
	    return 0;
	}
	bi_func_pc = a->bi_func_pc;
	if (bi_func_pc) {
	    bi_func_id = a->bi_func_id;
	    bi_func_data = a->bi_func_data;
	}
	player = a->player;
        saved_cputime = a->cputime;
	free_activation(*a, 0);	/* 0 == don't free bi_func_data */

	if (top_activ_stack == 0) {	/* done */
	    if (outcome)
		*outcome = (why == FIN_RETURN
			    ? OUTCOME_DONE
			    : OUTCOME_ABORTED);
	    return 1;
	}
	top_activ_stack--;
	RUN_ACTIV.cputime += saved_cputime;

	if (bi_func_pc != 0) {	/* Must unwind through a built-in function */
	    package p;

	    if (why == FIN_RETURN) {
		a = &(activ_stack[top_activ_stack]);
		p = call_bi_func(bi_func_id, value, bi_func_pc, a->progr,
				 bi_func_data);
		switch (p.kind) {
		case BI_RETURN:
		    *(a->top_rt_stack++) = p.u.ret;
		    return 0;
		case BI_RAISE:
		    if (a->debug)
			return raise_error(p, outcome);
		    else {
			*(a->top_rt_stack++) = p.u.raise.code;
			free_str(p.u.raise.msg);
			free_var(p.u.raise.value);
			return 0;
		    }
		case BI_SUSPEND:
		    {
			enum error e = suspend_task(p);

			if (e == E_NONE) {
			    if (outcome)
				*outcome = OUTCOME_BLOCKED;
			    return 1;
			} else {
			    value.type = TYPE_ERR;
			    value.v.err = e;
			    return unwind_stack(FIN_RAISE, value, outcome);
			}
		    }
		case BI_CALL:
		    a = &(activ_stack[top_activ_stack]);	/* TOS has changed */
		    a->bi_func_id = bi_func_id;
		    a->bi_func_pc = p.u.call.pc;
		    a->bi_func_data = p.u.call.data;
		    return 0;
		}
	    } else {
		/* Built-in functions receive zero as a `returned value' on
		 * errors and aborts, all further calls they make are short-
		 * circuited with an immediate return of zero, and any errors
		 * they raise are squelched.  This is compatible with older,
		 * pre-error-handling versions of the server, and thus
		 * acceptable for the existing built-ins.  It is conceivable
		 * that this model will have to be revisited at some point in
		 * the future.
		 */
		do {
		    p = call_bi_func(bi_func_id, zero, bi_func_pc, a->progr,
				     bi_func_data);
		    switch (p.kind) {
		    case BI_RETURN:
			free_var(p.u.ret);
			break;
		    case BI_RAISE:
			free_var(p.u.raise.code);
			free_str(p.u.raise.msg);
			free_var(p.u.raise.value);
			break;
		    case BI_SUSPEND:
			break;
		    case BI_CALL:
			free_activation(activ_stack[top_activ_stack--], 0);
			bi_func_pc = p.u.call.pc;
			bi_func_data = p.u.call.data;
			break;
		    }
		} while (p.kind == BI_CALL && bi_func_pc != 0);		/* !tailcall */
	    }
	} else if (why == FIN_RETURN) {		/* Push the value on the stack & go */
	    a = &(activ_stack[top_activ_stack]);
	    *(a->top_rt_stack++) = value;
	    return 0;
	}
    }
}

static int
find_handler_activ(Var code)
{
    /* Returns the index of the hottest activation with an active exception
     * handler for the given code.
     */
    int frame;

    for (frame = top_activ_stack; frame >= 0; frame--) {
	activation *a = &(activ_stack[frame]);
	Var *v, *vv;

	for (v = a->top_rt_stack - 1; v >= a->base_rt_stack; v--)
	    if (v->type == TYPE_CATCH) {
		for (vv = v - 2 * v->v.num; vv < v; vv += 2)
		    if (vv->type != TYPE_LIST || ismember(code, *vv, 0))
			return frame;
		v -= 2 * v->v.num;
	    }
    }

    return -1;
}

static Var
make_stack_list(activation * stack, int start, int end, int include_end,
		int root_vector, int options)
{
    Var r;
    int count = 0, i, j, listsize, do_linenums, do_cputime;

    for (i = end; i >= start; i--) {
	if (include_end || i != end)
	    count++;
	if (i != start && stack[i].bi_func_pc)
	    count++;
    }

    do_linenums = (options == 1 || options == 2);
    do_cputime = (options == 2);

    listsize = 5;
    if (do_linenums)
	listsize = 6;
    if (do_cputime)
	listsize = 7;

    r = new_list(count);
    j = 1;
    for (i = end; i >= start; i--) {
	Var v;

	if (include_end || i != end) {
	    v = r.v.list[j++] = new_list(listsize);
	   // v.v.list[1].type = TYPE_OBJ;
	   // v.v.list[1].v.obj = stack[i].this;
	    v.v.list[1] = var_ref(stack[i].THIS);
	    v.v.list[2].type = TYPE_STR;
	    v.v.list[2].v.str = str_ref(stack[i].verb);
	    v.v.list[3].type = TYPE_OBJ;
	    v.v.list[3].v.obj = stack[i].progr;
	    v.v.list[4].type = TYPE_OBJ;
	    v.v.list[4].v.obj = stack[i].vloc;
	    v.v.list[5].type = TYPE_OBJ;
	    v.v.list[5].v.obj = stack[i].player;
	    if (do_linenums) {
		v.v.list[6].type = TYPE_INT;
		v.v.list[6].v.num = find_line_number(stack[i].prog,
						     (i == 0 ? root_vector
						      : MAIN_VECTOR),
						     stack[i].error_pc);
	    }
	    if (do_cputime) {
		v.v.list[7].type = TYPE_INT;
		v.v.list[7].v.num = stack[i].cputime;
	    }
	}
	if (i != start && stack[i].bi_func_pc) {
	    v = r.v.list[j++] = new_list(listsize);
	    v.v.list[1].type = TYPE_OBJ;
	    v.v.list[1].v.obj = NOTHING;
	    v.v.list[2].type = TYPE_STR;
	    v.v.list[2].v.str = str_dup(name_func_by_num(stack[i].bi_func_id));
	    v.v.list[3].type = TYPE_OBJ;
	    v.v.list[3].v.obj = NOTHING;
	    v.v.list[4].type = TYPE_OBJ;
	    v.v.list[4].v.obj = NOTHING;
	    v.v.list[5].type = TYPE_OBJ;
	    v.v.list[5].v.obj = stack[i].player;
	    if (do_linenums) {
		v.v.list[6].type = TYPE_INT;
		v.v.list[6].v.num = stack[i].bi_func_pc;
	    }
	    if (do_cputime) {
		v.v.list[7].type = TYPE_INT;
		v.v.list[7].v.num = stack[i].cputime;
	    }
	}
    }

    return r;
}

static void
save_handler_info(const char *vname, Var args)
{
    handler_verb_name = vname;
    free_var(handler_verb_args);
    handler_verb_args = args;
}

static int
raise_error(package p, enum outcome *outcome)
{
    /* ASSERT: p.kind == BI_RAISE */
    int handler_activ = find_handler_activ(p.u.raise.code);
    Finally_Reason why;
    Var value;

    if (handler_activ >= 0) {	/* handler found */
	why = FIN_RAISE;
	value = new_list(4);
    } else {			/* uncaught exception */
	why = FIN_UNCAUGHT;
	value = new_list(5);
	value.v.list[5] = error_backtrace_list(p.u.raise.msg);
	handler_activ = 0;	/* get entire stack in list */
    }
    value.v.list[1] = p.u.raise.code;
    value.v.list[2].type = TYPE_STR;
    value.v.list[2].v.str = p.u.raise.msg;
    value.v.list[3] = p.u.raise.value;
    value.v.list[4] = make_stack_list(activ_stack, handler_activ,
				      top_activ_stack, 1,
				      root_activ_vector, 1);

    if (why == FIN_UNCAUGHT) {
	save_handler_info("handle_uncaught_error", value);
	value = zero;
    }
    return unwind_stack(why, value, outcome);
}

static void
abort_task(int is_ticks)
{
    Var value;
    const char *msg = (is_ticks ? "Task ran out of ticks"
		       : "Task ran out of seconds");

    value = new_list(3);
    value.v.list[1].type = TYPE_STR;
    value.v.list[1].v.str = str_dup(is_ticks ? "ticks" : "seconds");
    value.v.list[2] = make_stack_list(activ_stack, 0, top_activ_stack, 1,
				      root_activ_vector, 1);
    value.v.list[3] = error_backtrace_list(msg);
    save_handler_info("handle_task_timeout", value);
    unwind_stack(FIN_ABORT, zero, 0);
}

/**** activation manipulation ****/

static int
push_activation(void)
{
    if (top_activ_stack < max_stack_size - 1) {
	top_activ_stack++;
	return 1;
    } else
	return 0;
}

void
free_activation(activation a, char data_too)
{
    Var *i;

    free_rt_env(a.rt_env, a.prog->num_var_names);

    for (i = a.base_rt_stack; i < a.top_rt_stack; i++)
	free_var(*i);
    free_rt_stack(&a);
    free_var(a.THIS);
    free_var(a.temp);
    free_str(a.verb);
    free_str(a.verbname);

    free_program(a.prog);

    if (data_too && a.bi_func_pc && a.bi_func_data)
	free_data(a.bi_func_data);
    /* else bi_func_state will be later freed by bi_function */
}


/** Set up another activation for calling a verb
  does not change the vm in case of any error **/

enum error
_call_verb_handle(Objid this, db_verb_handle h, const char *vname, Var THIS, Var args)
{
    /* if call succeeds, args will be consumed.  If call fails, args
       will NOT be consumed  -- it must therefore be freed by caller */
    /* vname will never be consumed */
    /* THIS will never be consumed */

    /* will only return E_MAXREC, E_INVIND, E_VERBNF, or E_NONE */
    /* returns an error if there is one, and does not change the vm in that
       case, else sets up the activ_stack for the verb call and then returns
       E_NONE */

    Program *program;
    Var *env;
    Var v;

    program = db_verb_program(h);
    vname = str_dup(vname);	/* ensure that vname is heap-allocated */
    RUN_ACTIV.prog = program_ref(program);

    RUN_ACTIV.this = this;
    RUN_ACTIV.THIS = var_ref(THIS);
    RUN_ACTIV.progr = db_verb_owner(h);
    RUN_ACTIV.vloc = db_verb_definer(h);
    RUN_ACTIV.verb = str_ref(vname);
    RUN_ACTIV.verbname = str_ref(db_verb_names(h));
    RUN_ACTIV.debug = (db_verb_flags(h) & VF_DEBUG);

    alloc_rt_stack(&RUN_ACTIV, program->main_vector.max_stack);
    RUN_ACTIV.pc = 0;
    RUN_ACTIV.error_pc = 0;
    RUN_ACTIV.bi_func_pc = 0;
    RUN_ACTIV.temp.type = TYPE_NONE;
    RUN_ACTIV.cputime = 0;
    if (proftimer_on) {
        CALLER_ACTIV.cputime += read_proftimer();
        reset_proftimer();
    }

    RUN_ACTIV.rt_env = env = new_rt_env(RUN_ACTIV.prog->num_var_names);

    fill_in_rt_consts(env, program->version);

    set_rt_env_var(env, SLOT_THIS, var_ref(THIS));
    set_rt_env_var(env, SLOT_CALLER, var_ref(CALLER_ACTIV.THIS));

#define ENV_COPY(slot) \
    set_rt_env_var(env, slot, var_ref(CALLER_ACTIV.rt_env[slot]))

    ENV_COPY(SLOT_ARGSTR);
    ENV_COPY(SLOT_DOBJ);
    ENV_COPY(SLOT_DOBJSTR);
    ENV_COPY(SLOT_PREPSTR);
    ENV_COPY(SLOT_IOBJ);
    ENV_COPY(SLOT_IOBJSTR);

    if (is_wizard(CALLER_ACTIV.progr) &&
	(CALLER_ACTIV.rt_env[SLOT_PLAYER].type == TYPE_OBJ))
	ENV_COPY(SLOT_PLAYER);
    else
	set_rt_env_obj(env, SLOT_PLAYER, CALLER_ACTIV.player);
    RUN_ACTIV.player = env[SLOT_PLAYER].v.obj;

#undef ENV_COPY

    v.type = TYPE_STR;
     if (vname[0] == WAIF_VERB_PREFIX) {
 	v.v.str = str_dup(vname + 1);
 	free_str(vname);
     } else
	    v.v.str = vname;
    set_rt_env_var(env, SLOT_VERB, v);	/* no var_dup */
    set_rt_env_var(env, SLOT_ARGS, args);	/* no var_dup */

    log_activation_for_profile("call");

    return E_NONE;
}

enum error
_call_verb(Objid this, const char *vname, Var THIS, Var args, int do_pass)
{
    /* do_pass:
     *   1 - normal pass, find this verb on parent of this.
     *   2 - bf_call pass, go to entirely new verb, preserve this.
     *  */
    Objid where;
    db_verb_handle h;

    where = this;
    if (do_pass) {
        if (!valid(RUN_ACTIV.vloc)) {
            return E_INVIND;
        }
        else if (do_pass == 1) {
            where = db_object_parent(RUN_ACTIV.vloc);
        } else if (do_pass == 2) {
            where = RUN_ACTIV.vloc;
        } else {
            panic("unknown do_pass value in call_verb");
        }
    }

    if (!valid(where)) {
        return E_INVIND;
    }
    h = db_find_callable_verb(where, vname);
    if (!h.ptr) {
        return E_VERBNF;
    } else if (!push_activation()) {
        return E_MAXREC;
    }

    return _call_verb_handle(this, h, vname, THIS, args);
}

enum error
_call_verb_index(Objid where, unsigned verb_index, Objid this, const char *vname, Var THIS, Var args)
{
    db_verb_handle h;

    if (!valid(where)) {
        return E_INVIND;
    }
    h = db_find_indexed_verb(where, verb_index);
    if (!h.ptr) {
        return E_VERBNF;
    } else if (!push_activation()) {
        return E_MAXREC;
    }

    return _call_verb_handle(this, h, vname, THIS, args);
}

enum error
call_verb_index(Objid where, unsigned verb_index, Objid this, const char *vname, Var args)
{
    Var THIS;

    THIS.type = TYPE_OBJ;
    THIS.v.obj = this;
    return _call_verb_index(where, verb_index, this, vname, THIS, args);
}

 enum error
 call_verb(Objid this, const char *vname, Var args, int do_pass)
 {
 	Var THIS;
 
 	THIS.type = TYPE_OBJ;
 	THIS.v.obj = this;
 	return _call_verb(this, vname, THIS, args, do_pass);
 }
 
static int
rangeset_check(int end, int from, int to)
{
    if (from > end + 1 || to < 0)
	return 1;
    return 0;
}

#ifdef IGNORE_PROP_PROTECTED
#define bi_prop_protected(prop, progr) (0)
#else
static int
bi_prop_protected(enum bi_prop prop, Objid progr)
{
    const char *pname = 0;	/* silence warning */

    if (is_wizard(progr))
	return 0;

    switch (prop) {
    case BP_NAME:
	pname = "protect_name";
	break;
    case BP_OWNER:
	pname = "protect_owner";
	break;
    case BP_PROGRAMMER:
	pname = "protect_programmer";
	break;
    case BP_WIZARD:
	pname = "protect_wizard";
	break;
    case BP_R:
	pname = "protect_r";
	break;
    case BP_W:
	pname = "protect_w";
	break;
    case BP_F:
	pname = "protect_f";
	break;
    case BP_LOCATION:
	pname = "protect_location";
	break;
    case BP_CONTENTS:
	pname = "protect_contents";
	break;
    default:
	panic("Can't happen in BI_PROP_PROTECTED!");
    }

    return server_flag_option(pname);
}
#endif				/* IGNORE_PROP_PROTECTED */

/** 
  the main interpreter -- run()
  everything is just an entry point to it
**/

static enum outcome
run(char raise, enum error resumption_error, Var * result)
{				/* runs the_vm */
    /* If the returned value is OUTCOME_DONE and RESULT is non-NULL, then
     * *RESULT is the value returned by the top frame.
     */

    /* bc, bv, rts are distinguished as the state variables of run()
       their value capture the state of the running between OP_ cases */
    Bytecodes bc;
    register Byte *bv;
    Byte *error_bv;
    register Var *rts;			/* next empty slot */
    enum Opcode op;
    Var error_var;
    enum outcome outcome;

/** a bunch of macros that work *ONLY* inside run() **/

/* helping macros about the runtime_stack. */
#define POP()         (*(--rts))
#define PUSH(v)       (*(rts++) = v)
#define PUSH_REF(v)   PUSH(var_ref(v))
#define TOP_RT_VALUE           (*(rts - 1))
#define NEXT_TOP_RT_VALUE      (*(rts - 2))

#define READ_BYTES(bv, nb)			\
    ( bv += nb,					\
      (nb == 1				        \
       ? bv[-1]					\
       : (nb == 2				\
	  ? ((unsigned) bv[-2] << 8) + bv[-1]	\
	  : (((unsigned) bv[-4] << 24)		\
	     + ((unsigned) bv[-3] << 16)     	\
	     + ((unsigned) bv[-2] << 8)      	\
	     + bv[-1]))))

#define LOAD_STATE_VARIABLES() 					\
do {  								\
    bc = ( (top_activ_stack != 0 || root_activ_vector == MAIN_VECTOR) \
	   ? RUN_ACTIV.prog->main_vector 			\
	   : RUN_ACTIV.prog->fork_vectors[root_activ_vector]); 	\
    bv = bc.vector + RUN_ACTIV.pc;  				\
    error_bv = bc.vector + RUN_ACTIV.error_pc;			\
    rts = RUN_ACTIV.top_rt_stack; /* next empty slot */        	\
} while (0)

#define STORE_STATE_VARIABLES()			\
do {						\
    RUN_ACTIV.pc = bv - bc.vector;		\
    RUN_ACTIV.error_pc = error_bv - bc.vector;	\
    RUN_ACTIV.top_rt_stack = rts;		\
} while (0)

#define RAISE_ERROR(the_err) 			\
do {						\
    if (RUN_ACTIV.debug) { 			\
	STORE_STATE_VARIABLES();		\
	if (raise_error(make_error_pack(the_err), 0)) \
	    return OUTCOME_ABORTED;		\
	else {					\
	    LOAD_STATE_VARIABLES();		\
	    goto next_opcode;			\
	}					\
    } 						\
} while (0)

#define PUSH_ERROR(the_err)                                     \
do {    						    	\
    RAISE_ERROR(the_err);	/* may not return */		\
    error_var.type = TYPE_ERR;					\
    error_var.v.err = the_err;					\
    PUSH(error_var);						\
} while (0)

#define JUMP(label)     (bv = bc.vector + label)

/* end of major run() macros */

    LOAD_STATE_VARIABLES();

    if (raise) {
	error_bv = bv;
	PUSH_ERROR(resumption_error);
    }
    for (;;) {
      next_opcode:
	error_bv = bv;
	op = *bv++;

	if (COUNT_TICK(op)) {
	    if (--ticks_remaining <= 0) {
		STORE_STATE_VARIABLES();
		abort_task(1);
		return OUTCOME_ABORTED;
	    }
	    if (task_timed_out) {
		STORE_STATE_VARIABLES();
		abort_task(0);
		return OUTCOME_ABORTED;
	    }
	}
	if (task_killed) {
	    STORE_STATE_VARIABLES();
	    unwind_stack(FIN_ABORT, zero, 0);
	    return OUTCOME_ABORTED;
	}
	switch (op) {

	case OP_IF_QUES:
	case OP_IF:
	case OP_WHILE:
	case OP_EIF:
	  do_test:
	    {
		Var cond;

		cond = POP();
		if (!is_true(cond))	/* jump if false */
		    JUMP(READ_BYTES(bv, bc.numbytes_label));
		else {
		    bv += bc.numbytes_label;
		}
		free_var(cond);
	    }
	    break;

	case OP_JUMP:
	    JUMP(READ_BYTES(bv, bc.numbytes_label));
	    break;

        case OP_FOR_LIST:
            {
                unsigned id = READ_BYTES(bv, bc.numbytes_var_name);
                unsigned lab = READ_BYTES(bv, bc.numbytes_label);
                Var count, list;

                count = TOP_RT_VALUE;   /* will be a integer, at first */
                list = NEXT_TOP_RT_VALUE;       /* list or hash */
	
		if (list.type != TYPE_LIST && list.type != TYPE_HASH) {	
                    RAISE_ERROR(E_TYPE);
                    free_var(POP());
                    free_var(POP());
                    JUMP(lab);
                } else if (count.type == TYPE_INT && 
                           count.v.num > list.v.list[0].v.num /* size */ ) {
                    free_var(POP());
                    free_var(POP());
                    JUMP(lab);
                } else {
                    free_var(RUN_ACTIV.rt_env[id]);

		    if (list.type == TYPE_LIST) {
        	        RUN_ACTIV.rt_env[id] = var_ref(list.v.list[count.v.num]);
                	count.v.num++;      /* increment count */
                	TOP_RT_VALUE = count;
		    } else { /* hash */
			TOP_RT_VALUE = hashiterate(list, &(RUN_ACTIV.rt_env[id]), count);
			if (TOP_RT_VALUE.type == TYPE_ERR) {
			    free_var(POP());
			    free_var(POP());
			    JUMP(lab);
			}
		    }
                }
            }
            break;

	case OP_FOR_RANGE:
	    {
		unsigned id = READ_BYTES(bv, bc.numbytes_var_name);
		unsigned lab = READ_BYTES(bv, bc.numbytes_label);
		Var from, to;

		to = TOP_RT_VALUE;
		from = NEXT_TOP_RT_VALUE;
		if ((to.type != TYPE_INT && to.type != TYPE_OBJ)
		    || to.type != from.type) {
		    RAISE_ERROR(E_TYPE);
		    free_var(POP());
		    free_var(POP());
		    JUMP(lab);
		} else if (to.type == TYPE_INT
			   ? from.v.num > to.v.num
			   : from.v.obj > to.v.obj) {
		    free_var(POP());
		    free_var(POP());
		    JUMP(lab);
		} else {
		    free_var(RUN_ACTIV.rt_env[id]);
		    RUN_ACTIV.rt_env[id] = var_ref(from);
		    if (to.type == TYPE_INT)
			from.v.num++;
		    else
			from.v.obj++;
		    NEXT_TOP_RT_VALUE = from;
		}
	    }
	    break;

	case OP_POP:
	    free_var(POP());
	    break;

	case OP_IMM:
	    {
		int slot;

		/* If we'd just throw it away anyway (eg verbdocs),
		   skip both OPs.  This accounts for most executions
		   of OP_IMM in my tests.
		 */
		if (bv[bc.numbytes_literal] == OP_POP) {
		    bv += bc.numbytes_literal + 1;
		    break;
		}
		slot = READ_BYTES(bv, bc.numbytes_literal);
		PUSH_REF(RUN_ACTIV.prog->literals[slot]);
	    }
	    break;

	case OP_MAKE_EMPTY_LIST:
	    {
		Var list;

		list = new_list(0);
		PUSH(list);
	    }
	    break;

	case OP_LIST_ADD_TAIL:
	    {
		Var tail, list;

		tail = POP();	/* whatever */
		list = POP();	/* should be list */
		if (list.type != TYPE_LIST) {
		    free_var(list);
		    free_var(tail);
		    PUSH_ERROR(E_TYPE);
		} else
		    PUSH(listappend(list, tail));
	    }
	    break;

	case OP_LIST_APPEND:
	    {
		Var tail, list;

		tail = POP();	/* second, should be list */
		list = POP();	/* first, should be list */
		if (tail.type != TYPE_LIST || list.type != TYPE_LIST) {
		    free_var(tail);
		    free_var(list);
		    PUSH_ERROR(E_TYPE);
		} else
		    PUSH(listconcat(list, tail));
	    }
	    break;

	case OP_INDEXSET:
	    {
		Var value, index, list;

		value = POP();	/* rhs value */
		index = POP();	/* index, should be integer */
		list = POP();	/* lhs except last index, should be list or str */
		/* whole thing should mean list[index] = value */
		
		if (list.type == TYPE_HASH) {
		    if (index.type != TYPE_STR && index.type != TYPE_INT &&
		        index.type != TYPE_FLOAT && index.type != TYPE_OBJ &&
		        index.type != TYPE_ERR) {
		      free_var(value);
		      free_var(index);
		      free_var(list);
		      PUSH_ERROR(E_TYPE);
	    	    } else {
			Var res;
			
			if (var_refcount(list) == 1)
			    res = list;
			else {
			    res = var_dup(list);
			    free_var(list);
			}
			res = hashset(res, index, value);
		
			if (res.type == TYPE_ERR)
			    PUSH_ERROR(res.v.err);
			else
			    PUSH(res);
		    }
		} else if ((list.type != TYPE_LIST && list.type != TYPE_STR)
		    || index.type != TYPE_INT
		  || (list.type == TYPE_STR && value.type != TYPE_STR)) {
		    free_var(value);
		    free_var(index);
		    free_var(list);
		    PUSH_ERROR(E_TYPE);
		} else if (index.v.num < 1
			   || (list.type == TYPE_LIST
		       && index.v.num > list.v.list[0].v.num /* size */ )
			   || (list.type == TYPE_STR
			    && index.v.num > (int) strlen(list.v.str))) {
		    free_var(value);
		    free_var(index);
		    free_var(list);
		    PUSH_ERROR(E_RANGE);
		} else if (list.type == TYPE_STR && strlen(value.v.str) != 1) {
		    free_var(value);
		    free_var(index);
		    free_var(list);
		    PUSH_ERROR(E_INVARG);
		} else if (list.type == TYPE_LIST) {
		    Var res;

		    if (var_refcount(list) == 1)
			res = list;
		    else {
			res = var_dup(list);
		        free_var(list);
		    }
		    PUSH(listset(res, value, index.v.num));
		} else {	/* TYPE_STR */
		    char *tmp_str = str_dup(list.v.str);
		    free_str(list.v.str);
		    tmp_str[index.v.num - 1] = value.v.str[0];
		    list.v.str = tmp_str;
		    free_var(value);
		    PUSH(list);
		}
	    }
	    break;

	case OP_MAKE_SINGLETON_LIST:
	    {
		Var list;

		list = new_list(1);
		list.v.list[1] = POP();
		PUSH(list);
	    }
	    break;

	case OP_CHECK_LIST_FOR_SPLICE:
	    if (TOP_RT_VALUE.type != TYPE_LIST) {
		free_var(POP());
		PUSH_ERROR(E_TYPE);
	    }
	    /* no op if top-rt-stack is a list */
	    break;

	case OP_PUT_TEMP:
	    RUN_ACTIV.temp = var_ref(TOP_RT_VALUE);
	    break;

	case OP_PUSH_TEMP:
	    PUSH(RUN_ACTIV.temp);
	    RUN_ACTIV.temp.type = TYPE_NONE;
	    break;

	case OP_EQ:
	case OP_NE:
	    {
		Var rhs, lhs, ans;

		rhs = POP();
		lhs = POP();
		ans.type = TYPE_INT;
		ans.v.num = (op == OP_EQ
			     ? equality(rhs, lhs, 0)
			     : !equality(rhs, lhs, 0));
		PUSH(ans);
		free_var(rhs);
		free_var(lhs);
	    }
	    break;

	case OP_GT:
	case OP_LT:
	case OP_GE:
	case OP_LE:
	    {
		Var rhs, lhs, ans;
		int comparison;

		rhs = POP();
		lhs = POP();
		if ((lhs.type == TYPE_INT || lhs.type == TYPE_FLOAT)
		    && (rhs.type == TYPE_INT || rhs.type == TYPE_FLOAT)) {
		    ans = compare_numbers(lhs, rhs);
		    if (ans.type == TYPE_ERR) {
			free_var(rhs);
			free_var(lhs);
			PUSH_ERROR(ans.v.err);
		    } else {
			comparison = ans.v.num;
			goto finish_comparison;
		    }
		} else if (rhs.type != lhs.type || rhs.type == TYPE_LIST) {
		    free_var(rhs);
		    free_var(lhs);
		    PUSH_ERROR(E_TYPE);
		} else {
		    switch (rhs.type) {
		    case TYPE_INT:
			comparison = compare_integers(lhs.v.num, rhs.v.num);
			break;
		    case TYPE_OBJ:
			comparison = compare_integers(lhs.v.obj, rhs.v.obj);
			break;
		    case TYPE_ERR:
			comparison = ((int) lhs.v.err) - ((int) rhs.v.err);
			break;
		    case TYPE_STR:
			comparison = mystrcasecmp(lhs.v.str, rhs.v.str);
			break;
		    default:
			errlog("RUN: Impossible type in comparison: %d\n",
			       rhs.type);
			comparison = 0;
		    }

		  finish_comparison:
		    ans.type = TYPE_INT;
		    switch (op) {
		    case OP_LT:
			ans.v.num = (comparison < 0);
			break;
		    case OP_LE:
			ans.v.num = (comparison <= 0);
			break;
		    case OP_GT:
			ans.v.num = (comparison > 0);
			break;
		    case OP_GE:
			ans.v.num = (comparison >= 0);
			break;
		    default:
			errlog("RUN: Imposible opcode in comparison: %d\n", op);
			break;
		    }
		    PUSH(ans);
		    free_var(rhs);
		    free_var(lhs);
		}
	    }
	    break;

	case OP_IN:
	    {
		Var lhs, rhs, ans;

		rhs = POP();	/* should be list or hash*/
		lhs = POP();	/* lhs, any type */
		if (rhs.type == TYPE_LIST) {
		    ans.type = TYPE_INT;
		    ans.v.num = ismember(lhs, rhs, 0);
		    PUSH(ans);
		    free_var(rhs);
		    free_var(lhs);
		} else if (rhs.type == TYPE_HASH) {
		    if (lhs.type != TYPE_STR && lhs.type != TYPE_INT && 
		        lhs.type != TYPE_OBJ && lhs.type != TYPE_FLOAT &&
		        lhs.type != TYPE_ERR) {
			    free_var(rhs);
			    free_var(lhs);
			    PUSH_ERROR(E_TYPE);
		    } else {
			ans.type = TYPE_INT;
			ans.v.num = hashcontains(rhs, lhs);
			PUSH (ans);
			free_var(rhs);
			free_var(lhs);
		    }
		} else {
		    free_var(rhs);
		    free_var(lhs);
		    PUSH_ERROR(E_TYPE);
		}
	    }
	    break;

	case OP_MULT:
	case OP_MINUS:
	case OP_DIV:
	case OP_MOD:
	    {
		Var lhs, rhs, ans;

		rhs = POP();	/* should be number */
		lhs = POP();	/* should be number */
		if ((lhs.type == TYPE_INT || lhs.type == TYPE_FLOAT)
		    && (rhs.type == TYPE_INT || rhs.type == TYPE_FLOAT)) {
		    switch (op) {
		    case OP_MULT:
			ans = do_multiply(lhs, rhs);
			break;
		    case OP_MINUS:
			ans = do_subtract(lhs, rhs);
			break;
		    case OP_DIV:
			ans = do_divide(lhs, rhs);
			break;
		    case OP_MOD:
			ans = do_modulus(lhs, rhs);
			break;
		    default:
			errlog("RUN: Impossible opcode in arith ops: %d\n", op);
			break;
		    }
		} else {
		    ans.type = TYPE_ERR;
		    ans.v.err = E_TYPE;
		}
		free_var(rhs);
		free_var(lhs);
		if (ans.type == TYPE_ERR)
		    PUSH_ERROR(ans.v.err);
		else
		    PUSH(ans);
	    }
	    break;

	case OP_ADD:
	    {
		Var rhs, lhs, ans;

		rhs = POP();
		lhs = POP();
		if ((lhs.type == TYPE_INT || lhs.type == TYPE_FLOAT)
		    && (rhs.type == TYPE_INT || rhs.type == TYPE_FLOAT))
		    ans = do_add(lhs, rhs);
		else if (lhs.type == TYPE_STR && rhs.type == TYPE_STR) {
		    char *str;

		    str = mymalloc((strlen(rhs.v.str) + strlen(lhs.v.str) + 1)
				   * sizeof(char), M_STRING);
		    sprintf(str, "%s%s", lhs.v.str, rhs.v.str);
		    ans.type = TYPE_STR;
		    ans.v.str = str;
		} else {
		    ans.type = TYPE_ERR;
		    ans.v.err = E_TYPE;
		}
		free_var(rhs);
		free_var(lhs);

		if (ans.type == TYPE_ERR)
		    PUSH_ERROR(ans.v.err);
		else
		    PUSH(ans);
	    }
	    break;

	case OP_AND:
	case OP_OR:
	    {
		Var lhs;
		unsigned lab = READ_BYTES(bv, bc.numbytes_label);

		lhs = TOP_RT_VALUE;
		if ((op == OP_AND && !is_true(lhs))
		    || (op == OP_OR && is_true(lhs)))	/* short-circuit */
		    JUMP(lab);
		else {
		    free_var(POP());
		}
	    }
	    break;

	case OP_NOT:
	    {
		Var arg, ans;

		arg = POP();
		ans.type = TYPE_INT;
		ans.v.num = !is_true(arg);
		PUSH(ans);
		free_var(arg);
	    }
	    break;

	case OP_UNARY_MINUS:
	    {
		Var arg, ans;

		arg = POP();
		if (arg.type == TYPE_INT) {
		    ans.type = TYPE_INT;
		    ans.v.num = -arg.v.num;
		} else if (arg.type == TYPE_FLOAT)
		    ans = new_float(-*arg.v.fnum);
		else {
		    free_var(arg);
		    PUSH_ERROR(E_TYPE);
		    break;
		}

		PUSH(ans);
		free_var(arg);
	    }
	    break;

	case OP_REF:
	    {
		Var index, list;

		index = POP();	/* should be integer or string (for hash) */
		list = POP();	/* should be list or string or hash */


		if (list.type == TYPE_HASH) {
			if (index.type != TYPE_STR && index.type != TYPE_INT &&
			    index.type != TYPE_FLOAT && index.type != TYPE_OBJ &&
			    index.type != TYPE_ERR) {
				free_var(index);
				free_var(list);
				PUSH_ERROR(E_TYPE);
			} else {
			    Var result = hashget(list, index);
			    
			    if (result.type == TYPE_NONE) {
				free_var(index);
				free_var(list);
				PUSH_ERROR(E_RANGE);
			    } else {
				PUSH(result);
				free_var(index);
				free_var(list);
			    }
			}
		} else if (index.type != TYPE_INT ||
		    (list.type != TYPE_LIST && list.type != TYPE_STR)) {
		    free_var(index);
		    free_var(list);
		    PUSH_ERROR(E_TYPE);
		} else if (list.type == TYPE_LIST) {
		    if (index.v.num <= 0 || index.v.num > list.v.list[0].v.num) {
			free_var(index);
			free_var(list);
			PUSH_ERROR(E_RANGE);
		    } else {
			PUSH(var_ref(list.v.list[index.v.num]));
			free_var(index);
			free_var(list);
		    }
		} else {	/* list.type == TYPE_STR */
		    if (index.v.num <= 0
			|| index.v.num > (int) strlen(list.v.str)) {
			free_var(index);
			free_var(list);
			PUSH_ERROR(E_RANGE);
		    } else {
			PUSH(strget(list, index));
			free_var(index);
			free_var(list);
		    }
		}
	    }
	    break;

	case OP_PUSH_REF:
	    {
		Var index, list;

		index = TOP_RT_VALUE;
		list = NEXT_TOP_RT_VALUE;

		if (index.type != TYPE_INT || list.type != TYPE_LIST) {
		    PUSH_ERROR(E_TYPE);
		} else if (index.v.num <= 0 ||
			   index.v.num > list.v.list[0].v.num) {
		    PUSH_ERROR(E_RANGE);
		} else
		    PUSH(var_ref(list.v.list[index.v.num]));
	    }
	    break;

	case OP_RANGE_REF:
	    {
		Var base, from, to;

		to = POP();	/* should be integer */
		from = POP();	/* should be integer */
		base = POP();	/* should be list or string */

		if ((base.type != TYPE_LIST && base.type != TYPE_STR)
		    || to.type != TYPE_INT || from.type != TYPE_INT) {
		    free_var(to);
		    free_var(from);
		    PUSH_ERROR(E_TYPE);
		} else {
		    int len = (base.type == TYPE_STR ? strlen(base.v.str)
			       : base.v.list[0].v.num);
		    if (from.v.num <= to.v.num
			&& (from.v.num <= 0 || from.v.num > len
			    || to.v.num <= 0 || to.v.num > len)) {
			free_var(to);
			free_var(from);
			free_var(base);
			PUSH_ERROR(E_RANGE);
		    } else {
			PUSH((base.type == TYPE_STR
			      ? substr(base, from.v.num, to.v.num)
			      : sublist(base, from.v.num, to.v.num)));
			/* base freed by substr/sublist */
			free_var(from);
			free_var(to);
		    }
		}
	    }
	    break;

	case OP_G_PUT:
	    {
		unsigned id = READ_BYTES(bv, bc.numbytes_var_name);
		free_var(RUN_ACTIV.rt_env[id]);
		RUN_ACTIV.rt_env[id] = var_ref(TOP_RT_VALUE);
	    }
	    break;

	case OP_G_PUSH:
	    {
		Var value;

		value = RUN_ACTIV.rt_env[READ_BYTES(bv, bc.numbytes_var_name)];
		if (value.type == TYPE_NONE)
		    PUSH_ERROR(E_VARNF);
		else
		    PUSH_REF(value);
	    }
	    break;

	case OP_GET_PROP:
	    {
		Var propname, obj, prop;

		propname = POP();	/* should be string */
		obj = POP();	/* should be objid */
		if (obj.type == TYPE_WAIF && propname.type == TYPE_STR) {
		    enum error err;

		    err = waif_get_prop(obj.v.waif, propname.v.str, &prop,
					RUN_ACTIV.progr);
		    free_var(propname);
		    free_var(obj);
		    if (err == E_NONE)
			PUSH(prop);
		    else
			PUSH_ERROR(err);
		} else if (propname.type != TYPE_STR || obj.type != TYPE_OBJ) {
		    free_var(propname);
		    free_var(obj);
		    PUSH_ERROR(E_TYPE);
		} else if (!valid(obj.v.obj)) {
		    free_var(propname);
		    free_var(obj);
		    PUSH_ERROR(E_INVIND);
		} else {
		    db_prop_handle h;

		    h = db_find_property(obj.v.obj, propname.v.str, &prop);
		    free_var(propname);
		    free_var(obj);
		    if (!h.ptr)
			PUSH_ERROR(E_PROPNF);
		    else if (h.built_in
			 ? bi_prop_protected(h.built_in, RUN_ACTIV.progr)
		      : !db_property_allows(h, RUN_ACTIV.progr, PF_READ))
			PUSH_ERROR(E_PERM);
		    else if (h.built_in)
			PUSH(prop);	/* it's already freshly allocated */
		    else
			PUSH_REF(prop);
		}
	    }
	    break;

	case OP_PUSH_GET_PROP:
	    {
		Var propname, obj, prop;

		propname = TOP_RT_VALUE;
		obj = NEXT_TOP_RT_VALUE;

		if (obj.type == TYPE_WAIF && propname.type == TYPE_STR) {
		    enum error err;

		    err = waif_get_prop(obj.v.waif, propname.v.str, &prop,
					RUN_ACTIV.progr);
		    if (err == E_NONE)
			PUSH(prop);
		    else
			PUSH_ERROR(err);
		} else if (propname.type != TYPE_STR || obj.type != TYPE_OBJ)
		    PUSH_ERROR(E_TYPE);
		else if (!valid(obj.v.obj))
		    PUSH_ERROR(E_INVIND);
		else {
		    db_prop_handle h;

		    h = db_find_property(obj.v.obj, propname.v.str, &prop);
		    if (!h.ptr)
			PUSH_ERROR(E_PROPNF);
		    else if (h.built_in
			 ? bi_prop_protected(h.built_in, RUN_ACTIV.progr)
		      : !db_property_allows(h, RUN_ACTIV.progr, PF_READ))
			PUSH_ERROR(E_PERM);
		    else if (h.built_in)
			PUSH(prop);
		    else
			PUSH_REF(prop);
		}
	    }
	    break;

	case OP_PUT_PROP:
	    {
		Var obj, propname, rhs;

		rhs = POP();	/* any type */
		propname = POP();	/* should be string */
		obj = POP();	/* should be objid */

		if (obj.type == TYPE_WAIF && propname.type == TYPE_STR) {
		    enum error err;

		    err = waif_put_prop(obj.v.waif, propname.v.str, rhs,
					RUN_ACTIV.progr);
		    free_var(propname);
		    free_var(obj);
		    if (err == E_NONE) {
			PUSH(rhs);
		    } else {
		        free_var(rhs);
			PUSH_ERROR(err);
		    }
		} else if (obj.type != TYPE_OBJ || propname.type != TYPE_STR) {
		    free_var(rhs);
		    free_var(propname);
		    free_var(obj);
		    PUSH_ERROR(E_TYPE);
		} else if (!valid(obj.v.obj)) {
		    free_var(rhs);
		    free_var(propname);
		    free_var(obj);
		    PUSH_ERROR(E_INVIND);
		} else {
		    db_prop_handle h;
		    enum error err = E_NONE;
		    Objid progr = RUN_ACTIV.progr;

		    h = db_find_property(obj.v.obj, propname.v.str, 0);
		    if (!h.ptr)
			err = E_PROPNF;
		    else {
			switch (h.built_in) {
			case BP_NONE:	/* Not a built-in property */
			    if (!db_property_allows(h, progr, PF_WRITE))
				err = E_PERM;
			    if (db_property_flags(h) & PF_PRIVATE && RUN_ACTIV.vloc != obj.v.obj)
				err = E_PERM;
			    break;
			case BP_NAME:
			    if (rhs.type != TYPE_STR)
				err = E_TYPE;
			    else if (!is_wizard(progr)
				     && (is_user(obj.v.obj)
				 || progr != db_object_owner(obj.v.obj)))
				err = E_PERM;
			    break;
			case BP_OWNER:
			    if (rhs.type != TYPE_OBJ)
				err = E_TYPE;
			    else if (!is_wizard(progr))
				err = E_PERM;
			    break;
			case BP_PROGRAMMER:
			case BP_WIZARD:
			    if (!is_wizard(progr))
				err = E_PERM;
			    else if (h.built_in == BP_WIZARD
			     && !is_true(rhs) != !is_wizard(obj.v.obj)) {
				/* Notify only on changes in state; the !'s above
				 * serve to canonicalize the truth values.
				 */
				/* First make sure traceback will be accurate. */
				STORE_STATE_VARIABLES();
				oklog("%sWIZARDED: #%d by programmer #%d\n",
				      is_wizard(obj.v.obj) ? "DE" : "",
				      obj.v.obj, progr);
				print_error_backtrace(is_wizard(obj.v.obj)
						    ? "Wizard bit unset."
						      : "Wizard bit set.",
						      output_to_log);
			    }
			    break;
			case BP_R:
			case BP_W:
			case BP_F:
			    if (progr != db_object_owner(obj.v.obj)
				&& !is_wizard(progr))
				err = E_PERM;
			    break;
			case BP_LOCATION:
			case BP_CONTENTS:
			    err = E_PERM;
			    break;
			default:
			    panic("Unknown built-in property in OP_PUT_PROP!");
			}
		    }

		    free_var(propname);
		    free_var(obj);
		    if (err == E_NONE) {
			db_set_property_value(h, var_ref(rhs));
			PUSH(rhs);
		    } else {
			free_var(rhs);
			PUSH_ERROR(err);
		    }
		}
	    }
	    break;

	case OP_FORK:
	case OP_FORK_WITH_ID:
	    {
		Var time;
		unsigned id = 0, f_index;
		double when;

		time = POP();
		f_index = READ_BYTES(bv, bc.numbytes_fork);
		if (op == OP_FORK_WITH_ID)
		    id = READ_BYTES(bv, bc.numbytes_var_name);
		if (time.type != TYPE_INT && time.type != TYPE_FLOAT) {
		    free_var(time);
		    RAISE_ERROR(E_TYPE);
		}
		when = time.type == TYPE_INT ? time.v.num : * time.v.fnum;
		free_var(time);
		if(when < 0) {
		    RAISE_ERROR(E_INVARG);
		} else {
		    enum error e;

		    e = enqueue_forked_task2(RUN_ACTIV, f_index, when,
			op == OP_FORK_WITH_ID ? id : -1);
		    if (e != E_NONE)
			RAISE_ERROR(e);
		}
	    }
	    break;

  	case OP_CALL_VERB:
  	    {
 		enum error err = E_NONE;
  		Var args, verb, obj;
 		Objid class;
  
  		args = POP();	/* args, should be list */
  		verb = POP();	/* verbname, should be string */
  		obj = POP();	/* objid, should be obj */
  
 		if (verb.type != TYPE_STR || args.type != TYPE_LIST) {
 			err = E_TYPE;
 		} else if (obj.type == TYPE_WAIF) {
 			char *str = mymalloc(strlen(verb.v.str) + 2,M_STRING);
 
 			class = obj.v.waif->class;
                         str[0] = WAIF_VERB_PREFIX;
                         strcpy(str + 1, verb.v.str);
                         free_str(verb.v.str);
                         verb.v.str = str;
 		} else if (obj.type == TYPE_OBJ) {
 			class = obj.v.obj;
 			if (verb.v.str[0] == WAIF_VERB_PREFIX)
 				err = E_VERBNF;
 		} else
 			err = E_TYPE;
 
 		if (err == E_NONE && !valid(class))
 			err = E_INVIND;
 
 		if (err == E_NONE) {
  		    STORE_STATE_VARIABLES();
 		    err = _call_verb(class, verb.v.str, obj, args, 0);
		    /* if there is no error, RUN_ACTIV is now the CALLEE's.
		       args will be consumed in the new rt_env */
		    /* if there is an error, then RUN_ACTIV is unchanged, and
		       args is not consumed in this case */
		    LOAD_STATE_VARIABLES();
		}
		free_var(obj);
		free_var(verb);

		if (err != E_NONE) {	/* there is an error, RUN_ACTIV unchanged, 
					   args must be freed */
		    free_var(args);
		    PUSH_ERROR(err);
		}
	    }
	    break;

	case OP_RETURN:
	case OP_RETURN0:
	case OP_DONE:
	    {
		Var ret_val;

		if (op == OP_RETURN)
		    ret_val = POP();
		else
		    ret_val = zero;

		STORE_STATE_VARIABLES();
		log_activation_for_profile("return");
		if (unwind_stack(FIN_RETURN, ret_val, &outcome)) {
		    if (result && outcome == OUTCOME_DONE)
			*result = ret_val;
		    else
			free_var(ret_val);
		    return outcome;
		}
		LOAD_STATE_VARIABLES();
	    }
	    break;

	case OP_BI_FUNC_CALL:
	    {
		unsigned func_id;
		Var args;

		func_id = READ_BYTES(bv, 1);	/* 1 == numbytes of func_id */
		args = POP();	/* should be list */
		if (args.type != TYPE_LIST) {
		    free_var(args);
		    PUSH_ERROR(E_TYPE);
		} else {
		    package p;

		    STORE_STATE_VARIABLES();
		    p = call_bi_func(func_id, args, 1, RUN_ACTIV.progr, 0);
		    LOAD_STATE_VARIABLES();

		    switch (p.kind) {
		    case BI_RETURN:
			PUSH(p.u.ret);
			break;
		    case BI_RAISE:
			if (RUN_ACTIV.debug) {
			    if (raise_error(p, 0))
				return OUTCOME_ABORTED;
			    else
				LOAD_STATE_VARIABLES();
			} else {
			    PUSH(p.u.raise.code);
			    free_str(p.u.raise.msg);
			    free_var(p.u.raise.value);
			}
			break;
		    case BI_CALL:
			/* another activ has been pushed onto activ_stack */
			RUN_ACTIV.bi_func_id = func_id;
			RUN_ACTIV.bi_func_data = p.u.call.data;
			RUN_ACTIV.bi_func_pc = p.u.call.pc;
			break;
		    case BI_SUSPEND:
			{
			    enum error e = suspend_task(p);

			    if (e == E_NONE)
				return OUTCOME_BLOCKED;
			    else
				PUSH_ERROR(e);
			}
			break;
		    }
		}
	    }
	    break;

	case OP_EXTENDED:
	    {
		register enum Extended_Opcode eop = *bv;
		bv++;
		if (COUNT_EOP_TICK(eop))
		    ticks_remaining--;
		switch (eop) {
		case EOP_RANGESET:
		    {
			Var base, from, to, value;

			value = POP();	/* rhs value (list or string) */
			to = POP();	/* end of range (integer) */
			from = POP();	/* start of range (integer) */
			base = POP();	/* lhs (list or string) */
			/* base[from..to] = value */
			if (to.type != TYPE_INT || from.type != TYPE_INT
			    || (base.type != TYPE_LIST && base.type != TYPE_STR)
			    || (value.type != TYPE_LIST && value.type != TYPE_STR)
			    || (base.type != value.type)) {
			    free_var(base);
			    free_var(to);
			    free_var(from);
			    free_var(value);
			    PUSH_ERROR(E_TYPE);
			} else if (rangeset_check(base.type == TYPE_STR
						  ? strlen(base.v.str)
						  : base.v.list[0].v.num,
						  from.v.num, to.v.num)) {
			    free_var(base);
			    free_var(to);
			    free_var(from);
			    free_var(value);
			    PUSH_ERROR(E_RANGE);
			} else if (base.type == TYPE_LIST)
			    PUSH(listrangeset(base, from.v.num, to.v.num, value));
			else	/* TYPE_STR */
			    PUSH(strrangeset(base, from.v.num, to.v.num, value));
		    }
		    break;

		case EOP_LENGTH:
		    {
			unsigned i = READ_BYTES(bv, bc.numbytes_stack);
			Var item, v;

			v.type = TYPE_INT;
			item = RUN_ACTIV.base_rt_stack[i];
			if (item.type == TYPE_STR) {
			    v.v.num = strlen(item.v.str);
			    PUSH(v);
			} else if (item.type == TYPE_LIST) {
			    v.v.num = item.v.list[0].v.num;
			    PUSH(v);
			} else
			    PUSH_ERROR(E_TYPE);
		    }
		    break;

		case EOP_EXP:
		    {
			Var lhs, rhs, ans;

			rhs = POP();
			lhs = POP();
			ans = do_power(lhs, rhs);
			free_var(lhs);
			free_var(rhs);
			if (ans.type == TYPE_ERR)
			    PUSH_ERROR(ans.v.err);
			else
			    PUSH(ans);
		    }
		    break;
		
		case EOP_MAKE_HASHENTRY:
		    {
			/* this has no purpose other than to quickly make
			 * a 2-element list for args to EOP_MAKE_HASH. */
			Var key, value, hashentry;
			
			value = POP();
			key = POP();
			
			if (key.type != TYPE_STR && key.type != TYPE_OBJ && 
			    key.type != TYPE_ERR && key.type != TYPE_INT &&
			    key.type != TYPE_FLOAT) {
				free_var(key);
				free_var(value);
				PUSH_ERROR(E_TYPE);
			} else {
				hashentry = new_list(2);
				hashentry.v.list[1] = key;
				hashentry.v.list[2] = value;
				PUSH(hashentry);
			}
		    }
		    break;
		

		case EOP_MAKE_HASH:
		    {
			int i;
			Var list, newhash, entry;
			
			list = POP();
			/* assuming everything went well, this should be 
			 * a list of hash entries made by EOP_MAKE_HASHENTRY... */
			
			newhash = new_hash();
			if (newhash.v.list[0].v.num == 0) {
				errlog("got 0-length list from new-hash!");
				abort();
			}
			for (i = 1; i <= list.v.list[0].v.num; i++) {
				entry = list.v.list[i];
				if (entry.type != TYPE_LIST) {
					free_var(list);
					errlog("non-list received by EOP_MAKE_HASH");
					PUSH_ERROR(E_TYPE);
					break;
				} else {
					newhash = hashset(newhash, var_ref(entry.v.list[1]), var_ref(entry.v.list[2]));
				}
			}
			
			free_var(list);
			PUSH(newhash);
		    }
		    break;

		case EOP_SCATTER:
		    {
			int nargs = READ_BYTES(bv, 1);
			int nreq = READ_BYTES(bv, 1);
			int rest = READ_BYTES(bv, 1);
			int have_rest = (rest > nargs ? 0 : 1);
			Var list;
			int len = 0, nopt_avail, nrest, i, offset;
			int done, where = 0;
			enum error e = E_NONE;

			list = TOP_RT_VALUE;
			if (list.type != TYPE_LIST)
			    e = E_TYPE;
			else if ((len = list.v.list[0].v.num) < nreq
				 || (!have_rest && len > nargs))
			    e = E_ARGS;

			if (e != E_NONE) {	/* skip rest of operands */
			    free_var(POP());	/* replace list with error code */
			    PUSH_ERROR(e);
			    for (i = 1; i <= nargs; i++) {
				READ_BYTES(bv, bc.numbytes_var_name);
				READ_BYTES(bv, bc.numbytes_label);
			    }
			} else {
			    nopt_avail = len - nreq;
			    nrest = (have_rest && len >= nargs ? len - nargs + 1
				     : 0);
			    for (offset = 0, i = 1; i <= nargs; i++) {
				int id = READ_BYTES(bv, bc.numbytes_var_name);
				int label = READ_BYTES(bv, bc.numbytes_label);

				if (i == rest) {	/* rest */
				    free_var(RUN_ACTIV.rt_env[id]);
				    RUN_ACTIV.rt_env[id] = sublist(var_ref(list),
								   i,
							  i + nrest - 1);
				    offset += nrest - 1;
				} else if (label == 0) {	/* required */
				    free_var(RUN_ACTIV.rt_env[id]);
				    RUN_ACTIV.rt_env[id] =
					var_ref(list.v.list[i + offset]);
				} else {	/* optional */
				    if (nopt_avail > 0) {
					nopt_avail--;
					free_var(RUN_ACTIV.rt_env[id]);
					RUN_ACTIV.rt_env[id] =
					    var_ref(list.v.list[i + offset]);
				    } else {
					offset--;
					if (where == 0 && label != 1)
					    where = label;
				    }
				}
			    }
			}

			done = READ_BYTES(bv, bc.numbytes_label);
			if (where == 0)
			    JUMP(done);
			else
			    JUMP(where);
		    }
		    break;

		case EOP_PUSH_LABEL:
		case EOP_TRY_FINALLY:
		    {
			Var v;

			v.type = (eop == EOP_PUSH_LABEL ? TYPE_INT : TYPE_FINALLY);
			v.v.num = READ_BYTES(bv, bc.numbytes_label);
			PUSH(v);
		    }
		    break;

		case EOP_CATCH:
		case EOP_TRY_EXCEPT:
		    {
			Var v;

			v.type = TYPE_CATCH;
			v.v.num = (eop == EOP_CATCH ? 1 : READ_BYTES(bv, 1));
			PUSH(v);
		    }
		    break;

		case EOP_END_CATCH:
		case EOP_END_EXCEPT:
		    {
			Var v, marker;
			int i;

			if (eop == EOP_END_CATCH)
			    v = POP();

			marker = POP();
			if (marker.type != TYPE_CATCH)
			    panic("Stack marker is not TYPE_CATCH!");
			for (i = 0; i < marker.v.num; i++) {
			    (void) POP();	/* handler PC */
			    free_var(POP());	/* code list */
			}

			if (eop == EOP_END_CATCH)
			    PUSH(v);

			JUMP(READ_BYTES(bv, bc.numbytes_label));
		    }
		    break;

		case EOP_END_FINALLY:
		    {
			Var v, why;

			v = POP();
			if (v.type != TYPE_FINALLY)
			    panic("Stack marker is not TYPE_FINALLY!");
			why.type = TYPE_INT;
			why.v.num = FIN_FALL_THRU;
			PUSH(why);
			PUSH(zero);
		    }
		    break;

		case EOP_CONTINUE:
		    {
			Var v, why;

			v = POP();
			why = POP();
			switch (why.type == TYPE_INT ? why.v.num : -1) {
			case FIN_FALL_THRU:
			    /* Do nothing; normal case. */
			    break;
			case FIN_EXIT:
			case FIN_RAISE:
			case FIN_RETURN:
			case FIN_UNCAUGHT:
			    STORE_STATE_VARIABLES();
			    if (unwind_stack(why.v.num, v, &outcome))
				return outcome;
			    LOAD_STATE_VARIABLES();
			    break;
			default:
			    panic("Unknown FINALLY reason!");
			}
		    }
		    break;

		case EOP_WHILE_ID:
		    {
			unsigned id = READ_BYTES(bv, bc.numbytes_var_name);
			free_var(RUN_ACTIV.rt_env[id]);
			RUN_ACTIV.rt_env[id] = var_ref(TOP_RT_VALUE);
		    }
		    goto do_test;

		case EOP_EXIT_ID:
		    READ_BYTES(bv, bc.numbytes_var_name);	/* ignore id */
		    /* fall thru */
		case EOP_EXIT:
		    {
			Var v;

			v = new_list(2);
			v.v.list[1].type = TYPE_INT;
			v.v.list[1].v.num = READ_BYTES(bv, bc.numbytes_stack);
			v.v.list[2].type = TYPE_INT;
			v.v.list[2].v.num = READ_BYTES(bv, bc.numbytes_label);
			STORE_STATE_VARIABLES();
			unwind_stack(FIN_EXIT, v, 0);
			LOAD_STATE_VARIABLES();
		    }
		    break;
		    
		case EOP_YIELD0:
		case EOP_YIELD:
		    if (ticks_remaining <= YIELD_THRESHOLD_TICKS
		     || timer_wakeup_interval(task_alarm_id) <= YIELD_THRESHOLD_SECONDS) {
		    	enum error e;
		    	double yield_secs;
			package p;

		    	if (eop == EOP_YIELD0) {
		    	    yield_secs = (double) YIELD_FOR_SECONDS;
		    	} else {
		    	   Var time = POP();
		    	   if (time.type != TYPE_INT) {
		    	   	free_var(time);
		    	   	PUSH_ERROR(E_TYPE);
		    	   	break;
		    	   }
		    	   if (time.v.num < 0) {
		    	   	PUSH_ERROR(E_INVARG);
		    	   	break;
		    	   }
		   	   yield_secs = (double) time.v.num;
		    	}
		    
		    	p = make_suspend_pack(enqueue_yielded_task, &yield_secs);
		    
			STORE_STATE_VARIABLES();	
		    	e = suspend_task(p);
		    	if (e == E_NONE) {
			    task_was_suspended = 2;
			    return OUTCOME_BLOCKED;
		    	} else {
			    PUSH_ERROR(e);
		    	}
	    	    } else {
	    	    	if (eop == EOP_YIELD) {
	    	    	    free_var(POP());
	    	    	}
	    	    }
		    break;

		default:
		    panic("Unknown extended opcode!");
		}
	    }
	    break;

	    /* These opcodes account for about 20% of all opcodes executed, so
	       let's split out the case stmt so the compiler can help us out.
	       If you're here because the #error below got tripped, just change
	       the set of case stmts below for OP_PUSH and OP_PUT to
	       be 0..NUM_READY_VARS-1.
	     */
#if NUM_READY_VARS != 32
#error NUM_READY_VARS expected to be 32
#endif
	case OP_PUSH:
	case OP_PUSH + 1:
	case OP_PUSH + 2:
	case OP_PUSH + 3:
	case OP_PUSH + 4:
	case OP_PUSH + 5:
	case OP_PUSH + 6:
	case OP_PUSH + 7:
	case OP_PUSH + 8:
	case OP_PUSH + 9:
	case OP_PUSH + 10:
	case OP_PUSH + 11:
	case OP_PUSH + 12:
	case OP_PUSH + 13:
	case OP_PUSH + 14:
	case OP_PUSH + 15:
	case OP_PUSH + 16:
	case OP_PUSH + 17:
	case OP_PUSH + 18:
	case OP_PUSH + 19:
	case OP_PUSH + 20:
	case OP_PUSH + 21:
	case OP_PUSH + 22:
	case OP_PUSH + 23:
	case OP_PUSH + 24:
	case OP_PUSH + 25:
	case OP_PUSH + 26:
	case OP_PUSH + 27:
	case OP_PUSH + 28:
	case OP_PUSH + 29:
	case OP_PUSH + 30:
	case OP_PUSH + 31:
	    {
		Var value;
		value = RUN_ACTIV.rt_env[PUSH_n_INDEX(op)];
		if (value.type == TYPE_NONE) {
		    free_var(value);
		    PUSH_ERROR(E_VARNF);
		} else
		    PUSH_REF(value);
	    }
	    break;

#ifdef BYTECODE_REDUCE_REF
	case OP_PUSH_CLEAR:
	case OP_PUSH_CLEAR + 1:
	case OP_PUSH_CLEAR + 2:
	case OP_PUSH_CLEAR + 3:
	case OP_PUSH_CLEAR + 4:
	case OP_PUSH_CLEAR + 5:
	case OP_PUSH_CLEAR + 6:
	case OP_PUSH_CLEAR + 7:
	case OP_PUSH_CLEAR + 8:
	case OP_PUSH_CLEAR + 9:
	case OP_PUSH_CLEAR + 10:
	case OP_PUSH_CLEAR + 11:
	case OP_PUSH_CLEAR + 12:
	case OP_PUSH_CLEAR + 13:
	case OP_PUSH_CLEAR + 14:
	case OP_PUSH_CLEAR + 15:
	case OP_PUSH_CLEAR + 16:
	case OP_PUSH_CLEAR + 17:
	case OP_PUSH_CLEAR + 18:
	case OP_PUSH_CLEAR + 19:
	case OP_PUSH_CLEAR + 20:
	case OP_PUSH_CLEAR + 21:
	case OP_PUSH_CLEAR + 22:
	case OP_PUSH_CLEAR + 23:
	case OP_PUSH_CLEAR + 24:
	case OP_PUSH_CLEAR + 25:
	case OP_PUSH_CLEAR + 26:
	case OP_PUSH_CLEAR + 27:
	case OP_PUSH_CLEAR + 28:
	case OP_PUSH_CLEAR + 29:
	case OP_PUSH_CLEAR + 30:
	case OP_PUSH_CLEAR + 31:
	    {
		Var *vp;
		vp = &RUN_ACTIV.rt_env[PUSH_CLEAR_n_INDEX(op)];
		if (vp->type == TYPE_NONE) {
		    PUSH_ERROR(E_VARNF);
		} else {
		    PUSH(*vp);
		    vp->type = TYPE_NONE;
		}
	    }
	    break;
#endif /* BYTECODE_REDUCE_REF */

	case OP_PUT:
	case OP_PUT + 1:
	case OP_PUT + 2:
	case OP_PUT + 3:
	case OP_PUT + 4:
	case OP_PUT + 5:
	case OP_PUT + 6:
	case OP_PUT + 7:
	case OP_PUT + 8:
	case OP_PUT + 9:
	case OP_PUT + 10:
	case OP_PUT + 11:
	case OP_PUT + 12:
	case OP_PUT + 13:
	case OP_PUT + 14:
	case OP_PUT + 15:
	case OP_PUT + 16:
	case OP_PUT + 17:
	case OP_PUT + 18:
	case OP_PUT + 19:
	case OP_PUT + 20:
	case OP_PUT + 21:
	case OP_PUT + 22:
	case OP_PUT + 23:
	case OP_PUT + 24:
	case OP_PUT + 25:
	case OP_PUT + 26:
	case OP_PUT + 27:
	case OP_PUT + 28:
	case OP_PUT + 29:
	case OP_PUT + 30:
	case OP_PUT + 31:
	    {
		Var *varp = &RUN_ACTIV.rt_env[PUT_n_INDEX(op)];
		free_var(*varp);
		if (bv[0] == OP_POP) {
		    *varp = POP();
		    ++bv;
		} else
		    *varp = var_ref(TOP_RT_VALUE);
	    }
	    break;

	default:
	    if (IS_OPTIM_NUM_OPCODE(op)) {
		Var value;
		value.type = TYPE_INT;
		value.v.num = OPCODE_TO_OPTIM_NUM(op);
		PUSH(value);
	    } else
		panic("Unknown opcode!");
	    break;
	}
    }
}


/**** manipulating data of task ****/

static int timeouts_enabled = 1;	/* set to 0 in debugger to disable
					   timeouts */

static void
task_timeout(Timer_ID id, Timer_Data data)
{
    task_timed_out = timeouts_enabled;
}

static Timer_ID
setup_task_execution_limits(int seconds, int ticks)
{
    task_alarm_id = set_virtual_timer(seconds < 1 ? 1 : seconds,
				      task_timeout, 0);
    task_timed_out = task_killed = 0;
    ticks_remaining = (ticks < 100 ? 100 : ticks);
    return task_alarm_id;
}

enum outcome
run_interpreter(char raise, enum error e,
		Var * result, int is_fg, int do_db_tracebacks)
    /* raise is boolean, true iff an error should be raised.
       e is the specific error to be raised if so.
       (in earlier versions, an error was raised iff e != E_NONE,
       but now it is possible to raise E_NONE on resumption from
       suspend().) */
{
    enum outcome ret;

    setup_task_execution_limits(is_fg ? DEFAULT_FG_SECONDS : DEFAULT_BG_SECONDS,
				is_fg ? DEFAULT_FG_TICKS : DEFAULT_BG_TICKS);
    task_was_suspended = 0;

    log_activation_for_profile("start_execution");
						 
    handler_verb_args = zero;
    handler_verb_name = 0;
    start_proftimer();
    interpreter_is_running = 1;
    ret = run(raise, e, result);
    interpreter_is_running = 0;
    stop_proftimer();

    task_timed_out = 0;
    cancel_timer(task_alarm_id);

    log_activation_for_profile("end_execution");

    if (ret == OUTCOME_ABORTED && handler_verb_name) {
	db_verb_handle h;
	Var args, handled, traceback;
	int i;

	args = handler_verb_args;
	h = db_find_callable_verb(SYSTEM_OBJECT, handler_verb_name);
	if (do_db_tracebacks && h.ptr) {
	    ret = do_server_verb_task(SYSTEM_OBJECT, handler_verb_name,
				      var_ref(handler_verb_args), h,
				 activ_stack[0].player, "", &handled, 0);
	    if ((ret == OUTCOME_DONE && is_true(handled))
		|| ret == OUTCOME_BLOCKED) {
		/* Assume the in-DB code handled it */
		free_var(args);
		return OUTCOME_ABORTED;		/* original ret value */
	    }
	}
	i = args.v.list[0].v.num;
	traceback = args.v.list[i];	/* traceback is always the last argument */
	for (i = 1; i <= traceback.v.list[0].v.num; i++)
	    notify(activ_stack[0].player, traceback.v.list[i].v.str);
	free_var(args);
    }
    if (task_was_suspended) {
	db_verb_handle h;
	Var args;

	h = db_find_callable_verb(SYSTEM_OBJECT, "task_suspended");
	if (h.ptr) {
	    args = new_list(2);
	    args.v.list[1].type = TYPE_INT;
	    args.v.list[1].v.num = task_was_suspended - 1;
	    args.v.list[2] = list_for_last_suspended_task();

	    do_server_verb_task(SYSTEM_OBJECT, "task_suspended", args, h, -1, "", 0, 0);
	}
    }
    return ret;
}


Objid
caller()
{
    return RUN_ACTIV.this;	/* XXX waifs?! */
}

static void
check_activ_stack_size(int max)
{
    if (max_stack_size != max) {
	if (activ_stack)
	    myfree(activ_stack, M_VM);

	activ_stack = mymalloc(sizeof(activation) * max, M_VM);
	max_stack_size = max;
    }
}

static int
current_max_stack_size(void)
{
    int max = server_int_option("max_stack_depth", DEFAULT_MAX_STACK_DEPTH);

    if (max < DEFAULT_MAX_STACK_DEPTH)
	max = DEFAULT_MAX_STACK_DEPTH;

    return max;
}

/**** There are two methods of starting a new task:
   (1) Create a new one
   (2) Resume an old one  */

/* procedure to create a new task */

static enum outcome
do_task(Program * prog, int which_vector, Var * result, int do_db_tracebacks)
{				/* which vector determines the vector for the root_activ.
				   a forked task can also have which_vector == MAIN_VECTOR.
				   this happens iff it is recovered from a read from disk,
				   because in that case the forked statement is parsed as 
				   the main vector */
    int forked = (current_task_kind == TASK_FORKED);

    RUN_ACTIV.prog = program_ref(prog);

    root_activ_vector = which_vector;	/* main or which of the forked */
    alloc_rt_stack(&RUN_ACTIV, (which_vector == MAIN_VECTOR
				? prog->main_vector.max_stack
			  : prog->fork_vectors[which_vector].max_stack));

    RUN_ACTIV.pc = 0;
    RUN_ACTIV.error_pc = 0;
    RUN_ACTIV.bi_func_pc = 0;
    RUN_ACTIV.temp.type = TYPE_NONE;

    log_activation_for_profile("newtask");

    return run_interpreter(0, E_NONE, result, !forked, do_db_tracebacks);
}

/* procedure to resume an old task */

enum outcome
resume_from_previous_vm(vm the_vm, Var v, task_kind kind, Var * result, int ignore_value)
{
    int i;

    current_task_kind = kind;
    check_activ_stack_size(the_vm->max_stack_size);
    top_activ_stack = the_vm->top_activ_stack;
    root_activ_vector = the_vm->root_activ_vector;
    for (i = 0; i <= top_activ_stack; i++)
	activ_stack[i] = the_vm->activ_stack[i];

    free_vm(the_vm, 0);

    if (v.type == TYPE_ERR)
	return run_interpreter(1, v.v.err, result, 0, 1);
    else {
	/* PUSH_REF(v) */
    	if (!ignore_value)
	    *(RUN_ACTIV.top_rt_stack++) = var_ref(v);

	return run_interpreter(0, E_NONE, result, 0, 1);
    }
}


/*** external functions ***/

enum outcome
do_server_verb_task(Objid this, const char *verb, Var args, db_verb_handle h,
		    Objid player, const char *argstr, Var * result,
		    int do_db_tracebacks)
{
    return do_server_program_task(this, verb, args, db_verb_definer(h),
				  db_verb_names(h), db_verb_program(h),
				  db_verb_owner(h),
				  db_verb_flags(h) & VF_DEBUG,
			       player, argstr, result, do_db_tracebacks);
}

enum outcome
do_server_program_task(Objid this, const char *verb, Var args, Objid vloc,
		    const char *verbname, Program * program, Objid progr,
		       int debug, Objid player, const char *argstr,
		       Var * result, int do_db_tracebacks)
{
    Var *env;

    current_task_kind = TASK_INPUT;
    check_activ_stack_size(current_max_stack_size());
    top_activ_stack = 0;

    RUN_ACTIV.rt_env = env = new_rt_env(program->num_var_names);
    RUN_ACTIV.this = this;
    RUN_ACTIV.THIS.type = TYPE_OBJ;
    RUN_ACTIV.THIS.v.obj = this;
    RUN_ACTIV.player = player;
    RUN_ACTIV.progr = progr;
    RUN_ACTIV.vloc = vloc;
    RUN_ACTIV.verb = str_dup(verb);
    RUN_ACTIV.verbname = str_dup(verbname);
    RUN_ACTIV.debug = debug;
    fill_in_rt_consts(env, program->version);
    set_rt_env_obj(env, SLOT_PLAYER, player);
    set_rt_env_obj(env, SLOT_CALLER, -1);
    set_rt_env_obj(env, SLOT_THIS, this);
    set_rt_env_obj(env, SLOT_DOBJ, NOTHING);
    set_rt_env_obj(env, SLOT_IOBJ, NOTHING);
    set_rt_env_str(env, SLOT_DOBJSTR, str_dup(""));
    set_rt_env_str(env, SLOT_IOBJSTR, str_dup(""));
    set_rt_env_str(env, SLOT_ARGSTR, str_dup(argstr));
    set_rt_env_str(env, SLOT_PREPSTR, str_dup(""));
    set_rt_env_str(env, SLOT_VERB, str_ref(RUN_ACTIV.verb));
    set_rt_env_var(env, SLOT_ARGS, args);

    return do_task(program, MAIN_VECTOR, result, do_db_tracebacks);
}

enum outcome
do_input_task(Objid user, Parsed_Command * pc, Objid this, db_verb_handle vh)
{
    Program *prog = db_verb_program(vh);
    Var *env;

    current_task_kind = TASK_INPUT;
    check_activ_stack_size(current_max_stack_size());
    top_activ_stack = 0;

    RUN_ACTIV.rt_env = env = new_rt_env(prog->num_var_names);
    RUN_ACTIV.this = this;
    RUN_ACTIV.THIS.type = TYPE_OBJ;
    RUN_ACTIV.THIS.v.obj = this;
    RUN_ACTIV.player = user;
    RUN_ACTIV.progr = db_verb_owner(vh);
    RUN_ACTIV.vloc = db_verb_definer(vh);
    RUN_ACTIV.verb = str_ref(pc->verb);
    RUN_ACTIV.verbname = str_ref(db_verb_names(vh));
    RUN_ACTIV.debug = (db_verb_flags(vh) & VF_DEBUG);
    fill_in_rt_consts(env, prog->version);
    set_rt_env_obj(env, SLOT_PLAYER, user);
    set_rt_env_obj(env, SLOT_CALLER, user);
    set_rt_env_obj(env, SLOT_THIS, this);
    set_rt_env_obj(env, SLOT_DOBJ, pc->dobj);
    set_rt_env_obj(env, SLOT_IOBJ, pc->iobj);
    set_rt_env_str(env, SLOT_DOBJSTR, str_ref(pc->dobjstr));
    set_rt_env_str(env, SLOT_IOBJSTR, str_ref(pc->iobjstr));
    set_rt_env_str(env, SLOT_ARGSTR, str_ref(pc->argstr));
    set_rt_env_str(env, SLOT_PREPSTR, str_ref(pc->prepstr));
    set_rt_env_str(env, SLOT_VERB, str_ref(pc->verb));
    set_rt_env_var(env, SLOT_ARGS, var_ref(pc->args));

    return do_task(prog, MAIN_VECTOR, 0, 1);
}

enum outcome
do_forked_task(Program * prog, Var * rt_env, activation a, int f_id,
	       Var * result)
{
    current_task_kind = TASK_FORKED;
    check_activ_stack_size(current_max_stack_size());
    top_activ_stack = 0;

    RUN_ACTIV = a;
    RUN_ACTIV.rt_env = rt_env;

    return do_task(prog, f_id, result, 1);
}

/* this is called from bf_eval to set up stack for an eval call */

int
setup_activ_for_eval(Program * prog)
{
    Var *env;
    if (!push_activation())
	return 0;

    RUN_ACTIV.prog = prog;

    RUN_ACTIV.rt_env = env = new_rt_env(prog->num_var_names);
    fill_in_rt_consts(env, prog->version);
    set_rt_env_obj(env, SLOT_PLAYER, CALLER_ACTIV.player);
    set_rt_env_obj(env, SLOT_CALLER, CALLER_ACTIV.this);
    set_rt_env_obj(env, SLOT_THIS, NOTHING);
    set_rt_env_obj(env, SLOT_DOBJ, NOTHING);
    set_rt_env_obj(env, SLOT_IOBJ, NOTHING);
    set_rt_env_str(env, SLOT_DOBJSTR, str_dup(""));
    set_rt_env_str(env, SLOT_IOBJSTR, str_dup(""));
    set_rt_env_str(env, SLOT_ARGSTR, str_dup(""));
    set_rt_env_str(env, SLOT_PREPSTR, str_dup(""));
    set_rt_env_str(env, SLOT_VERB, str_dup(""));
    set_rt_env_var(env, SLOT_ARGS, new_list(0));

    RUN_ACTIV.this = NOTHING;
    RUN_ACTIV.THIS.type = TYPE_OBJ;
    RUN_ACTIV.THIS.v.obj = NOTHING;
    RUN_ACTIV.player = CALLER_ACTIV.player;
    RUN_ACTIV.progr = CALLER_ACTIV.progr;
    RUN_ACTIV.vloc = NOTHING;
    RUN_ACTIV.verb = str_dup("");
    RUN_ACTIV.verbname = str_dup("Input to EVAL");
    RUN_ACTIV.debug = 1;
    alloc_rt_stack(&RUN_ACTIV, RUN_ACTIV.prog->main_vector.max_stack);
    RUN_ACTIV.pc = 0;
    RUN_ACTIV.error_pc = 0;
    RUN_ACTIV.temp.type = TYPE_NONE;
    RUN_ACTIV.cputime = 0;
    if (proftimer_on) {
        CALLER_ACTIV.cputime += read_proftimer();
        reset_proftimer();
    }

    return 1;
}

void
abort_running_task(void)
{
    task_killed = 1;
}

/**** built in functions ****/

struct cf_state {
    unsigned fnum;
    void *data;
};

static package
bf_call_function(Var arglist, Byte next, void *vdata, Objid progr)
{
    package p;
    unsigned fnum;
    struct cf_state *s;

    if (next == 1) {		/* first call */
	const char *fname = arglist.v.list[1].v.str;

	fnum = number_func_by_name(fname);
	if (fnum == FUNC_NOT_FOUND) {
	    p = make_raise_pack(E_INVARG, "Unknown built-in function",
				var_ref(arglist.v.list[1]));
	    free_var(arglist);
	} else {
	    arglist = listdelete(arglist, 1);
	    p = call_bi_func(fnum, arglist, next, progr, vdata);
	}
    } else {			/* return to function */
	s = vdata;
	fnum = s->fnum;
	p = call_bi_func(fnum, arglist, next, progr, s->data);
	free_data(s);
    }

    if (p.kind == BI_CALL) {
	s = alloc_data(sizeof(struct cf_state));
	s->fnum = fnum;
	s->data = p.u.call.data;
	p.u.call.data = s;
    }
    return p;
}

static void
bf_call_function_write(void *data)
{
    struct cf_state *s = data;

    dbio_printf("bf_call_function data: fname = %s\n",
		name_func_by_num(s->fnum));
    write_bi_func_data(s->data, s->fnum);
}

static void *
bf_call_function_read(void)
{
    struct cf_state *s = alloc_data(sizeof(struct cf_state));
    const char *line = dbio_read_string();
    const char *hdr = "bf_call_function data: fname = ";
    int hlen = strlen(hdr);

    if (!strncmp(line, hdr, hlen)) {
	line += hlen;
	if ((s->fnum = number_func_by_name(line)) == FUNC_NOT_FOUND)
	    errlog("CALL_FUNCTION: Unknown built-in function: %s\n", line);
	else if (read_bi_func_data(s->fnum, &s->data, pc_for_bi_func_data()))
	    return s;
    }
    return 0;
}

static package
bf_raise(Var arglist, Byte next, void *vdata, Objid progr)
{
    package p;
    int nargs = arglist.v.list[0].v.num;
    Var code = var_ref(arglist.v.list[1]);
    const char *msg = (nargs >= 2
		       ? str_ref(arglist.v.list[2].v.str)
		       : str_dup(value2str(code)));
    Var value;

    value = (nargs >= 3 ? var_ref(arglist.v.list[3]) : zero);
    free_var(arglist);
    p.kind = BI_RAISE;
    p.u.raise.code = code;
    p.u.raise.msg = msg;
    p.u.raise.value = value;

    return p;
}

static package
bf_suspend(Var arglist, Byte next, void *vdata, Objid progr)
{
    static double seconds, *secondsp = NULL;
    int nargs = arglist.v.list[0].v.num;

	if(nargs >= 1) {
		seconds = arglist.v.list[1].type == TYPE_INT ?
					arglist.v.list[1].v.num :
					*arglist.v.list[1].v.fnum;
		secondsp = &seconds;
	} else {
		secondsp = NULL;
	}
    free_var(arglist);

    if (nargs >= 1 && seconds < 0)
	return make_error_pack(E_INVARG);
    else {
        task_was_suspended = 1;
	return make_suspend_pack(enqueue_suspended_task, secondsp);
    }
}

static package
bf_read(Var arglist, Byte next, void *vdata, Objid progr)
{
    int argc = arglist.v.list[0].v.num;
    static Objid connection;
    int non_blocking = (argc >= 2
			&& is_true(arglist.v.list[2]));

    if (argc >= 1)
	connection = arglist.v.list[1].v.obj;
    else
	connection = activ_stack[0].player;
    free_var(arglist);

    /* Permissions checking */
    if (argc >= 1) {
	if (!is_wizard(progr)
	    && (!valid(connection)
		|| progr != db_object_owner(connection)))
	    return make_error_pack(E_PERM);
    } else {
	if (!is_wizard(progr)
	    || last_input_task_id(connection) != current_task_id)
	    return make_error_pack(E_PERM);
    }

    if (non_blocking) {
	Var r;

	r = read_input_now(connection);
	if (r.type == TYPE_ERR)
	    return make_error_pack(r.v.err);
	else
	    return make_var_pack(r);
    }
    return make_suspend_pack(make_reading_task, &connection);
}

static package
bf_seconds_left(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    r.type = TYPE_INT;
    r.v.num = timer_wakeup_interval(task_alarm_id);
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_ticks_left(Var arglist, Byte next, void *vdata, Objid progr)
{
    Var r;
    r.type = TYPE_INT;
    r.v.num = ticks_remaining;
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_cputime(Var arglist, Byte next, void *vdata, Objid progr)
{
    int timered;
    Var r;
    r.type = TYPE_INT;
    r.v.num = RUN_ACTIV.cputime + read_proftimer();
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_pass(Var arglist, Byte next, void *vdata, Objid progr)
{
    enum error e = _call_verb(RUN_ACTIV.this, RUN_ACTIV.verb, RUN_ACTIV.THIS,
			arglist, 1);

    if (e == E_NONE)
	return tail_call_pack();

    free_var(arglist);
    return make_error_pack(e);
}

static package
bf_call_verb(Var arglist, Byte next, void *vdata, Objid progr)
{
   Objid new_vloc = arglist.v.list[1].v.obj;
   Objid saved_vloc = RUN_ACTIV.vloc;

   if (!is_wizard(progr)) {
	free_var(arglist);
	return make_error_pack(E_PERM);
   }

   if (!valid(new_vloc)) {
	free_var(arglist);
	return make_error_pack(E_INVIND);
   }

   RUN_ACTIV.vloc = new_vloc;

   Objid new_this;
   if (arglist.v.list[0].v.num == 4)
	new_this = arglist.v.list[4].v.obj;
   else
	new_this = RUN_ACTIV.this;

   enum error e = call_verb(new_this, arglist.v.list[2].v.str, arglist.v.list[3], 2);

   if (e == E_NONE) {
	arglist.v.list[3] = zero; /* avoid double-free */
	free_var(arglist);
	return tail_call_pack();
   } else {
	RUN_ACTIV.vloc = saved_vloc;
	free_var(arglist);
	return make_error_pack(e);
   }
}

/*
 * Call a verb via verb number.
 * call_verb_index(OBJ vloc, INT vnum, STR vname, LIST vargs, [, OBJ this[, OBJ permissions]]) => ANY
 */
static package
bf_call_verb_index(Var arglist, Byte next, void *vdata, Objid progr)
{
    Objid where = arglist.v.list[1].v.obj;
    Objid new_this;

    if (!is_wizard(progr)) {
        free_var(arglist);
        return make_error_pack(E_PERM);
    }

    if (!valid(where)) {
        free_var(arglist);
        return make_error_pack(E_INVIND);
    }

    if (arglist.v.list[0].v.num >= 5) {
        new_this = arglist.v.list[5].v.obj;
    } else {
        new_this = RUN_ACTIV.this;
    }

    if (arglist.v.list[2].v.num < 1) {
        /* Don't bother looking for 0 or negative verb numbers. */
        free_var(arglist);
        return make_error_pack(E_VERBNF);
    }

    unsigned verb_index = arglist.v.list[2].v.num;

    if (arglist.v.list[0].v.num >= 6) {
        /* Drop permissions too. */
        RUN_ACTIV.progr = arglist.v.list[6].v.obj;
    }

    enum error e = call_verb_index(where, verb_index, new_this, arglist.v.list[3].v.str, arglist.v.list[4]);

    if (e == E_NONE) {
        arglist.v.list[4] = zero; /* avoid double-free */
        free_var(arglist);
        return tail_call_pack();
    } else {
        free_var(arglist);
        return make_error_pack(e);
    }
}

static package
bf_set_task_perms(Var arglist, Byte next, void *vdata, Objid progr)
{				/* (player) */
    /* warning!!  modifies top activation */
    Objid oid = arglist.v.list[1].v.obj;

    free_var(arglist);

    if (progr != oid && !is_wizard(progr))
	return make_error_pack(E_PERM);

    RUN_ACTIV.progr = oid;
    return no_var_pack();
}

static package
bf_caller_perms(Var arglist, Byte next, void *vdata, Objid progr)
{				/* () */
    Var r;
    r.type = TYPE_OBJ;
    if (top_activ_stack == 0)
	r.v.obj = NOTHING;
    else
	r.v.obj = activ_stack[top_activ_stack - 1].progr;
    free_var(arglist);
    return make_var_pack(r);
}

static package
bf_callers(Var arglist, Byte next, void *vdata, Objid progr)
{
    int options = 0;

    if (arglist.v.list[0].v.num >= 1) {
	if (arglist.v.list[1].type != TYPE_INT) {
	    free_var(arglist);
	    return make_error_pack(E_TYPE);
	}
	options = arglist.v.list[1].v.num;
    }
    free_var(arglist);

    return make_var_pack(make_stack_list(activ_stack, 0, top_activ_stack, 0,
				   root_activ_vector, options));
}

static package
bf_task_stack(Var arglist, Byte next, void *vdata, Objid progr)
{
    int nargs = arglist.v.list[0].v.num;
    int id = arglist.v.list[1].v.num;
    int options = 0;

    vm the_vm = find_suspended_task(id);
    Objid owner = (the_vm ? progr_of_cur_verb(the_vm) : NOTHING);
    if (nargs >= 2) {
        if (arglist.v.list[2].type != TYPE_INT) {
            free_var(arglist);
            return make_error_pack(E_TYPE);
        }
        options = arglist.v.list[2].v.num;
    }

    free_var(arglist);
    if (!the_vm)
	return make_error_pack(E_INVARG);
    if (!is_wizard(progr) && progr != owner)
	return make_error_pack(E_PERM);

    return make_var_pack(make_stack_list(the_vm->activ_stack, 0,
					 the_vm->top_activ_stack, 1,
					 the_vm->root_activ_vector,
					 options));
}

static package
bf_set_live_profiling(Var arglist, Byte next, void *vdata, Objid progr)
{
    if (!is_wizard(progr))
	return make_error_pack(E_INVARG);

    do_verb_profiling = arglist.v.list[1].v.num;
    if (do_verb_profiling) {
	oklog("*** PROFILING ACTIVE: Activated by %s (#%d) ***\n",
	      db_object_name(progr), progr);
    }
    else {
	oklog("*** PROFILING INACTIVE: Deactivated by %s (#%d) ***\n",
	      db_object_name(progr), progr);
    }
    return make_var_pack(arglist.v.list[1]);
}

void
register_execute(void)
{
    register_function_with_read_write("call_function", 1, -1, bf_call_function,
				      bf_call_function_read,
				      bf_call_function_write,
				      TYPE_STR);
    register_function("raise", 1, 3, bf_raise, TYPE_ANY, TYPE_STR, TYPE_ANY);
    register_function("suspend", 0, 1, bf_suspend, TYPE_NUMERIC);
    register_function("read", 0, 2, bf_read, TYPE_OBJ, TYPE_ANY);

    register_function("seconds_left", 0, 0, bf_seconds_left);
    register_function("ticks_left", 0, 0, bf_ticks_left);
    register_function("cputime", 0, 0, bf_cputime);
    register_function("pass", 0, -1, bf_pass);
    register_function("call_verb", 3, 4, bf_call_verb, 
	TYPE_OBJ, TYPE_STR, TYPE_LIST, TYPE_OBJ);
    register_function("call_verb_index", 4, 6, bf_call_verb_index,
        TYPE_OBJ, TYPE_INT, TYPE_STR, TYPE_LIST, TYPE_OBJ, TYPE_OBJ);
    register_function("set_task_perms", 1, 1, bf_set_task_perms, TYPE_OBJ);
    register_function("caller_perms", 0, 0, bf_caller_perms);
    register_function("callers", 0, 1, bf_callers, TYPE_ANY);
    register_function("task_stack", 1, 2, bf_task_stack, TYPE_INT, TYPE_ANY);
    register_function("set_live_profiling", 1, 1, bf_set_live_profiling, TYPE_INT);
}


/**** storing to/loading from database ****/

void
write_activ_as_pi(activation a)
{
    dbio_write_var(a.THIS);

    dbio_printf("%d %d %d %d %d %d %d %d %d\n",
	    a.this, -7, -8, a.player, -9, a.progr, a.vloc, -10, a.debug);
    dbio_write_string("No");
    dbio_write_string("More");
    dbio_write_string("Parse");
    dbio_write_string("Infos");
    dbio_write_string(a.verb);
    dbio_write_string(a.verbname);
}

int
read_activ_as_pi(activation * a)
{
    Var T;
    int dummy;
    char c;

    T = dbio_read_var();

    /* I use a `dummy' variable here and elsewhere instead of the `*'
     * assignment-suppression syntax of `scanf' because it allows more
     * straightforward error checking; unfortunately, the standard says that
     * suppressed assignments are not counted in determining the returned value
     * of `scanf'...
     */
    if (dbio_scanf("%d %d %d %d %d %d %d %d %d%c",
		 &a->this, &dummy, &dummy, &a->player, &dummy, &a->progr,
		   &a->vloc, &dummy, &a->debug, &c) != 10
	|| c != '\n') {
	errlog("READ_A: Bad numbers.\n");
	return 0;
    }
    switch (T.type) {
    case TYPE_WAIF:
    case TYPE_OBJ:
	break;
    default:
	T.type = TYPE_OBJ;
	T.v.obj = a->this;
	break;
    }
    a->THIS = T;

    dbio_read_string();		/* was argstr */
    dbio_read_string();		/* was dobjstr */
    dbio_read_string();		/* was iobjstr */
    dbio_read_string();		/* was prepstr */
    a->verb = dbio_read_string_intern();
    a->verbname = dbio_read_string_intern();
    return 1;
}

void
write_rt_env(const char **var_names, Var * rt_env, unsigned size)
{
    unsigned i;

    dbio_printf("%d variables\n", size);
    for (i = 0; i < size; i++) {
	dbio_write_string(var_names[i]);
	dbio_write_var(rt_env[i]);
    }
}

int
read_rt_env(const char ***old_names, Var ** rt_env, int *old_size)
{
    unsigned i;

    if (dbio_scanf("%d variables\n", old_size) != 1) {
	errlog("READ_RT_ENV: Bad count.\n");
	return 0;
    }
    *old_names = (const char **) mymalloc((*old_size) * sizeof(char *),
					  M_NAMES);
    *rt_env = new_rt_env(*old_size);

    for (i = 0; i < *old_size; i++) {
	(*old_names)[i] = dbio_read_string_intern();
	(*rt_env)[i] = dbio_read_var();
    }
    return 1;
}

Var *
reorder_rt_env(Var * old_rt_env, const char **old_names,
	       int old_size, Program * prog)
{
    /* reorder old_rt_env, which is aligned according to old_names, 
       to align to prog->var_names -- return the new rt_env
       after freeing old_rt_env and old_names */

    unsigned size = prog->num_var_names;
    Var *rt_env = new_rt_env(size);

    unsigned i;

    for (i = 0; i < size; i++) {
	int slot;

	for (slot = 0; slot < old_size; slot++) {
	    if (mystrcasecmp(old_names[slot], prog->var_names[i]) == 0)
		break;
	}

	if (slot < old_size)
	    rt_env[i] = var_ref(old_rt_env[slot]);
    }

    free_rt_env(old_rt_env, old_size);
    for (i = 0; i < old_size; i++)
	free_str(old_names[i]);
    myfree((void *) old_names, M_NAMES);

    return rt_env;
}

void
write_activ(activation a)
{
    register Var *v;

    dbio_printf("language version %u\n", a.prog->version);
    dbio_write_program(a.prog);
    write_rt_env(a.prog->var_names, a.rt_env, a.prog->num_var_names);

    dbio_printf("%d rt_stack slots in use\n",
		a.top_rt_stack - a.base_rt_stack);

    for (v = a.base_rt_stack; v != a.top_rt_stack; v++)
	dbio_write_var(*v);

    write_activ_as_pi(a);
    dbio_write_var(a.temp);

    dbio_printf("%u %u %u %u\n", a.pc, a.bi_func_pc, a.error_pc, a.cputime);
    if (a.bi_func_pc != 0) {
	dbio_write_string(name_func_by_num(a.bi_func_id));
	write_bi_func_data(a.bi_func_data, a.bi_func_id);
    }
}

static int
check_pc_validity(Program * prog, int which_vector, unsigned pc)
{
    Bytecodes *bc = (which_vector == -1
		     ? &prog->main_vector
		     : &prog->fork_vectors[which_vector]);

    /* Current insn must be call to verb or built-in function like eval(),
     * move(), pass(), suspend(), or a yield statement.
     */
    return (pc < bc->size
	    && (bc->vector[pc - 1] == OP_CALL_VERB
		|| bc->vector[pc - 2] == OP_BI_FUNC_CALL
		|| bc->vector[pc - 1] == EOP_YIELD0
		|| bc->vector[pc - 1] == EOP_YIELD));
}

int
read_activ(activation * a, int which_vector)
{
    DB_Version version;
    Var *old_rt_env;
    const char **old_names;
    int old_size, stack_in_use;
    unsigned i;
    const char *func_name;
    int max_stack;
    char c;

    if (dbio_input_version < DBV_Float)
	version = dbio_input_version;
    else if (dbio_scanf("language version %u\n", &version) != 1) {
	errlog("READ_ACTIV: Malformed language version\n");
	return 0;
    } else if (!check_version(version)) {
	errlog("READ_ACTIV: Unrecognized language version: %d\n",
	       version);
	return 0;
    }
    if (!(a->prog = dbio_read_program(version,
				      0, (void *) "suspended task"))) {
	errlog("READ_ACTIV: Malformed program\n");
	return 0;
    }
    if (!read_rt_env(&old_names, &old_rt_env, &old_size)) {
	errlog("READ_ACTIV: Malformed runtime environment\n");
	return 0;
    }
    a->rt_env = reorder_rt_env(old_rt_env, old_names, old_size, a->prog);

    max_stack = (which_vector == MAIN_VECTOR
		 ? a->prog->main_vector.max_stack
		 : a->prog->fork_vectors[which_vector].max_stack);
    alloc_rt_stack(a, max_stack);

    if (dbio_scanf("%d rt_stack slots in use\n", &stack_in_use) != 1) {
	errlog("READ_ACTIV: Bad stack_in_use number\n");
	return 0;
    }
    a->top_rt_stack = a->base_rt_stack;
    for (i = 0; i < stack_in_use; i++)
	*(a->top_rt_stack++) = dbio_read_var();

    if (!read_activ_as_pi(a)) {
	errlog("READ_ACTIV: Bad activ.\n", stack_in_use);
	return 0;
    }
    a->temp = dbio_read_var();

    if (dbio_scanf("%u %u%c", &a->pc, &i, &c) != 3) {
	errlog("READ_ACTIV: bad pc, next. stack_in_use = %d\n", stack_in_use);
	return 0;
    }
    a->bi_func_pc = i;

    if (c == '\n')
	a->error_pc = a->pc;
    else if (dbio_scanf("%u%c", &a->error_pc, &c) != 2) {
	errlog("READ_ACTIV: no error pc.\n");
	return 0;
    }

    if (c == '\n')
	a->cputime = 0;
    else if (dbio_scanf("%u\n", &a->cputime) != 1) {
        errlog("READ_ACTIV: bad cputime.\n");
        return 0;
    }

    if (!check_pc_validity(a->prog, which_vector, a->pc)) {
	errlog("READ_ACTIV: Bad PC for suspended task.\n");
	return 0;
    }
    if (a->bi_func_pc != 0) {
	func_name = dbio_read_string();
	a->bi_func_id = number_func_by_name(func_name);
	if (a->bi_func_id == FUNC_NOT_FOUND) {
	    errlog("READ_ACTIV: Unknown built-in function `%s'\n", func_name);
	    return 0;
	}
	if (!read_bi_func_data(a->bi_func_id, &a->bi_func_data,
			       &a->bi_func_pc)) {
	    errlog("READ_ACTIV: Bad saved state for built-in function `%s'\n",
		   func_name);
	    return 0;
	}
    }
    a->cputime = 0;
    return 1;
}


char rcsid_execute[] = "$Id: execute.c,v 1.19 2014/02/26 09:02:00 broseidon Exp $";

/* 
 * $Log: execute.c,v $
 * Revision 1.19  2010/05/17 07:25:35  blacklite
 * last fixes for 1.10.4
 *
 * Revision 1.18  2010/05/17 05:52:47  blacklite
 * cputime tweaks
 *
 * Revision 1.17  2010/05/17 04:26:05  blacklite
 * add bf_cputime for reals
 *
 * Revision 1.16  2009/10/11 00:31:35  blacklite
 * rewrite OP_FOR_LIST for paranoia/sanity reasons -- should be functionally equivalent, but whatever, this is the copy that works
 *
 * Revision 1.14  2009/08/14 21:35:38  blacklite
 * call #0:task_suspended whenever it happens.
 *
 * Revision 1.13  2009/08/14 16:38:03  blacklite
 * fix horrible has bugs! use the return value from hashset! i am dumb!
 *
 * Revision 1.12  2009/07/27 01:47:21  blacklite
 * move from a preproc define for profiling to set_live_profiling().
 *
 * Revision 1.11  2009/07/26 20:00:56  blacklite
 * add optional 4th arg for 'this' to call_verb
 *
 * Revision 1.10  2009/07/25 04:36:58  blacklite
 * remove debug oklogs from testing bf_call_verb
 *
 * Revision 1.9  2009/07/25 04:20:57  blacklite
 * drop bf_transparent_pass in favour of the new bf_call_verb, which calls
 * a verb elsewhere while preserving the value of 'this'.
 *
 * Revision 1.8  2009/07/25 03:23:18  blacklite
 * cut out unused profile helper funcs when !PROFILE_VERBS
 *
 * Revision 1.7  2009/07/25 03:21:59  blacklite
 * add profiling when PROFILE_VERBS is defined, spits info out to a profile log
 * (specify with -p). I may still be missing some spots but this seems to give
 * usable info right now.
 *
 * Revision 1.6  2009/07/23 00:49:36  blacklite
 * add transparent_pass, minor tweaks to yield (probably didn't fix anything..)
 *
 * Revision 1.5  2009/03/27 20:26:49  blacklite
 * add optional argument to YIELD statement, make no-arg version into YIELD0 expression/op. add newer ops/exprs to disassembly. handle PF_PRIVATE in execute. make some vars 'register' in execute.
 *
 * Revision 1.4  2009/03/08 12:41:31  blacklite
 * Added HASH data type, yield keyword, MEMORY_TRACE, vfscanf(),
 * extra myrealloc() and memcpy() tricks for lists, Valgrind
 * support for str_intern.c, etc. See ChangeLog.txt.
 *
 * Revision 1.3  2007/09/12 07:33:29  spunky
 * This is a working version of the current HellMOO server
 *
 * Revision 1.10  1998/12/14 13:17:50  nop
 * Merge UNSAFE_OPTS (ref fixups); fix Log tag placement to fit CVS whims
 *
 * Revision 1.9  1998/02/19 07:36:17  nop
 * Initial string interning during db load.
 *
 * Revision 1.8  1997/07/07 03:24:54  nop
 * Merge UNSAFE_OPTS (r5) after extensive testing.
 *
 * Revision 1.7  1997/03/21 13:23:23  bjj
 * Reorganize the top of run() slightly to make it slightly more efficient
 * (we do execute it billions of times, after all).  Later we'll want to
 * get rid of if (task_killed) by introducing BI_KILL or by moving it into
 * the BI_FUNC_CALL case, at least.
 *
 * Revision 1.6.2.4  1998/12/06 07:13:21  bjj
 * Rationalize enqueue_forked_task interface and fix program_ref leak in
 * the case where fork fails with E_QUOTA.  Make .queued_task_limit=0 really
 * enforce a limit of zero tasks (for old behavior set it to 1, that's the
 * effect it used to have).
 *
 * Revision 1.6.2.3  1997/09/09 07:01:17  bjj
 * Change bytecode generation so that x=f(x) calls f() without holding a ref
 * to the value of x in the variable slot.  See the options.h comment for
 * BYTECODE_REDUCE_REF for more details.
 *
 * This checkin also makes x[y]=z (OP_INDEXSET) take advantage of that (that
 * new code is not conditional and still works either way).
 *
 * Revision 1.6.2.2  1997/05/24 07:08:37  bjj
 * Cleanup of Jay's last checkin to avoid some code duplication.
 *
 * Revision 1.6.2.1  1997/05/23 07:03:44  nop
 * Failure during property lookups/stores sometimes fails to free the string
 * containing the property name.  (PUSH_ERROR() may return immediately.)
 *
 * Revision 1.6  1997/03/08 06:25:39  nop
 * 1.8.0p6 merge by hand.
 *
 * Revision 1.5  1997/03/05 08:41:47  bjj
 * A few malloc-friendly changes:  rt_stacks are now centrally allocated/freed
 * so that we can keep a pool of them handy.  rt_envs are similarly pooled.
 * Both revert to malloc/free for large requests.
 *
 * Revision 1.4  1997/03/03 09:03:31  bjj
 * 3 opcode optimizations:
 *
 * 1)  OP_IMM+OP_POP is "peephole optimized" away at runtime.  This makes
 * verbdocs and other comments cheaper.
 *
 * 2)  OP_PUT_n+OP_POP is similarly optimized (PUT doesn't consume the
 * top value on the stack but it is often used that way in statements like
 * `var = expr;').  OP_G_PUT could use the same change but is rarely
 * executed.
 *
 * 3)  OP_PUT_n, OP_PUSH_n which used to be in an if/else in the default
 * case are split out into 32 cases each so the compiler can optimize it
 * for us.  These ops account for a large percentage of those executed.
 *
 * Revision 1.3  1997/03/03 06:14:44  nop
 * Nobody actually uses protected properties.  Make IGNORE_PROP_PROTECTED
 * the default.
 *
 * Revision 1.2  1997/03/03 04:18:38  nop
 * GNU Indent normalization
 *
 * Revision 1.1.1.1  1997/03/03 03:44:59  nop
 * LambdaMOO 1.8.0p5
 *
 * Revision 2.11  1997/03/04 04:31:48  eostrom
 * Modified run() and run_interpreter() to take a separate argument
 * indicating whether to raise an exception, rather than assuming E_NONE
 * means no and anything else means yes.
 *
 * Revision 2.10  1996/04/19  01:24:40  pavel
 * Added support for built-in functions making tail calls to MOO verbs and
 * changed pass() to use the new feature.  Added patches to allow generation
 * of the new warning in read_bi_func_data().  Release 1.8.0p4.
 *
 * Revision 2.9  1996/03/19  07:12:21  pavel
 * Fixed call_verb() to return E_INVIND when appropriate even when pass==0.
 * Removed is_user() test for setting .programmer and .wizard properties.
 * Reordered error tests in bf_task_stack() to check for an invalid argument
 * first.  Release 1.8.0p2.
 *
 * Revision 2.8  1996/03/10  01:18:22  pavel
 * Added new `caller()' entry point, for use by built-in fns.  Release 1.8.0.
 *
 * Revision 2.7  1996/02/11  00:44:01  pavel
 * Fixed handling of `kill_task(task_id())'.  Release 1.8.0beta2.
 *
 * Revision 2.6  1996/02/08  07:11:17  pavel
 * Added support for in-DB traceback handling, named WHILE loop, BREAK and
 * CONTINUE statement, exponentiation expression, and floating-point
 * arithmetic.  Fixed logging of wiz-bit setting to happen on any change in
 * value in either direction.  Added tick-counting for EOPs.  Fixed stack bug
 * in `$' expression.  Fixed horrible bugs in scattering assignment.  Added
 * version numbers on each frame of suspended tasks.  Renamed err/logf() to
 * errlog/oklog() and TYPE_NUM to TYPE_INT.  Updated copyright notice for
 * 1996.  Release 1.8.0beta1.
 *
 * Revision 2.5  1996/01/16  07:19:34  pavel
 * Fixed bug concerning which frame built-in function return data is stored
 * into.  Fixed tracebacks for wizarding to be accurate and only generated for
 * setting .wizard to true.  Added support for EOP_SCATTER.
 * Release 1.8.0alpha6.
 *
 * Revision 2.4  1996/01/11  07:28:04  pavel
 * Fixed bug in value of `progr' when MOO code returns to a built-in.  Added
 * support for getting the value of a resumed task.  Release 1.8.0alpha5.
 *
 * Revision 2.3  1995/12/31  03:12:19  pavel
 * Implemented the EOP_LENGTH opcode.  Release 1.8.0alpha4.
 *
 * Revision 2.2  1995/12/28  00:23:15  pavel
 * Fixed bug in unwinding through handlers that don't cover the current error.
 * Fixed bug in not squelching all errors raised by built-ins called from !d
 * verbs.  Commented conventions for errors vs. built-ins.  Fixed memory leaks
 * in built-in property reference, errors raised in !d verbs, and verbnames
 * read from suspended tasks in DB files.  Added call_function() built-in.
 * Fixed bug in registration of suspend().  Added support for better error
 * messages during DB loading of suspended task programs.
 * Release 1.8.0alpha3.
 *
 * Revision 2.1  1995/12/11  07:51:30  pavel
 * Added built-in function entries to stack traceback lists.  Accounted for
 * verb programs never being NULL any more.  Added support for built-in
 * properties being wiz-only.  Removed one more silly use of `unsigned'.
 * Added support for suspending forever.  Fixed registration of `read()' to
 * once again allow non-blocking mode.  Added `task_stack()' built-in
 * function.
 *
 * Release 1.8.0alpha2.
 *
 * Revision 2.0  1995/11/30  04:23:34  pavel
 * New baseline version, corresponding to release 1.8.0alpha1.
 *
 * Revision 1.27  1992/10/27  06:19:22  pavel
 * Fixed a memory leak that occurred when a read() call was rejected for
 * E_INVARG reasons.
 *
 * Revision 1.26  1992/10/23  23:03:47  pavel
 * Added copyright notice.
 *
 * Revision 1.25  1992/10/23  19:25:00  pavel
 * Eliminated all uses of the useless macro NULL..
 *
 * Revision 1.24  1992/10/21  03:02:35  pavel
 * Converted to use new automatic configuration system.
 *
 * Revision 1.23  1992/10/17  20:28:48  pavel
 * Global rename of strdup->str_dup, strref->str_ref, vardup->var_dup, and
 * varref->var_ref.
 * Added some (int) casts to placate over-protective compilers.
 * Changed return-type of read_activ() from char to int, for systems that use
 * unsigned chars.
 *
 * Revision 1.22  1992/09/25  21:11:36  pjames
 * Made use of the error_pc field in an activation to report correct
 * error tracebacks from verbs called by builtin verbs.
 *
 * Revision 1.21  1992/09/24  16:43:10  pavel
 * Exported `task_timed_out' for use by long-running built-in functions that
 * should abort if the timer goes off.
 *
 * Revision 1.20  1992/09/14  18:40:02  pjames
 * Updated #includes.  Moved rcsid to bottom.
 *
 * Revision 1.19  1992/09/14  17:45:17  pjames
 * Moved db_modification code to db modules.
 *
 * Revision 1.18  1992/09/08  22:07:06  pjames
 * Changed `register_bf_execute()' to `register_execute()'.
 * Changed range checking for subrange assigments.
 *
 * Revision 1.17  1992/09/04  22:42:05  pavel
 * Fixed some picky ANSI C problems with (const char *)'s.
 *
 * Revision 1.16  1992/09/03  16:29:44  pjames
 * Minor change.
 *
 * Revision 1.15  1992/09/02  18:41:17  pavel
 * Added mechanism for resuming tasks with errors.
 *
 * Revision 1.14  1992/09/01  06:24:01  pavel
 * Fixed minor storage leak just introduced.
 *
 * Revision 1.13  1992/09/01  05:30:57  pavel
 * Added a call to vardup() for first argument to listset().
 *
 * Revision 1.12  1992/08/31  22:29:01  pjames
 * Changed some `char *'s to `const char *' and fixed code accordingly.
 *
 * Revision 1.11  1992/08/28  23:23:03  pjames
 * Added support for EXTENDED instructions.
 * Added OP_RANGESET interpreting.
 * Changed OP_LISTSET to OP_INDEXSET, and added code for indexed string
 * assigments.
 *
 * Revision 1.10  1992/08/28  16:30:45  pjames
 * Changed myfree(*, M_STRING) to free_str(*).
 * Changed vardup to varref.
 * Changed some strref's to strdup.
 *
 * Revision 1.9  1992/08/12  02:01:46  pjames
 * Gave a variable an initial value to get rid on a bogus compiler
 * warning.
 *
 * Revision 1.8  1992/08/12  01:48:03  pjames
 * Fixed string freeing/copying to/from activation/forked_tasks.
 *
 * Revision 1.7  1992/08/11  17:30:44  pjames
 * Removed last read/write of Parse_Info, fixed a bug in a macro
 * (commented back in a line), and fixed call_verb (to not run the
 * program if it doesn't exist).
 *
 * Revision 1.6  1992/08/10  17:47:00  pjames
 * Moved several functions to eval_env.c and eval_vm.c.  Moved built in
 * functions which modified vm to this module.  Move property functions
 * to property.c.  Updated #includes.  Modified run() to be cleaner.
 * Used new built in function registration methods.  Removed uses of
 * Parse_Info field in activations.
 *
 * Revision 1.5  1992/08/01  01:14:52  pavel
 * Minor fixes...
 *
 * Revision 1.4  1992/07/30  21:18:48  pjames
 * Updated print_error_backtrace to print line numbers for each frame on
 * the stack.
 *
 * Revision 1.3  1992/07/27  18:14:17  pjames
 * Changed name of ct_env to var_names, const_env to literals, and
 * f_vectors to fork_vectors.
 *
 * Revision 1.2  1992/07/21  00:02:07  pavel
 * Added rcsid_<filename-root> declaration to hold the RCS ident. string.
 *
 * Revision 1.1  1992/07/20  23:23:12  pavel
 * Initial RCS-controlled version.
 */
