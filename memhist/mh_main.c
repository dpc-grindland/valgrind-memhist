
/*--------------------------------------------------------------------*/
/*--- A Valgrind tool for memory debugging.              mh_main.c ---*/
/*--------------------------------------------------------------------*/

/*
   Copyright (C) 2014 Sverker Eriksson

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

// This code was initially forked from lackey/lk_main.c

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
#include "rb_tree.h"

#define MH_DEBUG

#ifdef MH_DEBUG
#  define MH_ASSERT tl_assert
#  define MH_ASSERT2 tl_assert2
#else
#  define MH_ASSERT(C)
#  define MH_ASSERT2(C, FMT, ARGS...)
#endif


/*------------------------------------------------------------*/
/*--- Command line options                                 ---*/
/*------------------------------------------------------------*/

/* Command line options controlling instrumentation kinds */
static Bool clo_trace_mem = False;

enum mh_track_type {
    MH_WRITE  = 1,   /* Data store */
    MH_READ   = 2,   /* Data load */
    MH_EXE    = 4,   /* Instruction execution */
    MH_TRACK  = 8
};

enum mh_track_type enabled_tracking = MH_WRITE | MH_READ;

static Bool mh_process_cmd_line_option(const HChar* arg)
{
    const HChar* prot_str;
    if VG_BOOL_CLO(arg, "--trace-mem", clo_trace_mem) {}
    else if (VG_STR_CLO(arg, "--enable-tracking", prot_str)) {
	enabled_tracking = 0;
	while (*prot_str) {
	    switch (*prot_str) {
	    case 'w': case 'W':
		enabled_tracking |= MH_WRITE;
		break;
	    case 'r': case 'R':
		enabled_tracking |= MH_READ;
		break;
	    case 'x': case 'X':
		enabled_tracking |= MH_EXE;
		break;
	    default:
		VG_(fmsg_bad_option)(arg, "Invalid tracking type '%c'"
				  " (should be 'W', 'R' or 'X')\n", *prot_str);
	    }
	    ++prot_str;
	}
    }
    else return False;

    return True;
}

static void mh_print_usage(void)
{
    VG_(printf)("    --trace-mem=no|yes         trace all stores [no]\n");
    VG_(printf)("    --enable-tracking=[RWX]*   enable tracking of all Reads, Writes and/or eXecution [RW]\n");
}

static void mh_print_debug_usage(void)
{
    VG_(printf)("    (none)\n");
}


#ifdef MH_DEBUG
static Bool fit_in_ubytes(ULong v, Int nbytes)
{
    return nbytes >= sizeof(ULong) || (v >> (nbytes * 8)) == 0;
}
#endif


/*
 * Memory access tracking
 */

#define MAX_DSIZE    512


struct mh_mem_access_t {
    ExeContext* call_stack;
    unsigned time_stamp;
    HWord data;
};


static const char* prot_txt(enum mh_track_type flags)
{
    static const char* txt[] = {"NOWRITE", "NOREAD", "NOACCESS"};
    tl_assert(flags & (MH_WRITE | MH_READ));

    return txt[(flags & 3) - 1];
}

struct mh_region_t {
    rb_tree_node node;
    Addr start;
    Addr end;
    const char* name;
    unsigned birth_time_stamp;
    unsigned readonly_time_stamp;
    Bool     enabled;
    enum mh_track_type type;
    unsigned word_sz;  /* in bytes */
    unsigned nwords;   /* #columns */
    unsigned history;  /* #rows */
    struct mh_mem_access_t* access_matrix;
    unsigned hist_ix_vec[0];
};

static int region_cmp_key(rb_tree_node* a_node, void* b_key)
{
    struct mh_region_t* a = (struct mh_region_t*)a_node;
    Addr b_start = (Addr)b_key;
    return a->start < b_start ? -1 : (a->start == b_start ? 0 : 1);
}

static int region_cmp(rb_tree_node* a_node, rb_tree_node* b_node)
{
    struct mh_region_t* b = (struct mh_region_t*)b_node;
    return region_cmp_key(a_node, (void*)b->start);
}

static void region_print(rb_tree_node* a_node, int depth)
{
    static char spaces[] = "                                                  ";
    struct mh_region_t* a = (struct mh_region_t*)a_node;
    VG_(umsg)("%.*s%p -> %p", depth, spaces, (void*)a->start, (void*)a->end);
}


