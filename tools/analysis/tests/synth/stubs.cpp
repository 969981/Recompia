void operator delete(void*, unsigned long) {}
extern "C" int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
