/*
  Message passing library independent functions of parform

  This file contains functions needed for the parallel version of form3 
  these functions need no real link to the message passing libraries, they 
  only need some interface dependent preprocessor definitions (check 
  parallel.h). So there still need two different objectfiles to be compiled 
  for mpi and pvm!

  #[ includes
*/
#include "form3.h"
extern LONG deferskipped; /* used in store.c ?? */
#include "parallel.h"
PARALLELVARS PF;
#ifdef MPI2
WORD *PF_shared_buff;
#endif

#ifdef PF_WITHLOG
#define PRINTFBUF(TEXT,TERM,SIZE)  { if(PF.log){ WORD iii;\
  fprintf(stderr,"[%d|%d] %s : ",PF.me,PF.module,TEXT);\
  if(TERM){ fprintf(stderr,"[%d] ",*TERM);\
    if((SIZE)<500 && (SIZE)>0) for(iii=1;iii<(SIZE);iii++)\
      fprintf(stderr,"%d ",TERM[iii]); }\
  fprintf(stderr,"\n");\
  fflush(stderr); } }
#else
#define PRINTFBUF(TEXT,TERM,SIZE) {}
#endif

/*
  #] includes
  #[ statistics
     #[ variables (should be part of a struct?)
*/
static LONG PF_maxinterms;   /* maximum number of terms in one inputpatch */ 
static LONG PF_linterms;     /* local interms on this proces: PF_Proces */
static LONG PF_outterms;     /* total output terms ("on master"): PF_EndSort */

#define PF_STATS_SIZE 5 
static LONG **PF_stats=0;    /* space for collecting statistics of all procs */
static LONG PF_laststat;     /* last realtime when statistics were printed */
static LONG PF_statsinterval;/* timeinterval for printing statistics */
/*
     #] variables (should be part of a struct?)
	 #[ int        PF_Statistics(LONG**,int)

	 prints statistics every PF_statinterval seconds
	 for proc = 0 it prints final statistics for EndSort.

     LONG stats[proc][5] = {cpu,space,in,gen,left} 

*/

int
PF_Statistics ARG2(LONG**,stats,int,proc)
{
  SORTING *S = AR.SS;
  LONG real,cpu;
  WORD rpart,cpart;
  int i,j;
  
  if ( AR.SS == AM.S0 && PF.me == 0 ) {
	real = PF_RealTime(PF_TIME); rpart = (WORD)(real%100); real /= 100;

	if( !PF_stats ){
	  PF_stats = (LONG**)Malloc1(PF.numtasks*sizeof(LONG*),"PF_stats 1");
	  for(i=0;i<PF.numtasks;i++){
		PF_stats[i] = (LONG*)Malloc1(PF_STATS_SIZE*sizeof(LONG),"PF_stats 2");
		for(j=0;j<PF_STATS_SIZE;j++) PF_stats[i][j] = 0;
	  }
	}

	if(proc > 0) for(i=0;i<PF_STATS_SIZE;i++) PF_stats[proc][i] = stats[0][i];

	if( real >= PF_laststat + PF_statsinterval || proc == 0){
	  LONG sum[PF_STATS_SIZE];

	  for(i=0;i<PF_STATS_SIZE;i++) sum[i] = 0;
	  sum[0] = cpu = TimeCPU(1);
	  cpart = (WORD)(cpu%1000);
	  cpu /= 1000;
	  cpart /= 10;
	  MesPrint("");
	  if(proc && AC.StatsFlag){
		MesPrint("proc          CPU         in        gen       left       byte");
		MesPrint("%3d  : %7l.%2i %10l",0,cpu,cpart,PF.ginterms);
	  }
	  else if(AC.StatsFlag){
		MesPrint("proc          CPU         in        gen       out        byte");
		MesPrint("%3d  : %7l.%2i %10l %10l %10l",
				 0,cpu,cpart,PF.ginterms,0,PF_outterms);
	  }

	  for(i=1;i<PF.numtasks;i++){
		cpart = (WORD)(PF_stats[i][0]%1000);
		cpu = PF_stats[i][0] / 1000;
		cpart /= 10;
		if (AC.StatsFlag)
		  MesPrint("%3d  : %7l.%2i %10l %10l %10l",i,cpu,cpart,
				   PF_stats[i][2],PF_stats[i][3],PF_stats[i][4]);
		for(j=0;j<PF_STATS_SIZE;j++) sum[j] += PF_stats[i][j];
	  }
	  cpart = (WORD)(sum[0]%1000);
	  cpu = sum[0] / 1000;
	  cpart /= 10;
	  if(AC.StatsFlag){
		MesPrint("Sum  = %7l.%2i %10l %10l %10l",cpu,cpart,sum[2],sum[3],sum[4]);
		MesPrint("Real = %7l.%2i %20s (%l) %16s",
				 real,rpart,AC.Commercial,PF.module,EXPRNAME(AS.CurExpr));
		MesPrint("");
	  }
	  PF_laststat = real;

	}
  }
  return(0);
}
/*
     #] int PF_Statistics(LONG**,int)
  #] statistics
  #[ sort.c
     #[ sort variables
  */

/* a node for the tree of losers in the final sorting on the master */
typedef struct Node{
  struct Node *left;
  struct Node *rght;
  int lloser;
  int rloser;
  int lsrc;
  int rsrc;
}NODE;

/* should/could be put in one struct */
static  NODE *PF_root;           /* root of tree of losers */
static  WORD PF_loser;          /* this is the last loser */
static  WORD **PF_term;          /* these point to the active terms */
static  WORD **PF_newcpos;       /* new coeffs of merged terms */
static  WORD *PF_newclen;        /* length of new coefficients */

/* preliminary: could also write somewhere else? */
static  WORD *PF_WorkSpace;      /* used in PF_EndSort */
static  UWORD *PF_ScratchSpace;  /* used in PF_GetLosers */

