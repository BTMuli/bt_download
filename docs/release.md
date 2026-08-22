# Windows 发布产物

Release 安装目录是交给 BangumiToday Windows/MSIX 构建的完整伴随进程产物。它包含引擎、实际使用的原生 DLL、VC 运行库、第三方许可证通知和 SPDX 2.3 SBOM，不包含测试程序、PDB 或 vcpkg 缓存。

## 生成

先配置并构建锁定依赖的 Release 预设，再执行安装：

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
cmake --install out/build/windows-x64-release
```

默认输出位于 `out/install/windows-x64-release`。流水线可用 `cmake --install ... --prefix <目录>` 覆盖位置；许可证和 SBOM 会写入同一个目录。

产物至少包含：

- `bt_download.exe`、`torrent-rasterbar.dll`、`libcurl.dll`、`z.dll`、
  `libssl-3-x64.dll` 和 `libcrypto-3-x64.dll`；
- MSVC C++ 运行库 DLL；Windows 10/11 自带的系统 DLL 和 Universal CRT 不重复复制；
- `THIRD_PARTY_NOTICES.txt` 与 `licenses/` 下的原始许可证文本；
- `sbom.spdx.json`，记录应用、libtorrent、curl、zlib、OpenSSL、
  nlohmann/json、Boost 和 MSVC 运行库的锁定版本、依赖关系，以及全部随包
  二进制的 SHA-256。

## 接入约束

BangumiToday 应复制整个安装目录，不能只复制 `bt_download.exe`。MSIX 流水线应在打包前检查上述文件存在，并把 `sbom.spdx.json` 和许可证材料保留为发布制品；是否把通知文件放入最终包或应用内“开源许可”页面，可由主项目的分发策略决定。
