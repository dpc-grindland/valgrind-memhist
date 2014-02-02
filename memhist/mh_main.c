
/*--------------------------------------------------------------------*/
/*--- A Valgrind tool for memory debugging.              mh_main.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is copied from Lackey, an example Valgrind tool that does
   some simple program measurement and tracing.

   Copyright (C) 2002-2010 Nicholas Nethercote
      njn@valgrind.org

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

// This code was forked from lackey/lk_main.c
// A lot of code for detailed execution information gathering has been removed
// but the central memory access tracking is left almost untouched.

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_options.h"
#include "pub_tool_machine.h"     // VG_(fnptr_to_fnentry)
#include "pub_tool_execontext.h"
#include "pub_tool_threadstate.h"
#include "pub_tool_mallocfree.h"

#include "memhist.h"  // client requests


/*------------------------------------------------------------*/
/*--- Command line options                                 ---*/
/*------------------------------------------------------------*/

/* Command line options controlling instrumentation kinds, as described at
 * the top of this file. */
static Bool clo_track_mem       = True;
static Bool clo_trace_mem       = False;

static Bool mh_process_cmd_line_option(const HChar* arg)
{
   if VG_BOOL_CLO(arg, "--trace-mem", clo_trace_mem) {}
   else
      return False;

   return True;
}

static void mh_print_usage(void)
{
   VG_(printf)(
"    --trace-mem=no|yes        trace all stores [no]\n"
   );
}

static void mh_print_debug_usage(void)
{
   VG_(printf)("    (none)\n");
}


/* --- Operations --- */

typedef enum { OpLoad=0, OpStore=1, OpAlu=2 } Op;

#define N_OPS 3

/*
 * Memory access tracking
 */

#define MAX_DSIZE    512

typedef
   IRExpr
   IRAtom;

typedef
   enum { Event_Ir, Event_Dr, Event_Dw, Event_Dm }
   EventKind;

typedef
   struct {
      EventKind  ekind;
      ThreadId	 tid;
      IRAtom*    addr;
      Int        size;
   }
   Event;

/* Up to this many unnotified events are allowed.  Must be at least two,
   so that reads and writes to the same address can be merged into a modify.
   Beyond that, larger numbers just potentially induce more spilling due to
   extending live ranges of address temporaries. */
#define N_EVENTS 4

/* Maintain an ordered list of memory events which are outstanding, in
   the sense that no IR has yet been generated to do the relevant
   helper calls.  The SB is scanned top to bottom and memory events
   are added to the end of the list, merging with the most recent
   notified event where possible (Dw immediately following Dr and
   having the same size and EA can be merged).

   This merging is done so that for architectures which have
   load-op-store instructions (x86, amd64), the instr is treated as if
   it makes just one memory reference (a modify), rather than two (a
   read followed by a write at the same address).

   At various points the list will need to be flushed, that is, IR
   generated from it.  That must happen before any possible exit from
   the block (the end, or an IRStmt_Exit).  Flushing also takes place
   when there is no space to add a new event.

   If we require the simulation statistics to be up to date with
   respect to possible memory exceptions, then the list would have to
   be flushed before each memory reference.  That's a pain so we don't
   bother.

   Flushing the list consists of walking it start to end and emitting
   instrumentation IR for each event, in the order in which they
   appear. */

static Event events[N_EVENTS];
static Int   events_used = 0;


struct mh_mem_access_t
{
    ExeContext* call_stack;
    unsigned time_stamp;
    HWord data;
};

struct mh_track_mem_block_t
{
    struct mh_track_mem_block_t* next;
    Addr start;
    Addr end;
    const char* name;
    unsigned birth_time_stamp;
    Bool     enabled;
    unsigned word_sz;  /* in bytes */
    unsigned nwords;   /* #columns */
    unsigned history;  /* #rows */
    struct mh_mem_access_t* access_matrix;
    unsigned hist_ix_vec[0];
};

static struct mh_track_mem_block_t* mh_track_list;

static unsigned mh_logical_time = 0;

/*
VG_REGPARM(2)
static void track_instr(Addr addr, SizeT size)
{
}

VG_REGPARM(2)
static void track_load(Addr addr, SizeT size)
{
}
*/

