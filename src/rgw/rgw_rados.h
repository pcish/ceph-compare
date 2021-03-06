#ifndef __RGWRADOS_H
#define __RGWRADOS_H

#include "include/librados.h"
#include "rgw_access.h"


class RGWRados  : public RGWAccess
{
  /** Open the pool used as root for this gateway */
  int open_root_pool(rados_pool_t *pool);
public:
  /** Initialize the RADOS instance and prepare to do other ops */
  int initialize(int argc, char *argv[]);
  /** set up a bucket listing. id is ignored, handle is filled in. */
  int list_buckets_init(std::string& id, RGWAccessHandle *handle);
  /** 
   * get the next bucket in the listing. id is ignored, obj is filled in,
   * handle is updated.
   */
  int list_buckets_next(std::string& id, RGWObjEnt& obj, RGWAccessHandle *handle);

  /** get listing of the objects in a bucket */
  int list_objects(std::string& id, std::string& bucket, int max, std::string& prefix, std::string& delim,
                   std::string& marker, std::vector<RGWObjEnt>& result, map<string, bool>& common_prefixes);

  /**
   * create a bucket with name bucket and the given list of attrs
   * returns 0 on success, -ERR# otherwise.
   */
  int create_bucket(std::string& id, std::string& bucket, map<std::string,bufferlist>& attrs, uint64_t auid=0);

  /** Write/overwrite an object to the bucket storage. */
  int put_obj(std::string& id, std::string& bucket, std::string& obj, const char *data, size_t size,
              time_t *mtime,
              map<std::string, bufferlist>& attrs);
  /** Copy an object, with many extra options */
  int copy_obj(std::string& id, std::string& dest_bucket, std::string& dest_obj,
               std::string& src_bucket, std::string& src_obj,
               time_t *mtime,
               const time_t *mod_ptr,
               const time_t *unmod_ptr,
               const char *if_match,
               const char *if_nomatch,
               map<std::string, bufferlist>& attrs,
               struct rgw_err *err);
  /** delete a bucket*/
  int delete_bucket(std::string& id, std::string& bucket);

  /** Delete an object.*/
  int delete_obj(std::string& id, std::string& bucket, std::string& obj);

  /** Get the attributes for an object.*/
  int get_attr(std::string& bucket, std::string& obj,
               const char *name, bufferlist& dest);

  /** Set an attr on an object. */
  int set_attr(std::string& bucket, std::string& obj,
                       const char *name, bufferlist& bl);

  /** Get data about an object out of RADOS and into memory. */
  int get_obj(std::string& bucket, std::string& obj, 
            char **data, off_t ofs, off_t end,
            map<string, bufferlist> *attrs,
            const time_t *mod_ptr,
            const time_t *unmod_ptr,
            const char *if_match,
            const char *if_nomatch,
            bool get_data,
            struct rgw_err *err);
};

#endif
