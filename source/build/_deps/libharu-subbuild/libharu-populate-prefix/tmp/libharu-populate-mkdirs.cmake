# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src")
  file(MAKE_DIRECTORY "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src")
endif()
file(MAKE_DIRECTORY
  "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-build"
  "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-subbuild/libharu-populate-prefix"
  "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-subbuild/libharu-populate-prefix/tmp"
  "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-subbuild/libharu-populate-prefix/src/libharu-populate-stamp"
  "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-subbuild/libharu-populate-prefix/src"
  "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-subbuild/libharu-populate-prefix/src/libharu-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-subbuild/libharu-populate-prefix/src/libharu-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-subbuild/libharu-populate-prefix/src/libharu-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
