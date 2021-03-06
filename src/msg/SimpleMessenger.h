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

#ifndef __SIMPLEMESSENGER_H
#define __SIMPLEMESSENGER_H

#include "include/types.h"
#include "include/xlist.h"

#include <list>
#include <map>
using namespace std;
#include <ext/hash_map>
#include <ext/hash_set>
using namespace __gnu_cxx;

#include "common/Mutex.h"
#include "include/Spinlock.h"
#include "common/Cond.h"
#include "common/Thread.h"
#include "common/Throttle.h"

#include "Messenger.h"
#include "Message.h"
#include "tcp.h"


/*
 * This class handles transmission and reception of messages. Generally
 * speaking, there are 2 major components:
 * 1) Pipe. Each network connection is handled through a pipe, which handles
 *    the input and output of each message.
 * 2) SimpleMessenger. It's the exterior class passed to the external 
 *    message handler and handles queuing and ordering of pipes. Each
 *    pipe maintains its own message ordering, but the SimpleMessenger
 *    decides what order pipes get to deliver messages in.
 *
 * This class should only be created on the heap, and it should be destroyed
 * via a call to destroy(). Making it on the stack or otherwise calling
 * the destructor will lead to badness.
 */

class SimpleMessenger : public Messenger {
public:
  struct Policy {
    bool lossy;
    bool server;

    Policy(bool l=false, bool s=false) :
      lossy(l), server(s) {}

    static Policy stateful_server() { return Policy(false, true); }
    static Policy stateless_server() { return Policy(true, true); }
    static Policy lossless_peer() { return Policy(false, false); }
    static Policy client() { return Policy(false, false); }
  };


public:
  void sigint();

private:
  class Pipe;

  // incoming
  class Accepter : public Thread {
  public:
    SimpleMessenger *messenger;
    bool done;
    int listen_sd;
    
    Accepter(SimpleMessenger *r) : messenger(r), done(false), listen_sd(-1) {}
    
    void *entry();
    void stop();
    int bind(int64_t force_nonce);
    int start();
  } accepter;

  void sigint(int r);

  // pipe
  class Pipe {
  public:
    SimpleMessenger *messenger;
    ostream& _pipe_prefix();

    enum {
      STATE_ACCEPTING,
      STATE_CONNECTING,
      STATE_OPEN,
      STATE_STANDBY,
      STATE_CLOSED,
      STATE_CLOSING,
      STATE_WAIT       // just wait for racing connection
    };

    int sd;
    int peer_type;
    entity_addr_t peer_addr;
    Policy policy;
    
    Mutex pipe_lock;
    int state;

  protected:
    friend class SimpleMessenger;
    Connection *connection_state;

    utime_t backoff;         // backoff time

    bool reader_running, reader_joining;
    bool writer_running;

    map<int, list<Message*> > out_q;  // priority queue for outbound msgs
    map<int, list<Message*> > in_q; // and inbound ones
    int in_qlen;
    map<int, xlist<Pipe *>::item* > queue_items; // _map_ protected by pipe_lock, *item protected by q.lock
    list<Message*> sent;
    Cond cond;
    bool keepalive;
    
    __u32 connect_seq, peer_global_seq;
    uint64_t out_seq;
    uint64_t in_seq, in_seq_acked;
    
    int get_required_bits(); /* get bits this Messenger requires
			      * the peer to support */
    int get_supported_bits(); /* get bits this Messenger expects
			       * the peer to support */
    int accept();   // server handshake
    int connect();  // client handshake
    void reader();
    void writer();
    void unlock_maybe_reap();

    Message *read_message();
    int write_message(Message *m);
    int do_sendmsg(int sd, struct msghdr *msg, int len, bool more=false);
    int write_ack(uint64_t s);
    int write_keepalive();

    void fault(bool silent=false, bool reader=false);
    void fail();

    void was_session_reset();

