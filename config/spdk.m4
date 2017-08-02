# Userspace Software iWARP library for SPDK
#
# Authors: Patrick MacArthur <patrick@patrickmacarthur.net>
#
# Copyright (c) 2016-2018, University of New Hampshire
#
# This software is available to you under a choice of one of two
# licenses.  You may choose to be licensed under the terms of the GNU
# General Public License (GPL) Version 2, available from the file
# COPYING in the main directory of this source tree, or the
# BSD license below:
#
#   Redistribution and use in source and binary forms, with or
#   without modification, are permitted provided that the following
#   conditions are met:
#
#   - Redistributions of source code must retain the above copyright notice,
#     this list of conditions and the following disclaimer.
#
#   - Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#
#   - Neither the name of IBM nor the names of its contributors may be
#     used to endorse or promote products derived from this software without
#     specific prior written permission.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# URDMA_LIB_SPDK(VERSION, [PREFIX])
# -------------------------
# Checks that we have SPDK. Note that this macro will abort if SPDK is
# not found; fixing this would take more effort than it is worth.
AC_DEFUN([URDMA_LIB_SPDK],
[
AS_IF([test x$2 != x],
     [[SPDK_CPPFLAGS=-I$2/include
     SPDK_LDFLAGS=-L$2/lib]])

AC_SUBST([SPDK_CPPFLAGS])
AC_SUBST([SPDK_LDFLAGS])

old_CPPFLAGS="${CPPFLAGS}"
old_LDFLAGS="${LDFLAGS}"
old_LIBS="${LIBS}"
CPPFLAGS="${CPPFLAGS} ${DPDK_CPPFLAGS} ${SPDK_CPPFLAGS}"
LDFLAGS="${LDFLAGS} ${DPDK_LDFLAGS} ${SPDK_LDFLAGS}"
LIBS="${DPDK_LIBS} ${LIBS}"

AC_SEARCH_LIBS([spdk_env_init], [spdk_env_dpdk], [],
	       [AC_MSG_ERROR([SPDK requested but DPDK environment shim not found])])
AC_SEARCH_LIBS([spdk_nvme_probe], [spdk], [],
	       [AC_MSG_ERROR([SPDK requested but not found])])


AC_CHECK_HEADERS([spdk/version.h], [],
		 [AC_MSG_ERROR([urdma requires SPDK >= $1])])

AC_CACHE_CHECK([for SPDK release version m4_if(m4_eval([$# >= 1]), 1, [at least $1])],
       [urdma_cv_librelver_spdk],
       [[dummy=if$$
	cat <<_URDMA_EOF > $dummy.c
#include <spdk/version.h>
#if defined(SPDK_VERSION_MAJOR) && defined(SPDK_VERSION_MINOR)
spdk_major_version SPDK_VERSION_MAJOR
spdk_minor_version SPDK_VERSION_MINOR
#else
undefined
#endif
_URDMA_EOF
	_dpdk_out=`$CC $CPPFLAGS -E $dummy.c 2> /dev/null | tail -n 2 >$dummy.i`
	_dpdk_major=`grep '^spdk_major_version' $dummy.i | cut -d' ' -f2`
	_dpdk_minor=`grep '^spdk_minor_version' $dummy.i | cut -d' ' -f2`
	urdma_cv_librelver_spdk=`printf "%d.%02d" ${_dpdk_major} ${_dpdk_minor}`
	rm -f $dummy.c $dummy.i]])
m4_if(m4_eval([$# >= 1]), 1,
[case $urdma_cv_librelver_spdk in #(
undefined) AC_MSG_ERROR([cannot determine SPDK version, urdma requires SPDK >= $1]) ;; #(
*) AX_COMPARE_VERSION([$1], [le], [$urdma_cv_librelver_spdk], [],
		      [AC_MSG_ERROR([urdma requires SPDK >= $1; found SPDK v$])])
esac
])

CFLAGS="${old_CFLAGS}"
CPPFLAGS="${old_CPPFLAGS}"
LDFLAGS="${old_LDFLAGS}"
AC_SUBST([SPDK_LIBS], [${LIBS%${DPDK_LIBS} ${old_LIBS}}])
LIBS=${old_LIBS}
]) # URDMA_LIB_SPDK

# _WITH_SPDK_FLAGS(PROGRAM)
# -------------------------
# Runs the m4 code inside with SPDK_LIBS, SPDK_CFLAGS, SPDK_CPPFLAGS,
# and SPDK_LDFLAGS set to their respective variables first and restores
# them afterward.
AC_DEFUN([_WITH_SPDK_FLAGS],
[
_spdk_old_CFLAGS="${CFLAGS}"
_spdk_old_CPPFLAGS="${CPPFLAGS}"
_spdk_old_LDFLAGS="${LDFLAGS}"
_spdk_old_LIBS="${LIBS}"

CFLAGS="${CFLAGS} ${SPDK_CFLAGS}"
CPPFLAGS="${CPPFLAGS} ${SPDK_CPPFLAGS}"
LDFLAGS="${LDFLAGS} ${SPDK_LDFLAGS}"
LIBS="${SPDK_LIBS} ${LIBS}"

$1

CFLAGS="${_spdk_old_CFLAGS}"
CPPFLAGS="${_spdk_old_CPPFLAGS}"
LDFLAGS="${_spdk_old_LDFLAGS}"
LIBS=${_spdk_old_LIBS}
]) _WITH_SPDK_FLAGS

# SPDK_CHECK_FUNC(FUNCTION, [ACTION-IF-FOUND], [ACTION-IF-NOT-FOUND])
# -------------------------------------------------------------------
# Like AC_CHECK_FUNC, but add SPDK_LIBS, SPDK_CFLAGS, SPDK_CPPFLAGS, and
# SPDK_LDFLAGS to their respective variables first and restore them
# afterward.
AC_DEFUN([SPDK_CHECK_FUNCS], [
_WITH_SPDK_FLAGS([
m4_case([$#],
	[1], [AC_CHECK_FUNCS([$1])],
	[2], [AC_CHECK_FUNCS([$1], [$2])],
	[3], [AC_CHECK_FUNCS([$1], [$2], [$3])],
	[m4_fatal([SPDK_CHECK_FUNCS requires 1-3 arguments])])
])]) # SPDK_CHECK_FUNCS

# SPDK_CHECK_HEADERS(HEADER-FILE, [ACTION-IF-FOUND], [ACTION-IF-NOT-FOUND], [INCLUDES])
# -------------------------------------------------------------------------------------
# Like AC_CHECK_HEADERS, but add SPDK_LIBS, SPDK_CFLAGS, SPDK_CPPFLAGS, and
# SPDK_LDFLAGS to their respective variables first and restore them
# afterward.
AC_DEFUN([SPDK_CHECK_HEADERS], [
_WITH_SPDK_FLAGS([
m4_case([$#],
	[1], [AC_CHECK_HEADERS([$1])],
	[2], [AC_CHECK_HEADERS([$1], [$2])],
	[3], [AC_CHECK_HEADERS([$1], [$2], [$3])],
	[4], [AC_CHECK_HEADERS([$1], [$2], [$3], [$4])],
	[m4_fatal([SPDK_CHECK_HEADERS requires 1-4 arguments])])
])]) # SPDK_CHECK_HEADERS
