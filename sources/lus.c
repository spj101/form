/*
  	#[ Includes : function.c
*/

#include "form3.h"

/*
  	#] Includes :
  	#[ Lus :

	Routine to find loops.
	Mode: 0: Just tell whether there is such a loop.
	      1: Take out the functions and replace by outfun with the
	         remaining arguments of the loop function
	      > AM.OffsetIndex: This index must be included in the loop.
	      < -AM.OffsetIndex: This index must be included in the loop. Replace.
	Return value: 0: no loop. 1: there is/was such a loop.
	funnum: the function(s) in which we look for a loop.
	numargs: the number of arguments admissible in the function.
	outfun: the output function in case of a substitution.
	loopsize: the size of the loop we are looking for.
	          if < 0 we look for all loops.
*/

static int *funargs = 0;
static WORD **funlocs = 0;
static int numfargs = 0;
static int numflocs = 0;
static int nargs, tohunt, numoffuns;
static int *funinds = 0;
static int funisize = 0;

int Lus ARG6(WORD *,term,WORD,funnum,WORD,loopsize,WORD,numargs,WORD,outfun,WORD,mode)
{
	WORD *w, *t, *tt, *m, *r, **loc, *tstop, minloopsize;
	int nfun, i, j, jj, k, n, sign = 0, action = 0, L, ten, ten2, totnum,
	sign2, *alist, *wi, mini, maxi, medi = 0;
	if ( numargs <= 1 ) return(0);
	GETSTOP(term,tstop);
/*
	First count the number of functions with the proper number of arguments.
*/
	t = term+1; nfun = 0;
	if ( ( ten = functions[funnum-FUNCTION].spec ) >= TENSORFUNCTION ) {
		while ( t < tstop ) {
			if ( *t == funnum && t[1] == FUNHEAD+numargs ) { nfun++; }
			t += t[1];
		}
	}
	else {
		while ( t < tstop ) {
			if ( *t == funnum ) {
				i = 0; m = t+FUNHEAD; t += t[1];
				while ( m < t ) { i++; NEXTARG(m) }
				if ( i == numargs ) nfun++;
			}
			else t += t[1];
		}
	}
	if ( loopsize < 0 ) minloopsize = 2;
	else                minloopsize = loopsize;
	if ( funnum < minloopsize ) return(0); /* quick abort */
	if ( ((functions[funnum-FUNCTION].symmetric) & ~REVERSEORDER) == ANTISYMMETRIC ) sign = 1;
	if ( mode == 1 || mode < 0 ) {
		ten2 = functions[outfun-FUNCTION].spec >= TENSORFUNCTION;
	}
	else ten2 = -1;
/*
	Allocations:
*/
	if ( numflocs < funnum ) {
		if ( funlocs ) M_free(funlocs,"Lus: funlocs");
		numflocs = funnum;
		funlocs = (WORD **)Malloc1(sizeof(WORD *)*numflocs,"Lus: funlocs");
	}
	if ( numfargs < funnum*numargs ) {
		if ( funargs ) M_free(funargs,"Lus: funargs");
		numfargs = funnum*numargs;
		funargs = (int *)Malloc1(sizeof(int *)*numfargs,"Lus: funargs");
	}
/*
	Make a list of relevant indices
*/
	alist = funargs; loc = funlocs;
	t = term+1;
	if ( ten >= TENSORFUNCTION ) {
		while ( t < tstop ) {
			if ( *t == funnum && t[1] == FUNHEAD+numargs ) {
				*loc++ = t;
				t += FUNHEAD;
				j = i = numargs; while ( --i >= 0 ) {
					if ( *t >= AM.OffsetIndex && 
						( *t >= AM.OffsetIndex+WILDOFFSET ||
						indices[*t-AM.OffsetIndex].dimension != 0 ) ) {
						*alist++ = *t++; j--;
					}
					else t++;
				}
				while ( --j >= 0 ) *alist++ = -1;
			}
			else t += t[1];
		}
	}
	else {
		nfun = 0;
		while ( t < tstop ) {
			if ( *t == funnum ) {
				w = t;
				i = 0; m = t+FUNHEAD; t += t[1];
				while ( m < t ) { i++; NEXTARG(m) }
				if ( i == numargs ) {
					m = w + FUNHEAD;
					while ( m < t ) {
						if ( *m == -INDEX && m[1] >= AM.OffsetIndex && 
						( m[1] >= AM.OffsetIndex+WILDOFFSET ||
						indices[m[1]-AM.OffsetIndex].dimension != 0 ) ) {
							*alist++ = m[1]; m += 2; i--;
						}
						else if ( ten2 >= TENSORFUNCTION && *m != -INDEX
						 && *m != -VECTOR && *m != -MINVECTOR &&
						( *m != -SNUMBER || *m < 0 || *m >= AM.OffsetIndex ) ) {
							i = numargs; break;
						}
						else { NEXTARG(m) }
					}
					if ( i < numargs ) {
						*loc++ = w;
						nfun++;
						while ( --i >= 0 ) *alist++ = -1;
					}
				}
			}
			else t += t[1];
		}
		if ( nfun < minloopsize ) return(0);
	}
/*
	We have now nfun objects. Not all indices may be usable though.
	If the list is not long, we use a quadratic algorithm to remove
	indices and vertices that cannot be used. If it becomes large we
	sort the list of available indices (and their multiplicity) and
	work with binary searches.
*/
	alist = funargs; totnum = numargs*nfun;
	if ( nfun > 7 ) {
		if ( funisize < totnum ) {
			if ( funinds ) M_free(funinds,"funinds");
			funisize = (totnum*3)/2;
			funinds = (int *)Malloc1(funisize*2*sizeof(int),"funinds");
		}
		i = totnum; n = 0; wi = funinds;
		while ( --i >= 0 ) {
			if ( *alist >= 0 ) { n++; *wi++ = *alist; *wi++ = 1; }
			alist++;
		}
		n = SortTheList(funinds,n);
		do {
			action = 0;
			for ( i = 0; i < nfun; i++ ) {
				alist = funargs + i*numargs;
				jj = numargs;
				for ( j = 0; j < jj; j++ ) {
					if ( alist[j] < 0 ) break;
					mini = 0; maxi = n-1;
					while ( mini <= maxi ) {
						medi = (mini + maxi) / 2; k = funinds[2*medi];
						if ( alist[j] > k ) mini = medi + 1;
						else if ( alist[j] < k ) maxi = medi - 1;
						else break;
					}
					if ( funinds[2*medi+1] <= 1 ) {
						(funinds[2*medi+1])--;
						jj--; k = j; while ( k < jj ) { alist[k] = alist[k+1]; k++; }
						alist[jj] = -1; j--;
					}
				}
				if ( jj < 2 ) {
					if ( jj == 1 ) {
						mini = 0; maxi = n-1;
						while ( mini <= maxi ) {
							medi = (mini + maxi) / 2; k = funinds[2*medi];
							if ( alist[0] > k ) mini = medi + 1;
							else if ( alist[0] < k ) maxi = medi - 1;
							else break;
						}
						(funinds[2*medi+1])--;
						if ( funinds[2*medi+1] == 1 ) action++;
					}
					nfun--; totnum -= numargs; funlocs[i] = funlocs[nfun];
					wi = funargs + nfun*numargs;
					for ( j = 0; j < numargs; j++ ) alist[j] = *wi++;
					i--;
				}
			}
		} while ( action );
	}
	else {
		for ( i = 0; i < totnum; i++ ) {
			if ( alist[i] == -1 ) continue;
			for ( j = 0; j < totnum; j++ ) {
				if ( alist[j] == alist[i] && j != i ) break;
			}
			if ( j >= totnum ) alist[i] = -1;
		}
		do {
			action = 0;
			for ( i = 0; i < nfun; i++ ) {
				alist = funargs + i*numargs;
				n = numargs;
				for ( k = 0; k < n; k++ ) {
					if ( alist[k] < 0 ) { alist[k--] = alist[--n]; alist[n] = -1; }
				}
				if ( n <= 1 ) {
					if ( n == 1 ) { j = alist[0]; }
					else j = -1;
					nfun--; totnum -= numargs; funlocs[i] = funlocs[nfun];
					wi = funargs + nfun * numargs;
					for ( k = 0; k < numargs; k++ ) alist[k] = wi[k];
					i--;
					if ( j >= 0 ) {
						for ( k = 0, jj = 0, wi = funargs; k < totnum; k++, wi++ ) {
							if ( *wi == j ) { jj++; if ( jj > 1 ) break; }
						}
						if ( jj <= 1 ) {
							for ( k = 0, wi = funargs; k < totnum; k++, wi++ ) {
								if ( *wi == j ) { *wi = -1; action = 1; }
							}
						}
					}
				}
			}
		} while ( action );
	}
	if ( nfun < minloopsize ) return(0);
/*
	Now we have nfun objects, each with at least 2 indices, each of which
	occurs at least twice in our list. There will be a loop!
*/
	if ( mode != 0 && mode != 1 ) {
		if ( mode > 0 ) tohunt =  mode - 5;
		else            tohunt = -mode - 5;
		nargs = numargs; numoffuns = nfun;
		i = 0;
		if ( loopsize < 0 ) {
			if ( loopsize == -1 ) k = nfun;
			else { k = -loopsize-1; if ( k > nfun ) k = nfun; }
			for ( L = 2; L <= k; L++ ) {
				if ( FindLus(0,L,tohunt) ) goto Success;
			}
		}
		else if ( FindLus(0,loopsize,tohunt) ) { L = loopsize; goto Success; }
	}
	else {
		nargs = numargs; numoffuns = nfun;
		if ( loopsize < 0 ) {
			jj = 2; k = nfun;
			if ( loopsize < -1 ) { k = -loopsize-1; if ( k > nfun ) k = nfun; }
		}
		else { jj = k = loopsize; }
		for ( L = jj; L <= k; L++ ) {
			for ( i = 0; i <= nfun-L; i++ ) {
				alist = funargs + i * numargs;
				for ( jj = 0; jj < numargs; jj++ ) {
					if ( alist[jj] < 0 ) continue;
					tohunt = alist[jj];
					for ( j = jj+1; j < numargs; j++ ) {
						if ( alist[j] < 0 ) continue;
						if ( FindLus(i+1,L-1,alist[j]) ) {
							alist[0] = alist[jj];
							alist[1] = alist[j];
							goto Success;
						}
					}
				}
			}
		}
	}
	return(0);
Success:;
	if ( mode == 0 || mode > 1 ) return(1);
/*
	Now we have to make the replacement and fix the potential sign
*/
	sign2 = 1;
	wi = funargs + i*numargs; loc = funlocs + i;
	for ( k = 0; k < L; k++ ) *(loc[k]) = -1;
	if ( AR.WorkPointer < term + *term ) AR.WorkPointer = term + *term;
	w = AR.WorkPointer + 1;
	m = t = term + 1;
	while ( t < tstop ) {
		if ( *t == -1 ) break;
		t += t[1];
	}
	while ( m < t ) *w++ = *m++;
	r = w;
	*w++ = outfun;
	w++;
	*w++ = DIRTYFLAG;
	FILLFUN3(w)
	if ( functions[outfun-FUNCTION].spec >= TENSORFUNCTION ) {
		if ( ten >= TENSORFUNCTION ) {
			for ( i = 0; i < L; i++ ) {
				alist = wi + i*numargs;
				m = loc[i] + FUNHEAD;
				for ( k = 0; k < numargs; k++ ) {
					if ( m[k] == alist[0] ) {
						if ( k != 0 ) {
							jj = m[k]; m[k] = m[0]; m[0] = jj;
							sign = -sign;
						}
						break;
					}
				}
				for ( k = 1; k < numargs; k++ ) {
					if ( m[k] == alist[1] ) {
						if ( k != 1 ) {
							jj = m[k]; m[k] = m[1]; m[1] = jj;
							sign = -sign;
						}
						break;
					}
				}
				m += 2;
				for ( k = 2; k < numargs; k++ ) *w++ = *m++;
			}
		}
		else {
			for ( i = 0; i < L; i++ ) {
				alist = wi + i*numargs;
				tt = loc[i];
				m = tt + FUNHEAD;
				for ( k = 0; k < numargs; k++ ) {
					if ( m[2*k] == -INDEX && m[2*k+1] == alist[0] ) {
						if ( k != 0 ) {
							jj = m[2*k]; m[2*k] = m[0]; m[0] = jj;
							jj = m[2*k+1]; m[2*k+1] = m[0]; m[0] = jj;
							sign = -sign;
							break;
						}
					}
				}
				for ( k = 1; k < numargs; k++ ) {
					if ( m[2*k] == -INDEX && m[2*k+1] == alist[1] ) {
						if ( k != 1 ) {
							jj = m[2*k]; m[2*k] = m[2]; m[2] = jj;
							jj = m[2*k+1]; m[2*k+1] = m[3]; m[3] = jj;
							sign = -sign;
							break;
						}
					}
				}
				m += 4; tt += tt[1];
				while ( m < tt ) {
					if ( *m == -MINVECTOR ) sign2 = -sign2;
					m++; *w++ = *m++;
				}
			}
		}
	}
	else {
		if ( ten >= TENSORFUNCTION ) {
			for ( i = 0; i < L; i++ ) {
				alist = wi + i*numargs;
				m = loc[i] + FUNHEAD;
				for ( k = 0; k < numargs; k++ ) {
					if ( m[k] == alist[0] ) {
						if ( k != 0 ) {
							jj = m[k]; m[k] = m[0]; m[0] = jj;
							sign = -sign;
							break;
						}
					}
				}
				for ( k = 1; k < numargs; k++ ) {
					if ( m[k] == alist[1] ) {
						if ( k != 1 ) {
							jj = m[k]; m[k] = m[1]; m[1] = jj;
							sign = -sign;
							break;
						}
					}
				}
				m += 2;
				for ( k = 2; k < numargs; k++ ) {
					if ( *m >= AM.OffsetIndex ) { *w++ = -INDEX; }
					else if ( *m < 0 ) { *w++ = -VECTOR; }
					else { *w = -SNUMBER; }
					*w++ = *m++;
				}
			}
		}
		else {
			for ( i = 0; i < L; i++ ) {
				alist = wi + i*numargs;
				tt = loc[i];
				m = tt + FUNHEAD; jj = -1;
				for ( k = 0, j = 0; k < numargs; k++ ) {
					if ( *m == -INDEX && m[1] == alist[0] && ( j & 1 ) == 0 ) {
						if ( k != 0 && jj != 0 ) { sign = -sign; jj = k; }
						NEXTARG(m)
						j |= 1;
					}
					else if ( *m == -INDEX && m[1] == alist[1] && ( j & 2 ) == 0 ) {
						if ( k != 1 && jj != 1 ) { sign = -sign; jj = k; }
						NEXTARG(m)
						j |= 2;
					}
					else {
						tt = m;
						NEXTARG(m)
						while ( tt < m ) *w++ = *tt++;
					}
				}
			}
		}
	}
	r[1] = w-r;
	while ( t < tstop ) {
		if ( *t == -1 ) { t += t[1]; continue; }
		i = t[1];
		NCOPY(w,t,i)
	}
	tstop = term + *term;
	while ( t < tstop ) *w++ = *t++;
	if ( sign < 0 ) w[-1] = -w[-1];
	i = w - AR.WorkPointer;
	*AR.WorkPointer = i;
	t = term; w = AR.WorkPointer;
	NCOPY(t,w,i)
	*AR.RepPoint = 1;	/* For Repeat */
	return(1);
}

