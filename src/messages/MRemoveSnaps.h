// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef __MREMOVESNAPS_H
#define __MREMOVESNAPS_H

#include "messages/PaxosServiceMessage.h"

struct MRemoveSnaps : public PaxosServiceMessage {
  map<int, vector<snapid_t> > snaps;
  
  MRemoveSnaps() : 
    PaxosServiceMessage(MSG_REMOVE_SNAPS, 0) { }
  MRemoveSnaps(map<int, vector<snapid_t> >& s) : 
    PaxosServiceMessage(MSG_REMOVE_SNAPS, 0) {
    snaps.swap(s);
  }
private:
  ~MRemoveSnaps() {}

public:
  const char *get_type_name() { return "remove_snaps"; }
  void print(ostream& out) {
    out << "remove_snaps(" << snaps << " v" << version << ")";
  }

  void encode_payload() {
    paxos_encode();
    ::encode(snaps, payload);
  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    paxos_decode(p);
    ::decode(snaps, p);
    assert(p.end());
  }

};

#endif
