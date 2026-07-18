cmake_minimum_required(VERSION 3.28)
project(THsip VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(THSIP_WITH_WEBENGINE "Embed official Odorik portal" ON)
option(THSIP_WITH_PJSIP "Enable PJSUA2 telephony" ON)
option(THSIP_FETCH_PJSIP "Fetch pinned PJSIP when not installed" ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick Network Sql Multimedia Test WebSockets)
find_package(unofficial-libphonenumber CONFIG QUIET)
find_package(libphonenumber CONFIG QUIET)
if(THSIP_WITH_WEBENGINE)
  find_package(Qt6 QUIET COMPONENTS WebEngineQuick)
endif()

qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(thsip
  src/main.cpp
  src/backend.cpp
  src/backend.h
  src/secretstore.h
  src/contactstore.h
)
if(APPLE)
  target_sources(thsip PRIVATE src/secretstore_macos.mm src/contactstore_macos.mm)
  set_source_files_properties(src/contactstore_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
  target_sources(thsip PRIVATE src/secretstore_stub.cpp src/contactstore_stub.cpp)
endif()

qt_add_qml_module(thsip
  URI THsip
  VERSION 1.0
  QML_FILES qml/Main.qml
  RESOURCES resources/capabilities.json resources/api_endpoints.json resources/dial_actions.json
)

target_link_libraries(thsip PRIVATE
  Qt6::Core Qt6::Gui Qt6::Quick Qt6::Network Qt6::Sql Qt6::Multimedia
  Qt6::WebSockets
  "$<$<PLATFORM_ID:Darwin>:-framework Security>"
  "$<$<PLATFORM_ID:Darwin>:-framework Foundation>"
  "$<$<PLATFORM_ID:Darwin>:-framework Contacts>"
)

if(TARGET unofficial::libphonenumber::libphonenumber)
  target_link_libraries(thsip PRIVATE unofficial::libphonenumber::libphonenumber)
  target_compile_definitions(thsip PRIVATE THSIP_HAVE_LIBPHONENUMBER=1)
elseif(TARGET libphonenumber::phonenumber)
  target_link_libraries(thsip PRIVATE libphonenumber::phonenumber)
  target_compile_definitions(thsip PRIVATE THSIP_HAVE_LIBPHONENUMBER=1)
elseif(TARGET phonenumber)
  target_link_libraries(thsip PRIVATE phonenumber)
  target_compile_definitions(thsip PRIVATE THSIP_HAVE_LIBPHONENUMBER=1)
endif()

if(Qt6WebEngineQuick_FOUND)
  target_link_libraries(thsip PRIVATE Qt6::WebEngineQuick)
  target_compile_definitions(thsip PRIVATE THSIP_HAVE_WEBENGINE=1)
endif()

set(THSIP_PJSIP_TARGET "")
if(THSIP_WITH_PJSIP)
  find_package(Pj 2.17 QUIET)
  if(TARGET Pj::pjsua2)
    set(THSIP_PJSIP_TARGET Pj::pjsua2)
  elseif(THSIP_FETCH_PJSIP)
    include(FetchContent)
    set(PJ_SKIP_EXPERIMENTAL_NOTICE ON CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(PJMEDIA_WITH_VIDEO ON CACHE BOOL "" FORCE)
    set(PJMEDIA_WITH_FFMPEG OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(pjproject
      GIT_REPOSITORY https://github.com/pjsip/pjproject.git
      GIT_TAG 2.17
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(pjproject)
    set(THSIP_PJSIP_TARGET pjsua2)
    set(THSIP_PJSIP_INCLUDE_DIRS
      "${pjproject_BINARY_DIR}/pjsip/include"
      "${pjproject_BINARY_DIR}/pjmedia/include"
      "${pjproject_SOURCE_DIR}/pjlib/include"
      "${pjproject_SOURCE_DIR}/pjlib-util/include"
      "${pjproject_SOURCE_DIR}/pjnath/include"
      "${pjproject_SOURCE_DIR}/pjmedia/include"
      "${pjproject_SOURCE_DIR}/pjsip/include"
    )
  endif()
endif()
if(THSIP_PJSIP_TARGET)
  target_sources(thsip PRIVATE src/telephony_pjsua2.cpp src/telephony_pjsua2.h)
  target_link_libraries(thsip PRIVATE ${THSIP_PJSIP_TARGET})
  if(THSIP_PJSIP_INCLUDE_DIRS)
    target_include_directories(thsip PRIVATE ${THSIP_PJSIP_INCLUDE_DIRS})
  endif()
  target_compile_definitions(thsip PRIVATE THSIP_HAVE_PJSIP=1)
endif()

if(APPLE)
  set_target_properties(thsip PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME THsip
    MACOSX_BUNDLE_GUI_IDENTIFIER cz.thinkhome.thsip
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in
  )
endif()

enable_testing()
qt_add_executable(thsip_tests tests/test_core.cpp src/backend.cpp src/backend.h src/secretstore.h src/contactstore.h)
qt_add_resources(thsip_tests test_resources PREFIX "/" FILES resources/capabilities.json resources/api_endpoints.json resources/dial_actions.json)
if(APPLE)
  target_sources(thsip_tests PRIVATE src/secretstore_macos.mm src/contactstore_macos.mm)
  set_source_files_properties(src/contactstore_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
  target_sources(thsip_tests PRIVATE src/secretstore_stub.cpp src/contactstore_stub.cpp)
endif()
target_compile_definitions(thsip_tests PRIVATE THSIP_TESTING=1 THSIP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(thsip_tests PRIVATE src)
target_link_libraries(thsip_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::Test Qt6::WebSockets "$<$<PLATFORM_ID:Darwin>:-framework Security>" "$<$<PLATFORM_ID:Darwin>:-framework Foundation>" "$<$<PLATFORM_ID:Darwin>:-framework Contacts>")
if(TARGET unofficial::libphonenumber::libphonenumber)
  target_link_libraries(thsip_tests PRIVATE unofficial::libphonenumber::libphonenumber)
  target_compile_definitions(thsip_tests PRIVATE THSIP_HAVE_LIBPHONENUMBER=1)
elseif(TARGET libphonenumber::phonenumber)
  target_link_libraries(thsip_tests PRIVATE libphonenumber::phonenumber)
  target_compile_definitions(thsip_tests PRIVATE THSIP_HAVE_LIBPHONENUMBER=1)
elseif(TARGET phonenumber)
  target_link_libraries(thsip_tests PRIVATE phonenumber)
  target_compile_definitions(thsip_tests PRIVATE THSIP_HAVE_LIBPHONENUMBER=1)
endif()
add_test(NAME thsip_core COMMAND thsip_tests)

install(TARGETS thsip BUNDLE DESTINATION . RUNTIME DESTINATION bin)
