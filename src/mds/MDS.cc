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



#include "include/types.h"
#include "common/Clock.h"

#include "msg/Messenger.h"
#include "mon/MonClient.h"

#include "osd/OSDMap.h"
#include "osdc/Objecter.h"
#include "osdc/Filer.h"

#include "MDSMap.h"

#include "MDS.h"
#include "Server.h"
#include "Locker.h"
#include "MDCache.h"
#include "MDLog.h"
#include "MDBalancer.h"
#include "Migrator.h"

#include "AnchorServer.h"
#include "AnchorClient.h"
#include "SnapServer.h"
#include "SnapClient.h"

#include "InoTable.h"

#include "common/Logger.h"
#include "common/LogType.h"

#include "common/Timer.h"

#include "events/ESession.h"

#include "messages/MMDSMap.h"
#include "messages/MMDSBeacon.h"

#include "messages/MGenericMessage.h"

#include "messages/MOSDMap.h"

#include "messages/MClientRequest.h"
#include "messages/MClientRequestForward.h"

#include "messages/MMDSTableRequest.h"

#include "messages/MMonCommand.h"

#include "auth/AuthAuthorizeHandler.h"
#include "auth/KeyRing.h"

#include "config.h"

#define DOUT_SUBSYS mds
#undef dout_prefix
#define dout_prefix *_dout << dbeginl << "mds" << whoami << '.' << incarnation << ' '




// cons/des
MDS::MDS(const char *n, Messenger *m, MonClient *mc) : 
  mds_lock("MDS::mds_lock"),
  timer(mds_lock),
  name(n),
  whoami(-1), incarnation(0),
  standby_for_rank(-1),
  standby_replay_for(-1),
  messenger(m),
  monc(mc),
  logclient(messenger, &mc->monmap),
  sessionmap(this) {

  last_tid = 0;

  monc->set_messenger(messenger);

  mdsmap = new MDSMap;
  osdmap = new OSDMap;

  objecter = new Objecter(messenger, monc, osdmap, mds_lock);
  filer = new Filer(objecter);

  mdcache = new MDCache(this);
  mdlog = new MDLog(this);
  balancer = new MDBalancer(this);

  inotable = new InoTable(this);
  snapserver = new SnapServer(this);
  snapclient = new SnapClient(this);
  anchorserver = new AnchorServer(this);
  anchorclient = new AnchorClient(this);

  server = new Server(this);
  locker = new Locker(this, mdcache);

  // clients
  last_client_mdsmap_bcast = 0;
  
  // beacon
  beacon_last_seq = 0;
  beacon_sender = 0;
  beacon_killer = 0;
  laggy = false;

  // tick
  tick_event = 0;

  req_rate = 0;

  last_state = want_state = state = MDSMap::STATE_BOOT;

  logger = 0;
  mlogger = 0;
}

MDS::~MDS() {
  Mutex::Locker lock(mds_lock);

  if (mdcache) { delete mdcache; mdcache = NULL; }
  if (mdlog) { delete mdlog; mdlog = NULL; }
  if (balancer) { delete balancer; balancer = NULL; }
  if (inotable) { delete inotable; inotable = NULL; }
  if (anchorserver) { delete anchorserver; anchorserver = NULL; }
  if (snapserver) { delete snapserver; snapserver = NULL; }
  if (snapclient) { delete snapclient; snapclient = NULL; }
  if (anchorclient) { delete anchorclient; anchorclient = NULL; }
  if (osdmap) { delete osdmap; osdmap = 0; }
  if (mdsmap) { delete mdsmap; mdsmap = 0; }

  if (server) { delete server; server = 0; }
  if (locker) { delete locker; locker = 0; }

  if (filer) { delete filer; filer = 0; }
  if (objecter) { delete objecter; objecter = 0; }

  if (logger) { delete logger; logger = 0; }
  delete mlogger;
  
  if (messenger)
    messenger->destroy();
}


void MDS::reopen_logger(utime_t start)
{
  static LogType mds_logtype(l_mds_first, l_mds_last);
  static LogType mdm_logtype(l_mdm_first, l_mdm_last);

  static bool didit = false;
  if (!didit) {
    didit = true;
    
    mds_logtype.add_inc(l_mds_req, "req");
    mds_logtype.add_inc(l_mds_reply, "reply");
    mds_logtype.add_avg(l_mds_replyl, "replyl");
    mds_logtype.add_inc(l_mds_fw, "fw");
    
    mds_logtype.add_inc(l_mds_dir_f, "dir_f");
    mds_logtype.add_inc(l_mds_dir_c, "dir_c");
    mds_logtype.add_inc(l_mds_dir_sp, "dir_sp");
    mds_logtype.add_inc(l_mds_dir_ffc, "dir_ffc");
    //mds_logtype.add_inc("mkdir");

    /*
    mds_logtype.add_inc("newin"); // new inodes (pre)loaded
    mds_logtype.add_inc("newt");  // inodes first touched/used
    mds_logtype.add_inc("outt");  // trimmed touched
    mds_logtype.add_inc("outut"); // trimmed untouched (wasted effort)
    mds_logtype.add_avg("oututl"); // avg trim latency for untouched

    mds_logtype.add_inc("dirt1");
    mds_logtype.add_inc("dirt2");
    mds_logtype.add_inc("dirt3");
    mds_logtype.add_inc("dirt4");
    mds_logtype.add_inc("dirt5");
    */

    mds_logtype.add_set(l_mds_imax, "imax");
    mds_logtype.add_set(l_mds_i, "i");
    mds_logtype.add_set(l_mds_itop, "itop");
    mds_logtype.add_set(l_mds_ibot, "ibot");
    mds_logtype.add_set(l_mds_iptail, "iptail");  
    mds_logtype.add_set(l_mds_ipin, "ipin");
    mds_logtype.add_inc(l_mds_iex, "iex");
    mds_logtype.add_inc(l_mds_icap, "icap");
    mds_logtype.add_inc(l_mds_cap, "cap");
    
    mds_logtype.add_inc(l_mds_dis, "dis");

    mds_logtype.add_inc(l_mds_t, "t"); 
    mds_logtype.add_inc(l_mds_thit, "thit");
    mds_logtype.add_inc(l_mds_tfw, "tfw");
    mds_logtype.add_inc(l_mds_tdis, "tdis");
    mds_logtype.add_inc(l_mds_tdirf, "tdirf");
    mds_logtype.add_inc(l_mds_trino, "trino");
    mds_logtype.add_inc(l_mds_tlock, "tlock");
    
    mds_logtype.add_set(l_mds_l, "l");
    mds_logtype.add_set(l_mds_q, "q");
    mds_logtype.add_set(l_mds_popanyd, "popanyd");
    mds_logtype.add_set(l_mds_popnest, "popnest");
    
    mds_logtype.add_set(l_mds_sm, "sm");
    mds_logtype.add_inc(l_mds_ex, "ex");
    mds_logtype.add_inc(l_mds_iexp, "iexp");
    mds_logtype.add_inc(l_mds_im, "im");
    mds_logtype.add_inc(l_mds_iim, "iim");
    /*
    mds_logtype.add_inc("imex");  
    mds_logtype.add_set("nex");
    mds_logtype.add_set("nim");
    */

    mds_logtype.validate();

    mdm_logtype.add_set(l_mdm_ino, "ino");
    mdm_logtype.add_inc(l_mdm_inoa, "ino+");
    mdm_logtype.add_inc(l_mdm_inos, "ino-");
    mdm_logtype.add_set(l_mdm_dir, "dir");
    mdm_logtype.add_inc(l_mdm_dira, "dir+");
    mdm_logtype.add_inc(l_mdm_dirs, "dir-");
    mdm_logtype.add_set(l_mdm_dn, "dn");
    mdm_logtype.add_inc(l_mdm_dna, "dn+");
    mdm_logtype.add_inc(l_mdm_dns, "dn-");
    mdm_logtype.add_set(l_mdm_cap, "cap");
    mdm_logtype.add_inc(l_mdm_capa, "cap+");
    mdm_logtype.add_inc(l_mdm_caps, "cap-");
    mdm_logtype.add_set(l_mdm_rss, "rss");
    mdm_logtype.add_set(l_mdm_heap, "heap");
    mdm_logtype.add_set(l_mdm_malloc, "malloc");
    mdm_logtype.add_set(l_mdm_buf, "buf");
    mdm_logtype.validate();
  }

  if (whoami < 0) return;

  // flush+close old log
  delete logger;
  delete mlogger;

  // log
  char name[80];
  snprintf(name, sizeof(name), "mds%d", whoami);

  bool append = mdsmap->get_inc(whoami) > 1;

  logger = new Logger(name, (LogType*)&mds_logtype, append);
  logger->set_start(start);

  snprintf(name, sizeof(name), "mds%d.mem", whoami);
  mlogger = new Logger(name, (LogType*)&mdm_logtype, append);

  mdlog->reopen_logger(start, append);
  server->reopen_logger(start, append);
}




