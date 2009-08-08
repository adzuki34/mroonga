#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver.h"
#include "config.h"

grn_hash *mrn_hash;
grn_obj *mrn_db, *mrn_lexicon;
pthread_mutex_t *mrn_lock, *mrn_lock_hash;
const char *mrn_logfile_name=MRN_LOG_FILE_NAME;
FILE *mrn_logfile = NULL;

uint mrn_hash_counter=0;

grn_logger_info mrn_logger_info = {
  GRN_LOG_DUMP,
  GRN_LOG_TIME|GRN_LOG_MESSAGE,
  mrn_logger_func,
  NULL
};

void mrn_logger_func(int level, const char *time, const char *title,
		     const char *msg, const char *location, void *func_arg)
{
  const char slev[] = " EACewnid-";
  if ((mrn_logfile)) {
    fprintf(mrn_logfile, "%s|%c|%u|%s\n", time,
	    *(slev + level), (uint)pthread_self(), msg);
    fflush(mrn_logfile);
  }
}

int mrn_flush_logs(grn_ctx *ctx)
{
  pthread_mutex_lock(mrn_lock);
  GRN_LOG(ctx, GRN_LOG_NOTICE, "flush logfile");
  if (fflush(mrn_logfile) || fclose(mrn_logfile)
      || (mrn_logfile = fopen(mrn_logfile_name, "a")) == NULL)
  {
    GRN_LOG(ctx, GRN_LOG_ERROR, "cannot flush logfile");
    return -1;
  }
  pthread_mutex_unlock(mrn_lock);
  return 0;
}

int mrn_init()
{
  grn_ctx ctx;

  // init groonga
  if (grn_init() != GRN_SUCCESS)
  {
    goto err;
  }

  grn_ctx_init(&ctx,0);

  // init log, and then we can do logging
  if (!(mrn_logfile = fopen(mrn_logfile_name, "a")))
  {
    goto err;
  }

  grn_logger_info_set(&ctx, &mrn_logger_info);
  GRN_LOG(&ctx, GRN_LOG_NOTICE, "%s start", PACKAGE_STRING);

  // init database
  struct stat dummy;
  if ((stat(MRN_DB_FILE_PATH, &dummy)))
  {
    GRN_LOG(&ctx, GRN_LOG_NOTICE, "database not exists");
    if ((mrn_db = grn_db_create(&ctx, MRN_DB_FILE_PATH, NULL)))
    {
      GRN_LOG(&ctx, GRN_LOG_NOTICE, "database created");
    }
    else
    {
      GRN_LOG(&ctx, GRN_LOG_ERROR, "cannot create database, exiting");
      goto err;
    }
  }
  else
  {
    mrn_db = grn_db_open(&ctx, MRN_DB_FILE_PATH);
  }
  grn_ctx_use(&ctx, mrn_db);


  // init lexicon table
  if (!(mrn_lexicon = grn_ctx_get(&ctx,"lexicon",7)))
  {
    GRN_LOG(&ctx, GRN_LOG_NOTICE, "lexicon table not exists");
    if ((mrn_lexicon = grn_table_create(&ctx, MRN_LEXICON_TABLE_NAME,
                                        strlen(MRN_LEXICON_TABLE_NAME), NULL,
                                        GRN_OBJ_TABLE_PAT_KEY|GRN_OBJ_PERSISTENT,
                                        grn_ctx_at(&ctx,GRN_DB_SHORTTEXT), 0)))
    {
      GRN_LOG(&ctx, GRN_LOG_NOTICE, "lexicon table created");
    }
    else
    {
      GRN_LOG(&ctx, GRN_LOG_ERROR, "cannot create lexicon table, exiting");
      goto err;
    }
  }

  // init hash
  if (!(mrn_hash = grn_hash_create(&ctx,NULL,
                                   MRN_MAX_KEY_LEN,sizeof(size_t),
                                   GRN_OBJ_KEY_VAR_SIZE)))
  {
    GRN_LOG(&ctx, GRN_LOG_ERROR, "cannot init hash, exiting");
    goto err;
  }

  // init lock
  mrn_lock = malloc(sizeof(pthread_mutex_t));
  if ((mrn_lock == NULL) || (pthread_mutex_init(mrn_lock, NULL) != 0))
  {
    goto err;
  }
  mrn_lock_hash = malloc(sizeof(pthread_mutex_t));
  if ((mrn_lock_hash == NULL) || (pthread_mutex_init(mrn_lock_hash, NULL) != 0))
  {
    goto err;
  }

  grn_ctx_fin(&ctx);
  return 0;

err:
  // TODO: report more detail error to mysql
  grn_ctx_fin(&ctx);
  return -1;
}

