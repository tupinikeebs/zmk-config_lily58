# Vendored Azoteq TPS43 trackpad driver (from the Sofle config, originally
# stelmakhdigital/zmk_driver_azoteq). CONFIG_INPUT_TPS43 is only enabled on the
# half whose overlay declares an `azoteq,tps43` node (the right/peripheral).
if(CONFIG_INPUT_TPS43)
  # Named explicitly instead of plain zephyr_library(): that helper derives the
  # library name from this directory's path, so a checkout under a path with a
  # SPACE produces an invalid CMake target name.
  zephyr_library_named(lily58_tps43)
  zephyr_library_sources(drivers/input/tps43.c)
  # ZMK event listener (activity -> pad sleep) must live in the app image.
  target_sources(app PRIVATE drivers/input/tps43_idle_sleeper.c)
endif()
