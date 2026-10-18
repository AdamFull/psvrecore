# Known issues and open questions

- **Linux has not been built or tested.** The development host has no Linux machine and no WSL. The CI workflow
  covers Linux, but it runs only once the repository is pushed.
- **Android has nothing in its default build yet.** Tools and tests are host-only, and there is no runtime yet.
  nx-foundation cross-compiles for arm64. The address-space test has not run on an Android device; it will run once
  there is a device test runner.