MDSTableClient *MDS::get_table_client(int t)
{
  switch (t) {
  case TABLE_ANCHOR: return anchorclient;
  case TABLE_SNAP: return snapclient;
  default: assert(0);
  }
}

MDSTableServer *MDS::get_table_server(int t)
{
  switch (t) {
  case TABLE_ANCHOR: return anchorserver;
  case TABLE_SNAP: return snapserver;
  default: assert(0);
  }
}








void MDS::send_message(Message *m, Connection *c) { 
  messenger->send_message(m, c);
}


void MDS::send_message_mds(Message *m, int mds)
{
  // send mdsmap first?
  if (mds != whoami && peer_mdsmap_epoch[mds] < mdsmap->get_epoch()) {
    messenger->send_message(new MMDSMap(monc->get_fsid(), mdsmap), 
			    mdsmap->get_inst(mds));
    peer_mdsmap_epoch[mds] = mdsmap->get_epoch();
  }

  // send message
  messenger->send_message(m, mdsmap->get_inst(mds));
}

void MDS::forward_message_mds(Message *m, int mds)
{
  assert(mds != whoami);

  // client request?
  if (m->get_type() == CEPH_MSG_CLIENT_REQUEST &&
      ((MClientRequest*)m)->get_source().is_client()) {
    MClientRequest *creq = (MClientRequest*)m;
    creq->inc_num_fwd();    // inc forward counter

    /*
     * don't actually forward if non-idempotent!
     * client has to do it.  although the MDS will ignore duplicate requests,
     * the affected metadata may migrate, in which case the new authority
     * won't have the metareq_id in the completed request map.
     */
    // NEW: always make the client resend!  
    bool client_must_resend = true;  //!creq->can_forward();

    // tell the client where it should go
    messenger->send_message(new MClientRequestForward(creq->get_tid(), mds, creq->get_num_fwd(),
						      client_must_resend),
			    creq->get_source_inst());
    
    if (client_must_resend) {
      m->put();
      return; 
    }
  }

  // these are the only types of messages we should be 'forwarding'; they
  // explicitly encode their source mds, which gets clobbered when we resend
  // them here.
  assert(m->get_type() == MSG_MDS_DIRUPDATE ||
	 m->get_type() == MSG_MDS_EXPORTDIRDISCOVER);

  // send mdsmap first?
  if (peer_mdsmap_epoch[mds] < mdsmap->get_epoch()) {
    messenger->send_message(new MMDSMap(monc->get_fsid(), mdsmap), 
			    mdsmap->get_inst(mds));
    peer_mdsmap_epoch[mds] = mdsmap->get_epoch();
  }

  messenger->send_message(m, mdsmap->get_inst(mds));
}



void MDS::send_message_client_counted(Message *m, client_t client)
{
  if (sessionmap.have_session(entity_name_t::CLIENT(client.v))) {
    version_t seq = sessionmap.inc_push_seq(client);
    dout(10) << "send_message_client_counted client" << client << " seq " << seq << " " << *m << dendl;
    messenger->send_message(m, sessionmap.get_session(entity_name_t::CLIENT(client.v))->inst);
  } else {
    dout(10) << "send_message_client_counted no session for client" << client << " " << *m << dendl;
  }
}

void MDS::send_message_client_counted(Message *m, entity_inst_t clientinst)
{
  version_t seq = sessionmap.inc_push_seq(clientinst.name.num());
  dout(10) << "send_message_client_counted " << clientinst.name << " seq " << seq << " " << *m << dendl;
  messenger->send_message(m, clientinst);
}

void MDS::send_message_client_counted(Message *m, Connection *con)
{
  Session *session = (Session *)con->get_priv();
  version_t seq = session->inc_push_seq();
  dout(10) << "send_message_client_counted " << session->inst.name << " seq "
	   << seq << " " << *m << dendl;
  messenger->send_message(m, con);
}

