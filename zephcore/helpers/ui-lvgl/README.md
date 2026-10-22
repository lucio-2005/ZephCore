# LVGL touch UI

Companion UI for colour displays with touch input. See `docs/ARCHITECTURE.md` §8.1 for how it
runs and `docs/LINUX_NATIVE.md` for the native_sim desktop preview.

```
xml/          the layout, in LVGL's XML format (source of truth)
generated/    C made from xml/ by scripts/lvgl_xml2c.py (do not edit)
ui_lvgl.c           ui_task.h implementation: model -> subjects, lists, alerts, toasts
ui_lvgl_settings.c  the Settings screens: NodePrefs -> subjects, changes -> ui_settings / ui_mesh_actions
ui_lvgl_remote.c    Contacts tab, remote management (login, requests, console, remote settings), Sensors
ui_lvgl_edit.c      the edit panel for one text or number value
ui_lvgl_keys.c      joystick / keypad navigation (focus group, key conventions)
ui_lvgl_map.c       the map: markers, pan and zoom, drawn in one callback
ui_map_tiles.*      offline tiles (scripts/map_tiles.py makes them), native_sim only for now
ui_settings.*       NodePrefs access through the CompanionMesh methods the app's commands use
ui_remote.*         contact table and remote requests (C API over CompanionMesh)
```

## Changing the layout

1. Edit the XML. To see it in a browser, open the `xml/` folder of a public GitHub repo
   in https://viewer.lvgl.io (sign in with GitHub, paste the folder URL).
2. Regenerate the C:

   ```bash
   python3 zephcore/scripts/lvgl_xml2c.py zephcore/helpers/ui-lvgl/xml zephcore/helpers/ui-lvgl/generated
   ```

   Only when fonts change, also pass `--font-conv <path to lv_font_conv>`
   (`npm install lv_font_conv`) and `--font-fallback lv_font_montserrat_14`.
3. `--check` instead of regenerating fails if `generated/` is stale.

## What the C code relies on in the XML

- Names: `chat_list`, `contact_list` (home), `msg_list`, `msg_input`, `msg_kbd` (conversation).
  The sample rows in those containers are for the previewer and are removed at boot.
  Settings: `nav_bluetooth` (hidden without Bluetooth), `radio_preset`, `radio_txp`,
  `row_buzzer`, `dev_buzzer`, and `edit_input` / `edit_kbd` in the edit panel.
  Remote: `remote_login`, `remote_rows`, the `act_*` buttons (shown by node type and role),
  `console_list` / `console_input` / `console_kbd`, `sensor_rows`.
  Map: `map_area` (drawn by the device; children sit on top), `map_card`, `map_open`.
  Navigation: `nav_back` on every screen's Back button, `panel_close` on every panel's
  Cancel/Close button, `home_tabs` on the home tabview. Clickable rows extend `lv_button`
  so the joystick can reach them; `style_focus` / `style_focus_inner` are the cursor.
- Settings subjects (`set_*`, `sum_*`) hold dropdown indexes in the order of the XML options;
  `ui_lvgl_settings.c` maps them to NodePrefs values, so keep the two in step.
- Subjects in `globals.xml`; `ui_lvgl.c` sets them from the mesh.
- `event_cb` callbacks: `ui_on_conversation_open`, `ui_on_send`, `ui_on_advert_zerohop`,
  `ui_on_advert_flood`, `ui_on_alerts_open`, `ui_on_alerts_clear`, `ui_on_settings_open`
  (user_data names the screen), `ui_on_edit` (user_data is the setting key), `ui_on_edit_save`,
  `ui_on_edit_cancel`, `ui_on_radio_save`, `ui_on_reboot`, `ui_on_factory_reset`,
  `ui_on_contact_open`, `ui_on_remote_open`, `ui_on_remote_login`, `ui_on_remote_action`
  (user_data names the action), `ui_on_console_send`, `ui_on_console_close`,
  `ui_on_rsettings_reload`, `ui_on_rsettings_close`, `ui_on_sensors_open`, `ui_on_sensors_refresh`,
  `ui_on_map_open`, `ui_on_map_zoom` (user_data `in`, `out` or `fit`), `ui_on_map_open_node`.
- value_row keys starting with `r.` belong to the remote screen, the others to Settings.
- `user_data="$prop"` passes a component's string property to the callback.

The generator accepts only the XML subset it knows and stops with an error on anything else.
LVGL 9.5 moved its own XML engine out of the open-source library; this project only uses the
XML at build time, so the firmware depends on MIT-licensed LVGL alone.

Fonts: Montserrat (SIL OFL 1.1, `xml/fonts/OFL.txt`).
