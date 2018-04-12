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
#include <sys/time.h>
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

#ifdef MRB_PROC_ENV
# define _MRB_PROC_ENV(p) (p)->e.env
#else
# define _MRB_PROC_ENV(p) (p)->env
#endif

#ifndef MRB_PROC_SET_TARGET_CLASS
# define MRB_PROC_SET_TARGET_CLASS(p,tc) \
  p->target_class = tc
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
check_pthread_error(mrb_state *mrb, int res) {
  if (res == 0) { return; }
  mrb_raise(mrb, mrb_class_get(mrb, "ThreadError"), strerror(res));
}

static void
mrb_thread_context_free(mrb_state *mrb, void *p) {
  if (p) {
    mrb_thread_context* context = (mrb_thread_context*) p;
    if (context->alive) {
      pthread_cancel(context->thread);
    }
    if (context->mrb && context->mrb != mrb) mrb_close(context->mrb);
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
  pthread_mutex_t mutex, queue_lock;
  pthread_cond_t cond;
  mrb_state* mrb;
  mrb_value queue;
} mrb_queue_context;

static void
mrb_queue_context_free(mrb_state *mrb, void *p) {
  if (p) {
    mrb_queue_context* context = (mrb_queue_context*) p;
    pthread_cond_destroy(&context->cond);
    pthread_mutex_destroy(&context->mutex);
    pthread_mutex_destroy(&context->queue_lock);
    free(p);
  }
}

static const struct mrb_data_type mrb_queue_context_type = {
  "mrb_queue_context", mrb_queue_context_free,
};

mrb_value mrb_thread_migrate_value(mrb_state *mrb, mrb_value v, mrb_state *mrb2);

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
  mrb_value iv;
  mrb_int i;

  for (i=0; i<RARRAY_LEN(ivars); i++) {
    mrb_sym sym = mrb_symbol(RARRAY_PTR(ivars)[i]);
    mrb_sym sym2 = migrate_sym(mrb, sym, mrb2);
    iv = mrb_iv_get(mrb, v, sym);
    mrb_iv_set(mrb2, v2, sym2, mrb_thread_migrate_value(mrb, iv, mrb2));
  }
}

