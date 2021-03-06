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


#ifndef __CLIENT_TRACE_H
#define __CLIENT_TRACE_H

#include <stdlib.h>

#include <list>
#include <string>
#include <fstream>
using std::list;
using std::string;
using std::ifstream;

/*

 this class is more like an iterator over a constant tokenlist (which 
 is protected by a mutex, see Trace.cc)

 */

class Trace {
  int _line;
  const char *filename;
  ifstream *fs;
  string line;

 public:
  Trace(const char* f) : filename(f), fs(0) {}
  ~Trace() { 
    delete fs; 
  }

  int get_line() { return _line; }

  void start();

  const char *peek_string(char *buf, const char *prefix);
  const char *get_string(char *buf, const char *prefix);

  __int64_t get_int() {
    char buf[20];
    return atoll(get_string(buf, 0));
  }
  bool end() {
    return !fs || fs->eof();
    //return _cur == _end;
  }
};

#endif
