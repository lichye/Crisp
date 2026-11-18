add_library(crisp_verification STATIC verify.cpp proofs.cpp)
target_include_directories(crisp_verification PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crisp_verification PUBLIC veval_verification veval_commands)

add_executable(crisp_driver main.cpp)
target_link_libraries(crisp_driver PRIVATE crisp_verification)

add_executable(crisp_verify_tests EXCLUDE_FROM_ALL ../tests/verify.cpp)
target_link_libraries(crisp_verify_tests PRIVATE crisp_verification)
