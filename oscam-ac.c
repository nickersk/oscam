//FIXME Not checked on threadsafety yet; after checking please remove this line
#include "globals.h"

#ifdef CS_ANTICASC

//static time_t ac_last_chk;
static uchar  ac_ecmd5[CS_ECMSTORESIZE];

LLIST *ac_stat_list; //struct s_acasc
LLIST *acasc_list;   //struct  s_acasc_shm

int ac_init_log(void)
{
  if( (!fpa)  && (cfg->ac_logfile[0]))
  {
    if( (fpa=fopen(cfg->ac_logfile, "a+"))<=(FILE *)0 )
    {
      fpa=(FILE *)0;
      fprintf(stderr, "can't open anti-cascading logfile: %s\n", cfg->ac_logfile);
    }
    else
      cs_log("anti-cascading log initialized");
  }

  return(fpa<=(FILE *)0);
}

void ac_init_stat()
{
  ac_stat_list = llist_create();
  acasc_list = llist_create();

  if( fpa )
    fclose(fpa);
  fpa=(FILE *)0;
  if( ac_init_log() )
    cs_exit(0);
}

void ac_clear()
{
	llist_clear(acasc_list);
	llist_clear(ac_stat_list);
}

void ac_done_stat()
{
	ac_clear();
	llist_destroy(acasc_list);
	llist_destroy(ac_stat_list);
}

static struct s_client *idx_from_ac_idx(int ac_idx)
{
	struct s_client *cl;
	for (cl=first_client; cl ; cl=cl->next)
    if( cl->ac_idx==ac_idx )
      return cl;
  return NULL;
}

void ac_do_stat()
{
  int i, j, idx, exceeds, maxval, prev_deny=0;
  struct s_client *cl_idx;

  LLIST_ITR itr1, itr2;
  i = 1;
  struct s_acasc *ac_stat = llist_itr_init(ac_stat_list, &itr1);
  struct s_acasc_shm *acasc = llist_itr_init(acasc_list, &itr2);
  while (acasc)
  {
	int ac_stat_next = 1;
	if (!ac_stat) {
		ac_stat = malloc(sizeof(struct s_acasc));
		memset(ac_stat, 0, sizeof(struct s_acasc));
		llist_append(ac_stat_list, ac_stat);
		ac_stat_next = 0;
	}

    idx = ac_stat->idx;
    ac_stat->stat[idx] = acasc->ac_count;
    acasc->ac_count=0;
    cl_idx = idx_from_ac_idx(i);

    if( ac_stat->stat[idx] )
    {
      if( cl_idx == NULL ) {
        cs_log("ERROR: can't find client with ac_idx=%d", i);
        continue;
      }

      if( cl_idx->ac_penalty==2 ) {// banned
        cs_debug("user '%s' banned", cl_idx->usr);
        acasc->ac_deny=1;
      }
      else
      {
        for( j=exceeds=maxval=0; j<cfg->ac_samples; j++ ) 
        {
          if( ac_stat->stat[j] > maxval )
            maxval=ac_stat->stat[j];
          exceeds+=(ac_stat->stat[j]>cl_idx->ac_limit);
        }
        prev_deny=acasc->ac_deny;
        acasc->ac_deny = (exceeds >= cfg->ac_denysamples);
        
        cs_debug("%s limit=%d, max=%d, samples=%d, dsamples=%d, ac[ci=%d][si=%d]:",
          cl_idx->usr, cl_idx->ac_limit, maxval, 
          cfg->ac_samples, cfg->ac_denysamples, i, idx);
        cs_debug("%d %d %d %d %d %d %d %d %d %d ", ac_stat->stat[0],
          ac_stat->stat[1], ac_stat->stat[2], ac_stat->stat[3],
          ac_stat->stat[4], ac_stat->stat[5], ac_stat->stat[6],
          ac_stat->stat[7], ac_stat->stat[8], ac_stat->stat[9]);
        if( acasc->ac_deny ) {
          cs_log("user '%s' exceeds limit", cl_idx->usr);
          ac_stat->stat[idx] = 0;
        } else if( prev_deny )
          cs_log("user '%s' restored access", cl_idx->usr);
      }
    }
    else if( acasc->ac_deny )
    {
      prev_deny=1;
      acasc->ac_deny=0;
      if( cl_idx != NULL )
        cs_log("restored access for inactive user '%s'", cl_idx->usr);
      else
        cs_log("restored access for unknown user (ac_idx=%d)", i);
    }

    if( !acasc->ac_deny && !prev_deny )
      ac_stat->idx = (ac_stat->idx + 1) % cfg->ac_samples;

    if (ac_stat_next)
    	ac_stat = llist_itr_next(&itr1);
    else
    	ac_stat = 0;
    acasc = llist_itr_next(&itr2);
    i++;
  }
}

