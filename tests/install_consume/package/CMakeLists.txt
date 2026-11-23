# The install-and-consume check (tests/install_consume/README in run.sh): STXlib
# added as a subdirectory and its three libraries installed into one export set,
# as stx's cmake/Stxlib.cmake does, so install(EXPORT) sees every target a
# library links. run.sh installs this, then builds consumer/ against it.
cmake_minimum_required(VERSION 3.24)
project(stxlib_install_check C CXX)
include(GNUInstallDirs)
set(STXLIB_TESTS OFF CACHE BOOL "")
set(STXLIB_BUILD_TOOLS OFF CACHE BOOL "")
set(STXLIB_STATE_SHARED ON CACHE BOOL "")
add_subdirectory(${STXLIB_SOURCE} stxlib)
set_target_properties(stxlib_state PROPERTIES EXPORT_NAME state)
set_target_properties(stxlib_boundary PROPERTIES EXPORT_NAME boundary)
set_target_properties(stxlib_algorithms PROPERTIES EXPORT_NAME algorithms)
install(TARGETS stxlib_state stxlib_boundary stxlib_algorithms EXPORT stxlibTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${STXLIB_SOURCE}/include/stxlib DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT stxlibTargets NAMESPACE stxlib:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/stxlib)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stxlibConfig.cmake
"include(CMakeFindDependencyMacro)
find_dependency(Threads)
include(\"\${CMAKE_CURRENT_LIST_DIR}/stxlibTargets.cmake\")
")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/stxlibConfig.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/stxlib)