/*
     #] sort variables
	 #[ PF_BUFFER* PF_AllocBuf(int,LONG,WORD)

  Allocate one PF_BUFFER struct with numbuf buffers of size bsize
  For the first 'free' buffers there is no space allocated 
  For the first (index 0) buffer there is no space allocated (!!!)
  because we use existing space for it. 
  Maybe this should be really hidden in the send/recv routines and pvm/mpi 
  files, it is only comlicated because of nonblocking send/receives!
*/
PF_BUFFER*
PF_AllocBuf ARG3(int,nbufs,LONG,bsize,WORD,free)
{
  PF_BUFFER *buf;
  UBYTE *p,*stop;
  LONG allocsize;
  int i;

  allocsize = 
	(LONG)(sizeof(PF_BUFFER) + 4*nbufs*sizeof(WORD*) + (nbufs-free)*bsize); 

#ifdef MPI
  allocsize += 
	(LONG)( nbufs * (  2 * sizeof(MPI_Status)
			 		   +     sizeof(MPI_Request)
		               +     sizeof(MPI_Datatype)
		              )
		  );
#endif
  allocsize += 
	(LONG)( nbufs * 3 * sizeof(int) );

  if(!(buf = (PF_BUFFER*)Malloc1(allocsize,"PF_AllocBuf"))) return(0);  
  
  p = ((UBYTE *)buf) + sizeof(PF_BUFFER);
  stop = ((UBYTE *)buf) + allocsize;

  buf->numbufs = nbufs;
  buf->active = 0;
    
  buf->buff    = (WORD**)p;         p += buf->numbufs*sizeof(WORD*);
  buf->fill    = (WORD**)p;         p += buf->numbufs*sizeof(WORD*);
  buf->full    = (WORD**)p;         p += buf->numbufs*sizeof(WORD*);
  buf->stop    = (WORD**)p;         p += buf->numbufs*sizeof(WORD*);
#ifdef MPI
  buf->status  = (MPI_Status *)p;   p += buf->numbufs*sizeof(MPI_Status);
  buf->retstat = (MPI_Status *)p;   p += buf->numbufs*sizeof(MPI_Status);
  buf->request = (MPI_Request *)p;  p += buf->numbufs*sizeof(MPI_Request);
  buf->type    = (MPI_Datatype *)p; p += buf->numbufs*sizeof(MPI_Datatype);
  buf->index   = (int *)p;	        p += buf->numbufs*sizeof(int);

  for( i = 0; i < buf->numbufs; i++) buf->request[i] = MPI_REQUEST_NULL;
#endif
#ifdef PVM
  buf->type    = (int *)p;          p += buf->numbufs*sizeof(int);
#endif
  buf->tag     = (int *)p;          p += buf->numbufs*sizeof(int);
  buf->from    = (int *)p;          p += buf->numbufs*sizeof(int);
  /* and finally the real bufferspace */
  for(i = free; i < buf->numbufs;i++){
	buf->buff[i] = (WORD*)p;        p += bsize;
	buf->stop[i] = (WORD*)p;
	buf->fill[i] = buf->full[i] = buf->buff[i];
  }
  if( p != stop ){
	MesPrint("Error in PF_AllocBuf p = %x stop = %x\n",p,stop);
	return(0);
  }
  return(buf);
}
/*
     #] PF_BUFFER *PF_AllocBuf(int,LONG)
     #[ int        PF_InitTree()

  Initializes the sorting tree on the master.

  Allocates bufferspace (if necessary) for:
      pointers to terms in the tree and their coeffitients
	  the cyclic receive buffers for nonblocking receives
	  the nodes of the actual tree

  and initializes these with (hopefully) correct values

*/
int
PF_InitTree ARG0
{
  PF_BUFFER **rbuf = PF.rbufs;
  UBYTE *p,*stop;
  int numrbufs,numtasks = PF.numtasks;
  int i,j,src,numnodes;
  int numslaves = numtasks-1;
  long size;

  /* 
	 #[ the buffers for the new coefficients and the terms 
	    we need one for each slave 
  */
  if(!PF_term){ 
	size =  2*numtasks*sizeof(WORD*) + sizeof(WORD)*
	  ( numtasks*(1 + AM.MaxTal) + (AM.MaxTer+1) + 2*(AM.MaxTal+2)); 

	PF_term = (WORD **)Malloc1(size,"PF_term");
	stop = ((UBYTE*)PF_term) + size;
	p = ((UBYTE*)PF_term) + numtasks*sizeof(WORD*);

	PF_newcpos = (WORD **)p;  p += sizeof(WORD*) * numtasks;
	PF_newclen =  (WORD *)p;  p += sizeof(WORD)  * numtasks;
	for(i=0;i<numtasks;i++){ 
	  PF_newcpos[i] = (WORD *)p; p += sizeof(WORD)*AM.MaxTal;
	  PF_newclen[i] = 0;
	}
	PF_WorkSpace = (WORD *)p;    p += sizeof(WORD)*(AM.MaxTer+1);
	PF_ScratchSpace = (UWORD*)p; p += 2*(AM.MaxTal+2)*sizeof(UWORD);

	if( p != stop){ MesPrint("error in PF_InitTree"); return(-1); }
  }
  /* 
	 #] 
     #[ the receive buffers 
  */
  numrbufs = PF.numrbufs;
  /* this is the size we have in the combined sortbufs for one slave */
  size = (AR.SS->sTop2 - AR.SS->lBuffer - 1)/(PF.numtasks - 1);

  if(!rbuf){
	if(!(rbuf = (PF_BUFFER**)Malloc1(numtasks*sizeof(PF_BUFFER*),
									 "Master: rbufs"))) return(-1);
	if(!(rbuf[0] = PF_AllocBuf(1,0,1))) return(-1);
	for(i = 1;i < numtasks;i++){
	  if(!(rbuf[i] = PF_AllocBuf(numrbufs,sizeof(WORD)*size,1))) return(-1);
	}
  }
  rbuf[0]->buff[0] = AR.SS->lBuffer;
  rbuf[0]->full[0] = rbuf[0]->fill[0] = rbuf[0]->buff[0];
  rbuf[0]->stop[0] = rbuf[1]->buff[0] = rbuf[0]->buff[0] + 1;
  rbuf[1]->full[0] = rbuf[1]->fill[0] = rbuf[1]->buff[0];
  for(i = 2;i < numtasks;i++){
	rbuf[i-1]->stop[0] = rbuf[i]->buff[0] = rbuf[i-1]->buff[0] + size;
	rbuf[i]->full[0] = rbuf[i]->fill[0] = rbuf[i]->buff[0];
  }
  rbuf[numtasks-1]->stop[0] = rbuf[numtasks-1]->buff[0] + size;

  for(i = 1; i < numtasks; i++){
	for(j = 0; j < rbuf[i]->numbufs; j++){
	  rbuf[i]->full[j] = rbuf[i]->fill[j] = rbuf[i]->buff[j] + AM.MaxTer + 2;
	}
	PF_term[i] = rbuf[i]->fill[rbuf[i]->active];
	*PF_term[i] = 0;
	PF_IRecvRbuf(rbuf[i],rbuf[i]->active,i);
  }
  rbuf[0]->active = 0;
  PF_term[0] = rbuf[0]->buff[0];
  PF_term[0][0] = 0;
  PF.rbufs = rbuf;

  /* 
	 #] the receive buffers
	 #[ the actual tree 
	
	 calculate number of nodes in mergetree and allocate space for them 
  */
  if(numslaves < 3) numnodes = 1;
  else{
	numnodes = 2;
	while( numnodes < numslaves ) numnodes *=2;
	numnodes-=1;
  }

  if(!PF_root)
	if(!(PF_root = (NODE*)Malloc1(sizeof(NODE)*numnodes,"nodes in mergtree"))) 
	  return(-1);

  /* then initialize all the nodes */
  src = 1;
  for(i=0;i<numnodes;i++){
	if(2*(i+1) <= numnodes){
	  PF_root[i].left = &(PF_root[2*(i+1)-1]);
	  PF_root[i].lsrc = 0;
	}
	else{
	  PF_root[i].left = 0;
	  if(src<numtasks) PF_root[i].lsrc = src++;
	  else PF_root[i].lsrc = 0;
	}
	PF_root[i].lloser = 0;
  }
  for(i=0;i<numnodes;i++){
	if(2*(i+1)+1 <= numnodes){
	  PF_root[i].rght = &(PF_root[2*(i+1)]);
	  PF_root[i].rsrc = 0;
	}
	else{
	  PF_root[i].rght = 0;
	  if(src<numtasks) PF_root[i].rsrc = src++;
	  else PF_root[i].rsrc = 0;
	}
	PF_root[i].rloser = 0;
  }
  /*
	#] the actual tree 
  */
  return(numnodes);
}
/*
     #] PF_InitTree()
	 #[ WORD*      PF_PutIn(int)

  PF_PutIn replaces PutIn on the master process and is used in PF_GetLoser. 
  It puts in the next term from slaveprocess 'src' into the tree of losers
  on the master and is a lot like GetTerm. The main problems are: 
  buffering and decompression

  src == 0     => return the zeroterm PF_term[0]
  source != 0: receive terms from another machine. they are stored in
               the large sortbuffer which is divided into PF.buff[i] or 
			   in the PF.rbufs, if PF.numrbufs > 1.

  PF_term[0][0] == 0 (see InitTree), so PF_term[0] can be used to be the 
  returnvalue for a zero term (== no more terms);
*/
WORD* 
PF_PutIn ARG1(int,src)
{
  int tag;
  WORD im,r;
  WORD *m1,*m2;
  LONG size;
  PF_BUFFER *rbuf = PF.rbufs[src];
  int a = rbuf->active;
  int next = a+1 >= rbuf->numbufs ? 0 : a+1 ;
  WORD *lastterm = PF_term[src];
  WORD *term = rbuf->fill[a];
  
  if(src <= 0) return(PF_term[0]);

  if( rbuf->full[a] == rbuf->buff[a] + AM.MaxTer + 2){
	/* very first term from this src */
	tag = PF_WaitRbuf(rbuf,a,&size);
	rbuf->full[a] += size;
	if(tag == PF_ENDBUFFER_MSGTAG) *rbuf->full[a]++ = 0;
	else if(rbuf->numbufs > 1){
	  /* post a nonblock. recv. for the next buffer */
	  rbuf->full[next] = rbuf->buff[next] + AM.MaxTer + 2;
	  size = (LONG)(rbuf->stop[next] - rbuf->full[next]);
	  PF_IRecvRbuf(rbuf,next,src);
	}
  }
  if( *term == 0 && term != rbuf->full[a]) return(PF_term[0]);
  /* exception is for rare cases when the terms fitted exactly into buffer */

  if( term + *term > rbuf->full[a] || term + 1 >= rbuf->full[a] ){
newterms:
	m1 = rbuf->buff[next] + AM.MaxTer + 1;
	if(*term < 0 || term == rbuf->full[a]){ 
	  /* copy term and lastterm to the new buffer, so that they end at m1 */
	  m2 = rbuf->full[a] - 1;
	  while(m2 >= term) *m1-- = *m2--;
	  rbuf->fill[next] = term = m1 + 1;
	  m2 = lastterm + *lastterm - 1;
	  while(m2 >= lastterm) *m1-- = *m2--;
	  lastterm = m1 + 1;
	}
	else{
	  /* copy beginning of term to the next buffer so that it ends at m1 */
	  m2 = rbuf->full[a] - 1;
	  while(m2 >= term) *m1-- = *m2--;
	  rbuf->fill[next] = term = m1 + 1;
	}
	if(rbuf->numbufs == 1){
	  rbuf->full[a] = rbuf->buff[a] + AM.MaxTer + 2;
	  size = (LONG)(rbuf->stop[a] - rbuf->full[a]);
 	  PF_IRecvRbuf(rbuf,a,src);
	}
	/* wait for new terms in the next buffer */
	rbuf->full[next] = rbuf->buff[next] + AM.MaxTer + 2;
	tag = PF_WaitRbuf(rbuf,next,&size);
	rbuf->full[next] += size;
	if(tag == PF_ENDBUFFER_MSGTAG){
	  *rbuf->full[next]++ = 0;
	}
	else if(rbuf->numbufs > 1){
	  /* post a nonblock. recv. for active buffer, it is not needed anymore */
	  rbuf->full[a] = rbuf->buff[a] + AM.MaxTer + 2;
	  size = (LONG)(rbuf->stop[a] - rbuf->full[a]);
	  PF_IRecvRbuf(rbuf,a,src);
	}
	/* now savely make next buffer active */
	a = rbuf->active = next;
  }
  
  if(*term < 0){
	/* We need to decompress the term */
	im = *term;
	r = term[1] - im + 1;
	m1 = term + 2;
	m2 = lastterm - im + 1;
	while (++im <= 0) *--m1 = *--m2;
	*--m1 = r;
	rbuf->fill[a] = term = m1; 
	if(term + *term > rbuf->full[a]) goto newterms;
  }
  rbuf->fill[a] += *term;
  return(term);
}
/*
     #] WORD* PF_PutIn(int)
	 #[ int        PF_GetLoser(*NODE)
  
  Find the 'smallest' of all the PF_terms. Take also care of changing 
  coefficients and cancelling terms. When the coefficient changes, the new is 
  sitting in the array PF_newcpos, the length of the new coefficient in 
  PF_newclen. The original term will be untouched until it is copied to the 
  output buffer!
 
  Calling PF_GetLoser with argument node will return the loser of the 
  subtree under node when the next term of the stream # PF_loser 
  (the last "loserstream") is filled into the tree.
  PF_loser == 0 means we are just starting and should fill new terms into
  all the leaves of the tree.
  
*/
int
PF_GetLoser ARG1(NODE,*n)
{
  WORD comp;

  if(!PF_loser){
	/* this is for the right initialization of the tree only */
	if(n->left) n->lloser = PF_GetLoser(n->left);
	else{
	  n->lloser = n->lsrc;
	  if( *(PF_term[n->lsrc] = PF_PutIn(n->lsrc)) == 0) n->lloser = 0;
	}
	PF_loser = 0; 
	if(n->rght) n->rloser = PF_GetLoser(n->rght);
	else{
	  n->rloser = n->rsrc;
	  if( *(PF_term[n->rsrc] = PF_PutIn(n->rsrc)) == 0) n->rloser = 0;
	}
	PF_loser = 0; 
  }
  else if( PF_loser == n->lloser){
	if(n->left) n->lloser = PF_GetLoser(n->left);
	else{
	  n->lloser = n->lsrc;
	  if( *(PF_term[n->lsrc] = PF_PutIn(n->lsrc)) == 0) n->lloser = 0;	  
	}
  }
  else if(PF_loser == n->rloser){
newright:
	if(n->rght) n->rloser = PF_GetLoser(n->rght);
	else{
	  n->rloser = n->rsrc;
	  if(*(PF_term[n->rsrc] = PF_PutIn(n->rsrc)) == 0) n->rloser = 0;
	}
  }
  if(n->lloser > 0 && n->rloser > 0){
	comp = Compare(PF_term[n->lloser],PF_term[n->rloser],(WORD)0);
	if( comp > 0 )       return(n->lloser);
	else if(comp < 0) return(n->rloser);
	else{
	  /* 
		 #[ terms are equal!
	  */
	  WORD *lcpos,*rcpos;
	  UWORD *newcpos;
	  WORD lclen,rclen,newclen,newnlen;

	  if ( AR.SS->PolyWise ) {
		/* 
		   #[ Here we work with PolyFun 
		*/
		WORD *tt1, *w;
		WORD r1,r2;
		WORD *ml = PF_term[n->lloser];
		WORD *mr = PF_term[n->rloser];
		
		if ( ( r1 = (int)*PF_term[n->lloser] ) <= 0 ) r1 = 20;
		if ( ( r2 = (int)*PF_term[n->rloser] ) <= 0 ) r2 = 20;
		tt1 = ml;
		ml += AR.SS->PolyWise;
		mr += AR.SS->PolyWise;
		w = AR.WorkPointer;
		if ( w + ml[1] + mr[1] > AM.WorkTop ) {
		  MesPrint("A WorkSpace of %10l is too small",AM.WorkSize);
		  Terminate(-1);
		}
		AddArgs(ml,mr,w);
		r1 = w[1];
		if ( r1 <= FUNHEAD ) { 
		  goto cancelled; 
		}
		if ( r1 == ml[1] ) {
		  NCOPY(ml,w,r1);
		}
		else if ( r1 < ml[1] ) {
		  r2 = ml[1] - r1;
		  mr = w + r1;
		  ml += ml[1];
		  while ( --r1 >= 0 ) *--ml = *--mr;
		  mr = ml - r2;
		  r1 = AR.SS->PolyWise;
		  while ( --r1 >= 0 ) *--ml = *--mr;
		  *ml -= r2;
		  PF_term[n->lloser] = ml;
		}
		else { 
		  r2 = r1 - ml[1]; 
		  if( r2 > 2*AM.MaxTal) 
			MesPrint("warning: new term in polyfun is large");
		  mr = tt1 - r2;
		  r1 = AR.SS->PolyWise;
		  ml = tt1;
		  *ml += r2;
		  PF_term[n->lloser] = mr;
		  NCOPY(mr,ml,r1);
		  r1 = w[1];
		  NCOPY(mr,w,r1);
		}
		PF_newclen[n->rloser] = 0;
		PF_loser = n->rloser;
		goto newright;
		/* 
		   #] Here we work with PolyFun 
		*/
	  }
	  if(lclen = PF_newclen[n->lloser]) lcpos = PF_newcpos[n->lloser];
	  else{
		lcpos = PF_term[n->lloser];
		lclen = *(lcpos += *lcpos - 1);
		lcpos -= ABS(lclen) - 1;
	  }
	  if(rclen = PF_newclen[n->rloser]) rcpos = PF_newcpos[n->rloser];
	  else{
		rcpos = PF_term[n->rloser];
		rclen = *(rcpos += *rcpos - 1);
		rcpos -= ABS(rclen) -1;
	  }
	  lclen = ( (lclen > 0) ? (lclen-1) : (lclen+1) ) >> 1;
	  rclen = ( (rclen > 0) ? (rclen-1) : (rclen+1) ) >> 1;
	  newcpos = PF_ScratchSpace;
	  if(AddRat((UWORD *)lcpos,lclen,(UWORD *)rcpos,rclen,newcpos,&newnlen)) return(-1);
	  if( AC.ncmod != 0 ) {
		if( BigLong(newcpos,newnlen,(UWORD *)AC.cmod,ABS(AC.ncmod)) >=0 ){
		  WORD ii;
		  SubPLon(newcpos,newnlen,(UWORD *)AC.cmod,ABS(AC.ncmod),newcpos,&newnlen);
		  newcpos[newnlen] = 1;
		  for ( ii = 1; ii < newnlen; ii++ ) newcpos[newnlen+ii] = 0;
		}
	  }
	  if(newnlen == 0){
		/* terms cancel, get loser of left subtree and then of right subtree */
cancelled:
		PF_loser = n->lloser;
		PF_newclen[n->lloser] = 0;
		if(n->left) n->lloser = PF_GetLoser(n->left);
		else{ 
		  n->lloser = n->lsrc;
		  if( *(PF_term[n->lsrc] = PF_PutIn(n->lsrc)) == 0) n->lloser = 0;
		}
		PF_loser = n->rloser;
		PF_newclen[n->rloser] = 0;
		goto newright;
	  }
	  else{ /* keep the left term and get the loser of right subtree */
		newnlen <<= 1;
		newclen = ( newnlen > 0 ) ? ( newnlen + 1 ) : ( newnlen - 1 );
		if ( newnlen < 0 ) newnlen = -newnlen;
		PF_newclen[n->lloser] = newclen;
		lcpos = PF_newcpos[n->lloser];
		if(newclen < 0) newclen = -newclen;
		while(newclen--) *lcpos++ = *newcpos++;
		PF_loser = n->rloser;
		PF_newclen[n->rloser] = 0;	  
		goto newright;
	  }
	  /*
		#] terms are equal
	  */
	}
  }
  if(n->lloser > 0) return(n->lloser);
  if(n->rloser > 0) return(n->rloser);
  return(0);
}
/*
     #] PF_GetLoser(NODE)
	 #[ int        PF_EndSort()

  if this is not the masterprocess, just initialize the sendbuffers and 
  return 0, else PF_EndSort sends the rest of the terms in the sendbuffer 
  to the next slave and a dummy message to all slaves with tag 
  PF_ENDSORT_MSGTAG. Then it receives the sorted terms, sorts them using a 
  recursive 'tree of losers' (PF_GetLoser) and writes them to the 
  outputfile. 

*/ 
int 
PF_EndSort ARG0
{
  FILEHANDLE *fout = AR.outfile;
  PF_BUFFER *sbuf=PF.sbuf;
  SORTING *S = AR.SS;
  WORD *workspace,*outterm,*pp;
  LONG size;
  POSITION position;
  WORD i,j,cc,next;
  WORD newcoeff;
  int tag;

  if( AR.SS != AM.S0 || AC.mparallelflag == NOPARALLELFLAG) return(0);

  if( PF.me != MASTER){
	/* 
	   #[ the slaves have to initialize their sendbuffer

	   this is a slave and it's PObuffer should be the minimum of the 
	   sortiosize on the master and the POsize of our file.
	   First save the original PObuffer and POstop of the outfile

	*/
	size = (AR.SS->sTop2 - AR.SS->lBuffer - 1)/(PF.numtasks - 1);
	size -= (AM.MaxTer + 2); 
	if(fout->POsize < size*sizeof(WORD)) size = fout->POsize/sizeof(WORD);
	if(!sbuf){
	  if(!(sbuf = PF_AllocBuf(PF.numsbufs,size*sizeof(WORD),1))) return(-1);

	  sbuf->buff[0] = fout->PObuffer;
	  sbuf->stop[0] = fout->PObuffer+size;  
	  if( sbuf->stop[0] > fout->POstop ) return(-1);
	  sbuf->active = 0;
	}
	for(i=0;i<PF.numsbufs;i++)
	  sbuf->fill[i] = sbuf->full[i] = sbuf->buff[i];

	PF.sbuf = sbuf;
	fout->PObuffer = sbuf->buff[sbuf->active];
	fout->POstop = sbuf->stop[sbuf->active];
	fout->POsize = size*sizeof(WORD);
	fout->POfill = fout->POfull = fout->PObuffer;
	/*
	  #] slaves have to initialize their sendbuffer 
    */
	return(0);
  }
  /* this waits for all slaves to be ready to send terms back */
  PF_Wait4Slave(1);
  /* 
	 Now collect the terms of all slaves and merge them.
	 PF_GetLoser gives the position of the smallest term, which is the real 
	 work. The smallest term needs to be copied to the outbuf: use PutOut.
  */
  PF_InitTree();
  S->PolyFlag = AC.PolyFun ? 1: 0;
  *AR.CompressPointer = 0;
  PUTZERO(position);
  PF_outterms = 0;


  while(PF_loser >= 0){
	if(!(PF_loser = PF_GetLoser(PF_root))) break;
	outterm = PF_term[PF_loser];
	PF_outterms++;
	if(PF_newclen[PF_loser]!=0){
	  /*		  
		#[ this is only when new coeff was too long
	  */
	  outterm = PF_WorkSpace;
	  pp = PF_term[PF_loser];
	  cc = *pp;
	  while(cc--) *outterm++ = *pp++;
	  outterm = (outterm[-1] > 0) ? outterm-outterm[-1] : outterm+outterm[-1];
	  if(PF_newclen[PF_loser] > 0) cc =  (WORD)PF_newclen[PF_loser] - 1;
	  else                          cc = -(WORD)PF_newclen[PF_loser] - 1;
	  pp =  PF_newcpos[PF_loser];
	  while(cc--) *outterm++ = *pp++;
	  *outterm++ = PF_newclen[PF_loser];
	  *PF_WorkSpace = outterm - PF_WorkSpace;
	  outterm = PF_WorkSpace;
	  *PF_newcpos[PF_loser] = 0;
	  PF_newclen[PF_loser] = 0;

	  /*
		#] this is only when new coeff was too long 
	  */
	}
	PRINTFBUF("PF_EndSort to PutOut: ",outterm,*outterm);  
	PutOut(outterm,&position,fout,1);
  }		
  if( FlushOut(&position,fout) ) return(-1);
  return(1);
}
/*
     #] int PF_EndSort(WORD *,int,int,int)
  #] sort.c
  #[ proces.c
     #[ variables
  */

