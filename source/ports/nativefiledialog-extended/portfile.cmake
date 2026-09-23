# =============================================================================
#  nativefiledialog-extended 1.3.0 —— overlay port
#
#  背景：本项目的 vcpkg 快照（2024-04 版本 / ports 基线 eb0f108）中没有该 port，
#        而设计文档 §2.2 指定用它做文件/目录选择对话框，故以 overlay 方式补上。
#
#  依据：vcpkg 官方 ports/nativefiledialog-extended（REF 与 SHA512 与上游一致），
#        仅做了两处适配：
#          1) supports 收窄为 windows（本项目只发 Windows）
#          2) Windows 不需要 xdg 桌面门户，NFD_PORTAL 关闭
# =============================================================================
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO btzy/nativefiledialog-extended
    REF v1.3.0
    SHA512 1f2e17dd9ee5b416dfe1362b6eac6499c83c527a83478361769420f1d29bf21e0a81e4b6d45255703aba9be61c8379f7745fe182d74687a9c4f3309bd4fdf09e
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DNFD_BUILD_TESTS=OFF
        -DNFD_PORTAL=OFF
)

vcpkg_cmake_install()

vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME nfd
    CONFIG_PATH lib/cmake/nfd
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
