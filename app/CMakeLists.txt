##################################################################
#
# Bundle Protocol Library (BPLib) Test App CMake build recipe
#
# This file provides various test executables, including bpcat, that
# can be used to initiate or terminate BP/DTN traffic from the
# command line on a simulated node.
#
##################################################################

cmake_minimum_required(VERSION 3.5)
project(BPLIB_TEST_APP C)

# Sanity check: BPLib test app assumes a POSIX system
# and uses pthread calls.  If not POSIX, then this tool
# will not build.
if (NOT BPLIB_INCLUDE_POSIX)
  message(FATAL_ERROR "BPLib Test applications require POSIX")
endif()

set(BPAPP_COMPILE_FEATURES "c_std_99")
set(BPAPP_COMPILE_OPTIONS "$<$<C_COMPILER_ID:GNU>:-Wall;-Werror>")
set(BPAPP_LINK_LIBRARIES bplib)

add_executable(bpcat bpcat.c)
add_executable(bptest bptest.c)
add_executable(rbtest rbtest.c)

# compile this app as c99 (but this does not impose the same requirement on other users)
target_compile_features(bpcat PRIVATE ${BPAPP_COMPILE_FEATURES})
target_compile_features(bptest PRIVATE ${BPAPP_COMPILE_FEATURES})
target_compile_features(rbtest PRIVATE ${BPAPP_COMPILE_FEATURES})

# If using GNU GCC, then also enable full warning reporting
target_compile_options(bpcat PRIVATE ${BPAPP_COMPILE_OPTIONS})
target_compile_options(bptest PRIVATE ${BPAPP_COMPILE_OPTIONS})
target_compile_options(rbtest PRIVATE ${BPAPP_COMPILE_OPTIONS})

# Low level test apps may include "private" headers, whereas higher level tests should not
target_include_directories(bptest PRIVATE ${BPLIB_PRIVATE_INCLUDE_DIRS})
target_include_directories(rbtest PRIVATE ${BPLIB_PRIVATE_INCLUDE_DIRS})

# link with bplib
target_link_libraries(bpcat ${BPAPP_LINK_LIBRARIES})
target_link_libraries(bptest ${BPAPP_LINK_LIBRARIES})
target_link_libraries(rbtest ${BPAPP_LINK_LIBRARIES})
