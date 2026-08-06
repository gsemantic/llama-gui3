# Plan: Refactor Profile System

## Problem Statement

The project has 3 independent "profile" systems that share the name but mean different things, with massive code duplication, inconsistent data storage, and redundant settings.

### Current State

| System | Storage | Manager | UI |
|---|---|---|---|
| Settings profiles | `profiles/*.json` (flat dump) | `Settings` class | `SettingsDialog::render_profile_manager()` + `ProfileManagerDialog` |
| RAG Index profiles | `~/.llama-gui/rag_profiles/` | `RagIndexProfileManager` | RAG interface |
| Benchmark profiles | `profiles/*.json` (same files!) | `ProfileAdapter` | Benchmark tabs |
| Workspace configs | `WorkspaceManager` (separate JSON) | `WorkspaceManager` | Menu system |

### Key Issues

1. **4 directory scanners** for profiles (`Settings::list_profiles`, `ProfileAdapter::getAvailableProfiles`, `ConfigManager::listProfiles` delegates, `RagIndexProfileManager::get_profile_names`)
2. **Duplicate settings** in profile JSON: `chat.temperature=0.7` vs `sampling.temperature=0.6`, `chat.threads=4` vs `batch.threads=2`, `chat.n_ctx=4096` vs `batch.ctx_size=4096`
3. **Model path in 3 places**: `custom.model_path`, `model_loading.model_path`, `chat.mmproj`
4. **2 UIs for same operations**: `SettingsDialog::render_profile_manager()` and `ProfileManagerDialog::render()`
5. **Dead `settings.ini`**: written to but never read as source
6. **Workspace not fully in profile**: `WorkspaceSettingsIntegration` stores workspace NAME in profile custom settings, but the full menu visibility config (`WorkspaceMenuConfig`) lives in `WorkspaceManager` separately

---

## Architecture: Unified ProfileManager

```
┌─────────────────────────────────────────────────────────┐
│                    ProfileManager                        │
│  (single entry point for ALL profile operations)        │
│                                                         │
│  - listProfiles(type?)                                  │
│  - loadProfile(name) → ProfileData                      │
│  - saveProfile(name, ProfileData)                       │
│  - deleteProfile(name)                                  │
│  - renameProfile(old, new)                              │
│  - getCurrentProfile() → ProfileData                    │
│                                                         │
├──────────────────────┬──────────────────────────────────┤
│   SettingsProfile    │   RagProfile                     │
│   (renamed struct)   │   (existing, moved under PM)     │
├──────────────────────┴──────────────────────────────────┤
│              WorkspaceConfig (NEW, embedded)             │
│   - menu visibility (which menus, which items)          │
│   - enabled/disabled commands                           │
│   - workspace type (User/Developer/Admin)               │
└─────────────────────────────────────────────────────────┘
```

---

## Step-by-Step Implementation

### Phase 1: Unified ProfileManager (core)

**Files to create:**
- `include/core/profile_manager.h` — unified manager
- `src/core/profile_manager.cpp` — implementation

**What it does:**
- Single `list_profiles()` scanning `profiles/` directory (replaces 4 scanners)
- `load_profile()` / `save_profile()` using clean JSON schema
- `rename_profile()` with proper `current_profile_name_` update
- `delete_profile()` with current-profile guard
- `get_current_profile()` / `set_current_profile()`

