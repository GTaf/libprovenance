/*
*
* Author: Thomas Pasquier <tfjmp2@cl.cam.ac.uk>
*
* Copyright (C) 2015-2018 University of Cambridge, Harvard University
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2, as
* published by the Free Software Foundation.
*
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <linux/camflow.h>
#include <sys/utsname.h>
#include <linux/provenance_types.h>

#include "provenance.h"
#include "provenanceProvJSON.h"
#include "provenanceutils.h"

#define MAX_PROVJSON_BUFFER_EXP     13
#define MAX_PROVJSON_BUFFER_LENGTH  ((1 << MAX_PROVJSON_BUFFER_EXP)*sizeof(uint8_t))

static char date[256];
pthread_rwlock_t  date_lock = PTHREAD_RWLOCK_INITIALIZER;

// ideally should be derived from jiffies
static void update_time( void ){
  struct tm tm;
  struct timeval tv;

  pthread_rwlock_wrlock(&date_lock);
  gettimeofday(&tv, NULL);
  gmtime_r(&tv.tv_sec, &tm);
  strftime(date, 30,"%Y:%m:%dT%H:%M:%S", &tm);
  pthread_rwlock_unlock(&date_lock);
}
const static char prefix[] = "\"prov\" : \"http://www.w3.org/ns/prov\", \"cf\":\"http://www.camflow.org\"";
const char* prefix_json(){
  return prefix;
}

static pthread_mutex_t l_flush =  PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t l_activity =  PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t l_agent =  PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t l_entity =  PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t l_used =  PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t l_generated =  PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t l_informed =  PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t l_derived =  PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t l_message =  PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static char* activity;
static char* agent;
static char* entity;
static char* used;
static char* generated;
static char* informed;
static char* derived;
static char* message;

static inline void init_buffer(char **buffer){
  *buffer = (char*)malloc(MAX_PROVJSON_BUFFER_LENGTH);
  memset(*buffer, 0, MAX_PROVJSON_BUFFER_LENGTH);
}

void init_buffers(void){
  init_buffer(&activity);
  init_buffer(&agent);
  init_buffer(&entity);
  init_buffer(&used);
  init_buffer(&generated);
  init_buffer(&informed);
  init_buffer(&derived);
  init_buffer(&message);
}

bool writing_out = false;

static void (*print_json)(char* json);

int disclose_node_ProvJSON(uint64_t type, const char* content, union prov_identifier* identifier){
  int err;
  struct disc_node_struct node;

  strncpy(node.content, content, PATH_MAX);
  node.length=strnlen(content, PATH_MAX);
  node.identifier.node_id.type=type;
  err = provenance_disclose_node(&node);
  if(err<0)
    return err;
  memcpy(identifier, &node.identifier, sizeof(union prov_identifier));
  return err;
}

int disclose_relation_ProvJSON(uint64_t type, union prov_identifier* sender, union prov_identifier* receiver){
  struct relation_struct relation;
  relation.identifier.relation_id.type=type;
  relation.allowed=true;
  memcpy(&relation.snd, sender, sizeof(union prov_identifier));
  memcpy(&relation.rcv, receiver, sizeof(union prov_identifier));
  return provenance_disclose_relation(&relation);
}

void set_ProvJSON_callback( void (*fcn)(char* json) ){
  init_buffers();
  print_json = fcn;
}

static inline bool __append(char destination[MAX_PROVJSON_BUFFER_LENGTH], char* source){
  if (strlen(source) + 2 > MAX_PROVJSON_BUFFER_LENGTH - strlen(destination) - 1){ // not enough space
    return false;
  }
  // add the comma
  if(destination[0]!='\0')
    strncat(destination, ",", MAX_PROVJSON_BUFFER_LENGTH - strlen(destination) - 1);
  strncat(destination, source, MAX_PROVJSON_BUFFER_LENGTH - strlen(destination) - 1); // copy up to free space
  return true;
}

#define JSON_START "{\"prefix\":{"
#define JSON_ACTIVITY "}, \"activity\":{"
#define JSON_AGENT "}, \"agent\":{"
#define JSON_ENTITY "}, \"entity\":{"
#define JSON_MESSAGE "}, \"message\":{"
#define JSON_USED "}, \"used\":{"
#define JSON_GENERATED "}, \"wasGeneratedBy\":{"
#define JSON_INFORMED "}, \"wasInformedBy\":{"
#define JSON_DERIVED "}, \"wasDerivedFrom\":{"
#define JSON_END "}}"

#define JSON_LENGTH (strlen(JSON_START)\
                      +strlen(JSON_ACTIVITY)\
                      +strlen(JSON_AGENT)\
                      +strlen(JSON_ENTITY)\
                      +strlen(JSON_MESSAGE)\
                      +strlen(JSON_USED)\
                      +strlen(JSON_GENERATED)\
                      +strlen(JSON_INFORMED)\
                      +strlen(JSON_DERIVED)\
                      +strlen(JSON_END)\
                      +strlen(prefix_json())\
                      +strlen(activity)\
                      +strlen(agent)\
                      +strlen(entity)\
                      +strlen(message)\
                      +strlen(used)\
                      +strlen(generated)\
                      +strlen(derived)\
                      +strlen(informed)\
                      +1)

#define str_is_empty(str) (str[0]=='\0')

static inline bool cat_prov(char *json,
                            const char *prefix,
                            char *data,
                            pthread_mutex_t *lock){
  bool rc = false;
  if(!str_is_empty(data)){
    strncat(json, prefix, MAX_PROVJSON_BUFFER_LENGTH);
    strncat(json, data, MAX_PROVJSON_BUFFER_LENGTH);
    memset(data, 0, MAX_PROVJSON_BUFFER_LENGTH);
    rc = true;
  }
  pthread_mutex_unlock(lock);
  return rc;
}

// we create the JSON string to be sent to the call back
static inline char* ready_to_print(){
  char* json;
  bool content=false;

  pthread_mutex_lock(&l_derived);
  pthread_mutex_lock(&l_informed);
  pthread_mutex_lock(&l_generated);
  pthread_mutex_lock(&l_used);
  pthread_mutex_lock(&l_message);
  pthread_mutex_lock(&l_entity);
  pthread_mutex_lock(&l_agent);
  pthread_mutex_lock(&l_activity);

  json = (char*)malloc(JSON_LENGTH * sizeof(char));
  json[0]='\0';

  strncat(json, JSON_START, JSON_LENGTH);
  strncat(json, prefix_json(), JSON_LENGTH);

  content |= cat_prov(json, JSON_ACTIVITY, activity, &l_activity);
  content |= cat_prov(json, JSON_AGENT, agent, &l_agent);
  content |= cat_prov(json, JSON_ENTITY, entity, &l_entity);
  content |= cat_prov(json, JSON_MESSAGE, message, &l_message);
  content |= cat_prov(json, JSON_USED, used, &l_used);
  content |= cat_prov(json, JSON_GENERATED, generated, &l_generated);
  content |= cat_prov(json, JSON_INFORMED, informed, &l_informed);
  content |= cat_prov(json, JSON_DERIVED, derived, &l_derived);

  if(!content){
    free(json);
    return NULL;
  }

  strncat(json, JSON_END, JSON_LENGTH);
  return json;
}

void flush_json(){
  bool should_flush=false;
  char* json;

  pthread_mutex_lock(&l_flush);
  if(!writing_out){
    writing_out = true;
    should_flush = true;
    update_time(); // we update the time
  }
  pthread_mutex_unlock(&l_flush);

  if(should_flush){
    json = ready_to_print();
    if(json!=NULL){
      print_json(json);
      free(json);
    }
    pthread_mutex_lock(&l_flush);
    writing_out = false;
    pthread_mutex_unlock(&l_flush);
  }
}

static inline void json_append(pthread_mutex_t* l, char destination[MAX_PROVJSON_BUFFER_LENGTH], char* source){
  pthread_mutex_lock(l);
  // we cannot append buffer is full, need to print json out
  if(!__append(destination, source)){
    flush_json();
    pthread_mutex_unlock(l);
    json_append(l, destination, source);
    return;
  }
  pthread_mutex_unlock(l);
}

void append_activity(char* json_element){
  json_append(&l_activity, activity, json_element);
}

void append_agent(char* json_element){
  json_append(&l_agent, agent, json_element);
}

void append_entity(char* json_element){
  json_append(&l_entity, entity, json_element);
}

void append_message(char* json_element){
  json_append(&l_message, message, json_element);
}

void append_used(char* json_element){
  json_append(&l_used, used, json_element);
}

void append_generated(char* json_element){
  json_append(&l_generated, generated, json_element);
}

void append_informed(char* json_element){
  json_append(&l_informed, informed, json_element);
}

void append_derived(char* json_element){
  json_append(&l_derived, derived, json_element);
}

static __thread char buffer[MAX_PROVJSON_BUFFER_LENGTH];
#define BUFFER_LENGTH (MAX_PROVJSON_BUFFER_LENGTH-strnlen(buffer, MAX_PROVJSON_BUFFER_LENGTH))

static __thread char id[PROV_ID_STR_LEN];
static __thread char sender[PROV_ID_STR_LEN];
static __thread char receiver[PROV_ID_STR_LEN];
static __thread char parent_id[PROV_ID_STR_LEN];
static __thread char taint[PATH_MAX];

#define RELATION_PREP_IDs(e) ID_ENCODE(e->identifier.buffer, PROV_IDENTIFIER_BUFFER_LENGTH, id, PROV_ID_STR_LEN);\
                        ID_ENCODE(e->snd.buffer, PROV_IDENTIFIER_BUFFER_LENGTH, sender, PROV_ID_STR_LEN);\
                        ID_ENCODE(e->rcv.buffer, PROV_IDENTIFIER_BUFFER_LENGTH, receiver, PROV_ID_STR_LEN)

#define DISC_PREP_IDs(n) ID_ENCODE(n->identifier.buffer, PROV_IDENTIFIER_BUFFER_LENGTH, id, PROV_ID_STR_LEN);\
                        ID_ENCODE(n->parent.buffer, PROV_IDENTIFIER_BUFFER_LENGTH, parent_id, PROV_ID_STR_LEN)

#define NODE_PREP_IDs(n) ID_ENCODE(n->identifier.buffer, PROV_IDENTIFIER_BUFFER_LENGTH, id, PROV_ID_STR_LEN)

#define PACKET_PREP_IDs(p) ID_ENCODE(p->identifier.buffer, PROV_IDENTIFIER_BUFFER_LENGTH, id, PROV_ID_STR_LEN)


static inline void prov_prep_taint(union prov_elt *n){
  taint[0]='\0';
  if (!prov_bloom_empty(prov_taint(n)))
    TAINT_ENCODE(prov_taint(n), PROV_N_BYTES, taint, TAINT_STR_LEN);
}

static inline void __init_json_entry(const char* id)
{
  buffer[0]='\0';
  strncat(buffer, "\"cf:", BUFFER_LENGTH);
  strncat(buffer, id, BUFFER_LENGTH);
  strncat(buffer, "\":{", BUFFER_LENGTH);
}

static inline void __add_attribute(const char* name, bool comma){
  if(comma){
    strncat(buffer, ",\"", BUFFER_LENGTH);
  }else{
    strncat(buffer, "\"", BUFFER_LENGTH);
  }
  strncat(buffer, name, BUFFER_LENGTH);
  strncat(buffer, "\":", BUFFER_LENGTH);
}

static inline void __add_uint32_attribute(const char* name, const uint32_t value, bool comma){
  char tmp[32];
  __add_attribute(name, comma);
  strncat(buffer, utoa(value, tmp, DECIMAL), BUFFER_LENGTH);
}


static inline void __add_int32_attribute(const char* name, const int32_t value, bool comma){
  char tmp[32];
  __add_attribute(name, comma);
  strncat(buffer, itoa(value, tmp, DECIMAL), BUFFER_LENGTH);
}

static inline void __add_uint32hex_attribute(const char* name, const uint32_t value, bool comma){
  char tmp[32];
  __add_attribute(name, comma);
  strncat(buffer, "\"0x", BUFFER_LENGTH);
  strncat(buffer, utoa(value, tmp, HEX), BUFFER_LENGTH);
  strncat(buffer, "\"", BUFFER_LENGTH);
}

static inline void __add_uint64_attribute(const char* name, const uint64_t value, bool comma){
  char tmp[64];
  __add_attribute(name, comma);
  strncat(buffer, "\"", BUFFER_LENGTH);
  strncat(buffer, ulltoa(value, tmp, DECIMAL), BUFFER_LENGTH);
  strncat(buffer, "\"", BUFFER_LENGTH);
}

static inline void __add_uint64hex_attribute(const char* name, const uint64_t value, bool comma){
  char tmp[64];
  __add_attribute(name, comma);
  strncat(buffer, "\"", BUFFER_LENGTH);
  strncat(buffer, ulltoa(value, tmp, HEX), BUFFER_LENGTH);
  strncat(buffer, "\"", BUFFER_LENGTH);
}

static inline void __add_int64_attribute(const char* name, const int64_t value, bool comma){
  char tmp[64];
  __add_attribute(name, comma);
  strncat(buffer, "\"", BUFFER_LENGTH);
  strncat(buffer, lltoa(value, tmp, DECIMAL), BUFFER_LENGTH);
  strncat(buffer, "\"", BUFFER_LENGTH);
}

static inline void __add_string_attribute(const char* name, const char* value, bool comma){
  if(value[0]=='\0'){ // value is not set
    return;
  }
  __add_attribute(name, comma);
  strncat(buffer, "\"", BUFFER_LENGTH);
  strncat(buffer, value, BUFFER_LENGTH);
  strncat(buffer, "\"", BUFFER_LENGTH);
}

static inline void __add_machine_id(uint32_t value, bool comma){
  char tmp[32];
  __add_attribute("cf:machine_id", comma);
  strncat(buffer, "\"cf:", BUFFER_LENGTH);
  strncat(buffer, utoa(value, tmp, DECIMAL), BUFFER_LENGTH);
  strncat(buffer, "\"", BUFFER_LENGTH);
}

static inline void __add_reference(const char* name, const char* id, bool comma){
  if(id[0]=='\0'){ // value is not set
    return;
  }
  __add_attribute(name, comma);
  strncat(buffer, "\"cf:", BUFFER_LENGTH);
  strncat(buffer, id, BUFFER_LENGTH);
  strncat(buffer, "\"", BUFFER_LENGTH);
}


static inline void __add_json_attribute(const char* name, const char* value, bool comma){
  __add_attribute(name, comma);
  strncat(buffer, value, BUFFER_LENGTH);
}

static inline void __add_date_attribute(bool comma){
  __add_attribute("cf:date", comma);
  strncat(buffer, "\"", BUFFER_LENGTH);
  pthread_rwlock_rdlock(&date_lock);
  strncat(buffer, date, BUFFER_LENGTH);
  pthread_rwlock_unlock(&date_lock);
  strncat(buffer, "\"", BUFFER_LENGTH);
}

static inline void __add_label_attribute(const char* type, const char* text, bool comma){
  __add_attribute("prov:label", comma);
  if(type!=NULL){
    strncat(buffer, "\"[", BUFFER_LENGTH);
    strncat(buffer, type, BUFFER_LENGTH);
    strncat(buffer, "] ", BUFFER_LENGTH);
  }else{
    strncat(buffer, "\"", BUFFER_LENGTH);
  }
  if(text!=NULL)
    strncat(buffer, text, BUFFER_LENGTH);
  strncat(buffer, "\"", BUFFER_LENGTH);
}



static inline void __add_ipv4(uint32_t ip, uint32_t port){
    char tmp[8];
    strncat(buffer, uint32_to_ipv4str(ip), BUFFER_LENGTH);
    strncat(buffer, ":", BUFFER_LENGTH);
    strncat(buffer, utoa(htons(port), tmp, DECIMAL), BUFFER_LENGTH);
}

static inline void __add_ipv4_attribute(const char* name, const uint32_t ip, const uint32_t port, bool comma){
  char tmp[64];
  __add_attribute(name, comma);
  strncat(buffer, "\"", BUFFER_LENGTH);
  __add_ipv4(ip, port);
  strncat(buffer, "\"", BUFFER_LENGTH);
}

static inline void __close_json_entry(char* buffer)
{
  strncat(buffer, "}", BUFFER_LENGTH);
}

static inline void __node_identifier(const struct node_identifier* n){
  __add_uint64_attribute("cf:id", n->id, false);
  __add_string_attribute("prov:type", node_id_to_str(n->type), true);
  __add_uint32_attribute("cf:boot_id", n->boot_id, true);
  __add_machine_id(n->machine_id, true);
  __add_uint32_attribute("cf:version", n->version, true);
}

static inline void __node_start(const char* id,
                                const struct node_identifier* n,
                                const char* taint,
                                uint64_t jiffies){
  __init_json_entry(id);
  __node_identifier(n);
  __add_date_attribute(true);
  __add_string_attribute("cf:taint", taint, true);
  __add_uint64_attribute("cf:jiffies", jiffies, true);
}

static inline void __relation_identifier(const struct relation_identifier* e){
  __add_uint64_attribute("cf:id", e->id, false);
  __add_string_attribute("prov:type", relation_id_to_str(e->type), true);
  __add_uint32_attribute("cf:boot_id", e->boot_id, true);
  __add_machine_id(e->machine_id, true);
}

static char* __relation_to_json(struct relation_struct* e, const char* snd, const char* rcv){
  RELATION_PREP_IDs(e);
  prov_prep_taint((union prov_elt*)e);
  __init_json_entry(id);
  __relation_identifier(&(e->identifier.relation_id));
  __add_date_attribute(true);
  __add_string_attribute("cf:taint", taint, true);
  __add_uint64_attribute("cf:jiffies", e->jiffies, true);
  __add_label_attribute(NULL, relation_id_to_str(e->identifier.relation_id.type), true);
  if(e->allowed==FLOW_ALLOWED)
    __add_string_attribute("cf:allowed", "true", true);
  else
    __add_string_attribute("cf:allowed", "false", true);
  __add_reference(snd, sender, true);
  __add_reference(rcv, receiver, true);
  if(e->set==FILE_INFO_SET && e->offset>0)
    __add_int64_attribute("cf:offset", e->offset, true); // just offset for now
  __add_uint64hex_attribute("cf:flags", e->flags, true);
  __close_json_entry(buffer);
  return buffer;
}

char* used_to_json(struct relation_struct* e){
  return __relation_to_json(e, "prov:entity", "prov:activity");
}

char* generated_to_json(struct relation_struct* e){
  return __relation_to_json(e, "prov:activity", "prov:entity");
}

char* informed_to_json(struct relation_struct* e){
  return __relation_to_json(e, "prov:informant", "prov:informed");
}

char* derived_to_json(struct relation_struct* e){
  return __relation_to_json(e, "prov:usedEntity", "prov:generatedEntity");
}

char* disc_to_json(struct disc_node_struct* n){
  DISC_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  __add_reference("cf:hasParent", parent_id, true);
  if(n->length > 0){
    strncat(buffer, ",", BUFFER_LENGTH);
    strncat(buffer, n->content, BUFFER_LENGTH);
  }
  __close_json_entry(buffer);
  return buffer;
}

char* task_to_json(struct task_prov_struct* n){
  char tmp[33];
  char secctx[PATH_MAX];
  provenance_secid_to_secctx(n->secid, secctx, PATH_MAX);
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  __add_uint32_attribute("cf:uid", n->uid, true);
  __add_uint32_attribute("cf:gid", n->gid, true);
  __add_uint32_attribute("cf:pid", n->pid, true);
  __add_uint32_attribute("cf:vpid", n->vpid, true);
  __add_uint32_attribute("cf:ppid", n->ppid, true);
  __add_uint32_attribute("cf:tgid", n->tgid, true);
  __add_uint32_attribute("cf:utsns", n->utsns, true);
  __add_uint32_attribute("cf:ipcns", n->ipcns, true);
  __add_uint32_attribute("cf:mntns", n->mntns, true);
  __add_uint32_attribute("cf:pidns", n->pidns, true);
  __add_uint32_attribute("cf:netns", n->netns, true);
  __add_uint32_attribute("cf:cgroupns", n->cgroupns, true);
  __add_string_attribute("cf:secctx", secctx, true);
  __add_uint64_attribute("cf:utime", n->utime, true);
  __add_uint64_attribute("cf:stime", n->stime, true);
  __add_uint64_attribute("cf:vm", n->vm, true);
  __add_uint64_attribute("cf:rss", n->rss, true);
  __add_uint64_attribute("cf:hw_vm", n->hw_vm, true);
  __add_uint64_attribute("cf:hw_rss", n->hw_rss, true);
  __add_uint64_attribute("cf:rbytes", n->rbytes, true);
  __add_uint64_attribute("cf:wbytes", n->wbytes, true);
  __add_uint64_attribute("cf:cancel_wbytes", n->cancel_wbytes, true);
  __add_label_attribute("task", utoa(n->identifier.node_id.version, tmp, DECIMAL), true);
  __close_json_entry(buffer);
  return buffer;
}

#define UUID_STR_SIZE 37
char* uuid_to_str(uint8_t* uuid, char* str, size_t size){
  if(size<37){
    snprintf(str, size, "UUID-ERROR");
    return str;
  }
  snprintf(str, size, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    uuid[0], uuid[1], uuid[2], uuid[3]
    , uuid[4], uuid[5]
    , uuid[6], uuid[7]
    , uuid[8], uuid[9]
    , uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return str;
}

static const char STR_UNKNOWN[]= "unknown";
static const char STR_BLOCK_SPECIAL[]= "block special";
static const char STR_CHAR_SPECIAL[]= "char special";
static const char STR_DIRECTORY[]= "directory";
static const char STR_FIFO[]= "fifo";
static const char STR_LINK[]= "link";
static const char STR_FILE[]= "file";
static const char STR_SOCKET[]= "socket";

static inline const char* get_inode_type(mode_t mode){
  if(S_ISBLK(mode))
    return STR_BLOCK_SPECIAL;
  else if(S_ISCHR(mode))
    return STR_CHAR_SPECIAL;
  else if(S_ISDIR(mode))
    return STR_DIRECTORY;
  else if(S_ISFIFO(mode))
    return STR_FIFO;
  else if(S_ISLNK(mode))
    return STR_LINK;
  else if(S_ISREG(mode))
    return STR_FILE;
  else if(S_ISSOCK(mode))
    return STR_SOCKET;
  return STR_UNKNOWN;
}


char* inode_to_json(struct inode_prov_struct* n){
  char uuid[UUID_STR_SIZE];
  char tmp[65];
  char secctx[PATH_MAX];
  provenance_secid_to_secctx(n->secid, secctx, PATH_MAX);
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  __add_uint32_attribute("cf:uid", n->uid, true);
  __add_uint32_attribute("cf:gid", n->gid, true);
  __add_uint32hex_attribute("cf:mode", n->mode, true);
  __add_string_attribute("cf:secctx", secctx, true);
  __add_uint32_attribute("cf:ino", n->ino, true);
  __add_string_attribute("cf:uuid", uuid_to_str(n->sb_uuid, uuid, UUID_STR_SIZE), true);
  __add_label_attribute(node_id_to_str(n->identifier.node_id.type), utoa(n->identifier.node_id.version, tmp, DECIMAL), true);
  __close_json_entry(buffer);
  return buffer;
}

char* iattr_to_json(struct iattr_prov_struct* n){
  char tmp[65];
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  __add_uint32hex_attribute("cf:valid", n->valid, true);
  __add_uint32hex_attribute("cf:mode", n->mode, true);
  __add_uint32_attribute("cf:uid", n->uid, true);
  __add_uint32_attribute("cf:gid", n->gid, true);
  __add_int64_attribute("cf:size", n->size, true);
  __add_int64_attribute("cf:atime", n->atime, true);
  __add_int64_attribute("cf:ctime", n->ctime, true);
  __add_int64_attribute("cf:mtime", n->mtime, true);
  __add_label_attribute("iattr", utoa(n->identifier.node_id.id, tmp, DECIMAL), true);
  __close_json_entry(buffer);
  return buffer;
}

char* xattr_to_json(struct xattr_prov_struct* n){
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  __add_string_attribute("cf:name", n->name, true);
  if(n->size>0){
    __add_uint32_attribute("cf:size", n->size, true);
    // TODO record value when present
  }
  __add_label_attribute("xattr", n->name, true);
  __close_json_entry(buffer);
  return buffer;
}

char* pckcnt_to_json(struct pckcnt_struct* n){
  char* cntenc;
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  cntenc = malloc( encode64Bound(n->length) );
  base64encode(n->content, n->length, cntenc, encode64Bound(n->length));
  __add_string_attribute("cf:content", cntenc, true);
  free(cntenc);
  __add_uint32_attribute("cf:length", n->length, true);
  if(n->truncated==PROV_TRUNCATED)
    __add_string_attribute("cf:truncated", "true", true);
  else
    __add_string_attribute("cf:truncated", "false", true);
  __add_label_attribute("content", NULL, true);
  __close_json_entry(buffer);
  return buffer;
}

char* sb_to_json(struct sb_struct* n){
  char uuid[UUID_STR_SIZE];
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  __add_string_attribute("cf:uuid", uuid_to_str(n->uuid, uuid, UUID_STR_SIZE), true);
  __close_json_entry(buffer);
  return buffer;
}

char* msg_to_json(struct msg_msg_struct* n){
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  __close_json_entry(buffer);
  return buffer;
}

char* shm_to_json(struct shm_struct* n){
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  __add_uint32hex_attribute("cf:mode", n->mode, true);
  __close_json_entry(buffer);
  return buffer;
}

char* packet_to_json(struct pck_struct* p){
  char tmp[256];
  PACKET_PREP_IDs(p);
  prov_prep_taint((union prov_elt*)p);
  __init_json_entry(id);
  __add_uint32_attribute("cf:id", p->identifier.packet_id.id, false);
  __add_uint32_attribute("cf:seq", p->identifier.packet_id.seq, true);
  __add_ipv4_attribute("cf:sender", p->identifier.packet_id.snd_ip, p->identifier.packet_id.snd_port, true);
  __add_ipv4_attribute("cf:receiver", p->identifier.packet_id.rcv_ip, p->identifier.packet_id.rcv_port, true);
  __add_string_attribute("prov:type", "packet", true);
  __add_string_attribute("cf:taint", taint, true);
  __add_uint64_attribute("cf:jiffies", p->jiffies, true);
  strncat(buffer, ",\"prov:label\":\"[packet] ", BUFFER_LENGTH);
  __add_ipv4(p->identifier.packet_id.snd_ip, p->identifier.packet_id.snd_port);
  strncat(buffer, "->", BUFFER_LENGTH);
  __add_ipv4(p->identifier.packet_id.rcv_ip, p->identifier.packet_id.rcv_port);
  strncat(buffer, " (", BUFFER_LENGTH);
  strncat(buffer, utoa(p->identifier.packet_id.id, tmp, DECIMAL), BUFFER_LENGTH);
  strncat(buffer, ")\"", BUFFER_LENGTH);
  __close_json_entry(buffer);
  return buffer;
}

char* str_msg_to_json(struct str_struct* n){
  int i=0;
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  for(i=0; i < n->length; i++){
    if(n->str[i]=='"')
      n->str[i]=' ';
    if(n->str[i]<32 || n->str[i]>125)
      n->str[i]='_';
  }
  __add_string_attribute("cf:log", n->str, true);
  __add_label_attribute("log", n->str, true);
  __close_json_entry(buffer);
  return buffer;
}

char* sockaddr_to_json(char* buf, size_t blen, struct sockaddr* addr, size_t length){
  char host[NI_MAXHOST];
  char serv[NI_MAXSERV];

  if(addr->sa_family == AF_INET){
    getnameinfo(addr, length, host, NI_MAXHOST, serv, NI_MAXSERV, 0);
    snprintf(buf, blen, "{\"type\":\"AF_INET\", \"host\":\"%s\", \"serv\":\"%s\"}", host, serv);
  }else if(addr->sa_family == AF_INET6){
    getnameinfo(addr, length, host, NI_MAXHOST, serv, NI_MAXSERV, 0);
    snprintf(buf, blen, "{\"type\":\"AF_INET6\", \"host\":\"%s\", \"serv\":\"%s\"}", host, serv);
  }else if(addr->sa_family == AF_UNIX){
    snprintf(buf, blen, "{\"type\":\"AF_UNIX\", \"path\":\"%s\"}", ((struct sockaddr_un*)addr)->sun_path);
  }else{
    snprintf(buf, blen, "{\"type\":\"OTHER\"}");
  }

  return buf;
}

char* sockaddr_to_label(char* buf, size_t blen, struct sockaddr* addr, size_t length){
  char host[NI_MAXHOST];
  char serv[NI_MAXSERV];

  if(addr->sa_family == AF_INET){
    getnameinfo(addr, length, host, NI_MAXHOST, serv, NI_MAXSERV, 0);
    snprintf(buf, blen, "IPV4 %s", host);
  }else if(addr->sa_family == AF_INET6){
    getnameinfo(addr, length, host, NI_MAXHOST, serv, NI_MAXSERV, 0);
    snprintf(buf, blen, "IPV6 %s", host);
  }else if(addr->sa_family == AF_UNIX){
    snprintf(buf, blen, "UNIX %s", ((struct sockaddr_un*)addr)->sun_path);
  }else{
    snprintf(buf, blen, "OTHER");
  }

  return buf;
}

char* addr_to_json(struct address_struct* n){
  char addr_info[PATH_MAX+1024];
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  __add_json_attribute("cf:address", sockaddr_to_json(addr_info, PATH_MAX+1024, &n->addr, n->length), true);
  __add_label_attribute("address", sockaddr_to_label(addr_info, PATH_MAX+1024, &n->addr, n->length), true);
  __close_json_entry(buffer);
  return buffer;
}

char* pathname_to_json(struct file_name_struct* n){
  int i;
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  for(i=0; i<n->length; i++){
    if(n->name[i]=='\\')
      n->name[i]='/';
  }
  __add_string_attribute("cf:pathname", n->name, true);
  __add_label_attribute("path", n->name, true);
  __close_json_entry(buffer);
  return buffer;
}

char* arg_to_json(struct arg_struct* n){
  int i;
  char* tmp;
  NODE_PREP_IDs(n);
  prov_prep_taint((union prov_elt*)n);
  __node_start(id, &(n->identifier.node_id), taint, n->jiffies);
  for(i=0; i<n->length; i++){
    if(n->value[i]=='\\')
      n->value[i]='/';
    if(n->value[i]=='\n')
      n->value[i]=' ';
    if(n->value[i]=='\t')
      n->value[i]=' ';
  }
  tmp = repl_str(n->value, "\"", "\\\"");
  if(tmp==NULL)
    tmp = n->value;
  __add_string_attribute("cf:value", tmp, true);
  if(n->truncated==PROV_TRUNCATED)
    __add_string_attribute("cf:truncated", "true", true);
  else
    __add_string_attribute("cf:truncated", "false", true);
  if(n->identifier.node_id.type == ENT_ARG)
    __add_label_attribute("argv", tmp, true);
  else
    __add_label_attribute("envp", tmp, true);
  __close_json_entry(buffer);
  if(tmp != n->value)
    free(tmp);
  return buffer;
}

#define LSM_LIST "/sys/kernel/security/lsm"

char* machine_description_json(char* buffer){
  char tmp[64];
  uint32_t machine_id;
  struct utsname machine_info;
  int lsm_fd;
  char lsm_list[2048];

  memset(lsm_list, 0, 2048);

  provenance_get_machine_id(&machine_id);
  uname(&machine_info);

  lsm_fd = open(LSM_LIST, O_RDONLY);
  read(lsm_fd, lsm_list, 2048);

  buffer[0]='\0';
  strncat(buffer, "{\"prefix\":{", BUFFER_LENGTH);
  strncat(buffer, prefix_json(), BUFFER_LENGTH);
  strncat(buffer, "}", BUFFER_LENGTH);
  strncat(buffer, ",\"entity\":{", BUFFER_LENGTH);
  strncat(buffer, "\"cf:", BUFFER_LENGTH);
  strncat(buffer, utoa(machine_id, tmp, DECIMAL), BUFFER_LENGTH);
  strncat(buffer, "\":{", BUFFER_LENGTH);
  strncat(buffer, "\"prov:label\":\"[machine] ", BUFFER_LENGTH);
  strncat(buffer, utoa(machine_id, tmp, DECIMAL), BUFFER_LENGTH);
  strncat(buffer, "\",\"cf:camflow\":\"", BUFFER_LENGTH);
  provenance_version(tmp, 64);
  strncat(buffer, tmp, BUFFER_LENGTH);
  strncat(buffer, "\",\"cf:libprovenance\":\"", BUFFER_LENGTH);
  provenance_lib_version(tmp, 64);
  strncat(buffer, tmp, BUFFER_LENGTH);
  strncat(buffer, "\",\"cf:sysname\":\"", BUFFER_LENGTH);
  strncat(buffer, machine_info.sysname, BUFFER_LENGTH);
  strncat(buffer, "\",\"cf:nodename\":\"", BUFFER_LENGTH);
  strncat(buffer, machine_info.nodename, BUFFER_LENGTH);
  strncat(buffer, "\",\"cf:release\":\"", BUFFER_LENGTH);
  strncat(buffer, machine_info.release, BUFFER_LENGTH);
  strncat(buffer, "\",\"cf:version\":\"", BUFFER_LENGTH);
  strncat(buffer, machine_info.version, BUFFER_LENGTH);
  strncat(buffer, "\",\"cf:machine\":\"", BUFFER_LENGTH);
  strncat(buffer, machine_info.machine, BUFFER_LENGTH);
  strncat(buffer, "\",\"cf:lsm_list\":\"", BUFFER_LENGTH);
  strncat(buffer, lsm_list, BUFFER_LENGTH);
  strncat(buffer, "\", \"cf:date", BUFFER_LENGTH);
  strncat(buffer, "\":\"", BUFFER_LENGTH);
  update_time();
  pthread_rwlock_rdlock(&date_lock);
  strncat(buffer, date, BUFFER_LENGTH);
  pthread_rwlock_unlock(&date_lock);
  strncat(buffer, "\"}}}", BUFFER_LENGTH);
  return buffer;
}
