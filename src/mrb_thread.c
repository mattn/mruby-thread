#include <mruby.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/range.h>
#include <mruby/proc.h>
#include <mruby/data.h>
#include <mruby/class.h>
#include <mruby/value.h>
#include <mruby/variable.h>
#include <mruby/dump.h>
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#define _TIMESPEC_DEFINED
#endif
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

/*
For backward compatibility.
See also https://github.com/mruby/mruby/commit/79a621dd739faf4cc0958e11d6a887331cf79e48
*/
#ifdef mrb_range_ptr
#define MRB_RANGE_PTR(v) mrb_range_ptr(v)
#else
#define MRB_RANGE_PTR(v) mrb_range_ptr(mrb, v)
#endif

typedef struct {
  int argc;
  mrb_value* argv;
  struct RProc* proc;
  pthread_t thread;
  mrb_state* mrb_caller;
  mrb_state* mrb;
  mrb_value result;
  mrb_bool alive;
} mrb_thread_context;

static void
mrb_thread_context_free(mrb_state *mrb, void *p) {
  if (p) {
    mrb_thread_context* context = (mrb_thread_context*) p;
    if (context->mrb && context->mrb != mrb) mrb_close(context->mrb);
    if (context->alive) pthread_kill(context->thread, SIGINT);
    if (context->argv) free(context->argv);
    free(p);
  }
}

static const struct mrb_data_type mrb_thread_context_type = {
  "mrb_thread_context", mrb_thread_context_free,
};

typedef struct {
  pthread_mutex_t mutex;
  int locked;
} mrb_mutex_context;

static void
mrb_mutex_context_free(mrb_state *mrb, void *p) {
  if (p) {
    mrb_mutex_context* context = (mrb_mutex_context*) p;
    pthread_mutex_destroy(&context->mutex);
    free(p);
  }
}

static const struct mrb_data_type mrb_mutex_context_type = {
  "mrb_mutex_context", mrb_mutex_context_free,
};

typedef struct {
  pthread_mutex_t mutex;
  pthread_mutex_t queue_lock;
  mrb_state* mrb;
  mrb_value queue;
} mrb_queue_context;

static void
mrb_queue_context_free(mrb_state *mrb, void *p) {
  if (p) {
    mrb_queue_context* context = (mrb_queue_context*) p;
    pthread_mutex_destroy(&context->mutex);
    pthread_mutex_destroy(&context->queue_lock);
    free(p);
  }
}

static const struct mrb_data_type mrb_queue_context_type = {
  "mrb_queue_context", mrb_queue_context_free,
};

static mrb_value migrate_simple_value(mrb_state *mrb, mrb_value v, mrb_state *mrb2);

static mrb_sym
migrate_sym(mrb_state *mrb, mrb_sym sym, mrb_state *mrb2)
{
  mrb_int len;
  const char *p = mrb_sym2name_len(mrb, sym, &len);
  return mrb_intern_static(mrb2, p, len);
}

static void
migrate_all_symbols(mrb_state *mrb, mrb_state *mrb2)
{
  mrb_sym i;
  for (i = 1; i < mrb->symidx + 1; i++) {
    migrate_sym(mrb, i, mrb2);
  }
}

static void
migrate_simple_iv(mrb_state *mrb, mrb_value v, mrb_state *mrb2, mrb_value v2)
{
  mrb_value ivars = mrb_obj_instance_variables(mrb, v);
  struct RArray *a = mrb_ary_ptr(ivars);
  mrb_value iv;
  mrb_int i;

  for (i=0; i<a->len; i++) {
    mrb_sym sym = mrb_symbol(a->ptr[i]);
    mrb_sym sym2 = migrate_sym(mrb, sym, mrb2);
    iv = mrb_iv_get(mrb, v, sym);
    mrb_iv_set(mrb2, v2, sym2, migrate_simple_value(mrb, iv, mrb2));
  }
}

