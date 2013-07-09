#include <errno.h>
#include <memory.h>
#include <mruby.h>
#include <mruby/proc.h>
#include <mruby/data.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/variable.h>
#include <sqlite3.h>
#include <stdlib.h>

#if 1
#define ARENA_SAVE \
  int ai = mrb_gc_arena_save(mrb); \
  if (ai == MRB_ARENA_SIZE) { \
    mrb_raise(mrb, E_RUNTIME_ERROR, "arena overflow"); \
  }
#define ARENA_RESTORE \
  mrb_gc_arena_restore(mrb, ai);
#else
#define ARENA_SAVE
#define ARENA_RESTORE
#endif

typedef struct {
  mrb_state *mrb;
  sqlite3* db;
} mrb_sqlite3_database;

typedef struct {
  mrb_state *mrb;
  mrb_value db;
  sqlite3_stmt* stmt;
} mrb_sqlite3_resultset;

static void
mrb_sqlite3_database_free(mrb_state *mrb, void *p) {
  mrb_sqlite3_database* db = (mrb_sqlite3_database*) p;
  if (db->db) {
    sqlite3_close(db->db);
  }
  free(db);
}

static void
mrb_sqlite3_resultset_free(mrb_state *mrb, void *p) {
  mrb_sqlite3_resultset* rs = (mrb_sqlite3_resultset*) p;
  free(rs);
}

static const struct mrb_data_type mrb_sqlite3_database_type = {
  "mrb_sqlite3_database", mrb_sqlite3_database_free,
};

static const struct mrb_data_type mrb_sqlite3_resultset_type = {
  "mrb_sqlite3_resultset", mrb_sqlite3_resultset_free,
};

static mrb_value
mrb_sqlite3_database_init(mrb_state *mrb, mrb_value self) {
  char* name = NULL;
  mrb_value arg_file;
  sqlite3* sdb = NULL;
  int rv;
  mrb_sqlite3_database* db;

  mrb_get_args(mrb, "|S", &arg_file);
  if (!mrb_nil_p(arg_file)) {
    size_t len = RSTRING_LEN(arg_file);
    name = malloc(len + 1);
    if (!name) {
      mrb_raise(mrb, E_RUNTIME_ERROR, "can't memory alloc");
    }
    strncpy(name, RSTRING_PTR(arg_file), len);
    name[len] = 0;
  }

  rv = sqlite3_open_v2(name ? name : ":memory:", &sdb,
      SQLITE_OPEN_FULLMUTEX |
      SQLITE_OPEN_READWRITE |
      SQLITE_OPEN_CREATE |
#ifdef SQLITE_OPEN_URI
      SQLITE_OPEN_URI |
#endif
      0,
      NULL);
  if (name) {
    free(name);
  }
  if (rv != SQLITE_OK) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "can't open database");
  }

  db = (mrb_sqlite3_database*)
    malloc(sizeof(mrb_sqlite3_database));
  if (!db) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "can't memory alloc");
  }
  memset(db, 0, sizeof(mrb_sqlite3_database));
  db->mrb = mrb;
  db->db = sdb;

  mrb_iv_set(mrb, self, mrb_intern(mrb, "context"), mrb_obj_value(
    Data_Wrap_Struct(mrb, mrb->object_class,
    &mrb_sqlite3_database_type, (void*) db)));
  return self;
}

static const char*
bind_values(mrb_state* mrb, sqlite3* db, sqlite3_stmt* stmt, int argc, mrb_value* argv) {
  int i;
  sqlite3_reset(stmt);
  for (i = 0; i < argc; i++) {
    int rv = SQLITE_MISMATCH;
    switch (mrb_type(argv[i])) {
    case MRB_TT_UNDEF:
      rv = sqlite3_bind_null(stmt, i+1);
      break;
    case MRB_TT_STRING:
      rv = sqlite3_bind_text(stmt, i+1, RSTRING_PTR(argv[i]), RSTRING_LEN(argv[i]), NULL);
      break;
    case MRB_TT_FIXNUM:
      rv = sqlite3_bind_int(stmt, i+1, mrb_fixnum(argv[i]));
      break;
    case MRB_TT_FLOAT:
      rv = sqlite3_bind_double(stmt, i+1, mrb_float(argv[i]));
      break;
    case MRB_TT_TRUE:
      rv = sqlite3_bind_int(stmt, i+1, 1);
      break;
    case MRB_TT_FALSE:
      rv = sqlite3_bind_int(stmt, i+1, 0);
      break;
    default:
      return "invalid argument";
      break;
    }
    if (rv != SQLITE_OK) {
      return sqlite3_errmsg(db);
    }
  }
  return NULL;
}

