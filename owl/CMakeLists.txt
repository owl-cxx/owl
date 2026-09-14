add_library(owl INTERFACE)
add_library(${PROJECT_NAME}::owl ALIAS owl)

target_compile_features(owl INTERFACE cxx_std_23)

target_include_directories(owl INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_sources(owl INTERFACE
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/owl.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/detail.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/server.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/core/drivers.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/core/method.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/core/metrics.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/core/state.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/core/token.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/core/config.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/coro/loop_reactor.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/coro/loop_scheduler.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/extract/extractors.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/extract/from_context.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/extract/parse.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/http/cookie.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/http/detail/finish.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/http/policy.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/http/request.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/http/response.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/routing/detail/endpoint.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/routing/detail/layer.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/routing/detail/pattern.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/routing/detail/traits.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/routing/detail/trie.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/routing/cors.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/routing/middleware.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/routing/router.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/util/base64.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/util/pool_map.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/util/util.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/ws/message.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/ws/socket.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/ws/controller.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/ws/detail/handshake.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/ws/detail/session.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/ws/detail/engine.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/ws/detail/upgrade.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/ws/detail/views.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/owl/ws/detail/routes.h"
)

if (APPLE AND NOT OPENSSL_ROOT_DIR)
    execute_process(
            COMMAND brew --prefix openssl@3
            OUTPUT_VARIABLE OPENSSL_ROOT_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
endif ()

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(H2O REQUIRED IMPORTED_TARGET libh2o-evloop)
pkg_check_modules(WSLAY REQUIRED IMPORTED_TARGET libwslay)

include(FetchContent)
FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

target_compile_definitions(owl INTERFACE H2O_USE_LIBUV=0)

if (OWL_ENABLE_PROMETHEUS)
    # Prometheus itself depends on nothing; owl optionally depends on it so
    # dispatch can record HTTP RED under the OWL_ENABLE_PROMETHEUS macro.
    target_link_libraries(owl INTERFACE prometheus)
    target_compile_definitions(owl INTERFACE OWL_ENABLE_PROMETHEUS)
endif ()

if (OWL_ENABLE_SQLITE OR OWL_ENABLE_POSTGRESQL)
    # sql is added before owl (owl must link it); the OWL_ENABLE_* macros
    # ride on the driver targets and arrive transitively.
    target_link_libraries(owl INTERFACE sql)
endif ()

if (OWL_ENABLE_REDIS)
    # redis is added before owl (owl must link it); OWL_ENABLE_REDIS rides
    # on the redis target and arrives transitively.
    target_link_libraries(owl INTERFACE redis)
endif ()

target_link_libraries(owl INTERFACE
        PkgConfig::H2O
        PkgConfig::WSLAY
        coro
        fstr
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
)

enable_language(C)
add_executable(owl_bench "${CMAKE_CURRENT_SOURCE_DIR}/bench/owl_bench.cpp")
target_link_libraries(owl_bench PRIVATE ${PROJECT_NAME}::owl)

if (OWL_ENABLE_PROMETHEUS)
    add_executable(owl_bench_prometheus "${CMAKE_CURRENT_SOURCE_DIR}/bench/owl_bench_prometheus.cpp")
    target_link_libraries(owl_bench_prometheus PRIVATE ${PROJECT_NAME}::owl)
endif ()

add_executable(raw_h2o_bench "${CMAKE_CURRENT_SOURCE_DIR}/bench/raw_h2o_server.c")
target_compile_definitions(raw_h2o_bench PRIVATE H2O_USE_LIBUV=0)
target_link_libraries(raw_h2o_bench PRIVATE
        PkgConfig::H2O
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
)

if (BUILD_TESTING)
    function(owl_add_test name)
        add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.cpp")
        target_link_libraries(${name}
                PRIVATE
                ${PROJECT_NAME}::owl
                GTest::gtest_main)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    owl_add_test(test_owl)
    owl_add_test(test_method)
    owl_add_test(test_request)
    owl_add_test(test_parse_query)
    owl_add_test(test_pool_map)
    owl_add_test(test_response)
    owl_add_test(test_cookie)
    owl_add_test(test_router)
    owl_add_test(test_router_rejections)
    owl_add_test(test_middleware)
    owl_add_test(test_cors)
    owl_add_test(test_base64)
    owl_add_test(test_loop_scheduler)
    owl_add_test(test_loop_reactor)
    owl_add_test(test_builder)
    owl_add_test(test_config)
    owl_add_test(test_ws_message)
    owl_add_test(test_ws_router)
    owl_add_test(test_ws)

    if (OWL_ENABLE_PROMETHEUS)
        owl_add_test(test_prometheus_dispatch)
    endif ()

    if (OWL_ENABLE_SQLITE OR OWL_ENABLE_POSTGRESQL)
        # Links owl::owl + owl::sql; registered here because sql is added
        # before owl and cannot see the owl target. Shares sql's test
        # support (the temp database file). Its redis cases run when
        # OWL_ENABLE_REDIS is on alongside a sql driver.
        owl_add_test(test_owl_wiring)
        target_include_directories(test_owl_wiring PRIVATE "${PROJECT_SOURCE_DIR}/sql/tests")
    endif ()

    # A compile-fail test passes when its build fails. Given a message, it
    # passes only when that message is in the compiler's output, so a case
    # failing for some other reason -- a typo, a missing include -- is
    # caught rather than counted.
    function(owl_add_compile_fail_test name)
        add_executable(${name} EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/${name}.cpp")
        target_link_libraries(${name} PRIVATE ${PROJECT_NAME}::owl)
        add_test(NAME ${name}
                COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
                        --target ${name} --config $<CONFIG>)
        if (ARGC GREATER 1)
            set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${ARGV1}")
        else ()
            set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
        endif ()
    endfunction()

    owl_add_compile_fail_test(rejects_discarded_builder)
    owl_add_compile_fail_test(rejects_router_without_state)
    owl_add_compile_fail_test(rejects_wrong_state_type)
    owl_add_compile_fail_test(rejects_nest_of_other_state)
    owl_add_compile_fail_test(rejects_mutable_loop_ref)
    owl_add_compile_fail_test(rejects_ws_view_extractor)
    owl_add_compile_fail_test(rejects_ws_missing_on_message)

    owl_add_compile_fail_test(rejects_bad_route_pattern "invalid route pattern")
    owl_add_compile_fail_test(rejects_bad_ws_pattern "invalid route pattern")
    owl_add_compile_fail_test(rejects_bad_nest_prefix "invalid nest prefix")
    owl_add_compile_fail_test(rejects_nest_prefix_with_parameter "a nest prefix must be literal")
    owl_add_compile_fail_test(rejects_undeclared_path_parameter "path parameter this route pattern does not declare")
    owl_add_compile_fail_test(rejects_handler_return_type "a handler must return Response or coro::task<Response>")
    owl_add_compile_fail_test(rejects_layer_shape "layer expects task<Response>")
    owl_add_compile_fail_test(rejects_ws_bad_on_connect "on_connect must be on_connect")
    owl_add_compile_fail_test(rejects_ws_bad_on_disconnect "on_disconnect must be on_disconnect")
    owl_add_compile_fail_test(rejects_ws_extractors_not_tuple "Extractors must be a std::tuple")
    owl_add_compile_fail_test(rejects_ws_controller_ctor_args "the controller cannot be constructed from these arguments")
    owl_add_compile_fail_test(rejects_ws_controller_undeclared_parameter "path parameter this route pattern does not declare")
    owl_add_compile_fail_test(rejects_ws_controller_view_extractor "a WebSocket handler outlives its request")
endif ()
