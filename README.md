# lexec
一个c++17版本的sender/receiver模型实现

设计与实施计划见 [docs/design.md](docs/design.md)。

## 构建与测试

需要 CMake 3.25+、Ninja，以及 GCC 10+ 或 Clang 12+。

```sh
cmake --workflow --preset gcc-debug
```

可用的 preset 为 `{gcc,clang}-{debug,release,asan,tsan,noexcept}`。
