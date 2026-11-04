option(HBF_EVENTLAB_EXPERIMENT_TESTS "Register Python experiment tests (requires Python 3.10+)" ON)

function(hbf_eventlab_find_experiment_python)
    if(NOT HBF_EVENTLAB_EXPERIMENT_TESTS)
        message(STATUS "HBF EventLab: experiment tests explicitly disabled")
        return()
    endif()
    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR
            "HBF EventLab experiment tests require Python 3.10+. Install it or set Python3_EXECUTABLE. "
            "For C++-only testing, explicitly set -DHBF_EVENTLAB_EXPERIMENT_TESTS=OFF.")
    endif()
    set(Python3_EXECUTABLE "${Python3_EXECUTABLE}" PARENT_SCOPE)
    message(STATUS "HBF EventLab: experiment tests enabled with ${Python3_EXECUTABLE}")
endfunction()
