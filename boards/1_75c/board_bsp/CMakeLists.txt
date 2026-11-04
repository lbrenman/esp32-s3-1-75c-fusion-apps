# The board's BSP (from idf_component.yml) and board.h (screen size and shape).
# No sources: everything that includes board.h or bsp/... gets them through this component.
idf_component_register(INCLUDE_DIRS "include")
