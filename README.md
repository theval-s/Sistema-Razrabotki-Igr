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

## 1. Clone

```
git clone --recurse-submodules https://github.com/theval-s/Sistema-Razrabotki-Igr
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

## 4. Package for release if needed

```
cmake --install build/windows-msvc --config RelWithDebInfo --prefix dist
```