static mrb_bool
is_safe_migratable_datatype(const mrb_data_type *type)
{
  static const char *known_type_names[] = {
    "mrb_thread_context",
    "mrb_mutex_context",
    "mrb_queue_context",
    "IO",
    "Time",
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
      int i;
      for (i=0; i<RARRAY_LEN(v); i++) {
        if (!is_safe_migratable_simple_value(mrb, RARRAY_PTR(v)[i], mrb2)) {
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

static void
migrate_irep_child(mrb_state *mrb, mrb_irep *ret, mrb_state *mrb2)
{
  int i;
  mrb_code *old_iseq;

  // migrate pool
  for (i = 0; i < ret->plen; ++i) {
    mrb_value v = ret->pool[i];
    if (mrb_type(v) == MRB_TT_STRING) {
      struct RString *s = mrb_str_ptr(v);
      if (RSTR_NOFREE_P(s) && RSTRING_LEN(v) > 0) {
        char *old = RSTRING_PTR(v);
        s->as.heap.ptr = (char*)mrb_malloc(mrb2, RSTRING_LEN(v));
        memcpy(s->as.heap.ptr, old, RSTRING_LEN(v));
        RSTR_UNSET_NOFREE_FLAG(s);
      }
    }
  }

  // migrate iseq
  if (ret->flags & MRB_ISEQ_NO_FREE) {
    old_iseq = ret->iseq;
    ret->iseq = (mrb_code*)mrb_malloc(mrb2, sizeof(mrb_code) * ret->ilen);
    memcpy(ret->iseq, old_iseq, sizeof(mrb_code) * ret->ilen);
    ret->flags &= ~MRB_ISEQ_NO_FREE;
  }

  // migrate sub ireps
  for (i = 0; i < ret->rlen; ++i) {
    migrate_irep_child(mrb, ret->reps[i], mrb2);
  }
}

static mrb_irep*
migrate_irep(mrb_state *mrb, mrb_irep *src, mrb_state *mrb2) {
  uint8_t *irep = NULL;
  size_t binsize = 0;
  mrb_irep *ret;
  mrb_dump_irep(mrb, src, DUMP_ENDIAN_NAT, &irep, &binsize);

  ret = mrb_read_irep(mrb2, irep);
  migrate_irep_child(mrb, ret, mrb2);
  mrb_free(mrb, irep);
  return ret;
}

struct RProc*
migrate_rproc(mrb_state *mrb, struct RProc *rproc, mrb_state *mrb2) {
  struct RProc *newproc = mrb_proc_new(mrb2, migrate_irep(mrb, rproc->body.irep, mrb2));
  mrb_irep_decref(mrb2, newproc->body.irep);

#ifdef MRB_PROC_ENV_P
  if (_MRB_PROC_ENV(rproc) && MRB_PROC_ENV_P(rproc)) {
#else
  if (_MRB_PROC_ENV(rproc)) {
#endif
    mrb_int i, len = MRB_ENV_STACK_LEN(_MRB_PROC_ENV(rproc));
    struct REnv *newenv = (struct REnv*)mrb_obj_alloc(mrb2, MRB_TT_ENV, mrb2->object_class);

    newenv->stack = mrb_malloc(mrb, sizeof(mrb_value) * len);
    MRB_ENV_UNSHARE_STACK(newenv);
    for (i = 0; i < len; ++i) {
      mrb_value v = _MRB_PROC_ENV(rproc)->stack[i];
      if (mrb_obj_ptr(v) == ((struct RObject*)rproc)) {
        newenv->stack[i] = mrb_obj_value(newproc);
      } else {
        newenv->stack[i] = mrb_thread_migrate_value(mrb, v, mrb2);
      }
    }
    _MRB_PROC_ENV(newproc) = newenv;
#ifdef MRB_PROC_ENVSET
    newproc->flags |= MRB_PROC_ENVSET;
    if (rproc->upper) {
      newproc->upper = migrate_rproc(mrb, rproc->upper, mrb2);
    }
#endif
  }

  return newproc;
}

static struct RClass*
path2class(mrb_state *M, char const* path_begin, mrb_int len) {
  char const* begin = path_begin;
  char const* p = begin;
  char const* end = begin + len;
  struct RClass* ret = M->object_class;

  while(1) {
    mrb_sym cls;
    mrb_value cnst;

    while((p < end && p[0] != ':') ||
          ((p + 1) < end && p[1] != ':')) ++p;

    cls = mrb_intern(M, begin, p - begin);
    if (!mrb_mod_cv_defined(M, ret, cls)) {
      mrb_raisef(M, mrb_class_get(M, "ArgumentError"), "undefined class/module %S",
                 mrb_str_new(M, path_begin, p - path_begin));
    }

    cnst = mrb_mod_cv_get(M, ret, cls);
    if (mrb_type(cnst) != MRB_TT_CLASS &&  mrb_type(cnst) != MRB_TT_MODULE) {
      mrb_raisef(M, mrb_class_get(M, "TypeError"), "%S does not refer to class/module",
                 mrb_str_new(M, path_begin, p - path_begin));
    }
    ret = mrb_class_ptr(cnst);

    if(p >= end) { break; }

    p += 2;
    begin = p;
  }
  return ret;
}

enum mrb_timezone { TZ_NONE = 0 };

struct mrb_time {
  time_t              sec;
  time_t              usec;
  enum mrb_timezone   timezone;
  struct tm           datetime;
};

// based on https://gist.github.com/3066997
mrb_value
mrb_thread_migrate_value(mrb_state *mrb, mrb_value const v, mrb_state *mrb2) {
  if (mrb == mrb2) { return v; }

  switch (mrb_type(v)) {
  case MRB_TT_CLASS:
  case MRB_TT_MODULE: {
    mrb_value cls_path = mrb_class_path(mrb, mrb_class_ptr(v));
    struct RClass *c;
    if (mrb_nil_p(cls_path)) {
      return mrb_nil_value();
    }
    c = path2class(mrb2, RSTRING_PTR(cls_path), RSTRING_LEN(cls_path));
    return mrb_obj_value(c);
  }

  case MRB_TT_OBJECT:
  case MRB_TT_EXCEPTION:
    {
      mrb_value cls_path = mrb_class_path(mrb, mrb_class(mrb, v)), nv;
      struct RClass *c;
      if (mrb_nil_p(cls_path)) {
        return mrb_nil_value();
      }
      c = path2class(mrb2, RSTRING_PTR(cls_path), RSTRING_LEN(cls_path));
      nv = mrb_obj_value(mrb_obj_alloc(mrb2, mrb_type(v), c));
      migrate_simple_iv(mrb, v, mrb2, nv);
      if (mrb_type(v) == MRB_TT_EXCEPTION) {
        mrb_iv_set(mrb2, nv, mrb_intern_lit(mrb2, "mesg"),
                   mrb_thread_migrate_value(mrb, mrb_iv_get(mrb, v, mrb_intern_lit(mrb, "mesg")), mrb2));
      }
      return nv;
    }
    break;
  case MRB_TT_PROC:
    return mrb_obj_value(migrate_rproc(mrb, mrb_proc_ptr(v), mrb2));
  case MRB_TT_FALSE:
  case MRB_TT_TRUE:
  case MRB_TT_FIXNUM:
    return v;
  case MRB_TT_SYMBOL:
    return mrb_symbol_value(migrate_sym(mrb, mrb_symbol(v), mrb2));
  case MRB_TT_FLOAT:
    return mrb_float_value(mrb2, mrb_float(v));
  case MRB_TT_STRING:
    return mrb_str_new(mrb2, RSTRING_PTR(v), RSTRING_LEN(v));

  case MRB_TT_RANGE: {
    struct RRange *r = MRB_RANGE_PTR(v);
    return mrb_range_new(mrb2,
                         mrb_thread_migrate_value(mrb, r->edges->beg, mrb2),
                         mrb_thread_migrate_value(mrb, r->edges->end, mrb2),
                         r->excl);
  }

  case MRB_TT_ARRAY: {
    int i, ai;

    mrb_value nv = mrb_ary_new_capa(mrb2, RARRAY_LEN(v));
    ai = mrb_gc_arena_save(mrb2);
    for (i=0; i<RARRAY_LEN(v); i++) {
      mrb_ary_push(mrb2, nv, mrb_thread_migrate_value(mrb, RARRAY_PTR(v)[i], mrb2));
      mrb_gc_arena_restore(mrb2, ai);
    }
    return nv;
  }

  case MRB_TT_HASH: {
    mrb_value ka;
    int i, l;

    mrb_value nv = mrb_hash_new(mrb2);
    ka = mrb_hash_keys(mrb, v);
    l = RARRAY_LEN(ka);
    for (i = 0; i < l; i++) {
      int ai = mrb_gc_arena_save(mrb2);
      mrb_value k = mrb_thread_migrate_value(mrb, mrb_ary_entry(ka, i), mrb2);
      mrb_value o = mrb_thread_migrate_value(mrb, mrb_hash_get(mrb, v, k), mrb2);
      mrb_hash_set(mrb2, nv, k, o);
      mrb_gc_arena_restore(mrb2, ai);
    }
    migrate_simple_iv(mrb, v, mrb2, nv);
    return nv;
  }

  case MRB_TT_DATA: {
    mrb_value cls_path = mrb_class_path(mrb, mrb_class(mrb, v)), nv;
    struct RClass *c = path2class(mrb2, RSTRING_PTR(cls_path), RSTRING_LEN(cls_path));
    if (!is_safe_migratable_datatype(DATA_TYPE(v)))
      mrb_raisef(mrb, E_TYPE_ERROR, "cannot migrate object: %S(%S)",
                 mrb_str_new_cstr(mrb, DATA_TYPE(v)->struct_name), mrb_inspect(mrb, v));
    nv = mrb_obj_value(mrb_obj_alloc(mrb2, mrb_type(v), c));
    if (strcmp(DATA_TYPE(v)->struct_name, "Time") == 0) {
      DATA_PTR(nv) = mrb_malloc(mrb, sizeof(struct mrb_time));
      *((struct mrb_time*)DATA_PTR(nv)) = *((struct mrb_time*)DATA_PTR(v));
      DATA_TYPE(nv) = DATA_TYPE(v);
      return nv;
    } else {
      DATA_PTR(nv) = DATA_PTR(v);
      // Don't copy type information to avoid freeing in sub-thread.
      // DATA_TYPE(nv) = DATA_TYPE(v);
      migrate_simple_iv(mrb, v, mrb2, nv);
      return nv;
    }
  }

    // case MRB_TT_FREE: return mrb_nil_value();

  default: break;
  }

  // mrb_raisef(mrb, E_TYPE_ERROR, "cannot migrate object: %S", mrb_fixnum_value(mrb_type(v)));
  mrb_raisef(mrb, E_TYPE_ERROR, "cannot migrate object: %S(%S)", mrb_inspect(mrb, v), mrb_fixnum_value(mrb_type(v)));
  return mrb_nil_value();
}

static void*
mrb_thread_func(void* data) {
  mrb_thread_context* context = (mrb_thread_context*) data;
  mrb_state* mrb = context->mrb;
  context->result = mrb_yield_with_class(mrb, mrb_obj_value(context->proc),
                                         context->argc, context->argv, mrb_nil_value(), mrb->object_class);
  mrb_gc_protect(mrb, context->result);
  context->alive = FALSE;
  return NULL;
}

static mrb_value
mrb_thread_init(mrb_state* mrb, mrb_value self) {
  static mrb_thread_context const ctx_zero = {0};

  mrb_value proc = mrb_nil_value();
  mrb_int argc;
  mrb_value* argv;

  int i, l;
  mrb_thread_context* context = (mrb_thread_context*) malloc(sizeof(mrb_thread_context));
  mrb_state* mrb2;
  struct RProc *rproc;

  *context = ctx_zero;
  mrb_data_init(self, context, &mrb_thread_context_type);

  mrb_get_args(mrb, "&*", &proc, &argv, &argc);
  if (!mrb_nil_p(proc) && MRB_PROC_CFUNC_P(mrb_proc_ptr(proc))) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "forking C defined block");
  }
  if (mrb_nil_p(proc)) { return self; }

  context->mrb_caller = mrb;
  mrb2 = mrb_open();
  migrate_all_symbols(mrb, mrb2);
  if(!mrb2) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "copying mrb_state failed");
  }
  context->mrb = mrb2;
  rproc = mrb_proc_ptr(proc);
  context->proc = migrate_rproc(mrb, rproc, mrb2);
  MRB_PROC_SET_TARGET_CLASS(context->proc, context->mrb->object_class);
  context->argc = argc;
  context->argv = calloc(sizeof (mrb_value), context->argc);
  context->result = mrb_nil_value();
  context->alive = TRUE;
  for (i = 0; i < context->argc; i++) {
    context->argv[i] = mrb_thread_migrate_value(mrb, argv[i], context->mrb);
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
                   mrb_thread_migrate_value(mrb, o, context->mrb));
      }
      mrb_gc_arena_restore(mrb, ai);
    }
  }

  check_pthread_error(mrb, pthread_create(&context->thread, NULL, &mrb_thread_func, (void*) context));

  return self;
}