int mrn_deinit()
{
  grn_ctx ctx;
  grn_ctx_init(&ctx,0);
  grn_ctx_use(&ctx, mrn_db);
  GRN_LOG(&ctx, GRN_LOG_NOTICE, "shutdown");


  pthread_mutex_destroy(mrn_lock);
  free(mrn_lock);
  mrn_lock = NULL;
  pthread_mutex_destroy(mrn_lock_hash);
  free(mrn_lock_hash);
  mrn_lock_hash = NULL;

  grn_hash_close(&ctx, mrn_hash);
  mrn_hash = NULL;

  grn_obj_close(&ctx, mrn_lexicon);
  mrn_lexicon = NULL;

  grn_obj_close(&ctx, mrn_db);
  mrn_db = NULL;

  fclose(mrn_logfile);
  mrn_logfile = NULL;

  grn_ctx_fin(&ctx);
  grn_fin();

  return 0;
}

/**
 *   0 success
 *  -1 duplicated
 */
int mrn_hash_put(grn_ctx *ctx, const char *key, void *value)
{
  int added, res=0;
  void *buf;
  pthread_mutex_lock(mrn_lock_hash);
  grn_hash_add(ctx, mrn_hash, (const char*) key, strlen(key), &buf, &added);
  // duplicate check
  if (added == 0)
  {
    GRN_LOG(ctx, GRN_LOG_WARNING, "hash put duplicated (key=%s)", key);
    res = -1;
  }
  else
  {
    // store address of value
    memcpy(buf, &value, sizeof(buf));
    mrn_hash_counter++;
  }
  pthread_mutex_unlock(mrn_lock_hash);
  return res;
}

/**
 *   0 success
 *  -1 not found
 */
int mrn_hash_get(grn_ctx *ctx, const char *key, void **value)
{
  int res = 0;
  grn_id id;
  void *buf;
  pthread_mutex_lock(mrn_lock_hash);
  id = grn_hash_get(ctx, mrn_hash, (const char*) key, strlen(key), &buf);
  // key not found
  if (id == GRN_ID_NIL)
  {
    GRN_LOG(ctx, GRN_LOG_DEBUG, "hash get not found (key=%s)", key);
    res = -1;
  }
  else
  {
    // restore address of value
    memcpy(value, buf, sizeof(buf));
  }
  pthread_mutex_unlock(mrn_lock_hash);
  return res;
}

/**
 *   0 success
 *  -1 error
 */
int mrn_hash_remove(grn_ctx *ctx, const char *key)
{
  int res = 0;
  grn_rc rc;
  grn_id id;
  pthread_mutex_lock(mrn_lock_hash);
  id = grn_hash_get(ctx, mrn_hash, (const char*) key, strlen(key), NULL);
  if (id == GRN_ID_NIL)
  {
    GRN_LOG(ctx, GRN_LOG_WARNING, "hash remove not found (key=%s)", key);
    res = -1;
  }
  else
  {
    rc = grn_hash_delete_by_id(ctx, mrn_hash, id, NULL);
    if (rc != GRN_SUCCESS) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "hash remove error (key=%s)", key);
      res = -1;
    }
    else
    {
      mrn_hash_counter--;
    }
  }
  pthread_mutex_unlock(mrn_lock_hash);
  return res;
}

