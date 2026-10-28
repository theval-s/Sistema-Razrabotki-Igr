# General info and prerequisites

Engine is a Shared library, Editor is a separate project that loads that library and is a GUI Qt app that does calls to engine dll.

vcpkg and git submodules are used for dependencies, preferably vcpkg when possible. 



## Installing Qt for building editor

We have a custom config for installing Qt through aqt from Russia using Yandex mirror. We are using Qt 6.10.3.
Download [aqt](https://github.com/miurahr/aqtinstall) 3.3.0 currently, put it into tools folder and run the following command from that folder:

```
.\aqt_x64.exe -c .\qt-settings.ini install-qt --outputdir C:\Qt windows desktop 6.10.3 win64_msvc2022_64 -m qtimageformats qtmultimedia
```
Note: you might have aqt.exe instead of aqt_x64.exe and you might want to change the outputdir argument to the one you want. 


For CMake to find Qt properly you might need to also add an environment variable, for example using this PowerShell command, changing your path to Qt if needed:

```
[Environment]::SetEnvironmentVariable("QT_ROOT", "C:\Qt\6.10.3\msvc2022_64", "User")
```

# Build and setup

Material and material-instance JSON formats, editor serialization API, and streaming
examples: [material asset contract](docs/material-assets.md).

Imported meshes expose `MeshAsset::bounds` (`Aabb::min` / `max`) in mesh-local
coordinates, after Assimp node transforms and optional `centerOnLoad`
normalization. Assimp generates per-mesh bounding boxes via aiProcess_GenBoundingBoxes;
the importer combines them into the asset bounds, which remain
available after CPU geometry is released; apply the object's world transform
separately when world-space bounds are needed.

With `SRI_BUILD_TESTS=ON`, run `mesh_assets_tests.exe` for CPU mesh-import checks
and `material_assets_tests.exe` for asset/GPU integration, directly via GoogleTest.

## 1. Clone

```
git clone --recurse-submodules https://github.com/theval-s/Sistema-Razrabotki-Igr SRI
cd SRI
```

Already cloned without submodules:

```
git submodule update --init --recursive
```

## 2. vcpkg

Even with vcpkg as submodule, you should run its bootstrap.

```
.\external\vcpkg\bootstrap-vcpkg.bat  
```

## 3. Build using CMake

You can just open the project in IDE of your choice that supports CMake and CMake Presets.
Or use cmake from cli.

```
cmake --preset windows-msvc
cmake --build --preset windows-debug
```

You can also configure for VS and open the resulting solution: 
```
cmake --preset windows-vs2022
```

and then open .sln file in 'build/windows-vs2022/'.

If you don't want to build editor, or don't want to deal with installing Qt for building editor - change SRI_BUILD_EDITOR to OFF in your CMake preset.

For engine development, build the standalone example game without configuring
or building the Qt editor:

```
cmake --preset windows-example-game
cmake --build --preset windows-example-game-debug
```

`ExampleGame` links the engine statically through `Engine::EngineStatic`.

## 4. Package for release if needed

Build a portable, self-contained editor folder (exe, DLLs, Qt plugins, shaders, editor modules):

```
cmake --build --preset windows-release --target deploy
```

This produces `build/windows-msvc/deploy/<config>/editor`. The folder is wiped and recreated each time; set
`SRI_DEPLOY_DIR` to put it elsewhere. Debug deploys need the debug MSVC runtime, so they only run on machines with Visual Studio.

The engine SDK (headers, import library, CMake package) is a separate install component:

```
cmake --install build/windows-msvc --config RelWithDebInfo --component sdk --prefix dist/sdk
```