int MDS::init()
{
  dout(10) << sizeof(MDSCacheObject) << "\tMDSCacheObject" << dendl;
  dout(10) << sizeof(CInode) << "\tCInode" << dendl;
  dout(10) << sizeof(elist<void*>::item) << "\t elist<>::item   *7=" << 7*sizeof(elist<void*>::item) << dendl;
  dout(10) << sizeof(inode_t) << "\t inode_t " << dendl;
  dout(10) << sizeof(nest_info_t) << "\t  nest_info_t " << dendl;
  dout(10) << sizeof(frag_info_t) << "\t  frag_info_t " << dendl;
  dout(10) << sizeof(SimpleLock) << "\t SimpleLock   *5=" << 5*sizeof(SimpleLock) << dendl;
  dout(10) << sizeof(ScatterLock) << "\t ScatterLock  *3=" << 3*sizeof(ScatterLock) << dendl;
  dout(10) << sizeof(CDentry) << "\tCDentry" << dendl;
  dout(10) << sizeof(elist<void*>::item) << "\t elist<>::item" << dendl;
  dout(10) << sizeof(SimpleLock) << "\t SimpleLock" << dendl;
  dout(10) << sizeof(CDir) << "\tCDir " << dendl;
  dout(10) << sizeof(elist<void*>::item) << "\t elist<>::item   *2=" << 2*sizeof(elist<void*>::item) << dendl;
  dout(10) << sizeof(fnode_t) << "\t fnode_t " << dendl;
  dout(10) << sizeof(nest_info_t) << "\t  nest_info_t *2" << dendl;
  dout(10) << sizeof(frag_info_t) << "\t  frag_info_t *2" << dendl;
  dout(10) << sizeof(Capability) << "\tCapability " << dendl;
  dout(10) << sizeof(xlist<void*>::item) << "\t xlist<>::item   *2=" << 2*sizeof(xlist<void*>::item) << dendl;

  messenger->add_dispatcher_tail(this);
  messenger->add_dispatcher_head(&logclient);

  char name[30];
  snprintf(name, sizeof(name), "mds.%s", g_conf.id);
  _dout_create_courtesy_output_symlink(name);

  // get monmap
  monc->set_messenger(messenger);

  monc->set_want_keys(CEPH_ENTITY_TYPE_MON | CEPH_ENTITY_TYPE_OSD | CEPH_ENTITY_TYPE_MDS);
  monc->init();

  monc->authenticate();
  monc->wait_auth_rotating(30.0);

  mds_lock.Lock();

  // starting beacon.  this will induce an MDSMap from the monitor
  want_state = MDSMap::STATE_BOOT;
  beacon_start();
  whoami = -1;
  messenger->set_myname(entity_name_t::MDS(whoami));

  objecter->init();

  monc->sub_want("mdsmap", 0);
  monc->renew_subs();

  // schedule tick
  reset_tick();
  last_tick = g_clock.now();

  mds_lock.Unlock();
  return 0;
}

void MDS::reset_tick()
{
  // cancel old
  if (tick_event) timer.cancel_event(tick_event);

  // schedule
  tick_event = new C_MDS_Tick(this);
  timer.add_event_after(g_conf.mds_tick_interval, tick_event);
}

void MDS::tick()
{
  tick_event = 0;

  // reschedule
  reset_tick();

  _dout_check_log();

  logclient.send_log();

  utime_t now = g_clock.now();
  utime_t delay = now;
  delay -= last_tick;
  if (delay > g_conf.mds_session_timeout / 2) {
    dout(0) << " last tick was " << delay << " > " << g_conf.mds_tick_interval
	    << " seconds ago, laggy_until " << laggy_until
	    << ", setting laggy flag" << dendl;
    laggy = true;
  }  
  if (laggy)
    return;
  last_tick = now;

  // make sure mds log flushes, trims periodically
  mdlog->flush();

  if (is_active() || is_stopping()) {
    mdcache->trim();
    mdcache->trim_client_leases();
    mdcache->check_memory_usage();
    mdlog->trim();  // NOT during recovery!
  }

  // log
  mds_load_t load = balancer->get_load(now);
  
  if (logger) {
    req_rate = logger->get(l_mds_req);
    
    logger->fset(l_mds_l, (int)load.mds_load());
    logger->set(l_mds_q, messenger->get_dispatch_queue_len());
    logger->set(l_mds_sm, mdcache->num_subtrees());

    mdcache->log_stat();
  }

  // ...
  if (is_clientreplay() || is_active() || is_stopping()) {
    locker->scatter_tick();
    server->find_idle_sessions();
  }
  
  if (is_reconnect())
    server->reconnect_tick();
  
  if (is_active()) {
    balancer->tick();
    if (snapserver)
      snapserver->check_osd_map(false);
  }
}




// -----------------------
// beacons

void MDS::beacon_start()
{
  beacon_send();         // send first beacon
  
  //reset_beacon_killer(); // schedule killer
}
  


void MDS::beacon_send()
{
  ++beacon_last_seq;
  dout(10) << "beacon_send " << ceph_mds_state_name(want_state)
	   << " seq " << beacon_last_seq
	   << " (currently " << ceph_mds_state_name(state) << ")"
	   << dendl;

  beacon_seq_stamp[beacon_last_seq] = g_clock.now();
  
  MMDSBeacon *beacon = new MMDSBeacon(monc->get_fsid(), monc->get_global_id(), name, mdsmap->get_epoch(), 
				      want_state, beacon_last_seq);
  beacon->set_standby_for_rank(standby_for_rank);
  beacon->set_standby_for_name(standby_for_name);

  monc->send_mon_message(beacon);

  // schedule next sender
  if (beacon_sender) timer.cancel_event(beacon_sender);
  beacon_sender = new C_MDS_BeaconSender(this);
  timer.add_event_after(g_conf.mds_beacon_interval, beacon_sender);
}

void MDS::handle_mds_beacon(MMDSBeacon *m)
{
  version_t seq = m->get_seq();

  // update lab
  if (beacon_seq_stamp.count(seq)) {
    assert(beacon_seq_stamp[seq] > beacon_last_acked_stamp);
    beacon_last_acked_stamp = beacon_seq_stamp[seq];
    utime_t now = g_clock.now();
    utime_t rtt = now - beacon_last_acked_stamp;

    dout(10) << "handle_mds_beacon " << ceph_mds_state_name(m->get_state())
	     << " seq " << m->get_seq() 
	     << " rtt " << rtt << dendl;

    // clean up seq_stamp map
    while (!beacon_seq_stamp.empty() &&
	   beacon_seq_stamp.begin()->first <= seq)
      beacon_seq_stamp.erase(beacon_seq_stamp.begin());

    if (laggy && rtt < g_conf.mds_beacon_grace) {
      dout(1) << " clearing laggy flag" << dendl;
      laggy = false;
      laggy_until = now;
      last_tick = now;    // so that tick() will start up again
      queue_waiters(waiting_for_nolaggy);
    }
    
    reset_beacon_killer();
  } else {
    dout(10) << "handle_mds_beacon " << ceph_mds_state_name(m->get_state())
	     << " seq " << m->get_seq() << " dne" << dendl;
  }

  m->put();
}

void MDS::reset_beacon_killer()
{
  utime_t when = beacon_last_acked_stamp;
  when += g_conf.mds_beacon_grace;
  
  dout(25) << "reset_beacon_killer last_acked_stamp at " << beacon_last_acked_stamp
	   << ", will die at " << when << dendl;
  
  if (beacon_killer) timer.cancel_event(beacon_killer);

  beacon_killer = new C_MDS_BeaconKiller(this, beacon_last_acked_stamp);
  timer.add_event_at(when, beacon_killer);
}