static struct rb_tree region_tree;

static
struct mh_region_t* region_insert(struct mh_region_t* rp)
{
    return (struct mh_region_t*)rb_tree_insert(&region_tree,
					       &rp->node);
}

static void region_remove(struct mh_region_t* rp)
{
    rb_tree_remove(&region_tree, &rp->node);
}

static
struct mh_region_t* region_min(void)
{
    return (struct mh_region_t*)rb_tree_min(&region_tree);
}

static
struct mh_region_t* region_succ(struct mh_region_t* rp)
{
    return (struct mh_region_t*)rb_tree_succ(&region_tree,
					     &rp->node);
}

static
struct mh_region_t* region_pred(struct mh_region_t* rp)
{
    return (struct mh_region_t*)rb_tree_pred(&region_tree,
					     &rp->node);
}

static
struct mh_region_t* region_lookup_maxle(Addr addr)
{
    return (struct mh_region_t*)rb_tree_lookup_maxle(&region_tree,
						     (void*)addr);
}

static
struct mh_region_t* region_lookup_ming(Addr addr)
{
    return (struct mh_region_t*)rb_tree_lookup_ming(&region_tree,
						    (void*)addr);
}

static void insert_nonoverlapping(struct mh_region_t* rp)
{
    struct mh_region_t* clash = region_insert(rp);
    tl_assert(!clash);
    tl_assert(!(clash = region_pred(rp)) || clash->end <= rp->start);
    tl_assert(!(clash = region_succ(rp)) || clash->start >= rp->end);
}


/* ---------------------------------------------------------------------
 * Runtime "helper" functions called for every data load, data store
 * or instruction fetch.
 * --------------------------------------------------------------------- */

static unsigned mh_logical_time = 0;

static void report_store_in_block(struct mh_region_t* rp,
				  Addr addr, SizeT size, Addr64 data)
{
    ThreadId tid = VG_(get_running_tid)();  // Should tid be passed as arg instead?
    ExeContext* ec = VG_(record_ExeContext)(tid, 0);
    unsigned wix; /* word index */
    unsigned start_wix, end_wix;
    Addr start = addr;
    Addr end = addr + size;

    if (start < rp->start) {
	union {
	    Addr64 words[2];
	    char bytes[2 * 8];
	}u;
	int offs = rp->start - start;
	u.words[0] = data;
	u.words[1] = 0;
	tl_assert(offs < 8);
	data = *(Addr64*)&u.bytes[offs];    /* BUG: Unaligned word access */
	start = rp->start;
    }
    if (end > rp->end) end = rp->end;

    start_wix = (start - rp->start) / rp->word_sz;
    end_wix = (end - rp->start - 1) / rp->word_sz + 1;
    tl_assert(start_wix < end_wix);
    tl_assert(end_wix <= rp->nwords);

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: %u bytes written at addr %p at time %u:\n",
		  (unsigned)size, (void*)addr, mh_logical_time);
	VG_(pp_ExeContext)(ec);
    }

    for (wix = start_wix; wix < end_wix; wix++) {
	int i;
	unsigned hix = rp->hist_ix_vec[wix]++;

	if (rp->hist_ix_vec[wix] >= rp->history) rp->hist_ix_vec[wix] = 0;

	i = (rp->history * wix) + hix;
	//VG_(umsg)("TRACE: Saving at wix=%u hix=%u -> i=%u\n", wix, hix, i);
	rp->access_matrix[i].call_stack = ec;
	rp->access_matrix[i].time_stamp = mh_logical_time;
	rp->access_matrix[i].data = data;

	start += rp->word_sz;
    }
}

static Int track_mem_access(Addr addr, SizeT size, Long data,
			    enum mh_track_type type)
{
    Addr start = addr;
    Addr end = addr + size;
    struct mh_region_t* rp = region_lookup_maxle(addr);
    Bool got_a_hit = 0;

    if (!rp || start >= rp->end) return 0;