mrn_info *mrn_init_obj_info(grn_ctx *ctx, uint n_columns)
{
  int i, alloc_size = 0;
  void *ptr;
  mrn_info *info;
  alloc_size = sizeof(mrn_info) + sizeof(mrn_table_info) +
    (sizeof(void*) + sizeof(mrn_column_info)) * n_columns;
  ptr = malloc(alloc_size);
  if (ptr == NULL)
  {
    GRN_LOG(ctx, GRN_LOG_ERROR, "malloc error mrn_init_create_info size=%d",alloc_size);
    return NULL;
  }
  info = (mrn_info*) ptr;
  ptr += sizeof(mrn_info);

  info->table = ptr;
  info->table->name = NULL;
  info->table->name_size = 0;
  info->table->path = NULL;
  info->table->flags = GRN_OBJ_PERSISTENT;
  info->table->key_type = NULL;
  info->table->obj = NULL;

  ptr += sizeof(mrn_table_info);
  info->columns = (mrn_column_info**) ptr;

  ptr += sizeof(void*) * n_columns;
  for (i = 0; i < n_columns; i++)
  {
    info->columns[i] = ptr + sizeof(mrn_column_info) * i;
    info->columns[i]->name = NULL;
    info->columns[i]->name_size = 0;
    info->columns[i]->path = NULL;
    info->columns[i]->flags = GRN_OBJ_PERSISTENT;
    info->columns[i]->type = NULL;
    info->columns[i]->obj = NULL;
  }

  info->n_columns = n_columns;
  info->ref_count = 0;
  return info;
}

int mrn_deinit_obj_info(grn_ctx *ctx, mrn_info *info)
{
  free(info);
  return 0;
}

int mrn_create(grn_ctx *ctx, mrn_info *info)
{
  int i;
  mrn_table_info *table;
  mrn_column_info *column;

  table = info->table;

  table->obj = grn_table_create(ctx, table->name, table->name_size,
                                table->path, table->flags,
                                table->key_type, 0);
  if (table->obj == NULL)
  {
    GRN_LOG(ctx, GRN_LOG_ERROR, "cannot create table: name=%s, name_size=%d, path=%s, "
            "flags=%d, key_type=%p, value_size=%d", table->name, table->name_size,
            table->path, table->flags, table->key_type, NULL);
    return -1;
  }
  for (i=0; i < info->n_columns; i++)
  {
    column = info->columns[i];
    column->obj = grn_column_create(ctx, table->obj, column->name,
                                    column->name_size, column->path,
                                    column->flags, column->type);
    if (column->obj == NULL)
    {
      GRN_LOG(ctx, GRN_LOG_ERROR, "cannot create column: table=%p, name=%s, name_size=%d, "
              "path=%s, flags=%d, type=%p", table->obj, column->name,
              column->name_size, column->path,
              column->flags, column->type);
      goto auto_drop;
    }
    else
    {
      grn_obj_close(ctx, column->obj);
      column->obj = NULL;
    }
  }
  grn_obj_close(ctx, table->obj);
  table->obj = NULL;
  return 0;

auto_drop:
  GRN_LOG(ctx, GRN_LOG_ERROR, "auto-drop table/columns");
  for (; i > 0; --i)
  {
    grn_obj_remove(ctx, info->columns[i]->obj);
    info->columns[i]->obj = NULL;
  }
  grn_obj_remove(ctx, info->table->obj);
  info->table->obj = NULL;
  return -1;
}

