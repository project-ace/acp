#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include "acpbl.h"

size_t iacp_starter_memory_size_cl = 1024;

/* default parameters */
#define ACPCI_CRBENTRYNUM 8     // Number of entries of crb
#define ACPCI_CHSBUFENTRYNUM 2  // Number of entries of send buf of channel
#define ACPCI_CHSBUFMASK 0x1LL  // Mask for switching sbuf entry
#define ACPCI_CHRBUFENTRYNUM 16 // Number of entries of recv buf of channel
#define ACPCI_CHENTRYSZ 2048    // Size of the message of channel
#define ACPCI_CHREQNUM 32       // NUmber of entries of request table of channel
#define ACPCI_EAGER_LIMIT 1024  // Threshold of using eager protocol

/* types and statuses of channels */
#define CHTYSEND 0
#define CHTYRECV 1
#define CHSTINIT   0 // initial
#define CHSTWTHD   1 // sender channel: waiting head of receiver 
#define CHSTWTAK   2 // sender channel: waiting ack from receiver 
#define CHSTCONN   3 // connected 
#define CHSTATMASK 7 // mask to check the status 
#define CHSTDISCONFLAG 8 // flag for disconnecting channel 
#define CHCHECKSTAT(ch) (ch->state & CHSTATMASK)       // check the status of the channel 
#define CHCHECKDISCON(ch) (ch->state & CHSTDISCONFLAG) // check the disconnection flag 
#define CHSETSTAT(ch,st) (CHCHECKDISCON(ch) | st)      // set the status of the channel 
#define CHSETDISCON(ch) (ch->state | CHSTDISCONFLAG)   // set the status of the channel 

/* signal on the connection information */
#define CONNSTVALID 0
#define CONNSTINV 1

/* types and statuses of requests  */
#define CHREQTYSNDEGR 0 // Eager send
#define CHREQTYSNDRND 1 // Rendezvous send 
#define CHREQTYRCV 2    // Receive (protocol is unknown) 
#define CHREQTYRCVRND 3 // Rendezvous receive 
#define CHREQTYDISCON 4 // Disconnection 

/* types of messages */
#define CHMSGTYEGR 0    // Eager message 
#define CHMSGTYRND 1    // Rendezvous message 
#define CHMSGTYDISCON 2 // Disconnection message 

/* statuses of crb messages */
#define CRBSTINUSE 0  
#define CRBSTFREE 1

/* structures of lists */
typedef struct listitem{
  struct listitem *prev;
  struct listitem *next;
} listitem_t;

typedef struct listobj{
  struct listitem *head;
  struct listitem *tail;
} listobj_t;

/* lists of channels */
static listobj_t conch_list; // connecting channel list
static listobj_t idlech_list; // idle channel list
static listobj_t reqch_list; // requesting channel list

/* structure of channel endpoint */
typedef struct chitem{
  struct chitem *prev;
  struct chitem *next;
  uint16_t type;
  uint16_t state;
  int peer;
  char *chbody;
  uint64_t msgidx;
  acp_ga_t localga;
  acp_atkey_t localatkey;
  acp_ga_t remotega;
  int buf_entrysz;
  int rbuf_entrynum;
  int sbuf_entrynum;
  acp_handle_t *shdl;
  struct chreqitem *reqtable;
  struct chreqitem *freereqs;    
  struct chreqitem *initreqs;    
  struct listobj reqs;
  int lock;
} chitem_t;

static int acpch_eager_limit;
static int acpch_chreqnum;

/* macros for head, tail and idx of receive queue */
#define CHLOCALHEAD(ch) ((uint64_t *)((ch)->chbody))
#define CHLOCALTAIL(ch) ((uint64_t *)((ch)->chbody) + 1)
#define CHLOCALIDX(ch)  ((uint64_t *)((ch)->chbody) + 2)
#define CHREMOTEHEADGA(ch) ((ch)->remotega)
#define CHLOCALHEADGA(ch)  ((ch)->localga)
#define CHREMOTETAILGA(ch) ((ch)->remotega +   sizeof(uint64_t))
#define CHLOCALTAILGA(ch)  ((ch)->localga  +   sizeof(uint64_t))
#define CHREMOTEIDXGA(ch)  ((ch)->remotega + 2*sizeof(uint64_t))
#define CHLOCALIDXGA(ch)   ((ch)->localga  + 2*sizeof(uint64_t))

/* macros for message slots */
#define CHSBSLOTADDR(addr,index,slotsz) ((char *)(addr) + 3*sizeof(uint64_t) + (index)*(slotsz))
#define CHRBSLOTADDR(addr,index,slotsz) ((char *)(addr) + 3*sizeof(uint64_t) + (index)*(slotsz))
#define CHSBSLOTGA(ga,index,slotsz)     (ga + 3*sizeof(uint64_t) + (index)*(slotsz))
#define CHRBSLOTGA(ga,index,slotsz)     (ga + 3*sizeof(uint64_t) + (index)*(slotsz))

/* macros for messages */
#define CHMSGTYPE(addr) ((int *)addr)
#define CHMSGSIZE(addr) ((size_t *)((char *)addr + sizeof(int)))
#define CHMSGBODY(addr) ((size_t *)((char *)addr + sizeof(int) + sizeof(size_t)))
#define CHMSGHDRSZ (sizeof(int) + sizeof(size_t))
#define CHMSGRNDSZ (sizeof(int) + sizeof(size_t) + sizeof(acp_ga_t))

/* structure of request item in channel  */
typedef struct chreqitem{
  struct chreqitem *prev;
  struct chreqitem *next;
  acp_ch_t ch;
  int type;
  uint64_t msgidx;
  void *addr;
  size_t size;
  acp_atkey_t atkey;
  acp_handle_t hdl;
} chreqitem_t;

/* struct of connection request messages */
typedef struct crbmsg{
  int start; // flag for checking arrival of crbmsg 
  acp_ga_t ga;
  int rank;
  int state;
  int buf_entrysz;
  int rbuf_entrynum;
  int sbuf_entrynum;
  int end; // flag for checking arrival of crbmsg 
} crbmsg_t;

/* crb (connection request buffer) 
 *   - located in the head of the ML starter mem
 */
static uint64_t *crbhead;   // head of the receive queue of crb 
static uint64_t *crbtail;   // tail of the receive queue of crb 
static crbmsg_t *crbtbl;    // table used as the receive queue 
static acp_ga_t trashboxga; // used as a dummy place to store the result of remote atomic 
#define CRB_TAIL_OFFSET  (sizeof(uint64_t))  
#define CRB_TABLE_OFFSET (2*sizeof(uint64_t))
#define CRB_TRASHBOX_OFFSET (2*(sizeof(uint64_t)) + crbentrynum * sizeof(crbmsg_t))