void MDS::beacon_kill(utime_t lab)
{
  if (lab == beacon_last_acked_stamp) {
    dout(0) << "beacon_kill last_acked_stamp " << lab 
	    << ", setting laggy flag."
	    << dendl;
    laggy = true;
    //suicide();
  } else {
    dout(20) << "beacon_kill last_acked_stamp " << beacon_last_acked_stamp 
	     << " != my " << lab 
	     << ", doing nothing."
	     << dendl;
  }
}


void MDS::handle_command(MMonCommand *m)
{
  dout(10) << "handle_command args: " << m->cmd << dendl;
  if (m->cmd[0] == "injectargs")
    parse_config_option_string(m->cmd[1]);
  else if (m->cmd[0] == "dumpcache") {
    if (m->cmd.size() > 1)
      mdcache->dump_cache(m->cmd[1].c_str());
    else
      mdcache->dump_cache();
  }
  else if (m->cmd[0] == "session" && m->cmd[1] == "kill") {
    Session *session = sessionmap.get_session(entity_name_t(CEPH_ENTITY_TYPE_CLIENT,
							    strtol(m->cmd[2].c_str(), 0, 10)));
    if (session)
      server->kill_session(session);
    else
      dout(15) << "session " << session << " not in sessionmap!" << dendl;
  } else if (m->cmd[0] == "issue_caps") {
    long inum = strtol(m->cmd[1].c_str(), 0, 10);
    CInode * ino = mdcache->get_inode(inodeno_t(inum));
    if (ino) {
      bool r = locker->issue_caps(ino);
      dout(20) << "called issue_caps on inode "  << inum
	       << " with result " << r << dendl;
    } else dout(15) << "inode " << inum << " not in mdcache!" << dendl;
  } else if (m->cmd[0] == "try_eval") {
    long inum = strtol(m->cmd[1].c_str(), 0, 10);
    int mask = strtol(m->cmd[2].c_str(), 0, 10);
    CInode * ino = mdcache->get_inode(inodeno_t(inum));
    if (ino) {
      locker->try_eval(ino, mask);
      dout(20) << "try_eval(" << inum << ", " << mask << ")" << dendl;
    } else dout(15) << "inode " << inum << " not in mdcache!" << dendl;
  } else dout(0) << "unrecognized command! " << m->cmd << dendl;
  m->put();
}

void MDS::handle_mds_map(MMDSMap *m)
{
  version_t epoch = m->get_epoch();
  dout(5) << "handle_mds_map epoch " << epoch << " from " << m->get_source() << dendl;

  // note source's map version
  if (m->get_source().is_mds() && 
      peer_mdsmap_epoch[m->get_source().num()] < epoch) {
    dout(15) << " peer " << m->get_source()
	     << " has mdsmap epoch >= " << epoch
	     << dendl;
    peer_mdsmap_epoch[m->get_source().num()] = epoch;
  }

  // is it new?
  if (epoch <= mdsmap->get_epoch()) {
    dout(5) << " old map epoch " << epoch << " <= " << mdsmap->get_epoch() 
	    << ", discarding" << dendl;
    m->put();
    return;
  }

  // keep old map, for a moment
  MDSMap *oldmap = mdsmap;
  int oldwhoami = whoami;
  int oldstate = state;
  entity_addr_t addr;

  // decode and process
  mdsmap = new MDSMap;
  mdsmap->decode(m->get_encoded());

  monc->sub_got("mdsmap", mdsmap->get_epoch());

  // verify compatset
  dout(10) << "     my compat " << mdsmap_compat << dendl;
  dout(10) << " mdsmap compat " << mdsmap->compat << dendl;
  if (!mdsmap_compat.writeable(mdsmap->compat)) {
    dout(0) << "handle_mds_map mdsmap compatset " << mdsmap->compat
	    << " not writeable with daemon features " << mdsmap_compat
	    << ", killing myself" << dendl;
    suicide();
    goto out;
  }

  // see who i am
  addr = messenger->get_myaddr();
  whoami = mdsmap->get_rank_gid(monc->get_global_id());
  state = mdsmap->get_state_gid(monc->get_global_id());
  dout(10) << "map says i am " << addr << " mds" << whoami << " state " << ceph_mds_state_name(state) << dendl;

  if (state != oldstate)
    last_state = oldstate;

  if (state == MDSMap::STATE_STANDBY) {
    want_state = state = MDSMap::STATE_STANDBY;
    dout(1) << "handle_mds_map standby" << dendl;

    if (standby_replay_for >= 0)
      request_state(MDSMap::STATE_STANDBY_REPLAY);

    goto out;
  }

  if (whoami < 0) {
    if (want_state == MDSMap::STATE_BOOT) {
      dout(10) << "not in map yet" << dendl;
    } else {
      dout(1) << "handle_mds_map i (" << addr
	      << ") dne in the mdsmap, killing myself" << dendl;
      suicide();
    }
    goto out;
  }

  // ??
  assert(whoami >= 0);
  incarnation = mdsmap->get_inc(whoami);

  // open logger?
  if (oldwhoami != whoami || !logger) {
    _dout_create_courtesy_output_symlink("mds", whoami);
    reopen_logger(mdsmap->get_created());   // adopt mds cluster timeline
  }
  
  if (oldwhoami != whoami) {
    // update messenger.
    dout(1) << "handle_mds_map i am now mds" << whoami << "." << incarnation << dendl;
    messenger->set_myname(entity_name_t::MDS(whoami));
  }

  // tell objecter my incarnation
  if (objecter->get_client_incarnation() != incarnation)
    objecter->set_client_incarnation(incarnation);

  // for debug
  if (g_conf.mds_dump_cache_on_map)
    mdcache->dump_cache();

  // did it change?
  if (oldstate != state) {
    dout(1) << "handle_mds_map state change "
	    << ceph_mds_state_name(oldstate) << " --> "
	    << ceph_mds_state_name(state) << dendl;
    want_state = state;

    // did i just recover?
    if ((is_active() || is_clientreplay()) &&
	(oldstate == MDSMap::STATE_REJOIN ||
	 oldstate == MDSMap::STATE_RECONNECT)) 
      recovery_done();

    if (is_active()) {
      active_start();
    } else if (is_replay() || is_standby_replay()) {
      replay_start();
    } else if (is_resolve()) {
      resolve_start();
    } else if (is_reconnect()) {
      reconnect_start();
    } else if (is_clientreplay()) {
      clientreplay_start();
    } else if (is_creating()) {
      boot_create();
    } else if (is_starting()) {
      boot_start();
    } else if (is_stopping()) {
      assert(oldstate == MDSMap::STATE_ACTIVE);
      stopping_start();
    }
  }

  
  // RESOLVE
  // is someone else newly resolving?
  if (is_resolve() || is_rejoin() || is_clientreplay() || is_active() || is_stopping()) {
    set<int> oldresolve, resolve;
    oldmap->get_mds_set(oldresolve, MDSMap::STATE_RESOLVE);
    mdsmap->get_mds_set(resolve, MDSMap::STATE_RESOLVE);
    if (oldresolve != resolve) {
      dout(10) << "resolve set is " << resolve << ", was " << oldresolve << dendl;
      for (set<int>::iterator p = resolve.begin(); p != resolve.end(); ++p) 
	if (*p != whoami &&
	    oldresolve.count(*p) == 0)
	  mdcache->send_resolve(*p);  // now or later.
    }
  }
  
  // REJOIN
  // is everybody finally rejoining?
  if (is_rejoin() || is_clientreplay() || is_active() || is_stopping()) {
    // did we start?
    if (!oldmap->is_rejoining() && mdsmap->is_rejoining())
      rejoin_joint_start();

    // did we finish?
    if (g_conf.mds_dump_cache_after_rejoin &&
	oldmap->is_rejoining() && !mdsmap->is_rejoining()) 
      mdcache->dump_cache();      // for DEBUG only
  }
  if (oldmap->is_degraded() && !mdsmap->is_degraded() && state >= MDSMap::STATE_ACTIVE)
    dout(1) << "cluster recovered." << dendl;
  
  // did someone go active?
  if (is_clientreplay() || is_active() || is_stopping()) {
    set<int> oldactive, active;
    oldmap->get_mds_set(oldactive, MDSMap::STATE_ACTIVE);
    mdsmap->get_mds_set(active, MDSMap::STATE_ACTIVE);
    for (set<int>::iterator p = active.begin(); p != active.end(); ++p) 
      if (*p != whoami &&            // not me
	  oldactive.count(*p) == 0)  // newly so?
	handle_mds_recovery(*p);
  }

  // did someone fail?
  if (true) {
    // new failed?
    set<int> oldfailed, failed;
    oldmap->get_failed_mds_set(oldfailed);
    mdsmap->get_failed_mds_set(failed);
    for (set<int>::iterator p = failed.begin(); p != failed.end(); ++p)
      if (oldfailed.count(*p) == 0)
	mdcache->handle_mds_failure(*p);
    
    // or down then up?
    //  did their addr/inst change?
    set<int> up;
    mdsmap->get_up_mds_set(up);
    for (set<int>::iterator p = up.begin(); p != up.end(); ++p) 
      if (oldmap->have_inst(*p) &&
	  oldmap->get_inst(*p) != mdsmap->get_inst(*p))
	mdcache->handle_mds_failure(*p);
  }
  if (is_clientreplay() || is_active() || is_stopping()) {
    // did anyone stop?
    set<int> oldstopped, stopped;
    oldmap->get_stopped_mds_set(oldstopped);
    mdsmap->get_stopped_mds_set(stopped);
    for (set<int>::iterator p = stopped.begin(); p != stopped.end(); ++p) 
      if (oldstopped.count(*p) == 0)      // newly so?
	mdcache->migrator->handle_mds_failure_or_stop(*p);
  }

  balancer->try_rebalance();

 out:
  m->put();
  delete oldmap;
}

