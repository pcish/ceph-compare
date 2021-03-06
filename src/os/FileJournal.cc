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

#include "config.h"
#include "FileJournal.h"
#include "include/color.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <fcntl.h>


#define DOUT_SUBSYS journal
#undef dout_prefix
#define dout_prefix *_dout << dbeginl << "journal "


int FileJournal::_open(bool forwrite, bool create)
{
  char buf[80];
  int flags;

  if (forwrite) {
    flags = O_RDWR;
    if (directio) flags |= O_DIRECT | O_SYNC;
  } else {
    flags = O_RDONLY;
  }
  if (create)
    flags |= O_CREAT;
  
  if (fd >= 0) 
    ::close(fd);
  fd = ::open(fn.c_str(), flags, 0644);
  if (fd < 0) {
    dout(2) << "_open failed " << errno << " " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    return -errno;
  }

  // get size
  struct stat st;
  int r = ::fstat(fd, &st);
  if (create && ((r < 0 && errno == ENOENT) ||
		 (r == 0 && st.st_size < (g_conf.osd_journal_size << 20)))) {
    uint64_t newsize = g_conf.osd_journal_size << 20;
    dout(10) << "_open extending to " << newsize << " bytes" << dendl;
    r = ::ftruncate(fd, newsize);
    if (r < 0)
      return -errno;
    r = ::fstat(fd, &st);
  }
  if (r < 0)
    return -errno;
  max_size = st.st_size;
  block_size = st.st_blksize;

  if (max_size == 0) {
    // hmm, is this a raw block device?
#ifdef BLKGETSIZE64
    // ioctl block device
    uint64_t bytes;
    r = ::ioctl(fd, BLKGETSIZE64, &bytes);
    max_size = bytes;
#else
# ifdef BLKGETSIZE
    // hrm, try the 32 bit ioctl?
    unsigned long sectors = 0;
    r = ioctl(fd, BLKGETSIZE, &sectors);
    max_size = sectors * 512ULL;
# endif
#endif
    if (r < 0)
      return -errno;
    is_bdev = true;
  }

  max_size -= max_size % block_size;

  // try to check if the disk write cache is on
  if (is_bdev) {
    if (geteuid() == 0) {
      char cmd[4096];
      snprintf(cmd, sizeof(cmd), "/sbin/hdparm -W %s > /tmp/out.%d", fn.c_str(), getpid());
      int r = system(cmd);
      if (r == 0) {
	snprintf(cmd, sizeof(cmd), "/tmp/out.%d", getpid());
	FILE *f = fopen(cmd, "r");
	if (f) {
	  while (!feof(f)) {
	    char s[100];
	    fgets(s, sizeof(s), f);
	    int on;
	    if (sscanf(s, " write-caching =  %d", &on) == 1) {
	      if (on) {
		dout(0) << "WARNING: disk write cache is ON, journaling will not be reliable" << dendl;
		dout(0) << "         disable with 'hdparm -W 0 " << fn << "'" << dendl;
		cout << TEXT_RED
		     << " ** WARNING: disk write cache is ON on " << fn << ".\n"
		     << "    Journaling will not be reliable.  Disable write cache with\n"
		     << "    'hdparm -W 0 " << fn << "'"
		     << TEXT_NORMAL
		     << std::endl;
	      } else {
		dout(10) << "_open disk write cache is off (good) on " << fn << dendl;
	      }
	      break;
	    }
	  }
	  fclose(f);
	}
	::unlink(cmd);
      } else
	dout(10) << "_open failed to run '" << cmd << "', NOT checking disk write cache on " << fn << dendl;      
    } else
      dout(10) << "_open not root, NOT checking disk write cache on " << fn << dendl;
  } else
    dout(10) << "_open journal is not a block device, NOT checking disk write cache on " << fn << dendl;

  // static zeroed buffer for alignment padding
  zero_buf = new char[header.alignment];
  memset(zero_buf, 0, header.alignment);

  dout(2) << "_open " << fn << " fd " << fd 
	  << ": " << max_size 
	  << " bytes, block size " << block_size
	  << " bytes, directio = " << directio << dendl;

  return 0;
}

