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

// TODO: missing run() method, which creates the two main timers, refreshTimer and readTimer

#include "Monitor.h"

#include "osd/OSDMap.h"

#include "MonitorStore.h"

#include "msg/Messenger.h"

#include "messages/PaxosServiceMessage.h"
#include "messages/MMonMap.h"
#include "messages/MMonGetMap.h"
#include "messages/MGenericMessage.h"
#include "messages/MMonCommand.h"
#include "messages/MMonCommandAck.h"
#include "messages/MMonObserve.h"
#include "messages/MMonObserveNotify.h"

#include "messages/MMonPaxos.h"
#include "messages/MClass.h"
#include "messages/MRoute.h"
#include "messages/MForward.h"

#include "messages/MMonSubscribe.h"
#include "messages/MMonSubscribeAck.h"

#include "messages/MAuthReply.h"

#include "common/Timer.h"
#include "common/Clock.h"

#include "OSDMonitor.h"
#include "MDSMonitor.h"
#include "MonmapMonitor.h"
#include "PGMonitor.h"
#include "LogMonitor.h"
#include "ClassMonitor.h"
#include "AuthMonitor.h"

#include "osd/OSDMap.h"

#include "auth/AuthSupported.h"

#include "config.h"

#define DOUT_SUBSYS mon
#undef dout_prefix
#define dout_prefix _prefix(this)
static ostream& _prefix(Monitor *mon) {
  return *_dout << dbeginl
		<< "mon" << mon->whoami
		<< (mon->is_starting() ?
		    (const char*)"(starting)" : 
		    (mon->is_leader() ?
		     (const char*)"(leader)" :
		     (mon->is_peon() ? 
		      (const char*)"(peon)" : 
		      (const char*)"(?\?)")))
		<< " e" << mon->monmap->get_epoch()
		<< " ";
}

const CompatSet::Feature ceph_mon_feature_compat[] = 
  {END_FEATURE};
const CompatSet::Feature ceph_mon_feature_ro_compat[] = 
  {END_FEATURE};
const CompatSet::Feature ceph_mon_feature_incompat[] =
  { CEPH_MON_FEATURE_INCOMPAT_BASE , CompatSet::Feature(0, "")};

Monitor::Monitor(int w, MonitorStore *s, Messenger *m, MonMap *map) :
  whoami(w), 
  messenger(m),
  lock("Monitor::lock"),
  monmap(map),
  logclient(messenger, monmap),
  timer(lock), tick_timer(0),
  store(s),
  
  state(STATE_STARTING), stopping(false),
  
  elector(this, w),
  mon_epoch(0), 
  leader(0),
  paxos(PAXOS_NUM), paxos_service(PAXOS_NUM),
  routed_request_tid(0)
{
  paxos_service[PAXOS_MDSMAP] = new MDSMonitor(this, add_paxos(PAXOS_MDSMAP));
  paxos_service[PAXOS_MONMAP] = new MonmapMonitor(this, add_paxos(PAXOS_MONMAP));
  paxos_service[PAXOS_OSDMAP] = new OSDMonitor(this, add_paxos(PAXOS_OSDMAP));
  paxos_service[PAXOS_PGMAP] = new PGMonitor(this, add_paxos(PAXOS_PGMAP));
  paxos_service[PAXOS_LOG] = new LogMonitor(this, add_paxos(PAXOS_LOG));
  paxos_service[PAXOS_CLASS] = new ClassMonitor(this, add_paxos(PAXOS_CLASS));
  paxos_service[PAXOS_AUTH] = new AuthMonitor(this, add_paxos(PAXOS_AUTH));

  mon_caps = new MonCaps();
  mon_caps->set_allow_all(true);
  mon_caps->text = "allow *";
}

Paxos *Monitor::add_paxos(int type)
{
  Paxos *p = new Paxos(this, whoami, type);
  paxos[type] = p;
  return p;
}

Monitor::~Monitor()
{
  for (vector<PaxosService*>::iterator p = paxos_service.begin(); p != paxos_service.end(); p++)
    delete *p;
  for (vector<Paxos*>::iterator p = paxos.begin(); p != paxos.end(); p++)
    delete *p;
  //clean out MonSessionMap's subscriptions
  for (map<string, xlist<Subscription*> >::iterator i
	 = session_map.subs.begin();
       i != session_map.subs.end();
       ++i) {
    while (!i->second.empty()) {
      session_map.remove_sub(i->second.front());
    }
  }
  //clean out MonSessionMap's sessions
  while (!session_map.sessions.empty()) {
    session_map.remove_session(session_map.sessions.front());
  }
  delete mon_caps;
}

