/*
* CamFlow userspace provenance tool
*
* Author: Thomas Pasquier <tfjmp@seas.harvard.edu>
*
* Copyright (C) 2016 Harvard University
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2, as
* published by the Free Software Foundation.
*
*/
#define _XOPEN_SOURCE 500
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/camflow.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "provenancelib.h"
#include "provenancefilter.h"
#include "provenanceutils.h"

void usage( void ){
  printf("-h usage.\n");
  printf("-s print provenance capture state.\n");
  printf("-e <bool> enable/disable provenance capture.\n");
  printf("-a <bool> activate/deactivate whole-system provenance capture.\n");
  printf("-d <bool> activate/deactivate directories provenance capture.\n");
  printf("-f <filename> display provenance info of a file.\n");
  printf("-t <filename> <bool> [depth] activate/deactivate tracking of a file.\n");
  printf("-o <filename> <bool> mark/unmark a file as opaque.\n");
}

#define is_str_true(str) ( strcmp (str, "true") == 0)
#define is_str_false(str) ( strcmp (str, "false") == 0)

void enable( const char* str ){
  if(!is_str_true(str) && !is_str_false(str)){
    printf("Excepted a boolean, got %s.\n", str);
    return;
  }

  if(provenance_set_enable(is_str_true(str))<0)
    perror("Could not enable/disable provenance capture");
}

void all( const char* str ){
  if(!is_str_true(str) && !is_str_false(str)){
    printf("Excepted a boolean, got %s.\n", str);
    return;
  }

  if(provenance_set_all(is_str_true(str))<0)
    perror("Could not activate/deactivate whole-system provenance capture");
}

void dir( const char* str ){
  int err;
  if(!is_str_true(str) && !is_str_false(str)){
    printf("Excepted a boolean, got %s.\n", str);
    return;
  }

  if(is_str_true(str)){
    err = provenance_add_node_filter(MSG_INODE_DIRECTORY);
  }else{
    err = provenance_remove_node_filter(MSG_INODE_DIRECTORY);
  }

  if(err<0){
    perror("Could not activate/deactivate directories provenance capture");
  }
}

void state( void ){
  uint32_t filter=0;
  printf("Provenance capture:\n");
  if(provenance_get_enable()){
    printf("- capture enabled;\n");
  }else{
    printf("- capture disabled;\n");
  }
  if( provenance_get_all() ){
    printf("- all enabled;\n");
  }else{
    printf("- all disabled;\n");
  }

  provenance_get_node_filter(&filter);
  printf("\nNode filter (%0x):\n", filter);
  if( (filter&MSG_INODE_DIRECTORY) == 0 ){
    printf("- directories provenance captured;\n");
  }else{
    printf("- directories provenance not captured;\n");
  }
}

void print_version(){
  printf("CamFlow %s\n", CAMFLOW_VERSION_STR);
}

void file( const char* path){
  struct inode_prov_struct inode_info;
  char id[PROV_ID_STR_LEN];
  int err;

  err = provenance_read_file(path, &inode_info);
  if(err < 0){
    perror("Could not read file provenance information.\n");
  }

  ID_ENCODE(inode_info.identifier.buffer, PROV_IDENTIFIER_BUFFER_LENGTH, id, PROV_ID_STR_LEN);
  printf("Identifier: %s\n", id);
  printf("Type: %u\n", inode_info.identifier.relation_id.type);
  printf("ID: %lu\n", inode_info.identifier.relation_id.id);
  printf("Boot ID: %u\n", inode_info.identifier.relation_id.boot_id);
  printf("Machine ID: %u\n", inode_info.identifier.relation_id.machine_id);
  printf("\n");
  if(inode_info.node_kern.tracked == NODE_TRACKED){
    printf("File is tracked.\n");
  }else{
    printf("File is not tracked.\n");
  }
  if(inode_info.node_kern.opaque == NODE_OPAQUE){
    printf("File is opaque.\n");
  }else{
    printf("File is not opaque.\n");
  }
  printf("Propagate: %u\n", inode_info.node_kern.propagate);
}

#define CHECK_ATTR_NB(argc, min) if(argc < min){ usage();exit(-1);}

int main(int argc, char *argv[]){
  int err;
  tag_t tag;

  CHECK_ATTR_NB(argc, 2);
  // do it properly, but that will do for now
  switch(argv[1][1]){
    case 'h':
      usage();
      break;
    case 'v':
      print_version();
      break;
    case 's':
      state();
      break;
    case 'e':
      CHECK_ATTR_NB(argc, 3);
      enable(argv[2]);
      break;
    case 'a':
      CHECK_ATTR_NB(argc, 3);
      all(argv[2]);
      break;
    case 'd':
      CHECK_ATTR_NB(argc, 3);
      dir(argv[2]);
      break;
    case 'f':
      CHECK_ATTR_NB(argc, 3);
      file(argv[2]);
      break;
    case 't':
      CHECK_ATTR_NB(argc, 4);
      if(argc==4){ // no depth specified
        err = provenance_track_file(argv[2], is_str_true(argv[3]), 1);
      }else{
        err = provenance_track_file(argv[2], is_str_true(argv[3]), atoi(argv[4]));
      }
      if(err < 0){
        perror("Could not change tracking settings for this file.\n");
      }
      break;
    case 'o':
      CHECK_ATTR_NB(argc, 4);
      err = provenance_opaque_file(argv[2], is_str_true(argv[3]));
      if(err < 0){
        perror("Could not change opacity settings for this file.\n");
      }
      break;
    default:
      usage();
  }
  return 0;
}