__declspec(dllimport) int companion_value(void);
__declspec(dllexport) int companion_plugin_value(void) { return companion_value(); }
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reason; (void)reserved; return TRUE;
}
