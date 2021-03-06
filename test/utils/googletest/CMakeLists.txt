##===------------------------------------------------------------------------------*- CMake -*-===##
##                         _       _                   
##                        | |     | |                  
##                    __ _| |_ ___| | __ _ _ __   __ _ 
##                   / _` | __/ __| |/ _` | '_ \ / _` |
##                  | (_| | || (__| | (_| | | | | (_| |
##                   \__, |\__\___|_|\__,_|_| |_|\__, | - GridTools Clang DSL
##                    __/ |                       __/ |
##                   |___/                       |___/ 
##
##
##  This file is distributed under the MIT License (MIT). 
##  See LICENSE.txt for details.
##
##===------------------------------------------------------------------------------------------===##

include(CheckCXXCompilerFlag)

include_directories(SYSTEM 
  ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}
)

add_library(gtest src/gtest-all.cc)   

find_package(Threads)
if(CMAKE_THREAD_LIBS_INIT)
  target_compile_definitions(gtest PRIVATE -DGTEST_HAS_PTHREAD=1)
  target_link_libraries(gtest ${CMAKE_THREAD_LIBS_INIT})
endif()

check_cxx_compiler_flag(-w HAVE_GCC_NO_WARNING)
if(HAVE_GCC_NO_WARNING)
  target_compile_options(gtest PRIVATE PUBLIC -w)
endif()