void ac_init_client(struct s_auth *account)
{
  struct s_client *cl = cur_client();
  cl->ac_idx = account->ac_idx;
  cl->ac_limit = 0;
  if( cfg->ac_enabled )
  {
    if( account->ac_users )
    {
      cl->ac_limit = (account->ac_users*100+80)*cfg->ac_stime;
      cl->ac_penalty = account->ac_penalty;
      cs_debug("login '%s', ac_idx=%d, users=%d, stime=%d min, dwlimit=%d per min, penalty=%d", 
              account->usr, account->ac_idx, account->ac_users, cfg->ac_stime, 
              account->ac_users*100+80, account->ac_penalty);
    }
    else
    {
      cs_debug("anti-cascading not used for login '%s'", account->usr);
    }
  }
}

static int ac_dw_weight(ECM_REQUEST *er)
{
  struct s_cpmap *cpmap;

  for( cpmap=cfg->cpmap; (cpmap) ; cpmap=cpmap->next )
    if( (cpmap->caid  ==0 || cpmap->caid  ==er->caid)  &&
        (cpmap->provid==0 || cpmap->provid==er->prid)  &&
        (cpmap->sid   ==0 || cpmap->sid   ==er->srvid) &&
        (cpmap->chid  ==0 || cpmap->chid  ==er->chid) )
      return (cpmap->dwtime*100/60);

  cs_debug("WARNING: CAID %04X, PROVID %06X, SID %04X, CHID %04X not found in oscam.ac", 
           er->caid, er->prid, er->srvid, er->chid);
  cs_debug("set DW lifetime 10 sec");
  return 16; // 10*100/60
}

struct s_acasc_shm *get_acasc(ushort ac_idx) {
	int i=1;
	LLIST_ITR itr;

	struct s_acasc_shm *acasc = llist_itr_init(acasc_list, &itr);
	while (acasc) {
		if (i == ac_idx)
			return acasc;
		acasc = llist_itr_next(&itr);
		i++;
	}
	acasc = malloc(sizeof(struct s_acasc_shm));
	memset(acasc, 0, sizeof(struct s_acasc_shm));
	llist_append(acasc_list, acasc);
	return acasc;
}

void ac_chk(ECM_REQUEST *er, int level)
{
  struct s_client *cl = cur_client();
  if (!cl->ac_limit || !cfg->ac_enabled ) return;

  struct s_acasc_shm *acasc = get_acasc(cl->ac_idx);

  if( level==1 ) 
  {
    if( er->rc==7 ) acasc->ac_count++;
    if( er->rc>3 ) return; // not found
    if( memcmp(ac_ecmd5, er->ecmd5, CS_ECMSTORESIZE) != 0 )
    {
      acasc->ac_count+=ac_dw_weight(er);
      memcpy(ac_ecmd5, er->ecmd5, CS_ECMSTORESIZE);
    }
    return;
  }

  if( acasc->ac_deny )
    if( cl->ac_penalty )
    {
      cs_debug("send fake dw");
      er->rc=7; // fake
      er->rcEx=0;
      cs_sleepms(cfg->ac_fakedelay);
    }
}
#endif