void Monitor::init()
{
  lock.Lock();
  
  dout(1) << "init fsid " << monmap->fsid << dendl;
  
  // init paxos
  for (vector<Paxos*>::iterator p = paxos.begin(); p != paxos.end(); p++)
    (*p)->init();

  for (vector<PaxosService*>::iterator ps = paxos_service.begin(); ps != paxos_service.end(); ps++)
    (*ps)->init();

  logclient.set_synchronous(true);
  
  // i'm ready!
  messenger->add_dispatcher_tail(this);
  messenger->add_dispatcher_head(&logclient);
  
  // start ticker
  reset_tick();
  
  // call election?
  if (monmap->size() > 1) {
    call_election();
  } else {
    // we're standalone.
    set<int> q;
    q.insert(whoami);
    win_election(1, q);
  }
  
  lock.Unlock();
}

void Monitor::shutdown()
{
  dout(1) << "shutdown" << dendl;
  
  elector.shutdown();
  
  // clean up
  for (vector<PaxosService*>::iterator p = paxos_service.begin(); p != paxos_service.end(); p++)
    (*p)->shutdown();

  // cancel all events
  cancel_tick();
  timer.cancel_all();
  timer.join();  

  // die.
  messenger->shutdown();
}


void Monitor::call_election(bool is_new)
{
  if (monmap->size() == 1) return;
  
  if (is_new) {
    stringstream ss;
    ss << "mon" << whoami << " calling new monitor election";
    logclient.log(LOG_INFO, ss);
  }

  dout(10) << "call_election" << dendl;
  state = STATE_STARTING;
  
  // tell paxos
  for (vector<Paxos*>::iterator p = paxos.begin(); p != paxos.end(); p++)
    (*p)->election_starting();
  for (vector<PaxosService*>::iterator p = paxos_service.begin(); p != paxos_service.end(); p++)
    (*p)->election_starting();

  // call a new election
  elector.call_election();
}

void Monitor::win_election(epoch_t epoch, set<int>& active) 
{
  state = STATE_LEADER;
  leader = whoami;
  mon_epoch = epoch;
  quorum = active;
  dout(10) << "win_election, epoch " << mon_epoch << " quorum is " << quorum << dendl;

  stringstream ss;
  ss << "mon" << whoami << " won leader election with quorum " << quorum;
  logclient.log(LOG_INFO, ss);
  
  for (vector<Paxos*>::iterator p = paxos.begin(); p != paxos.end(); p++)
    (*p)->leader_init();
  for (vector<PaxosService*>::iterator p = paxos_service.begin(); p != paxos_service.end(); p++)
    (*p)->election_finished();

  resend_routed_requests();
} 

void Monitor::lose_election(epoch_t epoch, set<int> &q, int l) 
{
  state = STATE_PEON;
  mon_epoch = epoch;
  leader = l;
  quorum = q;
  dout(10) << "lose_election, epoch " << mon_epoch << " leader is mon" << leader
	   << " quorum is " << quorum << dendl;
  
  for (vector<Paxos*>::iterator p = paxos.begin(); p != paxos.end(); p++)
    (*p)->peon_init();
  for (vector<PaxosService*>::iterator p = paxos_service.begin(); p != paxos_service.end(); p++)
    (*p)->election_finished();

  resend_routed_requests();
}

