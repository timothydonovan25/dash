idf_component_register(
    SRCS "src/font5x7.cpp"
         "src/framebuffer_canvas.cpp"
         "src/icons.cpp"
         "src/widgets.cpp"
         "src/screens_cruise.cpp"
         "src/screens_track.cpp"
         "src/screens_stat.cpp"
         "src/screens_diag.cpp"
         "src/screen_config.cpp"
         "src/app.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core)
