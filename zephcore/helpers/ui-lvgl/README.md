# LVGL touch UI

Companion UI for colour displays with touch input. See `docs/ARCHITECTURE.md` §8.1 for how it
runs and `docs/LINUX_NATIVE.md` for the native_sim desktop preview.

```
xml/          the layout, in LVGL's XML format (source of truth)
generated/    C made from xml/ by scripts/lvgl_xml2c.py (do not edit)
ui_lvgl.c     ui_task.h implementation: model -> subjects, lists, settings -> ui_mesh_actions
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
- Subjects in `globals.xml`; `ui_lvgl.c` sets them from the mesh.
- `event_cb` callbacks: `ui_on_conversation_open`, `ui_on_send`, `ui_on_advert_zerohop`,
  `ui_on_advert_flood`.

The generator accepts only the XML subset it knows and stops with an error on anything else.
LVGL 9.5 moved its own XML engine out of the open-source library; this project only uses the
XML at build time, so the firmware depends on MIT-licensed LVGL alone.

Fonts: Montserrat (SIL OFL 1.1, `xml/fonts/OFL.txt`).