static mrb_bool
is_safe_migratable_datatype(const mrb_data_type *type)
{
  static const char *known_type_names[] = {
    "mrb_mutex_context",
    "mrb_queue_context",
    "IO",
    NULL
  };
  int i;
  for (i = 0; known_type_names[i]; i++) {
    if (strcmp(type->struct_name, known_type_names[i]) == 0)
      return TRUE;
  }
  return FALSE;
}

static mrb_bool
is_safe_migratable_simple_value(mrb_state *mrb, mrb_value v, mrb_state *mrb2)
{
  switch (mrb_type(v)) {
  case MRB_TT_OBJECT:
  case MRB_TT_EXCEPTION:
    {
      struct RObject *o = mrb_obj_ptr(v);
      mrb_value path = mrb_class_path(mrb, o->c);

      if (mrb_nil_p(path) || !mrb_class_defined(mrb2, RSTRING_PTR(path))) {
        return FALSE;
      }
    }
    break;
  case MRB_TT_PROC:
  case MRB_TT_FALSE:
  case MRB_TT_TRUE:
  case MRB_TT_FIXNUM:
  case MRB_TT_SYMBOL:
  case MRB_TT_FLOAT:
  case MRB_TT_STRING:
    break;
  case MRB_TT_RANGE:
    {
      struct RRange *r = MRB_RANGE_PTR(v);
      if (!is_safe_migratable_simple_value(mrb, r->edges->beg, mrb2) ||
          !is_safe_migratable_simple_value(mrb, r->edges->end, mrb2)) {
        return FALSE;
      }
    }
    break;
  case MRB_TT_ARRAY:
    {
      struct RArray *a0;
      int i;
      a0 = mrb_ary_ptr(v);
      for (i=0; i<a0->len; i++) {
        if (!is_safe_migratable_simple_value(mrb, a0->ptr[i], mrb2)) {
          return FALSE;
        }
      }
    }
    break;
  case MRB_TT_HASH:
    {
      mrb_value ka;
      int i, l;
      ka = mrb_hash_keys(mrb, v);
      l = RARRAY_LEN(ka);
      for (i = 0; i < l; i++) {
        mrb_value k = mrb_ary_entry(ka, i);
        if (!is_safe_migratable_simple_value(mrb, k, mrb2) ||
            !is_safe_migratable_simple_value(mrb, mrb_hash_get(mrb, v, k), mrb2)) {
          return FALSE;
        }
      }
    }
    break;
  case MRB_TT_DATA:
    if (!is_safe_migratable_datatype(DATA_TYPE(v)))
      return FALSE;
    break;
  default:
    return FALSE;
    break;
  }
  return TRUE;
}

static mrb_irep*
migrate_irep(mrb_state *mrb, mrb_irep *src, mrb_state *mrb2) {
  uint8_t *irep = NULL;
  size_t binsize = 0;
  int i;
  mrb_dump_irep(mrb, src, DUMP_ENDIAN_NAT, &irep, &binsize);

  mrb_irep *ret = mrb_read_irep(mrb2, irep);
  for (i = 0; i < src->slen; i++) {
    mrb_sym newsym = migrate_sym(mrb, src->syms[i], mrb2);
    ret->syms[i] = newsym;
  }
  return ret;
}

struct RProc*
migrate_rproc(mrb_state *mrb, struct RProc *rproc, mrb_state *mrb2) {
  struct RProc *newproc = mrb_closure_new(mrb2, migrate_irep(mrb, rproc->body.irep, mrb2));
  newproc->env = rproc->env;
  newproc->env->mid = migrate_sym(mrb, rproc->env->mid, mrb2);
  return newproc;
}

