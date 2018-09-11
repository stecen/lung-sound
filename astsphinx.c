#include "prefork.h"
#include "astsphinx.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <cmd_ln.h>
#include <pocketsphinx.h>
#include <ngram_model.h>


int ARGC;
char **ARGV;

char *reqtype_to_string(enum e_reqtype e)
{
  switch(e)
  {
    case REQTYPE_GRAMMAR:
      return "GRAMMAR";
    case REQTYPE_START:
      return "START";
    case REQTYPE_DATA:
      return "DATA";
    case REQTYPE_FINISH:
      return "FINISH";
    default:
      return "UNKNOWN";
  }
}



void testserver(int sock)
{
  cmd_ln_t *cmdln = cmd_ln_parse_file_r(NULL, ps_args(), ARGV[2], FALSE);
  if(cmdln == NULL)
  {
    printf("Unable to parse config file %s\n", ARGV[2]);
    return;
  }

  ps_decoder_t *decoder = ps_init(cmdln);
  if(decoder == NULL)
  {
    printf("Unable to allocate decoder.\n");
    return;
  }
  
  char **fn = &ARGV[3];
  ngram_model_t *ngram = NULL;
  ngram_model_t *lmset = ps_get_lmset(decoder);
  printf("Initializing grammars.\n");

  int x;
  for(x=3; x<ARGC; x++,fn++)
  {
    printf("Loading grammar '%s'\n", *fn);

    ngram = ngram_model_read(cmdln,
                             *fn,
                             NGRAM_AUTO,
                             ps_get_logmath(decoder));
    if(ngram == NULL)
    {
      printf("Unable to set grammar.\n");
    }
    else
    {
      printf("Adding %s to lmset.\n", *fn);
      if(lmset == NULL) 
      {
        printf ("Failed to fetch lmset.\n");
      }
      else
      {
        ngram_model_set_add(lmset, ngram, *fn, 1.0, TRUE);
      }
    }
  }
      
  ngram_model_set_select(lmset, ARGV[3]);
  if(ps_update_lmset(decoder, lmset) == NULL)
  {
    printf("Unable to update lmset.\n");
    return;
  }

  // SOCKET ACCEPT HERE
  printf("Waiting on connection.\n");
  int client = accept(sock, NULL, NULL);
  printf("Accepted connection.\n");

  if(ps_start_utt(decoder, NULL) != 0)
  {
    printf("Error starting decoding\n");
    ps_free(decoder);
    return;
  }

  int bcount, dlen;
  char buf[ASTSPHINX_BUFSIZE];
  enum e_reqtype rtype;
  const char *uttid = NULL;
  
  while(1)
  {
    bcount = read(client, &dlen, sizeof(dlen));
    if(bcount != sizeof(dlen))
    {
      printf("Error, read %d bytes, expecting %ld (%s)\n", bcount, sizeof(dlen), strerror(errno));
      break;
    }

    bcount = read(client, &rtype, sizeof(rtype));
    if(bcount != sizeof(rtype))
    {
      printf("Error, read %d bytes, expecting %ld (%s)\n", bcount, sizeof(rtype), strerror(errno));
      break;
    }
 
    // printf("Got dlen: %d for request of type %s\n", dlen, reqtype_to_string(rtype));

    if(dlen > ASTSPHINX_BUFSIZE)
    {
      printf("Error, got dlen of %d but buffer is only %d\n", dlen, ASTSPHINX_BUFSIZE);
      break;
    }

    if(dlen)
    {
      bcount = read(client, buf, dlen);
      if(bcount != dlen)
      {
        printf("Error, read %d bytes, expecting %d (%s)\n", bcount, dlen, strerror(errno));
        break;
      }
    }

    const char * hyp = NULL;
    int32 score = 0;
    int buflen = 0;
    switch(rtype)
    {
      case REQTYPE_START:
        break;
      case REQTYPE_GRAMMAR:
        if(uttid != NULL) // have utterance so stop it first.
        {
          printf("Stopping running decode to switch grammars.\n");
          ps_end_utt(decoder);
          uttid = NULL;
        }
        printf("Request to switch grammar to '%s'\n", buf);
        lmset = ps_get_lmset(decoder);
        ngram_model_set_select(lmset, buf);
        if(ps_update_lmset(decoder, lmset) == NULL)
        {
          printf("Unable to update lmset.\n");
        }
        break;
      case REQTYPE_DATA:
        if(dlen)
        {
          if(uttid == NULL)  // No utt, so start one first.
          {
            if(ps_start_utt(decoder, NULL) != 0)
            {
              printf("Error starting decoding.\n");
            }
          }

          if(ps_process_raw(decoder, (int16 *)buf, dlen / 2, 0, 0) < 0)
          {
            printf("Error decoding raw data\n");
          }
          hyp = ps_get_hyp(decoder, &score, &uttid);
          int32 modscore = abs(score) / 30000;
          if(modscore > 1000)
            modscore = 1000;
          if(hyp != NULL && strlen(hyp) != 0)
          {
            printf("Got hyp: %05d %010d '%s'\n", modscore, score, hyp);
            *(int32 *)buf=modscore;
            strncpy(buf+sizeof(int32), hyp, ASTSPHINX_BUFSIZE-sizeof(int32));
            //int32_t conf = ps_get_prob(decoder, NULL);
            //printf("Got confidence score of: %d\n", conf);
            //*(int32 *)buf=conf;
            buflen = sizeof(int32) + strlen(hyp);
          }
          break;
        }
      case REQTYPE_FINISH:
        printf("Finalizing and getting end hypothesis.\n");
        if(uttid == NULL)
        {
          printf("Error - cannot finalize when no processing occuring.\n");
        }

        if(ps_process_raw(decoder, NULL, 0, 0, 0) < 0)
        {
          printf("Error decoding raw data\n");
        }

        if(ps_end_utt(decoder) < 0)
        {
          printf("Error ending processing.\n");
        }
        hyp = ps_get_hyp(decoder, &score, &uttid);
        int32 modscore = abs(score) / 30000;
        if(modscore > 1000)
          modscore = 1000;
        if(hyp != NULL && strlen(hyp) != 0)
        {
          printf("Final hyp: %05d %010d '%s'\n", modscore, score, hyp);
          *(int32 *)buf=modscore;
          strncpy(buf+sizeof(int32), hyp, ASTSPHINX_BUFSIZE-sizeof(int32));
          //int32 conf = ps_get_prob(decoder, &uttid);
          //printf("Got confidence score of: %d\n", conf);
          //*(int32 *)buf=conf;
          buflen = sizeof(int32) + strlen(hyp);
        }
        else
        {
          printf("No hypothesis made.\n");
          buf[0] = '\0';
        }
        uttid = NULL;
        break;
    }

    bcount = write(client, &buflen, sizeof(int));
    if(bcount != sizeof(int))
    { 
      printf("Error, wrote %d bytes, expecting %ld (%s)\n", bcount, sizeof(int), strerror(errno));
      break;
    }

    bcount = write(client, buf, buflen);
    if(bcount != buflen)
    { 
      printf("Error, wrote %d bytes, expecting %d (%s)\n", bcount, buflen, strerror(errno));
      break;
    }

  }

  //close(logf);

  if(decoder != NULL)
  {
    printf("Freeing decoder, new ref count is %d\n", ps_free(decoder));
  }
  close(client);
  return;
}

int main(int argc, char *argv[])
{
  ARGC = argc;
  ARGV = argv;
  if(ARGC < 3)
  {
    printf("Usage: %s LISTENPORT SPHINXCONFIG GRAMMARFILE GRAMMARFILE ...\n", argv[0]);
    return 0;
  }

  int bindport=0;
  sscanf(argv[1], "%d", &bindport);
  printf("Listening on port: %d\n", bindport);

  prefork_listen(bindport, &testserver);
  return 0;
}
