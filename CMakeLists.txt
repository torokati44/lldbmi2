cmake_minimum_required(VERSION 3.5)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)

if(BUILD_TESTS)
	file(GLOB_RECURSE tests_paths ${CMAKE_CURRENT_SOURCE_DIR}/tests/src/*.cpp)
	foreach( test_path ${tests_paths} )
		get_filename_component(test_source ${test_path} NAME )
		string( REPLACE ".cpp" "" project_name ${test_source} )		# cut off .cpp
		project(${project_name})
		add_executable( ${project_name} ${test_path} )
#		set_target_properties(${project_name} PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")	# for testing arch
	endforeach( test_path ${tests_paths} )
endif(BUILD_TESTS)

project(lldbmi2)
execute_process(COMMAND git log --oneline
				COMMAND wc -l
				COMMAND tr -d " "
				COMMAND tr -d "\n"
				WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
				OUTPUT_VARIABLE version_revision)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/templates/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h)

if(USE_LIB_PATH)
    find_library(LLDB_LIBRARY NAMES lldb LLDB PATHS ${USE_LIB_PATH})
else(USE_LIB_PATH)
    if(APPLE)
    	find_library(LLDB_LIBRARY NAMES LLDB lldb PATHS "/Applications/Xcode.app/Contents/SharedFrameworks" "/Library/Developer/CommandLineTools/Library/PrivateFrameworks")
	else(APPLE)
		find_library(LLDB_LIBRARY NAMES lldb-19 lldb-18 lldb-17 lldb-16 lldb-15 lldb-14 lldb-13 lldb-12 lldb-11 lldb-10 lldb PATHS /usr/lib /usr/local/lib /usr/lib64 /usr/local/lib64)
	endif(APPLE)
endif(USE_LIB_PATH)

mark_as_advanced(LLDB_LIBRARY)
get_filename_component(LLDB_LIBRARY_PATH ${LLDB_LIBRARY} DIRECTORY)
set(CMAKE_BUILD_RPATH "/Applications/Xcode.app/Contents/SharedFrameworks;/Library/Developer/CommandLineTools/Library/PrivateFrameworks")

message(LLDB_LIBRARY: ${LLDB_LIBRARY} " @ " ${USE_LIB_PATH} " ^ " ${LLDB_LIBRARY_PATH} )

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/includes)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)

file(GLOB_RECURSE lldbmi2_sources ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB_RECURSE extern_sources ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_executable(${PROJECT_NAME} ${lldbmi2_sources} ${extern_sources})
target_link_libraries(${PROJECT_NAME} ${LLDB_LIBRARY})
if(WIN32)
  target_link_libraries(${PROJECT_NAME} wsock32 ws2_32)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -pedantic -pthread")

install(TARGETS ${PROJECT_NAME} DESTINATION bin)

unset (USE_LIB_PATH CACHE)
unset (LLDB_LIBRARY CACHE)
