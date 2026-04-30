# esd-elks

`esd-elks` is a standalone ELKS package providing a minimal Enlightened Sound
Daemon subset:

- `esd`: a small TCP listener that writes raw unsigned 8-bit mono PCM to
  `/dev/dsp` or `/dev/audio`
- `esdplay`: a matching client that streams a file or stdin to the daemon

This package is intentionally separate from the main ELKS build and image.

## Build

Requirements:

- an ELKS checkout with the ia16 toolchain built
- `env.sh` from that checkout sourced into the shell

Example:

```sh
. /path/to/elks-enhanced/env.sh
make ELKS_TOP=/path/to/elks-enhanced
```

## Install

To install into an ELKS rootfs staging tree:

```sh
make ELKS_TOP=/path/to/elks-enhanced DESTDIR=/path/to/elks-enhanced/target install
```

After that, rebuild the image from the ELKS tree if you want the binaries to
land in an image.

## Runtime

Start the daemon on ELKS:

```sh
esd 16001 8000
```

Stream a raw unsigned 8-bit mono file to it:

```sh
esdplay sample.raw 127.0.0.1 16001
```
