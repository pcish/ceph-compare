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


#include "LogMonitor.h"
#include "Monitor.h"
#include "MonitorStore.h"

#include "messages/MMonCommand.h"
#include "messages/MLog.h"

#include "common/Timer.h"

#include "osd/osd_types.h"
#include "osd/PG.h"  // yuck

#include "config.h"
#include <sstream>

#define DOUT_SUBSYS mon
#undef dout_prefix
#define dout_prefix _prefix(mon, paxos->get_version())
static ostream& _prefix(Monitor *mon, version_t v) {
  return *_dout << dbeginl
		<< "mon" << mon->whoami
		<< (mon->is_starting() ? (const char*)"(starting)":(mon->is_leader() ? (const char*)"(leader)":(mon->is_peon() ? (const char*)"(peon)":(const char*)"(?\?)")))
		<< ".log v" << v << " ";
}

ostream& operator<<(ostream& out, LogMonitor& pm)
{
  std::stringstream ss;
  /*
  for (hash_map<int,int>::iterator p = pm.pg_map.num_pg_by_state.begin();
       p != pm.pg_map.num_pg_by_state.end();
       ++p) {
    if (p != pm.pg_map.num_pg_by_state.begin())
      ss << ", ";
    ss << p->second << " " << pg_state_string(p->first);
  }
  string states = ss.str();
  return out << "v" << pm.pg_map.version << ": "
	     << pm.pg_map.pg_stat.size() << " pgs: "
	     << states << "; "
	     << kb_t(pm.pg_map.total_pg_kb()) << " data, " 
	     << kb_t(pm.pg_map.total_used_kb()) << " used, "
	     << kb_t(pm.pg_map.total_avail_kb()) << " / "
	     << kb_t(pm.pg_map.total_kb()) << " free";
  */
  return out << "log";
}

/*
 Tick function to update the map based on performance every N seconds
*/

void LogMonitor::tick() 
{
  if (!paxos->is_active()) return;

  update_from_paxos();
  dout(10) << *this << dendl;

  if (!mon->is_leader()) return; 

}

void LogMonitor::create_initial()
{
  dout(10) << "create_initial -- creating initial map" << dendl;
  LogEntry e;
  memset(&e.who, 0, sizeof(e.who));
  e.stamp = g_clock.now();
  e.type = LOG_INFO;
  e.msg = "mkfs";
  e.seq = 0;
  stringstream ss;
  ss << e;
  string s;
  getline(ss, s);
  pending_inc.append(s);
  pending_inc.append("\n");
}

bool LogMonitor::update_from_paxos()
{
  return true;
}

void LogMonitor::create_pending()
{
  pending_inc.clear();
  dout(10) << "create_pending v " << (paxos->get_version() + 1) << dendl;
}

void LogMonitor::encode_pending(bufferlist &bl)
{
  dout(10) << "encode_pending v " << (paxos->get_version() + 1) << dendl;
  bl = pending_inc;
}

bool LogMonitor::preprocess_query(Message *m)
{
  dout(10) << "preprocess_query " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_MON_COMMAND:
    return preprocess_command((MMonCommand*)m);

  case MSG_LOG:
    return preprocess_log((MLog*)m);

  default:
    assert(0);
    delete m;
    return true;
  }
}

bool LogMonitor::prepare_update(Message *m)
{
  dout(10) << "prepare_update " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_MON_COMMAND:
    return prepare_command((MMonCommand*)m);
  case MSG_LOG:
    return prepare_log((MLog*)m);
  default:
    assert(0);
    delete m;
    return false;
  }
}

void LogMonitor::committed()
{

}

bool LogMonitor::preprocess_log(MLog *m)
{
  return false;
}

bool LogMonitor::prepare_log(MLog *m) 
{
  dout(10) << "prepare_log " << *m << " from " << m->get_orig_source() << dendl;

  if (!ceph_fsid_equal(&m->fsid, &mon->monmap->fsid)) {
    dout(0) << "handle_log on fsid " << m->fsid << " != " << mon->monmap->fsid << dendl;
    delete m;
    return false;
  }

  for (deque<LogEntry>::iterator p = m->entries.begin();
       p != m->entries.end();
       p++) {
    dout(10) << " logging " << *p << dendl;
    stringstream ss;
    ss << *p;
    string s;
    getline(ss, s);
    pending_inc.append(s);
    pending_inc.append("\n");
  }

  paxos->wait_for_commit(new C_Log(this, m, m->get_orig_source_inst()));
  return true;
}

