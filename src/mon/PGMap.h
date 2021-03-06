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
 
/*
 * Placement Group Map. Placement Groups are logical sets of objects
 * that are replicated by the same set of devices. pgid=(r,hash(o)&m)
 * where & is a bit-wise AND and m=2^k-1
 */

#ifndef __PGMAP_H
#define __PGMAP_H

#include "osd/osd_types.h"
#include <sstream>

class PGMap {
public:
  // the map
  version_t version;
  epoch_t last_osdmap_epoch;   // last osdmap epoch i applied to the pgmap
  epoch_t last_pg_scan;  // osdmap epoch
  hash_map<pg_t,pg_stat_t> pg_stat;
  set<pg_t>                pg_set;
  hash_map<int,osd_stat_t> osd_stat;
  set<int> full_osds;
  set<int> nearfull_osds;

  class Incremental {
  public:
    version_t version;
    map<pg_t,pg_stat_t> pg_stat_updates;
    map<int,osd_stat_t> osd_stat_updates;
    set<int> osd_stat_rm;
    epoch_t osdmap_epoch;
    epoch_t pg_scan;  // osdmap epoch
    set<pg_t> pg_remove;

    void encode(bufferlist &bl) const {
      __u8 v = 1;
      ::encode(v, bl);
      ::encode(version, bl);
      ::encode(pg_stat_updates, bl);
      ::encode(osd_stat_updates, bl);
      ::encode(osd_stat_rm, bl);
      ::encode(osdmap_epoch, bl);
      ::encode(pg_scan, bl);
      ::encode(pg_remove, bl);
    }
    void decode(bufferlist::iterator &bl) {
      __u8 v;
      ::decode(v, bl);
      ::decode(version, bl);
      ::decode(pg_stat_updates, bl);
      ::decode(osd_stat_updates, bl);
      ::decode(osd_stat_rm, bl);
      ::decode(osdmap_epoch, bl);
      ::decode(pg_scan, bl);
      if (!bl.end())
	::decode(pg_remove, bl);
    }

    Incremental() : version(0), osdmap_epoch(0), pg_scan(0) {}
  };

  void apply_incremental(Incremental& inc) {
    assert(inc.version == version+1);
    version++;
    for (map<pg_t,pg_stat_t>::iterator p = inc.pg_stat_updates.begin();
	 p != inc.pg_stat_updates.end();
	 ++p) {
      if (pg_stat.count(p->first))
	stat_pg_sub(p->first, pg_stat[p->first]);
      if (pg_stat.count(p->first) == 0)
	pg_set.insert(p->first);
      pg_stat[p->first] = p->second;
      stat_pg_add(p->first, p->second);
    }
    for (map<int,osd_stat_t>::iterator p = inc.osd_stat_updates.begin();
	 p != inc.osd_stat_updates.end();
	 ++p) {
      if (osd_stat.count(p->first))
	stat_osd_sub(osd_stat[p->first]);
      osd_stat[p->first] = p->second;
      stat_osd_add(p->second);
      //update the full/nearful_osd sets
      int from = p->first;
      float ratio = ((float)p->second.kb_used) / (float) p->second.kb;
      if ( ratio > full_ratio ) {
	full_osds.insert(from);
	//sets don't double-insert so this might be a (very expensive) null-op
      }
      else if ( ratio > nearfull_ratio ) {
	nearfull_osds.insert(from);
	full_osds.erase(from);
      }
      else {//it's not full or near-full
	full_osds.erase(from);
	nearfull_osds.erase(from);
      }
    }
    for (set<pg_t>::iterator p = inc.pg_remove.begin();
	 p != inc.pg_remove.end();
	 p++) {
      if (pg_set.count(*p)) {
	pg_set.erase(*p);
	stat_pg_sub(*p, pg_stat[*p]);
	pg_stat.erase(*p);
      }
    }

    for (set<int>::iterator p = inc.osd_stat_rm.begin();
	 p != inc.osd_stat_rm.end();
	 p++) 
      if (osd_stat.count(*p)) {
	stat_osd_sub(osd_stat[*p]);
	osd_stat.erase(*p);
      }

    if (inc.osdmap_epoch)
      last_osdmap_epoch = inc.osdmap_epoch;
    if (inc.pg_scan)
      last_pg_scan = inc.pg_scan;
  }

