


#include <iostream>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <openssl/md5.h>
#include <openssl/sha.h>

#include "objclass/objclass.h"


CLS_VER(1,0)
CLS_NAME(crypto)

cls_handle_t h_class;

cls_method_handle_t h_md5;
cls_method_handle_t h_sha1;

int md5_method(cls_method_context_t ctx, char *indata, int datalen,
				 char **outdata, int *outdatalen)
{
   int i;
   MD5_CTX c;
   unsigned char *md;

   cls_log("md5 method");
   cls_log("indata=%.*s data_len=%d", datalen, indata, datalen);

   md = (unsigned char *)cls_alloc(MD5_DIGEST_LENGTH);
   if (!md)
     return -ENOMEM;

   MD5_Init(&c);
   MD5_Update(&c, indata, (unsigned long)datalen);
   MD5_Final(md,&c);

   *outdata = (char *)md;
   *outdatalen = MD5_DIGEST_LENGTH;

   return 0;
}

int sha1_method(cls_method_context_t ctx, char *indata, int datalen,
				 char **outdata, int *outdatalen)
{
   int i;
   SHA_CTX c;
   unsigned char *md;

   cls_log("sha1 method");
   cls_log("indata=%.*s data_len=%d", datalen, indata, datalen);

   md = (unsigned char *)cls_alloc(SHA_DIGEST_LENGTH);
   if (!md)
     return -ENOMEM;

   SHA1_Init(&c);
   SHA1_Update(&c, indata, (unsigned long)datalen);
   SHA1_Final(md,&c);

   *outdata = (char *)md;
   *outdatalen = SHA_DIGEST_LENGTH;

   return 0;
}

void class_init()
{
   cls_log("Loaded crypto class!");

   cls_register("crypto", &h_class);
   cls_register_method(h_class, "md5", md5_method, &h_md5);
   cls_register_method(h_class, "sha1", sha1_method, &h_sha1);

   return;
}
