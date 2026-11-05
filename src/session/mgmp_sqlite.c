/* mgmp_sqlite.c -- SQLite, compiled into mgmp.dll (0.1.107).
 *
 * The save checks used Windows' own winsqlite3.dll from System32. Its version is the Windows build's: an older Windows 10 ships one below 3.37, which
 * cannot read a table declared STRICT -- and every table of a Mewgenics save is (`CREATE TABLE cats (key INTEGER PRIMARY KEY,data BLOB) STRICT`). On such
 * a machine every save failed "unreadable or invalid SQLite save" and a client's click on a save slot did nothing (room XR8U, 2026-10-25). The game reads
 * its saves with its own SQLite; the mod now carries its own too (third_party/sqlite, the 3.54.1 amalgamation, public domain), so the Windows version no
 * longer matters.
 */
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_DEFAULT_MEMSTATUS 0
#define SQLITE_THREADSAFE 1
#define SQLITE_API
#include "sqlite/sqlite3.c"