static char *crbreqmsg;      // buffer used for sending connection request 
static acp_ga_t crbreqmsgga; // global address of crbreqmsg 
static int crbentrynum; // number of entries of crbtbl 

/* struct of connection information.
 * this will be located in the top of the chbody of the channel
 * until the completion of the connection. 
 */
typedef struct conninfo{
  uint64_t head;
  uint64_t tail;
  crbmsg_t crbmsg;
  acp_ga_t remotega;
  int state;
  acp_ga_t starterga;
  acp_handle_t hdl;
  chitem_t *nextch;
} conninfo_t;

#define CONNINFO_CRBMSG_OFFSET (sizeof(uint64_t)*2)
#define CONNINFO_REMGA_OFFSET (sizeof(uint64_t)*2+sizeof(crbmsg_t))
#define CONNINFO_STATE_OFFSET (sizeof(uint64_t)*2+sizeof(crbmsg_t)+sizeof(acp_ga_t))

/* functions for handling lists */
static void list_init(listobj_t *list)
{
  list->head = NULL;
  list->tail = NULL;
}

static void list_add(listobj_t *list, listitem_t *item)
{
  if (list->head == NULL){
    item->prev = NULL;
    item->next = NULL;
    list->head = item; 
    list->tail = item; 
  }else{
    item->prev = list->tail;
    item->next = NULL;
    list->tail->next = item;
    list->tail = item;
  }
}

static void list_remove(listobj_t *list, listitem_t *item)
{
  if ((item->next == NULL) && (item->prev==NULL)){
    list->head = NULL;
    list->tail = NULL;
  } else{
    if (item->next == NULL){
      list->tail = item->prev;
      item->prev->next = item->next;
    } else if (item->prev == NULL){
      list->head = item->next;
      item->next->prev = item->prev;
    } else{
      item->prev->next = item->next;
      item->next->prev = item->prev;
    }
  }
}

static int list_isempty(listobj_t *list)
{
  return list->head == NULL;
}

static void list_show(listobj_t *list)
{
  listitem_t *item;

  item = list->head;
  while (item != NULL){
    printf(" %p", item);
    item = item->next;
  }
  printf("\n");
}

/* initialize connection info
 *   - start getting head and atomic_fetching_and_adding tail of remote crb
 *   - change state of the channel to CHSTWTHD
 */
static void init_conninfo(acp_ch_t ch)
{
  conninfo_t *conninfo;
  int myrank;

  myrank = acp_rank();

  conninfo = (conninfo_t *)(ch->chbody);
  conninfo->state = CONNSTVALID;

  /* start getting head */
  acp_copy(ch->localga, conninfo->starterga, sizeof(uint64_t), ACP_HANDLE_NULL);
  /* start atomic_fetch_and_add tail. 
   * this handle waits for both head and tail. 
   * this tail will be used as an index of this crb message. */
  conninfo->hdl = acp_add8(ch->localga+sizeof(uint64_t), conninfo->starterga+sizeof(uint64_t), 
		     1LL, ACP_HANDLE_NULL);
  ch->state = CHCHECKDISCON(ch) | CHSTWTHD;

#ifdef DEBUG
    printf("%d: init_conninfo: ch %p get head dst %p src %p \n", 
	   myrank, ch, ch->localga, conninfo->starterga);
#endif

}

/* check connecting channels list
 * 
 *  connecting channels list:
 *   list of connecting items
 *    - from conch_list.head to conch_list.tail
 *  connecting items:
 *   keeps list of connecting channels with same type and peer
 *    - from con_item->chs
 *
 */
