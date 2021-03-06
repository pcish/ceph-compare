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

#ifndef __MDSTABLECLIENT_H
#define __MDSTABLECLIENT_H

#include "include/types.h"
#include "include/Context.h"
#include "mds_table_types.h"

class MDS;
class LogSegment;
class MMDSTableRequest;

class MDSTableClient {
protected:
  MDS *mds;
  int table;

  uint64_t last_reqid;

  // prepares
  struct _pending_prepare {
    Context *onfinish;
    version_t *ptid;
    bufferlist *pbl; 
    bufferlist mutation;

    _pending_prepare() : onfinish(0), ptid(0), pbl(0) {}
  };

  map<uint64_t, _pending_prepare> pending_prepare;

  // pending commits
  map<version_t, LogSegment*> pending_commit;
  map<version_t, list<Context*> > ack_waiters;

  void handle_reply(class MMDSTableQuery *m);  

  class C_LoggedAck : public Context {
    MDSTableClient *tc;
    version_t tid;
  public:
    C_LoggedAck(MDSTableClient *a, version_t t) : tc(a), tid(t) {}
    void finish(int r) {
      tc->_logged_ack(tid);
    }
  };
  void _logged_ack(version_t tid);

public:
  MDSTableClient(MDS *m, int tab) : mds(m), table(tab), last_reqid(0) {}
  virtual ~MDSTableClient() {}

  void handle_request(MMDSTableRequest *m);

  void _prepare(bufferlist& mutation, version_t *ptid, bufferlist *pbl, Context *onfinish);
  void commit(version_t tid, LogSegment *ls);

  // for recovery (by other nodes)
  void handle_mds_recovery(int mds); // called when someone else recovers
  void resend_commits();

  // for recovery (by me)
  void got_journaled_agree(version_t tid, LogSegment *ls);
  void got_journaled_ack(version_t tid);

  bool has_committed(version_t tid) {
    return pending_commit.count(tid) == 0;
  }
  void wait_for_ack(version_t tid, Context *c) {
    ack_waiters[tid].push_back(c);
  }
  void finish_recovery();                // called when i recover and go active


  // child must implement
  virtual void resend_queries() = 0;
  virtual void handle_query_result(MMDSTableRequest *m) = 0;

  // and friendly front-end for _prepare.

};

#endif