void Monitor::handle_command(MMonCommand *m)
{
  if (ceph_fsid_compare(&m->fsid, &monmap->fsid)) {
    dout(0) << "handle_command on fsid " << m->fsid << " != " << monmap->fsid << dendl;
    reply_command(m, -EPERM, "wrong fsid", 0);
    return;
  }

  MonSession *session = m->get_session();
  if (!session ||
      !session->caps.check_privileges(PAXOS_MONMAP, MON_CAP_ALL)) {
    string rs = "Access denied";
    reply_command(m, -EACCES, rs, 0);
    return;
  }

  dout(0) << "handle_command " << *m << dendl;
  bufferlist rdata;
  string rs;
  int r = -EINVAL;
  if (!m->cmd.empty()) {
      dout(0) << "m->cmd[0]=" << m->cmd[0] << dendl;
    if (m->cmd[0] == "mds") {
      mdsmon()->dispatch(m);
      return;
    }
    if (m->cmd[0] == "osd") {
      osdmon()->dispatch(m);
      return;
    }
    if (m->cmd[0] == "pg") {
      pgmon()->dispatch(m);
      return;
    }
    if (m->cmd[0] == "mon") {
      monmon()->dispatch(m);
      return;
    }
    if (m->cmd[0] == "stop") {
      shutdown();
      reply_command(m, 0, "stopping", 0);
      return;
    }
    if (m->cmd[0] == "stop_cluster") {
      stop_cluster();
      reply_command(m, 0, "initiating cluster shutdown", 0);
      return;
    }

    if (m->cmd[0] == "_injectargs") {
      parse_config_option_string(m->cmd[1]);
      return;
    } 
    if (m->cmd[0] == "class") {
      classmon()->dispatch(m);
      return;
    }
    if (m->cmd[0] == "auth") {
      authmon()->dispatch(m);
      return;
    }
    rs = "unrecognized subsystem";
  } else 
    rs = "no command";

  reply_command(m, r, rs, rdata, 0);
}

void Monitor::reply_command(MMonCommand *m, int rc, const string &rs, version_t version)
{
  bufferlist rdata;
  reply_command(m, rc, rs, rdata, version);
}

void Monitor::reply_command(MMonCommand *m, int rc, const string &rs, bufferlist& rdata, version_t version)
{
  MMonCommandAck *reply = new MMonCommandAck(m->cmd, rc, rs, version);
  reply->set_data(rdata);
  send_reply(m, reply);
  m->put();
}


// ------------------------
// request/reply routing
//
// a client/mds/osd will connect to a random monitor.  we need to forward any
// messages requiring state updates to the leader, and then route any replies
// back via the correct monitor and back to them.  (the monitor will not
// initiate any connections.)

void Monitor::forward_request_leader(PaxosServiceMessage *req)
{
  int mon = get_leader();
  MonSession *session = 0;
  if (req->get_connection())
    session = (MonSession *)req->get_connection()->get_priv();
  if (req->session_mon >= 0) {
    dout(10) << "forward_request won't double fwd request " << *req << dendl;
    req->put();
  } else if (session && !session->closed) {
    RoutedRequest *rr = new RoutedRequest;
    rr->tid = ++routed_request_tid;

    dout(10) << "forward_request " << rr->tid << " request " << *req << dendl;

    dout(10) << " noting that i mon" << whoami << " own this requests's session" << dendl;
    //set forwarding variables; clear payload so it re-encodes properly
    req->session_mon = whoami;
    req->session_mon_tid = rr->tid;
    req->clear_payload();
    //of course, the need to forward does give it an effectively lower priority
    
    //encode forward message and insert into routed_requests
    encode_message(req, rr->request_bl);
    rr->session = (MonSession *)session->get();
    routed_requests[rr->tid] = rr;

    session->routed_request_tids.insert(rr->tid);
    
    MForward *forward = new MForward(req);
    forward->set_priority(req->get_priority());
    messenger->send_message(forward, monmap->get_inst(mon));
  } else {
    dout(10) << "forward_request no session for request " << *req << dendl;
    req->put();
  }
  if (session)
    session->put();
}

//extract the original message and put it into the regular dispatch function
void Monitor::handle_forward(MForward *m)
{
  dout(10) << "received forwarded message from " << m->msg->get_source_inst()
	   << " via " << m->get_source_inst() << dendl;
  MonSession *session = (MonSession *)m->get_connection()->get_priv();
  assert(session);

  if (!session->caps.check_privileges(PAXOS_MONMAP, MON_CAP_X)) {
    dout(0) << "forward from entity with insufficient caps! " 
	    << session->caps << dendl;
  } else {

    MonSession *s = new MonSession(m->msg->get_source_inst());
    s->caps = m->client_caps;
    Connection *c = new Connection;
    c->set_priv(s);

    PaxosServiceMessage *req = m->msg;
    m->msg = NULL;  // so ~MForward doesn't delete it
    req->set_connection(c);
    _ms_dispatch(req);
  }
  session->put();
  m->put();
}