// based on https://gist.github.com/3066997
static mrb_value
migrate_simple_value(mrb_state *mrb, mrb_value v, mrb_state *mrb2) {
  mrb_value nv;

  switch (mrb_type(v)) {
#ifdef MRB_THREAD_COPY_VALUES
  case MRB_TT_OBJECT:
  case MRB_TT_EXCEPTION:
    {
      struct RClass *c = mrb_obj_class(mrb, v);
      nv = mrb_obj_value(mrb_obj_alloc(mrb2, mrb_type(v), c));
    }
    migrate_simple_iv(mrb, v, mrb2, nv);
    break;
  case MRB_TT_PROC:
    {
      struct RProc *rproc = mrb_proc_ptr(v);
      nv = mrb_obj_value(migrate_rproc(mrb, rproc, mrb2));
    }
    break;
  case MRB_TT_FALSE:
  case MRB_TT_TRUE:
  case MRB_TT_FIXNUM:
    nv = v;
    break;
  case MRB_TT_SYMBOL:
    nv = mrb_symbol_value(migrate_sym(mrb, mrb_symbol(v), mrb2));
    break;
  case MRB_TT_FLOAT:
    nv = mrb_float_value(mrb2, mrb_float(v));
    break;
  case MRB_TT_STRING:
    nv = mrb_str_new(mrb2, RSTRING_PTR(v), RSTRING_LEN(v));
    break;
  case MRB_TT_RANGE:
    {
      struct RRange *r = MRB_RANGE_PTR(v);
      nv = mrb_range_new(mrb2,
                         migrate_simple_value(mrb, r->edges->beg, mrb2),
                         migrate_simple_value(mrb, r->edges->end, mrb2),
                         r->excl);
    }
    break;
  case MRB_TT_ARRAY:
    {
      struct RArray *a0, *a1;
      int i;

      a0 = mrb_ary_ptr(v);
      nv = mrb_ary_new_capa(mrb2, a0->len);
      a1 = mrb_ary_ptr(nv);
      for (i=0; i<a0->len; i++) {
        int ai = mrb_gc_arena_save(mrb2);
        a1->ptr[i] = migrate_simple_value(mrb, a0->ptr[i], mrb2);
        a1->len++;
        mrb_gc_arena_restore(mrb2, ai);
      }
    }
    break;
  case MRB_TT_HASH:
    {
      mrb_value ka;
      int i, l;

      nv = mrb_hash_new(mrb2);
      ka = mrb_hash_keys(mrb, v);
      l = RARRAY_LEN(ka);
      for (i = 0; i < l; i++) {
        int ai = mrb_gc_arena_save(mrb2);
        mrb_value k = migrate_simple_value(mrb, mrb_ary_entry(ka, i), mrb2);
        mrb_value o = migrate_simple_value(mrb, mrb_hash_get(mrb, v, k), mrb2);
        mrb_hash_set(mrb2, nv, k, o);
        mrb_gc_arena_restore(mrb2, ai);
      }
    }
    migrate_simple_iv(mrb, v, mrb2, nv);
    break;
#else
    case MRB_TT_OBJECT:
    case MRB_TT_EXCEPTION:
    case MRB_TT_PROC:
    case MRB_TT_FALSE:
    case MRB_TT_TRUE:
    case MRB_TT_FIXNUM:
    case MRB_TT_SYMBOL:
    case MRB_TT_FLOAT:
    case MRB_TT_STRING:
    case MRB_TT_RANGE:
    case MRB_TT_ARRAY:
    case MRB_TT_HASH:
      nv = v;
      break;
#endif
  case MRB_TT_DATA:
    if (!is_safe_migratable_datatype(DATA_TYPE(v)))
      mrb_raise(mrb, E_TYPE_ERROR, "cannot migrate object");
    nv = v;
    DATA_PTR(nv) = DATA_PTR(v);
    DATA_TYPE(nv) = DATA_TYPE(v);
    migrate_simple_iv(mrb, v, mrb2, nv);
    break;
  default:
    mrb_raise(mrb, E_TYPE_ERROR, "cannot migrate object");
    break;
  }
  return nv;
}

static void*
mrb_thread_func(void* data) {
  mrb_thread_context* context = (mrb_thread_context*) data;
  mrb_state* mrb = context->mrb;
  context->result = mrb_yield_with_class(mrb, mrb_obj_value(context->proc),
                                         context->argc, context->argv, mrb_nil_value(), mrb->object_class);
  context->alive = FALSE;
  return NULL;
}

#include "mrb_init_functions.h"
#define DONE mrb_gc_arena_restore(mrb, 0);

