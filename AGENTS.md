# Repository instructions

- All Git commits must follow the Gitmoji convention documented in `CONTRIBUTING.md`.
- Use the format `<emoji> <简短的中文动宾短语>` with a Unicode emoji and no Conventional Commits prefix.
- Keep each commit focused on one logical change and run the relevant checks before committing.

## Local Windows toolchain

- `cmake` and `ctest` are not on `PATH` in the agent terminal. Use the Visual Studio copies at:
  - `D:\IDE\VS26\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
  - `D:\IDE\VS26\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe`
- Initialize the MSVC environment before building or testing by calling `D:\IDE\VS26\VC\Auxiliary\Build\vcvars64.bat`.
- Build the configured Debug preset with:
  `cmd /d /s /c "call D:\IDE\VS26\VC\Auxiliary\Build\vcvars64.bat >nul && D:\IDE\VS26\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe --build --preset windows-x64-debug"`
- Run its tests with:
  `cmd /d /s /c "call D:\IDE\VS26\VC\Auxiliary\Build\vcvars64.bat >nul && D:\IDE\VS26\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe --preset windows-x64-debug"`