void Monitor::try_send_message(Message *m, entity_inst_t to)
{
  dout(10) << "try_send_message " << *m << " to " << to << dendl;

  bufferlist bl;
  encode_message(m, bl);

  messenger->send_message(m, to);

  for (int i=0; i<(int)monmap->size(); i++) {
    if (i != whoami)
      messenger->send_message(new MRoute(0, bl, to),
			      monmap->get_inst(i));
  }
}

void Monitor::send_reply(PaxosServiceMessage *req, Message *reply, entity_inst_t to)
{
  if (req->session_mon >= 0) {
    if (req->session_mon < (int)monmap->size()) {
      dout(15) << "send_reply routing reply to " << to << " via mon" << req->session_mon
	       << " for request " << *req << dendl;
      messenger->send_message(new MRoute(req->session_mon_tid, reply, to),
			      monmap->get_inst(req->session_mon));
    } else
      dout(2) << "send_reply mon" << req->session_mon << " dne, dropping reply " << *reply
	      << " to " << *req << " for " << to << dendl;
  } else {
    messenger->send_message(reply, to);
  }
}

void Monitor::handle_route(MRoute *m)
{
  MonSession *session = (MonSession *)m->get_connection()->get_priv();
  //check privileges
  if (session && !session->caps.check_privileges(PAXOS_MONMAP, MON_CAP_X)) {
    dout(0) << "MRoute received from entity without appropriate perms! "
	    << dendl;
    session->put();
    m->put();
    return;
  }
  dout(10) << "handle_route " << *m->msg << " to " << m->dest << dendl;
  
  // look it up
  if (routed_requests.count(m->session_mon_tid)) {
    RoutedRequest *rr = routed_requests[m->session_mon_tid];
    messenger->send_message(m->msg, rr->session->inst);
    m->msg = NULL;
    routed_requests.erase(m->session_mon_tid);
    rr->session->routed_request_tids.insert(rr->tid);
    delete rr;
  } else {
    dout(10) << " don't have routed request tid " << m->session_mon_tid
	     << ", trying to send anyway" << dendl;
    messenger->lazy_send_message(m->msg, m->dest);
    m->msg = NULL;
  }
  m->put();
  if (session) session->put();
}

void Monitor::resend_routed_requests()
{
  dout(10) << "resend_routed_requests" << dendl;
  int mon = get_leader();
  for (map<uint64_t, RoutedRequest*>::iterator p = routed_requests.begin();
       p != routed_requests.end();
       p++) {
    RoutedRequest *rr = p->second;

    bufferlist::iterator q = rr->request_bl.begin();
    PaxosServiceMessage *req = (PaxosServiceMessage *)decode_message(q);

    dout(10) << " resend to mon" << mon << " tid " << rr->tid << " " << *req << dendl;
    MForward *forward = new MForward(req, rr->session->caps);
    forward->set_priority(req->get_priority());
    messenger->send_message(forward, monmap->get_inst(mon));
  }  
}

void Monitor::remove_session(MonSession *s)
{
  dout(10) << "remove_session " << s << " " << s->inst << dendl;
  assert(!s->closed);
  for (set<uint64_t>::iterator p = s->routed_request_tids.begin();
       p != s->routed_request_tids.end();
       p++) {
    if (routed_requests.count(*p)) {
      RoutedRequest *rr = routed_requests[*p];
      dout(10) << " dropping routed request " << rr->tid << dendl;
      delete rr;
      routed_requests.erase(*p);
    }
  }
  session_map.remove_session(s);
}


void Monitor::handle_observe(MMonObserve *m)
{
  dout(10) << "handle_observe " << *m << " from " << m->get_source_inst() << dendl;
  // check that there are perms. Send a response back if they aren't sufficient,
  // and delete the message (if it's not deleted for us, which happens when
  // we own the connection to the requested observer).
  MonSession *session = m->get_session();
  if (!session || !session->caps.check_privileges(PAXOS_MONMAP, MON_CAP_X)) {
    bool delete_m = false;
    if (m->session_mon)
      delete_m = true;
    send_reply(m, m);
    if (delete_m)
      m->put();
    return;
  }
  if (m->machine_id >= PAXOS_NUM) {
    dout(0) << "register_observer: bad monitor id: " << m->machine_id << dendl;
  } else {
    paxos[m->machine_id]->register_observer(m->get_orig_source_inst(), m->ver);
  }
  messenger->send_message(m, m->get_orig_source_inst());
}