int mrn_open(grn_ctx *ctx, mrn_info *info)
{
  int i;
  mrn_table_info *table;
  mrn_column_info *column;

  table = info->table;

  table->obj = grn_ctx_get(ctx, table->name, table->name_size);
  if (table->obj == NULL)
  {
    GRN_LOG(ctx, GRN_LOG_ERROR, "cannot open table: name=%s", table->name);
    return -1;
  }
  for (i=0; i < info->n_columns; i++)
  {
    column = info->columns[i];
    column->obj = grn_obj_column(ctx, table->obj, column->name, column->name_size);
    if (column->obj == NULL)
    {
      GRN_LOG(ctx, GRN_LOG_ERROR, "cannot open column: table=%s, column=%s",
              table->name, column->name);
      goto auto_close;
    }
  }
  return 0;

auto_close:
  GRN_LOG(ctx, GRN_LOG_ERROR, "auto-closing table/columns");
  for (; i > 0; --i)
  {
    grn_obj_close(ctx, info->columns[i]->obj);
    info->columns[i]->obj = NULL;
  }
  grn_obj_close(ctx, info->table->obj);
  info->table->obj = NULL;
  return -1;
}

int mrn_close(grn_ctx *ctx, mrn_info *info)
{
  int i;
  for (i=0; i < info->n_columns; i++)
  {
    grn_obj_close(ctx, info->columns[i]->obj);
    info->columns[i]->obj = NULL;
  }
  grn_obj_close(ctx, info->table->obj);
  info->table->obj = NULL;
  return 0;
}

int mrn_drop(grn_ctx *ctx, const char *table_name)
{
  grn_obj *table;
  table = grn_ctx_get(ctx, table_name, strlen(table_name));
  if (table == NULL)
  {
    goto err;
  }
  grn_obj_remove(ctx, table);
  return 0;

err:
  GRN_LOG(ctx, GRN_LOG_ERROR, "grn_ctx_get in mrn_drop failed:[%s,%d,%p,%p]",
          table_name, strlen(table_name), ctx, grn_ctx_db(ctx));
  return -1;
}

int mrn_write_row(grn_ctx *ctx, mrn_record *record)
{
  grn_obj *table, *column;
  grn_id gid;
  int added, i;
  table = record->info->table->obj;
  gid = grn_table_add(ctx, table, record->key, record->key_size, &added);
  if (added == 0)
  {
    goto duplicated_key_err;
  }
  for (i=0; i < record->info->n_columns; i++)
  {
    column = record->info->columns[i]->obj;
    if (record->value[i] == NULL)
    {
      continue;
    }
    if (grn_obj_set_value(ctx, column, gid, record->value[i], GRN_OBJ_SET)
        != GRN_SUCCESS)
    {
      goto obj_set_err;
    }
  }
  return 0;

duplicated_key_err:
  GRN_LOG(ctx, GRN_LOG_INFO, "duplicated key error for table=%s",
          record->info->table->name);
  return -2;
obj_set_err:
  return -1;
}

mrn_record* mrn_init_record(grn_ctx *ctx, mrn_info *info, mrn_column_list *list)
{
  mrn_record *record;
  int i, j, size, offset, actual_size;
  void *p;
  if (list)
  {
    actual_size = list->actual_size;
  }
  else
  {
    actual_size = info->n_columns;
  }
  size = sizeof(mrn_record) + (sizeof(grn_obj*) + sizeof(grn_obj)) * actual_size;
  p = malloc(size);
  record = (mrn_record*) p;
  p += sizeof(mrn_record);
  record->info = info;
  record->list = list;
  record->value = (grn_obj**) p;
  p += sizeof(grn_obj*) * actual_size;
  for (i=0,j=0,offset=0; i < info->n_columns; i++)
  {
    if ((list) && (list->columns[i] == NULL))
    {
      // column pruning
      continue;
    }
    else
    {
      record->value[j] = (grn_obj*) (p + offset);
      grn_builtin_type gtype = info->columns[i]->gtype;
      switch (gtype)
      {
      case GRN_DB_INT32:
        GRN_INT32_INIT(record->value[j], 0);
        break;
      case GRN_DB_TEXT:
        GRN_TEXT_INIT(record->value[j], 0);
        break;
      default:
        GRN_TEXT_INIT(record->value[j], 0);
      }
      offset += sizeof(grn_obj);
      j++;
    }
  }
  record->actual_size = actual_size;
  record->n_columns = info->n_columns;
  return record;
}