VG_REGPARM(2)
static void track_store(Addr addr, SizeT size)
{
    Addr start = addr;
    Addr end = addr + size;
    struct mh_track_mem_block_t *tmb;
    int got_a_hit = 0;

    for (tmb = mh_track_list; tmb; tmb = tmb->next) {
	if (end > tmb->start && start < tmb->end) {
	    if (tmb->enabled) {
		ThreadId tid = VG_(get_running_tid)();  // Should tid be passed as arg instead?
		ExeContext *ec = VG_(record_ExeContext)(tid, 0);
		unsigned wix; /* word index */
		unsigned start_wix, end_wix;


		if (start < tmb->start)
		    start = tmb->start;
		if (end > tmb->end)
		    end = tmb->end;

		start_wix = (start - tmb->start) / tmb->word_sz;
		end_wix = (end - tmb->start - 1) / tmb->word_sz + 1;
		tl_assert(start_wix < end_wix);
		tl_assert(end_wix <= tmb->nwords);

		if (clo_trace_mem) {
		    VG_(umsg)("TRACE: %u bytes written at addr %p at time %u:\n",
			      (unsigned)size, (void *)addr, mh_logical_time);
		    VG_(pp_ExeContext)(ec);
		}

		for (wix = start_wix; wix < end_wix; wix++) {
		    int i;
		    unsigned hix = tmb->hist_ix_vec[wix]++;
		    HWord data;

		    if (tmb->hist_ix_vec[wix] >= tmb->history)
			tmb->hist_ix_vec[wix] = 0;

		    i = (tmb->history * wix) + hix;
		    //VG_(umsg)("TRACE: Saving at wix=%u hix=%u -> i=%u\n", wix, hix, i);
		    tmb->access_matrix[i].call_stack = ec;
		    tmb->access_matrix[i].time_stamp = mh_logical_time;
		    switch (size) {
		    case sizeof(HWord): data = *(HWord*)start; break;
		    case sizeof(int): data = *(int*)start; break;
		    case sizeof(short): data = *(short*)start; break;
		    case sizeof(char): data = *(char*)start; break;
		    default: data = 0xdead;
		    }
		    tmb->access_matrix[i].data = data;

		    start += tmb->word_sz;
		}
		start = addr;
		end = addr + size;
		got_a_hit = 1;
	    }
	    else {
		//VG_(umsg)("TRACE: Disabled???\n");
	    }
	}
	else {
	    //VG_(umsg)("TRACE: Not within\n");
	}
    }
    if (got_a_hit)
	++mh_logical_time;
}


/*static VG_REGPARM(2) void track_modify(Addr addr, SizeT size)
{
	//VG_(printf)(" M %08lx,%lu\n", addr, size);
}*/


static void flushEvents(IRSB* sb)
{
   Int        i;
   const HChar*      helperName;
   void*      helperAddr;
   IRExpr**   argv;
   IRDirty*   di;
   Event*     ev;

   for (i = 0; i < events_used; i++) {

      ev = &events[i];

      // Decide on helper fn to call and args to pass it.
      switch (ev->ekind) {
      case Event_Ir:
	  /*helperName = "track_instr";
	  helperAddr =  track_instr;
	  argv = mkIRExprVec_2(ev->addr, mkIRExpr_HWord(ev->size));
	  argc = 2;*/
	  helperAddr = NULL;
	  break;

      case Event_Dr:
	  /*helperName = "track_load";
	  helperAddr =  track_load;
	  argv = mkIRExprVec_2(ev->addr, mkIRExpr_HWord(ev->size));
	  argc = 2;*/
	  helperAddr = NULL;
	  break;

      case Event_Dw:
	  helperName = "track_store";
	  helperAddr =  track_store;
	  argv = mkIRExprVec_2(ev->addr, mkIRExpr_HWord(ev->size));
	  break;

         /*case Event_Dm: helperName = "track_modify";
                        helperAddr =  track_modify; break;*/
      default:
	  tl_assert(0);
      }

      // Add the helper.
      if (helperAddr) {
	  di = unsafeIRDirty_0_N(2, /*regparms*/
				 helperName, VG_(fnptr_to_fnentry)( helperAddr ),
				 argv);
	  addStmtToIRSB( sb, IRStmt_Dirty(di) );
      }
   }

   events_used = 0;
}