void MDS::bcast_mds_map()
{
  dout(7) << "bcast_mds_map " << mdsmap->get_epoch() << dendl;

  // share the map with mounted clients
  set<Session*> clients;
  sessionmap.get_client_session_set(clients);
  for (set<Session*>::const_iterator p = clients.begin();
       p != clients.end();
       ++p) 
    messenger->send_message(new MMDSMap(monc->get_fsid(), mdsmap), (*p)->inst);
  last_client_mdsmap_bcast = mdsmap->get_epoch();
}


void MDS::request_state(int s)
{
  dout(3) << "request_state " << ceph_mds_state_name(s) << dendl;
  want_state = s;
  beacon_send();
}


class C_MDS_CreateFinish : public Context {
  MDS *mds;
public:
  C_MDS_CreateFinish(MDS *m) : mds(m) {}
  void finish(int r) { mds->creating_done(); }
};

void MDS::boot_create()
{
  dout(3) << "boot_create" << dendl;

  C_Gather *fin = new C_Gather(new C_MDS_CreateFinish(this));

  mdcache->init_layouts();

  // start with a fresh journal
  dout(10) << "boot_create creating fresh journal" << dendl;
  mdlog->create(fin->new_sub());
  mdlog->start_new_segment(fin->new_sub());

  if (whoami == mdsmap->get_root()) {
    dout(3) << "boot_create creating fresh hierarchy" << dendl;
    mdcache->create_empty_hierarchy(fin);
  }

  dout(3) << "boot_create creating mydir hierarchy" << dendl;
  mdcache->create_mydir_hierarchy(fin);

  // fixme: fake out inotable (reset, pretend loaded)
  dout(10) << "boot_create creating fresh inotable table" << dendl;
  inotable->reset();
  inotable->save(fin->new_sub());

  // write empty sessionmap
  sessionmap.save(fin->new_sub());
  
  // initialize tables
  if (mdsmap->get_tableserver() == whoami) {
    dout(10) << "boot_create creating fresh anchortable" << dendl;
    anchorserver->reset();
    anchorserver->save(fin->new_sub());

    dout(10) << "boot_create creating fresh snaptable" << dendl;
    snapserver->reset();
    snapserver->save(fin->new_sub());
  }
}

void MDS::creating_done()
{
  dout(1)<< "creating_done" << dendl;
  request_state(MDSMap::STATE_ACTIVE);

  // start new segment
  mdlog->start_new_segment(0);
}


class C_MDS_BootStart : public Context {
  MDS *mds;
  int nextstep;
public:
  C_MDS_BootStart(MDS *m, int n) : mds(m), nextstep(n) {}
  void finish(int r) { mds->boot_start(nextstep, r); }
};

