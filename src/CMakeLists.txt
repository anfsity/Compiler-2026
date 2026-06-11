# src/CMakelists.txt

add_library(headers INTERFACE)
target_include_directories(headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(headers INTERFACE fmt)

target_compile_options(headers INTERFACE
    -Wall
    -Wno-register
    -Wextra
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>
)

# generate lexer/parser
set(LEXER_SRC FE/sysy.lx)
set(YACC_SRC FE/sysy.y)

# generate the lexer and parser files
flex_target(Lexer ${LEXER_SRC} ${CMAKE_CURRENT_BINARY_DIR}/sysy.lex.cpp)
bison_target(Parser ${YACC_SRC} ${CMAKE_CURRENT_BINARY_DIR}/sysy.tab.cpp COMPILE_FLAGS "-Wcounterexamples")
add_flex_bison_dependency(Lexer Parser)

message(STATUS "[INFO]  Generated lexer: ${CMAKE_CURRENT_BINARY_DIR}/sysy.lex.cpp")
message(STATUS "[INFO]  Generated parser: ${CMAKE_CURRENT_BINARY_DIR}/sysy.tab.cpp")
message(STATUS "[INFO]  Generated lexer outputs ${FLEX_Lexer_OUTPUTS}")
message(STATUS "[INFO]  Generated parser outputs ${BISON_Parser_OUTPUT_SOURCE}")


file(GLOB_RECURSE CORE_SOURCES
    "base/*.cpp"
    "FE/*.cpp"
    "high/*.cpp"
    "mid/*.cpp"
    "opt/*.cpp"
    "helper/*.cpp"
)

list(APPEND CORE_SOURCES
    ${FLEX_Lexer_OUTPUTS}
    ${BISON_Parser_OUTPUT_SOURCE}
)

add_library(compiler_core STATIC ${CORE_SOURCES})

target_include_directories(compiler_core PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}  # cmake-build/src/* for generated lexer/parser
)

# compiler core link libraries
target_link_libraries(compiler_core PUBLIC
    pthread
    dl
    headers
)

# executable
add_executable(compiler
    main.cpp
)

target_include_directories(compiler PRIVATE $ENV{CDE_INCLUDE_PATH})
# compiler link libraries
target_link_libraries(compiler PRIVATE compiler_core)