    // threads
    class Reader : public Thread {
      Pipe *pipe;
    public:
      Reader(Pipe *p) : pipe(p) {}
      void *entry() { pipe->reader(); return 0; }
    } reader_thread;
    friend class Reader;

    class Writer : public Thread {
      Pipe *pipe;
    public:
      Writer(Pipe *p) : pipe(p) {}
      void *entry() { pipe->writer(); return 0; }
    } writer_thread;
    friend class Writer;
    
  public:
    Pipe(SimpleMessenger *r, int st) : 
      messenger(r),
      sd(-1), peer_type(-1),
      pipe_lock("SimpleMessenger::Pipe::pipe_lock"),
      state(st), 
      connection_state(new Connection),
      reader_running(false), reader_joining(false), writer_running(false),
      in_qlen(0), keepalive(false),
      connect_seq(0), peer_global_seq(0),
      out_seq(0), in_seq(0), in_seq_acked(0),
      reader_thread(this), writer_thread(this) {
      connection_state->pipe = this;
    }
    ~Pipe() {
      for (map<int, xlist<Pipe *>::item* >::iterator i = queue_items.begin();
	   i != queue_items.end();
	   ++i) {
	if (i->second->is_on_list())
	  i->second->remove_myself();
	delete i->second;
      }
      assert(out_q.empty());
      assert(sent.empty());
      connection_state->pipe = NULL;
      connection_state->put();
    }


    void start_reader() {
      assert(pipe_lock.is_locked());
      assert(!reader_running);
      reader_running = true;
      reader_thread.create();
    }
    void start_writer() {
      assert(pipe_lock.is_locked());
      assert(!writer_running);
      writer_running = true;
      writer_thread.create();
    }
    void join_reader() {
      if (!reader_running)
	return;
      assert(!reader_joining);
      reader_joining = true;
      cond.Signal();
      pipe_lock.Unlock();
      reader_thread.join();
      pipe_lock.Lock();
      assert(reader_joining);
      reader_joining = false;
    }

    // public constructors
    static const Pipe& Server(int s);
    static const Pipe& Client(const entity_addr_t& pi);

    //callers make sure it's not already enqueued or you'll just
    //push it to the back of the line!
    //Also, call with pipe_lock held or bad things happen
    void enqueue_me(int priority) {
      if (!queue_items.count(priority))
	queue_items[priority] = new xlist<Pipe *>::item(this);
      pipe_lock.Unlock();
      messenger->dispatch_queue.lock.Lock();
      if (messenger->dispatch_queue.queued_pipes.empty())
	messenger->dispatch_queue.cond.Signal();
      messenger->dispatch_queue.queued_pipes[priority].push_back(queue_items[priority]);
      messenger->dispatch_queue.lock.Unlock();
      pipe_lock.Lock();
    }

    //we have two queue_received's to allow local signal delivery
    // via Message * (that doesn't actually point to a Message)
    //Don't call while holding pipe_lock!
    void queue_received(Message *m, int priority) {
      list<Message *>& queue = in_q[priority];

      pipe_lock.Lock();
      bool was_empty = queue.empty();
      queue.push_back(m);
      if (was_empty) //this pipe isn't on the endpoint queue
	enqueue_me(priority);

      //increment queue length counters
      in_qlen++;
      messenger->dispatch_queue.qlen_lock.lock();
      ++messenger->dispatch_queue.qlen;
      messenger->dispatch_queue.qlen_lock.unlock();

      pipe_lock.Unlock();
    }
    
    void queue_received(Message *m) {
      m->set_recv_stamp(g_clock.now());

      // this is just to make sure that a changeset is working
      // properly; if you start using the refcounting more and have
      // multiple people hanging on to a message, ditch the assert!
      assert(m->nref.read() == 1); 

      queue_received(m, m->get_priority());
    }

    __u32 get_out_seq() { return out_seq; }

    bool is_queued() { return !out_q.empty() || keepalive; }

