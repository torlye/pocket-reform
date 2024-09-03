# POCKET REFORM SYSTEM FIRMWARE

# Setup
1. Invoke `./install-fw-dependencies.sh` from the parent directories, or edit the build.sh to specify the location of the pico-sdk and pico-extras.

2. Make sure the pico-sdk submodules are initialized, otherwise serial usb support will be missing.

```
cd pico-sdk
git submodule init
git submodule update --depth 1
```

# Build
```
./build.sh
```

# Flash
```
./flash.sh
```