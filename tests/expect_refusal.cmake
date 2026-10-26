# Builds one compile-fail probe and passes only if the build fails with the
# expected diagnostic. A probe that fails for any other reason -- a typo, a
# missing include -- does not count as the refusal it was written for.
#
#   cmake -DBUILD_DIR=<dir> -DTARGET=<target> -DEXPECT=<regex> -P expect_refusal.cmake
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}"
    OUTPUT_VARIABLE OUT
    ERROR_VARIABLE OUT
    RESULT_VARIABLE STATUS)
if(STATUS EQUAL 0)
    message(FATAL_ERROR "${TARGET} compiled; it was expected to be refused")
endif()
if(NOT OUT MATCHES "${EXPECT}")
    message(FATAL_ERROR
        "${TARGET} failed, but not with '${EXPECT}':\n${OUT}")
endif()