    do {
	tl_assert(end > rp->start && start < rp->end);

	if (rp->enabled) {
	    switch (type) {
	    case MH_WRITE:
		if (rp->type & MH_WRITE) {
		    VG_(umsg)("Provoking SEGV: %u bytes WRITTEN to protected "
			      "region '%s' at addr %p at time %u:\n",
			      (unsigned)size, rp->name, (void*)addr,
			      mh_logical_time);
		    return 1; /* Crash! */
		}
		if (rp->type & MH_TRACK) {
		    report_store_in_block(rp, addr, size, data);
		}
		break;

	    case MH_READ:
		if (rp->type & MH_READ) {
		    VG_(umsg)("Provoking SEGV: %u bytes READ from protected "
			      "region '%s' at addr %p at time %u:\n",
			      (unsigned)size, rp->name, (void*)addr,
			      mh_logical_time);
		    return 1; /* Crash! */
		}
		break;

	    case MH_EXE:
		if (rp->type & MH_EXE) {
		    VG_(umsg)("Provoking SEGV: %u-byte instruction executed in protected "
			      "region '%s' at addr %p at time %u:\n",
			      (unsigned)size, rp->name, (void*)addr,
			      mh_logical_time);
		    return 1; /* Crash! */
		}
		break;

	    default:
		tl_assert2(0, "Invalid mem access type %x", type);
	    }
	    got_a_hit = 1;
	}
	if (end <= rp->end) break;

	rp = region_succ(rp);
    }while (rp && end > rp->start);

    if (got_a_hit) ++mh_logical_time;

    return 0; /* Ok */
}

#define track_REGPARM 2

VG_REGPARM(track_REGPARM)
static Int track_store(Addr addr, SizeT size, Long data)
{
    return track_mem_access(addr, size, data, MH_WRITE);
}

VG_REGPARM(track_REGPARM)
static Int track_load(Addr addr, SizeT size)
{
    return track_mem_access(addr, size, 0, MH_READ);
}

VG_REGPARM(track_REGPARM)
static Int track_exe(Addr addr, SizeT size)
{
    return track_mem_access(addr, size, 0, MH_EXE);
}

VG_REGPARM(track_REGPARM)
static Int track_cas(Addr addr, SizeT size, ULong expected, ULong data)
{
    ULong actual;
    MH_ASSERT2(fit_in_ubytes(expected, size), " expected=%llx size=%u", expected, (int)size);
    MH_ASSERT2(fit_in_ubytes(data, size), " data=%llx size=%u", data, (int)size);
    switch (size) {
    case 1: actual = *(UChar*)addr;  break;
    case 2: actual = *(UShort*)addr; break;
    case 4: actual = *(UInt*)addr;   break;
    case 8: actual = *(ULong*)addr;  break;
    default:
	tl_assert2(0, "CAS on %u-words not implemented", size);
    }
    return (actual == expected) ? track_store(addr, size, data) : 0;
}



#if VEX_HOST_WORDSIZE == 4
    #define IRConst_HWord IRConst_U32
#elif VEX_HOST_WORDSIZE == 8
    #define IRConst_HWord IRConst_U64
#else
    #error "VEX_HOST_WORDSIZE not set to 4 or 8"
#endif


static IRType size2itype(int size)
{
    IRType type;
    switch (size) {
    case 1: type = Ity_I8;   break;
    case 2: type = Ity_I16;  break;
    case 4: type = Ity_I32;  break;
    case 8: type = Ity_I64;  break;
    case 16: type = Ity_I128; break;
    default:
	tl_assert2(0, "Invalid integer size %u", size);
    }
    return type;
}


static IRExpr* expr2atom(IRSB* sb, IRExpr* e)
{
    if (!isIRAtom(e)) {
	Int size = sizeofIRType(typeOfIRExpr(sb->tyenv, e));
	IRTemp tmp = newIRTemp(sb->tyenv, size2itype(size));
	addStmtToIRSB(sb, IRStmt_WrTmp(tmp, e));
	e = IRExpr_RdTmp(tmp);
    }
    return e;
}

static IRExpr*
widen_to_U64(IRSB* sb, IRExpr* iexpr)
{
    Int size = sizeofIRType(typeOfIRExpr(sb->tyenv, iexpr));
    switch (size) {
    case 1: return IRExpr_Unop(Iop_8Uto64, iexpr);
    case 2: return IRExpr_Unop(Iop_16Uto64, iexpr);
    case 4: return IRExpr_Unop(Iop_32Uto64, iexpr);
    case 8: return iexpr;
    }
    return NULL;
}

