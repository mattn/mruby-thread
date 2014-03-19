#include <mruby.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/proc.h>
#include <mruby/data.h>
#include <mruby/variable.h>
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#include <ctype.h>
#include <pthread.h>
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

// based on https://gist.github.com/3066997
static mrb_value
migrate_simple_value(mrb_state *mrb, mrb_value v, mrb_state *mrb2) {
  mrb_value nv = mrb_nil_value();

  nv.tt = v.tt;
  switch (mrb_type(v)) {
  case MRB_TT_OBJECT:
    nv.value.p = v.value.p;
    break;
  case MRB_TT_FALSE:
  case MRB_TT_TRUE:
  case MRB_TT_FIXNUM:
    nv.value.i = v.value.i;
    break;
  case MRB_TT_SYMBOL:
    nv = mrb_symbol_value(mrb_intern_str(mrb2, v));
    break;
  case MRB_TT_FLOAT:
    nv.value.f = v.value.f;
    break;
  case MRB_TT_STRING:
    nv = mrb_str_new(mrb2, RSTRING_PTR(v), RSTRING_LEN(v));
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
  context->result = mrb_yield_argv(mrb, mrb_obj_value(context->proc), context->argc, context->argv);
  return NULL;
}

static mrb_value
mrb_thread_init(mrb_state* mrb, mrb_value self) {
  mrb_value proc = mrb_nil_value();
  int argc;
  mrb_value* argv;
  mrb_get_args(mrb, "&*", &proc, &argv, &argc);
  if (!mrb_nil_p(proc)) {
    int i;
    mrb_thread_context* context = (mrb_thread_context*) malloc(sizeof(mrb_thread_context));
    context->mrb_caller = mrb;
    context->mrb = mrb_open();
    context->proc = mrb_proc_ptr(proc);
    context->argc = argc;
    context->argv = argv;
    context->argv = calloc(sizeof (mrb_value), context->argc);
    context->result = mrb_nil_value();
    for (i = 0; i < context->argc; i++) {
      context->argv[i] = migrate_simple_value(mrb, argv[i], context->mrb);
    }

    mrb_iv_set(mrb, self, mrb_intern_cstr(mrb, "context"), mrb_obj_value(
      Data_Wrap_Struct(mrb, mrb->object_class,
      &mrb_thread_context_type, (void*) context)));

    pthread_create(&context->thread, NULL, &mrb_thread_func, (void*) context);
  }
  return self;
}

static mrb_value
mrb_thread_join(mrb_state* mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern_cstr(mrb, "context"));
  mrb_thread_context* context = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_thread_context_type, context);
  pthread_join(context->thread, NULL);

  context->result = migrate_simple_value(mrb, context->result, mrb);
  mrb_close(context->mrb);
  context->mrb = NULL;
  return context->result;
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
    mrb_raise(mrb, E_RUNTIME_ERROR, "cannot lock");
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

void
mrb_mruby_thread_gem_init(mrb_state* mrb) {
  struct RClass *_class_thread, *_class_mutex;
  _class_thread = mrb_define_class(mrb, "Thread", mrb->object_class);
  mrb_define_method(mrb, _class_thread, "initialize", mrb_thread_init, ARGS_OPT(1));
  mrb_define_method(mrb, _class_thread, "join", mrb_thread_join, ARGS_NONE());
  mrb_define_module_function(mrb, _class_thread, "sleep", mrb_thread_sleep, ARGS_REQ(1));
  _class_mutex = mrb_define_class(mrb, "Mutex", mrb->object_class);
  mrb_define_method(mrb, _class_mutex, "initialize", mrb_mutex_init, ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "lock", mrb_mutex_lock, ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "try_lock", mrb_mutex_try_lock, ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "locked?", mrb_mutex_locked, ARGS_NONE());
  mrb_define_method(mrb, _class_mutex, "sleep", mrb_mutex_sleep, ARGS_REQ(1));
  mrb_define_method(mrb, _class_mutex, "synchronize", mrb_mutex_synchronize, ARGS_REQ(1));
  mrb_define_method(mrb, _class_mutex, "unlock", mrb_mutex_unlock, ARGS_NONE());
}

void
mrb_mruby_thread_gem_final(mrb_state* mrb) {
}

/* vim:set et ts=2 sts=2 sw=2 tw=0: */
