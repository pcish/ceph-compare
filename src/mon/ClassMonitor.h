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

#ifndef __CLASSMONITOR_H
#define __CLASSMONITOR_H

#include <map>
#include <set>
using namespace std;

#include "include/types.h"
#include "msg/Messenger.h"
#include "PaxosService.h"
#include "mon/Monitor.h"

#include "include/ClassLibrary.h"

class MMonCommand;
class MClass;

class ClassMonitor : public PaxosService {
  void class_usage(stringstream& ss);
private:
  multimap<utime_t,ClassLibraryIncremental> pending_class;
  ClassLibrary pending_list, list;

  void create_initial(bufferlist& bl);
  bool update_from_paxos();
  void create_pending();  // prepare a new pending
  void encode_pending(bufferlist &bl);  // propose pending update to peers

  void committed();

  bool preprocess_query(PaxosServiceMessage *m);  // true if processed.
  bool prepare_update(PaxosServiceMessage *m);

  bool preprocess_class(MClass *m);
  bool prepare_class(MClass *m);
  void _updated_class(MClass *m);

  struct C_Class : public Context {
    ClassMonitor *classmon;
    MClass *ack;
    C_Class(ClassMonitor *p, MClass *a) : classmon(p), ack(a) {}
    void finish(int r) {
      classmon->_updated_class(ack);
    }    
  };

  bool preprocess_command(MMonCommand *m);
  bool prepare_command(MMonCommand *m);
  bool store_impl(ClassInfo& info, ClassImpl& impl);
 public:
  ClassMonitor(Monitor *mn, Paxos *p) : PaxosService(mn, p) { }
  void handle_request(MClass *m);
  
  void tick();  // check state, take actions
};

#endif
