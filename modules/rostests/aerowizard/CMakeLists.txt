
add_executable(aeroshot aeroshot.c aeroshot.rc)
set_module_type(aeroshot win32cui UNICODE)
add_importlibs(aeroshot comctl32 user32 gdi32 msvcrt kernel32)
add_rostests_file(TARGET aeroshot SUBDIR suppl)