  // aggregate stats (soft state)
  hash_map<int,int> num_pg_by_state;
  int64_t num_pg, num_osd;
  hash_map<int,pool_stat_t> pg_pool_sum;
  pool_stat_t pg_sum;
  osd_stat_t osd_sum;

  float full_ratio;
  float nearfull_ratio;

  set<pg_t> creating_pgs;   // lru: front = new additions, back = recently pinged
  
  void stat_zero() {
    num_pg = 0;
    num_pg_by_state.clear();
    num_osd = 0;
    pg_pool_sum.clear();
    pg_sum = pool_stat_t();
    osd_sum = osd_stat_t();
  }
  void stat_pg_add(pg_t pgid, pg_stat_t &s) {
    num_pg++;
    num_pg_by_state[s.state]++;
    pg_pool_sum[pgid.pool()].add(s);
    pg_sum.add(s);
    if (s.state & PG_STATE_CREATING)
      creating_pgs.insert(pgid);
  }
  void stat_pg_sub(pg_t pgid, pg_stat_t &s) {
    num_pg--;
    if (--num_pg_by_state[s.state] == 0)
      num_pg_by_state.erase(s.state);
    pg_pool_sum[pgid.pool()].sub(s);
    pg_sum.sub(s);
    if (s.state & PG_STATE_CREATING)
      creating_pgs.erase(pgid);
  }
  void stat_osd_add(osd_stat_t &s) {
    num_osd++;
    osd_sum.add(s);
  }
  void stat_osd_sub(osd_stat_t &s) {
    num_osd--;
    osd_sum.sub(s);
  }

  PGMap() : version(0),
	    last_osdmap_epoch(0), last_pg_scan(0),
	    num_pg(0),
	    num_osd(0),
	    full_ratio(CEPH_OSD_FULL_RATIO),
	    nearfull_ratio(CEPH_OSD_NEARFULL_RATIO) {}

  void encode(bufferlist &bl) {
    __u8 v = 1;
    ::encode(v, bl);
    ::encode(version, bl);
    ::encode(pg_stat, bl);
    ::encode(osd_stat, bl);
    ::encode(last_osdmap_epoch, bl);
    ::encode(last_pg_scan, bl);
  }
  void decode(bufferlist::iterator &bl) {
    __u8 v;
    ::decode(v, bl);
    ::decode(version, bl);
    ::decode(pg_stat, bl);
    ::decode(osd_stat, bl);
    ::decode(last_osdmap_epoch, bl);
    ::decode(last_pg_scan, bl);
    stat_zero();
    for (hash_map<pg_t,pg_stat_t>::iterator p = pg_stat.begin();
	 p != pg_stat.end();
	 ++p) {
      stat_pg_add(p->first, p->second);
      pg_set.insert(p->first);
    }
    for (hash_map<int,osd_stat_t>::iterator p = osd_stat.begin();
	 p != osd_stat.end();
	 ++p)
      stat_osd_add(p->second);
  }

