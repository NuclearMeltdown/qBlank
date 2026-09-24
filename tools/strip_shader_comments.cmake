# A copy of a source file without its // comments, for the build.
#
# The HLSL in src/render/shaders.h is compiled when qBlank starts, so it goes
# into the executable as text -- and about two thirds of that text are
# comments: the measurements and the reasons behind each filter. They belong in
# the source, where they are read. The executable only needs the code.
#
# Every // up to the end of its line goes, inside the HLSL strings and outside
# them. That is only safe because nothing in shaders.h uses // for anything
# else -- no addresses, no /* */ -- and it has to stay that way. The lines
# themselves stay, empty, so a line number in a shader compiler error still
# points at the same line of shaders.h.
#
# The output is only rewritten when its content changes, so a configure run
# does not rebuild what uses it.

function(qblank_strip_comments input output)
  file(READ "${input}" text)
  string(REGEX REPLACE "[ \t]*//[^\r\n]*" "" text "${text}")
  file(WRITE "${output}.tmp" "${text}")
  configure_file("${output}.tmp" "${output}" COPYONLY)
  file(REMOVE "${output}.tmp")
endfunction()