void Monitor::inject_args(const entity_inst_t& inst, vector<string>& args, version_t version)
{
  dout(10) << "inject_args " << inst << " " << args << dendl;
  vector<string> new_args = args;
  new_args.insert(new_args.begin(), "injectargs");
  send_command(inst, new_args, version);
}

void Monitor::send_command(const entity_inst_t& inst,
			   const vector<string>& com, version_t version)
{
  dout(10) << "send_command " << inst << "" << com << dendl;
  MMonCommand *c = new MMonCommand(monmap->fsid, version);
  c->cmd = com;
  try_send_message(c, inst);
}


void Monitor::stop_cluster()
{
  dout(0) << "stop_cluster -- initiating shutdown" << dendl;
  stopping = true;
  mdsmon()->do_stop();
}


bool Monitor::_ms_dispatch(Message *m)
{
  bool ret = true;

  Connection *connection = m->get_connection();
  MonSession *s = NULL;
  bool reuse_caps = false;
  MonCaps caps;
  EntityName entity_name;
  bool src_is_mon;

  if (connection) {
    dout(20) << "have connection" << dendl;
    s = (MonSession *)connection->get_priv();
    if (s && s->closed) {
      caps = s->caps;
      reuse_caps = true;
      s->put();
      s = NULL;
    }
    if (!s) {
      dout(10) << "do not have session, making new one" << dendl;
      s = session_map.new_session(m->get_source_inst());
      m->get_connection()->set_priv(s->get());
      dout(10) << "ms_dispatch new session " << s << " for " << s->inst << dendl;

      if (m->get_connection()->get_peer_type() != CEPH_ENTITY_TYPE_MON) {
	dout(10) << "setting timeout on session" << dendl;
	// set an initial timeout here, so we will trim this session even if they don't
	// do anything.
	s->until = g_clock.now();
	s->until += g_conf.mon_subscribe_interval;
      } else {
	//give it monitor caps; the peer type has been authenticated
	reuse_caps = false;
	dout(5) << "setting monitor caps on this connection" << dendl;
	if (!s->caps.allow_all) //but no need to repeatedly copy
	  s->caps = *mon_caps;
      }
      if (reuse_caps)
        s->caps = caps;
    } else {
      dout(20) << "ms_dispatch existing session " << s << " for " << s->inst << dendl;
    }
    if (s->auth_handler) {
      entity_name = s->auth_handler->get_entity_name();
    }
  }
  src_is_mon = !connection || (connection->get_peer_type() & CEPH_ENTITY_TYPE_MON);

  if (s)
    dout(20) << " caps " << s->caps.get_str() << dendl;

  {
    switch (m->get_type()) {
      
    case MSG_ROUTE:
      handle_route((MRoute*)m);
      break;

      // misc
    case CEPH_MSG_MON_GET_MAP:
      handle_mon_get_map((MMonGetMap*)m);
      break;

    case MSG_MON_COMMAND:
      handle_command((MMonCommand*)m);
      break;

    case CEPH_MSG_MON_SUBSCRIBE:
      /* FIXME: check what's being subscribed, filter accordingly */
      handle_subscribe((MMonSubscribe*)m);
      break;

      // OSDs
    case MSG_OSD_FAILURE:
    case MSG_OSD_BOOT:
    case MSG_OSD_ALIVE:
    case MSG_OSD_PGTEMP:
      paxos_service[PAXOS_OSDMAP]->dispatch((PaxosServiceMessage*)m);
      break;

    case MSG_REMOVE_SNAPS:
      paxos_service[PAXOS_OSDMAP]->dispatch((PaxosServiceMessage*)m);
      break;

      // MDSs
    case MSG_MDS_BEACON:
    case MSG_MDS_OFFLOAD_TARGETS:
      paxos_service[PAXOS_MDSMAP]->dispatch((PaxosServiceMessage*)m);
      break;

      // auth
    case MSG_MON_GLOBAL_ID:
    case CEPH_MSG_AUTH:
      /* no need to check caps here */
      paxos_service[PAXOS_AUTH]->dispatch((PaxosServiceMessage*)m);
      break;

      // pg
    case CEPH_MSG_STATFS:
    case MSG_PGSTATS:
    case MSG_GETPOOLSTATS:
      paxos_service[PAXOS_PGMAP]->dispatch((PaxosServiceMessage*)m);
      break;

    case CEPH_MSG_POOLOP:
      paxos_service[PAXOS_OSDMAP]->dispatch((PaxosServiceMessage*)m);
      break;

      // log
    case MSG_LOG:
      paxos_service[PAXOS_LOG]->dispatch((PaxosServiceMessage*)m);
      break;

      // paxos
    case MSG_MON_PAXOS:
      {
	if (!src_is_mon && 
	    !s->caps.check_privileges(PAXOS_MONMAP, MON_CAP_X)) {
	  //can't send these!
	  m->put();
	  break;
	}

	MMonPaxos *pm = (MMonPaxos*)m;

	// sanitize
	if (pm->epoch > mon_epoch) 
	  call_election();
	if (pm->epoch != mon_epoch) {
	  pm->put();
	  break;
	}

	// send it to the right paxos instance
	assert(pm->machine_id < PAXOS_NUM);
	Paxos *p = paxos[pm->machine_id];
	p->dispatch((PaxosServiceMessage*)m);

	// make sure service finds out about any state changes
	if (p->is_active())
	  paxos_service[p->machine_id]->update_from_paxos();
      }
      break;

    case MSG_MON_OBSERVE:
      handle_observe((MMonObserve *)m);
      break;

      // elector messages
    case MSG_MON_ELECTION:
      //check privileges here for simplicity
      if (s &&
	  !s->caps.check_privileges(PAXOS_MONMAP, MON_CAP_X)) {
	dout(0) << "MMonElection received from entity without enough caps!"
		<< s->caps << dendl;
      }
      elector.dispatch(m);
      break;

    case MSG_CLASS:
      handle_class((MClass *)m);
      break;

    case MSG_FORWARD:
      handle_forward((MForward *)m);
      break;

    default:
      ret = false;
    }
  }
  if (s) {
    s->put();
  }

  return ret;
}

