add_library(msh_mesh_shape_interplay SHARED "mesh_shape_interplay.cpp")
target_include_directories( msh_mesh_shape_interplay PUBLIC ${KERNEL_DIR} ${CMAKE_SOURCE_DIR})
target_link_libraries( msh_mesh_shape_interplay PRIVATE pic_rc )

add_library(msh_current_deposition SHARED "current_deposition.cpp")
target_include_directories( msh_current_deposition PUBLIC ${KERNEL_DIR} ${CMAKE_SOURCE_DIR})
target_link_libraries( msh_current_deposition PRIVATE pic_rc )

if (${ENABLE_TESTS})
  test_gen(msh mesh_shape_interplay) # TODO
  test_gen(msh current_deposition silopp) # DONE

  msperf_gen(msh mesh_shape_interplay)
endif()
