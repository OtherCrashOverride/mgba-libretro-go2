#!lua
local output = "./build/" .. _ACTION

solution "mgba_solution"
   configurations { "Debug", "Release" }


project "mgba"
   location (output)
   kind "ConsoleApp"
   language "C"
   includedirs {".", "../include", "/usr/include/libdrm" }
   files { "./*.h", "./*.c"}
   buildoptions { "-flto=auto -fuse-linker-plugin -march=armv8-a+crc+simd+crypto -mtune=cortex-a35 -Wall -D__GBM__ -DUSE_PTHREADS -D_7ZIP_PPMD_SUPPPORT -DM_CORE_GB -DM_CORE_GBA" }
   linkoptions { "-L. -lEGL -lGLESv1_CM -lgo2 -l:../../../build/libmgba.a -lm -lpthread" }
   defines { "MGBA_STANDALONE" }

   configuration "Debug"
      flags { "Symbols" }
      defines { "DEBUG" }

   configuration "Release"
      flags { "Optimize" }
      defines { "NDEBUG" }
      linkoptions { "-flto=auto -fuse-linker-plugin" }