int FileJournal::create()
{
  char buf[80];
  dout(2) << "create " << fn << dendl;

  int err = _open(true, true);
  if (err < 0)
    return err;

  // write empty header
  memset(&header, 0, sizeof(header));
  header.clear();
  header.fsid = fsid;
  header.max_size = max_size;
  header.block_size = block_size;
  if (g_conf.journal_block_align || directio)
    header.alignment = block_size;
  else
    header.alignment = 16;  // at least stay word aligned on 64bit machines...
  header.start = get_top();
  print_header();

  buffer::ptr bp = prepare_header();
  int r = ::pwrite(fd, bp.c_str(), bp.length(), 0);
  if (r < 0) {
    dout(0) << "create write header error " << errno << " " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    return -errno;
  }

  // zero first little bit, too.
  char z[block_size];
  memset(z, 0, block_size);
  ::pwrite(fd, z, block_size, get_top());

  ::close(fd);
  fd = -1;
  dout(2) << "create done" << dendl;
  return 0;
}

int FileJournal::open(uint64_t next_seq)
{
  dout(2) << "open " << fn << " next_seq " << next_seq << dendl;

  int err = _open(false);
  if (err < 0) 
    return err;

  // assume writeable, unless...
  read_pos = 0;
  write_pos = get_top();

  // read header?
  read_header();
  dout(10) << "open header.fsid = " << header.fsid 
    //<< " vs expected fsid = " << fsid 
	   << dendl;
  if (header.fsid != fsid) {
    dout(2) << "open fsid doesn't match, invalid (someone else's?) journal" << dendl;
    err = -EINVAL;
  } 
  if (header.max_size > max_size) {
    dout(2) << "open journal size " << header.max_size << " > current " << max_size << dendl;
    err = -EINVAL;
  }
  if (header.block_size != block_size) {
    dout(2) << "open journal block size " << header.block_size << " != current " << block_size << dendl;
    err = -EINVAL;
  }
  if (header.max_size % header.block_size) {
    dout(2) << "open journal max size " << header.max_size
	    << " not a multiple of block size " << header.block_size << dendl;
    err = -EINVAL;
  }
  if (header.alignment != block_size && directio) {
    derr(0) << "open journal alignment " << header.alignment << " does not match block size " 
	    << block_size << " (required for direct_io journal mode)" << dendl;
    err = -EINVAL;
  }
  if (err)
    return err;

  // looks like a valid header.
  write_pos = 0;  // not writeable yet

  // find next entry
  read_pos = header.start;
  uint64_t seq = 0;
  while (1) {
    bufferlist bl;
    off64_t old_pos = read_pos;
    if (!read_entry(bl, seq)) {
      dout(10) << "open reached end of journal." << dendl;
      break;
    }
    if (seq > next_seq) {
      dout(10) << "open entry " << seq << " len " << bl.length() << " > next_seq " << next_seq << dendl;
      read_pos = -1;
      return 0;
    }
    if (seq == next_seq) {
      dout(10) << "open reached seq " << seq << dendl;
      read_pos = old_pos;
      break;
    }
    seq++;  // next event should follow.
  }

  return 0;
}

void FileJournal::close()
{
  dout(1) << "close " << fn << dendl;

  // stop writer thread
  stop_writer();

  // close
  assert(writeq.empty());
  assert(fd > 0);
  ::close(fd);
  fd = -1;
}

void FileJournal::start_writer()
{
  write_stop = false;
  write_thread.create();
}

void FileJournal::stop_writer()
{
  write_lock.Lock();
  {
    write_stop = true;
    write_cond.Signal();
  } 
  write_lock.Unlock();
  write_thread.join();
}



void FileJournal::print_header()
{
  dout(10) << "header: block_size " << header.block_size
	   << " alignment " << header.alignment
	   << " max_size " << header.max_size
	   << dendl;
  dout(10) << "header: start " << header.start << dendl;
  dout(10) << " write_pos " << write_pos << dendl;
}

void FileJournal::read_header()
{
  int r;
  dout(10) << "read_header" << dendl;
  if (directio) {
    buffer::ptr bp = buffer::create_page_aligned(block_size);
    bp.zero();
    r = ::pread(fd, bp.c_str(), bp.length(), 0);
    memcpy(&header, bp.c_str(), sizeof(header));
  } else {
    memset(&header, 0, sizeof(header));  // zero out (read may fail)
    r = ::pread(fd, &header, sizeof(header), 0);
  }
  if (r < 0) {
    char buf[80];
    dout(0) << "read_header error " << errno << " " << strerror_r(errno, buf, sizeof(buf)) << dendl;
  }
  print_header();
}