void MDS::boot_start(int step, int r)
{
  if (r < 0) {
    dout(0) << "boot_start encountered an error, failing" << dendl;
    suicide();
    return;
  }

  switch (step) {
  case 0:
    mdcache->init_layouts();
    step = 1;  // fall-thru.

  case 1:
    {
      C_Gather *gather = new C_Gather(new C_MDS_BootStart(this, 2));
      dout(2) << "boot_start " << step << ": opening inotable" << dendl;
      inotable->load(gather->new_sub());

      dout(2) << "boot_start " << step << ": opening sessionmap" << dendl;
      sessionmap.load(gather->new_sub());

      if (mdsmap->get_tableserver() == whoami) {
	dout(2) << "boot_start " << step << ": opening anchor table" << dendl;
	anchorserver->load(gather->new_sub());

	dout(2) << "boot_start " << step << ": opening snap table" << dendl;	
	snapserver->load(gather->new_sub());
      }
      
      dout(2) << "boot_start " << step << ": opening mds log" << dendl;
      mdlog->open(gather->new_sub());
    }
    break;

  case 2:
    if (is_starting()) {
      dout(2) << "boot_start " << step << ": loading/discovering root inode" << dendl;
      mdcache->open_root_inode(new C_MDS_BootStart(this, 3));
      break;
    } else {
      // replay.  make up fake root inode to start with
      mdcache->create_root_inode();
      step = 3;
    }

  case 3:
    if (is_replay() || is_standby_replay()) {
      dout(2) << "boot_start " << step << ": replaying mds log" << dendl;
      mdlog->replay(new C_MDS_BootStart(this, 4));
      break;
    } else {
      dout(2) << "boot_start " << step << ": positioning at end of old mds log" << dendl;
      mdlog->append();
      step++;
    }

  case 4:
    if (is_replay() || is_standby_replay()) {
      replay_done();
      break;
    }
    step++;
    
    starting_done();
    break;
  }
}

void MDS::starting_done()
{
  dout(3) << "starting_done" << dendl;
  assert(is_starting());
  request_state(MDSMap::STATE_ACTIVE);

  // start new segment
  mdlog->start_new_segment(0);

  mdcache->open_root();
}


void MDS::replay_start()
{
  dout(1) << "replay_start" << dendl;

  // initialize gather sets
  set<int> rs;
  mdsmap->get_recovery_mds_set(rs);
  rs.erase(whoami);
  dout(1) << "now replay.  my recovery peers are " << rs << dendl;
  mdcache->set_recovery_set(rs);

  // start?
  //if (osdmap->get_epoch() > 0 &&
  //mdsmap->get_epoch() > 0)
  boot_start();
}

void MDS::replay_done()
{
  dout(1) << "replay_done in=" << mdsmap->get_num_mds()
	  << " failed=" << mdsmap->get_num_failed()
	  << dendl;

  if (is_standby_replay()) {
    dout(2) << "hack.  journal looks ok.  shutting down." << dendl;
    suicide();
    return;
  }

  if (mdsmap->get_num_mds() == 1 &&
      mdsmap->get_num_failed() == 0) { // just me!
    dout(2) << "i am alone, moving to state reconnect" << dendl;      
    request_state(MDSMap::STATE_RECONNECT);
  } else {
    dout(2) << "i am not alone, moving to state resolve" << dendl;
    request_state(MDSMap::STATE_RESOLVE);
  }

  // start new segment
  mdlog->start_new_segment(0);
}


void MDS::resolve_start()
{
  dout(1) << "resolve_start" << dendl;
  mdcache->resolve_start();
}
void MDS::resolve_done()
{
  dout(1) << "resolve_done" << dendl;
  request_state(MDSMap::STATE_RECONNECT);
}

void MDS::reconnect_start()
{
  dout(1) << "reconnect_start" << dendl;
  server->reconnect_clients();
  finish_contexts(waiting_for_reconnect);
}
void MDS::reconnect_done()
{
  dout(1) << "reconnect_done" << dendl;
  request_state(MDSMap::STATE_REJOIN);    // move to rejoin state
}

void MDS::rejoin_joint_start()
{
  dout(1) << "rejoin_joint_start" << dendl;
  mdcache->rejoin_send_rejoins();
}
void MDS::rejoin_done()
{
  dout(1) << "rejoin_done" << dendl;
  mdcache->show_subtrees();
  mdcache->show_cache();

  // funny case: is our cache empty?  no subtrees?
  if (!mdcache->is_subtrees()) {
    dout(1) << " empty cache, no subtrees, leaving cluster" << dendl;
    request_state(MDSMap::STATE_STOPPED);
    return;
  }

  if (replay_queue.empty())
    request_state(MDSMap::STATE_ACTIVE);
  else
    request_state(MDSMap::STATE_CLIENTREPLAY);
}

void MDS::clientreplay_start()
{
  dout(1) << "clientreplay_start" << dendl;
  finish_contexts(waiting_for_replay);  // kick waiters
  queue_one_replay();
}

void MDS::clientreplay_done()
{
  dout(1) << "clientreplay_done" << dendl;
  request_state(MDSMap::STATE_ACTIVE);
}

void MDS::active_start()
{
  dout(1) << "active_start" << dendl;

  if (last_state == MDSMap::STATE_CREATING)
    mdcache->open_root();

  mdcache->clean_open_file_lists();
  finish_contexts(waiting_for_replay);  // kick waiters
  finish_contexts(waiting_for_active);  // kick waiters
}


void MDS::recovery_done()
{
  dout(1) << "recovery_done -- successful recovery!" << dendl;
  assert(is_clientreplay() || is_active() || is_clientreplay());
  
  // kick anchortable (resent AGREEs)
  if (mdsmap->get_tableserver() == whoami) {
    anchorserver->finish_recovery();
    snapserver->finish_recovery();
  }
  
  // kick anchorclient (resent COMMITs)
  anchorclient->finish_recovery();
  snapclient->finish_recovery();
  
  mdcache->start_recovered_truncates();
  mdcache->do_file_recover();

  mdcache->reissue_all_caps();
  
  // tell connected clients
  //bcast_mds_map();     // not anymore, they get this from the monitor

  mdcache->populate_mydir();
}

void MDS::handle_mds_recovery(int who) 
{
  dout(5) << "handle_mds_recovery mds" << who << dendl;
  
  mdcache->handle_mds_recovery(who);

  if (anchorserver) {
    anchorserver->handle_mds_recovery(who);
    snapserver->handle_mds_recovery(who);
  }
  anchorclient->handle_mds_recovery(who);
  snapclient->handle_mds_recovery(who);
  
  queue_waiters(waiting_for_active_peer[who]);
  waiting_for_active_peer.erase(who);
}

void MDS::stopping_start()
{
  dout(2) << "stopping_start" << dendl;

  if (mdsmap->get_num_mds() == 1 && !sessionmap.empty()) {
    // we're the only mds up!
    dout(0) << "we are the last MDS, and have mounted clients: we cannot flush our journal.  suicide!" << dendl;
    suicide();
  }

  // start cache shutdown
  mdcache->shutdown_start();
  
  // terminate client sessions
  server->terminate_sessions();
}

void MDS::stopping_done()
{
  dout(2) << "stopping_done" << dendl;

  // tell monitor we shut down cleanly.
  request_state(MDSMap::STATE_STOPPED);
}

  

