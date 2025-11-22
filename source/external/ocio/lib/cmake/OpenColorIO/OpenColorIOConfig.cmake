
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was Config.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

####################################################################################

include(CMakeFindDependencyMacro)

if (NOT ON) # NOT ON
    if (APPLE)
        # Store the previous value of CMAKE_FIND_FRAMEWORK and CMAKE_FIND_APPBUNDLE.
        set(_PREVIOUS_CMAKE_FIND_FRAMEWORK ${CMAKE_FIND_FRAMEWORK})
        set(_PREVIOUS_CMAKE_FIND_APPBUNDLE ${CMAKE_FIND_APPBUNDLE})

        # Prioritize other paths before Frameworks and Appbundle for find_path, find_library and 
        # find_package.
        set(CMAKE_FIND_FRAMEWORK LAST)
        set(CMAKE_FIND_APPBUNDLE LAST)
    endif()

    # Append OCIO custom find module path.
    list(APPEND CMAKE_MODULE_PATH "${PACKAGE_PREFIX_DIR}/share/OpenColorIO/cmake/modules")
    list(APPEND CMAKE_MODULE_PATH "${PACKAGE_PREFIX_DIR}/share/OpenColorIO/cmake/macros")

    ########################
    # Required dependencies 
    ########################

    if (NOT TARGET expat::expat)
        find_dependency(expat 2.7.2)
    endif()

    if (NOT TARGET Imath::Imath)
        find_dependency(Imath 3.2.1)
    endif()

    if (NOT TARGET pystring::pystring)
        find_dependency(pystring 1.1.4)
    endif()

    if (NOT TARGET yaml-cpp::yaml-cpp)
        find_dependency(yaml-cpp 0.8.0)
        if (TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
            add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
        endif()
    endif()

    if (NOT TARGET ZLIB::ZLIB)
        # ZLIB_VERSION is available starting CMake 3.26+.
        # ZLIB_VERSION_STRING is still available for backward compatibility.
        # See https://cmake.org/cmake/help/git-stage/module/FindZLIB.html

        if (1.3.1) # 1.3.1
            find_dependency(ZLIB 1.3.1)
        else()
            find_dependency(ZLIB )
        endif()
    endif()

    if (NOT TARGET MINIZIP::minizip-ng)
        find_dependency(minizip-ng 4.0.10)
    endif()

    # Remove OCIO custom find module path.
    list(REMOVE_AT CMAKE_MODULE_PATH -1)

    if (APPLE)
        # Restore CMAKE_FIND_FRAMEWORK and CMAKE_FIND_APPBUNDLE values.
        set(CMAKE_FIND_FRAMEWORK ${_PREVIOUS_CMAKE_FIND_FRAMEWORK})
        set(CMAKE_FIND_APPBUNDLE ${_PREVIOUS_CMAKE_FIND_APPBUNDLE})
    endif()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/OpenColorIOTargets.cmake)

include(FindPackageHandleStandardArgs)
set(OpenColorIO_CONFIG ${CMAKE_CURRENT_LIST_FILE})
find_package_handle_standard_args(OpenColorIO CONFIG_MODE)
