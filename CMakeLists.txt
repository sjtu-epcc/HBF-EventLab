cmake_minimum_required(VERSION 3.20)
project(HBFEventLab VERSION 0.5.0 LANGUAGES CXX)
add_library(hbf_eventlab
    src/configuration/simulation_configuration.cpp
    src/host/host_configuration.cpp
    src/architecture/topology_configuration.cpp
    src/architecture/ucie_profile.cpp
    src/statistics/collector.cpp
    src/statistics/pending.cpp
    src/statistics/media.cpp
    src/statistics/reads.cpp
    src/host/global_request_splitter.cpp
    src/host/retention_store.cpp
    src/host/recovery_manager.cpp
    src/host/host_driver.cpp
    src/host/write_retry.cpp
    src/runtime/simulation_runner.cpp
    src/architecture/execution_configuration.cpp
    src/runtime/bank_cache.cpp
    src/runtime/device_driver.cpp
    src/architecture/device_description.cpp
    src/policies/reference_address_layout.cpp
    src/policies/round_robin_interleaver.cpp
    src/runtime/zone_mapping.cpp
    src/runtime/block_replacement.cpp
    src/runtime/capacity_retirement_coordinator.cpp
    src/runtime/nand_media_state.cpp
    src/runtime/write_path.cpp
    src/runtime/read_path.cpp
    src/runtime/command_admission.cpp
    src/runtime/admission_wait_registry.cpp
    src/runtime/write_assembly.cpp
    src/runtime/block_write_order.cpp
    src/infrastructure/event_queue.cpp
    src/policies/read_dispatch.cpp
)
add_library(HBFEventLab::core ALIAS hbf_eventlab)
target_include_directories(hbf_eventlab PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hbf_eventlab PUBLIC cxx_std_17)
set_target_properties(hbf_eventlab PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(hbf_eventlab PUBLIC /utf-8 /EHsc PRIVATE /W4 /permissive-)
else()
    target_compile_options(hbf_eventlab PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_library(hbf_eventlab_workload_io src/adapters/workload_reader.cpp)
target_link_libraries(hbf_eventlab_workload_io PUBLIC HBFEventLab::core)
add_executable(hbf_eventlab_demo examples/foundations_demo.cpp)
target_link_libraries(hbf_eventlab_demo PRIVATE HBFEventLab::core)
add_executable(hbf_eventlab_device_demo examples/device_loop_demo.cpp)
target_link_libraries(hbf_eventlab_device_demo PRIVATE HBFEventLab::core)
add_executable(hbf_eventlab_cli examples/minimal_loop.cpp)
target_link_libraries(hbf_eventlab_cli PRIVATE hbf_eventlab_workload_io)
include(CTest)
include(cmake/ExperimentPython.cmake)
if(BUILD_TESTING)
    add_executable(hbf_eventlab_read_scheduling_tests tests/read_scheduling.cpp)
    target_link_libraries(hbf_eventlab_read_scheduling_tests PRIVATE HBFEventLab::core)
    foreach(scenario policy_order window_reasons window_stale cache_bypass cache_removal terminal_read
            snapshot observational guards custom_reason)
        add_test(NAME read_scheduling_${scenario} COMMAND hbf_eventlab_read_scheduling_tests ${scenario})
        set_tests_properties(read_scheduling_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    # Core-only users opt out explicitly; requested verification must never disappear.
    hbf_eventlab_find_experiment_python()
    if(HBF_EVENTLAB_EXPERIMENT_TESTS)
        add_test(NAME experiment_common_unit COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_experiment_common.py")
        add_test(NAME experiment_python_configuration COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/experiment-python-check-$<CONFIG>"
            "-DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_experiment_python.cmake")
        set_tests_properties(experiment_common_unit experiment_python_configuration PROPERTIES TIMEOUT 60)
        add_test(NAME experiment_read_scheduling_unit COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_read_scheduling_experiment.py")
        add_test(NAME experiment_read_scheduling_integration COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_read_scheduling_experiment.py"
            --cli "$<TARGET_FILE:hbf_eventlab_cli>" --output "${CMAKE_CURRENT_BINARY_DIR}/read-scheduling-test-$<CONFIG>")
        set_tests_properties(experiment_read_scheduling_unit experiment_read_scheduling_integration PROPERTIES TIMEOUT 180)
        add_test(NAME experiment_capacity_unit COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capacity_experiment.py")
        add_test(NAME experiment_capacity_integration COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capacity_experiment.py"
            --cli "$<TARGET_FILE:hbf_eventlab_cli>" --output "${CMAKE_CURRENT_BINARY_DIR}/capacity-test-$<CONFIG>")
        set_tests_properties(experiment_capacity_unit experiment_capacity_integration PROPERTIES TIMEOUT 120)
        add_test(NAME experiment_interleave_arrival_unit COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_interleave_arrival_experiment.py")
        add_test(NAME experiment_interleave_arrival_integration COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_interleave_arrival_experiment.py"
            --cli "$<TARGET_FILE:hbf_eventlab_cli>" --output "${CMAKE_CURRENT_BINARY_DIR}/interleave-arrival-test-$<CONFIG>")
        set_tests_properties(experiment_interleave_arrival_unit experiment_interleave_arrival_integration PROPERTIES TIMEOUT 180)
        add_test(NAME experiment_arrival_policy_unit COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_arrival_policy_experiment.py")
        add_test(NAME experiment_arrival_policy_integration COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_arrival_policy_experiment.py"
            --cli "$<TARGET_FILE:hbf_eventlab_cli>" --output "${CMAKE_CURRENT_BINARY_DIR}/arrival-policy-test-$<CONFIG>")
        set_tests_properties(experiment_arrival_policy_unit experiment_arrival_policy_integration PROPERTIES TIMEOUT 180)
    endif()
    add_executable(hbf_eventlab_pending_statistics_tests tests/pending_statistics.cpp)
    target_link_libraries(hbf_eventlab_pending_statistics_tests PRIVATE HBFEventLab::core)
    foreach(scenario lifecycle occupancy rejected timeout_reuse failure same_tick_burst commit_failure
            snapshot teardown identity guarded_attach device_delivery device_limit read_only curve_reconstruction)
        add_test(NAME pending_statistics_${scenario} COMMAND hbf_eventlab_pending_statistics_tests ${scenario})
        set_tests_properties(pending_statistics_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(hbf_eventlab_host_send_policy_tests tests/host_send_policy.cpp)
    target_link_libraries(hbf_eventlab_host_send_policy_tests PRIVATE HBFEventLab::core)
    foreach(scenario order fcfs fill_open blocked_open release_order same_tick dependency_age not_taken retry_tail
            same_tick_retry timeout_tail timeout_unsent replay_followers burst_mixed cross_global_burst channels)
        add_test(NAME host_send_${scenario} COMMAND hbf_eventlab_host_send_policy_tests ${scenario})
        set_tests_properties(host_send_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(hbf_eventlab_configuration_tests tests/configuration.cpp)
    target_link_libraries(hbf_eventlab_configuration_tests PRIVATE HBFEventLab::core)
    foreach(scenario defaults parse_override parser_errors unknown_fields numeric grades grade_limits mocs_ranges
            small_read small_write small_burst capabilities geometry topology capacity buffers timing zeros
            policies custom_policy address faults roundtrip frozen all_keys default_equivalence)
        add_test(NAME configuration_${scenario} COMMAND hbf_eventlab_configuration_tests ${scenario})
        set_tests_properties(configuration_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    foreach(scenario precedence roundtrip invalid_grade invalid_mocs unknown duplicate policy buffer collision workload_guard config_guard read_small write_small burst_small host_default host_fill_open host_override)
        add_test(NAME configuration_cli_${scenario} COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:hbf_eventlab_cli>"
            "-DWORKLOAD=${CMAKE_CURRENT_SOURCE_DIR}/examples/workloads/normal.txt"
            "-DCASE=${scenario}" "-DCASE_DIR=${CMAKE_CURRENT_BINARY_DIR}/configuration-${scenario}-$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_configuration_cli.cmake")
        set_tests_properties(configuration_cli_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(hbf_eventlab_statistics_tests tests/statistics.cpp)
    target_link_libraries(hbf_eventlab_statistics_tests PRIVATE HBFEventLab::core)
    foreach(scenario boundaries cross_global_burst multi_channel pending_retry timeout_retry replay
            unfinished_resume ingress_wait cancelled_future partial_future_cancel mismatch backpressure observational
            event_time zero_and_status guards_empty stalled bank_lifetime bank_limit bank_guards)
        add_test(NAME statistics_${scenario} COMMAND hbf_eventlab_statistics_tests ${scenario})
        set_tests_properties(statistics_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()
    foreach(mode normal limit assembly_limit)
        set(stats_args "")
        set(stats_exit 0)
        if(mode STREQUAL "limit")
            set(stats_args --max-tick 100)
            set(stats_exit 4)
        elseif(mode STREQUAL "assembly_limit")
            set(stats_args --max-tick 10000)
            set(stats_exit 4)
        endif()
        add_test(NAME statistics_cli_${mode} COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:hbf_eventlab_cli>"
            "-DWORKLOAD=${CMAKE_CURRENT_SOURCE_DIR}/examples/workloads/normal.txt"
            "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/statistics-${mode}-$<CONFIG>.json"
            "-DEXPECTED=${stats_exit}" "-DARGS=${stats_args}" "-DMODE=${mode}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_statistics_cli.cmake")
        set_tests_properties(statistics_cli_${mode} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(hbf_eventlab_host_retry_tests tests/host_retry.cpp)
    target_link_libraries(hbf_eventlab_host_retry_tests PRIVATE HBFEventLab::core)
    foreach(scenario pending_root_first pending_follower_first pending_late_followers pending_exhausted
            pending_disabled pending_progress pending_burst timeout_late_fragments timeout_release
            timeout_missing_retention timeout_missing_coverage unexplained_order real_pending
            real_timeout_success real_timeout_budget pending_during_replay timeout_during_replay)
        add_test(NAME host_retry_${scenario} COMMAND hbf_eventlab_host_retry_tests ${scenario})
        set_tests_properties(host_retry_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()

    function(host_cli_case name input expected text)
        add_test(NAME host_cli_${name} COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:hbf_eventlab_cli>"
            "-DWORKLOAD=${CMAKE_CURRENT_SOURCE_DIR}/examples/workloads/${input}.txt"
            "-DEXPECTED=${expected}" "-DEXPECT_TEXT=${text}" "-DARGS=${ARGN}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli.cmake")
        set_tests_properties(host_cli_${name} PROPERTIES TIMEOUT 60)
    endfunction()
    host_cli_case(normal normal 0 "STOP Completed workload=Success")
    host_cli_case(burst normal 0 "HOST handed=130" --burst)
    host_cli_case(recovery recovery 0 "recovery_tasks=1 replay_passes=1" --fail-sequence 2 --open 4)
    host_cli_case(merged_recovery recovery 0 "recovery_tasks=1 replay_passes=2" --fail-sequence 2,1 --open 4)
    host_cli_case(persistent recovery 2 "RecoveryBudgetExhausted" --persistent-page 0)
    host_cli_case(missing recovery 2 "MissingRecoveryData" --fail-sequence 0 --retention 0)
    host_cli_case(timeout partial 2 "WriteStatus:5")
    host_cli_case(limit normal 4 "STOP LimitReached" --max-tick 100)
    add_executable(hbf_eventlab_host_tests tests/host.cpp)
    target_link_libraries(hbf_eventlab_host_tests PRIVATE hbf_eventlab_workload_io)
    foreach(scenario cancelled_future_release late_read_second_pass delayed_old_attempts ingress_old_attempts burst_recovery multi_channel_recovery prior_success_retained abort_new_content teardown_host runner_device_activity round_limit normal sixteen_channels burst_compare burst_cross_global input_validation content_validation burst_validation splitter parser hints ordered_reads open_limit fill_open recovery_once follower_before_root replay_merge persistent_failure missing_retention replacement_recovery zero_budget partial_timeout mocs_one mocs_partial empty not_started stalled_dependency time_limit event_limit same_tick_limit ordered_pending read_during_recovery versions data_mismatch delayed_release retention_pins recovery_snapshot policy_guard)
        add_test(NAME host_${scenario} COMMAND hbf_eventlab_host_tests ${scenario})
        set_tests_properties(host_${scenario} PROPERTIES TIMEOUT 60)
    endforeach()
    add_test(NAME device_demo COMMAND hbf_eventlab_device_demo)
    add_executable(hbf_eventlab_device_tests tests/device.cpp)
    target_link_libraries(hbf_eventlab_device_tests PRIVATE HBFEventLab::core)
    foreach(scenario auto_write_read split_write bank_parallel bank_serial mocs_ingress rx_reservation
            window_batch_timer window_threshold window_channel_isolation batch_pending batch_missing cache_hit
            miss_becomes_hit ordered_pages ordered_error pending_order_diagnostic tx_cache_pressure cache_rewrite
            pending_frozen pending_tx_full rejected_batch_no_effect ingress_bypass fault_fanout replacement_replay
            remap_cache duplicate_handoff response_feedback configuration cache_pin_invalidation teardown unsupported
            policies invalid_policy link_latency pending_recheck deadline_link_boundary
            sense_order_policy sense_order_blocked ordered_late_error rewrite_saved_read retirement_loop
            teardown_cache_hit callback_limit multi_channel_bandwidth)
        add_test(NAME device_${scenario} COMMAND hbf_eventlab_device_tests ${scenario})
        set_tests_properties(device_${scenario} PROPERTIES TIMEOUT 20)
    endforeach()
    add_executable(hbf_eventlab_lifecycle_tests tests/lifecycle.cpp)
    target_link_libraries(hbf_eventlab_lifecycle_tests PRIVATE HBFEventLab::core)
    foreach(scenario issue_rollback issue_sampling guard_multiple_readers pending_fixed_missing physical_zone_wakeup fixture_capabilities
                     event_rounds deadline_batch deadline_limits deadline_exception event_cancel_limits
                     wait_targeting wait_race_cancel wait_reregister read_accept_issue read_bounded_results
                     read_order read_order_errors pending_order_diagnostic pending_capacity
                     read_failure_revalidation read_repair_revalidation read_quiesce_drain read_stale_candidate
                     write_probes program_stale_candidate write_ready_notifications physical_release_notification
                     deadline_credit_chain deadline_resident_only deadline_two_dlus window_membership
                     group_channel_isolation teardown_queued_read)
        add_test(NAME lifecycle_${scenario} COMMAND hbf_eventlab_lifecycle_tests ${scenario})
        set_tests_properties(lifecycle_${scenario} PROPERTIES TIMEOUT 20)
    endforeach()
    add_executable(hbf_eventlab_foundation_tests tests/foundations.cpp)
    target_link_libraries(hbf_eventlab_foundation_tests PRIVATE HBFEventLab::core)
    foreach(scenario address_mapping admission write_lifecycle events_dispatch)
        add_test(NAME ${scenario} COMMAND hbf_eventlab_foundation_tests ${scenario})
    endforeach()
    add_executable(hbf_eventlab_write_path_tests tests/write_path.cpp)
    add_executable(hbf_eventlab_read_path_tests tests/read_path.cpp)
    target_link_libraries(hbf_eventlab_read_path_tests PRIVATE HBFEventLab::core)
    foreach(scenario basic_errors shared_credit pending_forward media_lifetime recovery_reads merged_read_gate
                     replacement_timeout_gate retirement_reads remap_read_generation event_error_order
                     deadline_forward quiesce_reads read_issue_snapshot ingress_scope read_teardown)
        add_test(NAME ${scenario} COMMAND hbf_eventlab_read_path_tests ${scenario})
    endforeach()
    add_executable(hbf_eventlab_capacity_tests tests/capacity_retirement.cpp)
    target_link_libraries(hbf_eventlab_capacity_tests PRIVATE HBFEventLab::core)
    foreach(scenario capacity_compensation capacity_density capacity_atomicity capacity_hole_failure capacity_lease_boundary capacity_chain)
        add_test(NAME ${scenario} COMMAND hbf_eventlab_capacity_tests ${scenario})
    endforeach()
    target_link_libraries(hbf_eventlab_write_path_tests PRIVATE HBFEventLab::core)
    foreach(scenario replacement_pipeline overlap_restart timeout_retry replay_prefix replacement_failure admission_boundaries
                     replay_merge replay_replacement_merge replay_remap_generation retirement_write_swap)
        add_test(NAME ${scenario} COMMAND hbf_eventlab_write_path_tests ${scenario})
    endforeach()
endif()