static mrb_value
row_to_value(mrb_state* mrb, sqlite3_stmt* stmt) {
  int i;
  int count = sqlite3_column_count(stmt);
  mrb_value a = mrb_ary_new(mrb);
  for (i = 0; i < count; i++) {
    switch (sqlite3_column_type(stmt, i)) {
    case SQLITE_INTEGER:
      {
        sqlite3_int64 value = sqlite3_column_int64(stmt, i);
        mrb_ary_push(mrb, a, mrb_fixnum_value((mrb_int) value));
      }
      break;
    case SQLITE_FLOAT:
      {
        double value = sqlite3_column_double(stmt, i);
        mrb_ary_push(mrb, a, mrb_float_value(mrb, value));
      }
      break;
    case SQLITE_BLOB:
      {
        int size = sqlite3_column_bytes(stmt, i);
        const char* ptr = sqlite3_column_blob(stmt, i);
        mrb_ary_push(mrb, a, mrb_str_new(mrb, ptr, size));
      }
      break;
    case SQLITE_NULL:
      mrb_ary_push(mrb, a, mrb_nil_value());
      break;
    case SQLITE_TEXT:
      {
        const char* value = (const char*) sqlite3_column_text(stmt, i);
        mrb_ary_push(mrb, a, mrb_str_new_cstr(mrb, value));
      }
      break;
    }
  }
  return a;
}

static mrb_value
mrb_sqlite3_database_execute(mrb_state *mrb, mrb_value self) {
  int argc = 0;
  mrb_value* argv = NULL;
  mrb_value b = mrb_nil_value();
  mrb_value value_context;
  mrb_sqlite3_database* db = NULL;
  mrb_value fields;
  int i, r, count;
  sqlite3_stmt* stmt = NULL;
  mrb_value args[2];
  mrb_value query;

  mrb_get_args(mrb, "&S*", &b, &query, &argv, &argc);

  value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  db = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_database_type, db);
  if (!db) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }

  r = sqlite3_prepare_v2(db->db, RSTRING_PTR(query), RSTRING_LEN(query),
    &stmt, NULL);
  if (r != SQLITE_OK) {
    if (stmt) {
      sqlite3_finalize(stmt);
      sqlite3_reset(stmt);
    }
    mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
  }
  if (!stmt) {
    return mrb_nil_value();
  }

  if (argc > 0) {
    const char* error = bind_values(mrb, db->db, stmt, argc, argv);
    if (error) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, error);
    }
  }

  fields = mrb_ary_new(mrb);
  count = sqlite3_column_count(stmt);
  for (i = 0; i < count; i++) {
    const char* name = sqlite3_column_name(stmt, i);
    mrb_ary_push(mrb, fields, mrb_str_new_cstr(mrb, name));
  }

  if (mrb_nil_p(b)) {
    struct RClass* _class_sqlite3;
    struct RClass* _class_sqlite3_resultset;
    mrb_value c;
    mrb_sqlite3_resultset* rs = (mrb_sqlite3_resultset*)
      malloc(sizeof(mrb_sqlite3_resultset));
    if (!rs) {
      mrb_raise(mrb, E_RUNTIME_ERROR, "can't memory alloc");
    }
    memset(rs, 0, sizeof(mrb_sqlite3_resultset));
    rs->mrb = mrb;
    rs->stmt = stmt;
    _class_sqlite3 = mrb_class_get(mrb, "SQLite3");
    _class_sqlite3_resultset = mrb_class_ptr(mrb_const_get(mrb, mrb_obj_value(_class_sqlite3), mrb_intern(mrb, "ResultSet")));
    c = mrb_class_new_instance(mrb, 0, NULL, _class_sqlite3_resultset);
    mrb_iv_set(mrb, c, mrb_intern(mrb, "context"), mrb_obj_value(
      Data_Wrap_Struct(mrb, mrb->object_class,
      &mrb_sqlite3_resultset_type, (void*) rs)));
    mrb_iv_set(mrb, c, mrb_intern(mrb, "fields"), fields);
    mrb_iv_set(mrb, c, mrb_intern(mrb, "db"), self);
    mrb_iv_set(mrb, c, mrb_intern(mrb, "eof"), mrb_false_value());
    return c;
  }
  while ((r = sqlite3_step(stmt)) == SQLITE_ROW) {
    int ai = mrb_gc_arena_save(mrb);
    args[0] = row_to_value(mrb, stmt);
    args[1] = fields;
    mrb_yield_argv(mrb, b, 2, args);
    mrb_gc_arena_restore(mrb, ai);
  }
  sqlite3_finalize(stmt);
  if (r != SQLITE_OK && r != SQLITE_DONE) {
    mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
  }
  return mrb_nil_value();
}