/* Base from mrb_open_core in state.c */
static mrb_state*
mrb_symbol_safe_copy(mrb_state *mrb_src) {
  static const mrb_state mrb_state_zero = {0};
  static const struct mrb_context mrb_context_zero = {0};
  mrb_state *mrb;
  mrb_allocf f = mrb_src->allocf;
  void *ud = mrb_src->allocf_ud;

  mrb = (mrb_state *)(f)(NULL, NULL, sizeof(mrb_state), ud);
  if (mrb == NULL) return NULL;

  *mrb = mrb_state_zero;
  mrb->allocf_ud = ud;
  mrb->allocf = f;
  mrb->atexit_stack_len = 0;

  mrb_gc_init(mrb, &mrb->gc);
  mrb->c = (struct mrb_context *)mrb_malloc(mrb, sizeof(struct mrb_context));
  *mrb->c = mrb_context_zero;
  mrb->root_c = mrb->c;

  /* As mrb_init_core do but copy symbols before library initialization */
  mrb_init_symtbl(mrb); DONE;

  migrate_all_symbols(mrb_src, mrb); DONE;

  mrb_init_class(mrb); DONE;
  mrb_init_object(mrb); DONE;
  mrb_init_kernel(mrb); DONE;
  mrb_init_comparable(mrb); DONE;
  mrb_init_enumerable(mrb); DONE;

  mrb_init_symbol(mrb); DONE;
  mrb_init_exception(mrb); DONE;
  mrb_init_proc(mrb); DONE;
  mrb_init_string(mrb); DONE;
  mrb_init_array(mrb); DONE;
  mrb_init_hash(mrb); DONE;
  mrb_init_numeric(mrb); DONE;
  mrb_init_range(mrb); DONE;
  mrb_init_gc(mrb); DONE;
  mrb_init_version(mrb); DONE;
  mrb_init_mrblib(mrb); DONE;

#ifndef DISABLE_GEMS
  mrb_init_mrbgems(mrb); DONE;
#endif

  return mrb;
}

static mrb_value
mrb_thread_init(mrb_state* mrb, mrb_value self) {
  mrb_value proc = mrb_nil_value();
  mrb_int argc;
  mrb_value* argv;
  mrb_get_args(mrb, "&*", &proc, &argv, &argc);
  if (!mrb_nil_p(proc) && MRB_PROC_CFUNC_P(mrb_proc_ptr(proc))) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "forking C defined block");
  }
  if (!mrb_nil_p(proc)) {
    int i, l;
    mrb_thread_context* context = (mrb_thread_context*) malloc(sizeof(mrb_thread_context));
    context->mrb_caller = mrb;
    mrb_state* mrb2 = mrb_symbol_safe_copy(mrb);
    if(!mrb2) {
      mrb_raise(mrb, E_RUNTIME_ERROR, "copying mrb_state failed");
    }
    context->mrb = mrb2;
    struct RProc *rproc = mrb_proc_ptr(proc);
    context->proc = migrate_rproc(mrb, rproc, mrb2);
    context->proc->target_class = context->mrb->object_class;
    context->argc = argc;
    context->argv = calloc(sizeof (mrb_value), context->argc);
    context->result = mrb_nil_value();
    context->alive = TRUE;
    for (i = 0; i < context->argc; i++) {
      context->argv[i] = migrate_simple_value(mrb, argv[i], context->mrb);
    }

    {
      mrb_value gv = mrb_funcall(mrb, self, "global_variables", 0, NULL);
      l = RARRAY_LEN(gv);
      for (i = 0; i < l; i++) {
        mrb_int len;
        int ai = mrb_gc_arena_save(mrb);
        mrb_value k = mrb_ary_entry(gv, i);
        mrb_value o = mrb_gv_get(mrb, mrb_symbol(k));
        if (is_safe_migratable_simple_value(mrb, o, context->mrb)) {
          const char *p = mrb_sym2name_len(mrb, mrb_symbol(k), &len);
          mrb_gv_set(context->mrb,
            mrb_intern_static(context->mrb, p, len),
            migrate_simple_value(mrb, o, context->mrb));
        }
        mrb_gc_arena_restore(mrb, ai);
      }
    }

    mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "context"), mrb_obj_value(
      Data_Wrap_Struct(mrb, mrb->object_class,
      &mrb_thread_context_type, (void*) context)));

    pthread_create(&context->thread, NULL, &mrb_thread_func, (void*) context);
  }
  return self;
}

