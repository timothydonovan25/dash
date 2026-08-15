The tasks. Each glues drivers to core logic and publishes into state.

Planned: sensor_hub, gnss_service, imu_service, engine_service,
input_service, ble, logger, storage, health

Cores and priorities are fixed in docs/system-architecture.md section 5.
Core 0 is a real-time island; do not put a radio or a filesystem on it.
