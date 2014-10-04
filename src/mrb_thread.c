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
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  int argc;
  mrb_value* argv;
  struct RProc* proc;
  pthread_t thread;
  mrb_state* mrb_caller;
  mrb_state* mrb;
  mrb_value result;
} mrb_thread_context;

static void
mrb_thread_context_free(mrb_state *mrb, void *p) {
  if (p) {
    mrb_thread_context* context = (mrb_thread_context*) p;
    if (context->mrb) mrb_close(context->mrb);
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
  mrb_value queue;
} mrb_queue_context;

static void
mrb_queue_context_free(mrb_state *mrb, void *p) {
  if (p) {
    mrb_queue_context* context = (mrb_queue_context*) p;
    pthread_mutex_destroy(&context->mutex);
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

// based on https://gist.github.com/3066997
static mrb_value
migrate_simple_value(mrb_state *mrb, mrb_value v, mrb_state *mrb2) {
  mrb_value nv;

  switch (mrb_type(v)) {
  case MRB_TT_OBJECT:
  case MRB_TT_EXCEPTION:
    {
      struct RObject *o = mrb_obj_ptr(v);
      mrb_value path = mrb_class_path(mrb, o->c);
      struct RClass *c;

      if (mrb_nil_p(path)) {
        mrb_raise(mrb, E_TYPE_ERROR, "cannot migrate class");
      }
      c = mrb_class_get(mrb2, RSTRING_PTR(path));
      nv = mrb_obj_value(mrb_obj_alloc(mrb2, mrb_type(v), c));
    }
    migrate_simple_iv(mrb, v, mrb2, nv);
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
      struct RRange *r = mrb_range_ptr(v);
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
  case MRB_TT_DATA:
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
  return NULL;
}

static mrb_value
mrb_thread_init(mrb_state* mrb, mrb_value self) {
  mrb_value proc = mrb_nil_value();
  mrb_int argc;
  mrb_value* argv;
  mrb_get_args(mrb, "&*", &proc, &argv, &argc);
  if (MRB_PROC_CFUNC_P(mrb_proc_ptr(proc))) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "forking C defined block");
  }
  if (!mrb_nil_p(proc)) {
    int i, l;
    mrb_thread_context* context = (mrb_thread_context*) malloc(sizeof(mrb_thread_context));
    context->mrb_caller = mrb;
    context->mrb = mrb_open();
    context->proc = mrb_proc_new(mrb, mrb_proc_ptr(proc)->body.irep);
    context->proc->target_class = context->mrb->object_class;
    context->argc = argc;
    context->argv = calloc(sizeof (mrb_value), context->argc);
    context->result = mrb_nil_value();
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
        const char *p = mrb_sym2name_len(mrb, mrb_symbol(k), &len);
        mrb_gv_set(context->mrb,
          mrb_intern_static(context->mrb, p, len),
          migrate_simple_value(mrb, o, context->mrb));
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
  pthread_kill(context->thread, 0);

  mrb_close(context->mrb);
  context->mrb = NULL;
  return context->result;
}

static mrb_value
mrb_thread_alive(mrb_state* mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "context"));
  mrb_thread_context* context = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_thread_context_type, context);

  return context->mrb != NULL ? mrb_true_value() : mrb_false_value();
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
  mrb_value proc = mrb_nil_value();
  mrb_get_args(mrb, "&", &proc);
  if (!mrb_nil_p(proc)) {
    mrb_mutex_lock(mrb, self);
    mrb_yield_argv(mrb, proc, 0, NULL);
    mrb_mutex_unlock(mrb, self);
  }
  return mrb_nil_value();
}

static mrb_value
mrb_queue_init(mrb_state* mrb, mrb_value self) {
  mrb_queue_context* context = (mrb_queue_context*) malloc(sizeof(mrb_queue_context));
  pthread_mutex_init(&context->mutex, NULL);
  context->queue = mrb_ary_new(mrb);
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
  mrb_get_args(mrb, "o", &arg);
  mrb_queue_lock(mrb, self);
  mrb_ary_push(mrb, context->queue, arg);
  mrb_queue_unlock(mrb, self);
  return mrb_nil_value();
}

static mrb_value
mrb_queue_pop(mrb_state* mrb, mrb_value self) {
  mrb_value ret;
  mrb_queue_context* context = DATA_PTR(self);
  mrb_queue_lock(mrb, self);
  ret = mrb_ary_pop(mrb, context->queue);
  mrb_queue_unlock(mrb, self);
  return ret;
}

static mrb_value
mrb_queue_num_waiting(mrb_state* mrb, mrb_value self) {
  /* TODO */
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
  MRB_SET_INSTANCE_TT(_class_thread, MRB_TT_DATA);
  mrb_define_method(mrb, _class_thread, "initialize", mrb_thread_init, ARGS_OPT(1));
  mrb_define_method(mrb, _class_thread, "join", mrb_thread_join, ARGS_NONE());
  mrb_define_method(mrb, _class_thread, "kill", mrb_thread_kill, ARGS_NONE());
  mrb_define_method(mrb, _class_thread, "terminate", mrb_thread_kill, ARGS_NONE());
  mrb_define_method(mrb, _class_thread, "alive?", mrb_thread_alive, ARGS_NONE());
  mrb_define_module_function(mrb, _class_thread, "sleep", mrb_thread_sleep, ARGS_REQ(1));

  _class_mutex = mrb_define_class(mrb, "Mutex", mrb->object_class);
  MRB_SET_INSTANCE_TT(_class_mutex, MRB_TT_DATA);
  mrb_define_method(mrb, _class_mutex, "initialize", mrb_mutex_init, ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "lock", mrb_mutex_lock, ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "try_lock", mrb_mutex_try_lock, ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "locked?", mrb_mutex_locked, ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "sleep", mrb_mutex_sleep, ARGS_REQ(1));
  mrb_define_method(mrb, _class_mutex, "synchronize", mrb_mutex_synchronize, ARGS_REQ(1));
  mrb_define_method(mrb, _class_mutex, "unlock", mrb_mutex_unlock, ARGS_NONE());

  _class_queue = mrb_define_class(mrb, "Queue", mrb->object_class);
  MRB_SET_INSTANCE_TT(_class_queue, MRB_TT_DATA);
  mrb_define_method(mrb, _class_queue, "initialize", mrb_queue_init, ARGS_NONE());
  mrb_define_method(mrb, _class_queue, "clear", mrb_queue_clear, ARGS_NONE());
  mrb_define_method(mrb, _class_queue, "push", mrb_queue_push, ARGS_NONE());
  mrb_define_alias(mrb, _class_queue, "<<", "push");
  mrb_define_alias(mrb, _class_queue, "enq", "push");
  mrb_define_method(mrb, _class_queue, "pop", mrb_queue_pop, ARGS_OPT(1));
  mrb_define_alias(mrb, _class_queue, "deq", "pop");
  mrb_define_alias(mrb, _class_queue, "shift", "pop");
  mrb_define_method(mrb, _class_queue, "size", mrb_queue_size, ARGS_NONE());
  mrb_define_method(mrb, _class_queue, "num_waiting", mrb_queue_num_waiting, ARGS_NONE());
  mrb_define_method(mrb, _class_queue, "empty?", mrb_queue_empty_p, ARGS_NONE());
}

void
mrb_mruby_thread_gem_final(mrb_state* mrb) {
}

/* vim:set et ts=2 sts=2 sw=2 tw=0: */