**Files to modify:**
- `include/core/settings.h` — REMOVE `list_profiles()`, `save_profile()`, `load_profile()`, `delete_profile()`, `load_last_profile()`, `current_profile_name_`, `profiles_directory_`
- `src/core/settings_profiles.cpp` — DELETE this file entirely (all logic moves to ProfileManager)
- `include/core/config_manager.h` — REMOVE `loadProfile()`, `saveProfile()`, `listProfiles()`, `getCurrentProfileName()` (delegate to ProfileManager or remove ConfigManager's profile role)
- `src/core/config_manager.cpp` — remove profile methods

### Phase 2: Clean JSON Schema

**Current `default.json` (289 lines, 18 sections)** → **New schema:**

```json
{
  "meta": {
    "name": "default",
    "description": "Описание профиля",
    "created_at": "2025-07-18T10:00:00Z",
    "modified_at": "2025-07-18T12:00:00Z",
    "tags": ["gemma", "local"]
  },
  "model": {
    "path": "/path/to/model.gguf",
    "alias": "gemma-3-1b-it-UD",
    "draft_model": "",
    "mmproj": ""
  },
  "server": {
    "host": "localhost",
    "port": 8081,
    "api_url": "http://localhost:8081",
    "connection_timeout": 30000,
    "request_timeout": 120000,
    "max_retries": 3
  },
  "server_runtime": { ... },
  "sampling": {
    "temperature": 0.7,
    "top_p": 0.9,
    "top_k": 40,
    "min_p": 0.05,
    "repeat_penalty": 1.1,
    "max_tokens": 2048,
    "seed": -1,
    "threads": 4,
    "n_ctx": 4096
  },
  "gpu": {
    "n_gpu_layers": 0,
    "flash_attn": false,
    "split_mode": "layer",
    "tensor_split": ""
  },
  "cache": { ... },
  "batch": { ... },
  "rope": { ... },
  "grammar": { ... },
  "output": { ... },
  "control_vectors": [],
  "tensor_overrides": [],
  "rag": { ... },
  "display": { ... },
  "performance": { ... },
  "files": { ... },
  "openrouter": { ... },
  "workspace": {
    "current_workspace": "User",
    "menu_visibility": {
      "File": { "visible": true, "hidden_items": [] },
      "Settings": { "visible": true, "hidden_items": ["gpu", "cache"] },
      "Developer": { "visible": false }
    },
    "enabled_commands": ["open_settings_server"],
    "disabled_commands": ["show_debug_log"]
  }
}
```

**Key changes from current:**
- `model_path` in ONE place (`model.path`) — remove from `custom.model_path`, `model_loading.model_path`
- `n_ctx` in ONE place (`sampling.n_ctx`) — remove from `batch.ctx_size` and `chat.n_ctx`
- `threads` in ONE place (`sampling.threads`) — remove from `batch.threads` and `chat.threads`
- `n_gpu_layers` in ONE place (`gpu.n_gpu_layers`) — remove from `chat.n_gpu_layers`
- `temperature` in ONE place (`sampling.temperature`) — remove from `chat.temperature`
- `workspace` section embedded with full menu visibility config

**Files to modify:**
- `include/core/settings.h` — update struct fields, remove duplicates
- `src/core/settings.cpp` — update serialize/deserialize methods
- `src/core/settings_sampling.cpp` — add `n_ctx`, `threads`, `max_tokens` (move from chat)
- Remove duplicate fields from `ChatSettings`, `BatchSettings`

### Phase 3: Remove Duplicate UI

**Delete `SettingsDialog::render_profile_manager()`** — keep only `ProfileManagerDialog`.

**Files to modify:**
- `src/ui/settings_dialog_main.cpp` — remove `render_profile_manager()` method (~170 lines)
- `include/ui/settings_dialog.h` — remove profile-related members: `current_profile_name_`, `current_loaded_profile_`, `current_profile_idx_`, `profile_loaded_pending_apply_`, `render_profile_manager()`, `apply_profile()`, `syncProfileSelection()`

**Files to keep unchanged:**
- `src/ui/profile_manager_dialog.cpp` — the one and only profile UI
- `include/ui/profile_manager_dialog.h` — uses `ProfileManager` instead of `ConfigManager`

### Phase 4: Workspace Integration

**Current state:**
- `WorkspaceManager` stores menu configs in its own JSON
- `WorkspaceSettingsIntegration` stores only workspace NAME in profile custom settings
- Menu visibility (`visible_items`, `hidden_items`) is NOT in the profile

**What changes:**
- Add `workspace` section to profile JSON (see Phase 2 schema)
- `ProfileManager::save_profile()` also saves `WorkspaceManager::serializeToJson()` workspace data
- `ProfileManager::load_profile()` also loads workspace config and applies it via `WorkspaceManager::loadWorkspaceConfig()`
- On profile switch: workspace menu visibility updates automatically

**Files to modify:**
- `include/core/profile_manager.h` — add `WorkspaceManager*` dependency
- `src/core/profile_manager.cpp` — serialize/deserialize workspace section
- `src/core/workspace_settings_integration.cpp` — simplify (no longer needs custom settings trick)
- `include/core/workspace_settings_integration.h` — remove `getCurrentWorkspaceFromSettings()`, `setCurrentWorkspaceInSettings()`

**What stays the same:**
- `WorkspaceManager` class itself — no changes to its API
- `WorkspaceMenuConfig` struct — unchanged
- `AdvancedMenuSystem` — unchanged, still checks `WorkspaceManager::isMenuVisible()`

### Phase 5: Clean up Settings

**Remove dead code:**
- `settings.ini` — remove from priority chain (or make it real)
- `sync_ctx_size()` — no longer needed (single `n_ctx`)
- `sync_max_tokens()` — no longer needed (single `max_tokens`)
- `simplify_at_startup()` logic (profiles > ini > defaults) → just profiles > defaults

**Files to modify:**
- `src/core/settings_profiles.cpp` → DELETE
- `src/core/settings.cpp` — remove sync methods
- `include/core/settings.h` — remove sync methods and duplicate fields

### Phase 6: ProfileAdapter Simplification

`ProfileAdapter` currently reads JSON manually. After Phase 2, it should use `ProfileManager::loadProfile()` or read the clean schema directly.

**Files to modify:**
- `src/bench/profile_adapter.cpp` — simplify extraction (single `model.path`, single `n_ctx`, etc.)
- `include/bench/profile_adapter.h` — remove `getAvailableProfiles()` (use `ProfileManager`)

---

## Files Summary

### DELETE:
- `src/core/settings_profiles.cpp`
- `SettingsDialog::render_profile_manager()` (from `settings_dialog_main.cpp`)

### CREATE:
- `include/core/profile_manager.h`
- `src/core/profile_manager.cpp`

### MODIFY (major):
- `include/core/settings.h` — remove profile methods, remove duplicate fields
- `src/core/settings.cpp` — update serialization
- `include/core/config_manager.h` — remove profile methods
- `src/core/config_manager.cpp` — remove profile methods
- `include/ui/profile_manager_dialog.h` — use ProfileManager
- `src/ui/profile_manager_dialog.cpp` — use ProfileManager
- `src/ui/settings_dialog_main.cpp` — remove render_profile_manager (~170 lines)
- `include/ui/settings_dialog.h` — remove profile members
- `src/bench/profile_adapter.cpp` — simplify to use clean schema

### MODIFY (minor):
- `src/core/workspace_settings_integration.cpp` — simplify
- `include/core/workspace_settings_integration.h` — simplify
- `profiles/default.json` — migrate to new schema

---

## Impact Assessment

| Metric | Before | After | Delta |
|---|---|---|---|
| Directory scanners | 4 | 1 | -3 |
| Profile UI components | 2 | 1 | -1 |
| `model_path` fields | 3 | 1 | -2 |
| `n_ctx` fields | 3 | 1 | -2 |
| `threads` fields | 3 | 1 | -2 |
| `temperature` fields | 2 | 1 | -1 |
| `settings.ini` logic | present | removed | cleaner |
| Workspace-in-profile | name only | full menu visibility | +feature |
| Lines of profile code | ~1200 | ~600 | -50% |
| Profile JSON size | ~289 lines | ~180 lines | -38% |

---

## Risk Assessment

| Risk | Mitigation |
|---|---|
| Breaking existing profiles | Migration function: old JSON → new schema (one-time) |
| WorkspaceManager integration complexity | Low — just embed/extract a JSON section, no API changes |
| SettingsDialog regression from removing render_profile_manager | Minimal — ProfileManagerDialog is more complete |
| Bench profile reading changes | Low — ProfileAdapter just reads cleaner fields |

---

## Answers to User Questions

### Can menu visibility be included in profiles?

**YES — and it's the right thing to do.** Here's why:

1. **The infrastructure already exists.** `WorkspaceMenuConfig` has `visible`, `visible_items`, `hidden_items`. `WorkspaceManager` has `serializeToJson()`/`deserializeFromJson()`. We just need to embed this data in the profile JSON.

2. **It's minimal additional code.** ~30 lines in `ProfileManager` to save/load the workspace section. The `WorkspaceManager` class doesn't change at all.

3. **The UX improvement is significant.** Currently switching profiles changes model/sampling/etc. but NOT which menu items are visible. A "Developer" profile should show debug menus, a "User" profile should hide them. This is already what `WorkspaceManager` does — we just connect it to profiles.

4. **No over-engineering.** The workspace data is already a JSON blob. Embedding it in the profile is just moving where the blob lives, not redesigning anything.

### What would over-complicate things:
- Making each menu item independently toggleable per-profile (the current `visible_items`/`hidden_items` approach is sufficient)
- Dynamic menu registration based on profile (too complex, not needed)
- Per-window menu configurations (the `WindowState` in `AdvancedMenuSystem` already handles this separately)
