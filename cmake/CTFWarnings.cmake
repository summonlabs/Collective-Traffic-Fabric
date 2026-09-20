# Collective Traffic Fabric - the project's warning gate.
# Copyright 2026 Summon Software Labs.
#
# Included from the top level build.  Kept in a function so it can be applied to
# any target, including targets that are created later (tests, tools, examples).
# The exported package never references this module: warnings are a build
# concern, not part of the library's usage requirements.
include_guard(GLOBAL)

function(ctf_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /permissive-
      /Zc:__cplusplus
      /Zc:preprocessor
      /utf-8
      /wd4251   # dll interface on std:: types: no dll boundary is exported
      /wd4141)  # 'inline' repeated on a member definition: harmless
    if(CTF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
      -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
      -Wformat=2 -Wimplicit-fallthrough -Werror=return-type)
    if(CTF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