static mrb_value
mrb_thread_join(mrb_state* mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "context"));
  mrb_thread_context* context = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_thread_context_type, context);
  pthread_join(context->thread, NULL);

  context->result = migrate_simple_value(mrb, context->result, mrb);
  mrb_close(context->mrb);
  context->mrb = NULL;
  return context->result;
}

static mrb_value
mrb_thread_kill(mrb_state* mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "context"));
  mrb_thread_context* context = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_thread_context_type, context);
  if (context->mrb == NULL) {
    return mrb_nil_value();
  }
  if(context->alive) pthread_kill(context->thread, SIGINT);
  mrb_close(context->mrb);
  context->mrb = NULL;
  return context->result;
}

static mrb_value
mrb_thread_alive(mrb_state* mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "context"));
  mrb_thread_context* context = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_thread_context_type, context);

  return context->alive ? mrb_true_value() : mrb_false_value();
}

static mrb_value
mrb_thread_sleep(mrb_state* mrb, mrb_value self) {
  mrb_int t;
  mrb_get_args(mrb, "i", &t);
#ifndef _WIN32
  sleep(t);
#else
  Sleep(t * 1000);
#endif
  return mrb_nil_value();
}

static mrb_value
mrb_mutex_init(mrb_state* mrb, mrb_value self) {
  mrb_mutex_context* context = (mrb_mutex_context*) malloc(sizeof(mrb_mutex_context));
  pthread_mutex_init(&context->mutex, NULL);
  context->locked = FALSE;
  DATA_PTR(self) = context;
  DATA_TYPE(self) = &mrb_mutex_context_type;
  return self;
}

static mrb_value
mrb_mutex_lock(mrb_state* mrb, mrb_value self) {
  mrb_mutex_context* context = DATA_PTR(self);
  if (pthread_mutex_lock(&context->mutex) != 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "cannot lock");
  }
  context->locked = TRUE;
  return mrb_nil_value();
}

static mrb_value
mrb_mutex_try_lock(mrb_state* mrb, mrb_value self) {
  mrb_mutex_context* context = DATA_PTR(self);
  if (pthread_mutex_trylock(&context->mutex) == 0) {
    context->locked = TRUE;
    return mrb_true_value();
  }
  return mrb_false_value();
}

static mrb_value
mrb_mutex_locked(mrb_state* mrb, mrb_value self) {
  mrb_mutex_context* context = DATA_PTR(self);
  return context->locked ? mrb_true_value() : mrb_false_value();
}

static mrb_value
mrb_mutex_unlock(mrb_state* mrb, mrb_value self) {
  mrb_mutex_context* context = DATA_PTR(self);
  if (pthread_mutex_unlock(&context->mutex) != 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "cannot unlock");
  }
  context->locked = FALSE;
  return mrb_nil_value();
}

static mrb_value
mrb_mutex_sleep(mrb_state* mrb, mrb_value self) {
  mrb_int t;
  mrb_get_args(mrb, "i", &t);
#ifndef _WIN32
  sleep(t);
#else
  Sleep(t * 1000);
#endif
  return mrb_mutex_unlock(mrb, self);
}

static mrb_value
mrb_mutex_synchronize(mrb_state* mrb, mrb_value self) {
  mrb_value ret = mrb_nil_value();
  mrb_value proc = mrb_nil_value();
  mrb_get_args(mrb, "&", &proc);
  if (!mrb_nil_p(proc)) {
    mrb_mutex_lock(mrb, self);
    ret = mrb_yield_argv(mrb, proc, 0, NULL);
    mrb_mutex_unlock(mrb, self);
  }
  return ret;
}