void MDS::suicide()
{
  if (want_state == MDSMap::STATE_STOPPED)
    state = want_state;
  else
    state = CEPH_MDS_STATE_DNE; // whatever.

  dout(1) << "suicide.  wanted " << ceph_mds_state_name(want_state)
	  << ", now " << ceph_mds_state_name(state) << dendl;

  // stop timers
  if (beacon_killer) {
    timer.cancel_event(beacon_killer);
    beacon_killer = 0;
  }
  if (beacon_sender) {
    timer.cancel_event(beacon_sender);
    beacon_sender = 0;
  }
  if (tick_event) {
    timer.cancel_event(tick_event);
    tick_event = 0;
  }
  timer.cancel_all();
  //timer.join();  // this will deadlock from beacon_kill -> suicide
  
  // shut down cache
  mdcache->shutdown();

  objecter->shutdown();
  
  // shut down messenger
  messenger->shutdown();

  monc->shutdown();
}





bool MDS::ms_dispatch(Message *m)
{
  mds_lock.Lock();
  bool ret = _dispatch(m);
  mds_lock.Unlock();
  return ret;
}

bool MDS::ms_get_authorizer(int dest_type, AuthAuthorizer **authorizer, bool force_new)
{
  dout(10) << "MDS::ms_get_authorizer type=" << ceph_entity_type_name(dest_type) << dendl;

  /* monitor authorization is being handled on different layer */
  if (dest_type == CEPH_ENTITY_TYPE_MON)
    return true;

  if (force_new) {
    if (monc->wait_auth_rotating(10) < 0)
      return false;
  }

  *authorizer = monc->auth->build_authorizer(dest_type);
  return *authorizer != NULL;
}

bool MDS::_dispatch(Message *m)
{
  bool check_from = false;

  utime_t req_start = g_clock.now();

#define ALLOW_MESSAGES_FROM(peers) \
do { \
  if (m->get_connection() && (m->get_connection()->get_peer_type() & (peers)) == 0) { \
    dout(0) << __FILE__ << "." << __LINE__ << ": filtered out request, peer=" << m->get_connection()->get_peer_type() \
           << " allowing=" << #peers << " message=" << *m << dendl; \
    m->put();							    \
    return true; \
  } \
  check_from = true; \
} while (0)

  // from bad mds?
  if (m->get_source().is_mds()) {
    int from = m->get_source().num();
    if (!mdsmap->have_inst(from) ||
	mdsmap->get_inst(from) != m->get_source_inst() ||
	mdsmap->is_down(from)) {
      // bogus mds?
      if (m->get_type() == CEPH_MSG_MDS_MAP) {
	dout(5) << "got " << *m << " from old/bad/imposter mds " << m->get_source()
		<< ", but it's an mdsmap, looking at it" << dendl;
      } else if (m->get_type() == MSG_MDS_CACHEEXPIRE &&
		 mdsmap->get_inst(from) == m->get_source_inst()) {
	dout(5) << "got " << *m << " from down mds " << m->get_source()
		<< ", but it's a cache_expire, looking at it" << dendl;
      } else {
	dout(5) << "got " << *m << " from down/old/bad/imposter mds " << m->get_source()
		<< ", dropping" << dendl;
	m->put();
	return true;
      }
    }
  }

  switch (m->get_type()) {

  case CEPH_MSG_MON_MAP:
    ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MON);
    m->put();
    break;

    // MDS
  case CEPH_MSG_MDS_MAP:
    ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MON | CEPH_ENTITY_TYPE_MDS);
    handle_mds_map((MMDSMap*)m);
    break;
  case MSG_MDS_BEACON:
    ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MON);
    handle_mds_beacon((MMDSBeacon*)m);
    break;
    
    // misc
  case MSG_MON_COMMAND:
    ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MON);
    handle_command((MMonCommand*)m);
    break;    
    
  default:
    
    if (laggy) {
      dout(10) << "laggy, deferring " << *m << dendl;
      waiting_for_nolaggy.push_back(new C_MDS_RetryMessage(this, m));
    } else {
      int port = m->get_type() & 0xff00;
      switch (port) {
      case MDS_PORT_CACHE:
        ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MDS);
	mdcache->dispatch(m);
	break;
	
      case MDS_PORT_MIGRATOR:
        ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MDS);
	mdcache->migrator->dispatch(m);
	break;
	
      default:
	switch (m->get_type()) {
	  // SERVER
	case CEPH_MSG_CLIENT_SESSION:
	case CEPH_MSG_CLIENT_REQUEST:
	case CEPH_MSG_CLIENT_RECONNECT:
          ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_CLIENT);
	  server->dispatch(m);
	  break;
	case MSG_MDS_SLAVE_REQUEST:
          ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MDS);
	  server->dispatch(m);
	  break;
	  
	case MSG_MDS_HEARTBEAT:
          ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MDS);
	  balancer->proc_message(m);
	  break;
	  
	case MSG_MDS_TABLE_REQUEST:
          ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MDS);
	  {
	    MMDSTableRequest *req = (MMDSTableRequest*)m;
	    if (req->op < 0) {
	      MDSTableClient *client = get_table_client(req->table);
	      client->handle_request(req);
	    } else {
	      MDSTableServer *server = get_table_server(req->table);
	      server->handle_request(req);
	    }
	  }
	  break;

        case MSG_MDS_LOCK:
        case MSG_MDS_INODEFILECAPS:
          ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MDS);
	  locker->dispatch(m);
          break;

        case CEPH_MSG_CLIENT_CAPS:
        case CEPH_MSG_CLIENT_CAPRELEASE:
        case CEPH_MSG_CLIENT_LEASE:
          ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_CLIENT);
	  locker->dispatch(m);
	  break;
	  
	  // OSD
	case CEPH_MSG_OSD_OPREPLY:
          ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_OSD);
	  objecter->handle_osd_op_reply((class MOSDOpReply*)m);
	  break;
	case CEPH_MSG_OSD_MAP:
          ALLOW_MESSAGES_FROM(CEPH_ENTITY_TYPE_MON | CEPH_ENTITY_TYPE_OSD);
	  objecter->handle_osd_map((MOSDMap*)m);
	  if (is_active() && snapserver)
	    snapserver->check_osd_map(true);
	  break;
	  
	default:
	  return false;
	}
      }
      
    }
  }

  //assert(check_from);


  if (laggy)
    return true;


  // finish any triggered contexts
  static bool finishing = false;
  if (!finishing) {
    while (finished_queue.size()) {
      dout(7) << "mds has " << finished_queue.size() << " queued contexts" << dendl;
      dout(10) << finished_queue << dendl;
      finishing = true;
      finish_contexts(finished_queue);
      finishing = false;
    }

    // done with all client replayed requests?
    if (is_clientreplay() &&
	mdcache->is_open() &&
	replay_queue.empty() &&
	want_state == MDSMap::STATE_CLIENTREPLAY) {
      dout(10) << " still have " << mdcache->get_num_active_requests()
	       << " active replay requests" << dendl;
      if (mdcache->get_num_active_requests() == 0)
	clientreplay_done();
    }
  }

  utime_t duration = g_clock.now();
  duration -= req_start;
  if (duration > g_conf.mds_session_timeout / 2) {
    dout(0) << " dispatch took " << duration << " > " << g_conf.mds_tick_interval
	    << " seconds, setting laggy flag" << dendl;
    laggy = true;
    return true;
  }


  // hack: thrash exports
  static utime_t start;
  utime_t now = g_clock.now();
  if (start == utime_t()) 
    start = now;
  double el = now - start;
  if (el > 30.0 &&
	   el < 60.0)
  for (int i=0; i<g_conf.mds_thrash_exports; i++) {
    set<int> s;
    if (!is_active()) break;
    mdsmap->get_mds_set(s, MDSMap::STATE_ACTIVE);
    if (s.size() < 2 || mdcache->get_num_inodes() < 10) 
      break;  // need peers for this to work.

    dout(7) << "mds thrashing exports pass " << (i+1) << "/" << g_conf.mds_thrash_exports << dendl;
    
    // pick a random dir inode
    CInode *in = mdcache->hack_pick_random_inode();

    list<CDir*> ls;
    in->get_dirfrags(ls);
    if (ls.empty()) continue;                // must be an open dir.
    CDir *dir = ls.front();
    if (!dir->get_parent_dir()) continue;    // must be linked.
    if (!dir->is_auth()) continue;           // must be auth.

    int dest;
    do {
      int k = rand() % s.size();
      set<int>::iterator p = s.begin();
      while (k--) p++;
      dest = *p;
    } while (dest == whoami);
    mdcache->migrator->export_dir_nicely(dir,dest);
  }
  // hack: thrash fragments
  for (int i=0; i<g_conf.mds_thrash_fragments; i++) {
    if (!is_active()) break;
    dout(7) << "mds thrashing fragments pass " << (i+1) << "/" << g_conf.mds_thrash_fragments << dendl;
    
    // pick a random dir inode
    CInode *in = mdcache->hack_pick_random_inode();

    list<CDir*> ls;
    in->get_dirfrags(ls);
    if (ls.empty()) continue;                // must be an open dir.
    CDir *dir = ls.front();
    if (!dir->get_parent_dir()) continue;    // must be linked.
    if (!dir->is_auth()) continue;           // must be auth.
    mdcache->split_dir(dir, 1);// + (rand() % 3));
  }

  // hack: force hash root?
  /*
  if (false &&
      mdcache->get_root() &&
      mdcache->get_root()->dir &&
      !(mdcache->get_root()->dir->is_hashed() || 
        mdcache->get_root()->dir->is_hashing())) {
    dout(0) << "hashing root" << dendl;
    mdcache->migrator->hash_dir(mdcache->get_root()->dir);
  }
  */

  if (mlogger) {
    mlogger->set(l_mdm_ino, g_num_ino);
    mlogger->set(l_mdm_dir, g_num_dir);
    mlogger->set(l_mdm_dn, g_num_dn);
    mlogger->set(l_mdm_cap, g_num_cap);

    mlogger->inc(l_mdm_inoa, g_num_inoa);  g_num_inoa = 0;
    mlogger->inc(l_mdm_inos, g_num_inos);  g_num_inos = 0;
    mlogger->inc(l_mdm_dira, g_num_dira);  g_num_dira = 0;
    mlogger->inc(l_mdm_dirs, g_num_dirs);  g_num_dirs = 0;
    mlogger->inc(l_mdm_dna, g_num_dna);  g_num_dna = 0;
    mlogger->inc(l_mdm_dns, g_num_dns);  g_num_dns = 0;
    mlogger->inc(l_mdm_capa, g_num_capa);  g_num_capa = 0;
    mlogger->inc(l_mdm_caps, g_num_caps);  g_num_caps = 0;

    mlogger->set(l_mdm_buf, buffer_total_alloc.read());

  }

  // shut down?
  if (is_stopping()) {
    mdlog->trim();
    if (mdcache->shutdown_pass()) {
      dout(7) << "shutdown_pass=true, finished w/ shutdown, moving to down:stopped" << dendl;
      stopping_done();
    }
    else {
      dout(7) << "shutdown_pass=false" << dendl;
    }
  }
  return true;
}