void LogMonitor::_updated_log(MLog *ack, entity_inst_t who)
{
  dout(7) << "_updated_log for " << who << dendl;
  mon->messenger->send_message(ack, who);
}



bool LogMonitor::preprocess_command(MMonCommand *m)
{
  int r = -1;
  bufferlist rdata;
  stringstream ss;

  /*
  if (m->cmd.size() > 1) {
    if (m->cmd[1] == "stat") {
      ss << *this;
      r = 0;
    }
    else if (m->cmd[1] == "getmap") {
      pg_map.encode(rdata);
      ss << "got pgmap version " << pg_map.version;
      r = 0;
    }
    else if (m->cmd[1] == "send_pg_creates") {
      send_pg_creates();
      ss << "sent pg creates ";
      r = 0;
    }
    else if (m->cmd[1] == "dump") {
      ss << "version " << pg_map.version << std::endl;
      ss << "last_osdmap_epoch " << pg_map.last_osdmap_epoch << std::endl;
      ss << "last_pg_scan " << pg_map.last_pg_scan << std::endl;
      ss << "pg_stat\tobjects\tkb\tbytes\tv\treported\tstate\tosds" << std::endl;
      for (set<pg_t>::iterator p = pg_map.pg_set.begin();
	   p != pg_map.pg_set.end();
	   p++) {
	pg_stat_t &st = pg_map.pg_stat[*p];
	ss << *p 
	   << "\t" << st.num_objects
	   << "\t" << st.num_kb
	   << "\t" << st.num_bytes
	   << "\t" << pg_state_string(st.state)
	   << "\t" << st.version
	   << "\t" << st.reported
	   << "\t" << st.acting
	   << std::endl;
      }
      ss << "osdstat\tobject\tkbused\tkbavail\tkb\thb in\thb out" << std::endl;
      for (hash_map<int,osd_stat_t>::iterator p = pg_map.osd_stat.begin();
	   p != pg_map.osd_stat.end();
	   p++)
	ss << p->first
	   << "\t" << p->second.num_objects
	   << "\t" << p->second.kb_used
	   << "\t" << p->second.kb_avail 
	   << "\t" << p->second.kb
	   << "\t" << p->second.hb_in
	   << "\t" << p->second.hb_out
	   << std::endl;
      while (!ss.eof()) {
	string s;
	getline(ss, s);
	rdata.append(s.c_str(), s.length());
	rdata.append("\n", 1);
      }
      ss << "ok";
      r = 0;
    }
    else if (m->cmd[1] == "scrub" && m->cmd.size() == 3) {
      pg_t pgid;
      r = -EINVAL;
      if (pgid.parse(m->cmd[2].c_str())) {
	if (mon->pgmon->pg_map.pg_stat.count(pgid)) {
	  if (mon->pgmon->pg_map.pg_stat[pgid].acting.size()) {
	    int osd = mon->pgmon->pg_map.pg_stat[pgid].acting[0];
	    if (mon->osdmon->osdmap.is_up(osd)) {
	      vector<pg_t> pgs(1);
	      pgs[0] = pgid;
	      mon->messenger->send_message(new MOSDScrub(mon->monmap->fsid, pgs),
					   mon->osdmon->osdmap.get_inst(osd));
	      ss << "instructing pg " << pgid << " on osd" << osd << " to scrub";
	      r = 0;
	    } else
	      ss << "pg " << pgid << " primary osd" << osd << " not up";
	  } else
	    ss << "pg " << pgid << " has no primary osd";
	} else
	  ss << "pg " << pgid << " dne";
      } else
	ss << "invalid pgid '" << m->cmd[2] << "'";
    }
  }
  */

  if (r != -1) {
    string rs;
    getline(ss, rs);
    mon->reply_command(m, r, rs, rdata);
    return true;
  } else
    return false;
}


bool LogMonitor::prepare_command(MMonCommand *m)
{
  stringstream ss;
  string rs;
  int err = -EINVAL;

  // nothing here yet
  ss << "unrecognized command";

  getline(ss, rs);
  mon->reply_command(m, err, rs);
  return false;
}