static mrb_value
mrb_queue_init(mrb_state* mrb, mrb_value self) {
  mrb_queue_context* context = (mrb_queue_context*) malloc(sizeof(mrb_queue_context));
  pthread_mutex_init(&context->mutex, NULL);
  pthread_mutex_init(&context->queue_lock, NULL);
  if (pthread_mutex_lock(&context->queue_lock) != 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "cannot lock");
  }
  context->mrb = mrb;
  context->queue = mrb_ary_new(mrb);
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "queue"), context->queue);
  DATA_PTR(self) = context;
  DATA_TYPE(self) = &mrb_queue_context_type;
  return self;
}

static mrb_value
mrb_queue_lock(mrb_state* mrb, mrb_value self) {
  mrb_queue_context* context = DATA_PTR(self);
  if (pthread_mutex_lock(&context->mutex) != 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "cannot lock");
  }
  return mrb_nil_value();
}


static mrb_value
mrb_queue_unlock(mrb_state* mrb, mrb_value self) {
  mrb_queue_context* context = DATA_PTR(self);
  if (pthread_mutex_unlock(&context->mutex) != 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "cannot unlock");
  }
  return mrb_nil_value();
}

static mrb_value
mrb_queue_clear(mrb_state* mrb, mrb_value self) {
  mrb_queue_context* context = DATA_PTR(self);
  mrb_queue_lock(mrb, self);
  mrb_ary_clear(mrb, context->queue);
  mrb_queue_unlock(mrb, self);
  return mrb_nil_value();
}

static mrb_value
mrb_queue_push(mrb_state* mrb, mrb_value self) {
  mrb_value arg;
  mrb_queue_context* context = DATA_PTR(self);
  mrb_queue_lock(mrb, self);
  mrb_get_args(mrb, "o", &arg);
  mrb_ary_push(context->mrb, context->queue, migrate_simple_value(mrb, arg, context->mrb));
  mrb_queue_unlock(mrb, self);
  if (pthread_mutex_unlock(&context->queue_lock) != 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "cannot unlock");
  }
  return mrb_nil_value();
}

static mrb_value
mrb_queue_pop(mrb_state* mrb, mrb_value self) {
  mrb_value ret;
  mrb_queue_context* context = DATA_PTR(self);
  int len;
  mrb_queue_lock(mrb, self);
  len = RARRAY_LEN(context->queue);
  mrb_queue_unlock(mrb, self);
  if (len == 0) {
    if (pthread_mutex_lock(&context->queue_lock) != 0) {
      mrb_raise(mrb, E_RUNTIME_ERROR, "cannot lock");
    }
  }
  mrb_queue_lock(mrb, self);
  ret = migrate_simple_value(context->mrb, mrb_ary_pop(context->mrb, context->queue), mrb);
  mrb_queue_unlock(mrb, self);
  return ret;
}

static mrb_value
mrb_queue_unshift(mrb_state* mrb, mrb_value self) {
  mrb_value arg;
  mrb_queue_context* context = DATA_PTR(self);
  mrb_queue_lock(mrb, self);
  mrb_get_args(mrb, "o", &arg);
  mrb_ary_unshift(context->mrb, context->queue, migrate_simple_value(mrb, arg, context->mrb));
  mrb_queue_unlock(mrb, self);
  if (pthread_mutex_unlock(&context->queue_lock) != 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "cannot unlock");
  }
  return mrb_nil_value();
}

static mrb_value
mrb_queue_shift(mrb_state* mrb, mrb_value self) {
  mrb_value ret;
  mrb_queue_context* context = DATA_PTR(self);
  int len;
  mrb_queue_lock(mrb, self);
  len = RARRAY_LEN(context->queue);
  mrb_queue_unlock(mrb, self);
  if (len == 0) {
    if (pthread_mutex_lock(&context->queue_lock) != 0) {
      mrb_raise(mrb, E_RUNTIME_ERROR, "cannot lock");
    }
  }
  mrb_queue_lock(mrb, self);
  ret = migrate_simple_value(context->mrb, mrb_ary_shift(context->mrb, context->queue), mrb);
  mrb_queue_unlock(mrb, self);
  return ret;
}

