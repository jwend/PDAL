find_package(MPI REQUIRED)
if(MPI_CXX_FOUND)
#  SET(CMAKE_C_COMPILER ${MPI_C_COMPILER})
#  SET(CMAKE_CXX_COMPILER ${MPI_CXX_COMPILER})
  include_directories(${MPI_CXX_INCLUDE_PATH})
  link_libraries(${MPI_CXX_LIBRARIES})
endif(MPI_CXX_FOUND)