void Monitor::handle_subscribe(MMonSubscribe *m)
{
  dout(10) << "handle_subscribe " << *m << dendl;
  
  bool reply = false;

  MonSession *s = (MonSession *)m->get_connection()->get_priv();
  if (!s) {
    dout(10) << " no session, dropping" << dendl;
    m->put();
    return;
  }

  s->until = g_clock.now();
  s->until += g_conf.mon_subscribe_interval;
  for (map<string,ceph_mon_subscribe_item>::iterator p = m->what.begin();
       p != m->what.end();
       p++) {
    if (!p->second.onetime)
      reply = true;
    session_map.add_update_sub(s, p->first, p->second.have, p->second.onetime);
    if (p->first == "mdsmap") {
      if ((int)s->caps.check_privileges(PAXOS_MDSMAP, MON_CAP_R)) {
        mdsmon()->check_sub(s->sub_map["mdsmap"]);
      }
    } else if (p->first == "osdmap") {
      if ((int)s->caps.check_privileges(PAXOS_OSDMAP, MON_CAP_R)) {
        osdmon()->check_sub(s->sub_map["osdmap"]);
      }
    } else if (p->first == "monmap") {
      check_sub(s->sub_map["monmap"]);
    }
  }

  // ???

  if (reply)
    messenger->send_message(new MMonSubscribeAck(monmap->get_fsid(), (int)g_conf.mon_subscribe_interval),
			    m->get_source_inst());

  s->put();
  m->put();
}

bool Monitor::ms_handle_reset(Connection *con)
{
  // ignore lossless monitor sessions
  if (con->get_peer_type() == CEPH_ENTITY_TYPE_MON)
    return false;

  MonSession *s = (MonSession *)con->get_priv();
  if (!s)
    return false;

  Mutex::Locker l(lock);

  dout(10) << "reset/close on session " << s->inst << dendl;
  if (!s->closed)
    remove_session(s);
  s->put();
    
  // remove from connection, too.
  con->set_priv(NULL);
  return true;
}