static mrb_value
mrb_queue_num_waiting(mrb_state* mrb, mrb_value self) {
  /* TODO: multiple waiting */
  return mrb_fixnum_value(0);
}

static mrb_value
mrb_queue_empty_p(mrb_state* mrb, mrb_value self) {
  mrb_bool ret;
  mrb_queue_context* context = DATA_PTR(self);
  mrb_queue_lock(mrb, self);
  ret = RARRAY_LEN(context->queue) == 0;
  mrb_queue_unlock(mrb, self);
  return mrb_bool_value(ret);
}

static mrb_value
mrb_queue_size(mrb_state* mrb, mrb_value self) {
  mrb_int ret;
  mrb_queue_context* context = DATA_PTR(self);
  mrb_queue_lock(mrb, self);
  ret = RARRAY_LEN(context->queue);
  mrb_queue_unlock(mrb, self);
  return mrb_fixnum_value(ret);
}

void
mrb_mruby_thread_gem_init(mrb_state* mrb) {
  struct RClass *_class_thread, *_class_mutex, *_class_queue;

  _class_thread = mrb_define_class(mrb, "Thread", mrb->object_class);
#ifdef MRB_THREAD_COPY_VALUES
  mrb_define_const(mrb, _class_thread, "COPY_VALUES", mrb_true_value());
#else
  mrb_define_const(mrb, _class_thread, "COPY_VALUES", mrb_false_value());
#endif
  MRB_SET_INSTANCE_TT(_class_thread, MRB_TT_DATA);
  mrb_define_method(mrb, _class_thread, "initialize", mrb_thread_init, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, _class_thread, "join", mrb_thread_join, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_thread, "kill", mrb_thread_kill, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_thread, "terminate", mrb_thread_kill, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_thread, "alive?", mrb_thread_alive, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, _class_thread, "sleep", mrb_thread_sleep, MRB_ARGS_REQ(1));
  mrb_define_module_function(mrb, _class_thread, "start", mrb_thread_init, MRB_ARGS_REQ(1));

  _class_mutex = mrb_define_class(mrb, "Mutex", mrb->object_class);
  MRB_SET_INSTANCE_TT(_class_mutex, MRB_TT_DATA);
  mrb_define_method(mrb, _class_mutex, "initialize", mrb_mutex_init, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "lock", mrb_mutex_lock, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "try_lock", mrb_mutex_try_lock, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "locked?", mrb_mutex_locked, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "sleep", mrb_mutex_sleep, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, _class_mutex, "synchronize", mrb_mutex_synchronize, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, _class_mutex, "unlock", mrb_mutex_unlock, MRB_ARGS_NONE());

  _class_queue = mrb_define_class(mrb, "Queue", mrb->object_class);
  MRB_SET_INSTANCE_TT(_class_queue, MRB_TT_DATA);
  mrb_define_method(mrb, _class_queue, "initialize", mrb_queue_init, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_queue, "clear", mrb_queue_clear, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_queue, "push", mrb_queue_push, MRB_ARGS_NONE());
  mrb_define_alias(mrb, _class_queue, "<<", "push");
  mrb_define_method(mrb, _class_queue, "unshift", mrb_queue_unshift, MRB_ARGS_NONE());
  mrb_define_alias(mrb, _class_queue, "enq", "unshift");
  mrb_define_method(mrb, _class_queue, "pop", mrb_queue_pop, MRB_ARGS_OPT(1));
  mrb_define_alias(mrb, _class_queue, "deq", "pop");
  mrb_define_method(mrb, _class_queue, "shift", mrb_queue_shift, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, _class_queue, "size", mrb_queue_size, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_queue, "num_waiting", mrb_queue_num_waiting, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_queue, "empty?", mrb_queue_empty_p, MRB_ARGS_NONE());
}

void
mrb_mruby_thread_gem_final(mrb_state* mrb) {
}

/* vim:set et ts=2 sts=2 sw=2 tw=0: */
