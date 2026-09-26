# Fuzz targets, for libFuzzer: configure a build of their own with
#   -DCHEVRON_FUZZ=ON -DCHEVRON_TESTS=OFF
#   -DCMAKE_CXX_FLAGS="<the usual> -fsanitize=address,undefined,fuzzer-no-link"
# and run, say, ./test/fuzz/chevron-fuzz-parser -max_total_time=60.
foreach(target parser read)
  add_executable(chevron-fuzz-${target} ${target}.cc)
  target_link_libraries(chevron-fuzz-${target} PRIVATE chevron)
  # libFuzzer's runtime is built with libstdc++; beside libc++, in namespaces of their own.
  target_link_options(chevron-fuzz-${target} PRIVATE -fsanitize=fuzzer)
  target_link_libraries(chevron-fuzz-${target} PRIVATE /usr/lib/x86_64-linux-gnu/libstdc++.so.6)
endforeach()