bufferptr FileJournal::prepare_header()
{
  bufferptr bp = buffer::create_page_aligned(get_top());
  bp.zero();
  memcpy(bp.c_str(), &header, sizeof(header));
  return bp;
}



int FileJournal::check_for_full(uint64_t seq, off64_t pos, off64_t size)
{
  // already full?
  if (full_commit_seq || full_restart_seq)
    return -ENOSPC;

  // take 1 byte off so that we only get pos == header.start on EMPTY, never on FULL.
  off64_t room;
  if (pos >= header.start)
    room = (header.max_size - pos) + (header.start - get_top()) - 1;
  else
    room = header.start - pos - 1;
  dout(10) << "room " << room << " max_size " << max_size << " pos " << pos << " header.start " << header.start
	   << " top " << get_top() << dendl;

  if (do_sync_cond) {
    if (room < (header.max_size >> 1) &&
	room + size > (header.max_size >> 1))
      do_sync_cond->Signal();  // initiate a real commit so we can trim
  }

  if (room >= size) {
    dout(10) << "check_for_full at " << pos << " : " << size << " < " << room << dendl;
    if (pos + size > header.max_size)
      must_write_header = true;
    return 0;
  }

  // full
  dout(1) << "check_for_full at " << pos << " : JOURNAL FULL "
	  << pos << " >= " << room
	  << " (max_size " << header.max_size << " start " << header.start << ")"
	  << dendl;

  if (wait_on_full)
    return -ENOSPC;

  // throw out what we have so far
  full_commit_seq = seq;
  full_restart_seq = seq+1;
  while (!writeq.empty()) {
    if (writeq.front().fin) {
      writing_seq.push_back(writeq.front().seq);
      writing_fin.push_back(writeq.front().fin);
    }
    throttle_ops.put(1);
    throttle_bytes.put(writeq.front().bl.length());
    writeq.pop_front();
  }  
  print_header();
  return -ENOSPC;
}

int FileJournal::prepare_multi_write(bufferlist& bl, uint64_t& orig_ops, uint64_t& orig_bytes)
{
  // gather queued writes
  off64_t queue_pos = write_pos;

  int eleft = g_conf.journal_max_write_entries;
  unsigned bmax = g_conf.journal_max_write_bytes;

  if (full_commit_seq || full_restart_seq)
    return -ENOSPC;
  
  while (!writeq.empty()) {
    int r = prepare_single_write(bl, queue_pos, orig_ops, orig_bytes);
    if (r == -ENOSPC) {
      if (orig_ops)
	break;         // commit what we have
      dout(20) << "prepare_multi_write full on first entry, need to wait" << dendl;
      return -ENOSPC;  // hrm, full on first op
    }

    if (eleft) {
      if (--eleft == 0) {
	dout(20) << "prepare_multi_write hit max events per write " << g_conf.journal_max_write_entries << dendl;
	break;
      }
    }
    if (bmax) {
      if (bl.length() >= bmax) {
	dout(20) << "prepare_multi_write hit max write size " << g_conf.journal_max_write_bytes << dendl;
	break;
      }
    }
  }

  //assert(write_pos + bl.length() == queue_pos);
  return 0;
}

int FileJournal::prepare_single_write(bufferlist& bl, off64_t& queue_pos, uint64_t& orig_ops, uint64_t& orig_bytes)
{
  // grab next item
  uint64_t seq = writeq.front().seq;
  bufferlist &ebl = writeq.front().bl;
  off64_t base_size = 2*sizeof(entry_header_t) + ebl.length();
  off64_t size = ROUND_UP_TO(base_size, header.alignment);

  int r = check_for_full(seq, queue_pos, size);
  if (r < 0)
    return r;   // ENOSPC or EAGAIN

  orig_bytes += ebl.length();
  orig_ops++;

  // add to write buffer
  dout(15) << "prepare_single_write " << orig_ops << " will write " << queue_pos << " : seq " << seq
	   << " len " << ebl.length() << " -> " << size
	   << dendl;
    
  // add it this entry
  entry_header_t h;
  h.seq = seq;
  h.pre_pad = 0;
  h.len = ebl.length();
  h.post_pad = size - base_size;
  h.make_magic(queue_pos, header.fsid);

  bl.append((const char*)&h, sizeof(h));
  bl.claim_append(ebl);
  if (h.post_pad) {
    bufferptr bp = buffer::create_static(h.post_pad, zero_buf);
    bl.push_back(bp);
  }
  bl.append((const char*)&h, sizeof(h));
   
  if (writeq.front().fin) {
    writing_seq.push_back(seq);
    writing_fin.push_back(writeq.front().fin);
  }

  // pop from writeq
  writeq.pop_front();
  journalq.push_back(pair<uint64_t,off64_t>(seq, queue_pos));

  queue_pos += size;
  if (queue_pos > header.max_size)
    queue_pos = queue_pos + get_top() - header.max_size;

  return 0;
}

