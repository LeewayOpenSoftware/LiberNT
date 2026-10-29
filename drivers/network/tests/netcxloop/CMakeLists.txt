
add_library(netcxloop MODULE netcxloop.cpp netcxloop.rc)
set_module_type(netcxloop kmdfdriver KMDF_VERSION 1.33)
target_include_directories(netcxloop PRIVATE
    ${REACTOS_SOURCE_DIR}/sdk/include/ddk/netcx/kmdf/adapter/2.5
    ${REACTOS_SOURCE_DIR}/sdk/include/psdk/netcx/shared/1.0)
target_compile_definitions(netcxloop PRIVATE NET_VERSION_MAJOR=2 NET_VERSION_MINOR=5)
target_link_libraries(netcxloop netadaptercxstub_2_5 ntoskrnl_vista libcntpr ${PSEH_LIB})
add_importlibs(netcxloop ntoskrnl hal)

add_subdirectory(test)
