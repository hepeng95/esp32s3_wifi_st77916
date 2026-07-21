Import("env")

env.Append(CPPPATH=[
    "$PROJECT_DIR",
    "$PROJECT_DIR/src",
    "$PROJECT_DIR/src/config",
    "$PROJECT_DIR/src/drivers",
    "$PROJECT_DIR/src/wifi",
    "$PROJECT_DIR/src/ui"
])