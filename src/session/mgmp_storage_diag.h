// Targeted, read-only diagnostics for the House storage and its save property.
#pragma once

#include <cstdint>

namespace mgmp {

// The numeric value is PageState from mgmp_page.h. Keeping this header
// independent avoids a page/diagnostic include cycle.
void storage_diag_page(uint8_t page);
void storage_diag_save_loaded();

// Called by the existing property hooks. These functions only record values;
// they never replace a getter result or suppress a setter.
void storage_diag_property_get(const char* key, int64_t local, int64_t answer,
                               bool overridden);
void storage_diag_property_set(const char* key, int value);

} // namespace mgmp
