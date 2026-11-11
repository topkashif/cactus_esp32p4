# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/esp/v6.1/esp-idf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "C:/esp/v6.1/esp-idf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "D:/ibtivia/cactus_clone/whistle-esp32/esp32/whistle_p4/build/bootloader"
  "D:/ibtivia/cactus_clone/whistle-esp32/esp32/whistle_p4/build/bootloader-prefix"
  "D:/ibtivia/cactus_clone/whistle-esp32/esp32/whistle_p4/build/bootloader-prefix/tmp"
  "D:/ibtivia/cactus_clone/whistle-esp32/esp32/whistle_p4/build/bootloader-prefix/src/bootloader-stamp"
  "D:/ibtivia/cactus_clone/whistle-esp32/esp32/whistle_p4/build/bootloader-prefix/src"
  "D:/ibtivia/cactus_clone/whistle-esp32/esp32/whistle_p4/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/ibtivia/cactus_clone/whistle-esp32/esp32/whistle_p4/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/ibtivia/cactus_clone/whistle-esp32/esp32/whistle_p4/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
