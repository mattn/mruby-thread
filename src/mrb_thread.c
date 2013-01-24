#include <mruby.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/proc.h>
#include <mruby/data.h>
#include <mruby/variable.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>

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
  mrb_thread_context* context = (mrb_thread_context*) p;
  if (context->mrb) mrb_close(context->mrb);
  free(p);
}

static const struct mrb_data_type mrb_thread_context_type = {
  "mrb_thread_context", mrb_thread_context_free,
};

// based on https://gist.github.com/3066997
static mrb_value
migrate_simple_value(mrb_state *mrb, mrb_value v, mrb_state *mrb2) {
  mrb_value nv;
  const char *s;
  int len;

  nv.tt = v.tt;
  switch (mrb_type(v)) {
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
    {
      struct RString *str = mrb_str_ptr(v);

      s = str->ptr;
      len = str->len;
      nv = mrb_str_new(mrb2, s, len);
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
  struct RProc* np = mrb_proc_new(mrb, context->proc->body.irep);
  context->result = mrb_yield_argv(mrb, mrb_obj_value(np), context->argc, context->argv);
  return NULL;
}

static mrb_value
mrb_thread_init(mrb_state* mrb, mrb_value self) {
  mrb_value proc = mrb_nil_value();
  int argc;
  mrb_value* argv;
  mrb_get_args(mrb, "&*", &proc, &argv, &argc);
  if (!mrb_nil_p(proc)) {
    mrb_thread_context* context = (mrb_thread_context*) malloc(sizeof(mrb_thread_context));
    context->proc = mrb_proc_ptr(proc);
    context->argc = argc;
    context->argv = argv;
    context->mrb_caller = mrb;
    context->mrb = mrb_open();
    context->result = mrb_nil_value();
    int i;
    for (i = 0; i < context->argc; i++) {
      context->argv[i] = migrate_simple_value(mrb, context->argv[i], context->mrb);
    }

    mrb_iv_set(mrb, self, mrb_intern(mrb, "context"), mrb_obj_value(
      Data_Wrap_Struct(mrb, mrb->object_class,
      &mrb_thread_context_type, (void*) context)));

    pthread_create(&context->thread, NULL, &mrb_thread_func, (void*) context);
  }
  return self;
}

static mrb_value
mrb_thread_join(mrb_state* mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  mrb_thread_context* context = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_thread_context_type, context);
  pthread_join(context->thread, NULL);

  context->result = migrate_simple_value(mrb, context->result, mrb);
  mrb_close(context->mrb);
  context->mrb = NULL;
  return context->result;
}

void
mrb_mruby_thread_gem_init(mrb_state* mrb) {
  struct RClass* _class_thread = mrb_define_class(mrb, "Thread", mrb->object_class);
  mrb_define_method(mrb, _class_thread, "initialize", mrb_thread_init, ARGS_OPT(1));
  mrb_define_method(mrb, _class_thread, "join", mrb_thread_join, ARGS_NONE());
}

void
mrb_mruby_thread_gem_final(mrb_state* mrb) {
}

/* vim:set et ts=2 sts=2 sw=2 tw=0: */