static  WORD *PF_CurrentBracket;      

/*
     #] sort variables
     #[ WORD       PF_GetTerm(WORD*)

  This replaces GetTerm on the slaves, which get their terms from the master, 
  not the infile anymore, is nonblocking and buffered ...
  use AR.infile->PObuffer as buffer. For the moment, don't care 
  about compression, since terms come uncompressed from master. 

  To enable keep-brackets when AR.DeferFlag isset, we need to do some
  preparation here:

    1: copy the part ouside brackets to current_bracket
	2: skip term if part outside brackets is same as for last term
	3: if POfill >= POfull receive new terms as usual

  different from GetTerm we use an extra buffer for the part outside brackets:
    PF_CurrentBracket

*/
WORD
PF_GetTerm ARG1(WORD *,term)
{
  FILEHANDLE *fi = AR.infile;
  WORD i,j;
  WORD *next,*np,*last,*lp=0,*nextstop,*tp=term;

  deferskipped = 0;
  if( fi->POfill >= fi->POfull || fi->POfull == fi->PObuffer ){
ReceiveNew:
	{
	  /*
		#[ receive new terms from master
	  */
	  int src = MASTER,tag;
	  int follow = 0;
	  LONG size,cpu,space = 0;
	  
	  if(PF.log){
		fprintf(stderr,"[%d] Starting to send to Master\n",PF.me);
		fflush(stderr);
	  }

	  PF_Send(MASTER,PF_READY_MSGTAG,0);
	  cpu = TimeCPU(1);
	  PF_Pack(&cpu               ,1,PF_LONG);         
	  PF_Pack(&space             ,1,PF_LONG);          
	  PF_Pack(&PF_linterms       ,1,PF_LONG);   
	  PF_Pack(&(AM.S0->GenTerms) ,1,PF_LONG);
	  PF_Pack(&(AM.S0->TermsLeft),1,PF_LONG);
	  PF_Pack(&follow            ,1,PF_INT );

	  if(PF.log){
		fprintf(stderr,"[%d] Now sending with tag = %d\n",PF.me,PF_READY_MSGTAG);
		fflush(stderr);
	  }

	  PF_Send(MASTER,PF_READY_MSGTAG,1);
	  
	  if(PF.log){
		fprintf(stderr,"[%d] returning from send\n",PF.me);
		fflush(stderr);
	  }

	  /* size = fi->POstop - fi->PObuffer; 1 less for the last 0 if ENDSORTMSGTAG */
	  size = fi->POstop - fi->PObuffer - 1;


         PF_Receive(MASTER,PF_ANY_MSGTAG,&src,&tag);
	 
#ifdef MPI2          
	        if (tag == PF_TERM_MSGTAG) {
		   PF_UnPack(&size, 1, PF_LONG);
		  if(!PF_Put_target(src)){
		   printf("PF_Put_target error ...\n");
		  }
		} else {
		  PF_RecvWbuf(fi->PObuffer,&size,&src);
		}  
#else
                PF_RecvWbuf(fi->PObuffer,&size,&src);
#endif



	  fi->POfill = fi->PObuffer;
	  /* get PF.ginterms which sits in the first 2 WORDS */
	  PF.ginterms = (LONG)(fi->POfill[0])*(LONG)WORDMASK + (LONG)(fi->POfill[1]);
	  fi->POfill += 2;
	  fi->POfull = fi->PObuffer + size;
	  if(tag == PF_ENDSORT_MSGTAG) *fi->POfull++ = 0;
	  /*
		#] receive new terms from master
	  */
	}
	if( PF_CurrentBracket ) *PF_CurrentBracket = 0;
  }
  if( *fi->POfill == 0 ){
	fi->POfill = fi->POfull = fi->PObuffer;
	*term = 0;
	goto RegRet;
  }
  if ( AR.DeferFlag ) {
	if ( !PF_CurrentBracket ){
	  /*
		#[ alloc space 
	  */
	  PF_CurrentBracket = 
		(WORD*)Malloc1(sizeof(WORD)*AM.MaxTer,"PF_CurrentBracket");
	  *PF_CurrentBracket = 0;
	  /*
		#] alloc space 
	  */
	}
	while ( *PF_CurrentBracket ){ /* "for each term in the buffer" */
	  /*
		#[ test bracket & skip if it's equal to the last in PF_CurrentBracket
	  */
	  next = fi->POfill;
	  nextstop = next + *next; nextstop -= ABS(nextstop[-1]);
	  next++;
	  last = PF_CurrentBracket+1;
	  while ( next < nextstop ) {
		/* scan the next term and PF_CurrentBracket */
		if ( *last == HAAKJE && *next == HAAKJE ){
		  /* the part outside brackets is equal => skip this term */
		  PRINTFBUF("PF_GetTerm skips",fi->POfill,*fi->POfill);
		  break;
		} 
		/* check if the current subterms are equal */
		np = next; next += next[1];
		lp = last; last += last[1];
		while ( np < next ) if( *lp++ != *np++ ) goto strip;
	  }
	  /* go on to next term */
	  fi->POfill += *fi->POfill;
	  deferskipped++;
	  /* the usual checks */
	  if( fi->POfill >= fi->POfull || fi->POfull == fi->PObuffer ) 
		goto ReceiveNew;
	  if( *fi->POfill == 0 ){
		fi->POfill = fi->POfull = fi->PObuffer;
		*term = 0;
		goto RegRet;
	  }
	  /*
		#] test bracket & skip if it's equal to the last in PF_CurrentBracket
	  */
	}
	/*
	  #[ copy this term to CurrentBracket and the part outside of bracket 
	     to WorkSpace at term
	*/
strip:
	next = fi->POfill;
	nextstop = next + *next; nextstop -= ABS(nextstop[-1]);
	next++;
	tp++;
	lp = PF_CurrentBracket + 1;
	while ( next < nextstop ) {
	  if( *next == HAAKJE ){
		fi->POfill += *fi->POfill;
		while( next < fi->POfill ) *lp++ = *next++;
		*PF_CurrentBracket = lp - PF_CurrentBracket;
		*lp = 0;
		*tp++ = 1;
		*tp++ = 1;
		*tp++ = 3;
		*term = WORDDIF(tp,term);
		PRINTFBUF("PF_GetTerm new brack",PF_CurrentBracket,*PF_CurrentBracket);
		PRINTFBUF("PF_GetTerm POfill",fi->POfill,*fi->POfill);
		goto RegRet;
	  }
	  np = next; next += next[1];
	  while ( np < next ) *tp++ = *lp++ = *np++;
	}
	tp = term;
	/*
	  #] 
	*/
  }

  i = *fi->POfill;
  while(i--) *tp++ = *fi->POfill++;
RegRet:
  PRINTFBUF("PF_GetTerm returns",term,*term);
  return(*term);
}
/*							  
     #] int        PF_GetTerm
	 #[ WORD       PF_Deferred(WORD*,WORD) :
	 
	 Picks up the deferred brackets.
*/