// WARNING:  If you aren't interested in instruction reads, you can omit the
// code that adds calls to track_instr() in flushEvents().  However, you
// must still call this function, addEvent_Ir() -- it is necessary to add
// the Ir events to the events list so that merging of paired load/store
// events into modify events works correctly.
static void addEvent_Ir ( IRSB* sb, IRAtom* iaddr, UInt isize )
{
   Event* evt;
   tl_assert(clo_track_mem);
   tl_assert( (VG_MIN_INSTR_SZB <= isize && isize <= VG_MAX_INSTR_SZB)
            || VG_CLREQ_SZB == isize );
   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Ir;
   evt->addr  = iaddr;
   evt->size  = isize;
   events_used++;
}

static
void addEvent_Dr ( IRSB* sb, IRAtom* daddr, Int dsize)
{
   Event* evt;
   tl_assert(clo_track_mem);
   tl_assert(isIRAtom(daddr));
   tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);
   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Dr;
   evt->addr  = daddr;
   evt->size  = dsize;
   events_used++;
}

static
void addEvent_Dw (IRSB* sb, IRAtom* daddr, Int dsize)
{
   //Event* lastEvt;
   Event* evt;
   tl_assert(clo_track_mem);
   tl_assert(isIRAtom(daddr));
   tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);

   // Is it possible to merge this write with the preceding read?
   /*lastEvt = &events[events_used-1];
   if (events_used > 0 && lastEvt->ekind == Event_Dr
	   && lastEvt->size  == dsize && eqIRAtom(lastEvt->addr, daddr))
   {
      lastEvt->ekind = Event_Dm;
      return;
   }*/

   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Dw;
   //evt->tid   = tid;
   evt->size  = dsize;
   evt->addr  = daddr;
   events_used++;
}


/*------------------------------------------------------------*/
/*--- Basic tool functions                                 ---*/
/*------------------------------------------------------------*/

static void mh_post_clo_init(void)
{
}

