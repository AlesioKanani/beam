cmake_minimum_required(VERSION 3.13)

add_library(wallet_api STATIC
    v6_0/api_base.cpp
    v6_0/api_errors_imp.cpp
    v6_0/wallet_api.cpp
    v6_0/wallet_api_handle.cpp
    v6_0/wallet_api_parse.cpp
    i_wallet_api.cpp
)

if(BEAM_ATOMIC_SWAP_SUPPORT)
    target_sources(wallet_api
        PRIVATE
            v6_0/wallet_api_swap_handle.cpp
            v6_0/wallet_api_swap_parse.cpp
            cli/swap_client.cpp
            cli/swap_eth_client.cpp
    )
endif()

target_link_libraries(wallet_api
    PUBLIC 
        wallet 
        wallet_client
        utility
)

if (NOT BEAM_WALLET_CLIENT_LIBRARY)
    set(WALLET_API_NAME wallet-api${BEAM_TARGET_SUFFIX})

    add_executable(${WALLET_API_NAME} cli/api_cli.cpp cli/api_cli_swap.cpp)

    target_link_libraries(${WALLET_API_NAME}
        PRIVATE
            wallet_api
            http
            cli
    )

    if(LINUX)
        target_link_libraries(${WALLET_API_NAME} 
            PRIVATE
                -static-libstdc++
                -static-libgcc)
    endif()

    if (BEAM_SIGN_PACKAGE AND WIN32)
        add_custom_command(
            TARGET ${WALLET_API_NAME} POST_BUILD
            COMMAND ${SIGNTOOL_EXECUTABLE} sign ${SIGNTOOL_OPTIONS} "${CMAKE_CURRENT_BINARY_DIR}\\${WALLET_API_NAME}.exe"
        )
    endif()
endif()