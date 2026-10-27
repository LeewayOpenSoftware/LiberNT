
add_executable(drvinst drvinst.c)
set_module_type(drvinst win32cui UNICODE)
add_importlibs(drvinst newdev cfgmgr32 msvcrt kernel32 ntdll)
add_cd_file(TARGET drvinst DESTINATION reactos/system32 FOR all)