static mrb_value
mrb_sqlite3_database_execute_batch(mrb_state *mrb, mrb_value self) {
  int argc = 0;
  mrb_value *argv;
  mrb_value value_context;
  mrb_sqlite3_database* db = NULL;
  mrb_value query;
  int len;
  char* top;
  const char *tail, *sql;

  mrb_get_args(mrb, "*", &argv, &argc);

  if (argc == 0) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
  value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_database_type, db);
  if (!db) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }

  query = argv[0];
  len = RSTRING_LEN(query);
  top = malloc(len + 1);
  if (!top) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "can't memory alloc");
  }
  memcpy(top, RSTRING_PTR(query), len);
  top[len] = 0;
  tail = (const char*) top;

  while (*(sql = tail)) {
    sqlite3_stmt* stmt = NULL;
    int r = sqlite3_prepare_v2(db->db, sql, -1, &stmt, &tail);
    if (argc > 1) {
      const char* error = bind_values(mrb, db->db, stmt, argc-1, &argv[1]);
      argc = 0;
      if (error) {
        free(top);
        mrb_raise(mrb, E_ARGUMENT_ERROR, error);
      }
    }
    if (r != SQLITE_OK) {
      if (stmt) {
        sqlite3_finalize(stmt);
        sqlite3_reset(stmt);
      }
      free(top);
      mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
    }
    if (stmt) {
      r = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      if (r != SQLITE_OK && r != SQLITE_DONE) {
        free(top);
        mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
      }
    }
  }
  free(top);
  return mrb_fixnum_value(sqlite3_changes(db->db));
}

static mrb_value
mrb_sqlite3_database_close(mrb_state *mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  mrb_sqlite3_database* db = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_database_type, db);
  if (!db) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
  if (db->db) {
    sqlite3_stmt* stmt = sqlite3_next_stmt(db->db, NULL);
    while (stmt != NULL) {
      sqlite3_finalize(stmt);
      stmt = sqlite3_next_stmt(db->db, NULL);
    }
    if (sqlite3_close(db->db) != SQLITE_OK) {
      mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
    }
    db->db = NULL;
  }
  return mrb_nil_value();
}

static mrb_value
mrb_sqlite3_database_last_insert_rowid(mrb_state *mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  mrb_sqlite3_database* db = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_database_type, db);
  if (!db) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
  return mrb_fixnum_value((mrb_int) sqlite3_last_insert_rowid(db->db));
}

static mrb_value
mrb_sqlite3_database_changes(mrb_state *mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  mrb_sqlite3_database* db = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_database_type, db);
  if (!db) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
  return mrb_fixnum_value(sqlite3_changes(db->db));
}

static mrb_value
mrb_sqlite3_database_exec(mrb_state *mrb, mrb_value self, const char* query) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  mrb_sqlite3_database* db = NULL;
  int r;
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_database_type, db);
  if (!db) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
  r = sqlite3_exec(db->db, query, NULL, NULL, NULL);
  if (r != SQLITE_OK) {
    mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
  }
  return mrb_nil_value();
}

static mrb_value
mrb_sqlite3_database_transaction(mrb_state *mrb, mrb_value self) {
  return mrb_sqlite3_database_exec(mrb, self, "begin");
}

static mrb_value
mrb_sqlite3_database_commit(mrb_state *mrb, mrb_value self) {
  return mrb_sqlite3_database_exec(mrb, self, "commit");
}

static mrb_value
mrb_sqlite3_database_rollback(mrb_state *mrb, mrb_value self) {
  return mrb_sqlite3_database_exec(mrb, self, "rollback");
}

static mrb_value
mrb_sqlite3_resultset_next(mrb_state *mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  mrb_sqlite3_resultset* rs = NULL;
  int r;
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_resultset_type, rs);
  if (!rs) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
  r = sqlite3_step(rs->stmt);
  if (r != SQLITE_ROW && r != SQLITE_OK && r != SQLITE_DONE) {
    mrb_value vdb = mrb_iv_get(mrb, self, mrb_intern(mrb, "db"));
    if (!mrb_nil_p(vdb)) {
      mrb_sqlite3_database* db = NULL;
      value_context = mrb_iv_get(mrb, vdb, mrb_intern(mrb, "context"));
      Data_Get_Struct(mrb, value_context, &mrb_sqlite3_database_type, db);
      if (db) {
        mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
      }
    }
    return mrb_nil_value();
  }
  if (r == SQLITE_DONE) {
    mrb_iv_set(mrb, self, mrb_intern(mrb, "eof"), mrb_true_value());
    return mrb_nil_value();
  }

  return row_to_value(mrb, rs->stmt);
}