static void handl_conchlist(void)
{
  acp_ch_t ch, nextch, tmpch;
  conninfo_t *conninfo;
  int myrank, idx;
  uint64_t head, tail, crbidx;
  crbmsg_t *msg;

  myrank = acp_rank();

  /* check items in the connecting channels list */
  ch = (acp_ch_t)(conch_list.head);

  while (ch != NULL){
    nextch = ch->next;
    switch (ch->type){
    case CHTYSEND:
      conninfo = (conninfo_t *)(ch->chbody);
      switch (CHCHECKSTAT(ch)){
      case CHSTINIT:
	printf("handl_conchlist: rank %d : Error: head of the connecting channel list cannot be CHSTINIT. \n", 
		myrank);
	fflush(stdout);
	iacp_abort_cl();
	break;
      case CHSTWTHD:
	/* check if head (and tail) has arrived */
	if (acp_inquire(conninfo->hdl) == 0){
	  head = conninfo->head;
	  tail = conninfo->tail; 
	  /* check if there is a room in crb */
	  if ((tail - head) < crbentrynum){
	    /* send the connection request message to the tail of the crb */
	    acp_copy(conninfo->starterga + CRB_TABLE_OFFSET
		     + sizeof(crbmsg_t) * (tail % crbentrynum), 
		     ch->localga + CONNINFO_CRBMSG_OFFSET,
		     sizeof(crbmsg_t),
		     ACP_HANDLE_NULL);
#ifdef DEBUG
	    printf("%d: handl_conchlist: remote start %016llx put crb msg to %016llx (rank %d) from %p crbmsg %d %d %016llx %d %d %d %d %d\n", 
		   myrank, iacp_query_starter_ga_cl(ch->peer), 
		   conninfo->starterga + CRB_TABLE_OFFSET + sizeof(crbmsg_t) * (tail % crbentrynum),
		   acp_query_rank(conninfo->starterga + CRB_TABLE_OFFSET + sizeof(crbmsg_t) * (tail % crbentrynum)),
		   ch->localga + CONNINFO_CRBMSG_OFFSET,
		   (conninfo->crbmsg).start, (conninfo->crbmsg).rank, (conninfo->crbmsg).ga,
		   (conninfo->crbmsg).state, (conninfo->crbmsg).buf_entrysz, (conninfo->crbmsg).rbuf_entrynum,
		   (conninfo->crbmsg).sbuf_entrynum, (conninfo->crbmsg).end);
	    fflush(stdout);
#endif
	    /* change state to waiting connection ack */
	    ch->state = CHSETSTAT(ch, CHSTWTAK);
	  } else{ 
	    /* no room in crb. get the head of the crb again. */
	    conninfo->hdl = acp_copy(ch->localga, conninfo->starterga, sizeof(acp_ga_t), ACP_HANDLE_NULL);
	  }
	}

	break;
      case CHSTWTAK:
	if (conninfo->state != CONNSTVALID){
#ifdef DEBUG
	  printf("%d: handl_conchlist: conn req tail %llu has been invalidated\n", 
		 myrank, conninfo->tail);
	  fflush(stdout);
#endif
	  /* connection request is invalidated. re-initialize the request. */
	  init_conninfo(ch);
	} else if (conninfo->remotega != ACP_GA_NULL){ 
#ifdef DEBUG
	  printf("%d: handl_conchlist: send conn req tail %llu has been connected to ga %p (rank %d)\n", 
		 myrank, conninfo->tail, conninfo->remotega, acp_query_rank(conninfo->remotega));
	  fflush(stdout);
#endif
	  /* remotega has arrived. complete the connection */
	  ch->state = CHSETSTAT(ch, CHSTCONN);
	  ch->remotega = conninfo->remotega;

	  /* remove this channel from the connecting list */
	  list_remove(&conch_list, (listitem_t *)ch);

	  /* check if there are pending requests in this channel already */
	  if (list_isempty(&(ch->reqs)))
	    list_add(&idlech_list, (listitem_t *)ch); // add this channel to idle channels list 
	  else
	    list_add(&reqch_list, (listitem_t *)ch); // add this channel to requesting channels list 

	  tmpch = conninfo->nextch;

	  /* check if there is a connecting channel with the same type and peer */
	  if (tmpch != NULL){
	    list_add(&conch_list, (listitem_t *)tmpch); // add the next channel to the connecting channels list 
	    init_conninfo(tmpch); // start requesting connection of the next channel 
	  }

	  /* clear chbody */
	  memset (ch->chbody, 0, sizeof(uint64_t)*3 + ch->sbuf_entrynum * ch->buf_entrysz);
	}
	break;
      default:
	printf("handl_conchlist: rank %d : wrong channel status %d\n", 
		myrank, ch->state);
	fflush(stdout);
	iacp_abort_cl();
      }
      break;
    case CHTYRECV:
      crbidx = *crbhead;
      idx = crbidx % crbentrynum;
      while ((crbidx < *crbtail) && (crbtbl[idx].start!=0) && (crbtbl[idx].end!=0)
	     &&(crbtbl[idx].state != CRBSTFREE)){
	if (ch->peer == crbtbl[idx].rank){
	  /* check parameters of channel endpoints */
	  msg = &(crbtbl[idx]);

	  if ((msg->buf_entrysz != ch->buf_entrysz) 
	      || (msg->rbuf_entrynum != ch->rbuf_entrynum)
	      || (msg->sbuf_entrynum != ch->sbuf_entrynum)){
	    printf("handl_conchlist: rank %d : channel parameter does not match. peer %d sz %d/%d rnum %d/%d snum %d/%d\n", 
		    myrank, msg->buf_entrysz,ch->buf_entrysz, 
		    msg->rbuf_entrynum, ch->rbuf_entrynum, 
		    msg->sbuf_entrynum, ch->sbuf_entrynum);
	    fflush(stdout);
	    iacp_abort_cl();
	  }

	  /* clear start and end flags of the element */
	  msg->start = 0;
	  msg->end = 0;

	  /* connect */
	  ch->state = CHSETSTAT(ch, CHSTCONN);
	  msg->state = CRBSTFREE;
	  ch->remotega = msg->ga;

	  conninfo = (conninfo_t *)(ch->chbody);
	  tmpch = conninfo->nextch;
#ifdef DEBUG
	  printf("%d: handl_conchlist: recv con req idx %llu has been connected to ga %p (rank %d)\n", 
		 myrank, crbidx, ch->remotega, acp_query_rank(ch->remotega));
	  fflush(stdout);
#endif

	  /* clear chbody */
	  memset (ch->chbody, 0, sizeof(uint64_t)*3 + ch->rbuf_entrynum * ch->buf_entrysz);

	  /* use the first slot of the receive queue for sending back localga */
	  *((acp_ga_t *)CHRBSLOTADDR(ch->chbody, 0, ch->buf_entrysz)) = ch->localga;
	  acp_copy(ch->remotega+CONNINFO_REMGA_OFFSET,
		   CHRBSLOTGA(ch->localga, 0, ch->buf_entrysz),
		   sizeof(acp_ga_t), ACP_HANDLE_NULL);

#ifdef DEBUG
	  printf("%d: handl_conchlist: send back acknowledgemnet ga %p \n", 
		 myrank, ch->localga);
	  fflush(stdout);
#endif

	  /* remove this channel from the connecting list */
	  list_remove(&conch_list, (listitem_t *)ch);

	  /* check if there are pending requests in this channel already */
	  if (list_isempty(&(ch->reqs)))
	    list_add(&idlech_list, (listitem_t *)ch); // add this channel to idle channels list 
	  else
	    list_add(&reqch_list, (listitem_t *)ch); // add this channel to requesting channels list 

	  /* check if there is a connecting channel with the same type and peer */
	  if (tmpch != NULL)
	    list_add(&conch_list, (listitem_t *)tmpch); // add the next channel to the connecting channels list 

	  /* quit from the loop for searching connection request buffer */
	  break;

	}
	crbidx++;
	idx = crbidx % crbentrynum;
      }
      break;
    default:
      printf("handl_conchlist: rank %d : wrong channel type %d\n", 
	      myrank, ch->type);
	fflush(stdout);
      iacp_abort_cl();
    }
    ch = nextch;
  }
}

  /* check connection request buffer */
static void handl_crb(void)
{
  uint64_t crbidx;
  crbmsg_t *msg;
  int idx;
  int myrank;

  myrank = acp_rank();

  crbidx = *crbhead;
  idx = crbidx % crbentrynum;

  while ((crbidx < *crbtail) && (crbtbl[idx].state == CRBSTFREE)){
    crbidx++;
    idx = crbidx % crbentrynum;
  }

  /* update crbhead */
  *crbhead = crbidx;

  if ((*crbtail - *crbhead) >= crbentrynum){
    /* invalidate the request at the head (== oldest request) 
     * to avoid deadlock */
    msg = &(crbtbl[idx]);
    msg->state = CRBSTFREE;
    /* increment state of connection information on the requester process
     * to invalidate the request */
    acp_add4(trashboxga, 
	     msg->ga + CONNINFO_STATE_OFFSET, 1LL, ACP_HANDLE_NULL);
    *crbhead = crbidx + 1;

#ifdef DEBUG
    printf("%d: handl_crb: invalidated crbidx %llu rank %d ga %p\n", 
	   myrank, crbidx, acp_query_rank(msg->ga), msg->ga);
    fflush(stdout);
#endif

  }
}

