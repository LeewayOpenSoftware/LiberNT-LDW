option(NTFS_J31C_HOOK "J31c deterministic MFT-record repro hook (TEST ONLY; never enable in a product build)" OFF)
if(NTFS_J31C_HOOK)
    # Applies to this directory's subtrees (src/ and env/), i.e. every ntfslib_new_* target.
    add_compile_definitions(NTFS_J31C_HOOK)
endif()

option(NTFS_J36_HOOK "J36 write-barrier fault-injection hook (TEST ONLY; never enable in a product build)" OFF)
if(NTFS_J36_HOOK)
    add_compile_definitions(NTFS_J36_HOOK)
endif()

add_library(ntfslib_new)
add_subdirectory(src)
add_dependencies(ntfslib_new psdk bugcodes)

target_include_directories(ntfslib_new
    PUBLIC include/public
    PRIVATE include/internal
)

add_subdirectory(env)
