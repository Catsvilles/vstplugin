cmake_minimum_required (VERSION 2.8)

set(PROJECT "vstplugin_tilde")
set(FILENAME "vstplugin~")
message(STATUS "---\n*** ${FILENAME} ***")

set(SRC "src/${FILENAME}.h" "src/${FILENAME}.cpp")

include_directories(src)

# Pd directories:
if (NOT DEFINED PD_DIR)
    if (WIN32 OR MINGW)
        set(PD_INCLUDEDIR "${PROGRAMFILES}/Pd/src" CACHE PATH "Pd header files")
    elseif(APPLE)
        # isn't there a better way?
        file(GLOB _globresults "/Applications/Pd*.app/Contents/Resources/src")
        list(GET _globresults 0 _globresult)
        set(PD_INCLUDEDIR ${_globresult} CACHE PATH "Pd header files")
    else()
        set(PD_INCLUDEDIR "/usr/include/pd" CACHE PATH "Pd header files")
    endif()
    if (WIN32 OR MINGW)
        set(PD_BINDIR "${PROGRAMFILES}/Pd/bin" CACHE PATH "Pd binaries")
    endif()
else()
    set(PD_INCLUDEDIR "${PD_DIR}/src" CACHE PATH "Pd header files" FORCE)
    if (WIN32 OR MINGW)
        set(PD_BINDIR "${PD_DIR}/bin" CACHE PATH "Pd binaries" FORCE)
    endif()
endif()
include_directories("${PD_INCLUDEDIR}")
message(STATUS "PD_INCLUDEDIR: ${PD_INCLUDEDIR}")
if (WIN32 OR MINGW)
    message(STATUS "PD_BINDIR: ${PD_BINDIR}")
endif()

# install directory:
if (WIN32 OR MINGW)
    set(PD_INSTALLDIR "${APPDATA}/Pd" CACHE PATH "Installation directoy")
elseif(APPLE)
    set(PD_INSTALLDIR "~/Library/Pd" CACHE PATH "Installation directoy")
else()
    set(PD_INSTALLDIR "/usr/local/lib/pd-externals" CACHE PATH "Installation directoy")
endif()
message(STATUS "PD_INSTALLDIR: ${PD_INSTALLDIR}")

# plugin extension
set(CMAKE_SHARED_MODULE_PREFIX "") # no prefix!
if (NOT PD_EXTENSION)
    if (WIN32 OR MINGW)
        set(CMAKE_SHARED_MODULE_SUFFIX ".dll")
    elseif(APPLE)
        set(CMAKE_SHARED_MODULE_SUFFIX ".pd_darwin")
    elseif(LINUX)
        set(CMAKE_SHARED_MODULE_SUFFIX ".pd_linux")
    endif()
else()
    set(CMAKE_SHARED_MODULE_SUFFIX ${PD_EXTENSION})
endif()
message(STATUS "PD_EXTENSION: ${CMAKE_SHARED_MODULE_SUFFIX}")

if (MINGW)
    set(CMAKE_EXECUTABLE_SUFFIX ".exe")
endif()

# the Pd external
add_library(${PROJECT} MODULE ${SRC} ${VST_HEADERS} ${VST_SRC})
set_target_properties(${PROJECT} PROPERTIES OUTPUT_NAME ${FILENAME})
target_link_libraries(${PROJECT} ${VST_LIBS})
set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS_RELEASE -s)

# platform specific definitions and options
if (WIN32 OR MINGW)
    target_compile_definitions(${PROJECT} PUBLIC -DHAVE_UI_THREAD=1)
    if (MINGW)
        set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS "--enable-auto-import")
        target_link_libraries(${PROJECT} "${PD_BINDIR}/pd.dll")
    else()
        target_link_libraries(${PROJECT} "${PD_BINDIR}/pd.lib")
    endif()
elseif(APPLE)
    target_compile_definitions(${PROJECT} PUBLIC -DHAVE_UI_THREAD=0)
    set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS "-undefined suppress -flat_namespace -bundle")
else()
    target_compile_definitions(${PROJECT} PUBLIC -DHAVE_UI_THREAD=1)
    set_target_properties(${PROJECT} PROPERTIES COMPILE_FLAGS "-fPIC -fcheck-new")
    set_target_properties(${PROJECT} PROPERTIES LINK_FLAGS "-rdynamic -fPIC")
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
endif()

# installation
add_dependencies(${PROJECT} ${PROBE})

install(TARGETS ${PROJECT} DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
install(PROGRAMS "${PROBE_PATH}" DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
install(FILES "${FILENAME}-help.pd" "vstpluginbrowser.pd" "vstpluginbrowser-help.pd"
    "vstpresetbrowser.pd" "vstpresetbrowser-help.pd" "../README.md" "../LICENSE.txt"
     DESTINATION "${PD_INSTALLDIR}/${FILENAME}")