static void emit_track_call(IRSB* sb, HWord ip,
			    void* fn, const char* fn_name, IRExpr** argv)
{
    IRExpr* cond_ex;
    IRTemp cond_tmp;
    IRTemp retval_tmp = newIRTemp(sb->tyenv, Ity_I32);
    IRDirty* di = unsafeIRDirty_1_N(retval_tmp,
				    track_REGPARM,
				    fn_name,
				    VG_(fnptr_to_fnentry)(fn),
				    argv);
    addStmtToIRSB(sb, IRStmt_Dirty(di));
    cond_ex = IRExpr_Unop(Iop_32to1, IRExpr_RdTmp(retval_tmp));
    cond_tmp = newIRTemp(sb->tyenv, Ity_I1);
    addStmtToIRSB(sb, IRStmt_WrTmp(cond_tmp, cond_ex));
    addStmtToIRSB(sb, IRStmt_Exit(IRExpr_RdTmp(cond_tmp), Ijk_SigSEGV,
				  IRConst_HWord(ip), sb->offsIP));
}

static
void addEvent_Dw(IRSB* sb, IRExpr* daddr, Int dsize,
		 IRExpr* expected, /* if CAS */
		 IRExpr* data, HWord ip)
{
    IRExpr**   argv;
    IRExpr*    data64 = NULL;
    IRExpr*    expd64;

    tl_assert(isIRAtom(daddr));
    tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);

    if (data) {
	data64 = widen_to_U64(sb, data);
    }
    if (!data64) {
	data64 = IRExpr_Const(IRConst_U64((ULong)0xdead));
    }

    if (expected) {
	/*  Emit:
	 *
	 *  if (track_cas(daddr, dsize, expd, data))
	 *      exit(SEGV);
	 */
	expd64 = widen_to_U64(sb, expected);
	tl_assert(expd64 != NULL);
	argv = mkIRExprVec_4(daddr, mkIRExpr_HWord(dsize),
			     expr2atom(sb, expd64), expr2atom(sb, data64));
	emit_track_call(sb, ip, track_cas, "track_cas", argv);
    }
    else {
	/*  Emit:
	 *
	 *  if (track_store(daddr, dsize, data))
	 *      exit(SEGV);
	 */
	argv = mkIRExprVec_3(daddr, mkIRExpr_HWord(dsize), expr2atom(sb, data64));
	emit_track_call(sb, ip, track_store, "track_store", argv);
    }
}

static void addEvent_Dr(IRSB* sb, IRExpr* daddr, Int dsize, HWord ip)
{
    IRExpr**   argv;

    tl_assert(isIRAtom(daddr));
    tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);

    /*  Emit:
     *
     *  if (track_load(daddr, dsize))
     *      exit(SEGV);
     */
    argv = mkIRExprVec_2(daddr, mkIRExpr_HWord(dsize));
    emit_track_call(sb, ip, track_load, "track_load", argv);
}

static void addEvent_Ir(IRSB* sb, HWord iaddr, UInt isize)
{
    IRExpr**   argv;

    tl_assert(isize >= 1 && isize <= MAX_DSIZE);

    /*  Emit:
     *
     *  if (track_exe(iaddr, isize))
     *      exit(SEGV);
     */
    argv = mkIRExprVec_2(mkIRExpr_HWord(iaddr), mkIRExpr_HWord(isize));
    emit_track_call(sb, iaddr, track_exe, "track_exe", argv);
}


/*------------------------------------------------------------*/
/*--- Basic tool functions                                 ---*/
/*------------------------------------------------------------*/

static void mh_post_clo_init(void)
{
}

