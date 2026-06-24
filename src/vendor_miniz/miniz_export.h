#ifndef MINIZ_EXPORT
// We compile miniz's sources directly into MapForGoblins (static, internal), so the export macro is
// empty. (Normally miniz's CMake generates this file; we don't run its CMake - see CMakeLists.txt.)
#define MINIZ_EXPORT
#endif
