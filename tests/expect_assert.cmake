# Runs a program that must die on an engine assertion.
#
# ctest cannot express this on its own. PASS_REGULAR_EXPRESSION does not
# override a process that a signal stopped, so the test fails even when the
# message is there. WILL_FAIL passes whether the message appeared or not, which
# makes it useless here, because the failure this test guards against is a
# process that dies with nothing to say.
#
# This script checks both halves. The program must not exit cleanly, and it must
# have reported through the engine log before it stopped.
#
# Set EXE to the program to run.

if(NOT DEFINED EXE)
    message(FATAL_ERROR "Set EXE to the program to run.")
endif()

execute_process(
    COMMAND "${EXE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE captured_output
    ERROR_VARIABLE captured_error
)

set(combined "${captured_output}${captured_error}")
message(STATUS "The program said:\n${combined}")

if(result EQUAL 0)
    message(FATAL_ERROR "${EXE} exited cleanly. The assertion did not fire.")
endif()

# Only engine::detail::assert_failed in src/core/assert.h writes this.
string(FIND "${combined}" "Assertion failed: " found)
if(found EQUAL -1)
    message(FATAL_ERROR
        "${EXE} stopped with no engine assertion message.\n"
        "EnTT is not reaching ENGINE_ASSERT. See src/core/entt.h.")
endif()

message(STATUS "The engine assertion reported before the process stopped.")
