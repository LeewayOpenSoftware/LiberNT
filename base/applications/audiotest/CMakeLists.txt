add_executable(audiotest audiotest.c)
set_module_type(audiotest win32cui)
add_importlibs(audiotest winmm msvcrt kernel32)
add_cd_file(TARGET audiotest DESTINATION reactos/system32 FOR all)