void Monitor::check_subs()
{
  string type = "monmap";
  xlist<Subscription*>::iterator p = session_map.subs[type].begin();
  while (!p.end()) {
    Subscription *sub = *p;
    ++p;
    check_sub(sub);
  }
}

void Monitor::check_sub(Subscription *sub)
{
  dout(0) << "check_sub monmap last " << sub->last << " have " << monmap->get_epoch() << dendl;
  if (sub->last < monmap->get_epoch()) {
    send_latest_monmap(sub->session->inst);
    if (sub->onetime)
      session_map.remove_sub(sub);
    else
      sub->last = monmap->get_epoch();
  }
}


// -----

void Monitor::send_latest_monmap(entity_inst_t i)
{
  bufferlist bl;
  monmap->encode(bl);
  messenger->send_message(new MMonMap(bl), i);
}

void Monitor::handle_mon_get_map(MMonGetMap *m)
{
  dout(10) << "handle_mon_get_map" << dendl;
  send_latest_monmap(m->get_orig_source_inst());
  m->put();
}







/************ TICK ***************/

class C_Mon_Tick : public Context {
  Monitor *mon;
public:
  C_Mon_Tick(Monitor *m) : mon(m) {}
  void finish(int r) {
    mon->tick();
  }
};

void Monitor::cancel_tick()
{
  if (tick_timer) timer.cancel_event(tick_timer);
}

void Monitor::reset_tick()
{
  cancel_tick();
  tick_timer = new C_Mon_Tick(this);
  timer.add_event_after(g_conf.mon_tick_interval, tick_timer);
}


void Monitor::tick()
{
  tick_timer = 0;

  _dout_check_log();

  // ok go.
  dout(11) << "tick" << dendl;
  
  for (vector<PaxosService*>::iterator p = paxos_service.begin(); p != paxos_service.end(); p++)
    (*p)->tick();
  
  // trim sessions
  utime_t now = g_clock.now();
  xlist<MonSession*>::iterator p = session_map.sessions.begin();
  while (!p.end()) {
    MonSession *s = *p;
    ++p;
    
    // don't trim monitors
    if (s->inst.name.is_mon())
      continue; 

    if (!s->until.is_zero() && s->until < now) {
      dout(10) << " trimming session " << s->inst
	       << " (until " << s->until << " < now " << now << ")" << dendl;
      messenger->mark_down(s->inst.addr);
      remove_session(s);
    }
  }

  // next tick!
  reset_tick();
}





/*
 * this is the closest thing to a traditional 'mkfs' for ceph.
 * initialize the monitor state machines to their initial values.
 */
int Monitor::mkfs(bufferlist& osdmapbl)
{
  // create it
  int err = store->mkfs();
  if (err < 0) {
    char buf[80];
    cerr << "error " << err << " " << strerror_r(err, buf, sizeof(buf)) << std::endl;
    exit(1);
  }
  
  store->put_int(whoami, "whoami", 0);

  bufferlist magicbl;
  magicbl.append(CEPH_MON_ONDISK_MAGIC);
  magicbl.append("\n");
  store->put_bl_ss(magicbl, "magic", 0);

  bufferlist features;
  CompatSet mon_features(ceph_mon_feature_compat,
			 ceph_mon_feature_ro_compat,
			 ceph_mon_feature_incompat);
  mon_features.encode(features);
  store->put_bl_ss(features, COMPAT_SET_LOC, 0);

  bufferlist monmapbl;
  monmap->encode(monmapbl);

  for (vector<PaxosService*>::iterator p = paxos_service.begin(); p != paxos_service.end(); p++) {
    PaxosService *svc = *p;
    bufferlist bl;
    dout(10) << "initializing " << svc->get_machine_name() << dendl;
    svc->paxos->init();
    svc->create_pending();
    if (svc->paxos->machine_id == PAXOS_OSDMAP)
      svc->create_initial(osdmapbl);
    else if (svc->paxos->machine_id == PAXOS_MONMAP)
      svc->create_initial(monmapbl);
    else
      svc->create_initial(bl);
    // commit to paxos
    svc->encode_pending(bl);
    store->put_bl_sn(bl, svc->get_machine_name(), 1);
    store->put_int(1, svc->get_machine_name(), "last_committed");
  }

  // stash latest monmap
  bufferlist latest;
  version_t v = monmap->get_epoch();
  ::encode(v, latest);
  ::encode(monmapbl, latest);
  store->put_bl_ss(latest, "monmap", "latest");

  return 0;
}