static mrb_value
mrb_thread_join(mrb_state* mrb, mrb_value self) {
  mrb_thread_context* context = (mrb_thread_context*)DATA_PTR(self);
  check_pthread_error(mrb, pthread_join(context->thread, NULL));

  context->result = mrb_thread_migrate_value(context->mrb, context->result, mrb);
  mrb_close(context->mrb);
  context->mrb = NULL;
  return context->result;
}

static mrb_value
mrb_thread_kill(mrb_state* mrb, mrb_value self) {
  mrb_thread_context* context = (mrb_thread_context*)DATA_PTR(self);
  if (context->mrb == NULL) {
    return mrb_nil_value();
  }
  if(context->alive) {
    check_pthread_error(mrb, pthread_kill(context->thread, SIGINT));
    context->result = mrb_thread_migrate_value(context->mrb, context->result, mrb);
    return context->result;
  }
  return mrb_nil_value();
}

static mrb_value
mrb_thread_alive(mrb_state* mrb, mrb_value self) {
  mrb_thread_context* context = (mrb_thread_context*)DATA_PTR(self);
  return mrb_bool_value(context->alive);
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

#ifdef _MSC_VER
static
int usleep(useconds_t usec) {
  LARGE_INTEGER pf, s, c;
  if (!QueryPerformanceFrequency(&pf))
    return -1;
  if (!QueryPerformanceCounter(&s))
    return -1;
  do {
    if (QueryPerformanceCounter((LARGE_INTEGER*) &c))
      return -1;
  } while ((c.QuadPart - s.QuadPart) / (float)pf.QuadPart * 1000 * 1000 < t);
  return 0;
}
#endif

static mrb_value
mrb_thread_usleep(mrb_state* mrb, mrb_value self) {
  mrb_int t;
  mrb_get_args(mrb, "i", &t);
  usleep(t);
  return mrb_nil_value();
}

static mrb_value
mrb_mutex_init(mrb_state* mrb, mrb_value self) {
  mrb_mutex_context* context = (mrb_mutex_context*) malloc(sizeof(mrb_mutex_context));
  check_pthread_error(mrb, pthread_mutex_init(&context->mutex, NULL));
  context->locked = FALSE;
  DATA_PTR(self) = context;
  DATA_TYPE(self) = &mrb_mutex_context_type;
  return self;
}

static mrb_value
mrb_mutex_lock(mrb_state* mrb, mrb_value self) {
  mrb_mutex_context* context = DATA_PTR(self);
  check_pthread_error(mrb, pthread_mutex_lock(&context->mutex));
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
  check_pthread_error(mrb, pthread_mutex_unlock(&context->mutex));
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
  check_pthread_error(mrb, pthread_mutex_init(&context->mutex, NULL));
  check_pthread_error(mrb, pthread_cond_init(&context->cond, NULL));
  check_pthread_error(mrb, pthread_mutex_init(&context->queue_lock, NULL));
  context->mrb = mrb;
  context->queue = mrb_ary_new(mrb);
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "queue"), context->queue);
  mrb_data_init(self, context, &mrb_queue_context_type);

  check_pthread_error(mrb, pthread_mutex_lock(&context->queue_lock));

  return self;
}

