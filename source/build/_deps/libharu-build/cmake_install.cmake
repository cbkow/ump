# Install script for directory: D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files/ump")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_types.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_consts.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_annotation.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_catalog.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_conf.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_destination.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_doc.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_encoder.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_encrypt.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_encryptdict.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_error.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_ext_gstate.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_font.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_fontdef.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_gstate.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_image.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_info.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_list.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_mmgr.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_namedict.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_objects.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_outline.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_pages.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_page_label.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_streams.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_u3d.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_utils.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_pdfa.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_3dmeasure.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_exdata.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/include/hpdf_version.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-build/include/hpdf_config.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/libharu" TYPE FILE FILES
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/README.md"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/CHANGES"
    "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/INSTALL"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/libharu" TYPE DIRECTORY FILES "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-src/bindings")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("D:/z_DevTemp/UnionPlayer/build/_deps/libharu-build/src/cmake_install.cmake")

endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "D:/z_DevTemp/UnionPlayer/build/_deps/libharu-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