    entity_addr_t& get_peer_addr() { return peer_addr; }

    void set_peer_addr(const entity_addr_t& a) {
      if (&peer_addr != &a)  // shut up valgrind
	peer_addr = a;
      connection_state->set_peer_addr(a);
    }
    void set_peer_type(int t) {
      peer_type = t;
      connection_state->set_peer_type(t);
    }

    void register_pipe();
    void unregister_pipe();
    void join() {
      if (writer_thread.is_started())
	writer_thread.join();
      if (reader_thread.is_started())
	reader_thread.join();
    }
    void stop();

    void send(Message *m) {
      pipe_lock.Lock();
      _send(m);
      pipe_lock.Unlock();
    }    
    void _send(Message *m) {
      out_q[m->get_priority()].push_back(m);
      cond.Signal();
    }
    void send_keepalive() {
      pipe_lock.Lock();
      _send_keepalive();
      pipe_lock.Unlock();
    }    
    void _send_keepalive() {
      keepalive = true;
      cond.Signal();
    }
    Message *_get_next_outgoing() {
      Message *m = 0;
      while (!m && !out_q.empty()) {
	map<int, list<Message*> >::reverse_iterator p = out_q.rbegin();
	if (!p->second.empty()) {
	  m = p->second.front();
	  p->second.pop_front();
	}
	if (p->second.empty())
	  out_q.erase(p->first);
      }
      return m;
    }

    void requeue_sent();
    void discard_queue();

    void force_close() {
      if (sd >= 0) ::close(sd);
    }
  };


  struct DispatchQueue {
    Mutex lock;
    Cond cond;
    bool stop;

    map<int, xlist<Pipe *> > queued_pipes;
    map<int, xlist<Pipe *>::iterator> queued_pipe_iters;
    int qlen;
    Spinlock qlen_lock;
    
    enum { D_CONNECT, D_BAD_REMOTE_RESET, D_BAD_RESET };
    list<Connection*> connect_q;
    list<Connection*> remote_reset_q;
    list<Connection*> reset_q;

    Pipe *local_pipe;
    void local_delivery(Message *m, int priority) {
      if ((unsigned long)m > 10)
	m->set_connection(local_pipe->connection_state->get());
      local_pipe->queue_received(m, priority);
    }

    int get_queue_len() {
      qlen_lock.lock();
      int l = qlen;
      qlen_lock.unlock();
      return l;
    }
    
    void local_delivery(Message *m) {
      if ((unsigned long)m > 10)
	m->set_connection(local_pipe->connection_state->get());
      local_pipe->queue_received(m);
    }

    void queue_connect(Connection *con) {
      lock.Lock();
      connect_q.push_back(con);
      lock.Unlock();
      local_delivery((Message*)D_CONNECT, CEPH_MSG_PRIO_HIGHEST);
    }
    void queue_remote_reset(Connection *con) {
      lock.Lock();
      remote_reset_q.push_back(con);
      lock.Unlock();
      local_delivery((Message*)D_BAD_REMOTE_RESET, CEPH_MSG_PRIO_HIGHEST);
    }
    void queue_reset(Connection *con) {
      lock.Lock();
      reset_q.push_back(con);
      lock.Unlock();
      local_delivery((Message*)D_BAD_RESET, CEPH_MSG_PRIO_HIGHEST);
    }
    
    DispatchQueue() :
      lock("SimpleMessenger::DispatchQeueu::lock"), 
      stop(false),
      qlen(0),
      qlen_lock("SimpleMessenger::DispatchQueue::qlen_lock"),
      local_pipe(NULL)
    {}
    ~DispatchQueue() {
      for (map< int, xlist<Pipe *> >::iterator i = queued_pipes.begin();
	   i != queued_pipes.end();
	   ++i) {
	i->second.clear();
      }
    }
  } dispatch_queue;