static mrb_value
mrb_queue_lock(mrb_state* mrb, mrb_value self) {
  mrb_queue_context* context = DATA_PTR(self);
  check_pthread_error(mrb, pthread_mutex_lock(&context->mutex));
  return mrb_nil_value();
}


static mrb_value
mrb_queue_unlock(mrb_state* mrb, mrb_value self) {
  mrb_queue_context* context = DATA_PTR(self);
  check_pthread_error(mrb, pthread_mutex_unlock(&context->mutex));
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
  mrb_ary_push(context->mrb, context->queue, mrb_thread_migrate_value(mrb, arg, context->mrb));
  mrb_queue_unlock(mrb, self);

  check_pthread_error(mrb, pthread_cond_signal(&context->cond));
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
    check_pthread_error(mrb, pthread_cond_wait(&context->cond, &context->queue_lock));
  }

  mrb_queue_lock(mrb, self);
  ret = mrb_thread_migrate_value(context->mrb, mrb_ary_pop(context->mrb, context->queue), mrb);
  mrb_queue_unlock(mrb, self);

  return ret;
}

static mrb_value
mrb_queue_unshift(mrb_state* mrb, mrb_value self) {
  mrb_value arg;
  mrb_queue_context* context = DATA_PTR(self);
  mrb_get_args(mrb, "o", &arg);

  mrb_queue_lock(mrb, self);
  mrb_ary_unshift(context->mrb, context->queue, mrb_thread_migrate_value(mrb, arg, context->mrb));
  mrb_queue_unlock(mrb, self);

  check_pthread_error(mrb, pthread_cond_signal(&context->cond));
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
    check_pthread_error(mrb, pthread_cond_wait(&context->cond, &context->queue_lock));
  }

  mrb_queue_lock(mrb, self);
  ret = mrb_thread_migrate_value(context->mrb, mrb_ary_shift(context->mrb, context->queue), mrb);
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

  mrb_define_class(mrb, "ThreadError", mrb->eStandardError_class);

  _class_thread = mrb_define_class(mrb, "Thread", mrb->object_class);
  MRB_SET_INSTANCE_TT(_class_thread, MRB_TT_DATA);
  mrb_define_const(mrb, _class_thread, "COPY_VALUES", mrb_true_value());
  mrb_define_method(mrb, _class_thread, "initialize", mrb_thread_init, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, _class_thread, "join", mrb_thread_join, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_thread, "kill", mrb_thread_kill, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_thread, "terminate", mrb_thread_kill, MRB_ARGS_NONE());
  mrb_define_method(mrb, _class_thread, "alive?", mrb_thread_alive, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, _class_thread, "sleep", mrb_thread_sleep, MRB_ARGS_REQ(1));
  mrb_define_module_function(mrb, _class_thread, "usleep", mrb_thread_usleep, MRB_ARGS_REQ(1));
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