/*
  	#] Lus :
  	#[ FindLus :
*/

int FindLus ARG3(int, from, int, level, int, openindex)
{
	int i, j, k, jj, *alist, *blist, *w, *m, partner;
	WORD **loc = funlocs, *wor;
	if ( level == 1 ) {
		for ( i = from; i < numoffuns; i++ ) {
			alist = funargs + i*nargs;
			for ( j = 0; j < nargs; j++ ) {
				if ( alist[j] == openindex ) {
					for ( k = 0; k < nargs; k++ ) {
						if ( k == j ) continue;
						if ( alist[k] == tohunt ) {
							loc[from] = loc[i];
							alist = funargs + from*nargs;
							alist[0] = openindex; alist[1] = tohunt;
							return(1);
						}
					}
				}
			}
		}
	}
	else {
		for ( i = from; i < numoffuns; i++ ) {
			alist = funargs + i*nargs;
			for ( j = 0; j < nargs; j++ ) {
				if ( alist[j] == openindex ) {
					if ( from != i ) {
						wor = loc[i]; loc[i] = loc[from]; loc[from] = wor;
						blist = w = funargs + from*nargs;
						m = alist;
						k = nargs;
						while ( --k >= 0 ) { jj = *m; *m++ = *w; *w++ = jj; }
					}
					else blist = alist;
					for ( k = 0; k < nargs; k++ ) {
						if ( k == j || blist[k] < 0 ) continue;
						partner = blist[k];
						if ( FindLus(from+1,level-1,partner) ) {
							blist[0] = openindex; blist[1] = partner;
							return(1);
						}
					}
					if ( from != i ) {
						wor = loc[i]; loc[i] = loc[from]; loc[from] = wor;
						w = funargs + from*nargs;
						m = alist;
						k = nargs;
						while ( --k >= 0 ) { jj = *m; *m++ = *w; *w++ = jj; }
					}
				}
			}
		}
	}
	return(0);
}

