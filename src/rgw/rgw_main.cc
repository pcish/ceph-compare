#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/md5.h>

#include "fcgiapp.h"

#include "rgw_access.h"
#include "rgw_acl.h"
#include "rgw_user.h"
#include "rgw_op.h"
#include "rgw_rest.h"

#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>

#include "include/types.h"
#include "common/base64.h"
#include "common/BackTrace.h"

using namespace std;


#define CGI_PRINTF(stream, format, ...) do { \
   fprintf(stderr, format, __VA_ARGS__); \
   FCGX_FPrintF(stream, format, __VA_ARGS__); \
} while (0)

/*
 * ?get the canonical amazon-style header for something?
 */
static void get_canon_amz_hdr(struct req_state *s, string& dest)
{
  dest = "";
  map<string, string>::iterator iter;
  for (iter = s->x_amz_map.begin(); iter != s->x_amz_map.end(); ++iter) {
    dest.append(iter->first);
    dest.append(":");
    dest.append(iter->second);
    dest.append("\n");
  }
}

/*
 * ?get the canonical representation of the object's location
 */
static void get_canon_resource(struct req_state *s, string& dest)
{
  if (s->host_bucket) {
    dest = "/";
    dest.append(s->host_bucket);
  }

  dest.append(s->path_name_url.c_str());

  string& sub = s->args.get_sub_resource();
  if (sub.size() > 0) {
    dest.append("?");
    dest.append(sub);
  }
}

/*
 * get the header authentication  information required to
 * compute a request's signature
 */
static void get_auth_header(struct req_state *s, string& dest, bool qsr)
{
  dest = "";
  if (s->method)
    dest = s->method;
  dest.append("\n");
  
  const char *md5 = FCGX_GetParam("HTTP_CONTENT_MD5", s->fcgx->envp);
  if (md5)
    dest.append(md5);
  dest.append("\n");

  const char *type = FCGX_GetParam("CONTENT_TYPE", s->fcgx->envp);
  if (type)
    dest.append(type);
  dest.append("\n");

  string date;
  if (qsr) {
    date = s->args.get("Expires");
  } else {
    const char *str = FCGX_GetParam("HTTP_DATE", s->fcgx->envp);
    if (str)
      date = str;
  }

  if (date.size())
      dest.append(date);
  dest.append("\n");

  string canon_amz_hdr;
  get_canon_amz_hdr(s, canon_amz_hdr);
  dest.append(canon_amz_hdr);

  string canon_resource;
  get_canon_resource(s, canon_resource);
  dest.append(canon_resource);
}

/*
 * calculate the sha1 value of a given msg and key
 */
static void calc_hmac_sha1(const char *key, int key_len,
                           const char *msg, int msg_len,
                           char *dest, int *len) /* dest should be large enough to hold result */
{
  char hex_str[128];
  unsigned char *result = HMAC(EVP_sha1(), key, key_len, (const unsigned char *)msg,
                               msg_len, (unsigned char *)dest, (unsigned int *)len);

  buf_to_hex(result, *len, hex_str);

  cerr << "hmac=" << hex_str << std::endl;
}

/*
 * verify that a signed request comes from the keyholder
 * by checking the signature against our locally-computed version
 */
static bool verify_signature(struct req_state *s)
{
  bool qsr = false;
  string auth_id;
  string auth_sign;

  if (!s->http_auth || !(*s->http_auth)) {
    auth_id = s->args.get("AWSAccessKeyId");
    if (auth_id.size()) {
      url_decode(s->args.get("Signature"), auth_sign);

      string date = s->args.get("Expires");
      time_t exp = atoll(date.c_str());
      time_t now;
      time(&now);
      if (now >= exp)
        return false;

      qsr = true;
    } else {
      /* anonymous access */
      rgw_get_anon_user(s->user);
      return true;
    }
  } else {
    if (strncmp(s->http_auth, "AWS ", 4))
      return false;
    string auth_str(s->http_auth + 4);
    int pos = auth_str.find(':');
    if (pos < 0)
      return false;

    auth_id = auth_str.substr(0, pos);
    auth_sign = auth_str.substr(pos + 1);
  }

  /* first get the user info */
  if (rgw_get_user_info(auth_id, s->user) < 0) {
    cerr << "error reading user info, uid=" << auth_id << " can't authenticate" << std::endl;
    return false;
  }

  /* now verify signature */
   
  string auth_hdr;
  get_auth_header(s, auth_hdr, qsr);
  cerr << "auth_hdr:" << std::endl << auth_hdr << std::endl;

  const char *key = s->user.secret_key.c_str();
  int key_len = strlen(key);

  char hmac_sha1[EVP_MAX_MD_SIZE];
  int len;
  calc_hmac_sha1(key, key_len, auth_hdr.c_str(), auth_hdr.size(), hmac_sha1, &len);

  char b64[64]; /* 64 is really enough */
  int ret = encode_base64(hmac_sha1, len, b64, sizeof(b64));
  if (ret < 0) {
    cerr << "encode_base64 failed" << std::endl;
    return false;
  }

  cerr << "b64=" << b64 << std::endl;
  cerr << "auth_sign=" << auth_sign << std::endl;
  cerr << "compare=" << auth_sign.compare(b64) << std::endl;
  return (auth_sign.compare(b64) == 0);
}

static sighandler_t sighandler;

/*
 * ?print out the C++ errors to log in case it fails
 */
static void godown(int signum)
{
  BackTrace bt(0);
  bt.print(cerr);

  signal(SIGSEGV, sighandler);
}

/*
 * start up the RADOS connection and then handle HTTP messages as they come in
 */
int main(int argc, char *argv[])
{
  struct req_state s;
  struct fcgx_state fcgx;
  RGWHandler_REST rgwhandler;

  if (!RGWAccess::init_storage_provider("rados", argc, argv)) {
    cerr << "couldn't init storage provider" << std::endl;
    return 5; //EIO
  }

  sighandler = signal(SIGSEGV, godown);

  while (FCGX_Accept(&fcgx.in, &fcgx.out, &fcgx.err, &fcgx.envp) >= 0) 
  {
    rgwhandler.init_state(&s, &fcgx);

    int ret = read_acls(&s);
    if (ret < 0) {
      switch (ret) {
      case -ENOENT:
        break;
      default:
        cerr << "could not read acls" << " ret=" << ret << std::endl;
        abort_early(&s, -EPERM);
        continue;
      }
    }
    ret = verify_signature(&s);
    if (!ret) {
      cerr << "signature DOESN'T match" << std::endl;
      abort_early(&s, -EPERM);
      continue;
    }

    ret = rgwhandler.read_permissions();
    if (ret < 0) {
      abort_early(&s, ret);
      continue;
    }

    RGWOp *op = rgwhandler.get_op();
    if (op) {
      op->execute();
    }
  }
  return 0;
}