static
IRSB* mh_instrument ( VgCallbackClosure* closure,
                      IRSB* sbIn,
                      VexGuestLayout* layout,
                      VexGuestExtents* vge,
		      VexArchInfo* arch,
                      IRType gWordTy, IRType hWordTy )
{
   Int        i;
   IRSB*      sbOut;
   IRTypeEnv* tyenv = sbIn->tyenv;

   if (gWordTy != hWordTy) {
      /* We don't currently support this case. */
      VG_(tool_panic)("host/guest word size mismatch");
   }

   /* Set up SB */
   sbOut = deepCopyIRSBExceptStmts(sbIn);

   // Copy verbatim any IR preamble preceding the first IMark
   i = 0;
   while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
      addStmtToIRSB( sbOut, sbIn->stmts[i] );
      i++;
   }

   if (clo_track_mem) {
      events_used = 0;
   }

   for (/*use current i*/; i < sbIn->stmts_used; i++) {
      IRStmt* st = sbIn->stmts[i];
      if (!st || st->tag == Ist_NoOp) continue;

      switch (st->tag) {
         case Ist_NoOp:
         case Ist_AbiHint:
         case Ist_Put:
         case Ist_PutI:
         case Ist_MBE:
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_IMark:
            if (clo_track_mem) {
               // WARNING: do not remove this function call, even if you
               // aren't interested in instruction reads.  See the comment
               // above the function itself for more detail.
               addEvent_Ir( sbOut, mkIRExpr_HWord( (HWord)st->Ist.IMark.addr ),
                            st->Ist.IMark.len );
            }
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_WrTmp:
            if (clo_track_mem) {
               IRExpr* data = st->Ist.WrTmp.data;
               if (data->tag == Iex_Load) {
                  addEvent_Dr( sbOut, data->Iex.Load.addr,
                               sizeofIRType(data->Iex.Load.ty) );
               }
            }
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_Store:
            if (clo_track_mem) {
               IRExpr* data  = st->Ist.Store.data;
               addEvent_Dw( sbOut, st->Ist.Store.addr,
                            sizeofIRType(typeOfIRExpr(tyenv, data)));
            }
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_Dirty: {
            if (clo_track_mem) {
               Int      dsize;
               IRDirty* d = st->Ist.Dirty.details;
               if (d->mFx != Ifx_None) {
                  // This dirty helper accesses memory.  Collect the details.
                  tl_assert(d->mAddr != NULL);
                  tl_assert(d->mSize != 0);
                  dsize = d->mSize;
                  if (d->mFx == Ifx_Read || d->mFx == Ifx_Modify)
                     addEvent_Dr( sbOut, d->mAddr, dsize );
                  if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
                     addEvent_Dw( sbOut, d->mAddr, dsize);
               } else {
                  tl_assert(d->mAddr == NULL);
                  tl_assert(d->mSize == 0);
               }
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_CAS: {
            /* We treat it as a read and a write of the location.  I
               think that is the same behaviour as it was before IRCAS
               was introduced, since prior to that point, the Vex
               front ends would translate a lock-prefixed instruction
               into a (normal) read followed by a (normal) write. */
            Int    dataSize;
            IRType dataTy;
            IRCAS* cas = st->Ist.CAS.details;
            tl_assert(cas->addr != NULL);
            tl_assert(cas->dataLo != NULL);
            dataTy   = typeOfIRExpr(tyenv, cas->dataLo);
            dataSize = sizeofIRType(dataTy);
            if (cas->dataHi != NULL)
               dataSize *= 2; /* since it's a doubleword-CAS */
            if (clo_track_mem) {
               addEvent_Dr( sbOut, cas->addr, dataSize );
               addEvent_Dw( sbOut, cas->addr, dataSize);
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_LLSC: {
            IRType dataTy;
            if (st->Ist.LLSC.storedata == NULL) {
               /* LL */
               dataTy = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
               if (clo_track_mem)
                  addEvent_Dr( sbOut, st->Ist.LLSC.addr,
                                      sizeofIRType(dataTy) );
            } else {
               /* SC */
               dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
               if (clo_track_mem) {
                  addEvent_Dw (sbOut, st->Ist.LLSC.addr, sizeofIRType(dataTy));
	       }
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_Exit:
            if (clo_track_mem) {
               flushEvents(sbOut);
            }

            addStmtToIRSB( sbOut, st );      // Original statement

            break;

         default:
            tl_assert(0);
      }
   }

   if (clo_track_mem) {
      /* At the end of the sbIn.  Flush outstandings. */
      flushEvents(sbOut);
   }

   return sbOut;
}

static unsigned align_up(unsigned unit, unsigned value)
{
    return ((value + unit - 1) / unit) * unit;
}


static void track_mem_write(Addr addr, SizeT size, unsigned word_sz, unsigned history,
			    const char* name)
{
    struct mh_track_mem_block_t *tmb;
    const unsigned nwords = (size + word_sz - 1) / word_sz;
    const unsigned sizeof_hist_ix_vec = nwords * sizeof(*tmb->hist_ix_vec);
    unsigned i;
    const unsigned matrix_offset = align_up(sizeof(void*),
					    (sizeof(struct mh_track_mem_block_t)
					     + sizeof_hist_ix_vec));

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: Tracking %u-words from %p to %p with history %u\n",
		  word_sz, (void*)addr, (void*)(addr+size), history);
    }

    tmb = VG_(malloc)("track_mem_write",
		      matrix_offset + history * nwords * sizeof(struct mh_mem_access_t));
    tmb->start = addr;
    tmb->end = addr + size;
    tmb->name = name;
    tmb->birth_time_stamp = mh_logical_time++;
    tmb->enabled = True;
    tmb->word_sz = word_sz;
    tmb->nwords = nwords;
    tmb->history = history;
    tmb->access_matrix = (struct mh_mem_access_t*) ((char*)tmb + matrix_offset);
    tl_assert((char*)&tmb->hist_ix_vec[history] <= (char*)tmb->access_matrix);
    for (i = 0; i < nwords; i++) {
	tmb->hist_ix_vec[i] = 0;
    }
    for (i = 0; i < history*nwords; i++) {
	tmb->access_matrix[i].call_stack = NULL;
	tmb->access_matrix[i].time_stamp = 0;
    }
    tmb->next = mh_track_list;
    mh_track_list = tmb;
}

static void untrack_mem_write (Addr addr, SizeT size)
{
    Addr end = addr + size;
    struct mh_track_mem_block_t* tmb;
    struct mh_track_mem_block_t** prevp = &mh_track_list;

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: Untracking from %p to %p\n",
		  (void*)addr, (void*)end);
    }

    for (tmb = *prevp; tmb; tmb = *prevp) {
	if (addr == tmb->start) {
	    tl_assert(end == tmb->end);
	    *prevp = tmb->next;
	    VG_(free) (tmb);
	    return;
	}
	else {
	    prevp = &tmb->next;
	}
    }
    tl_assert(!"Could not find region to untrack");
}


/*------------------------------------------------------------*/
/*--- Client requests                                      ---*/
/*------------------------------------------------------------*/

static Bool mh_handle_client_request ( ThreadId tid, UWord* arg, UWord* ret )
{
   if (!VG_IS_TOOL_USERREQ('M','H',arg[0])) {
      return False;
   }

   switch (arg[0]) {
      case VG_USERREQ__TRACK_MEM_WRITE:
	  track_mem_write (arg[1], arg[2], arg[3], arg[4], (char*)arg[5]);
         *ret = -1;
         break;
      case VG_USERREQ__UNTRACK_MEM_WRITE:
         untrack_mem_write (arg[1], arg[2]);
         *ret = -1;
         break;

      default:
         VG_(message)(
            Vg_UserMsg,
            "Warning: unknown memcheck client request code %llx\n",
            (ULong)arg[0]
         );
         return False;
   }
   return True;
}


static void print_word(unsigned word_sz, struct mh_mem_access_t* ap)
{
    switch (word_sz) {
    case sizeof(HWord):
	VG_(umsg)("%p", (void*)ap->data); break;
    case sizeof(int):
	VG_(umsg)("%x", (int)ap->data); break;
    case sizeof(short):
	VG_(umsg)("%x", (int)(short)ap->data); break;
    case sizeof(char):
	VG_(umsg)("%x", (int)(char)ap->data); break;
    default:
	VG_(umsg)("(?)"); break;
    }
}


static void mh_fini(Int exitcode)
{
    struct mh_track_mem_block_t* tmb;

    for (tmb = mh_track_list; tmb; tmb=tmb->next) {
	unsigned wix = 0; /* word index */
	Addr addr = tmb->start;
	VG_(umsg) ("Memhist tracking '%s' from %p to %p with word size %u "
		   "and history %u created at time %u.\n", tmb->name,
		   (void*)tmb->start, (void*)tmb->end, tmb->word_sz,
		   tmb->history, tmb->birth_time_stamp);
	for (addr=tmb->start; addr < tmb->end; wix++, addr += tmb->word_sz) {
	    unsigned h;
	    int hist_ix = tmb->hist_ix_vec[wix] - 1;

	    for (h=0; h < tmb->history; h++, hist_ix--) {
		struct mh_mem_access_t* ap;

		if (hist_ix < 0)
		    hist_ix = tmb->history - 1;

		//VG_(umsg)("TRACE: Reading at ix=%u\n", wix*tmb->history + hist_ix);
		ap = &tmb->access_matrix[wix*tmb->history + hist_ix];
		if (ap->call_stack) {
		    if (!h) {
			VG_(umsg)("%u-bytes ", tmb->word_sz);
			print_word(tmb->word_sz, ap);
			VG_(umsg)(" written to address %p at time %u:\n",
				  (void*)addr, ap->time_stamp);
		    }
		    else {
			VG_(umsg) ("       AND ");
			print_word(tmb->word_sz, ap);
			VG_(umsg) (" written at time %u:\n", ap->time_stamp);
		    }
		    VG_(pp_ExeContext)(ap->call_stack);
		}
		else {
		    if (!h)
			VG_(umsg) ("%u-bytes at %p not written.\n", tmb->word_sz, (void*)addr);
		    break;
		}
	    }
	}
    }
}

static void mh_pre_clo_init(void)
{
   VG_(details_name)            ("Memhist");
   VG_(details_version)         (NULL);
   VG_(details_description)     ("Sverker's Valgrind tool for tracking memory access history");
   VG_(details_copyright_author)(
      "Copyright (C) 2014, and GNU GPL'd, by Sverker Eriksson.");
   VG_(details_bug_reports_to)  (VG_BUGS_TO);
   VG_(details_avg_translation_sizeB) ( 200 );

   VG_(basic_tool_funcs)          (mh_post_clo_init,
                                   mh_instrument,
                                   mh_fini);
   VG_(needs_command_line_options)(mh_process_cmd_line_option,
                                   mh_print_usage,
                                   mh_print_debug_usage);
   VG_(needs_client_requests)     (mh_handle_client_request);
}

VG_DETERMINE_INTERFACE_VERSION(mh_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                mh_main.c ---*/
/*--------------------------------------------------------------------*/
