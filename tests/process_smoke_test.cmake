cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED ENGINE_PATH OR NOT EXISTS "${ENGINE_PATH}")
    message(FATAL_ERROR "ENGINE_PATH must point to bt_download")
endif()
if(NOT DEFINED STATE_PATH OR "${STATE_PATH}" STREQUAL "")
    message(FATAL_ERROR "STATE_PATH is required")
endif()

cmake_path(ABSOLUTE_PATH STATE_PATH NORMALIZE OUTPUT_VARIABLE absolute_state_path)
cmake_path(CONVERT "${absolute_state_path}" TO_CMAKE_PATH_LIST json_state_path)
file(MAKE_DIRECTORY "${absolute_state_path}")

set(input_path "${absolute_state_path}/requests.ndjson")
file(WRITE "${input_path}"
    "{\"jsonrpc\":\"2.0\",\"id\":\"init\",\"method\":\"engine.initialize\",\"params\":{\"protocolVersion\":\"1.0\",\"statePath\":\"${json_state_path}\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":\"stop\",\"method\":\"engine.shutdown\"}\n"
)

execute_process(
    COMMAND "${ENGINE_PATH}"
    WORKING_DIRECTORY "${absolute_state_path}"
    INPUT_FILE "${input_path}"
    OUTPUT_VARIABLE protocol_output
    ERROR_VARIABLE diagnostic_output
    RESULT_VARIABLE process_result
    TIMEOUT 15
)
if(NOT process_result EQUAL 0)
    message(FATAL_ERROR
        "bt_download exited with ${process_result}\n"
        "stdout:\n${protocol_output}\n"
        "stderr:\n${diagnostic_output}"
    )
endif()

if(NOT protocol_output MATCHES "\"method\":\"event.ready\"")
    message(FATAL_ERROR "event.ready was not emitted: ${protocol_output}")
endif()
if(NOT protocol_output MATCHES "\"id\":\"init\"[^\n]*\"result\"")
    message(FATAL_ERROR "initialize response was not emitted: ${protocol_output}")
endif()
if(NOT protocol_output MATCHES "\"id\":\"stop\"[^\n]*\"shutdown\":true")
    message(FATAL_ERROR "shutdown response was not emitted: ${protocol_output}")
endif()