/* setup free request list of the channel
 */
static void initreq(acp_ch_t ch)
{
  int i;
  chreqitem_t *reqitem;
  int myrank;

  myrank = acp_rank();

  ch->reqtable = (chreqitem_t *)malloc(sizeof(chreqitem_t) * acpch_chreqnum);
  if (ch->reqtable == NULL){
    printf("initreq(): %d: cannot allocate reqtable\n", myrank);
	fflush(stdout);
    iacp_abort_cl();
  }
  
  for (i = 0; i < acpch_chreqnum-1; i++){
    reqitem = &(ch->reqtable[i]);
    reqitem->prev = NULL;
    reqitem->next = &(ch->reqtable[i+1]);
    reqitem->ch = ch;
  }
  ch->reqtable[acpch_chreqnum-1].prev = NULL;
  ch->reqtable[acpch_chreqnum-1].next = NULL;

  ch->freereqs = &(ch->reqtable[0]);
  ch->initreqs = NULL;
  list_init(&(ch->reqs));
#ifdef DEBUG
  printf("%d: initreq: reqtable %p \n", myrank, ch->reqtable);
    fflush(stdout);
#endif
}

/* create a new request item and place it at the tail of the request list
 * of the channel.
 */
static chreqitem_t *newreq(acp_ch_t ch)
{
  chreqitem_t *req;
  int myrank;

  myrank = acp_rank();

  /* check one request entry from free request list */
  req = ch->freereqs;

  if (req != NULL){
    /* connect the item at the tail of the request list */
    ch->freereqs = req->next;
    list_add(&(ch->reqs), (listitem_t *)req);

    /* if there was no requests waiting to be handled, set initreqs to this item */
    if ((ch->type == CHTYRECV) && (ch->initreqs == NULL))
      ch->initreqs = req;

    /* initialize members of the request item */
    req->msgidx = 0;
    req->addr = NULL;
    req->size = 0;
    req->atkey = ACP_ATKEY_NULL;
    req->hdl = ACP_HANDLE_NULL;
#ifdef DEBUG
    printf("%d: newreq: req %p \n", myrank, req);
    fflush(stdout);
#endif
  } 

  return req;
}

/* free a request item */
static int freereq(chreqitem_t *req)
{
  chitem_t *ch = req->ch;

  /* add the request to the free request list */
  req->prev = NULL;
  req->next = ch->freereqs;
  ch->freereqs = req;

  return 0;
}



/* progress send requests in a channel */
static void progress_send(acp_ch_t ch)
{
  chreqitem_t *req, *nextreq;
  int sbufidx;
  uint64_t head, tail;
  char *msgslot;
  size_t msgsz;
  acp_ga_t ga;
  int myrank;

  myrank = acp_rank();

  req = (chreqitem_t *)(ch->reqs.head);

  head = *(CHLOCALHEAD(ch));
  tail = *(CHLOCALTAIL(ch));

  while ((req != NULL) && ((tail - head) < ch->rbuf_entrynum)){
    sbufidx = req->msgidx % ch->sbuf_entrynum;
    if (acp_inquire(ch->shdl[sbufidx])!=0)
      /* sbuf is in use. stop progress of nbsends. */
      break;

    msgslot = CHSBSLOTADDR(ch->chbody, sbufidx, ch->buf_entrysz);

    /* create a message to be sent */
    switch(req->type){
    case CHREQTYSNDEGR:
      *(CHMSGTYPE(msgslot)) = CHMSGTYEGR;
      *(CHMSGSIZE(msgslot)) = req->size;
      memcpy(CHMSGBODY(msgslot), req->addr, req->size);
      msgsz = CHMSGHDRSZ + req->size;
#ifdef DEBUG
      printf("%d: progress_send: eager req %p msgslot %p type %d size %llu\n", 
	     myrank, req, msgslot, *(CHMSGTYPE(msgslot)), *(CHMSGSIZE(msgslot)));
      fflush(stdout);
#endif
      break;
    case CHREQTYSNDRND:
      *(CHMSGTYPE(msgslot)) = CHMSGTYRND;
      *(CHMSGSIZE(msgslot)) = req->size;
      req->atkey = acp_register_memory(req->addr, req->size, 0);
      ga = acp_query_ga(req->atkey, req->addr);
      *((acp_ga_t *)CHMSGBODY(msgslot)) = ga;
      msgsz = CHMSGRNDSZ;
#ifdef DEBUG
      printf("%d: progress_send: rendez req %p msgslot %p type %d size %llu ga %p (atkey %p)\n", 
	     myrank, req, msgslot, *(CHMSGTYPE(msgslot)), *(CHMSGSIZE(msgslot)), ga, req->atkey);
      fflush(stdout);
#endif
      break;
    case CHREQTYDISCON:
      *(CHMSGTYPE(msgslot)) = CHMSGTYDISCON;
      *(CHMSGSIZE(msgslot)) = 0;
      msgsz = CHMSGHDRSZ;
#ifdef DEBUG
      printf("%d: progress_send: discon req %p msgslot %p type %d size %llu\n", 
	     myrank, req, msgslot, *(CHMSGTYPE(msgslot)), *(CHMSGSIZE(msgslot)));
      fflush(stdout);
#endif
      break;
    default:
      printf("progress_send(): Wrong request type %d\n", req->type);
      fflush(stdout);
      iacp_abort_cl();
    }

    /* send the message */
    ch->shdl[sbufidx] = acp_copy(CHRBSLOTGA(ch->remotega,tail,ch->buf_entrysz),
				 CHSBSLOTGA(ch->localga,sbufidx,ch->buf_entrysz),
				 msgsz, ACP_HANDLE_NULL);
#ifdef DEBUG
    printf("%d: progress_send: acp_copy to %p from %p size %d\n", 
	   myrank, CHRBSLOTGA(ch->remotega,tail,ch->buf_entrysz),
	   CHSBSLOTGA(ch->localga,sbufidx,ch->buf_entrysz), msgsz);
    fflush(stdout);
#endif

    /* update tail */
    tail++;
    *(CHLOCALTAIL(ch)) = tail;
    req->hdl = acp_copy(CHREMOTETAILGA(ch), CHLOCALTAILGA(ch), sizeof(uint64_t), ACP_HANDLE_NULL);
#ifdef DEBUG
    printf("%d: progress_send: put tail %llu %llu (%p) from %p to %p\n", 
	   myrank, tail, *(CHLOCALTAIL(ch)), CHLOCALTAIL(ch), CHLOCALTAILGA(ch), CHREMOTETAILGA(ch));
    fflush(stdout);
#endif

    /* remove the request from active request list, and move on to the next request */
    nextreq = req->next;
    list_remove(&(ch->reqs), (listitem_t *)req);
    req = nextreq;
  }

}

