# -----------------------------------------------------------------------------
#  运行时 DLL 拷贝（被 aiwrite_copy_runtime_dlls / aiwrite_sync_vcpkg_bin 调用）
#
#  用法：
#    cmake -DDEST=<目标目录> "-DDLLS=a.dll;b.dll" -P copy_runtime_dlls.cmake
#    cmake -DDEST=<目标目录> -DSYNC_DIR=<vcpkg 的 bin 目录> -P copy_runtime_dlls.cmake
#
#  说明：逐个文件用 cmake -E copy_if_different 拷贝，失败时只告警不中断构建，
#        以便目标 DLL 被正在运行的进程占用时仍能完成构建（已有副本可继续使用）。
# -----------------------------------------------------------------------------
if(NOT DEFINED DEST OR DEST STREQUAL "")
    message(FATAL_ERROR "copy_runtime_dlls: 缺少 DEST")
endif()

string(REPLACE "\\" "/" DEST "${DEST}")
file(MAKE_DIRECTORY "${DEST}")

function(aiwrite_copy_one src dest)
    if(NOT EXISTS "${src}")
        message(WARNING "copy_runtime_dlls: 源文件不存在 ${src}")
        return()
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${src}" "${dest}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(WARNING "copy_runtime_dlls: 跳过（文件可能被占用）${src}\n  ${_err}")
        return()
    endif()
    message(STATUS "copy_runtime_dlls: ${src} -> ${dest}")
endfunction()

if(DEFINED DLLS AND NOT DLLS STREQUAL "")
    foreach(_dll IN LISTS DLLS)
        if(_dll STREQUAL "")
            continue()
        endif()
        aiwrite_copy_one("${_dll}" "${DEST}")
    endforeach()
endif()

if(DEFINED SYNC_DIR AND NOT SYNC_DIR STREQUAL "" AND EXISTS "${SYNC_DIR}")
    file(GLOB _sync_dlls "${SYNC_DIR}/*.dll")
    foreach(_dll IN LISTS _sync_dlls)
        aiwrite_copy_one("${_dll}" "${DEST}")
    endforeach()
endif()