void FileJournal::write_bl(off64_t& pos, bufferlist& bl)
{
  // make sure this is a single, contiguous buffer
  if (directio && !bl.is_contiguous()) {
    bl.rebuild();
    assert((bl.length() & ~PAGE_MASK) == 0);
  }

  ::lseek64(fd, pos, SEEK_SET);

  for (list<bufferptr>::const_iterator it = bl.buffers().begin();
       it != bl.buffers().end();
       it++) {
    if ((*it).length() == 0)
      continue;  // blank buffer.
    int r = ::write(fd, (char*)(*it).c_str(), (*it).length());
    if (r < 0) {
      char buf[80];
      derr(0) << "do_write failed with " << errno << " " << strerror_r(errno, buf, sizeof(buf)) 
	      << " with " << (void*)(*it).c_str() << " len " << (*it).length()
	      << dendl;
    }
    pos += (*it).length();
  }
}

void FileJournal::do_write(bufferlist& bl)
{
  // nothing to do?
  if (bl.length() == 0 && !must_write_header) 
    return;

  buffer::ptr hbp;
  if (must_write_header) {
    must_write_header = false;
    hbp = prepare_header();
  }

  writing = true;

  header_t old_header = header;

  write_lock.Unlock();

  dout(15) << "do_write writing " << write_pos << "~" << bl.length() 
	   << (hbp.length() ? " + header":"")
	   << dendl;
  
  utime_t from = g_clock.now();

  // entry
  off64_t pos = write_pos;

  // split?
  off64_t split = 0;
  if (pos + bl.length() > header.max_size) {
    bufferlist first, second;
    split = header.max_size - pos;
    first.substr_of(bl, 0, split);
    second.substr_of(bl, split, bl.length() - split);
    assert(first.length() + second.length() == bl.length());
    dout(10) << "do_write wrapping, first bit at " << pos << " len " << first.length()
	     << " second bit len " << second.length() << " (orig len " << bl.length() << ")" << dendl;

    write_bl(pos, first);
    assert(pos == header.max_size);
    if (hbp.length()) {
      // be sneaky: include the header in the second fragment
      second.push_front(hbp);
      pos = 0;          // we included the header
    } else
      pos = get_top();  // no header, start after that
    write_bl(pos, second);
  } else {
    // header too?
    if (hbp.length())
      ::pwrite(fd, hbp.c_str(), hbp.length(), 0);

    write_bl(pos, bl);
  }

  if (!directio) {
    dout(20) << "do_write fsync" << dendl;
#ifdef DARWIN
    ::fsync(fd);
#else
# ifdef HAVE_SYNC_FILE_RANGE
    if (is_bdev) {
      if (split) {
	::sync_file_range(fd, header.max_size - split, split, SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE);
	::sync_file_range(fd, get_top(), bl.length() - split, SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE);
	::sync_file_range(fd, header.max_size - split, split, SYNC_FILE_RANGE_WAIT_AFTER);
	::sync_file_range(fd, get_top(), bl.length() - split, SYNC_FILE_RANGE_WAIT_AFTER);
      } else {
	::sync_file_range(fd, write_pos, bl.length(),
			  SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE|SYNC_FILE_RANGE_WAIT_AFTER);
      }
    } else
# endif
      ::fdatasync(fd);
#endif
  }

  utime_t lat = g_clock.now() - from;    
  dout(20) << "do_write latency " << lat << dendl;

  write_lock.Lock();    

  writing = false;

  write_pos = pos;
  assert(write_pos % header.alignment == 0);

  // kick finisher?  
  //  only if we haven't filled up recently!
  if (full_commit_seq || full_restart_seq) {
    dout(10) << "do_write NOT queueing finisher seq " << writing_seq.front()
	     << ", full_commit_seq|full_restart_seq" << dendl;
  } else {
    dout(20) << "do_write queueing finishers " << writing_fin << dendl;
    writing_seq.clear();
    finisher->queue(writing_fin);
  }
}

