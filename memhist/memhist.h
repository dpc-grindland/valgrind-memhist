#ifndef __MEMHIST_H
#define __MEMHIST_H


/* This file is for inclusion into client (your!) code.

   You can use these macros to trace memory writes
   inside your own programs.

   See comment near the top of valgrind.h on how to use them.
*/

#include "valgrind.h"

typedef
   enum {
      VG_USERREQ__TRACK_MEM_WRITE = VG_USERREQ_TOOL_BASE('M','H'),
      VG_USERREQ__UNTRACK_MEM_WRITE

   } Vg_MemHistClientRequest;

#define VALGRIND_TRACK_MEM_WRITE(_qzz_addr,_qzz_len, _qzz_granularity, _qzz_history)	\
    VALGRIND_DO_CLIENT_REQUEST_EXPR(0 /* default return */,     \
                            VG_USERREQ__TRACK_MEM_WRITE,        \
			    (_qzz_addr), (_qzz_len), (_qzz_granularity), (_qzz_history), 0)

#define VALGRIND_UNTRACK_MEM_WRITE(_qzz_addr,_qzz_len)            \
    VALGRIND_DO_CLIENT_REQUEST_EXPR(0 /* default return */,     \
                            VG_USERREQ__UNTRACK_MEM_WRITE,        \
                            (_qzz_addr), (_qzz_len), 0, 0, 0)

#endif // __MEMHIST_H
