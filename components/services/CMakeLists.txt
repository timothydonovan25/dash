idf_component_register(
    SRCS "src/queues.cpp"
         "src/ctrl_service.cpp"
         "src/imu_service.cpp"
         "src/gnss_service.cpp"
         "src/logic_service.cpp"
         "src/input_service.cpp"
         "src/render_service.cpp"
         "src/keeper_service.cpp"
         "src/logger_service.cpp"
         "src/storage_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core dash_hal ui bsp drivers esp_timer)
