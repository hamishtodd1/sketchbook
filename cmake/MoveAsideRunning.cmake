# Makes room for a fresh executable at EXE, even while the old one is running.
#
# Windows refuses to overwrite or delete the file of a program that is running,
# which is what made the link fail whenever the app was open. It does let that
# file be *renamed*, and the running copy carries on from the renamed file. So
# the old executable is deleted if it can be, and moved aside if it cannot, and
# the linker writes the new one under the usual name. Whatever is running keeps
# running; launching again gets the new build.
#
# Copies moved aside by earlier builds are deleted here too, as soon as nothing
# is running them any more.

get_filename_component(dir "${EXE}" DIRECTORY)
get_filename_component(name "${EXE}" NAME_WE)

file(GLOB stale "${dir}/${name}.*.old")
foreach(old IN LISTS stale)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${old}" OUTPUT_QUIET ERROR_QUIET)
endforeach()

if(EXISTS "${EXE}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${EXE}" OUTPUT_QUIET ERROR_QUIET)
endif()
if(EXISTS "${EXE}")
  string(TIMESTAMP stamp "%Y%m%d-%H%M%S")
  string(RANDOM LENGTH 4 ALPHABET 0123456789abcdef salt)
  file(RENAME "${EXE}" "${dir}/${name}.${stamp}-${salt}.old")
  message(STATUS "${name} is running; moved it aside so the new build can be linked")
endif()