static mrb_value
mrb_sqlite3_resultset_close(mrb_state *mrb, mrb_value self) {
  mrb_value value_context = mrb_iv_get(mrb, self, mrb_intern(mrb, "context"));
  mrb_sqlite3_resultset* rs = NULL;
  Data_Get_Struct(mrb, value_context, &mrb_sqlite3_resultset_type, rs);
  if (!rs) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid argument");
  }
  if (sqlite3_finalize(rs->stmt) != SQLITE_OK) {
    mrb_value vdb = mrb_iv_get(mrb, self, mrb_intern(mrb, "db"));
    if (!mrb_nil_p(vdb)) {
      mrb_sqlite3_database* db = NULL;
      value_context = mrb_iv_get(mrb, vdb, mrb_intern(mrb, "context"));
      Data_Get_Struct(mrb, value_context, &mrb_sqlite3_database_type, db);
      if (db) {
        mrb_raise(mrb, E_RUNTIME_ERROR, sqlite3_errmsg(db->db));
      }
    }
    return mrb_nil_value();
  }
  rs->stmt = NULL;
  return mrb_nil_value();
}

static mrb_value
mrb_sqlite3_resultset_fields(mrb_state *mrb, mrb_value self) {
  return mrb_iv_get(mrb, self, mrb_intern(mrb, "fields"));
}

static mrb_value
mrb_sqlite3_resultset_eof(mrb_state *mrb, mrb_value self) {
  return mrb_iv_get(mrb, self, mrb_intern(mrb, "eof"));
}

void
mrb_mruby_sqlite3_gem_init(mrb_state* mrb) {
  struct RClass *_class_sqlite3;
  struct RClass *_class_sqlite3_database;
  struct RClass* _class_sqlite3_resultset;
  ARENA_SAVE;
  
  _class_sqlite3 = mrb_define_module(mrb, "SQLite3");

  _class_sqlite3_database = mrb_define_class_under(mrb, _class_sqlite3, "Database", mrb->object_class);
  mrb_define_method(mrb, _class_sqlite3_database, "initialize", mrb_sqlite3_database_init, ARGS_OPT(1));
  mrb_define_method(mrb, _class_sqlite3_database, "execute", mrb_sqlite3_database_execute, ARGS_ANY());
  mrb_define_method(mrb, _class_sqlite3_database, "execute_batch", mrb_sqlite3_database_execute_batch, ARGS_ANY());
  mrb_define_method(mrb, _class_sqlite3_database, "close", mrb_sqlite3_database_close, ARGS_NONE());
  mrb_define_method(mrb, _class_sqlite3_database, "last_insert_rowid", mrb_sqlite3_database_last_insert_rowid, ARGS_NONE());
  mrb_define_method(mrb, _class_sqlite3_database, "changes", mrb_sqlite3_database_changes, ARGS_NONE());
  mrb_define_method(mrb, _class_sqlite3_database, "transaction", mrb_sqlite3_database_transaction, ARGS_NONE());
  mrb_define_method(mrb, _class_sqlite3_database, "commit", mrb_sqlite3_database_commit, ARGS_NONE());
  mrb_define_method(mrb, _class_sqlite3_database, "rollback", mrb_sqlite3_database_rollback, ARGS_NONE());
  ARENA_RESTORE;

  _class_sqlite3_resultset = mrb_define_class_under(mrb, _class_sqlite3, "ResultSet", mrb->object_class);
  mrb_define_method(mrb, _class_sqlite3_resultset, "next", mrb_sqlite3_resultset_next, ARGS_NONE());
  mrb_define_method(mrb, _class_sqlite3_resultset, "close", mrb_sqlite3_resultset_close, ARGS_NONE());
  mrb_define_method(mrb, _class_sqlite3_resultset, "fields", mrb_sqlite3_resultset_fields, ARGS_NONE());
  mrb_define_method(mrb, _class_sqlite3_resultset, "eof?", mrb_sqlite3_resultset_eof, ARGS_NONE());
  ARENA_RESTORE;
}

void
mrb_mruby_sqlite3_gem_final(mrb_state* mrb) {
}

/* vim:set et ts=2 sts=2 sw=2 tw=0: */