void FileJournal::flush()
{
  write_lock.Lock();
  while ((!writeq.empty() || writing) && !write_stop) {
    dout(10) << "flush waiting for writeq to empty and writes to complete" << dendl;
    write_empty_cond.Wait(write_lock);
  }
  write_lock.Unlock();
  dout(10) << "flush waiting for finisher" << dendl;
  finisher->wait_for_empty();
  dout(10) << "flush done" << dendl;
}


void FileJournal::write_thread_entry()
{
  dout(10) << "write_thread_entry start" << dendl;
  write_lock.Lock();
  
  while (!write_stop || 
	 !writeq.empty()) {  // ensure we fully flush the writeq before stopping
    if (writeq.empty()) {
      // sleep
      dout(20) << "write_thread_entry going to sleep" << dendl;
      write_empty_cond.Signal();
      write_cond.Wait(write_lock);
      dout(20) << "write_thread_entry woke up" << dendl;
      continue;
    }
    
    uint64_t orig_ops = 0;
    uint64_t orig_bytes = 0;

    bufferlist bl;
    int r = prepare_multi_write(bl, orig_ops, orig_bytes);
    if (r == -ENOSPC) {
      dout(20) << "write_thread_entry full, going to sleep (waiting for commit)" << dendl;
      commit_cond.Wait(write_lock);
      dout(20) << "write_thread_entry woke up" << dendl;
      continue;
    }
    assert(r == 0);
    do_write(bl);
    
    uint64_t new_ops = throttle_ops.put(orig_ops);
    uint64_t new_bytes = throttle_bytes.put(orig_bytes);
    dout(10) << "write_thread throttle finished " << orig_ops << " ops and " 
	     << orig_bytes << " bytes, now "
	     << new_ops << " ops and " << new_bytes << " bytes"
	     << dendl;
  }
  write_empty_cond.Signal();
  write_lock.Unlock();
  dout(10) << "write_thread_entry finish" << dendl;
}


void FileJournal::submit_entry(uint64_t seq, bufferlist& e, Context *oncommit)
{
  Mutex::Locker locker(write_lock);  // ** lock **

  // dump on queue
  dout(10) << "submit_entry seq " << seq
	   << " len " << e.length()
	   << " (" << oncommit << ")" << dendl;

  throttle_ops.take(1);
  throttle_bytes.take(e.length());
  
  if (!full_commit_seq && full_restart_seq && 
      seq >= full_restart_seq) {
    dout(1) << " seq " << seq << " >= full_restart_seq " << full_restart_seq 
	     << ", restarting journal" << dendl;
    full_restart_seq = 0;
  }
  if (!full_commit_seq && !full_restart_seq) {
    writeq.push_back(write_item(seq, e, oncommit));
    write_cond.Signal(); // kick writer thread
  } else {
    // not journaling this.  restart writing no sooner than seq + 1.
    full_restart_seq = seq+1;
    dout(10) << " journal is/was full, will restart no sooner than seq " << full_restart_seq << dendl;
    if (oncommit) {
      writing_seq.push_back(seq);
      writing_fin.push_back(oncommit);
    }
  }
}


