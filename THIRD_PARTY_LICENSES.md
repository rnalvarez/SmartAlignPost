# Third-party licenses

## Steinberg VST3 SDK

The project uses the Steinberg VST3 SDK as a Git submodule at `extern/vst3sdk`.

Official repository:
https://github.com/steinbergmedia/vst3sdk

The current VST3 SDK is distributed under the MIT license. The SDK remains a separate third-party component; its license and usage guidelines apply to the SDK files themselves.

Do not copy the SDK source into this repository. Clone/update the submodule instead:

```bash
git submodule update --init --recursive
```
