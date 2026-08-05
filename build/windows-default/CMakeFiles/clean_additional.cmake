# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "CMakeFiles\\DocConverter_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\DocConverter_autogen.dir\\ParseCache.txt"
  "DocConverter_autogen"
  )
endif()
