/* m.lib: an empty library, as MinGW ships libm.a. Portable builds name the
   math library `m` on every link (libavif and plutovg do, under UNIX OR
   MINGW); on x86_64-pc-windows-msvc the link then asks for m.lib. The math
   functions themselves come from ucrtbase.lib, so this library holds
   nothing but this declaration. */
typedef int stxlib_empty_math_library;
