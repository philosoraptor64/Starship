# Netplay build fingerprint, run at every build: a hash of every source file that can change what the game
# simulates, so two machines connect only when they built the same code. Writes OUT only when it changes.
# Usage: cmake -DSRC=<source dir> -DOUT=<header> -P NetBuildId.cmake
file(GLOB_RECURSE files RELATIVE "${SRC}"
  "${SRC}/src/*.c" "${SRC}/src/*.h" "${SRC}/src/*.cpp" "${SRC}/include/*.h"
  "${SRC}/external/n64net/client/src/*.cpp" "${SRC}/external/n64net/client/include/*.h")
list(SORT files)
set(all "")
foreach(f IN LISTS files)
  file(SHA1 "${SRC}/${f}" h)
  string(APPEND all "${f}:${h}\n")
endforeach()
string(SHA1 id "${all}")
string(SUBSTRING "${id}" 0 12 id)
set(text "#define SFNET_BUILD_ID \"${id}\"\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT "${old}" STREQUAL "${text}")
  file(WRITE "${OUT}" "${text}")
endif()