/* progress recv requests in a channel */
static void progress_recv(acp_ch_t ch)
{
  chreqitem_t *req, *nextreq;
  uint64_t head, tail, idx;
  char *msgslot;
  size_t size;
  acp_ga_t localga, remotega;
  int type, headidx;
  int myrank;

  myrank = acp_rank();

  /* handle newly added requests */
  req = ch->initreqs;
  head = *(CHLOCALHEAD(ch));
  tail = *(CHLOCALTAIL(ch));

  while ((req != NULL) && (tail-head)>0){
    headidx = head % ch->rbuf_entrynum;
    msgslot = CHRBSLOTADDR(ch->chbody, headidx, ch->buf_entrysz);
    type = *(CHMSGTYPE(msgslot));
    size = *(CHMSGSIZE(msgslot));
    nextreq = req->next;
#ifdef DEBUG
    printf("%d: progress_recv: match req %p with msg slot %p type %d size %llu\n", myrank, req, msgslot, type, size);
    fflush(stdout);
#endif

    /* exact size to be copied is the smaller one */
    if (req->size > size) req->size = size;
    size = req->size;

    /* retrieve one message from the head of the receive queue */
    switch(type){
    case CHMSGTYEGR:
      memcpy(req->addr, CHMSGBODY(msgslot), size);
#ifdef DEBUG
      printf("%d: progress_recv: eager request %p. copied to %p with size %llu\n", myrank, req, req->addr, size);
    fflush(stdout);
#endif

      list_remove(&(ch->reqs), (listitem_t *)req);
      break;
    case CHMSGTYRND:
      remotega = *((acp_ga_t *)CHMSGBODY(msgslot));

      /* register rbuf */
      req->atkey = acp_register_memory(req->addr, size, 0);
      localga = acp_query_ga(req->atkey, req->addr);

      /* start get data from the sender */
      req->hdl = acp_copy(localga, remotega, size, ACP_HANDLE_NULL);
      req->type = CHREQTYRCVRND;

#ifdef DEBUG
      printf("%d: progress_recv: rendezvous request %p. get from ga %p to ga %p (atkey %p) with size %llu handle %llu\n", 
	     myrank, req, remotega, localga, req->atkey, size, req->hdl);
    fflush(stdout);
#endif

      break;
    case CHMSGTYDISCON:
      if (req->type != CHREQTYDISCON){
	printf("progress_recv(): Message mismatch. src %d dst %d \n", 
		type, req->type);
	fflush(stdout);
	iacp_abort_cl();
      }
      
      list_remove(&(ch->reqs), (listitem_t *)req);

      break;
    default:
      printf("progress_recv(): Wrong request type %d from sender\n", type);
	fflush(stdout);
      iacp_abort_cl();
    }

    /* update head */
    head++;
    *(CHLOCALHEAD(ch)) = head;
    acp_copy(CHREMOTEHEADGA(ch), CHLOCALHEADGA(ch), sizeof(uint64_t), ACP_HANDLE_NULL);

#ifdef DEBUG
    printf("%d: progress_recv: head updated to %llu \n", myrank, head);
    fflush(stdout);
#endif

    if (type == CHMSGTYDISCON)
      return;

    /* move on to the next request */
    req = nextreq;
  }

  /* update initreqs to the head of the not handled requests */
  ch->initreqs = req;

  /* handle waiting rendezvous requests */
  req = (chreqitem_t *)(ch->reqs.head);

  while ((req != NULL) && (req != ch->initreqs)){
    /* requests must be rendezvous receive */
    if (req->type != CHREQTYRCVRND){
      printf("progress_recv(): Wrong request type %d\n", req->type);
	fflush(stdout);
      iacp_abort_cl();
    }

    /* check if the data has been arrived */
    if (acp_inquire(req->hdl) != 0)
      break;

#ifdef DEBUG
    printf("%d: progress_recv: complete req %p hdl %llu \n", myrank, req, req->hdl);
    fflush(stdout);
#endif

    /* unregister rbuf */
    acp_unregister_memory(req->atkey);

    /* update message index */
    *(CHLOCALIDX(ch)) = req->msgidx + 1;
    acp_copy(CHREMOTEIDXGA(ch), CHLOCALIDXGA(ch), sizeof(uint64_t), ACP_HANDLE_NULL);

    /* move on to the next request */
    nextreq = req->next;
    list_remove(&(ch->reqs), (listitem_t *)req);
    req = nextreq;
  }
}

/* check requesting channels */
static void handl_reqchlist(void)
{
  acp_ch_t ch, nextch;
  int myrank;

  myrank = acp_rank();
  ch = (acp_ch_t)(reqch_list.head);

  while (ch != NULL){
    switch(ch->type){
    case CHTYSEND:
      progress_send(ch);
      break;
    case CHTYRECV:
      progress_recv(ch);
      break;
    default:
      printf("handl_reqchlist: rank %d : wrong channel type %d\n", 
	      myrank, ch->type);
	fflush(stdout);
      iacp_abort_cl();
    }

    nextch = ch->next;
    /* if request list is empty, move the channel to the idle channels list */
    if (list_isempty(&(ch->reqs))){
      list_remove(&reqch_list, (listitem_t *)ch);
      list_add(&idlech_list, (listitem_t *)ch);
#ifdef DEBUG
      printf("%d: handl_reqchlist: move channel %p from request list to idle list\n", myrank, ch);
      fflush(stdout);
#endif
    }
    ch = nextch;
  }
}

/* progress */
static void progress(void)
{
  /* check connecting channels */
  handl_conchlist();

  /* check connection request buffer */
  handl_crb();

  /* check requesting channels */
  handl_reqchlist();
}

/* int iacp_init_cl(void)
 *  return: 0 (success)
 *  
 *  error: aborts when starter memory is less than crb size
 *
 *  contents:
 *  - set parameters
 *  - prepare crb (connection request buffer)
 *  - prepare channel lists
 *  - synchronize among processes
 * 
 */
