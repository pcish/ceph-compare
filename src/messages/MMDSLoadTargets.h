// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2009 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef __MMDSLoadTargets_H
#define __MMDSLoadTargets_H

#include "msg/Message.h"
#include "include/types.h"

#include <map>
using std::map;

class MMDSLoadTargets : public Message {
 public:
  set<int32_t> targets;

  MMDSLoadTargets() : Message(MSG_MDS_OFFLOAD_TARGETS) {}

  MMDSLoadTargets(set<int32_t>& mds_targets) :
    Message(MSG_MDS_OFFLOAD_TARGETS),
    targets(mds_targets) {}

  const char* get_type_name() { return "mds_load_targets"; }
  void print(ostream& o) {
    o << "mds_load_targets(" << targets << ")";
  }

  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    ::decode(targets, p);
  }

  void encode_payload() {
    ::encode(targets, payload);
  }
};

#endif