  // SimpleMessenger stuff
 public:
  Mutex lock;
  Cond  wait_cond;  // for wait()
  bool started;
  bool did_bind;
  Throttle message_throttler;

  // where i listen
  bool need_addr;
  entity_addr_t ms_addr;
  
  // local
  bool destination_stopped;
  
  // remote
  hash_map<entity_addr_t, Pipe*> rank_pipe;
 
  int my_type;
  Policy default_policy;
  map<int, Policy> policy_map; // entity_name_t::type -> Policy

  set<Pipe*>      pipes;
  list<Pipe*>     pipe_reap_queue;
  
  Mutex global_seq_lock;
  __u32 global_seq;
      
  Pipe *connect_rank(const entity_addr_t& addr, int type);

  const entity_addr_t &get_ms_addr() { return ms_addr; }

  void mark_down(const entity_addr_t& addr);

  void reaper();

  Policy get_policy(int t) {
    if (policy_map.count(t))
      return policy_map[t];
    else
      return default_policy;
  }

  /***** Messenger-required functions  **********/
  void destroy() {
    if (dispatch_thread.is_started())
      dispatch_thread.join();
    Messenger::destroy();
  }

  entity_addr_t get_myaddr();

  int get_dispatch_queue_len() {
    return dispatch_queue.get_queue_len();
  }

  void ready();
  int shutdown();
  void suicide();
  void prepare_dest(const entity_inst_t& inst);
  int send_message(Message *m, const entity_inst_t& dest);
  int send_message(Message *m, Connection *con);
  int lazy_send_message(Message *m, const entity_inst_t& dest);
  int lazy_send_message(Message *m, Connection *con) {
    return send_message(m, con);
  }

  /***********************/

private:
  class DispatchThread : public Thread {
    SimpleMessenger *messenger;
  public:
    DispatchThread(SimpleMessenger *_messenger) : messenger(_messenger) {}
    void *entry() {
      messenger->get();
      messenger->dispatch_entry();
      messenger->put();
      return 0;
    }
  } dispatch_thread;

  void dispatch_entry();

  SimpleMessenger *messenger; //hack to make dout macro work, will fix

public:
  SimpleMessenger() :
    Messenger(entity_name_t()),
    accepter(this),
    lock("SimpleMessenger::lock"), started(false), did_bind(false),
    message_throttler(g_conf.ms_waiting_message_bytes), need_addr(true),
    destination_stopped(true), my_type(-1),
    global_seq_lock("SimpleMessenger::global_seq_lock"), global_seq(0),
    dispatch_thread(this), messenger(this) {
    // for local dmsg delivery
    dispatch_queue.local_pipe = new Pipe(this, Pipe::STATE_OPEN);
  }
  ~SimpleMessenger() {
    delete dispatch_queue.local_pipe;
  }

  //void set_listen_addr(tcpaddr_t& a);

  int bind(int64_t force_nonce = -1);
  int start(bool nodaemon = false);
  void wait();

  __u32 get_global_seq(__u32 old=0) {
    Mutex::Locker l(global_seq_lock);
    if (old > global_seq)
      global_seq = old;
    return ++global_seq;
  }

  AuthAuthorizer *get_authorizer(int peer_type, bool force_new);
  bool verify_authorizer(Connection *con, int peer_type, int protocol, bufferlist& auth, bufferlist& auth_reply,
			 bool& isvalid);

  bool register_entity(entity_name_t addr);

  void submit_message(Message *m, const entity_addr_t& addr, int dest_type, bool lazy);
  void submit_message(Message *m, Pipe *pipe);
		      
  int send_keepalive(const entity_inst_t& addr);

  void learned_addr(entity_addr_t peer_addr_for_me);
  void init_local_pipe();

  void set_default_policy(Policy p) {
    default_policy = p;
  }
  void set_policy(int type, Policy p) {
    policy_map[type] = p;
  }
} ;

#endif