int iacp_init_cl(void) 
{
  int crbsz;
  int myrank;

  myrank = acp_rank();

  /* set default parameters 
   *   todo: modify parameters according to the runtime options 
   */
  crbentrynum = ACPCI_CRBENTRYNUM;
  acpch_chreqnum = ACPCI_CHREQNUM;
  acpch_eager_limit = ACPCI_EAGER_LIMIT;

  /* prepare crb (connection request buffer) 
   *   - check the size of the starter memory 
   *   - structure of crb:
   *        head: head of the connection request queue, uint64_t
   *        tail: tail of the connection request queue, uint64_t
   *        crbentrynum*crbmsg: body of the queue
   *        trashbox: used as a dummy target of remote atomic operation, uint64_t
   */
  crbsz = 2*(sizeof(uint64_t)) + crbentrynum * sizeof(crbmsg_t) + sizeof(uint64_t);
  if (iacp_starter_memory_size_cl < crbsz){
    printf("Error: iacp_init_cl: iacp_starter_memory_size_cl %d is smaller than crbsz %d\n",
	    iacp_starter_memory_size_cl, crbsz);
	fflush(stdout);
    iacp_abort_cl();
  }
#ifdef DEBUG
  printf("%d: iacp_init_cl: crbsz %d \n", myrank, crbsz);
    fflush(stdout);
#endif

  /* set addresses of crb */
  crbhead = (uint64_t *)acp_query_address(iacp_query_starter_ga_cl(myrank));
  crbtail = (uint64_t *)((char *)crbhead + CRB_TAIL_OFFSET);
  crbtbl = (crbmsg_t *)((char *)crbhead + CRB_TABLE_OFFSET);
  trashboxga = iacp_query_starter_ga_cl(myrank) + CRB_TRASHBOX_OFFSET;

#ifdef DEBUG
  printf("%d: iacp_init_cl: crbhead %p, crbtail %p, crbtbl %p, trashboxga %p\n", 
	 myrank, crbhead, crbtail, crbtbl, trashboxga);
  fflush(stdout);
#endif

  /* clear crb tbl */
  memset (crbtbl, 0, crbentrynum * sizeof(crbmsg_t));

  /* initialize counters of crb */
  *crbhead = 0LL;
  *crbtail = 0LL;

  /*                                         
   * initialize lists
   */
  list_init(&conch_list);
  list_init(&idlech_list);
  list_init(&reqch_list);

  /* synchronize */
  acp_sync();
  
  return 0; 
};

int iacp_finalize_cl(void) 
{
  acp_ch_t ch;
  /* wait until lists conch_list, reqch_list and disconnch_list becomes empty
   *  (all channel endpoints must be disconnected or in idle list
   */
  while ((conch_list.head != NULL) || (reqch_list.head != NULL))
    progress();

  /* NOTE: acp_sync is still available in the finalization function? */
  acp_sync();

  /* free channels */
  ch = (acp_ch_t)idlech_list.head;
  while(ch != NULL){
    list_remove(&idlech_list, (listitem_t *)ch);
    acp_unregister_memory(ch->localatkey);
    free(ch->chbody);
    free(ch->reqtable);
    if (ch->shdl != NULL)
      free(ch->shdl);
    ch = (acp_ch_t)idlech_list.head;
  }
#ifdef DEBUG
  printf("finalize: done \n");
  fflush(stdout);
#endif

  return 0; 
};

void iacp_abort_cl(void) 
{ 
  exit (-1);
};

/* acp_ch_t acp_create_ch(int src, int dst)
 *  parameters: 
 *    int src : source process of the channel
 *    int dst : destination process of the channel
 *
 *  return:
 *    acp_ch_t : success. a channel handle.
 *
 *  error:
 *    - src or dst is not within the 0 <= p < procs
 *    - neither src nor dst is myrank
 *    - src == dst
 *    - memory not available
 *    
 *  behavior
 *    - allocate channel endpoint and buffers
 *    - initialize channel parameters
 *    - setup a connection request message in the send channel buffer
 *    - connect this channel to conch_list
 *    - progress
 *
 *  calls
 *    - initreq()
 *    - list_add()
 *
 *  allocate
 *    - src process:
 *        endpoint: sizeof(chitem_t)
 *        send buffer: 8*3+sbuf_entrynum*buf_entrysz
 *        send handle table: sizeof(acp_handle_t)*sbuf_entrynum
 *        requests: sizeof(chreqitem_t)*acpch_chreqnum
 *    - dst process: 
 *        endpoint: sizeof(chitem_t)
 *        receive buffer: 8*3+rbuf_entrynum*buf_entrysz
 *        requests: sizeof(chreqitem_t)*acpch_chreqnum
 *
 */

