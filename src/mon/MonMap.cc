
#include "MonMap.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


// read from/write to a file
int MonMap::write(const char *fn) 
{
  // encode
  bufferlist bl;
  encode(bl);
  
  return bl.write_file(fn);
}

int MonMap::read(const char *fn) 
{
  // read
  bufferlist bl;
  int r = bl.read_file(fn);
  if (r < 0)
    return r;
  decode(bl);
  return 0;
}

void MonMap::print_summary(ostream& out)
{
  out << "e" << epoch << ": "
      << mon_inst.size() << " mons at";
  for (unsigned i = 0; i<mon_inst.size(); i++)
    out << " " << mon_inst[i].addr;
}
 
void MonMap::print(ostream& out)
{
  out << "epoch " << epoch << "\n";
  out << "fsid " << fsid << "\n";
  out << "last_changed " << last_changed << "\n";
  out << "created " << created << "\n";
  for (unsigned i=0; i<mon_inst.size(); i++) {
    out << "\t" << mon_inst[i] << "\n";
  }
}
