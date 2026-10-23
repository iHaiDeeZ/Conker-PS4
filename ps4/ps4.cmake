# PS4: Conker's Bad Fur Day: Recompiled's host as a PS4 app, included at the end of host/CMakeLists.txt
# when building with the port's toolchain (toolchain/ps4-conker.cmake, and ps4-love-style.cmake as
# the project include: PS4_RUNTIME_SOURCES, ps4_link_mesa, ps4_add_eboot).

target_sources(ConkerRecomp PRIVATE
    ${CONKER_PS4_DIR}/ps4_log.cpp
    ${CONKER_PS4_DIR}/ps4_platform.cpp
    ${CONKER_PS4_DIR}/ps4_pad.cpp
    ${CONKER_PS4_DIR}/ps4_wsi.cpp
    ${PS4_RUNTIME_SOURCES})
target_include_directories(ConkerRecomp PRIVATE
    ${CONKER_PS4_DIR}
    "${RT64_DIR}/src/contrib/plume/contrib/Vulkan-Headers/include")

# The runtime's game memory and the recompiled code's pages, through ps4_platform.cpp
target_include_directories(librecomp PRIVATE ${CONKER_PS4_DIR})
target_include_directories(recompui PRIVATE ${CONKER_PS4_DIR})

# The network library's shares (port/ps4_netfs.cpp: SMB and NFS under /net)
target_link_libraries(ConkerRecomp PRIVATE ${PS4_SYSROOT}/lib/libnfs.a ${PS4_SYSROOT}/lib/libsmb2.a)
# every undefined symbol at once
target_link_options(ConkerRecomp PRIVATE --error-limit=0)

# RADV on GNM, the system libraries, and eboot.bin
ps4_link_mesa(ConkerRecomp)
target_link_libraries(ConkerRecomp PRIVATE
    SceSysmodule SceSystemService SceUserService SceVideoOut SceAudioOut ScePad SceNet)
ps4_add_eboot(ConkerRecomp)

# zstd's dictionary builder sorts with qsort_r when _GNU_SOURCE is defined (the toolchain defines
# it); the PS4's C library has no qsort_r: its C90 fallback then.
set_source_files_properties("${RT64_DIR}/src/contrib/zstd/lib/dictBuilder/cover.c"
    DIRECTORY "${RT64_DIR}/src/contrib/zstd/build/cmake/lib"
    PROPERTIES COMPILE_OPTIONS "-U_GNU_SOURCE")

# re-spirv's command-line tool isn't needed on the console (its library is RT64's).
set_target_properties(re-spirv-cli PROPERTIES EXCLUDE_FROM_ALL TRUE)

# SDL2's static target asks for -pthread and libm, which ld.lld (called directly) rejects: both are
# in the system libraries already.
if (TARGET SDL2::SDL2-static)
    set_target_properties(SDL2::SDL2-static PROPERTIES INTERFACE_LINK_LIBRARIES "")
endif()

# zstd's tracing hooks are weak symbols left undefined, which create-fself rejects ("missing module").
target_compile_definitions(libzstd_static PRIVATE ZSTD_TRACE=0)

# The C library's lstat calls its own fstatat, which --wrap=fstatat (port/ps4_fileat.cpp) leaves to
# __real_fstatat, and lld doesn't extract an archive member for that: pull it in by name, or
# create-fself finds "no module" for fstatat.
target_link_options(ConkerRecomp PRIVATE --undefined=fstatat)
