# Repository instructions

- All Git commits must follow the Gitmoji convention documented in `CONTRIBUTING.md`.
- Use the format `<emoji> <简短的中文动宾短语>` with a Unicode emoji and no Conventional Commits prefix.
- Keep each commit focused on one logical change and run the relevant checks before committing.

## Local Windows toolchain

- `cmake` is not on `PATH` in the agent terminal. Use the Visual Studio copy at
  `D:\IDE\VS26\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.
- Initialize the MSVC environment before building by calling `D:\IDE\VS26\VC\Auxiliary\Build\vcvars64.bat`.
- Build the configured Debug preset with:
  `cmd /d /s /c "call D:\IDE\VS26\VC\Auxiliary\Build\vcvars64.bat >nul && D:\IDE\VS26\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build --preset windows-x64-debug"`
- 自动化测试默认关闭：`dev_build.ps1` 与发布流水线只用 `windows-x64-debug` / `windows-x64-release` 预设（显式 `BT_DOWNLOAD_BUILD_TESTS=OFF`），不要在这些预设或 CI 中加入测试步骤。手动验证用 `windows-x64-debug-tests` 预设：
  `cmd /d /s /c "call D:\IDE\VS26\VC\Auxiliary\Build\vcvars64.bat >nul && set VCPKG_ROOT=D:\IDE\VS26\VC\vcpkg && D:\IDE\VS26\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --preset windows-x64-debug-tests && D:\IDE\VS26\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build --preset windows-x64-debug-tests && D:\IDE\VS26\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe --preset windows-x64-debug-tests"`
