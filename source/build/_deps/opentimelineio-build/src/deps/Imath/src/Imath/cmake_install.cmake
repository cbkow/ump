# Install script for directory: D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath

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
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-build/src/deps/Imath/src/Imath/Debug/Imath-3_2_d.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-build/src/deps/Imath/src/Imath/Release/Imath-3_2.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-build/src/deps/Imath/src/Imath/MinSizeRel/Imath-3_2.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-build/src/deps/Imath/src/Imath/RelWithDebInfo/Imath-3_2.lib")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/Imath" TYPE FILE FILES
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/half.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/halfFunction.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/halfLimits.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathBox.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathBoxAlgo.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathColor.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathColorAlgo.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathEuler.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathExport.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathForward.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathFrame.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathFrustum.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathFrustumTest.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathFun.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathGL.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathGLU.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathInt64.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathInterval.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathLine.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathLineAlgo.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathMath.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathMatrix.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathMatrixAlgo.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathNamespace.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathPlane.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathPlatform.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathQuat.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathRandom.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathRoots.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathShear.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathSphere.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathTypeTraits.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathVec.h"
    "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-src/src/deps/Imath/src/Imath/ImathVecAlgo.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "D:/z_DevTemp/UnionPlayer/build/_deps/opentimelineio-build/src/deps/Imath/src/Imath/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