void MDS::ms_handle_connect(Connection *con) 
{
  Mutex::Locker l(mds_lock);
  dout(0) << "ms_handle_connect on " << con->get_peer_addr() << dendl;
  objecter->ms_handle_connect(con);
}

bool MDS::ms_handle_reset(Connection *con) 
{
  Mutex::Locker l(mds_lock);
  dout(0) << "ms_handle_reset on " << con->get_peer_addr() << dendl;
  if (con->get_peer_type() == CEPH_ENTITY_TYPE_OSD) {
    objecter->ms_handle_reset(con);
  } else if (con->get_peer_type() == CEPH_ENTITY_TYPE_CLIENT) {
    Session *session = (Session *)con->get_priv();
    if (session) {
      if (session->is_closed()) {
	messenger->mark_down(con->get_peer_addr());
	sessionmap.remove_session(session);
      }
      session->put();
    } else {
      messenger->mark_down(con->get_peer_addr());
    }
  }
  return false;
}


void MDS::ms_handle_remote_reset(Connection *con) 
{
  Mutex::Locker l(mds_lock);
  dout(0) << "ms_handle_remote_reset on " << con->get_peer_addr() << dendl;
  objecter->ms_handle_remote_reset(con);
}

bool MDS::ms_verify_authorizer(Connection *con, int peer_type,
			       int protocol, bufferlist& authorizer_data, bufferlist& authorizer_reply,
			       bool& is_valid)
{
  Mutex::Locker l(mds_lock);

  AuthAuthorizeHandler *authorize_handler = get_authorize_handler(protocol);
  if (!authorize_handler) {
    is_valid = false;
    return true;
  }

  AuthCapsInfo caps_info;
  EntityName name;
  uint64_t global_id;

  is_valid = authorize_handler->verify_authorizer(monc->rotating_secrets,
						  authorizer_data, authorizer_reply, name, global_id, caps_info);

  if (is_valid) {
    // wire up a Session* to this connection, and add it to the session map
    entity_name_t n(con->get_peer_type(), global_id);
    Session *s = sessionmap.get_session(n);
    if (!s) {
      s = new Session;
      s->inst.addr = con->get_peer_addr();
      s->inst.name = n;
      dout(10) << " new session " << s << " for " << s->inst << dendl;
      con->set_priv(s);
      sessionmap.add_session(s);
    } else {
      dout(10) << " existing session " << s << " for " << s->inst << dendl;
      con->set_priv(s->get());
    }

    /*
    s->caps.set_allow_all(caps_info.allow_all);
 
    if (caps_info.caps.length() > 0) {
      bufferlist::iterator iter = caps_info.caps.begin();
      s->caps.parse(iter);
      dout(10) << " session " << s << " has caps " << s->caps << dendl;
    }
    */
  }

  return true;  // we made a decision (see is_valid)
};


