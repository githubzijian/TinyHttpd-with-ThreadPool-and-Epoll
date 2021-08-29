cmake_minimum_required(VERSION 2.8)
project(MyHttpd C)

set(CMAKE_C_STANDARD 99)

aux_source_directory(./ DIR_SRCS )

add_executable(MyHttpd test.c ${DIR_SRCS})

SET(CMAKE_C_FLAGS "-std=c99")
SET(CMAKE_C_FLAGS -pthread)
