
add_executable(netstat netstat.c netstat.rc)
set_module_type(netstat win32cui UNICODE)
target_link_libraries(netstat conutils)
add_importlibs(netstat ws2_32 snmpapi iphlpapi msvcrt kernel32 ntdll)
add_cd_file(TARGET netstat DESTINATION reactos/system32 FOR all)