static
IRSB* mh_instrument(VgCallbackClosure* closure,
		    IRSB* sbIn,
		    VexGuestLayout* layout,
		    VexGuestExtents* vge,
		    VexArchInfo* arch,
		    IRType gWordTy, IRType hWordTy)
{
    Int        i;
    IRSB*      sbOut;
    IRTypeEnv* tyenv = sbIn->tyenv;
    HWord      currIP = 0;

    if (gWordTy != hWordTy) {
	/* We don't currently support this case. */
	VG_(tool_panic)("host/guest word size mismatch");
    }

    /* Set up SB */
    sbOut = deepCopyIRSBExceptStmts(sbIn);

    // Copy verbatim any IR preamble preceding the first IMark
    i = 0;
    while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
	addStmtToIRSB(sbOut, sbIn->stmts[i]);
	i++;
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
	    break;

	case Ist_IMark:
	    /* Remember pointer to current hw instruction */
	    currIP = (HWord)st->Ist.IMark.addr;

	    if (enabled_tracking & MH_EXE) {
		addEvent_Ir(sbOut, (HWord)st->Ist.IMark.addr,
			    st->Ist.IMark.len);
	    }
	    break;

	case Ist_WrTmp:
	    if (enabled_tracking & MH_READ) {
		IRExpr* data = st->Ist.WrTmp.data;
		if (data->tag == Iex_Load) {
		    addEvent_Dr(sbOut, data->Iex.Load.addr,
				sizeofIRType(data->Iex.Load.ty), currIP);
		}
	    }
	    break;

	case Ist_Store:
	    if (enabled_tracking & MH_WRITE) {
		IRExpr* data  = st->Ist.Store.data;
		addEvent_Dw(sbOut, st->Ist.Store.addr,
			    sizeofIRType(typeOfIRExpr(tyenv, data)),
			    NULL,
			    data,
			    currIP);
	    }
	    break;

	case Ist_StoreG:
	    tl_assert(!"Tell Sverker he forgot to implement Ist_StoreGTo.");
	    break;

	case Ist_LoadG:
	    tl_assert(!"Tell Sverker he forgot to implement Ist_LoadG.");
	    break;

	case Ist_Dirty: {
	    Int      dsize;
	    IRDirty* d = st->Ist.Dirty.details;
	    if (d->mFx != Ifx_None) {
		// This dirty helper accesses memory.  Collect the details.
		tl_assert(d->mAddr != NULL);
		tl_assert(d->mSize != 0);
		dsize = d->mSize;
		if ((enabled_tracking & MH_READ)
		    && (d->mFx == Ifx_Read || d->mFx == Ifx_Modify))
		{
		    addEvent_Dr(sbOut, d->mAddr, dsize, currIP);
		}
		if ((enabled_tracking & MH_WRITE)
		    && (d->mFx == Ifx_Write || d->mFx == Ifx_Modify))
		{
		    addEvent_Dw(sbOut, d->mAddr, dsize, NULL, NULL, currIP);
		}
	    }
	    else {
		tl_assert(d->mAddr == NULL);
		tl_assert(d->mSize == 0);
	    }
	    break;
	}

	case Ist_CAS: {
	    Int    dataSize;
	    IRCAS*  cas = st->Ist.CAS.details;
	    IRExpr* data = cas->dataLo;
	    IRExpr* expd = cas->expdLo;
	    IROp mergeOp;
	    tl_assert(cas->addr != NULL);
	    tl_assert(cas->dataLo != NULL);
	    dataSize = sizeofIRType(typeOfIRExpr(tyenv, cas->dataLo));
	    tl_assert(dataSize == sizeofIRType(typeOfIRExpr(tyenv, cas->expdLo)));

	    if (cas->dataHi) { /* a doubleword-CAS */
		dataSize *= 2;
	    }
	    if (enabled_tracking & MH_READ) {
		addEvent_Dr(sbOut, cas->addr, dataSize, currIP);
	    }
	    if (enabled_tracking & MH_WRITE) {
		if (cas->dataHi) {  /* a doubleword-CAS */
		    switch (dataSize) {
		    case  2:
			mergeOp = Iop_8HLto16;   break;
		    case  4:
			mergeOp = Iop_16HLto32;  break;
		    case  8:
			mergeOp = Iop_32HLto64;  break;
		    default:
			tl_assert2(0, "doubleword CAS instruction with size %u not implemented", dataSize);
		    }
		    data = IRExpr_Binop(mergeOp, cas->dataHi, cas->dataLo);
		    expd = IRExpr_Binop(mergeOp, cas->expdHi, cas->expdLo);
		}
		addEvent_Dw(sbOut, cas->addr, dataSize, expd, data, currIP);
	    }
	    break;
	}

	case Ist_LLSC: {
	    IRType dataTy;
	    if (st->Ist.LLSC.storedata == NULL) {
		/* LL */
		if (enabled_tracking & MH_READ) {
		    dataTy = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
		    addEvent_Dr(sbOut, st->Ist.LLSC.addr,
				sizeofIRType(dataTy), currIP);
		}
	    }
	    else if (enabled_tracking & MH_WRITE) {
		/* SC */
		dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
		/* Todo:
		 * Do real *conditional* store as we do for CAS above
		 * This is trickier to do as we don't know until *after* the
		 * original LLSC instruction if the store really happened.
		 */
		addEvent_Dw(sbOut, st->Ist.LLSC.addr, sizeofIRType(dataTy),
			    NULL, st->Ist.LLSC.storedata, currIP);
	    }
	    break;
	}

	case Ist_Exit:
	    break;

	default:
	    tl_assert(0);
	}
	addStmtToIRSB(sbOut, st);      // Original statement
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
    struct mh_region_t* rp;
    const unsigned nwords = (size + word_sz - 1) / word_sz;
    const unsigned sizeof_hist_ix_vec = nwords * sizeof(*rp->hist_ix_vec);
    unsigned i;
    const unsigned matrix_offset = align_up(sizeof(void*),
					    (sizeof(struct mh_region_t)
					     + sizeof_hist_ix_vec));

    if (!(enabled_tracking & MH_WRITE))
	return;

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: Tracking %u-words from %p to %p with history %u\n",
		  word_sz, (void*)addr, (void*)(addr + size), history);
    }

    rp = VG_(malloc)("track_mem_write",
		     matrix_offset + history * nwords * sizeof(struct mh_mem_access_t));
    rp->start = addr;
    rp->end = addr + size;
    rp->name = name;
    rp->birth_time_stamp = mh_logical_time++;
    rp->enabled = True;
    rp->type = MH_TRACK;
    rp->word_sz = word_sz;
    rp->nwords = nwords;
    rp->history = history;
    rp->access_matrix = (struct mh_mem_access_t*)((char*)rp + matrix_offset);
    tl_assert((char*)&rp->hist_ix_vec[nwords] <= (char*)rp->access_matrix);
    for (i = 0; i < nwords; i++) {
	rp->hist_ix_vec[i] = 0;
    }
    for (i = 0; i < history * nwords; i++) {
	rp->access_matrix[i].call_stack = NULL;
	rp->access_matrix[i].time_stamp = 0;
    }

    insert_nonoverlapping(rp);
}

