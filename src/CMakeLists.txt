# src/CMakelists.txt
# generate lexer/parser
set(LEXER_SRC FE/sysy.lx)
set(YACC_SRC FE/sysy.y)

# generate the lexer and parser files
flex_target(Lexer ${LEXER_SRC} ${CMAKE_CURRENT_BINARY_DIR}/sysy.lex.cpp)
bison_target(Parser ${YACC_SRC} ${CMAKE_CURRENT_BINARY_DIR}/sysy.tab.cpp COMPILE_FLAGS "-Wcounterexamples")
add_flex_bison_dependency(Lexer Parser)

message(STATUS "[INFO]  Generated lexer: ${CMAKE_CURRENT_BINARY_DIR}/sysy.lex.cpp")
message(STATUS "[INFO]  Generated parser: ${CMAKE_CURRENT_BINARY_DIR}/sysy.tab.cpp")
message(STATUS "[INFO]  Generated lexer outpus ${FLEX_Lexer_OUTPUTS}")
message(STATUS "[INFO]  Generated parser outpus ${BISON_Parser_OUTPUT_SOURCE}")

set(CORE_SOURCES
    ${FLEX_Lexer_OUTPUTS}
    ${BISON_Parser_OUTPUT_SOURCE}
)

add_library(compiler_core STATIC ${CORE_SOURCES})
set_target_properties(compiler_core PROPERTIES CXX_CLANG_TIDY "")

target_include_directories(compiler_core PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}  # cmake-build/src/* for generated lexer/parser
)

target_sources(compiler_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# compiler core link libraries
target_link_libraries(compiler_core PUBLIC
    pthread
    dl
    headers
)

# complie options
target_compile_options(compiler_core PRIVATE
    -Wall
    -Wno-register
    -Wextra
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>
)

# executable
add_executable(compiler
    main.cpp
    opt/high/registration.cpp
)
target_compile_options(compiler PRIVATE
    -Wall
    -Wno-register
    -Wextra
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:MinSizeRel>:-O2>
)
target_include_directories(compiler PRIVATE $ENV{CDE_INCLUDE_PATH})

# compiler link libraries
target_link_libraries(compiler PRIVATE compiler_core)
# ?
target_link_directories(compiler PRIVATE $ENV{CDE_LIBRARY_PATH}/native)