/*
  	#] FindLus : 
  	#[ SortTheList :
*/

static int *tlistbuf;
static int tlistsize = 0;

int SortTheList ARG2(int *,slist,int,num)
{
	int i, nleft, nright, *t1, *t2, *t3, *rlist;
	if ( num <= 2 ) {
		if ( num <=  1 ) return(num);
		if ( slist[0] < slist[2] ) return(2);
		if ( slist[0] > slist[2] ) {
			i = slist[0]; slist[0] = slist[2]; slist[2] = i;
			i = slist[1]; slist[1] = slist[3]; slist[3] = i;
			return(2);
		}
		slist[1] += slist[3];
		return(1);
	}
	else {
		nleft = num/2; rlist = slist + 2*nleft;
		nright = SortTheList(rlist,num-nleft);
		nleft = SortTheList(slist,nleft);
		if ( tlistsize < nleft ) {
			if ( tlistbuf ) M_free(tlistbuf,"tlistbuf");
			tlistsize = (nleft*3)/2;
			tlistbuf = (int *)Malloc1(tlistsize*2*sizeof(int),"tlistbuf");
		}
		i = nleft; t1 = slist; t2 = tlistbuf;
		while ( --i >= 0 ) { *t2++ = *t1++; *t2++ = *t1++; }
		i = nleft+nright; t1 = tlistbuf; t2 = rlist; t3 = slist;
		while ( nleft > 0 && nright > 0 ) {
			if ( *t1 < *t2 ) {
				*t3++ = *t1++; *t3++ = *t1++; nleft--;
			}
			else if ( *t1 > *t2 ) {
				*t3++ = *t2++; *t3++ = *t2++; nright--;
			}
			else {
				*t3++ = *t1++; t2++; *t3++ = (*t1++) + (*t2++); i--;
				nleft--; nright--;
			}
		}
		while ( --nleft >= 0 ) { *t3++ = *t1++; *t3++ = *t1++; }
		while ( --nright >= 0 ) { *t3++ = *t2++; *t3++ = *t2++; }
		return(i);
	}
}

/*
  	#] SortTheList : 
*/

