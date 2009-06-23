#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver.h"

/* TLS variables */
__thread grn_ctx *mrn_ctx_tls;


/* static variables */
grn_hash *mrn_hash;
grn_obj *mrn_db, *mrn_lexicon;
pthread_mutex_t *mrn_mutext;
const char *mrn_logfile_name=MRN_LOG_FILE_NAME;
FILE *mrn_logfile = NULL;

grn_logger_info mrn_logger_info = {
  GRN_LOG_DUMP,
  GRN_LOG_TIME|GRN_LOG_MESSAGE,
  mrn_logger_func,
  NULL
};

/* additional functions */
int mrn_flush_logs()
{
  MRN_TRACE;
  pthread_mutex_lock(mrn_mutext);
  MRN_LOG(GRN_LOG_NOTICE, "logfile closed by FLUSH LOGS");
  fflush(mrn_logfile);
  fclose(mrn_logfile); /* reopen logfile for rotation */
  mrn_logfile = fopen(mrn_logfile_name, "a");
  MRN_LOG(GRN_LOG_NOTICE, "-------------------------------");
  MRN_LOG(GRN_LOG_NOTICE, "logfile re-opened by FLUSH LOGS");
  pthread_mutex_unlock(mrn_mutext);
  return 0;
}

int mrn_init()
{
  grn_ctx ctx;

  /* libgroonga init */
  if (grn_init() != GRN_SUCCESS) {
    return -1;
  }
  grn_ctx_init(&ctx,0);

  /* log init */
  if (!(mrn_logfile = fopen(mrn_logfile_name, "a"))) {
    return -1;
  }
  grn_logger_info_set(mrn_ctx_tls, &mrn_logger_info);
  GRN_LOG(&ctx, GRN_LOG_NOTICE, "++++++ starting mroonga ++++++");

  /* init meta-data repository */
  mrn_hash = grn_hash_create(&ctx,NULL,
				 MRN_MAX_KEY_LEN,sizeof(size_t),
				 GRN_OBJ_KEY_VAR_SIZE);
  mrn_db = mrn_db_open_or_create(&ctx);

  /* mutex init */
  mrn_mutext = (pthread_mutex_t*) MRN_MALLOC(sizeof(pthread_mutex_t));
  // TODO: FIX THIS 
  //pthread_mutex_init(mrn_mutext, PTHREAD_MUTEX_INITIALIZER);

  grn_ctx_fin(&ctx);
  return 0;
}

/*
  TODO: release all grn_obj in global hash
*/
int mrn_deinit()
{
  grn_ctx ctx;
  grn_ctx_init(&ctx,0);
  grn_obj_close(&ctx, mrn_lexicon);
  grn_obj_close(&ctx, mrn_db);

  /* mutex deinit*/
  pthread_mutex_destroy(mrn_mutext);
  MRN_FREE(mrn_mutext);

  /* log deinit */
  GRN_LOG(&ctx, GRN_LOG_NOTICE, "------ stopping mroonga ------");
  fclose(mrn_logfile);
  mrn_logfile = NULL;

  /* hash deinit */
  grn_hash_close(&ctx, mrn_hash);

  /* libgroonga deinit */
  grn_ctx_fin(&ctx);
  grn_fin();

  return 0;
}

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


void mrn_ctx_init()
{
  if (mrn_ctx_tls == NULL) {
    mrn_ctx_tls = (grn_ctx*) MRN_MALLOC(sizeof(grn_ctx));
    grn_ctx_init(mrn_ctx_tls, 0);
    if ((mrn_db))
      grn_ctx_use(mrn_ctx_tls, mrn_db);
  }
}

grn_obj *mrn_db_open_or_create(grn_ctx *ctx)
{
  grn_obj *obj;
  struct stat dummy;
  if ((stat(MRN_DB_FILE_PATH, &dummy))) { // check if file not exists
    GRN_LOG(ctx, GRN_LOG_DEBUG, "-> grn_db_create: '%s'", MRN_DB_FILE_PATH);
    obj = grn_db_create(ctx, MRN_DB_FILE_PATH, NULL);
    /* create global lexicon table */
    mrn_lexicon = grn_table_create(ctx, "lexicon", 7, NULL,
				       GRN_OBJ_TABLE_PAT_KEY|GRN_OBJ_PERSISTENT, 
				       grn_ctx_at(mrn_ctx_tls,GRN_DB_SHORTTEXT), 0);
    grn_obj_set_info(ctx, mrn_lexicon, GRN_INFO_DEFAULT_TOKENIZER,
		     grn_ctx_at(mrn_ctx_tls, GRN_DB_BIGRAM));
    GRN_LOG(ctx, GRN_LOG_DEBUG, "created lexicon table = %p", mrn_lexicon);
  } else {
    GRN_LOG(ctx, GRN_LOG_DEBUG, "-> grn_db_open: '%s'", MRN_DB_FILE_PATH);
    obj = grn_db_open(ctx, MRN_DB_FILE_PATH);
    /* open global lexicon table */
    mrn_lexicon = grn_table_open(ctx, "lexicon", 7, NULL);
    GRN_LOG(ctx, GRN_LOG_DEBUG, "opened lexicon table = %p", mrn_lexicon);
  }
  return obj;
}

void mrn_share_put(mrn_table *share)
{
  void *value;
  grn_search_flags flags = GRN_TABLE_ADD;
  /* TODO: check duplication */
  MRN_LOG(GRN_LOG_DEBUG,"-> grn_hash_lookup(put): name='%s'", share->name);
  grn_hash_lookup(mrn_ctx_tls, mrn_hash, share->name,
		  strlen(share->name), &value, &flags);
  memcpy(value, share, sizeof(share));
}

/* returns NULL if specified obj_name is not found in grn_hash */
mrn_table *mrn_share_get(const char *name)
{
  void *value;
  grn_search_flags flags = 0;
  MRN_LOG(GRN_LOG_DEBUG,"-> grn_hash_lookup(get): name='%s'", name);
  grn_id rid = grn_hash_lookup(mrn_ctx_tls, mrn_hash, name,
			       strlen(name), &value, &flags);
  if (rid == 0) {
    return NULL;
  } else {
    return (mrn_table*) value;
  }
}

void mrn_share_remove(mrn_table *share)
{
  /* TODO: check return value */
  MRN_LOG(GRN_LOG_DEBUG, "-> grn_hash_delete: name='%s'", share->name);
  grn_hash_delete(mrn_ctx_tls, mrn_hash, share->name,
		  strlen(share->name), NULL);
}

void mrn_share_remove_all()
{
  /* TODO: implement this function by using GRN_HASH_EACH */
}