void Monitor::handle_class(MClass *m)
{
  MonSession *session = m->get_session();
  if (!session)
    goto done;
  if (!session->caps.check_privileges(PAXOS_OSDMAP, MON_CAP_X)) {
    dout(0) << "MClass received from entity without sufficient privileges "
	    << session->caps << dendl;
    goto done;
  }

  switch (m->action) {
    case CLASS_SET:
    case CLASS_GET:
      classmon()->handle_request(m);
      return;

    case CLASS_RESPONSE:
      dout(0) << "got a class response (" << *m << ") ???" << dendl;
      break;

    default:
      dout(0) << "got an unknown class message (" << *m << ") ???" << dendl;
      break;
  }

 done:
  m->put();
}

bool Monitor::ms_get_authorizer(int service_id, AuthAuthorizer **authorizer, bool force_new)
{
  dout(10) << "ms_get_authorizer for " << ceph_entity_type_name(service_id) << dendl;

  // we only connect to other monitors; every else connects to us.
  if (service_id != CEPH_ENTITY_TYPE_MON)
    return false;

  if (!is_supported_auth(CEPH_AUTH_CEPHX))
    return false;

  CephXServiceTicketInfo auth_ticket_info;
  CephXSessionAuthInfo info;
  int ret;
  EntityName name;
  name.entity_type = CEPH_ENTITY_TYPE_MON;

  auth_ticket_info.ticket.name = name;
  auth_ticket_info.ticket.global_id = 0;

  CryptoKey secret;
  if (!key_server.get_secret(name, secret)) {
    dout(0) << " couldn't get secret for mon service" << dendl;
    stringstream ss;
    key_server.list_secrets(ss);
    dout(0) << ss.str() << dendl;
    return false;
  }

  /* mon to mon authentication uses the private monitor shared key and not the
     rotating key */
  ret = key_server.build_session_auth_info(service_id, auth_ticket_info, info, secret, (uint64_t)-1);
  if (ret < 0) {
    dout(0) << "ms_get_authorizer failed to build session auth_info for use with mon ret " << ret << dendl;
    return false;
  }

  CephXTicketBlob blob;
  if (!cephx_build_service_ticket_blob(info, blob)) {
    dout(0) << "ms_get_authorizer failed to build service ticket use with mon" << dendl;
    return false;
  }
  bufferlist ticket_data;
  ::encode(blob, ticket_data);

  bufferlist::iterator iter = ticket_data.begin();
  CephXTicketHandler handler;
  ::decode(handler.ticket, iter);

  handler.service_id = service_id;
  handler.session_key = info.session_key;

  *authorizer = handler.build_authorizer(0);
  
  return true;
}

bool Monitor::ms_verify_authorizer(Connection *con, int peer_type,
				   int protocol, bufferlist& authorizer_data, bufferlist& authorizer_reply,
				   bool& isvalid)
{
  dout(10) << "ms_verify_authorizer " << con->get_peer_addr()
	   << " " << ceph_entity_type_name(peer_type)
	   << " protocol " << protocol << dendl;

  if (peer_type == CEPH_ENTITY_TYPE_MON &&
      is_supported_auth(CEPH_AUTH_CEPHX)) {
    // monitor, and cephx is enabled
    isvalid = false;
    if (protocol == CEPH_AUTH_CEPHX) {
      bufferlist::iterator iter = authorizer_data.begin();
      CephXServiceTicketInfo auth_ticket_info;
      
      if (authorizer_data.length()) {
	int ret = cephx_verify_authorizer(&key_server, iter, auth_ticket_info, authorizer_reply);
	if (ret >= 0)
	  isvalid = true;
	else
	  dout(0) << "ms_verify_authorizer bad authorizer from mon " << con->get_peer_addr() << dendl;
      }
    } else {
      dout(0) << "ms_verify_authorizer cephx enabled, but no authorizer (required for mon)" << dendl;
    }
  } else {
    // who cares.
    isvalid = true;
  }
  return true;
};