static void untrack_mem_write(Addr addr, SizeT size)
{
    Addr end = addr + size;
    struct mh_region_t* rp = region_lookup_maxle(addr);

    tl_assert2(rp && addr == rp->start && end == rp->end,
	       "Could not find region to remove [%p -> %p]", addr, end);
    tl_assert(rp->type & MH_TRACK);

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: Untracking '%s' from %p to %p\n",
		  rp->name, (void*)addr, (void*)(addr + size));
    }
    rp->type &= ~MH_TRACK;

    if (!rp->type) {
	region_remove(rp);
	VG_(free)(rp);
    }
}

static void track_able(Addr addr, SizeT size, Bool enabled)
{
    Addr end = addr + size;
    struct mh_region_t* rp = region_lookup_maxle(addr);

    if (!rp)
	return;

    tl_assert2(addr == rp->start && end == rp->end,
	       "Could not find region to %sable", enabled ? "en" : "dis");

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: %sable '%s' from %p to %p\n",
		  (enabled ? "En" : "Dis"),
		  rp->name, (void*)addr, (void*)(addr + size));
    }
    rp->enabled = enabled;
}


static struct mh_region_t* new_region(Addr start, Addr end,
				      const char* name,
				      unsigned flags)
{
    struct mh_region_t* rp;
    rp = VG_(malloc)("set_mem_readonly", sizeof(struct mh_region_t));
    rp->start = start;
    rp->end = end;
    rp->name = name;
    rp->birth_time_stamp = mh_logical_time++;
    rp->enabled = True;
    rp->type = flags;
    insert_nonoverlapping(rp);
    return rp;
}

static void set_mem_flags(Addr start, SizeT size, const char* name,
			  enum mh_track_type flags)
{
    Addr end = start + size;
    struct mh_region_t* rp;
    enum {VOID_AT_START, REGION_AT_START } state;

    tl_assert(flags & (MH_WRITE | MH_READ | MH_EXE));
    tl_assert(!(flags & ~(MH_WRITE | MH_READ | MH_EXE)));

