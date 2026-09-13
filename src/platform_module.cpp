// Windows VST3 module handle required by VSTGUI's platform initialization.
// The VST3 SDK references this global from getPlatformModuleHandle().
#if defined(_WIN32)
void* moduleHandle = nullptr;
#endif
