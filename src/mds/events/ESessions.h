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

#ifndef __MDS_ESESSIONS_H
#define __MDS_ESESSIONS_H

#include "config.h"
#include "include/types.h"

#include "../LogEvent.h"

class ESessions : public LogEvent {
protected:
  version_t cmapv;  // client map version

public:
  map<client_t,entity_inst_t> client_map;

  ESessions() : LogEvent(EVENT_SESSION) { }
  ESessions(version_t v) :
    LogEvent(EVENT_SESSION),
    cmapv(v) {
  }
  
  void encode(bufferlist &bl) const {
    ::encode(client_map, bl);
    ::encode(cmapv, bl);
  }
  void decode(bufferlist::iterator &bl) {
    ::decode(client_map, bl);
    ::decode(cmapv, bl);
  }


  void print(ostream& out) {
    out << "ESessions " << client_map.size() << " opens cmapv " << cmapv;
  }
  
  void update_segment();
  void replay(MDS *mds);  
};

#endif