WORD
PF_Deferred ARG2(WORD *,term,WORD,level)
{
  WORD *bra,*bp,*bstop;
  WORD *tstart,*tstop;
  WORD *next = AR.infile->POfill;
  WORD *termout = AR.WorkPointer;
  WORD *oldwork = AR.WorkPointer;

  AR.WorkPointer += AM.MaxTer;  
  AR.DeferFlag = 0;
  
  PRINTFBUF("PF_Deferred (Term)   ",term,*term);
  PRINTFBUF("PF_Deferred (Bracket)",PF_CurrentBracket,*PF_CurrentBracket);

  bra = bstop = PF_CurrentBracket;
  bstop += *bstop;
  bstop -= ABS(bstop[-1]);
  bra++;
  while ( *bra != HAAKJE && bra < bstop ) bra += bra[1];
  if ( bra >= bstop ) {	/* No deferred action! */
	AR.WorkPointer = term + *term;
	if ( Generator(term,level) ) goto DefCall;
	AR.DeferFlag = 1;
	AR.WorkPointer = oldwork;
	return(0);
  }
  bstop = bra;
  tstart = bra + bra[1];
  bra = PF_CurrentBracket;
  tstart--;
  *tstart = bra + *bra - tstart;
  bra++;
  /*
	Status of affairs:
	First bracket content starts at tstart.
	Next term starts at next.
	The outside of the bracket runs from bra = PF_CurrentBracket to bstop.
  */
  for(;;) {
  	if ( InsertTerm(term,0,AM.rbufnum,tstart,termout,0) < 0 ) {
  	  goto DefCall;
  	}
	/* call Generator with new composed term */
  	AR.WorkPointer = termout + *termout;
  	if ( Generator(termout,level) ) goto DefCall;
  	AR.WorkPointer = termout;
	tstart = next + 1;
	if( tstart >= AR.infile->POfull ) goto ThatsIt;
	next += *next;
	/* compare with current bracket */
  	while ( bra <= bstop ) {
  	  if ( *bra != *tstart ) goto ThatsIt;
  	  bra++; tstart++;
  	}
	/* now bra and tstart should both be a HAAKJE */
	bra--;tstart--;
	if ( *bra != HAAKJE || *tstart != HAAKJE ) goto ThatsIt;
	tstart += tstart[1];
	tstart--;
	*tstart = next - tstart;
	bra = PF_CurrentBracket + 1;
  }

 ThatsIt:
  /* AR.WorkPointer = oldwork; */
  AR.DeferFlag = 1;
  return(0);

 DefCall:
  MesCall("PF_Deferred");
  SETERROR(-1);
 }

