project(jemalloc C)
cmake_minimum_required(VERSION 2.6)

option(DEBUG_MEMORY "Debug memory." OFF)

if(DEBUG_MEMORY)
 add_definitions(-DJEMALLOC_DEBUG)
 set(JEMALLOC_DEBUG 1)
endif(DEBUG_MEMORY)

if("${CMAKE_C_COMPILER_ID}" MATCHES "clang" OR "${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
 set(CLANG_COMPILER 1)
endif("${CMAKE_C_COMPILER_ID}" MATCHES "clang" OR "${CMAKE_C_COMPILER_ID}" MATCHES "Clang")

if(CMAKE_COMPILER_IS_GNUCC OR MINGW OR CYGWIN OR CLANG_COMPILER)
 set(CMAKE_C_FLAGS "-std=gnu99 -pipe -g -fvisibility=hidden")
 set(CMAKE_C_FLAGS_RELEASE "-O3 -funroll-loops")
 set(CMAKE_C_FLAGS_DEBUG "-ggdb -Wall")
 set(JEMALLOC_HAVE_ATTR 1)
endif(CMAKE_COMPILER_IS_GNUCC OR MINGW OR CYGWIN OR CLANG_COMPILER)

if(CLANG_COMPILER)
 set(CMAKE_C_FLAGS "-Wno-unknown-warning-option ${CMAKE_C_FLAGS}")
endif(CLANG_COMPILER)

if(MSVC)
 set(CMAKE_C_FLAGS "")
 set(CMAKE_C_FLAGS_RELEASE "/MD /O2 /Z7")
 set(CMAKE_C_FLAGS_DEBUG "/MDd /Od /Zi")
endif(MSVC)

set(JEMALLOC_C_SRCS
src/jemalloc.c src/arena.c src/atomic.c
src/base.c src/bitmap.c src/chunk.c src/chunk_dss.c
src/chunk_mmap.c src/ckh.c src/ctl.c
src/extent.c src/hash.c src/huge.c
src/mb.c src/mutex.c src/prof.c
src/quarantine.c src/rtree.c src/stats.c
src/tcache.c src/util.c src/tsd.c)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
 set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
 add_definitions(-D_GNU_SOURCE)
 set(JEMALLOC_HAS_ALLOCA_H 1)
 set(JEMALLOC_PURGE_MADVISE_DONTNEED 1)
 set(JEMALLOC_THREADED_INIT 1)
endif(CMAKE_SYSTEM_NAME MATCHES "Linux")

if(CMAKE_SYSTEM_NAME MATCHES ".*BSD")
 set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
 set(JEMALLOC_PURGE_MADVISE_FREE 1)
endif(CMAKE_SYSTEM_NAME MATCHES ".*BSD")

if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
 set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
 set(JEMALLOC_C_SRCS ${JEMALLOC_C_SRCS} src/zone.c)
 set(JEMALLOC_ZONE 1)
 set(JEMALLOC_PURGE_MADVISE_FREE 1)
endif(CMAKE_SYSTEM_NAME MATCHES "Darwin")

if(WIN32 OR MINGW)
 set(PLATFORM_EXE_SUFFIX ".exe")
endif(WIN32 OR MINGW)

macro(JEMALLOC_LG_SIZEOF CTYPE VARIABLE)
 include(CheckTypeSize)
 set(CMAKE_REQUIRED_INCLUDES stdint.h)
 check_type_size(${CTYPE} TYPE_SIZEOF_${VARIABLE})
 if(TYPE_SIZEOF_${VARIABLE} EQUAL 1)
  set(${VARIABLE} 0)
 elseif(TYPE_SIZEOF_${VARIABLE} EQUAL 2)
  set(${VARIABLE} 1)
 elseif(TYPE_SIZEOF_${VARIABLE} EQUAL 4)
  set(${VARIABLE} 2)
 elseif(TYPE_SIZEOF_${VARIABLE} EQUAL 8)
  set(${VARIABLE} 3)
 elseif(TYPE_SIZEOF_${VARIABLE} EQUAL 16)
  set(${VARIABLE} 4)
 else(TYPE_SIZEOF_${VARIABLE} EQUAL 1)
	message(FATAL_ERROR "Unrecognized type " ${CTYPE})
 endif(TYPE_SIZEOF_${VARIABLE} EQUAL 1)
endmacro(JEMALLOC_LG_SIZEOF)

JEMALLOC_LG_SIZEOF("void *" LG_SIZEOF_PTR)
JEMALLOC_LG_SIZEOF("int" LG_SIZEOF_INT)
JEMALLOC_LG_SIZEOF("long" LG_SIZEOF_LONG)
JEMALLOC_LG_SIZEOF("intmax_t" LG_SIZEOF_INTMAX_T)

include(CheckFunctionExists)
set(CMAKE_REQUIRED_INCLUDES stdlib.h)
check_function_exists(valloc JEMALLOC_OVERRIDE_VALLOC)
set(CMAKE_REQUIRED_INCLUDES malloc.h)
check_function_exists(memalign JEMALLOC_OVERRIDE_MEMALIGN)
set(jemalloc_version 3)
set(jemalloc_version_major 6)
set(jemalloc_version_minor 0)
set(jemalloc_version_bugfix 0)
set(jemalloc_version_nrev 0)
set(private_namespace "je_")
include(CheckCSourceCompiles)
check_c_source_compiles("int main (void) { __asm__ volatile(\"pause\"); return 0; }" CPU_SPINWAIT)
check_c_source_compiles("#include <sys/types.h>
#include <machine/atomic.h>
#include <inttypes.h>
int main(void){
	{
		uint32_t x32 = 0;
		volatile uint32_t *x32p = &x32;
		atomic_fetchadd_32(x32p, 1);
	}
	{
		unsigned long xlong = 0;
		volatile unsigned long *xlongp = &xlong;
		atomic_fetchadd_long(xlongp, 1);
	}
	return 0;
}" JEMALLOC_ATOMIC9)
check_c_source_compiles("#include <libkern/OSAtomic.h>
#include <inttypes.h>
int main(void){
	{
		int32_t x32 = 0;
		volatile int32_t *x32p = &x32;
		OSAtomicAdd32(1, x32p);
	}
	{
		int64_t x64 = 0;
		volatile int64_t *x64p = &x64;
		OSAtomicAdd64(1, x64p);
	}
	return 0;
}" JEMALLOC_OSATOMIC)
check_c_source_compiles("#include <stdint.h>
int main(void){
	#ifndef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4
	{
		uint32_t x = 0;
		__sync_add_and_fetch(&x, 42);
		__sync_sub_and_fetch(&x, 1);
	}
	#else
		#error no need to force
	#endif
	return 0;
}" JE_FORCE_SYNC_COMPARE_AND_SWAP_4)
check_c_source_compiles("#include <stdint.h>
int main(void){
	#ifndef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8
	{
		uint64_t x = 0;
		__sync_add_and_fetch(&x, 42);
		__sync_sub_and_fetch(&x, 1);
	}
	#else
		#error no need to force
	#endif
	return 0;
}" JE_FORCE_SYNC_COMPARE_AND_SWAP_8)
check_c_source_compiles("#include <libkern/OSAtomic.h>
#include <inttypes.h>
int main(void){
	OSSpinLock lock = 0;
	OSSpinLockLock(&lock);
	OSSpinLockUnlock(&lock);
	return 0;
}" JEMALLOC_OSSPIN)
set(JEMALLOC_TCACHE 1)
check_function_exists(_malloc_thread_cleanup JEMALLOC_MALLOC_THREAD_CLEANUP)
check_function_exists(_pthread_mutex_init_calloc_cb JEMALLOC_MUTEX_INIT_CB)
check_function_exists(sbrk JEMALLOC_HAVE_SBRK)
if (${JEMALLOC_HAVE_SBRK})
 set(JEMALLOC_DSS 1)
 message(STATUS "DSS enabled")
endif (${JEMALLOC_HAVE_SBRK})
check_c_source_compiles("int main(void){
	static __thread int __attribute__((tls_model(\"initial-exec\"))) foo;
	foo = 0;
	return 0;
}" JEMALLOC_TLS_MODEL)
if (${JEMALLOC_TLS_MODEL})
 set(JEMALLOC_TLS_MODEL "__attribute__((tls_model(\"initial-exec\")))")
endif (${JEMALLOC_TLS_MODEL})
set(JEMALLOC_CC_SILENCE 1)
if (CMAKE_CROSSCOMPILING)
 set(STATIC_PAGE_SHIFT 12) # set default
else (CMAKE_CROSSCOMPILING)
 try_run(STATIC_PAGE_SHIFT STATIC_PAGE_SHIFT ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests/printpagesize.c ARGS ${CMAKE_CURRENT_SOURCE_DIR}/ps.out)
 file(READ ps.out STATIC_PAGE_SHIFT)
 file(REMOVE ps.out)
 string(STRIP ${STATIC_PAGE_SHIFT} STATIC_PAGE_SHIFT)
endif (CMAKE_CROSSCOMPILING)

configure_file(include/jemalloc/jemalloc_defs.h.in include/jemalloc/jemalloc_defs.h)
configure_file(include/jemalloc/jemalloc_macros.h.in include/jemalloc/jemalloc_macros.h)
configure_file(include/jemalloc/jemalloc_protos.h.in include/jemalloc/jemalloc_protos.h)
configure_file(include/jemalloc/internal/jemalloc_internal.h.in include/jemalloc/internal/jemalloc_internal.h)
configure_file(include/jemalloc/internal/jemalloc_internal_defs.h.in include/jemalloc/internal/jemalloc_internal_defs.h)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(jemalloc STATIC ${JEMALLOC_C_SRCS})

if (NOT CMAKE_CROSSCOMPILING)
 enable_testing()
 add_executable(test_malloc tests/malloc.c)
 add_executable(test_calloc tests/calloc.c)
 add_executable(test_realloc tests/realloc.c)
 target_link_libraries(test_malloc jemalloc)
 target_link_libraries(test_calloc jemalloc)
 target_link_libraries(test_realloc jemalloc)
 add_test(test_malloc ${CMAKE_CURRENT_BINARY_DIR}/test_malloc${PLATFORM_EXE_SUFFIX})
 add_test(test_calloc ${CMAKE_CURRENT_BINARY_DIR}/test_calloc${PLATFORM_EXE_SUFFIX})
 add_test(test_realloc ${CMAKE_CURRENT_BINARY_DIR}/test_realloc${PLATFORM_EXE_SUFFIX})
 add_custom_target(tests ALL ctest)
endif(NOT CMAKE_CROSSCOMPILING)