    flags &= enabled_tracking;  /* ignore flags that we do not track */
    if (!flags)
	return;

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: Set protection %s for '%s' from %p to %p\n",
		  prot_txt(flags), name, (void*)start, (void*)end);
    }

    rp = region_lookup_maxle(start);
    if (rp) {
	if (rp->end < start
	    || (rp->end == start && rp->type != flags)) {
	    state = VOID_AT_START;
	    rp = region_succ(rp);
	}
	else {
	    state = REGION_AT_START;
	}
    }
    else {
	state = VOID_AT_START;
	rp = region_lookup_ming(start);
    }

    while (start < end) {
	switch (state) {
	case VOID_AT_START:
	    tl_assert(!rp || rp->start > start);
	    if (!rp || rp->start > end) {
		new_region(start, end, name, flags);
		return;
	    }
	    else if (rp->type == flags) {
		/* extent start of region */
		rp->start = start;
	    }
	    else {
		new_region(start, rp->start, name, flags);
		start = rp->start;
	    }
	    state = REGION_AT_START;
	    break;

	case REGION_AT_START:
	    tl_assert(rp && rp->start <= start && rp->end >= start);
	    if (rp->end > end) {
		tl_assert(rp->type & flags);
		return;
	    }
	    if (rp->type == flags) {
		struct mh_region_t* succ = region_succ(rp);
		if (!succ || succ->start > end) {
		    rp->end = end;
		    return;
		}
		if (succ->type == flags) {
		    Addr succ_end = succ->end;
		    region_remove(succ);
		    VG_(free)(succ);
		    rp->end = succ_end;
		}
		else {
		    rp->end = succ->start;
		    rp = succ;
		    start = rp->start;
		}
		/*state = REGION_AT_START */
	    }
	    else {
		rp->type |= flags;
		if (rp->end == end) return;
		start = rp->end;
		rp = region_succ(rp);
		state = (!rp || rp->start > start) ? VOID_AT_START : REGION_AT_START;
	    }
	    break;
	}
    }
}

static void clear_mem_flags(Addr start, SizeT size, enum mh_track_type flags)
{
    Addr end = start + size;
    struct mh_region_t* rp, * pred = NULL;

    tl_assert(flags & (MH_WRITE | MH_READ));
    tl_assert(!(flags & MH_TRACK));

    flags &= enabled_tracking;  /* ignore flags that we do not track */
    if (!flags)
	return;

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: Clear protection %s from %p to %p\n",
		  prot_txt(flags), (void*)start, (void*)end);
    }

    rp = region_lookup_maxle(start);
    if (rp) {
	if (rp->start < start) {
	    if (rp->end > start) {
		tl_assert(!(rp->type & MH_TRACK));

		if (rp->type & flags) {
		    Addr old_end = rp->end;
		    enum mh_track_type new_flags = rp->type & ~flags;
		    rp->end = start;
		    if (new_flags) {
			rp = new_region(start, old_end, rp->name, new_flags);
		    }
		}
	    }
	    pred = rp;
	    rp = region_succ(rp);
	}
    }
    else {
	rp = region_lookup_ming(start);
    }

    while (rp && rp->start < end) {
	if (rp->type & flags) {
	    enum mh_track_type new_flags = rp->type & ~flags;
	    if (rp->end > end) {
		Addr old_end = rp->end;

		tl_assert(!(rp->type & MH_TRACK));
		if (new_flags) { /* split region */
		    rp->type = new_flags;
		    rp->end = end;
		    new_region(end, old_end, rp->name, new_flags);
		}
		else { /* shrink region */
		    rp->start = end;
		    return;
		}
	    }
	    else if (new_flags) {
		rp->type = new_flags;
	    }
	    else { /* remove region */
		pred =  rp;
		rp = region_succ(rp);
		region_remove(pred);
		pred = NULL;
		continue;
	    }
	}
	if (pred && pred->end == rp->start
	    && pred->type == rp->type && !(rp->type & MH_TRACK)) { /* merge regions */
	    Addr pred_start = pred->start;
	    region_remove(pred);
	    rp->start = pred_start;
	}
	pred = rp;
	rp = region_succ(rp);
    }
}


/*------------------------------------------------------------*/
/*--- Client requests                                      ---*/
/*------------------------------------------------------------*/