acp_ch_t acp_create_ch(int src, int dst)
{
  int myrank, procs, type;
  size_t memsize;
  acp_ch_t ch, conch;
  conninfo_t *conninfo;

  myrank = acp_rank();
  procs = acp_procs();

  /* check parameters */
  if ((src < 0) || (src >= procs) || (dst < 0) || (dst >= procs)){
    printf("acp_create_ch: rank %d : Error: Wrong paramter src %d, dst %d (procs = %d) \n",
	    myrank, src, dst, procs);
	fflush(stdout);
    iacp_abort_cl();
  }
  if (src == dst){
    printf("acp_create_ch: rank %d : Error: ACP does not support loop back channel (src %d, dst %d) \n",
	    myrank, src, dst);
    fflush(stdout);
    iacp_abort_cl();
  }

  /* determine sender or receiver */
  if (src == myrank)
    type = CHTYSEND;
  else if (dst == myrank)
    type = CHTYRECV;
  else{
    printf("acp_create_ch: rank %d : Error: ACP does not support remote-to-remote channel (src %d, dst %d) \n",
	    myrank, src, dst);
    fflush(stdout);
    iacp_abort_cl();
  }

  /* allocate channel endpoint */
  ch = (acp_ch_t)malloc(sizeof(chitem_t));
  if (ch == NULL){
    printf("acp_create_ch: rank %d : Error: cannot allocate channel endpoint structure \n", myrank);
    fflush(stdout);
    iacp_abort_cl();
  }

  /* set common members */
  ch->type = type;
  ch->state = CHSTINIT;
  ch->buf_entrysz = ACPCI_CHENTRYSZ;
  ch->sbuf_entrynum = ACPCI_CHSBUFENTRYNUM;
  ch->rbuf_entrynum = ACPCI_CHRBUFENTRYNUM;

  /* setup request list */
  initreq(ch);

  /* set other members */
  if (ch->type == CHTYSEND){ // sender
    ch->peer = dst;

    memsize = sizeof(uint64_t)*3 + ch->sbuf_entrynum * ch->buf_entrysz;
    ch->shdl = (acp_handle_t *)malloc(sizeof(acp_handle_t)*ch->sbuf_entrynum);
  } else{ // receiver
    ch->peer = src;

    memsize = sizeof(uint64_t)*3 + ch->rbuf_entrynum * ch->buf_entrysz;
    ch->shdl = NULL;
  }

  ch->chbody = (char *)malloc(memsize);
  if (ch->chbody == NULL){
    printf("acp_create_ch: rank %d : Error: cannot allocate chbody \n", myrank);
	fflush(stdout);
    iacp_abort_cl();
  }

  /* initialize head, tail and idx */
  *(CHLOCALHEAD(ch)) = 0LL;
  *(CHLOCALTAIL(ch)) = 0LL;
  *(CHLOCALIDX(ch)) = 0LL;

  ch->localatkey = acp_register_memory(ch->chbody, memsize, 0);
  ch->localga = acp_query_ga(ch->localatkey, ch->chbody);
  if (ch->localga == ACP_GA_NULL){
    printf("acp_create_ch: rank %d : cannot register buffer \n",
	    myrank);
	fflush(stdout);
    iacp_abort_cl();
  }

#ifdef DEBUG
  printf("acp_create_ch: rank %d : ch %p chbody %p ch->reqs.head %p localga %016llx sz %d sb %d rb %d\n", 
	 myrank, ch, ch->chbody, ch->reqs.head, ch->localga, 
	 ch->buf_entrysz, ch->sbuf_entrynum, ch->rbuf_entrynum);
  fflush(stdout);
#endif

  conninfo = (conninfo_t *)(ch->chbody);
  conninfo->nextch = NULL;
  if (ch->type == CHTYSEND){
    /* initialize chbody to be used as a connection request structure */
    (conninfo->crbmsg).start = 1;
    (conninfo->crbmsg).rank = myrank;
    (conninfo->crbmsg).ga = ch->localga;
    (conninfo->crbmsg).state = CRBSTINUSE;
    (conninfo->crbmsg).buf_entrysz = ch->buf_entrysz;
    (conninfo->crbmsg).rbuf_entrynum = ch->rbuf_entrynum;
    (conninfo->crbmsg).sbuf_entrynum = ch->sbuf_entrynum;
    (conninfo->crbmsg).end = 1;
#ifdef DEBUG
    printf("acp_create_ch: rank %d : crbmsg %d %d %016llx %d %d %d %d %d \n", 
	   myrank, (conninfo->crbmsg).start, (conninfo->crbmsg).rank, (conninfo->crbmsg).ga,
	   (conninfo->crbmsg).state, (conninfo->crbmsg).buf_entrysz, (conninfo->crbmsg).rbuf_entrynum,
	   (conninfo->crbmsg).sbuf_entrynum, (conninfo->crbmsg).end);
    fflush(stdout);
#endif
    conninfo->starterga = iacp_query_starter_ga_cl(ch->peer);
    conninfo->remotega = ACP_GA_NULL;
  }
    
  /* search for the connection requests with same peer and type */
  conch = (acp_ch_t)(conch_list.head);
  while (conch != NULL){
    if ((conch->type == ch->type) && (conch->peer == ch->peer))
      break;
    conch = conch->next;
  }

  if (conch != NULL){ 
    /* found connecting channel with the same peer and type.
     * add this channel to the tail of the list of connecting channels 
     * with the same peer and type. */
    while (((conninfo_t *)(conch->chbody))->nextch != NULL)
      conch = ((conninfo_t *)(conch->chbody))->nextch;
    ((conninfo_t *)(conch->chbody))->nextch = ch;
#ifdef DEBUG
    printf("acp_create_ch: rank %d : added a channel with peer %d and type %d\n", 
	 myrank, ch->peer, ch->type);
    fflush(stdout);
#endif

  }else{
    /* there is no connecting channels with the same peer and type */
    list_add(&conch_list, (listitem_t *)ch);

    /* this channel is in the head. start getting head (and adding_and_fetching tail) */
    if (ch->type ==CHTYSEND)
      init_conninfo(ch);
#ifdef DEBUG
    printf("acp_create_ch: rank %d : added and initiated a channel with peer %d and type %d\n", 
	   myrank, ch->peer, ch->type);
    fflush(stdout);
#endif
  }

  /* progress requests */
  progress();

  /* return channel endpoint handle */
  return ch;
}

acp_request_t acp_nbfree_ch(acp_ch_t ch)
{
 chreqitem_t *req;
 int myrank;

 myrank == acp_rank();

 if (CHCHECKDISCON(ch)!=0){
    printf("acp_nbfree_ch: rank %d : Error: channel has already been requested to disconnect\n", 
	    myrank);
	fflush(stdout);
    iacp_abort_cl();
  }

  /* if the request list of this channel is empty, move it to reqch_list  */
 if ((CHCHECKSTAT(ch) == CHSTCONN) &&(list_isempty(&(ch->reqs)))){
    list_remove(&idlech_list, (listitem_t *)ch);
    list_add(&reqch_list, (listitem_t *)ch);
#ifdef DEBUG
    printf("acp_nbfree_ch: rank %d : moved channel %p from idle to requesting\n", 
	 myrank, ch);
    fflush(stdout);
#endif
  }

  /* add disconnection request */
  req = newreq(ch);
  if (req == NULL){
    return NULL;
  }

  /* set message info to req */
  req->msgidx = ch->msgidx++;
  req->addr = NULL;
  req->size = 0;
  req->type = CHREQTYDISCON;

  /* progress */
  progress();

  /* change status to disconnecting */
  ch->state = CHSETDISCON(ch);

  return req;
}

int acp_free_ch(acp_ch_t ch)
{
 }

acp_request_t acp_nbsend_ch(acp_ch_t ch, void *sbuf, size_t sz)
{
  chreqitem_t *req;
  int myrank;

  myrank = acp_rank();

  if (CHCHECKDISCON(ch) != 0){
    printf("acp_nbsend_ch: rank %d : Error: requesting nbsend to the channel that has been requested to disconnect\n", 
	   myrank);
    fflush(stdout);
    iacp_abort_cl();
  }

  /* if the request list of this channel is empty, move it to reqch_list  */
  if ((CHCHECKSTAT(ch) == CHSTCONN) &&(list_isempty(&(ch->reqs)))){
    list_remove(&idlech_list, (listitem_t *)ch);
    list_add(&reqch_list, (listitem_t *)ch);
#ifdef DEBUG
    printf("acp_nbsend_ch: rank %d : moved channel %p from idle to requesting\n", 
	   myrank, ch);
    fflush(stdout);
#endif
  }

  /* get a new request item */
  req = newreq(ch);
  if (req == NULL)
    return NULL;

  /* set message info to req */
  req->msgidx = ch->msgidx++;
  req->addr = sbuf;
  req->size = sz;
  if (sz <= acpch_eager_limit){
    req->type = CHREQTYSNDEGR;
#ifdef DEBUG
    printf("acp_nbsend_ch: rank %d : prepared eager mesg in channel %p req->msgidx %llu remote msgidx %llu\n", 
	   myrank, ch, req->msgidx, *(CHLOCALIDX(ch)));
    fflush(stdout);
#endif
  }else {
    req->type = CHREQTYSNDRND;    
#ifdef DEBUG
    printf("acp_nbsend_ch: rank %d : prepared rendz mesg in channel %p req->msgidx %llu remote msgidx %llu\n", 
	   myrank, ch, req->msgidx, *(CHLOCALIDX(ch)));
    fflush(stdout);
#endif
  }

  /* progress */
  progress();

  return req;
}