void FileJournal::committed_thru(uint64_t seq)
{
  Mutex::Locker locker(write_lock);

  if (seq < last_committed_seq) {
    dout(10) << "committed_thru " << seq << " < last_committed_seq " << last_committed_seq << dendl;
    assert(seq >= last_committed_seq);
    return;
  }
  if (seq == last_committed_seq) {
    dout(10) << "committed_thru " << seq << " == last_committed_seq " << last_committed_seq << dendl;
    return;
  }

  dout(10) << "committed_thru " << seq << " (last_committed_seq " << last_committed_seq << ")" << dendl;
  last_committed_seq = seq;

  // was full?
  if (full_commit_seq && seq >= full_commit_seq) {
    dout(1) << " seq " << seq << " >= full_commit_seq " << full_commit_seq 
	     << ", prior journal contents are now fully committed.  resetting journal." << dendl;
    full_commit_seq = 0;
  }

  // adjust start pointer
  while (!journalq.empty() && journalq.front().first <= seq) {
    journalq.pop_front();
  }
  if (!journalq.empty()) {
    header.start = journalq.front().second;
  } else {
    header.start = write_pos;
  }
  must_write_header = true;
  print_header();
  
  // committed but writing
  while (!writing_seq.empty() && writing_seq.front() <= seq) {
    dout(15) << " finishing committed but writing|waiting seq " << writing_seq.front() << dendl;
    finisher->queue(writing_fin.front());
    writing_seq.pop_front();
    writing_fin.pop_front();
  }
  
  // committed but unjournaled items
  while (!writeq.empty() && writeq.front().seq <= seq) {
    dout(15) << " dropping committed but unwritten seq " << writeq.front().seq 
	     << " len " << writeq.front().bl.length()
	     << " (" << writeq.front().fin << ")"
	     << dendl;
    if (writeq.front().fin)
      finisher->queue(writeq.front().fin);
    throttle_ops.put(1);
    throttle_bytes.put(writeq.front().bl.length());
    writeq.pop_front();  
  }
  
  commit_cond.Signal();

  dout(10) << "committed_thru done" << dendl;
}


void FileJournal::make_writeable()
{
  _open(true);

  if (read_pos > 0)
    write_pos = read_pos;
  else
    write_pos = get_top();
  read_pos = 0;

  must_write_header = true;
  start_writer();
}

void FileJournal::wrap_read_bl(off64_t& pos, int64_t olen, bufferlist& bl)
{
  while (olen > 0) {
    while (pos >= header.max_size)
      pos = pos + get_top() - header.max_size;

    int64_t len;
    if (pos + olen > header.max_size)
      len = header.max_size - pos;        // partial
    else
      len = olen;                         // rest
    
#ifdef DARWIN
    ::lseek(fd, pos, SEEK_SET);
#else
    ::lseek64(fd, pos, SEEK_SET);
#endif
    
    bufferptr bp = buffer::create(len);
    int r = ::read(fd, bp.c_str(), len);
    assert(r == len);
    bl.push_back(bp);
    pos += len;
    olen -= len;
  }
}

bool FileJournal::read_entry(bufferlist& bl, uint64_t& seq)
{
  if (!read_pos) {
    dout(2) << "read_entry -- not readable" << dendl;
    return false;
  }

  off64_t pos = read_pos;
  bl.clear();

  // header
  entry_header_t *h;
  bufferlist hbl;
  wrap_read_bl(pos, sizeof(*h), hbl);
  h = (entry_header_t *)hbl.c_str();

  if (!h->check_magic(read_pos, header.fsid)) {
    dout(2) << "read_entry " << read_pos << " : bad header magic, end of journal" << dendl;
    return false;
  }

  // pad + body + pad
  if (h->pre_pad)
    pos += h->pre_pad;
  wrap_read_bl(pos, h->len, bl);
  if (h->post_pad)
    pos += h->post_pad;

  // footer
  entry_header_t *f;
  bufferlist fbl;
  wrap_read_bl(pos, sizeof(*f), fbl);
  f = (entry_header_t *)fbl.c_str();
  if (memcmp(f, h, sizeof(*f))) {
    dout(2) << "read_entry " << read_pos << " : bad footer magic, partial entry, end of journal" << dendl;
    return false;
  }

  // yay!
  dout(1) << "read_entry " << read_pos << " : seq " << h->seq
	  << " " << h->len << " bytes"
	  << dendl;

  if (seq && h->seq < seq) {
    dout(2) << "read_entry " << read_pos << " : got seq " << h->seq << ", expected " << seq << ", stopping" << dendl;
    return false;
  }

  if (h->seq < last_committed_seq) {
    dout(0) << "read_entry seq " << seq << " < last_committed_seq " << last_committed_seq << dendl;
    assert(h->seq >= last_committed_seq);
    return false;
  }
  last_committed_seq = h->seq;

  // ok!
  seq = h->seq;
  journalq.push_back(pair<uint64_t,off64_t>(h->seq, read_pos));

  read_pos = pos;
  assert(read_pos % header.alignment == 0);
 
  return true;
}

void FileJournal::throttle()
{
  if (throttle_ops.wait(g_conf.journal_queue_max_ops))
    dout(1) << "throttle: waited for ops" << dendl;
  if (throttle_bytes.wait(g_conf.journal_queue_max_bytes))
    dout(1) << "throttle: waited for bytes" << dendl;
}