int mrn_deinit_record(grn_ctx *ctx, mrn_record *record)
{
  int i;
  if (record->list)
  {
    for (i=0; i < record->list->actual_size; i++)
    {
      grn_obj_close(ctx, record->value[i]);
    }
  }
  else
  {
    for (i=0; i < record->n_columns; i++)
    {
      grn_obj_close(ctx, record->value[i]);
    }
  }
  free(record);
  return 0;
}

int mrn_rewind_record(grn_ctx *ctx, mrn_record *record)
{
  int i;
  for (i=0; i < record->actual_size; i++)
  {
    GRN_BULK_REWIND(record->value[i]);
  }
  return 0;
}

int mrn_rnd_init(grn_ctx *ctx, mrn_info *info)
{
  info->cursor = grn_table_cursor_open(ctx, info->table->obj, NULL, 0, NULL, 0, 0);
  if (info->cursor == NULL)
  {
    GRN_LOG(ctx, GRN_LOG_ERROR, "cannot open cursor: %s", info->table->name);
    return -1;
  }
  return 0;
}

int mrn_rnd_next(grn_ctx *ctx, mrn_record *record, mrn_column_list *list)
{
  int i,j;
  grn_table_cursor *cursor = record->info->cursor;
  record->id = grn_table_cursor_next(ctx, cursor);
  if (record->id == GRN_ID_NIL)
  {
    grn_table_cursor_close(ctx, cursor);
    record->info->cursor = NULL;
    return 1; // EOF
  }
  else
  {
    for (i=0,j=0; i < record->n_columns; i++)
    {
      grn_obj *res;
      if ((list) && (list->columns[i] == NULL))
      {
        // column pruning
        continue;
      }
      else
      {
        if (grn_obj_get_value(ctx, record->info->columns[i]->obj,
                              record->id, record->value[j]) != NULL)
        {
          j++;
        }
        else
        {
          GRN_LOG(ctx, GRN_LOG_ERROR, "error while fetching cursor:[%s,%d,%d]",
                  record->info->table->name, i, record->id);
          goto err;
        }
      }
    }
  }
  return 0;

err:
  return -1;
}

uint mrn_table_size(grn_ctx *ctx, mrn_info *info)
{
  return grn_table_size(ctx, info->table->obj);
}

mrn_column_list* mrn_init_column_list(grn_ctx *ctx, mrn_info *info, int *src, int size)
{
  int *spot;
  mrn_column_list *list;
  int i, actual_size=0 , n_columns=info->n_columns;
  void *p;
  spot = (int*) malloc(sizeof(int) * n_columns);
  if (spot == NULL)
  {
    goto err_oom;
  }
  memset(spot, 0, sizeof(int) * n_columns);
  for (i=0; i < size; i++)
  {
    spot[src[i]] = 1;
  }
  for (i=0; i < n_columns; i++)
  {
    if (spot[i] == 1)
    {
      actual_size++;
    }
  }
  p = malloc(sizeof(mrn_column_list) + sizeof(mrn_column_list*) * n_columns);
  if (p == NULL)
  {
    goto err_oom;
  }
  list = (mrn_column_list*) p;
  list->info = info;
  list->columns = (mrn_column_info**) (p + sizeof(mrn_column_list));
  list->actual_size = actual_size;
  for (i=0; i < n_columns; i++)
  {
    if (spot[i] == 0)
    {
      list->columns[i] = NULL;
    }
    else
    {
      list->columns[i] = info->columns[i];
    }
  }
  free(spot);
  return list;

err_oom:
  GRN_LOG(ctx, GRN_LOG_ERROR, "oom error in mrn_init_column_list:[%s,%d]",
          info->table->name, size);
  if (spot)
  {
    free(spot);
  }
  return NULL;
}

int mrn_deinit_column_list(grn_ctx *ctx, mrn_column_list *list)
{
  free(list);
  return 0;
}