acp_request_t acp_nbrecv_ch(acp_ch_t ch, void *rbuf, size_t sz)
{
  chreqitem_t *req;
  int myrank;

  myrank = acp_rank();

  if (CHCHECKDISCON(ch) != 0){
    printf("acp_nbrecv_ch: rank %d : Error: channel has been requested to disconnect\n", 
	    myrank);
    fflush(stdout);
    iacp_abort_cl();
  }

  /* if the request list of this channel is empty, move it to reqch_list  */
  if ((CHCHECKSTAT(ch) == CHSTCONN) &&(list_isempty(&(ch->reqs)))){
    list_remove(&idlech_list, (listitem_t *)ch);
    list_add(&reqch_list, (listitem_t *)ch);
#ifdef DEBUG
    printf("acp_nbrecv_ch: rank %d : moved channel %p from idle to requesting\n", 
	 myrank, ch);
    fflush(stdout);
#endif
  }

  /* get a new request item */
  req = newreq(ch);
  if (req == NULL)
    return NULL;

  /* set message info to req */
  req->msgidx = ch->msgidx++;
  req->addr = rbuf;
  req->size = sz;
  req->type = CHREQTYRCV;

#ifdef DEBUG
  printf("acp_nbrecv_ch: rank %d : prepared receive in channel %p\n", 
	 myrank, ch);
    fflush(stdout);
#endif

  /* progress */
  progress();

  return req;
}


int acp_send_ch(acp_ch_t ch, void * sbuf, size_t sz)
{
}

int acp_recv_ch(acp_ch_t ch, void *rbuf, size_t sz)
{
}


size_t acp_wait_ch(acp_request_t req)
{
  acp_ch_t ch;
  int myrank;

  myrank = acp_rank();

  ch = req->ch;

  switch(req->type){
  case CHREQTYSNDEGR:
    while((CHCHECKSTAT(ch) != CHSTCONN) || (req->msgidx >= *(CHLOCALTAIL(ch))) || (acp_inquire(req->hdl) != 0))
      progress();

#ifdef DEBUG
    printf("acp_wait_ch: rank %d : completed send eager ch %p msgidx %llu head %llu tail %llu type %d state %d\n", 
	   myrank, ch, req->msgidx, *(CHLOCALHEAD(ch)), *(CHLOCALTAIL(ch)), ch->type, ch->state);
    fflush(stdout);
#endif

    freereq(req);

    break;
  case CHREQTYSNDRND:
    while((CHCHECKSTAT(ch) != CHSTCONN) || (req->msgidx >= *(CHLOCALIDX(ch))))
      progress();

#ifdef DEBUG
    printf("acp_wait_ch: rank %d : completed send rendezvous ch %p msgidx %llu head %llu tail %llu idx %llu type %d state %d\n", 
	   myrank, ch, req->msgidx, *(CHLOCALHEAD(ch)), *(CHLOCALTAIL(ch)), *(CHLOCALIDX(ch)), ch->type, ch->state);
    fflush(stdout);
#endif
    acp_unregister_memory(req->atkey); // unregister send buffer for rendezvous

    freereq(req);
    break;
  case CHREQTYRCV:
    /* eager receive or rendezvous receive. 
     * since the message type is not known at this point,
     * wait for the message to be sent, first */
    while((CHCHECKSTAT(ch) != CHSTCONN) || (req->msgidx >= *(CHLOCALHEAD(ch))))
      progress();

    /* check if the message type has been changed to rendezvous receive */
    if (req->type == CHREQTYRCVRND){
#ifdef DEBUG
      printf("acp_wait_ch: rank %d : start waiting rendezvous recv req->msgidx %llu ch idx %p\n", 
	     myrank, req->msgidx, *(CHLOCALIDX(ch)));
      fflush(stdout);
#endif
      while(req->msgidx >= *(CHLOCALIDX(ch)))
	progress();
      acp_unregister_memory(req->atkey); // unregister recv buffer for rendezvous
    }

    freereq(req);
    break;

  case CHREQTYRCVRND:
    while((CHCHECKSTAT(ch) != CHSTCONN) || (req->msgidx >= *(CHLOCALIDX(ch)))) 
      progress();

    acp_unregister_memory(req->atkey); // unregister recv buffer for rendezvous

    freereq(req);
    break;
  case CHREQTYDISCON:
    switch (ch->type){
    case CHTYSEND:
      while((CHCHECKSTAT(ch) != CHSTCONN) || (req->msgidx >= *(CHLOCALTAIL(ch))) || (acp_inquire(req->hdl) != 0))
	progress();
#ifdef DEBUG
      printf("acp_wait_ch: rank %d : discon sender ch done\n", myrank);
      fflush(stdout);
#endif
      break;
    case CHTYRECV:
      while((CHCHECKSTAT(ch) != CHSTCONN) || (req->msgidx >= *(CHLOCALHEAD(ch))))
	progress();
#ifdef DEBUG
      printf("acp_wait_ch: rank %d : discon receiver ch done\n", myrank);
      fflush(stdout);
#endif
      break;
    default:
      printf("acp_wait_ch: rank %d : Error: wrong channel type %d\n", myrank, ch->type);
	fflush(stdout);
      iacp_abort_cl();
    }

    acp_unregister_memory(ch->localatkey); // unregister chbody
    list_remove(&idlech_list, (listitem_t *)ch);
    free(ch->chbody);
    free(ch->reqtable);
    if (ch->shdl != NULL)
      free(ch->shdl);
    free(ch);

#ifdef DEBUG
    printf("acp_wait_ch: rank %d : channel freed\n", myrank);
    fflush(stdout);
#endif

    break;
  default:
    printf("acp_wait_ch: rank %d : Error: Wrong request type %d\n", myrank, req->type);
    fflush(stdout);
    iacp_abort_cl();
  }
}

int acp_waitall_ch(acp_request_t *req, int num, size_t *sz)
{
}

