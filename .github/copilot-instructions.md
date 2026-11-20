# Copilot instructions for Bronx

## Build and validation

Bronx is a C++17 CMake application. From this directory:

```sh
cmake -S . -B build
cmake --build build --target bronx --parallel
```

This project does not currently register Bronx tests with CTest, and there is no single-test command. `ctest --test-dir build --output-on-failure` can be used to check for registered tests. There is no configured C++ lint/format command. `website/package.json` defines `npm test`, but it is only a placeholder that exits with an error.

`CMakeLists.txt` adds the sibling `stoat++` source tree with `add_subdirectory(..)`, so a standalone checkout needs that dependency in the expected parent directory. The build also requires the native libraries discovered by CMake, including MariaDB client, Tesseract/Leptonica, and PulseAudio; Cairo is optional.

## Architecture

- `main.cpp` owns process startup: environment loading, database initialization, Stoat++ cluster configuration, global command/event handling, and registration of bot modules.
- `cogs/*.h` contains the feature modules. Each module derives from `stoatpp::bot_module` and registers commands and/or event callbacks in `register_handlers`.
- `database/database.h` and `database/types.h` define the persistence API and shared data types. Implementations are split across `database/*.cpp`; `database/tables.cpp` initializes the SQL schema. The bot's modules use the shared `bronx::g_db` database handle.
- `i18n.cpp` and `locales/*.json` provide localization. Keep user-facing strings consistent with the existing localization approach where it is already used.
- `website/` is a separate Express dashboard and REST API. Its front end is mostly HTML/CSS/JavaScript; API routes use `mysql2` to read and update dashboard and bot data in MariaDB.

The bot prefix defaults to `.`, and `main.cpp` configures commands as case-insensitive and dispatches edited command messages again. Its prefix resolver combines the default prefix with mention, user, and server prefixes.

## Repository-specific conventions

- Implement bot features in the relevant cog module rather than growing the central message handler in `main.cpp`. Register any new module with `bot.use(...)` in `main.cpp`.
- Keep command and gateway event registration inside the module's `register_handlers` method; use the Stoat++ cluster and event types for platform operations.
- For a persisted feature, keep the data type, `Database` method declaration, SQL schema, and method implementation aligned across `database/types.h`, `database/database.h`, `database/tables.cpp`, and the appropriate `database/*.cpp` file.
- Runtime configuration is loaded from `.env` in the working directory (with a parent-directory fallback), without overriding variables already exported by the environment. `main.cpp` currently reads `BOT_TOKEN` and `DB_DATABASE` (and `STAGING_BOT_TOKEN` / `STAGING_DB_DATABASE` in staging mode); check the code when README examples differ.