  void dump(ostream& ss)
  {
    ss << "version " << version << std::endl;
    ss << "last_osdmap_epoch " << last_osdmap_epoch << std::endl;
    ss << "last_pg_scan " << last_pg_scan << std::endl;
    ss << "full_ratio " << full_ratio << std::endl;
    ss << "nearfull_ratio " << nearfull_ratio << std::endl;
    ss << "pg_stat\tobjects\tmip\tdegr\tkb\tbytes\tlog\tdisklog\tstate\tv\treported\tup\tacting\tlast_scrub" << std::endl;
    for (set<pg_t>::iterator p = pg_set.begin();
	 p != pg_set.end();
	 p++) {
      pg_stat_t &st = pg_stat[*p];
      ss << *p 
	 << "\t" << st.num_objects
	//<< "\t" << st.num_object_copies
	 << "\t" << st.num_objects_missing_on_primary
	 << "\t" << st.num_objects_degraded
	 << "\t" << st.num_kb
	 << "\t" << st.num_bytes
	 << "\t" << st.log_size
	 << "\t" << st.ondisk_log_size
	 << "\t" << pg_state_string(st.state)
	 << "\t" << st.version
	 << "\t" << st.reported
	 << "\t" << st.up
	 << "\t" << st.acting
	 << "\t" << st.last_scrub << "\t" << st.last_scrub_stamp
	 << std::endl;
    }
    for (hash_map<int,pool_stat_t>::iterator p = pg_pool_sum.begin();
	 p != pg_pool_sum.end();
	 p++)
      ss << "pool " << p->first
	 << "\t" << p->second.num_objects
	//<< "\t" << p->second.num_object_copies
	 << "\t" << p->second.num_objects_missing_on_primary
	 << "\t" << p->second.num_objects_degraded
	 << "\t" << p->second.num_kb
	 << "\t" << p->second.num_bytes
	 << "\t" << p->second.log_size
	 << "\t" << p->second.ondisk_log_size
	 << std::endl;
    ss << " sum\t" << pg_sum.num_objects
      //<< "\t" << pg_sum.num_object_copies
       << "\t" << pg_sum.num_objects_missing_on_primary
       << "\t" << pg_sum.num_objects_degraded
       << "\t" << pg_sum.num_kb
       << "\t" << pg_sum.num_bytes
       << "\t" << pg_sum.log_size
       << "\t" << pg_sum.ondisk_log_size
       << std::endl;
    ss << "osdstat\tkbused\tkbavail\tkb\thb in\thb out" << std::endl;
    for (hash_map<int,osd_stat_t>::iterator p = osd_stat.begin();
	 p != osd_stat.end();
	 p++)
      ss << p->first
	 << "\t" << p->second.kb_used
	 << "\t" << p->second.kb_avail 
	 << "\t" << p->second.kb
	 << "\t" << p->second.hb_in
	 << "\t" << p->second.hb_out
	 << std::endl;
    ss << " sum\t" << osd_sum.kb_used
	 << "\t" << osd_sum.kb_avail 
	 << "\t" << osd_sum.kb
	 << std::endl;
  }

  void print_summary(ostream& out) {
    std::stringstream ss;
    for (hash_map<int,int>::iterator p = num_pg_by_state.begin();
	 p != num_pg_by_state.end();
	 ++p) {
      if (p != num_pg_by_state.begin())
	ss << ", ";
      ss << p->second << " " << pg_state_string(p->first);
    }
    string states = ss.str();
    out << "v" << version << ": "
	<< pg_stat.size() << " pgs: "
	<< states << "; "
	<< kb_t(pg_sum.num_kb) << " data, " 
	<< kb_t(osd_sum.kb_used) << " used, "
	<< kb_t(osd_sum.kb_avail) << " / "
	<< kb_t(osd_sum.kb) << " avail";
    
    if (pg_sum.num_objects_degraded) {
      double pc = (double)pg_sum.num_objects_degraded / (double)pg_sum.num_object_copies * (double)100.0;
      char b[20];
      sprintf(b, "%.3lf", pc);
      out << "; " //<< pg_sum.num_objects_missing_on_primary << "/"
	  << pg_sum.num_objects_degraded 
	  << "/" << pg_sum.num_object_copies << " degraded (" << b << "%)";
    }
  }

};

inline ostream& operator<<(ostream& out, PGMap& m) {
  m.print_summary(out);
  return out;
}

#endif