static Bool mh_handle_client_request(ThreadId tid, UWord* arg, UWord* ret)
{
    if (!VG_IS_TOOL_USERREQ('M', 'H', arg[0])) {
	return False;
    }

    switch (arg[0]) {
    case VG_USERREQ__TRACK_MEM_WRITE:
	track_mem_write(arg[1], arg[2], arg[3], arg[4], (char*)arg[5]);
	*ret = -1;
	break;
    case VG_USERREQ__UNTRACK_MEM_WRITE:
	untrack_mem_write(arg[1], arg[2]);
	*ret = -1;
	break;

    case VG_USERREQ__TRACK_ENABLE:
	track_able(arg[1], arg[2], 1);
	*ret = -1;
	break;

    case VG_USERREQ__TRACK_DISABLE:
	track_able(arg[1], arg[2], 0);
	*ret = -1;
	break;

    case VG_USERREQ__SET_PROTECTION:
	set_mem_flags(arg[1], arg[2], (char*)arg[3],
		      (enum mh_track_type)arg[4]);
	*ret = -1;
	break;

    case VG_USERREQ__CLEAR_PROTECTION:
	clear_mem_flags(arg[1], arg[2], (enum mh_track_type)arg[3]);
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
#if VEX_HOST_WORDSIZE == 8
    case sizeof(HWord):
	VG_(umsg)("%p", (void*)ap->data);
	break;
#endif
    case sizeof(int):
	VG_(umsg)("%#x", (int)ap->data); break;
    case sizeof(short):
	VG_(umsg)("%#x", (int)(short)ap->data); break;
    case sizeof(char):
	VG_(umsg)("%#x", (int)(char)ap->data); break;
    default:
	VG_(umsg)("(?)"); break;
    }
}


static void mh_fini(Int exitcode)
{
    struct mh_region_t* rp = region_min();

    for (rp = region_min(); rp; rp = region_succ(rp)) {
	if (rp->type & MH_TRACK) {
	    unsigned wix = 0; /* word index */
	    Addr addr = rp->start;
	    VG_(umsg)("Memhist tracking '%s' from %p to %p with word size %u "
		      "and history %u created at time %u.\n", rp->name,
		      (void*)rp->start, (void*)rp->end, rp->word_sz,
		      rp->history, rp->birth_time_stamp);
	    for (addr = rp->start; addr < rp->end; wix++, addr += rp->word_sz) {
		unsigned h;
		int hist_ix = rp->hist_ix_vec[wix] - 1;

		for (h = 0; h < rp->history; h++, hist_ix--) {
		    struct mh_mem_access_t* ap;

		    if (hist_ix < 0) hist_ix = rp->history - 1;

		    //VG_(umsg)("TRACE: Reading at ix=%u\n", wix*rp->history + hist_ix);
		    ap = &rp->access_matrix[wix * rp->history + hist_ix];
		    if (ap->call_stack) {
			if (!h) {
			    VG_(umsg)("%u-bytes ", rp->word_sz);
			    print_word(rp->word_sz, ap);
			    VG_(umsg)(" written to address %p at time %u:\n",
				      (void*)addr, ap->time_stamp);
			}
			else {
			    VG_(umsg)("       AND ");
			    print_word(rp->word_sz, ap);
			    VG_(umsg)(" written at time %u:\n", ap->time_stamp);
			}
			VG_(pp_ExeContext)(ap->call_stack);
		    }
		    else {
			if (!h) VG_(umsg)("%u-bytes at %p not written.\n", rp->word_sz, (void*)addr);
			break;
		    }
		}
	    }
	}
	if (rp->type & MH_WRITE) {
	    VG_(umsg)("Region '%s' set as %s from %p to %p.\n",
		      rp->name, prot_txt(rp->type),
		      (void*)rp->start, (void*)rp->end);
	}
    }
}

static void mh_pre_clo_init(void)
{
    rb_tree_init(&region_tree, region_cmp, region_cmp_key, region_print);

    VG_(details_name)("Memhist");
    VG_(details_version)(NULL);
    VG_(details_description)("Sverker's Valgrind tool for tracking memory access history");
    VG_(details_copyright_author)(
	"Copyright (C) 2014, and GNU GPL'd, by Sverker Eriksson.");
    VG_(details_bug_reports_to)(VG_BUGS_TO);
    VG_(details_avg_translation_sizeB)(200);

    VG_(basic_tool_funcs)(mh_post_clo_init,
			  mh_instrument,
			  mh_fini);
    VG_(needs_command_line_options)(mh_process_cmd_line_option,
				    mh_print_usage,
				    mh_print_debug_usage);
    VG_(needs_client_requests)(mh_handle_client_request);
}

VG_DETERMINE_INTERFACE_VERSION(mh_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                mh_main.c ---*/
/*--------------------------------------------------------------------*/