/*
 	 #] Deferred : 
     #[ int        PF_Wait4Slave(int)

  Waiting for the next slave to accept terms, it returns the number of that 
  slave. 
  for par = 0 it receives a message, provides services until it receives a
  ready msgtag. It then takes actions in tail of the message and returns 
  the number of sender.
  for par = 1 it sends a ENDSORT_MSGTAG to all slaves and waits until all 
  slaves are ready to send terms back.

  It provides all the services that slaves can ask for during generating terms.

*/
static LONG **PF_W4Sstats=0;

int PF_Wait4Slave ARG1(int,par)
{
  int i,j,tag,next=PF_ANY_SOURCE,src=PF_ANY_SOURCE;
  UBYTE *has_sent=0;

  if(par == 1){
	has_sent = (UBYTE*)Malloc1(sizeof(UBYTE)*PF.numtasks,"PF_Wait4Slave");
	for(i=0;i<PF.numtasks;i++) has_sent[i] = 0;	
  }
recv:  
  if(par == 1){
	/* 
	   #[ only receive from slaves that are still working 
	*/
	tag = 0;
	  while(!tag){
		if( next != PF_ANY_SOURCE ){
		  next++;
		  while( has_sent[next] ) next++;
		  if( next >= PF.numtasks ){
			next = 1;
			while(has_sent[next]) next++;
		  }
		}
		tag = PF_Probe(next);
		if(tag == PF_BUFFER_MSGTAG || tag == PF_ENDBUFFER_MSGTAG){
		  has_sent[next] = 1;
		  j = 0;
		  for(i=1;i<PF.numtasks;i++) j += has_sent[i];  
		  if(j < PF.numtasks-1) goto recv;
		  else{
			for(i=0;i<PF.numtasks;i++) has_sent[i] = 0;	
			if(has_sent) M_free(has_sent,"PF_Wait4Slave");
			return(0);
		  }
		  break;
		}
		src = next;
	  }
	  
	/* 
	   #] only receive from slaves that are still working 
	*/
  }
  PF_Receive(src,PF_ANY_MSGTAG,&next,&tag);

  if(tag != PF_READY_MSGTAG){
	MesPrint("[%d] PF_Wait4Slave: received MSGTAG %d",(WORD)PF.me,(WORD)tag);
	return(-1);
  }
  if(!PF_W4Sstats){
	PF_W4Sstats = (LONG**)Malloc1(sizeof(LONG*),"");
	PF_W4Sstats[0] = (LONG*)Malloc1(PF_STATS_SIZE*sizeof(LONG),"");
  }
  PF_UnPack(PF_W4Sstats[0],PF_STATS_SIZE,PF_LONG);
  PF_Statistics(PF_W4Sstats,next);

  PF_UnPack(&j,1,PF_INT);
  
  if(j){
	/* actions depending on rest of information in last message */
  }
  
  if(par == 0) {
	return(next);
  }
  
  if(par == 1){
	if(!has_sent[0]){
	  PF.sbuf->active = 0;
          PF_Send(next,PF_ENDSORT_MSGTAG,0);	  
	  PF_Send(next,PF_ENDSORT_MSGTAG,1);
	  PF_ISendSbuf(next,PF_ENDSORT_MSGTAG);
	  has_sent[0] = 1;
	}
	else{
	  /* copy PF.ginterms into each buffer */
	  *(PF.sbuf->fill[next])++ = (UWORD)((PF.ginterms+1)/(LONG)WORDMASK);
	  *(PF.sbuf->fill[next])++ = (UWORD)((PF.ginterms+1)%(LONG)WORDMASK);
	  *(PF.sbuf->fill[next])++ = 0;
	  PF.sbuf->active = next;
	  PF_Send(next,PF_ENDSORT_MSGTAG,0);
	  PF_Send(next,PF_ENDSORT_MSGTAG,1);
	  PF_ISendSbuf(next,PF_ENDSORT_MSGTAG);
	}
  }
  /* wait for the rest to finish */ 
  goto recv;
}
/*
     #] int        PF_Wait4Slave
	 #[ int        PF_Processor(EXPRESSION,WORD)

  replaces parts of Processor on the masters and slaves.
  On the master PF_Processor is responsible for proper distribution of terms 
  from the input file to the slaves.
  On the slaves it calls Generator for all the terms that this process gets,
  but PF_GetTerm gets terms from the master ( not directly from infile).

*/
int
PF_Processor ARG3( EXPRESSIONS,e,WORD,i,WORD,LastExpression)
{			
  WORD *term = AR.WorkPointer;
  LONG dd = 0,ll;
  PF_BUFFER *sb=PF.sbuf;
  WORD j,*t,*s,next;
  LONG termsinpatch;
  LONG size,cpu;
  POSITION position;
  int k,src,tag,attach,code;

#ifdef MPI2  
  if (PF_shared_buff == NULL) {
    if (!PF_SMWin_Init()) {
     MesPrint("PF_SMWin_Init error");
     exit(-1);
    } 
  } 
#endif


  if ( ( AR.WorkPointer + AM.MaxTer ) > AM.WorkTop ) return(MesWork());

  /* allocate and/or reset the variables used for the redefine */
  if ( !PF.redef || NumPre > (LONG)PF.numredefs ){
	if(PF.redef) M_free(PF.redef,"resize PF.redef");
	PF.numredefs = (LONG)NumPre;
	PF.redef = (LONG*)Malloc1(PF.numredefs*sizeof(LONG),"PF.redef");
  }
  for(ll = 0;ll < PF.numredefs; ll++) PF.redef[ll] = 0;

  if(PF.me == MASTER){
	/* 
	   #[ Master
	     #[ write prototype to outfile
	*/
	if ( PF.log && PF.module >= PF.log )
	  MesPrint("[%d] working on expression %s in module %l",PF.me,EXPRNAME(i),PF.module);
	if ( GetTerm(term) <= 0 ) {
	  MesPrint("[%d] Expression %d has problems in scratchfile",PF.me,i);
	  return(-1);
	}
	if ( AC.bracketindexflag ) OpenBracketIndex(i);
	term[3] = i;
	AS.CurExpr = i;
	SeekScratch(AR.outfile,&position);
	e->onfile = position;
	if ( PutOut(term,&position,AR.outfile,0) < 0 ) return(-1);
	/*
	     #] write prototype to outfile
	     #[ initialize sendbuffer if necessary

		 the size of the sendbufs is:
		 MIN(1/PF.numtasks*(AR.SS->sBufsize+AR.SS->lBufsize),AR.infile->POsize)
		 No allocation for extra buffers necessary, just make sb->buf... point 
		 to the right places in the sortbuffers.
	*/
	NewSort(); /* we need AR.SS to be set for this!!! */
	if(!sb || sb->buff[0] != AR.SS->lBuffer){
	  size = (LONG)((AR.SS->sTop2 - AR.SS->lBuffer)/(PF.numtasks));
	  if(size > (AR.infile->POsize/sizeof(WORD) - 1) )
		size = AR.infile->POsize/sizeof(WORD) - 1;
	  if(!sb) 
		if(!(sb = PF_AllocBuf(PF.numtasks,size*sizeof(WORD),PF.numtasks))) 
		          return(-1);
	  sb->buff[0] = AR.SS->lBuffer;
	  sb->full[0] = sb->fill[0] = sb->buff[0];
	  for(j=1;j<PF.numtasks;j++)
		sb->stop[j-1] = sb->buff[j] = sb->buff[j-1] + size;
	  sb->stop[PF.numtasks-1] = sb->buff[PF.numtasks-1] + size;
	  PF.sbuf = sb;
	}
	for(j=0;j<PF.numtasks;j++) sb->full[j] = sb->fill[j] = sb->buff[j];
       
	/*
	     #] initialize sendbuffers
		 #[ loop for all terms in infile:

		 copy them always to sb->buff[0], when that is full, wait for 
		 next slave to accept terms, exchange sb->buff[0] and 
		 sb->buff[next], send sb->buff[next] to next slave and go on 
		 filling the now empty sb->buff[0].
	*/
    AR.DeferFlag = AC.ComDefer;
	/* The master schould leave the brackets in any case!!! */
	if(AC.mparallelflag == PARALLELFLAG && PF.me == MASTER) AR.DeferFlag = 0;
    PF.ginterms = 0;
    termsinpatch = 0;
    *(sb->fill[0])++ = (UWORD)(0);
	*(sb->fill[0])++ = (UWORD)(1);
	while ( GetTerm(term) ) {
	  PF.ginterms++; dd = deferskipped;
	  if ( AC.CollectFun && *term <= (AM.MaxTer>>1) ) {
		if ( GetMoreTerms(term) ) {
		  LowerSortLevel(); return(-1);
		}
	  }
	  if(AC.mparallelflag == PARALLELFLAG){
		PRINTFBUF("PF_Processor gets",term,*term);
		if(termsinpatch >= PF_maxinterms || sb->fill[0] + *term >= sb->stop[0]){
		  next = PF_Wait4Slave(0);
		  sb->fill[next] = sb->fill[0]; sb->full[next] = sb->full[0];
		  s = sb->stop[next]; sb->stop[next] = sb->stop[0]; sb->stop[0] = s;
		  s = sb->buff[next]; sb->buff[next] = sb->buff[0]; 
		  sb->fill[0] = sb->full[0] = sb->buff[0] = s;

		  sb->active = next;
		  

                  size = sb->fill[next] - sb->buff[next];
		  PF_Send(next,PF_TERM_MSGTAG,0);
                   PF_Pack(&size, 1, PF_LONG);
		  PF_Send(next,PF_TERM_MSGTAG,1);


#ifdef MPI2
		  if (!PF_Put_origin(next)){
		    printf("PF_Put_origin error...\n");
		  }
#else
                  PF_ISendSbuf(next,PF_TERM_MSGTAG);
#endif

		  *(sb->fill[0])++ = (UWORD)(PF.ginterms/(LONG)WORDMASK);
		  *(sb->fill[0])++ = (UWORD)(PF.ginterms%(LONG)WORDMASK);
		  termsinpatch = 0;
		}
		j = *(s = term);
		while(j--) *(sb->fill[0])++ = *s++;
		termsinpatch++;
	  }
	  else { /* not parallel */
		AR.WorkPointer = term + *term;
		AR.RepPoint = AM.RepCount + 1;
		AR.CurDum = ReNumber(term);
		if ( AC.SymChangeFlag ) MarkDirty(term,DIRTYSYMFLAG);
		if ( Generator(term,0) ) {
		  LowerSortLevel(); return(-1);
		}
		PF.ginterms += dd;	
	  }
	}
	PF.ginterms += dd;
	/*
		 #] loop for all terms in infile
		 #[ Clean up & EndSort
	*/
	
	if ( LastExpression ) {
	  if ( AR.infile->handle >= 0 ) {
		CloseFile(AR.infile->handle);
		AR.infile->handle = -1;
		remove(AR.infile->name);
		PUTZERO(AR.infile->POposition);
		AR.infile->POfill = AR.infile->POfull = AR.infile->PObuffer;
	  }
	}
	
	if ( EndSort(AM.S0->sBuffer,0) < 0 ) return(-1);
	if ( AM.S0->TermsLeft ) e->vflags &= ~ISZERO;
	else e->vflags |= ISZERO;
	if ( AR.expchanged == 0 ) e->vflags |= ISUNMODIFIED;
	
	if ( AM.S0->TermsLeft ) AR.expflags |= ISZERO;
	if ( AR.expchanged ) AR.expflags |= ISUNMODIFIED;
	AR.GetFile = 0;	
	/* 
	     #] Clean up and call EndSort
	     #[ Collect (stats,prepro,...)
	*/
	if(AC.mparallelflag == PARALLELFLAG){
	  for(k=1;k<PF.numtasks;k++){
		PF_Receive(PF_ANY_SOURCE,PF_ENDSORT_MSGTAG,&src,&tag);
		PF_UnPack(PF_stats[src],PF_STATS_SIZE,PF_LONG);
		PF_UnPack(&attach,1,PF_INT);
		if(attach){
		  /* actions depending on rest of information in last message */
		  switch(attach){
		  case PF_ATTACH_REDEF:
			{
			  int ll,kk,ii;
			  UBYTE *value,*name;
			  LONG redef;

			  PF_UnPack(&kk,1,PF_INT);
			  while ( --kk >= 0) {
				PF_UnPack(&ll,1,PF_INT);
				name = (UBYTE*)Malloc1(ll,"redef name");
				PF_UnPack(name,ll,PF_BYTE);
				PF_UnPack(&ll,1,PF_INT);
				value = (UBYTE*)Malloc1(ll,"redef value");
				PF_UnPack(value,ll,PF_BYTE);
				PF_UnPack(&redef,1,PF_LONG);
				if( redef > PF.redef[kk]){
				  for(ii=NumPre-1;ii>=0;ii--) 
					if(StrCmp(name,PreVar[ii].name)== 0) break;
				  PF.redef[ii] = redef;
				  PutPreVar(name,value,0,1); // I reduced the possibility to transfer prepro variables with args for the moment
				}
			  }
			  /* here we should free the allocated memory of value & name ??*/
			}
			break;
		  default:
			/* here should go an error message */
			break;
		  }
		  /* end of switch */
		}
	  }
	  PF_Statistics(PF_stats,0);
	}
	/*
	     #] Collect (stats,prepro,...)
		 #[ BroadCast PreProcessor variables that have changed
	*/
	if(AC.mparallelflag == PARALLELFLAG){
	  int l=0;
	  UBYTE *value,*name,*p;

	  PF_BroadCast(0);
	  k = NumPre;
	  /* count the preprocessor variables that have changed in this module 
	   and for this expression and pack this number into sendbuffer */
	  while ( --k >= 0) if (PF.redef[k]) l++;
	  k = NumPre;
	  PF_Pack(&l,1,PF_INT);
      /* now pack for each of the changed preprovariables the length of the 
         name, the name the length of the value and the value into the 
         sendbuffer */
	  while ( --k >= 0) {
		if(PF.redef[k]){
		  l = 1;
		  p = name = PreVar[k].name;
		  while(*p++) l++;
		  PF_Pack(&l,1,PF_INT);
		  PF_Pack(name,l,PF_BYTE);
		  l = 1;
		  p = value = GetPreVar(name,WITHERROR);
		  while(*p++) l++;
		  PF_Pack(&l,1,PF_INT);
		  PF_Pack(value,l,PF_BYTE);
		}
	  }
	  /* broadcast the sendbuffer */
	  PF_BroadCast(1);
	}
	/*
		 #] BroadCast 
	   #] Master
	*/
  }
  else{ 
	if ( AC.mparallelflag == NOPARALLELFLAG ) return(0);
	/* 
	   #[ Slave 
		 #[ Generator Loop & EndSort

	   loop for all terms to get from master, call Generator for each of them
	   then call EndSort and do cleanup (to be implemented)
	*/				  
	SeekScratch(AR.outfile,&position);
	e->onfile = position;
 	AR.DeferFlag = AC.ComDefer;
	NewSort();
	PF_linterms = 0;
	PF.parallel = 1;
#ifdef MPI2	
	AR.infile->POfull = AR.infile->POfill = AR.infile->PObuffer = PF_shared_buff;
#else
        AR.infile->POfull = AR.infile->POfill = AR.infile->PObuffer;
#endif	

	while ( PF_GetTerm(term) ) {
	  PF_linterms++; dd = deferskipped;
	  AR.WorkPointer = term + *term;
	  AR.RepPoint = AM.RepCount + 1;
	  	  AR.CurDum = ReNumber(term);
	  if ( AC.SymChangeFlag ) MarkDirty(term,DIRTYSYMFLAG);
	  if ( Generator(term,0) ) {
		MesPrint("[%d] PF_Processor: Error in Generator",PF.me);
		LowerSortLevel(); return(-1);
	  }
	  PF_linterms += dd;
	}
	PF_linterms += dd;
	if ( EndSort(AM.S0->sBuffer,0) < 0 ) return(-1);
	/*
		 #] Generator Loop & EndSort
		 #[ Collect (stats,prepro...)
	*/
	PF_Send(MASTER,PF_ENDSORT_MSGTAG,0);
	cpu = TimeCPU(1);
	size = 0;
	PF_Pack(&cpu               ,1,PF_LONG);         
	PF_Pack(&size              ,1,PF_LONG);          
	PF_Pack(&PF_linterms       ,1,PF_LONG);   
	PF_Pack(&(AM.S0->GenTerms) ,1,PF_LONG);
	PF_Pack(&(AM.S0->TermsLeft),1,PF_LONG);
	/* now handle the redefined Preprovars */
	k = attach = 0;
	for(ll = 0; ll < PF.numredefs;ll++) if(PF.redef[ll]) k++;

	if(k) attach = PF_ATTACH_REDEF;
	PF_Pack(&attach,1,PF_INT);
	if(k){
	  int l;
	  UBYTE *value,*name,*p;
	  
	  PF_Pack(&k,1,PF_INT);
	  k = NumPre;
	  while ( --k >= 0) {
		if(PF.redef[k]){
		  l = 1;
		  p = name = PreVar[k].name;
		  while(*p++) l++;
		  PF_Pack(&l,1,PF_INT);
		  PF_Pack(name,l,PF_BYTE);
		  l = 1;
		  p = value = GetPreVar(name,WITHERROR);
		  while(*p++) l++;
		  PF_Pack(&l,1,PF_INT);
		  PF_Pack(value,l,PF_BYTE);
		  PF_Pack(&(PF.redef[k]),1,PF_LONG);
		}
	  }
	}
	PF_Send(MASTER,PF_ENDSORT_MSGTAG,1);
	/* 
		 #] Collect (stats,prepro,...)
		 #[ BroadCast (only necessary when slaves run PreProcessor)	
	*/
	{
	  int k;
	  LONG l,nl=0,vl=0;
	  UBYTE *name=0,*val=0;
	  
	  PF_BroadCast(1);
	  PF_UnPack(&k,1,PF_INT);
	  while ( --k >= 0) {
		PF_UnPack(&l,1,PF_INT);
		if(l > nl){ 
		  if(name) M_free(name,"PreVar name");
		  name =  (UBYTE*)Malloc1((int)l,"PreVar name"); 
		  nl = l; 
		}
		PF_UnPack(name,l,PF_BYTE);
		PF_UnPack(&l,1,PF_INT);
		if(l > vl){ 		
		  if(val) M_free(val,"PreVar val");
		  val = (UBYTE*)Malloc1((int)l,"PreVar value"); 
		  vl = l; 
		}
		PF_UnPack(val,l,PF_BYTE);
		if(PF.log) 
		  printf("[%d] module %d: PutPreVar(\"%s\",\"%s\",1);\n",
				 PF.me,PF.module,name,val);
		PutPreVar(name,val,0,1); // I reduced the possibility to transfer prepro variables with args for the moment
	  }
	  if(name) M_free(name,"PreVar name");
	  if(val) M_free(val,"PreVar value");
	}
	/*
		 #] BroadCast 
	   #] Slave 
	*/				  
    if(PF.log){
	  fprintf(stderr,"[%d|%d] Endsort,Collect,Broadcast done\n",PF.me,PF.module);
	  fflush(stderr);
	}
  }
  return(0);
}
/*
     #] int        PF_Processor()
  #] proces.c
  #[ startup
     #[ int        PF_Init(int*,char***)

	 PF_LibInit should do all library dependent initializations.
	 Then PF_Init should do all the library independent stuff.
*/
int PF_Init ARG2(int*,argc,char ***,argv){
  UBYTE *fp,*ubp;
  char *c;
  int fpsize=0,s;

  /* this should definitly be somewhere else ... */
  PF_CurrentBracket = 0;

  PF.numtasks = 0; /* number of tasks, is determined in PF_Lib_Init or must be set before! */
  PF.numsbufs = 2; /* might be changed by LibInit ! */
  PF.numrbufs = 2; /* might be changed by LibInit ! */

  PF_LibInit(argc,argv);
  PF_RealTime(PF_RESET);

  PF_maxinterms = 10;
  PF.log = 0;
  PF.parallel = 0;
  PF.module = 0;
  PF_statsinterval = 10;

  if(PF.me == MASTER){

#ifdef PF_WITHGETENV
	/* get these from the environment at the moment sould be in setfile/tail */
	if( c =  getenv("PF_LOG") ){
	  if (*c) PF.log = (int)atoi(c);
	  else PF.log = 1;
	  fprintf(stderr,"[%d] changing PF.log to %d\n",PF.me,PF.log);
	  fflush(stderr);
	}
	if( ( c = (char*)getenv("PF_RBUFS") ) ){
	  PF.numrbufs = (int)atoi(c);
	  fprintf(stderr,"[%d] changing numrbufs to: %d\n",PF.me,PF.numrbufs);
	  fflush(stderr);
	}
	if( ( c = (char*)getenv("PF_SBUFS") ) ){
	  PF.numsbufs = (int)atoi(c);
	  fprintf(stderr,"[%d] changing numsbufs to: %d\n",PF.me,PF.numsbufs);
	  fflush(stderr);
	}
	if(PF.numsbufs > 10 ) PF.numsbufs = 10;
	if(PF.numsbufs < 1 ) PF.numsbufs = 1;
	if(PF.numrbufs > 2 ) PF.numrbufs = 2;
	if(PF.numrbufs < 1 ) PF.numrbufs = 1;

	if( (c = getenv("PF_MAXINTERMS")) ){
	  PF_maxinterms = (LONG)atoi(c);
	  fprintf(stderr,"[%d] changing PF_maxinterms to %d\n",PF.me,PF_maxinterms);
	  fflush(stderr);
	}
	if( (c = getenv("PF_STATS")) ){
	  PF_statsinterval = (int)atoi(c);
	  fprintf(stderr,"[%d] changing PF_statsinterval to %d\n",PF.me,PF_statsinterval);
	  fflush(stderr);
	  if(PF_statsinterval < 1) PF_statsinterval = 10;
	}
	fp = (UBYTE*)getenv("FORMPATH");
	if(fp){
	  ubp = fp;
	  while ( *ubp++ ) fpsize++;
	  fprintf(stderr,"[%d] changing Path to %s\n",PF.me,fp);
	  fflush(stderr);
	}
	else{
	  fp = (UBYTE*)"";
	  fpsize++;
	}
	fpsize++;
#endif
  }
  /*
    #[ BroadCast settings from getenv: could also be done in PF_DoSetup 
  */
  if(PF.me == MASTER){
	PF_BroadCast(0);
	PF_Pack(&PF.log,1,PF_INT);
	PF_Pack(&PF.numrbufs,1,PF_WORD);
	PF_Pack(&PF.numsbufs,1,PF_WORD);
	PF_Pack(&PF_maxinterms,1,PF_LONG);
	PF_Pack(&fpsize,1,PF_INT);
	PF_Pack(fp,(LONG)fpsize,PF_BYTE);
  }
  PF_BroadCast(1);
  if(PF.me != MASTER){
	PF_UnPack(&PF.log,1,PF_INT);
	PF_UnPack(&PF.numrbufs,1,PF_WORD);
	PF_UnPack(&PF.numsbufs,1,PF_WORD);
	PF_UnPack(&PF_maxinterms,1,PF_LONG);
	PF_UnPack(&fpsize,1,PF_INT);
	AM.Path = (UBYTE*)Malloc1(fpsize*sizeof(UBYTE),"Path");
	PF_UnPack(AM.Path,(LONG)fpsize,PF_BYTE);
	if(PF.log) {
	  fprintf(stderr,"[%d] log=%d rbufs=%d sbufs=%d maxin=%d path=%s\n",
			  PF.me,PF.log,PF.numrbufs,PF.numsbufs,PF_maxinterms,AM.Path);
	  fflush(stderr);
	}
  }
  /*
    #] BroadCast settings from getenv
  */  
  
  return(0);
}
/*	 
     #] int         PF_Init(int*,char***)
  #] startup, prepro & compile
*/
